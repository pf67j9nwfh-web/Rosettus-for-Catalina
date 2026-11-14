# gate.s - the system-call gate: the 32-bit world's sysenter stubs reach the host without a signal.
#
# A patched stub ("popl %edx; movl %esp,%ecx; sysenter", Mojave's i386 libsystem_kernel) jumps to a
# 32-bit trampoline that does the same two moves and a far jump into 64-bit code (a small stub in
# low memory that jumps to r2_gate64). Here, still on the 32-bit stack with the 32-bit %gs base:
#   - the 32-bit registers, flags, %esp (= %ecx), the return address (%edx, as for sysexit) and the
#     %gs selector go into a block below the 32-bit stack pointer, past the 128 bytes the kernel
#     leaves alone below it when it builds a signal frame (the translator keeps data there);
#   - the thread's record is found by thread id (as in sigentry.s); the depth count goes up before
#     the switch to the host stack (a signal now stays on the current stack, as a nested one does);
#   - the host %gs base is set (thread_fast_set_cthread_self64), the 32-bit world's float and vector
#     state saved (xsave), and r2_gate_c (trap.c) runs the system call on the block and leaves the
#     registers to return with in a second block on the host stack (a 32-bit signal handler's frame,
#     built by the call just below the 32-bit %esp, covers the first one);
# and back: the float state is restored, the 32-bit %gs base set again, an iretq frame is built on
# the 32-bit stack below both the new stack pointer and the block, the depth count goes down (a
# signal from here on is taken on the host stack; on_signal_c keeps the 32-bit %gs base in its frame
# when the interrupted address is in [r2_gate_exit_lo, r2_gate_exit_hi)), and iretq.
# The block: u32 eax ebx ecx edx esi edi ebp eflags esp eip gs (offsets 0..40).
	.text
	.globl _r2_gate64, _r2_gate_entry_hi, _r2_gate_exit_lo, _r2_gate_exit_hi
	.p2align 4
_r2_gate64:
	movl	%esp, %esp                # the 32-bit stack pointer, zero-extended
	subq	$192, %rsp                # 128 bytes below it stay untouched, as the kernel leaves them
	                                  # when it builds a signal frame: Rosetta's translator keeps data
	                                  # there across a system call (G4 Quasar lost a mapping now and then)
	andq	$-16, %rsp
	movl	%eax, 0(%rsp)
	movl	%ebx, 4(%rsp)
	movl	%ecx, 8(%rsp)
	movl	%edx, 12(%rsp)
	movl	%esi, 16(%rsp)
	movl	%edi, 20(%rsp)
	movl	%ebp, 24(%rsp)
	pushfq
	popq	%rax
	movl	%eax, 28(%rsp)
	movl	%ecx, 32(%rsp)            # %esp after "popl %edx; movl %esp,%ecx"
	movl	%edx, 36(%rsp)            # where sysexit would return
	movw	%gs, %ax
	movzwl	%ax, %eax
	movl	%eax, 40(%rsp)
	cld
	movq	%rsp, %r12                # the block
	movl	$0x2000174, %eax          # thread_selfid
	syscall
	movq	%rax, %rdi
	callq	_r2_thread_lookup         # (no thread-local storage used)
	testq	%rax, %rax
	jz	9f
	movq	%rax, %rbx
	movq	24(%rbx), %rax
	incq	24(%rbx)
	testq	%rax, %rax
	jnz	1f
	movq	16(%rbx), %rsp            # the host stack
1:	movq	(%rbx), %rdi              # host %gs base
	movl	$0x3000003, %eax
	syscall
_r2_gate_entry_hi:
	movslq	_g_gate_xsz(%rip), %rax
	subq	%rax, %rsp
	andq	$-64, %rsp
	movq	%rsp, %r13                # xsave area (64-byte aligned)
	xorl	%eax, %eax                # its header must be zero for xrstor
	movq	%rax, 512(%r13)
	movq	%rax, 520(%r13)
	movq	%rax, 528(%r13)
	movq	%rax, 536(%r13)
	movq	%rax, 544(%r13)
	movq	%rax, 552(%r13)
	movq	%rax, 560(%r13)
	movq	%rax, 568(%r13)
	movl	$-1, %eax
	movl	$-1, %edx
	xsave64	(%r13)
	subq	$64, %rsp
	movq	%rsp, %r15                # where r2_gate_c leaves the registers to return with: on the
	movq	%r12, %rdi                # host stack, not in the block, which a 32-bit signal frame built
	movq	%r13, %rsi                # by the call (just below the 32-bit %esp) can cover
	movq	%r15, %rdx
	callq	_r2_gate_c
	movl	$-1, %eax
	movl	$-1, %edx
	xrstor64 (%r13)
	movq	8(%rbx), %rdi             # the 32-bit world's %gs base
	movl	$0x3000003, %eax
	syscall
_r2_gate_exit_lo:
	movl	32(%r15), %eax            # new 32-bit %esp
	cmpq	%r12, %rax
	jb	2f
	movq	%r12, %rax
2:	subq	$224, %rax                # the 96-byte frame ends 128 bytes below both
	andq	$-16, %rax
	movq	%rax, %r14                # iretq frame: eax ebx ecx edx esi edi ebp | rip cs rflags rsp ss
	movl	0(%r15), %ecx
	movq	%rcx, 0(%r14)
	movl	4(%r15), %ecx
	movq	%rcx, 8(%r14)
	movl	8(%r15), %ecx
	movq	%rcx, 16(%r14)
	movl	12(%r15), %ecx
	movq	%rcx, 24(%r14)
	movl	16(%r15), %ecx
	movq	%rcx, 32(%r14)
	movl	20(%r15), %ecx
	movq	%rcx, 40(%r14)
	movl	24(%r15), %ecx
	movq	%rcx, 48(%r14)
	movl	36(%r15), %ecx
	movq	%rcx, 56(%r14)
	movzwl	_g_cs32(%rip), %ecx
	movq	%rcx, 64(%r14)
	movl	28(%r15), %ecx
	movq	%rcx, 72(%r14)
	movl	32(%r15), %ecx
	movq	%rcx, 80(%r14)
	movzwl	_g_ds32(%rip), %ecx
	movq	%rcx, 88(%r14)
	movl	40(%r15), %r15d           # %gs selector
	movq	%r14, %rsp
	decq	24(%rbx)
	movw	%cx, %ds
	movw	%cx, %es
	movw	%r15w, %gs
	popq	%rax
	popq	%rbx
	popq	%rcx
	popq	%rdx
	popq	%rsi
	popq	%rdi
	popq	%rbp
	iretq
_r2_gate_exit_hi:
9:	ud2                               # no record for this thread: a host fault (dumped by the handler)
