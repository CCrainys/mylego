/*
 * Copyright (c) 2016-2017 Wuklab, Purdue University. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * Virtual memory map management code
 * Based on mm/mmap.c
 */

#include <lego/mm.h>
#include <lego/slab.h>
#include <lego/rbtree.h>
#include <lego/sched.h>
#include <lego/kernel.h>
#include <lego/netmacro.h>
#include <lego/comp_memory.h>
#include <lego/comp_common.h>

static inline unsigned long vma_pages(struct vm_area_struct *vma)
{
	return (vma->vm_end - vma->vm_start) >> PAGE_SHIFT;
}

#define validate_mm_rb(root, ignore) do { } while (0)
#define validate_mm(mm) do { } while (0)

/* description of effects of mapping type and prot in current implementation.
 * this is due to the limited x86 page protection hardware.  The expected
 * behavior is in parens:
 *
 * map_type	prot
 *		PROT_NONE	PROT_READ	PROT_WRITE	PROT_EXEC
 * MAP_SHARED	r: (no) no	r: (yes) yes	r: (no) yes	r: (no) yes
 *		w: (no) no	w: (no) no	w: (yes) yes	w: (no) no
 *		x: (no) no	x: (no) yes	x: (no) yes	x: (yes) yes
 *
 * MAP_PRIVATE	r: (no) no	r: (yes) yes	r: (no) yes	r: (no) yes
 *		w: (no) no	w: (no) no	w: (copy) copy	w: (no) no
 *		x: (no) no	x: (no) yes	x: (no) yes	x: (yes) yes
 *
 * On arm64, PROT_EXEC has the following behaviour for both MAP_SHARED and
 * MAP_PRIVATE:
 *								r: (no) no
 *								w: (no) no
 *								x: (yes) yes
 */
pgprot_t protection_map[16] = {
	__P000, __P001, __P010, __P011, __P100, __P101, __P110, __P111,
	__S000, __S001, __S010, __S011, __S100, __S101, __S110, __S111
};

pgprot_t vm_get_page_prot(unsigned long vm_flags)
{
	return __pgprot(pgprot_val(protection_map[vm_flags &
				(VM_READ|VM_WRITE|VM_EXEC|VM_SHARED)]));
}

static inline pgprot_t pgprot_modify(pgprot_t oldprot, pgprot_t newprot)
{
	pgprotval_t preservebits = pgprot_val(oldprot) & _PAGE_CHG_MASK;
	pgprotval_t addbits = pgprot_val(newprot);
	return __pgprot(preservebits | addbits);
}

static pgprot_t vm_pgprot_modify(pgprot_t oldprot, unsigned long vm_flags)
{
	return pgprot_modify(oldprot, vm_get_page_prot(vm_flags));
}

/* Update vma->vm_page_prot to reflect vma->vm_flags. */
void vma_set_page_prot(struct vm_area_struct *vma)
{
	unsigned long vm_flags = vma->vm_flags;
	pgprot_t vm_page_prot;

	vm_page_prot = vm_pgprot_modify(vma->vm_page_prot, vm_flags);

	/* remove_protection_ptes reads vma->vm_page_prot without mmap_sem */
	WRITE_ONCE(vma->vm_page_prot, vm_page_prot);
}

static long vma_compute_subtree_gap(struct vm_area_struct *vma)
{
	unsigned long max, subtree_gap;
	max = vma->vm_start;
	if (vma->vm_prev)
		max -= vma->vm_prev->vm_end;
	if (vma->vm_rb.rb_left) {
		subtree_gap = rb_entry(vma->vm_rb.rb_left,
				struct vm_area_struct, vm_rb)->rb_subtree_gap;
		if (subtree_gap > max)
			max = subtree_gap;
	}
	if (vma->vm_rb.rb_right) {
		subtree_gap = rb_entry(vma->vm_rb.rb_right,
				struct vm_area_struct, vm_rb)->rb_subtree_gap;
		if (subtree_gap > max)
			max = subtree_gap;
	}
	return max;
}

RB_DECLARE_CALLBACKS(static, vma_gap_callbacks, struct vm_area_struct, vm_rb,
		     unsigned long, rb_subtree_gap, vma_compute_subtree_gap)

/*
 * Update augmented rbtree rb_subtree_gap values after vma->vm_start or
 * vma->vm_prev->vm_end values changed, without modifying the vma's position
 * in the rbtree.
 */
static void vma_gap_update(struct vm_area_struct *vma)
{
	/*
	 * As it turns out, RB_DECLARE_CALLBACKS() already created a callback
	 * function that does exacltly what we want.
	 */
	vma_gap_callbacks_propagate(&vma->vm_rb, NULL);
}

static inline void vma_rb_insert(struct vm_area_struct *vma,
				 struct rb_root *root)
{
	/* All rb_subtree_gap values must be consistent prior to insertion */
	validate_mm_rb(root, NULL);

	rb_insert_augmented(&vma->vm_rb, root, &vma_gap_callbacks);
}

static void __vma_rb_erase(struct vm_area_struct *vma, struct rb_root *root)
{
	/*
	 * Note rb_erase_augmented is a fairly large inline function,
	 * so make sure we instantiate it only once with our desired
	 * augmented rbtree callbacks.
	 */
	rb_erase_augmented(&vma->vm_rb, root, &vma_gap_callbacks);
}

static __always_inline void vma_rb_erase_ignore(struct vm_area_struct *vma,
						struct rb_root *root,
						struct vm_area_struct *ignore)
{
	/*
	 * All rb_subtree_gap values must be consistent prior to erase,
	 * with the possible exception of the "next" vma being erased if
	 * next->vm_start was reduced.
	 */
	validate_mm_rb(root, ignore);

	__vma_rb_erase(vma, root);
}

static __always_inline void vma_rb_erase(struct vm_area_struct *vma,
					 struct rb_root *root)
{
	/*
	 * All rb_subtree_gap values must be consistent prior to erase,
	 * with the possible exception of the vma being erased.
	 */
	validate_mm_rb(root, vma);

	__vma_rb_erase(vma, root);
}

struct vm_area_struct *find_vma(struct lego_mm_struct *mm, unsigned long addr)
{
	struct rb_node *rb_node;
	struct vm_area_struct *vma;

