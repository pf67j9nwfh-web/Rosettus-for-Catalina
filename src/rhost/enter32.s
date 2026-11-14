# enter32.s - r2_enter32(frame, regs, ds, gs): load ds/es (and gs if not 0), the six 32-bit registers
# (regs: eax ebx ecx edx esi edi) and iretq through frame (rip cs rflags rsp ss) into 32-bit code.
	.text
	.globl _r2_enter32
	.p2align 4
_r2_enter32:
	testw	%cx, %cx
	jz	1f
	movw	%cx, %gs                    # the thread's 32-bit %gs (its base comes from the LDT entry)
1:	movw	%dx, %ds
	movw	%dx, %es
	movq	%rdi, %rsp
	movl	0(%rsi), %eax
	movl	4(%rsi), %ebx
	movl	8(%rsi), %ecx
	movl	12(%rsi), %edx
	movl	20(%rsi), %edi
	movl	16(%rsi), %esi
	xorl	%ebp, %ebp
	iretq
