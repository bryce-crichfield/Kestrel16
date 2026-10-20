; Fibonacci.s -- print every Fibonacci number that fits in 16 bits, then report
; how many timer interrupts fired while doing it. The hardware check runs it on the
; gate-level Kestrel16 Computer (Tools/make_cpu.py loads it into the RAMs).
;
; Shows off: subroutine calls and a stack, software division (the ISA has
; no divide instruction), 16-bit constants via li, carry-based overflow
; detection, and a timer interrupt handler.

UART        = 0xFC80        ; slot 1: data
INT_MASK    = 0xFC02        ; slot 0: interrupt mask, bit n = slot n
TIMER       = 0xFC04        ; slot 0: timer reload
TIMER_ACK   = 0xFC06        ; slot 0: timer acknowledge
STACK_TOP   = 0xFC00        ; stack grows down from the top of RAM
TICK_PERIOD = 100           ; timer interrupt every 100 instructions

; ------------------------------------------------------------------ vectors

        .org 0x0000
reset:
        j    start          ; skip over the interrupt handler

        .org 0x0002         ; interrupt / trap vector
interrupt:
        push r0             ; save everything the handler touches;
        push r1             ; reti restores the flags for us
        li   r1, TIMER_ACK
        sw   r1, [r1]       ; drop the timer's request
        la   r1, ticks
        lw   r0, [r1]
        addi r0, r0, 1
        sw   r0, [r1]
        pop  r1
        pop  r0
        reti

; ------------------------------------------------------------------ main

start:
        li   sp, STACK_TOP
        li   r0, INT_MASK
        ldi  r1, 1          ; let the timer (slot 0) interrupt
        sw   r1, [r0]
        li   r0, TIMER
        li   r1, TICK_PERIOD
        sw   r1, [r0]       ; start the timer
        ei

        la   r0, banner
        call puts

        ldi  r4, 0          ; r4 = a (number to print)
        ldi  r5, 1          ; r5 = b (the one after it)
next_number:
        mov  r0, r4
        call print_decimal
        ldi  r0, ' '
        call putc
        add  r0, r4, r5     ; a + b -- carry set means it no longer fits
        bcs  last_number
        mov  r4, r5         ; mov doesn't touch flags, but we're past the test
        mov  r5, r0
        bra  next_number
last_number:
        mov  r0, r5         ; b still fit, so print it too
        call print_decimal

        di                  ; freeze the tick count
        la   r0, ticks_label
        call puts
        la   r1, ticks
        lw   r0, [r1]
        call print_decimal
        ldi  r0, '\n'
        call putc
        halt                ; interrupts are off, so this ends the program

; ------------------------------------------------------------------ routines
; Calling convention: arguments and results in r0/r1, r2/r3 are scratch,
; r4/r5 are preserved, lr holds the return address.

; putc(r0 = character).  Clobbers r1.
putc:
        li   r1, UART
        sb   r0, [r1]
        ret

; puts(r0 = address of a zero-terminated string).  Clobbers r0-r2.
puts:
        li   r2, UART
puts_loop:
        lb   r1, [r0]
        cmpi r1, 0
        beq  puts_done
        sb   r1, [r2]
        addi r0, r0, 1
        bra  puts_loop
puts_done:
        ret

; print_decimal(r0 = unsigned value).  Clobbers r0-r3.
; Divides by 10 until nothing is left, pushing each digit, then pops them
; so they come out most significant first.
print_decimal:
        push lr             ; we make calls, so save our return address
        push r4
        ldi  r4, 0          ; digits on the stack
pd_divide:
        ldi  r1, 10
        call divide         ; r0 = r0 / 10, r1 = r0 % 10
        ldi  r2, '0'
        add  r1, r1, r2     ; digit -> ASCII
        push r1
        addi r4, r4, 1
        cmpi r0, 0
        bne  pd_divide
pd_print:
        pop  r0
        call putc
        addi r4, r4, -1
        bne  pd_print
        pop  r4
        pop  lr
        ret

; divide(r0 = dividend, r1 = divisor) -> r0 = quotient, r1 = remainder.
; Unsigned shift-and-subtract long division, one quotient bit per loop.
; The divisor must be nonzero and below 0x8000.  Clobbers r2, r3.
divide:
        ldi  r2, 0          ; running remainder
        ldi  r3, 16         ; bits left
divide_loop:
        add  r0, r0, r0     ; shift the dividend left; its top bit -> carry
        adc  r2, r2, r2     ; ...and the carry shifts into the remainder
        cmp  r2, r1
        bltu divide_next    ; remainder < divisor: this quotient bit is 0
        sub  r2, r2, r1
        addi r0, r0, 1      ; quotient bit is 1 (the shift left a 0 there)
divide_next:
        addi r3, r3, -1
        bne  divide_loop
        mov  r1, r2
        ret

; ------------------------------------------------------------------ data

banner:      .asciz "Fibonacci numbers that fit in 16 bits:\n"
ticks_label: .asciz "\ntimer interrupts: "
ticks:       .word 0        ; .word aligns itself after the odd-length string