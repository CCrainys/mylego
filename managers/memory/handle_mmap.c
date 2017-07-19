/*
 * Copyright (c) 2016-2017 Wuklab, Purdue University. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <lego/mm.h>
#include <lego/rwsem.h>
#include <lego/slab.h>
#include <lego/rbtree.h>
#include <lego/sched.h>
#include <lego/kernel.h>
#include <lego/netmacro.h>
#include <lego/comp_memory.h>
#include <lego/fit_ibapi.h>

#include <memory/include/vm.h>
#include <memory/include/pid.h>
#include <memory/include/vm-pgtable.h>

/**
 * Returns: the brk address
 *  ERROR:
 *	RET_ESRCH
 *	RET_EINTR
 */
int handle_p2m_brk(struct p2m_brk_struct *payload, u64 desc,
		   struct common_header *hdr)
{
	u32 nid = hdr->src_nid;
	u32 pid = payload->pid;
	unsigned long min_brk, brk = payload->brk;
	unsigned long newbrk, oldbrk;
	struct lego_task_struct *tsk;
	struct lego_mm_struct *mm;
	unsigned long ret_brk;
	int ret;

	tsk = find_lego_task_by_pid(nid, pid);
	if (unlikely(!tsk)) {
		ret_brk = RET_ESRCH;
		ibapi_reply_message(&ret_brk, sizeof(ret_brk), desc);
		return 0;
	}
	mm = tsk->mm;

	if (down_write_killable(&mm->mmap_sem)) {
		ret_brk = RET_EINTR;
		ibapi_reply_message(&ret_brk, sizeof(ret_brk), desc);
		return 0;
	}

	min_brk = mm->start_brk;
	if (brk < min_brk)
		goto out;

	newbrk = PAGE_ALIGN(brk);
	oldbrk = PAGE_ALIGN(mm->brk);

	/* within same page, great! */
	if (oldbrk == newbrk)
		goto set_brk;

	/* Shrink the brk */
	if (brk <= mm->brk) {
		ret = do_munmap(mm, newbrk, oldbrk - newbrk);
		if (likely(!ret))
			goto set_brk;
		goto out;
	}

	/* Check against existing mmap mappings. */
	if (find_vma_intersection(mm, oldbrk, newbrk+PAGE_SIZE))
		goto out;

	/* Ok, looks good - let it rip. */
	ret = do_brk(tsk, oldbrk, newbrk-oldbrk);
	if (unlikely(ret < 0))
		goto out;

set_brk:
	mm->brk = brk;

	/* Yup, by default, we populate */
	if (newbrk > oldbrk)
		lego_mm_populate(mm, oldbrk, newbrk - oldbrk);

out:
	up_write(&mm->mmap_sem);

	ret_brk = mm->brk;
	ibapi_reply_message(&ret_brk, sizeof(ret_brk), desc);

	return 0;
}

/**
 * Returns: the virtual address
 *  ERROR:
 *	RET_ESRCH
 */
int handle_p2m_mmap(struct p2m_mmap_struct *payload, u64 desc,
		    struct common_header *hdr)
{
	u32 nid = hdr->src_nid;
	u32 pid = payload->pid;
	u64 addr = payload->addr;
	u64 len = payload->len;
	u64 prot = payload->prot;
	u64 flags = payload->flags;
	u64 pgoff = payload->pgoff;
	struct lego_task_struct *tsk;
	struct lego_file *file = NULL;
	struct p2m_mmap_reply_struct reply;
	s64 ret;

	tsk = find_lego_task_by_pid(nid, pid);
	if (unlikely(!tsk)) {
		reply.ret = RET_ESRCH;
		goto out;
	}

	if (!(flags & MAP_ANONYMOUS)) {
		/* use fd to find file */
		/* file backed mmap() */
		file = NULL;
	}

	flags &= ~(MAP_EXECUTABLE | MAP_DENYWRITE);
	ret = vm_mmap_pgoff(tsk, file, addr, len, prot, flags, pgoff);

	/* which means vm_mmap_pgoff() returns -ERROR */
	if (unlikely(ret < 0)) {
		reply.ret = ERR_TO_LEGO_RET(ret);
		goto out;
	}

	reply.ret = RET_OKAY;
	reply.ret_addr = (u64)ret;

out:
	ibapi_reply_message(&reply, sizeof(reply), desc);

	return 0;
}

int handle_p2m_munmap(struct p2m_munmap_struct *payload, u64 desc,
		      struct common_header *hdr)
{
	u32 nid = hdr->src_nid;
	u32 pid = payload->pid;
	u64 addr = payload->addr;
	u64 len = payload->len;
	struct lego_task_struct *tsk;
	struct lego_mm_struct *mm;
	u64 ret;

	tsk = find_lego_task_by_pid(nid, pid);
	if (unlikely(!tsk)) {
		ret = RET_ESRCH;
		goto out;
	}

	mm = tsk->mm;
	if (down_write_killable(&mm->mmap_sem)) {
		ret = RET_EINTR;
		goto out;
	}

	ret = do_munmap(mm, addr, len);
	up_write(&mm->mmap_sem);

out:
	ibapi_reply_message(&ret, sizeof(ret), desc);
	return 0;
}