	rb_node = mm->mm_rb.rb_node;

	while (rb_node) {
		struct vm_area_struct *tmp;

		tmp = rb_entry(rb_node, struct vm_area_struct, vm_rb);

		if (tmp->vm_end > addr) {
			vma = tmp;
			if (tmp->vm_start <= addr)
				break;
			rb_node = rb_node->rb_left;
		} else
			rb_node = rb_node->rb_right;
	}

	return vma;
}

void __vma_link_rb(struct lego_mm_struct *mm, struct vm_area_struct *vma,
		struct rb_node **rb_link, struct rb_node *rb_parent)
{
	/* Update tracking information for the gap following the new vma. */
	if (vma->vm_next)
		vma_gap_update(vma->vm_next);
	else
		mm->highest_vm_end = vma->vm_end;

	/*
	 * vma->vm_prev wasn't known when we followed the rbtree to find the
	 * correct insertion point for that vma. As a result, we could not
	 * update the vma vm_rb parents rb_subtree_gap values on the way down.
	 * So, we first insert the vma with a zero rb_subtree_gap value
	 * (to be consistent with what we did on the way down), and then
	 * immediately update the gap to the correct value. Finally we
	 * rebalance the rbtree after all augmented values have been set.
	 */
	rb_link_node(&vma->vm_rb, rb_parent, rb_link);
	vma->rb_subtree_gap = 0;
	vma_gap_update(vma);
	vma_rb_insert(vma, &mm->mm_rb);
}

void __vma_link_list(struct lego_mm_struct *mm, struct vm_area_struct *vma,
		struct vm_area_struct *prev, struct rb_node *rb_parent)
{
	struct vm_area_struct *next;

	vma->vm_prev = prev;
	if (prev) {
		next = prev->vm_next;
		prev->vm_next = vma;
	} else {
		mm->mmap = vma;
		if (rb_parent)
			next = rb_entry(rb_parent,
					struct vm_area_struct, vm_rb);
		else
			next = NULL;
	}
	vma->vm_next = next;
	if (next)
		next->vm_prev = vma;
}

static void
__vma_link(struct lego_mm_struct *mm, struct vm_area_struct *vma,
	struct vm_area_struct *prev, struct rb_node **rb_link,
	struct rb_node *rb_parent)
{
	__vma_link_list(mm, vma, prev, rb_parent);
	__vma_link_rb(mm, vma, rb_link, rb_parent);
}

static void vma_link(struct lego_mm_struct *mm, struct vm_area_struct *vma,
			struct vm_area_struct *prev, struct rb_node **rb_link,
			struct rb_node *rb_parent)
{
	__vma_link(mm, vma, prev, rb_link, rb_parent);

	mm->map_count++;
	validate_mm(mm);
}

static int find_vma_links(struct lego_mm_struct *mm, unsigned long addr,
		unsigned long end, struct vm_area_struct **pprev,
		struct rb_node ***rb_link, struct rb_node **rb_parent)
{
	struct rb_node **__rb_link, *__rb_parent, *rb_prev;

	__rb_link = &mm->mm_rb.rb_node;
	rb_prev = __rb_parent = NULL;

	while (*__rb_link) {
		struct vm_area_struct *vma_tmp;

		__rb_parent = *__rb_link;
		vma_tmp = rb_entry(__rb_parent, struct vm_area_struct, vm_rb);

		if (vma_tmp->vm_end > addr) {
			/* Fail if an existing vma overlaps the area */
			if (vma_tmp->vm_start < end)
				return -ENOMEM;
			__rb_link = &__rb_parent->rb_left;
		} else {
			rb_prev = __rb_parent;
			__rb_link = &__rb_parent->rb_right;
		}
	}

	*pprev = NULL;
	if (rb_prev)
		*pprev = rb_entry(rb_prev, struct vm_area_struct, vm_rb);
	*rb_link = __rb_link;
	*rb_parent = __rb_parent;
	return 0;
}

/*
 * Helper for vma_adjust() in the split_vma insert case: insert a vma into the
 * mm's list and rbtree.  It has already been inserted into the interval tree.
 */
static void __insert_vm_struct(struct lego_mm_struct *mm, struct vm_area_struct *vma)
{
	struct vm_area_struct *prev;
	struct rb_node **rb_link, *rb_parent;

	if (find_vma_links(mm, vma->vm_start, vma->vm_end,
			   &prev, &rb_link, &rb_parent))
		BUG();
	__vma_link(mm, vma, prev, rb_link, rb_parent);
	mm->map_count++;
}

/* Insert vm structure into process list sorted by address
 * and into the inode's i_mmap tree.  If vm_file is non-NULL
 * then i_mmap_rwsem is taken here.
 */
int insert_vm_struct(struct lego_mm_struct *mm, struct vm_area_struct *vma)
{
	struct vm_area_struct *prev;
	struct rb_node **rb_link, *rb_parent;

	if (find_vma_links(mm, vma->vm_start, vma->vm_end,
			   &prev, &rb_link, &rb_parent))
		return -ENOMEM;

	/*
	 * The vm_pgoff of a purely anonymous vma should be irrelevant
	 * until its first write fault, when page's anon_vma and index
	 * are set.  But now set the vm_pgoff it will almost certainly
	 * end up with (unless mremap moves it elsewhere before that
	 * first wfault), so /proc/pid/maps tells a consistent story.
	 *
	 * By setting it to reflect the virtual start address of the
	 * vma, merges and splits can happen in a seamless way, just
	 * using the existing file pgoff checks and manipulations.
	 * Similarly in do_mmap_pgoff and in do_brk.
	 */
	if (vma_is_anonymous(vma)) {
		BUG_ON(vma->anon_vma);
		vma->vm_pgoff = vma->vm_start >> PAGE_SHIFT;
	}

	vma_link(mm, vma, prev, rb_link, rb_parent);
	return 0;
}

static __always_inline void __vma_unlink_common(struct lego_mm_struct *mm,
						struct vm_area_struct *vma,
						struct vm_area_struct *prev,
						bool has_prev,
						struct vm_area_struct *ignore)
{
	struct vm_area_struct *next;

