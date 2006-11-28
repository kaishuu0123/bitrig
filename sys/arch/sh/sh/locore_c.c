/*	$OpenBSD: locore_c.c,v 1.2 2006/11/28 18:52:23 kettenis Exp $	*/
/*	$NetBSD: locore_c.c,v 1.13 2006/03/04 01:13:35 uwe Exp $	*/

/*-
 * Copyright (c) 1996, 1997, 2002 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Jason R. Thorpe of the Numerical Aerospace Simulation Facility,
 * NASA Ames Research Center.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the NetBSD
 *	Foundation, Inc. and its contributors.
 * 4. Neither the name of The NetBSD Foundation nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*-
 * Copyright (c) 1982, 1987, 1990 The Regents of the University of California.
 * All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * William Jolitz.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)Locore.c
 */

/*-
 * Copyright (c) 1993, 1994, 1995, 1996, 1997
 *	 Charles M. Hannum.  All rights reserved.
 * Copyright (c) 1992 Terrence R. Lambert.
 *
 * This code is derived from software contributed to Berkeley by
 * William Jolitz.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)Locore.c
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/user.h>
#include <sys/sched.h>
#include <sys/proc.h>

#include <uvm/uvm_extern.h>

#include <sh/locore.h>
#include <sh/cpu.h>
#include <sh/pmap.h>
#include <sh/mmu_sh3.h>
#include <sh/mmu_sh4.h>
#include <sh/ubcreg.h>

void (*__sh_switch_resume)(struct proc *);
struct proc *cpu_switch_search(struct proc *);
struct proc *cpu_switch_prepare(struct proc *, struct proc *);
void idle(void);
int want_resched;

#ifdef LOCKDEBUG
#define	SCHED_LOCK_IDLE()	sched_lock_idle()
#define	SCHED_UNLOCK_IDLE()	sched_unlock_idle()
#else
#define	SCHED_LOCK_IDLE()	do {} while (/* CONSTCOND */ 0)
#define	SCHED_UNLOCK_IDLE()	do {} while (/* CONSTCOND */ 0)
#endif


/*
 * Prepare context switch from oproc to nproc.
 * This code is shared by cpu_switch and cpu_switchto.
 */
struct proc *
cpu_switch_prepare(struct proc *oproc, struct proc *nproc)
{
	nproc->p_stat = SONPROC;

	if (oproc && (oproc->p_md.md_flags & MDP_STEP))
		_reg_write_2(SH_(BBRB), 0);

	if (nproc != oproc) {
		curpcb = nproc->p_md.md_pcb;
		pmap_activate(nproc);
	}

	if (nproc->p_md.md_flags & MDP_STEP) {
		int pm_asid = nproc->p_vmspace->vm_map.pmap->pm_asid;

		_reg_write_2(SH_(BBRB), 0);
		_reg_write_4(SH_(BARB), nproc->p_md.md_regs->tf_spc);
		_reg_write_1(SH_(BASRB), pm_asid);
		_reg_write_1(SH_(BAMRB), 0);
		_reg_write_2(SH_(BRCR), 0x0040);
		_reg_write_2(SH_(BBRB), 0x0014);
	}

	curproc = nproc;
	return (nproc);
}

/*
 * Find the highest priority proc and prepare to switching to it.
 */
struct proc *
cpu_switch_search(struct proc *oproc)
{
	struct prochd *q;
	struct proc *p;

	curproc = NULL;

	SCHED_LOCK_IDLE();
	while (whichqs == 0) {
		SCHED_UNLOCK_IDLE();
		idle();
		SCHED_LOCK_IDLE();
	}

	q = &qs[ffs(whichqs) - 1];
	p = q->ph_link;
	remrunqueue(p);
	want_resched = 0;
	SCHED_UNLOCK_IDLE();

	return (cpu_switch_prepare(oproc, p));
}

/*
 * void idle(void):
 *	When no processes are on the run queue, wait for something to come
 *	ready. Separated function for profiling.
 */
void
idle()
{
	spl0();
	uvm_pageidlezero();
	__asm volatile("sleep");
	splsched();
}

#ifndef P1_STACK
#ifdef SH3
/*
 * void sh3_switch_setup(struct proc *p):
 *	prepare kernel stack PTE table. TLB miss handler check these.
 */
void
sh3_switch_setup(struct proc *p)
{
	pt_entry_t *pte;
	struct md_upte *md_upte = p->p_md.md_upte;
	uint32_t vpn;
	int i;

	vpn = (uint32_t)p->p_addr;
	vpn &= ~PGOFSET;
	for (i = 0; i < UPAGES; i++, vpn += PAGE_SIZE, md_upte++) {
		pte = __pmap_kpte_lookup(vpn);
		KDASSERT(pte && *pte != 0);

		md_upte->addr = vpn;
		md_upte->data = (*pte & PG_HW_BITS) | PG_D | PG_V;
	}
}
#endif /* SH3 */

#ifdef SH4
/*
 * void sh4_switch_setup(struct proc *p):
 *	prepare kernel stack PTE table. sh4_switch_resume wired this PTE.
 */
void
sh4_switch_setup(struct proc *p)
{
	pt_entry_t *pte;
	struct md_upte *md_upte = p->p_md.md_upte;
	uint32_t vpn;
	int i, e;

	vpn = (uint32_t)p->p_addr;
	vpn &= ~PGOFSET;
	e = SH4_UTLB_ENTRY - UPAGES;
	for (i = 0; i < UPAGES; i++, e++, vpn += PAGE_SIZE) {
		pte = __pmap_kpte_lookup(vpn);
		KDASSERT(pte && *pte != 0);
		/* Address array */
		md_upte->addr = SH4_UTLB_AA | (e << SH4_UTLB_E_SHIFT);
		md_upte->data = vpn | SH4_UTLB_AA_D | SH4_UTLB_AA_V;
		md_upte++;
		/* Data array */
		md_upte->addr = SH4_UTLB_DA1 | (e << SH4_UTLB_E_SHIFT);
		md_upte->data = (*pte & PG_HW_BITS) |
		    SH4_UTLB_DA1_D | SH4_UTLB_DA1_V;
		md_upte++;
	}
}
#endif /* SH4 */
#endif /* !P1_STACK */

/*
 * copystr(caddr_t from, caddr_t to, size_t maxlen, size_t *lencopied);
 * Copy a NUL-terminated string, at most maxlen characters long.  Return the
 * number of characters copied (including the NUL) in *lencopied.  If the
 * string is too long, return ENAMETOOLONG; else return 0.
 */
int
copystr(const void *kfaddr, void *kdaddr, size_t maxlen, size_t *lencopied)
{
	const char *from = kfaddr;
	char *to = kdaddr;
	int i;

	for (i = 0; maxlen-- > 0; i++) {
		if ((*to++ = *from++) == '\0') {
			if (lencopied)
				*lencopied = i + 1;
			return (0);
		}
	}

	if (lencopied)
		*lencopied = i;

	return (ENAMETOOLONG);
}
