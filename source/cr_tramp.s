// cr_tramp -- detour trampoline for fnaTexture_CreateRender (see hooks/game.c).
//
// hook_arm64() overwrites the engine function's first 16 bytes (4 instructions)
// with a branch to my_CreateRender. To still run the original after fixing up the
// width/height args, my_CreateRender tail-calls this: it re-executes those exact
// 4 prologue instructions, then jumps back into the engine at +0x10 (the runtime
// address stashed in g_cr_continue). w0..w4 (the args) flow through untouched.
//
// Must be a hand-written .s: GCC ignores __attribute__((naked)) on AArch64.

	.global	cr_tramp
	.type	cr_tramp, %function
	.balign	4
cr_tramp:
	sub	sp, sp, #0x70
	str	x25, [sp, #0x20]
	stp	x24, x23, [sp, #0x30]
	stp	x22, x21, [sp, #0x40]
	adrp	x16, g_cr_continue
	ldr	x16, [x16, #:lo12:g_cr_continue]
	br	x16
	.size	cr_tramp, .-cr_tramp