	vma_rb_erase_ignore(vma, &mm->mm_rb, ignore);
	next = vma->vm_next;
	if (has_prev)
		prev->vm_next = next;
	else {
		prev = vma->vm_prev;
		if (prev)
			prev->vm_next = next;
		else
			mm->mmap = next;
	}
	if (next)
		next->vm_prev = prev;
}

static inline void __vma_unlink_prev(struct lego_mm_struct *mm,
				     struct vm_area_struct *vma,
				     struct vm_area_struct *prev)
{
	__vma_unlink_common(mm, vma, prev, true, vma);
}

/*
 * We cannot adjust vm_start, vm_end, vm_pgoff fields of a vma that
 * is already present in an i_mmap tree without adjusting the tree.
 * The following helper function should be used when such adjustments
 * are necessary.  The "insert" vma (if any) is to be inserted
 * before we drop the necessary locks.
 */
int __vma_adjust(struct vm_area_struct *vma, unsigned long start,
	unsigned long end, pgoff_t pgoff, struct vm_area_struct *insert,
	struct vm_area_struct *expand)
{
	struct lego_mm_struct *mm = vma->vm_mm;
	struct vm_area_struct *next = vma->vm_next;
	struct lego_file *file = vma->vm_file;
	bool start_changed = false, end_changed = false;
	long adjust_next = 0;
	int remove_next = 0;

	if (next && !insert) {
		struct vm_area_struct *exporter = NULL, *importer = NULL;

		if (end >= next->vm_end) {
			/*
			 * vma expands, overlapping all the next, and
			 * perhaps the one after too (mprotect case 6).
			 * The only other cases that gets here are
			 * case 1, case 7 and case 8.
			 */
			if (next == expand) {
				/*
				 * The only case where we don't expand "vma"
				 * and we expand "next" instead is case 8.
				 */
				VM_WARN_ON(end != next->vm_end);
				/*
				 * remove_next == 3 means we're
				 * removing "vma" and that to do so we
				 * swapped "vma" and "next".
				 */
				remove_next = 3;
				VM_WARN_ON(file != next->vm_file);
				swap(vma, next);
			} else {
				VM_WARN_ON(expand != vma);
				/*
				 * case 1, 6, 7, remove_next == 2 is case 6,
				 * remove_next == 1 is case 1 or 7.
				 */
				remove_next = 1 + (end > next->vm_end);
				VM_WARN_ON(remove_next == 2 &&
					   end != next->vm_next->vm_end);
				VM_WARN_ON(remove_next == 1 &&
					   end != next->vm_end);
				/* trim end to next, for case 6 first pass */
				end = next->vm_end;
			}

			exporter = next;
			importer = vma;

			/*
			 * If next doesn't have anon_vma, import from vma after
			 * next, if the vma overlaps with it.
			 */
			if (remove_next == 2 && !next->anon_vma)
				exporter = next->vm_next;

		} else if (end > next->vm_start) {
			/*
			 * vma expands, overlapping part of the next:
			 * mprotect case 5 shifting the boundary up.
			 */
			adjust_next = (end - next->vm_start) >> PAGE_SHIFT;
			exporter = next;
			importer = vma;
			VM_WARN_ON(expand != importer);
		} else if (end < vma->vm_end) {
			/*
			 * vma shrinks, and !insert tells it's not
			 * split_vma inserting another: so it must be
			 * mprotect case 4 shifting the boundary down.
			 */
			adjust_next = -((vma->vm_end - end) >> PAGE_SHIFT);
			exporter = vma;
			importer = next;
			VM_WARN_ON(expand != importer);
		}
	}
again:
	if (start != vma->vm_start) {
		vma->vm_start = start;
		start_changed = true;
	}
	if (end != vma->vm_end) {
		vma->vm_end = end;
		end_changed = true;
	}
	vma->vm_pgoff = pgoff;
	if (adjust_next) {
		next->vm_start += adjust_next << PAGE_SHIFT;
		next->vm_pgoff += adjust_next;
	}

	if (remove_next) {
		/*
		 * vma_merge has merged next into vma, and needs
		 * us to remove next before dropping the locks.
		 */
		if (remove_next != 3)
			__vma_unlink_prev(mm, next, vma);
		else
			/*
			 * vma is not before next if they've been
			 * swapped.
			 *
			 * pre-swap() next->vm_start was reduced so
			 * tell validate_mm_rb to ignore pre-swap()
			 * "next" (which is stored in post-swap()
			 * "vma").
			 */
			__vma_unlink_common(mm, next, NULL, false, vma);
	} else if (insert) {
		/*
		 * split_vma has split insert from vma, and needs
		 * us to insert it before dropping the locks
		 * (it may either follow vma or precede it).
		 */
		__insert_vm_struct(mm, insert);
	} else {
		if (start_changed)
			vma_gap_update(vma);
		if (end_changed) {
			if (!next)
				mm->highest_vm_end = end;
			else if (!adjust_next)
				vma_gap_update(next);
		}
	}

	if (remove_next) {
		mm->map_count--;
		kfree(next);
		/*
		 * In mprotect's case 6 (see comments on vma_merge),
		 * we must remove another next too. It would clutter
		 * up the code too much to do both in one go.
		 */
		if (remove_next != 3) {
			/*
			 * If "next" was removed and vma->vm_end was
			 * expanded (up) over it, in turn
			 * "next->vm_prev->vm_end" changed and the
			 * "vma->vm_next" gap must be updated.
			 */
			next = vma->vm_next;
		} else {
			/*
			 * For the scope of the comment "next" and
			 * "vma" considered pre-swap(): if "vma" was
			 * removed, next->vm_start was expanded (down)
			 * over it and the "next" gap must be updated.
			 * Because of the swap() the post-swap() "vma"
			 * actually points to pre-swap() "next"
			 * (post-swap() "next" as opposed is now a
			 * dangling pointer).
			 */
			next = vma;
		}
		if (remove_next == 2) {
			remove_next = 1;
			end = next->vm_end;
			goto again;
		}
		else if (next)
			vma_gap_update(next);
		else {
			/*
			 * If remove_next == 2 we obviously can't
			 * reach this path.
			 *
			 * If remove_next == 3 we can't reach this
			 * path because pre-swap() next is always not
			 * NULL. pre-swap() "next" is not being
			 * removed and its next->vm_end is not altered
			 * (and furthermore "end" already matches
			 * next->vm_end in remove_next == 3).
			 *
			 * We reach this only in the remove_next == 1
			 * case if the "next" vma that was removed was
			 * the highest vma of the mm. However in such
			 * case next->vm_end == "end" and the extended
			 * "vma" has vma->vm_end == next->vm_end so
			 * mm->highest_vm_end doesn't need any update
			 * in remove_next == 1 case.
			 */
			VM_WARN_ON(mm->highest_vm_end != end);
		}
	}

