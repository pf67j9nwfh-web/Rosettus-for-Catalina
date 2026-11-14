# reserve.s - a __LOW32 segment at 0x1000 (-segaddr); fixseg.py then makes it zero-fill up to 0xfffec000,
# so the 32-bit world's address space is taken at exec, before libSystem's start-up allocates anything.
	.globl _r2_low32
	.zerofill __LOW32,__low32,_r2_low32,0x1000,12
# __GUARD: 256 MB of no access just above 4 GB (fixseg.py), so that a write past the top of the
# 32-bit world faults instead of landing in the host malloc regions that would otherwise start there.
	.globl _r2_guard
	.zerofill __GUARD,__guard,_r2_guard,0x1000,12
