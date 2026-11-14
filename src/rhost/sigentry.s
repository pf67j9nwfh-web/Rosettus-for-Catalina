# sigentry.s - signal handler entry for rhost.
# The kernel builds the signal frame on the interrupted stack (for 32-bit code: its own stack; no
# sigaltstack, which makes the kernel drop the 32-bit %gs on return). Here: look up this thread's host
# data by thread id (r2_thread_lookup, plain C without thread-local storage), switch to the thread's
# host stack unless already on it (nested signal), set the host's %gs base, run on_signal_c, and if
# that returns 1 (back into 32-bit code) set the 32-bit world's %gs base again.
# thread_fast_set_cthread_self64 = machine-dependent call 3.
# Per-thread record (trap.c, r2thr_t): 0 host_tsd, 8 tsd32, 16 host stack top, 24 depth.
	.text
	.globl _r2_sigentry
	.p2align 4
_r2_sigentry:
	pushq	%rbp
	movq	%rsp, %rbp
	pushq	%rbx
	pushq	%r12
	pushq	%r13
	pushq	%r14
	movq	%rdi, %r12
	movq	%rsi, %r13
	movq	%rdx, %r14
	movl	$0x2000174, %eax          # thread_selfid
	syscall
	movq	%rax, %rdi
	andq	$-16, %rsp
	callq	_r2_thread_lookup         # -> r2thr_t * or 0
	movq	%rax, %rbx
	testq	%rbx, %rbx
	jz	2f
	movq	24(%rbx), %rax
	incq	24(%rbx)
	testq	%rax, %rax
	jnz	3f                         # nested: already on the host stack
	movq	16(%rbx), %rsp
3:	movq	(%rbx), %rdi
	movl	$0x3000003, %eax
	syscall
2:	andq	$-16, %rsp
	movq	%r12, %rdi
	movq	%r13, %rsi
	movq	%r14, %rdx
	callq	_on_signal_c
	movl	%eax, %r12d
	testq	%rbx, %rbx
	jz	1f
	decq	24(%rbx)
	testl	%r12d, %r12d
	jz	1f
	movq	8(%rbx), %rdi
	movl	$0x3000003, %eax
	syscall
1:	leaq	-32(%rbp), %rsp
	popq	%r14
	popq	%r13
	popq	%r12
	popq	%rbx
	popq	%rbp
	retq