	validate_mm(mm);

	return 0;
}

static inline int vma_adjust(struct vm_area_struct *vma, unsigned long start,
	unsigned long end, pgoff_t pgoff, struct vm_area_struct *insert)
{
	return __vma_adjust(vma, start, end, pgoff, insert, NULL);
}

/*
 * If the vma has a ->close operation then the driver probably needs to release
 * per-vma resources, so we don't attempt to merge those.
 */
static inline int is_mergeable_vma(struct vm_area_struct *vma,
				struct lego_file *file, unsigned long vm_flags)
{
	if (vma->vm_file != file)
		return 0;
	return 1;
}

static inline int is_mergeable_anon_vma(struct anon_vma *anon_vma1,
					struct anon_vma *anon_vma2,
					struct vm_area_struct *vma)
{
	/*
	 * The list_is_singular() test is to avoid merging VMA cloned from
	 * parents. This can improve scalability caused by anon_vma lock.
	 */
	if ((!anon_vma1 || !anon_vma2) && (!vma ||
		list_is_singular(&vma->anon_vma_chain)))
		return 1;
	return anon_vma1 == anon_vma2;
}

/*
 * Return true if we can merge this (vm_flags,anon_vma,file,vm_pgoff)
 * beyond (at a higher virtual address and file offset than) the vma.
 *
 * We cannot merge two vmas if they have differently assigned (non-NULL)
 * anon_vmas, nor if same anon_vma is assigned but offsets incompatible.
 */
static int
can_vma_merge_after(struct vm_area_struct *vma, unsigned long vm_flags,
		    struct anon_vma *anon_vma, struct lego_file *file,
		    pgoff_t vm_pgoff)
{
	if (is_mergeable_vma(vma, file, vm_flags) &&
	    is_mergeable_anon_vma(anon_vma, vma->anon_vma, vma)) {
		pgoff_t vm_pglen;
		vm_pglen = vma_pages(vma);
		if (vma->vm_pgoff + vm_pglen == vm_pgoff)
			return 1;
	}
	return 0;
}

/*
 * Return true if we can merge this (vm_flags,anon_vma,file,vm_pgoff)
 * in front of (at a lower virtual address and file offset than) the vma.
 *
 * We cannot merge two vmas if they have differently assigned (non-NULL)
 * anon_vmas, nor if same anon_vma is assigned but offsets incompatible.
 *
 * We don't check here for the merged mmap wrapping around the end of pagecache
 * indices (16TB on ia32) because do_mmap_pgoff() does not permit mmap's which
 * wrap, nor mmaps which cover the final page at index -1UL.
 */
static int
can_vma_merge_before(struct vm_area_struct *vma, unsigned long vm_flags,
		     struct anon_vma *anon_vma, struct lego_file *file,
		     pgoff_t vm_pgoff)
{
	if (is_mergeable_vma(vma, file, vm_flags) &&
	    is_mergeable_anon_vma(anon_vma, vma->anon_vma, vma)) {
		if (vma->vm_pgoff == vm_pgoff)
			return 1;
	}
	return 0;
}

/*
 * Given a mapping request (addr,end,vm_flags,file,pgoff), figure out
 * whether that can be merged with its predecessor or its successor.
 * Or both (it neatly fills a hole).
 *
 * In most cases - when called for mmap, brk or mremap - [addr,end) is
 * certain not to be mapped by the time vma_merge is called; but when
 * called for mprotect, it is certain to be already mapped (either at
 * an offset within prev, or at the start of next), and the flags of
 * this area are about to be changed to vm_flags - and the no-change
 * case has already been eliminated.
 *
 * The following mprotect cases have to be considered, where AAAA is
 * the area passed down from mprotect_fixup, never extending beyond one
 * vma, PPPPPP is the prev vma specified, and NNNNNN the next vma after:
 *
 *     AAAA             AAAA                AAAA          AAAA
 *    PPPPPPNNNNNN    PPPPPPNNNNNN    PPPPPPNNNNNN    PPPPNNNNXXXX
 *    cannot merge    might become    might become    might become
 *                    PPNNNNNNNNNN    PPPPPPPPPPNN    PPPPPPPPPPPP 6 or
 *    mmap, brk or    case 4 below    case 5 below    PPPPPPPPXXXX 7 or
 *    mremap move:                                    PPPPXXXXXXXX 8
 *        AAAA
 *    PPPP    NNNN    PPPPPPPPPPPP    PPPPPPPPNNNN    PPPPNNNNNNNN
 *    might become    case 1 below    case 2 below    case 3 below
 *
 * It is important for case 8 that the the vma NNNN overlapping the
 * region AAAA is never going to extended over XXXX. Instead XXXX must
 * be extended in region AAAA and NNNN must be removed. This way in
 * all cases where vma_merge succeeds, the moment vma_adjust drops the
 * rmap_locks, the properties of the merged vma will be already
 * correct for the whole merged range. Some of those properties like
 * vm_page_prot/vm_flags may be accessed by rmap_walks and they must
 * be correct for the whole merged range immediately after the
 * rmap_locks are released. Otherwise if XXXX would be removed and
 * NNNN would be extended over the XXXX range, remove_migration_ptes
 * or other rmap walkers (if working on addresses beyond the "end"
 * parameter) may establish ptes with the wrong permissions of NNNN
 * instead of the right permissions of XXXX.
 */
