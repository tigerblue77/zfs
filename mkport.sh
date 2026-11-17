#!/bin/sh
# Turn Linux's arch/x86/crypto/aes-gcm-aesni-x86_64.S into an ICP-style file:
# ICP linkage macros instead of <linux/linkage.h>, icp_ prefix on the exported
# symbols (Linux exports the same names), GNU-stack note.  Code body untouched.
# usage: mkport.sh <linux .S> <output .S>
set -eu
in=$1; out=$2
{
	printf '// SPDX-License-Identifier: Apache-2.0 OR BSD-2-Clause\n'
	printf '// Ported from Linux arch/x86/crypto/aes-gcm-aesni-x86_64.S\n'
	sed -e '1d' \
	    -e 's|^#include <linux/linkage.h>|#define _ASM\n#include <sys/asm_linkage.h>|' \
	    -e 's|^SYM_FUNC_START(\(.*\))|ENTRY_ALIGN(icp_\1, 16)\n\tENDBR|' \
	    -e 's|^SYM_FUNC_END(\(.*\))|SET_SIZE(icp_\1)|' "$in"
	printf '\n#ifdef __ELF__\n.section .note.GNU-stack,"",%%progbits\n#endif\n'
} > "$out"
