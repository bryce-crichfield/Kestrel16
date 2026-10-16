; Timer interrupts landing in every kind of instruction: the result depends on exactly
; where each interrupt hits, so the emulator and the gate-level computer must agree.
_start:
    j main
handler:                    ; 0x0002: count interrupts
    push r0
    li r0, 0xFC06           ; timer acknowledge: drop the request
    sw r0, [r0]
    pop r0
    addi r5, r5, 1
    reti

main:
    li r5, 0
    li r3, 0
    li r4, 600
    li sp, 0xFE00
    li r0, 0xFC02           ; interrupt mask: the timer (slot 0) only
    li r1, 1
    sw r1, [r0]
    li r0, 0xFC04           ; timer reload: every 11 instructions, a little longer than the handler
    li r1, 11
    sw r1, [r0]
    ei
loop:
    add r3, r3, r5
    li r1, 3
    shl r2, r3, r1
    xor r3, r3, r2
    sw r3, [sp]
    lw r2, [sp]
    sb r5, [sp+1]
    lb r1, [sp+1]
    add r3, r3, r1
    cmpi r2, 0
    beq skip
    addi r3, r3, 5
skip:
    addi r4, r4, -1
    bne loop
    nop
    di
    mov r0, r3
    jal hex
    mov r0, r5
    jal hex
    halt

hex:                        ; print r0 as 4 hex digits and a space
    li r2, 4
    li r6, 0xFC80           ; UART data
digit:
    li r1, 12
    shr r1, r0, r1
    li r4, 10
    cmp r1, r4
    blt decimal
    addi r1, r1, 7
decimal:
    addi r1, r1, 24
    addi r1, r1, 24
    sb r1, [r6]
    li r1, 4
    shl r0, r0, r1
    addi r2, r2, -1
    bne digit
    li r1, 32
    sb r1, [r6]
    ret