struct vm_area_struct *vma_merge(struct lego_mm_struct *mm,
			struct vm_area_struct *prev, unsigned long addr,
			unsigned long end, unsigned long vm_flags,
			struct anon_vma *anon_vma, struct lego_file *file,
			pgoff_t pgoff)
{
	pgoff_t pglen = (end - addr) >> PAGE_SHIFT;
	struct vm_area_struct *area, *next;
	int err;

	/*
	 * We later require that vma->vm_flags == vm_flags,
	 * so this tests vma->vm_flags & VM_SPECIAL, too.
	 */
	if (vm_flags & VM_SPECIAL)
		return NULL;

	if (prev)
		next = prev->vm_next;
	else
		next = mm->mmap;
	area = next;
	if (area && area->vm_end == end)		/* cases 6, 7, 8 */
		next = next->vm_next;

	/* verify some invariant that must be enforced by the caller */
	VM_WARN_ON(prev && addr <= prev->vm_start);
	VM_WARN_ON(area && end > area->vm_end);
	VM_WARN_ON(addr >= end);

	/*
	 * Can it merge with the predecessor?
	 */
	if (prev && prev->vm_end == addr &&
			can_vma_merge_after(prev, vm_flags,
					    anon_vma, file, pgoff)) {
		/*
		 * OK, it can.  Can we now merge in the successor as well?
		 */
		if (next && end == next->vm_start &&
				can_vma_merge_before(next, vm_flags,
						     anon_vma, file,
						     pgoff+pglen) &&
				is_mergeable_anon_vma(prev->anon_vma,
						      next->anon_vma, NULL)) {
							/* cases 1, 6 */
			err = __vma_adjust(prev, prev->vm_start,
					 next->vm_end, prev->vm_pgoff, NULL,
					 prev);
		} else					/* cases 2, 5, 7 */
			err = __vma_adjust(prev, prev->vm_start,
					 end, prev->vm_pgoff, NULL, prev);
		if (err)
			return NULL;
		return prev;
	}

	/*
	 * Can this new request be merged in front of next?
	 */
	if (next && end == next->vm_start &&
			can_vma_merge_before(next, vm_flags,
					     anon_vma, file, pgoff+pglen)) {
		if (prev && addr < prev->vm_end)	/* case 4 */
			err = __vma_adjust(prev, prev->vm_start,
					 addr, prev->vm_pgoff, NULL, next);
		else {					/* cases 3, 8 */
			err = __vma_adjust(area, addr, next->vm_end,
					 next->vm_pgoff - pglen, NULL, next);
			/*
			 * In case 3 area is already equal to next and
			 * this is a noop, but in case 8 "area" has
			 * been removed and next was expanded over it.
			 */
			area = next;
		}
		if (err)
			return NULL;
		return area;
	}

	return NULL;
}

/*
 * __split_vma() bypasses sysctl_max_map_count checking.  We use this on the
 * munmap path where it doesn't make sense to fail.
 */
static int __split_vma(struct lego_mm_struct *mm, struct vm_area_struct *vma,
	      unsigned long addr, int new_below)
{
	struct vm_area_struct *new;
	int err = 0;

	new = kmalloc(sizeof(*new), GFP_KERNEL);
	if (!new)
		return -ENOMEM;

	/* most fields are the same, copy all, and then fixup */
	*new = *vma;

	INIT_LIST_HEAD(&new->anon_vma_chain);

	if (new_below)
		new->vm_end = addr;
	else {
		new->vm_start = addr;
		new->vm_pgoff += ((addr - vma->vm_start) >> PAGE_SHIFT);
	}

#if 0
	err = anon_vma_clone(new, vma);
	if (err)
		goto out_free_mpol;

	if (new->vm_file)
		get_file(new->vm_file);

	if (new->vm_ops && new->vm_ops->open)
		new->vm_ops->open(new);
#endif

	if (new_below)
		err = vma_adjust(vma, addr, vma->vm_end, vma->vm_pgoff +
			((addr - new->vm_start) >> PAGE_SHIFT), new);
	else
		err = vma_adjust(vma, vma->vm_start, addr, vma->vm_pgoff, new);

	/* Success. */
	if (!err)
		return 0;

#if 0
	/* Clean everything up if vma_adjust failed. */
	if (new->vm_ops && new->vm_ops->close)
		new->vm_ops->close(new);
	if (new->vm_file)
		fput(new->vm_file);
	unlink_anon_vmas(new);
#endif

	kfree(new);
	return err;
}

/*
 * Get rid of page table information in the indicated region.
 *
 * Called with the mm semaphore held.
 */
static void unmap_region(struct lego_mm_struct *mm,
		struct vm_area_struct *vma, struct vm_area_struct *prev,
		unsigned long start, unsigned long end)
{
	//struct vm_area_struct *next = prev ? prev->vm_next : mm->mmap;

	//tlb_gather_mmu(&tlb, mm, start, end);
	//unmap_vmas(&tlb, vma, start, end);
	//free_pgtables(&tlb, vma, prev ? prev->vm_end : FIRST_USER_ADDRESS,
	//			 next ? next->vm_start : USER_PGTABLES_CEILING);
	//tlb_finish_mmu(&tlb, start, end);
}

/*
 * Create a list of vma's touched by the unmap, removing them from the mm's
 * vma list as we go..
 */
static void
detach_vmas_to_be_unmapped(struct lego_mm_struct *mm, struct vm_area_struct *vma,
	struct vm_area_struct *prev, unsigned long end)
{
	struct vm_area_struct **insertion_point;
	struct vm_area_struct *tail_vma = NULL;

	insertion_point = (prev ? &prev->vm_next : &mm->mmap);
	vma->vm_prev = NULL;
	do {
		vma_rb_erase(vma, &mm->mm_rb);
		mm->map_count--;
		tail_vma = vma;
		vma = vma->vm_next;
	} while (vma && vma->vm_start < end);
	*insertion_point = vma;
	if (vma) {
		vma->vm_prev = prev;
		vma_gap_update(vma);
	} else
		mm->highest_vm_end = prev ? prev->vm_end : 0;
	tail_vma->vm_next = NULL;
}

/*
 * Close a vm structure and free it, returning the next.
 */
static struct vm_area_struct *remove_vma(struct vm_area_struct *vma)
{
	struct vm_area_struct *next = vma->vm_next;

#if 0
	might_sleep();
	if (vma->vm_ops && vma->vm_ops->close)
		vma->vm_ops->close(vma);
	if (vma->vm_file)
		fput(vma->vm_file);
	mpol_put(vma_policy(vma));
#endif
	kfree(vma);
	return next;
}

/*
 * Ok - we have the memory areas we should free on the vma list,
 * so release them, and do the vma updates.
 *
 * Called with the mm semaphore held.
 */
