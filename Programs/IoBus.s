; The I/O bus: the system slot's registers, read back and poked at the edges (aliases, empty
; slots, byte accesses), printed so the emulator and the gate-level computer must agree.
INT_PENDING = 0xFC00
INT_MASK    = 0xFC02
TIMER       = 0xFC04
TIMER_ACK   = 0xFC06
UART        = 0xFC80

_start:
    li r5, INT_MASK
    li r0, 0x5AA5
    sw r0, [r5]
    lw r0, [r5]             ; 00A5: the mask is 8 bits
    jal hex
    lb r0, [r5+1]           ; 0000: its high byte
    jal hex
    li r0, 0
    sw r0, [r5]             ; mask everything off again

    li r5, INT_PENDING
    lw r0, [r5]             ; 0000: nothing pending
    jal hex
    li r0, 3
    sw r0, [r5+4]           ; timer every 3 instructions, masked off
    nop
    nop
    nop
    lw r0, [r5]             ; 0001: the timer is pending, masked or not
    jal hex
    sw r0, [r5+6]           ; acknowledge (the timer stays pending while it runs: stop it first)
    li r0, 0
    sw r0, [r5+4]
    sw r0, [r5+6]
    lw r0, [r5]             ; 0000: acknowledged
    jal hex
    lw r0, [r5+4]           ; 0000: the reload reads back
    jal hex

    li r0, 0x1234
    sb r0, [r5+5]           ; a byte write to an odd I/O address does nothing
    lw r0, [r5+4]           ; 0000
    jal hex
    li r0, 0x1234
    sb r0, [r5+4]           ; an even one writes the low byte, the high one zero
    lw r0, [r5+4]           ; 0034
    jal hex
    li r0, 0
    sw r0, [r5+4]

    lw r0, [r5+8]           ; 0000: the registers don't repeat through the slot
    jal hex
    li r4, 0xFD00
    lw r0, [r4]             ; 0000: an empty slot
    jal hex
    halt

hex:                        ; print r0 as 4 hex digits and a space. Clobbers r0-r3.
    li r2, 4
    li r3, UART
digit:
    ldi r1, 12
    shr r1, r0, r1
    cmpi r1, 10
    blt decimal
    addi r1, r1, 7
decimal:
    addi r1, r1, 24
    addi r1, r1, 24
    sb r1, [r3]
    ldi r1, 4
    shl r0, r0, r1
    addi r2, r2, -1
    bne digit
    ldi r1, 32
    sb r1, [r3]
    ret
