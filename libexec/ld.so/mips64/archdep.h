/*	$OpenBSD: archdep.h,v 1.10 2015/12/06 23:36:12 guenther Exp $ */

/*
 * Copyright (c) 1998-2002 Opsycon AB, Sweden.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

#ifndef _MIPS_ARCHDEP_H_
#define _MIPS_ARCHDEP_H_

#include <link.h>

#include "syscall.h"
#include "resolve.h"
#include "util.h"

#define	RELOC_TAG	DT_REL
#define	DL_MALLOC_ALIGN	8	/* Arch constraint or otherwise */
#define	MACHID	EM_MIPS		/* ELF e_machine ID value checked */


#define RELOC_DYN(relp, symp, adrp, val)				\
do {									\
	if (ELF64_R_TYPE(relp->r_info) == R_MIPS_REL32_64) {		\
		if (ELF64_R_SYM(relp->r_info) != 0)			\
			*adrp += symp->st_value + val;			\
		else							\
			*adrp += val;					\
	} else if (ELF64_R_TYPE(relp->r_info) != R_MIPS_NONE) {		\
		_dl_exit(ELF64_R_TYPE(relp->r_info)+100);		\
	}								\
} while (0)

#define RELOC_GOT(obj, off)						\
do {									\
	struct boot_dyn *__dynld = obj;					\
	long __loff = off;						\
	Elf64_Addr *gotp;						\
	int i, n;							\
	const Elf_Sym *sp;						\
									\
	/* Do all local gots */						\
	gotp = __dynld->dt_pltgot;					\
	n = __dynld->dt_proc[DT_MIPS_LOCAL_GOTNO - DT_LOPROC];		\
									\
	for (i = ((gotp[1] & 0x0000000080000000) ? 2 : 1); i < n; i++) {\
		gotp[i] += __loff;					\
	}								\
	gotp += n;							\
									\
	/* Do symbol referencing gots. There should be no global... */	\
	n =  __dynld->dt_proc[DT_MIPS_SYMTABNO - DT_LOPROC] -		\
	  __dynld->dt_proc[DT_MIPS_GOTSYM - DT_LOPROC];			\
	sp = __dynld->dt_symtab;					\
	sp += __dynld->dt_proc[DT_MIPS_GOTSYM - DT_LOPROC];		\
									\
	while (n--) {							\
		if (sp->st_shndx == SHN_UNDEF ||			\
		    sp->st_shndx == SHN_COMMON) {			\
			if (ELF64_ST_BIND(sp->st_info) != STB_WEAK)	\
				_dl_exit(7);				\
		} else if (ELF64_ST_TYPE(sp->st_info) == STT_FUNC) {	\
			*gotp += __loff;				\
		} else {						\
			*gotp = sp->st_value + __loff;			\
		}							\
		gotp++;							\
		sp++;							\
	}								\
} while (0)

#define GOT_PERMS PROT_READ

#endif /* _MIPS_ARCHDEP_H_ */