static void remove_vma_list(struct lego_mm_struct *mm, struct vm_area_struct *vma)
{
	do {
		vma = remove_vma(vma);
	} while (vma);
	validate_mm(mm);
}

/* Munmap is split into 2 main parts -- this part which finds
 * what needs doing, and the areas themselves, which do the
 * work.  This now handles partial unmappings.
 * Jeremy Fitzhardinge <jeremy@goop.org>
 */
int do_munmap(struct lego_mm_struct *mm, unsigned long start, size_t len)
{
	unsigned long end;
	struct vm_area_struct *vma, *prev, *last;

	if ((offset_in_page(start)) || start > TASK_SIZE || len > TASK_SIZE-start)
		return -EINVAL;

	len = PAGE_ALIGN(len);
	if (len == 0)
		return -EINVAL;

	/* Find the first overlapping VMA */
	vma = find_vma(mm, start);
	if (!vma)
		return 0;
	prev = vma->vm_prev;
	/* we have  start < vma->vm_end  */

	/* if it doesn't overlap, we have nothing.. */
	end = start + len;
	if (vma->vm_start >= end)
		return 0;

	/*
	 * If we need to split any vma, do it now to save pain later.
	 *
	 * Note: mremap's move_vma VM_ACCOUNT handling assumes a partially
	 * unmapped vm_area_struct will remain in use: so lower split_vma
	 * places tmp vma above, and higher split_vma places tmp vma below.
	 */
	if (start > vma->vm_start) {
		int error;

		error = __split_vma(mm, vma, start, 0);
		if (error)
			return error;
		prev = vma;
	}

	/* Does it split the last one? */
	last = find_vma(mm, end);
	if (last && end > last->vm_start) {
		int error = __split_vma(mm, last, end, 1);
		if (error)
			return error;
	}
	vma = prev ? prev->vm_next : mm->mmap;

	/*
	 * Remove the vma's, and unmap the actual pages
	 */
	detach_vmas_to_be_unmapped(mm, vma, prev, end);
	unmap_region(mm, vma, prev, start, end);

	/* Fix up all other VM information */
	remove_vma_list(mm, vma);

	return 0;
}

int vm_munmap(struct lego_task_struct *p, unsigned long start, size_t len)
{
	int ret;

	ret = do_munmap(p->mm, start, len);
	return ret;
}

unsigned long unmapped_area(struct lego_task_struct *p,
			    struct vm_unmapped_area_info *info)
{
	/*
	 * We implement the search by looking for an rbtree node that
	 * immediately follows a suitable gap. That is,
	 * - gap_start = vma->vm_prev->vm_end <= info->high_limit - length;
	 * - gap_end   = vma->vm_start        >= info->low_limit  + length;
	 * - gap_end - gap_start >= length
	 */

	struct lego_mm_struct *mm = p->mm;
	struct vm_area_struct *vma;
	unsigned long length, low_limit, high_limit, gap_start, gap_end;

	/* Adjust search length to account for worst case alignment overhead */
	length = info->length + info->align_mask;
	if (length < info->length)
		return -ENOMEM;

	/* Adjust search limits by the desired length */
	if (info->high_limit < length)
		return -ENOMEM;
	high_limit = info->high_limit - length;

	if (info->low_limit > high_limit)
		return -ENOMEM;
	low_limit = info->low_limit + length;

	/* Check if rbtree root looks promising */
	if (RB_EMPTY_ROOT(&mm->mm_rb))
		goto check_highest;
	vma = rb_entry(mm->mm_rb.rb_node, struct vm_area_struct, vm_rb);
	if (vma->rb_subtree_gap < length)
		goto check_highest;

	while (true) {
		/* Visit left subtree if it looks promising */
		gap_end = vma->vm_start;
		if (gap_end >= low_limit && vma->vm_rb.rb_left) {
			struct vm_area_struct *left =
				rb_entry(vma->vm_rb.rb_left,
					 struct vm_area_struct, vm_rb);
			if (left->rb_subtree_gap >= length) {
				vma = left;
				continue;
			}
		}

		gap_start = vma->vm_prev ? vma->vm_prev->vm_end : 0;
check_current:
		/* Check if current node has a suitable gap */
		if (gap_start > high_limit)
			return -ENOMEM;
		if (gap_end >= low_limit && gap_end - gap_start >= length)
			goto found;

		/* Visit right subtree if it looks promising */
		if (vma->vm_rb.rb_right) {
			struct vm_area_struct *right =
				rb_entry(vma->vm_rb.rb_right,
					 struct vm_area_struct, vm_rb);
			if (right->rb_subtree_gap >= length) {
				vma = right;
				continue;
			}
		}

		/* Go back up the rbtree to find next candidate node */
		while (true) {
			struct rb_node *prev = &vma->vm_rb;
			if (!rb_parent(prev))
				goto check_highest;
			vma = rb_entry(rb_parent(prev),
				       struct vm_area_struct, vm_rb);
			if (prev == vma->vm_rb.rb_left) {
				gap_start = vma->vm_prev->vm_end;
				gap_end = vma->vm_start;
				goto check_current;
			}
		}
	}

check_highest:
	/* Check highest gap, which does not precede any rbtree node */
	gap_start = mm->highest_vm_end;
	gap_end = ULONG_MAX;  /* Only for VM_BUG_ON below */
	if (gap_start > high_limit)
		return -ENOMEM;

found:
	/* We found a suitable gap. Clip it with the original low_limit. */
	if (gap_start < info->low_limit)
		gap_start = info->low_limit;

	/* Adjust gap address to the desired alignment */
	gap_start += (info->align_offset - gap_start) & info->align_mask;

	VM_BUG_ON(gap_start + info->length > info->high_limit);
	VM_BUG_ON(gap_start + info->length > gap_end);
	return gap_start;
}

unsigned long unmapped_area_topdown(struct lego_task_struct *p,
				    struct vm_unmapped_area_info *info)
{
	struct lego_mm_struct *mm = p->mm;
	struct vm_area_struct *vma;
	unsigned long length, low_limit, high_limit, gap_start, gap_end;

	/* Adjust search length to account for worst case alignment overhead */
	length = info->length + info->align_mask;
	if (length < info->length)
		return -ENOMEM;

