/* coro_mips.S: the coroutine switch on 32-bit MIPS with an FPU (the PSP's
   Allegrex, EABI: s0-s7, s8, ra, sp and $f20-$f31 are the callee's to
   keep). */
#if defined(__mips__) && defined(__mips_hard_float) && _MIPS_SZPTR == 32
    .set noreorder
    .text
    .align 2

    .globl coro_swap
    .ent coro_swap
coro_swap:
    addiu $sp, $sp, -96
    sw $16, 0($sp)
    sw $17, 4($sp)
    sw $18, 8($sp)
    sw $19, 12($sp)
    sw $20, 16($sp)
    sw $21, 20($sp)
    sw $22, 24($sp)
    sw $23, 28($sp)
    sw $30, 32($sp)
    sw $31, 36($sp)
    swc1 $f20, 40($sp)
    swc1 $f21, 44($sp)
    swc1 $f22, 48($sp)
    swc1 $f23, 52($sp)
    swc1 $f24, 56($sp)
    swc1 $f25, 60($sp)
    swc1 $f26, 64($sp)
    swc1 $f27, 68($sp)
    swc1 $f28, 72($sp)
    swc1 $f29, 76($sp)
    swc1 $f30, 80($sp)
    swc1 $f31, 84($sp)
    sw $sp, 0($4)
    move $sp, $5
    lw $16, 0($sp)
    lw $17, 4($sp)
    lw $18, 8($sp)
    lw $19, 12($sp)
    lw $20, 16($sp)
    lw $21, 20($sp)
    lw $22, 24($sp)
    lw $23, 28($sp)
    lw $30, 32($sp)
    lw $31, 36($sp)
    lwc1 $f20, 40($sp)
    lwc1 $f21, 44($sp)
    lwc1 $f22, 48($sp)
    lwc1 $f23, 52($sp)
    lwc1 $f24, 56($sp)
    lwc1 $f25, 60($sp)
    lwc1 $f26, 64($sp)
    lwc1 $f27, 68($sp)
    lwc1 $f28, 72($sp)
    lwc1 $f29, 76($sp)
    lwc1 $f30, 80($sp)
    lwc1 $f31, 84($sp)
    jr $31
    addiu $sp, $sp, 96
    .end coro_swap

    .globl coro_start
    .ent coro_start
coro_start:
    move $4, $16
    move $30, $0
    jal coro_entry
    addiu $sp, $sp, -16        /* (in the delay slot) room for a callee that keeps its arguments */
    break
    .end coro_start
#endif