	/*
	 * Adjust search limits by the desired length.
	 * See implementation comment at top of unmapped_area().
	 */
	gap_end = info->high_limit;
	if (gap_end < length)
		return -ENOMEM;
	high_limit = gap_end - length;

	if (info->low_limit > high_limit)
		return -ENOMEM;
	low_limit = info->low_limit + length;

	/* Check highest gap, which does not precede any rbtree node */
	gap_start = mm->highest_vm_end;
	if (gap_start <= high_limit)
		goto found_highest;

	/* Check if rbtree root looks promising */
	if (RB_EMPTY_ROOT(&mm->mm_rb))
		return -ENOMEM;
	vma = rb_entry(mm->mm_rb.rb_node, struct vm_area_struct, vm_rb);
	if (vma->rb_subtree_gap < length)
		return -ENOMEM;

	while (true) {
		/* Visit right subtree if it looks promising */
		gap_start = vma->vm_prev ? vma->vm_prev->vm_end : 0;
		if (gap_start <= high_limit && vma->vm_rb.rb_right) {
			struct vm_area_struct *right =
				rb_entry(vma->vm_rb.rb_right,
					 struct vm_area_struct, vm_rb);
			if (right->rb_subtree_gap >= length) {
				vma = right;
				continue;
			}
		}

check_current:
		/* Check if current node has a suitable gap */
		gap_end = vma->vm_start;
		if (gap_end < low_limit)
			return -ENOMEM;
		if (gap_start <= high_limit && gap_end - gap_start >= length)
			goto found;

		/* Visit left subtree if it looks promising */
		if (vma->vm_rb.rb_left) {
			struct vm_area_struct *left =
				rb_entry(vma->vm_rb.rb_left,
					 struct vm_area_struct, vm_rb);
			if (left->rb_subtree_gap >= length) {
				vma = left;
				continue;
			}
		}

		/* Go back up the rbtree to find next candidate node */
		while (true) {
			struct rb_node *prev = &vma->vm_rb;
			if (!rb_parent(prev))
				return -ENOMEM;
			vma = rb_entry(rb_parent(prev),
				       struct vm_area_struct, vm_rb);
			if (prev == vma->vm_rb.rb_right) {
				gap_start = vma->vm_prev ?
					vma->vm_prev->vm_end : 0;
				goto check_current;
			}
		}
	}

found:
	/* We found a suitable gap. Clip it with the original high_limit. */
	if (gap_end > info->high_limit)
		gap_end = info->high_limit;

found_highest:
	/* Compute highest gap address at the desired alignment */
	gap_end -= info->length;
	gap_end -= (gap_end - info->align_offset) & info->align_mask;

	BUG_ON(gap_end < info->low_limit);
	BUG_ON(gap_end < gap_start);
	return gap_end;
}

static unsigned long
get_unmapped_area(struct lego_task_struct *p, struct lego_file *file,
		  unsigned long addr, unsigned long len, unsigned long pgoff,
		  unsigned long flags)
{
	unsigned long (*get_area)(struct lego_task_struct *, struct lego_file *, unsigned long,
				  unsigned long, unsigned long, unsigned long);

	/* Careful about overflows.. */
	if (len > TASK_SIZE)
		return -ENOMEM;

	/* XXX: may have different impls of get_area */

	/* chosen by arch_pick_mmap_layout */
	get_area = p->mm->get_unmapped_area;
	BUG_ON(!get_area);

	addr = get_area(p, file, addr, len, pgoff, flags);
	if (IS_ERR_VALUE(addr))
		return addr;

	if (addr > TASK_SIZE - len)
		return -ENOMEM;
	if (offset_in_page(addr))
		return -EINVAL;
	return addr;
}

unsigned long
mmap_region(struct lego_task_struct *p, struct lego_file *file,
	    unsigned long addr, unsigned long len, vm_flags_t vm_flags,
	    unsigned long pgoff)
{
	struct lego_mm_struct *mm = p->mm;
	struct vm_area_struct *vma, *prev;
	struct rb_node **rb_link, *rb_parent;

	/* Clear old maps */
	while (find_vma_links(mm, addr, addr + len, &prev, &rb_link,
			      &rb_parent)) {
		if (do_munmap(mm, addr, len))
			return -ENOMEM;
	}

	/*
	 * Can we just expand an old mapping?
	 */
	vma = vma_merge(mm, prev, addr, addr + len, vm_flags,
			NULL, file, pgoff);
	if (vma)
		goto out;

	vma = kmalloc(sizeof(*vma), GFP_KERNEL);
	if (!vma)
		return -ENOMEM;

	vma->vm_mm = mm;
	vma->vm_start = addr;
	vma->vm_end = addr + len;
	vma->vm_flags = vm_flags;
	vma->vm_page_prot = vm_get_page_prot(vm_flags);
	vma->vm_pgoff = pgoff;

	vma_link(mm, vma, prev, rb_link, rb_parent);

out:
	vma_set_page_prot(vma);
	return addr;
}

/* minimum virtual address that a process is allowed to mmap */
static unsigned long sysctl_mmap_min_addr = PAGE_SIZE;

/*
 * If a hint addr is less than mmap_min_addr change hint to be as
 * low as possible but still greater than mmap_min_addr
 */
static inline unsigned long round_hint_to_min(unsigned long hint)
{
	hint &= PAGE_MASK;
	if (((void *)hint != NULL) &&
	    (hint < sysctl_mmap_min_addr))
		return PAGE_ALIGN(sysctl_mmap_min_addr);
	return hint;
}

/*
 * The caller must hold down_write(&current->mm->mmap_sem).
 */
unsigned long do_mmap(struct lego_task_struct *p, struct lego_file *file,
	unsigned long addr, unsigned long len, unsigned long prot,
	unsigned long flags, vm_flags_t vm_flags, unsigned long pgoff)
{
	struct lego_mm_struct *mm = p->mm;

	if (!(flags & MAP_FIXED))
		addr = round_hint_to_min(addr);

	/* Careful about overflows.. */
	len = PAGE_ALIGN(len);
	if (!len)
		return -ENOMEM;

	/* offset overflow? */
	if ((pgoff + (len >> PAGE_SHIFT)) < pgoff)
		return -EOVERFLOW;

	/*
	 * Obtain the address to map to. we verify (or select) it and ensure
	 * that it represents a valid section of the address space.
	 */
	addr = get_unmapped_area(p, file, addr, len, pgoff, flags);
	if (unlikely(offset_in_page(addr))) /* which means error */
		return addr;

	/* Do simple checking here so the lower-level routines won't have
	 * to. we assume access permissions have been handled by the open
	 * of the memory object, so we don't do any here.
	 */
	vm_flags |= calc_vm_prot_bits(prot) | calc_vm_flag_bits(flags) |
			mm->def_flags;

	if (file) {
		switch (flags & MAP_TYPE) {
		case MAP_SHARED:
		case MAP_PRIVATE:
		default:
			return -EINVAL;
		}
	} else {
		switch (flags & MAP_TYPE) {
		case MAP_SHARED:
			if (vm_flags & (VM_GROWSDOWN|VM_GROWSUP))
				return -EINVAL;
			/*
			 * Ignore pgoff.
			 */
			pgoff = 0;
			vm_flags |= VM_SHARED;
			break;
		case MAP_PRIVATE:
			/*
			 * Set pgoff according to addr for anon_vma.
			 */
			pgoff = addr >> PAGE_SHIFT;
			break;
		default:
			return -EINVAL;
		}
	}

	addr = mmap_region(p, file, addr, len, vm_flags, pgoff);
	return addr;
}

static inline unsigned long
do_mmap_pgoff(struct lego_task_struct *p, struct lego_file *file,
	unsigned long addr, unsigned long len, unsigned long prot,
	unsigned long flags, unsigned long pgoff)
{
	return do_mmap(p, file, addr, len, prot, flags, 0, pgoff);
}

unsigned long vm_mmap_pgoff(struct lego_task_struct *p, struct lego_file *file,
		unsigned long addr, unsigned long len, unsigned long prot,
		unsigned long flag, unsigned long pgoff)
{
	unsigned long ret;

	/* TODO mm locking */
	ret = do_mmap_pgoff(p, file, addr, len, prot, flag, pgoff);
	return ret;
}

unsigned long vm_mmap(struct lego_task_struct *p, struct lego_file *file,
		unsigned long addr, unsigned long len, unsigned long prot,
		unsigned long flag, unsigned long offset)
{
	if (unlikely(offset + PAGE_ALIGN(len) < offset))
		return -EINVAL;
	if (unlikely(offset_in_page(offset)))
		return -EINVAL;

	return vm_mmap_pgoff(p, file, addr, len, prot, flag, offset >> PAGE_SHIFT);
}

static int do_brk(struct lego_task_struct *p, unsigned long addr,
		  unsigned long request)
{
	struct lego_mm_struct *mm = p->mm;
	struct vm_area_struct *vma, *prev;
	unsigned long flags, len;
	struct rb_node **rb_link, *rb_parent;
	pgoff_t pgoff = addr >> PAGE_SHIFT;
	int error;

	len = PAGE_ALIGN(request);
	if (len < request)
		return -ENOMEM;
	if (!len)
		return 0;

	flags = VM_READ | VM_WRITE | mm->def_flags;

	error = get_unmapped_area(p, NULL, addr, len, 0, MAP_FIXED);
	if (offset_in_page(error))
		return error;

	/*
	 * Clear old maps.  this also does some error checking for us
	 */
	while (find_vma_links(mm, addr, addr + len, &prev, &rb_link,
			      &rb_parent)) {
		if (do_munmap(mm, addr, len))
			return -ENOMEM;
	}

	/* Can we just expand an old private anonymous mapping? */
	vma = vma_merge(mm, prev, addr, addr + len, flags,
			NULL, NULL, pgoff);
	if (vma)
		goto out;

	/*
	 * create a vma struct for an anonymous mapping
	 */
	vma = kmalloc(sizeof(*vma), GFP_KERNEL);
	if (!vma)
		return -ENOMEM;

	INIT_LIST_HEAD(&vma->anon_vma_chain);
	vma->vm_mm = mm;
	vma->vm_start = addr;
	vma->vm_end = addr + len;
	vma->vm_pgoff = pgoff;
	vma->vm_flags = flags;
	vma->vm_page_prot = vm_get_page_prot(flags);
	vma_link(mm, vma, prev, rb_link, rb_parent);

out:
	mm->total_vm += len >> PAGE_SHIFT;
	mm->data_vm += len >> PAGE_SHIFT;
	return 0;
}

int vm_brk(struct lego_task_struct *p, unsigned long addr, unsigned long len)
{
	int ret;

	/* TODO mm locking */
	ret = do_brk(p, addr, len);
	return ret;
}

#define LEGO_PGALLOC_GFP     (GFP_KERNEL | __GFP_ZERO)

static pgd_t *lego_pgd_alloc(struct lego_mm_struct *mm)
{
	return (pgd_t *)__get_free_page(LEGO_PGALLOC_GFP);
}

static void lego_pgd_free(struct lego_mm_struct *mm)
{
	free_page((unsigned long)mm->pgd);
}

static struct lego_mm_struct *lego_mm_init(struct lego_mm_struct *mm,
					   struct lego_task_struct *p)
{
	atomic_set(&mm->mm_users, 1);
	atomic_set(&mm->mm_count, 1);

	mm->pgd = lego_pgd_alloc(mm);
	if (unlikely(!mm->pgd)) {
		kfree(mm);
		return NULL;
	}
	return mm;
}

struct lego_mm_struct *lego_mm_alloc(struct lego_task_struct *p)
{
	struct lego_mm_struct *mm;

	mm = kzalloc(sizeof(*mm), GFP_KERNEL);
	if (!mm)
		return NULL;
	return lego_mm_init(mm, p);
}

/*
 * Called when the last reference to the mm
 * is dropped: either by a lazy thread or by
 * mmput. Free the page directory and the mm.
 */
void __lego_mmdrop(struct lego_mm_struct *mm)
{
	lego_pgd_free(mm);
	kfree(mm);
}

void __lego_mmput(struct lego_mm_struct *mm)
{
	BUG_ON(atomic_read(&mm->mm_users));
	/*
	 * TODO exit a lot of things here
	 */
	lego_mmdrop(mm);
}
