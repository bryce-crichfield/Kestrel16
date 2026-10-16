"""Generates the Kestrel16 CPU and the computer around it (run from anywhere, after make_alu.py and
make_registers.py, whose components it uses):

    python Tools/make_cpu.py

Parts (each its own component, most of them a familiar 74HC chip or two):
    Bus Driver 8 / 16   tri-state buffers with one /OE (74HC541)
    Mux 2x4             four 2-input multiplexers with one select (74HC157)
    Counter 4 / 16      loadable down-counters (74HC193)
    Program Counter     PC: a register that also counts up by 2 (74HC161s), so fetching needs no ALU
    Shifter 16          a barrel shifter: SHL, SHR or SAR by 0-15 in one pass, from Mux 2x4s
    Immediate Unit      the instruction's constants, one at a time onto the Y bus
    Status              the flags Z N C V, the interrupt enable, and their copies saved on an exception
    Control Unit        a 5-bit state register and two microcode ROMs (Control A.bin, Control B.bin)
    Kestrel16 CPU       the datapath: register file, ALU, PC, IR, MAR, T, EPC and the buses between them
    Timer               an interrupt every N instructions, until acknowledged
    Interrupt Controller  the slots' interrupt requests, pending and mask registers, one INT to the CPU
    System Slot         slot 0 on the main board: the Interrupt Controller and the Timer
    Terminal Card       the UART as an I/O backplane card
    Clock Stretcher     holds the clock back while a card pulls READY low (wait states)
    Kestrel16 Computer  the main board (CPU, two 32K x 8 RAMs, clock, address decode, System Slot) and the
                        I/O backplane: slots 1-7 on a 32-pin connector, the Terminal Card in slot 1

I/O is memory mapped: 0xFC00-0xFFFF is 8 slots of 128 bytes (Source/Kestrel/MemoryMap.h), and the
slot connector's signals are listed above make_timer() below.

The CPU is multi-cycle. An instruction is fetched (IR <- memory[PC], PC += 2 in the Program Counter)
and then takes one to three more cycles. Fetching overlaps execution: when an instruction's last
cycle leaves the memory bus and PC alone (ALU ops, immediates, shifts, compares, branches not taken)
that cycle also fetches the next instruction, so those instructions take one cycle. Loads, stores,
jumps, taken branches and EI/DI/RETI/TRAP are followed by a FETCH cycle of their own. Shifts go
through the barrel shifter in one cycle.
A store's write cycle drives the address and data while CLK is high and pulses /WE while it is low,
so /WE ends at the rising edge, before the address can change.
"""
import os
from hdl import *
from circuit import COMPONENTS
from definitions import Addresses, AluFunctions, Alu2Functions, AluOperations, Conditions, IoBus, Opcodes, Slots, SystemFunctions

BINARY = '../../Binary/Programs/Fibonacci.bin'  # what the computer's RAM starts out holding


# ---------------------------------------------------------------------------- small parts

def make_bus_driver(bits):
    with component('Bus Driver %d' % bits, '%d tri-state buffers: Y = A while /OE is 0, otherwise Y lets go. '
                                           'Like a 74HC541.' % bits):
        A = bus_input('A', bits)
        OE = inputs('/OE')
        output('Y', [tristate(a, OE) for a in A])


def make_mux():
    with component('Mux 2x4', 'Four 2-input multiplexers: Yn = S ? Bn : An. Like a 74HC157.'):
        A, B = bus_input('A', 4), bus_input('B', 4)
        S = inputs('S')
        S_not = ~S
        output('Y', [mux(S, a, b, S_not) for a, b in zip(A, B)])


def make_counter(bits):
    with component('Counter %d' % bits,
                   '%d-bit down-counter. On the rising edge of CLK: LOAD = 1 loads D; otherwise DEC = 1 counts '
                   'down by one. ZERO is 1 when Q is 0, ONE when Q is 1. Like 74HC193s.' % bits):
        D = bus_input('D', bits)
        LOAD, DEC, CLK = inputs('LOAD', 'DEC', 'CLK')
        LOAD_not = ~LOAD
        ffs = [flip_flop(CLK) for _ in range(bits)]
        borrow = DEC
        for i, ff in enumerate(ffs):
            counted = ff.Q ^ borrow  # a bit flips when every bit below it is 0
            if i + 1 < bits:
                borrow = borrow & ff['/Q']
            mux(LOAD, counted, D[i], LOAD_not) >> ff.D
        Q = bus(ff.Q for ff in ffs)
        upper_zero = ~any_of(Q[1:])
        output('Q', Q)
        output('ZERO', upper_zero & ffs[0]['/Q'])
        output('ONE', upper_zero & Q[0])


def make_program_counter():
    with component('Program Counter',
                   'PC: a 16-bit register that can also count. On the rising edge of CLK it loads D if /LOAD is 0, '
                   'otherwise adds 2 if INC is 1 (bit 0 stays as it is), otherwise keeps its value. Two tri-state '
                   'read ports like Register 16\'s: QA while /OEA is 0, QB while /OEB is 0. In 74HC terms: four '
                   '74HC161 counters for bits 1-15 plus a flip-flop for bit 0, and four 74HC541s.'):
        D = bus_input('D', 16)
        LOAD_not, INC, CLK, OEA, OEB = inputs('/LOAD', 'INC', 'CLK', '/OEA', '/OEB')
        take = ~LOAD_not
        carry, Q = INC, Bus()
        for i, d in enumerate(D):
            ff = flip_flop(CLK)
            counted = ff.Q
            if i > 0:  # a bit flips when every bit from 1 up to it is 1
                counted, carry = ff.Q ^ carry, carry & ff.Q
            mux(take, counted, d, LOAD_not) >> ff.D
            Q.append(ff.Q)
        output('QA', [tristate(q, OEA) for q in Q])
        output('QB', [tristate(q, OEB) for q in Q])


def make_shifter():
    with component('Shifter 16',
                   'A barrel shifter: Y = A shifted by N (0-15) while /OE is 0, left when LEFT is 1, otherwise right, '
                   'filling with A15 when ARITH is 1 (SAR). C is the last bit shifted out, or CIN when N is 0. A left '
                   'shift reverses A, shifts right and reverses back; the right shift is 17 bits wide, with a guard '
                   'bit below A0 that catches the bit shifted out. Four stages of Mux 2x4s (74HC157s), one per bit '
                   'of N.'):
        A, N = bus_input('A', 16), bus_input('N', 4)
        LEFT, ARITH, CIN, OE = inputs('LEFT', 'ARITH', 'CIN', '/OE')
        bits = bus(low(), mux_bus(LEFT, A, A.reversed()))  # bit 0 is the guard
        fill = ARITH & A[15]
        for stage, select in enumerate(N):  # shift right by 1, 2, 4 and 8
            distance = 1 << stage
            bits = mux_bus(select, bits, bits[distance:] + [fill] * distance)
        result = mux_bus(LEFT, bits[1:], bits[1:].reversed())
        output('Y', drive(result, OE))
        output('C', mux(any_of(N), CIN, bits[0]))


# ---------------------------------------------------------------------------- immediate unit

IMMEDIATES = ['IMM6', 'IMM8', 'LHI', 'OFF8', 'OFF12', 'TWO', 'ZERO']


def make_immediate_unit():
    with component('Immediate Unit',
                   'Puts one constant from the instruction onto Y (while /OE is 0), picked by SEL: 0 imm6 sign-'
                   'extended, 1 imm8 zero-extended, 2 LHI\'s value (imm8 in the high byte, OLD0-7 in the low), '
                   '3 offset8 x 2 sign-extended, 4 offset12 x 2 sign-extended, 5 the constant 2 (instruction size '
                   'and interrupt vector), 6 zero. All wiring: each choice is a Bus Driver 16 whose inputs are '
                   'instruction bits, OLD bits (the destination register\'s old value, for LHI) or Low/High.'):
        IR, OLD, SEL = bus_input('IR', 12), bus_input('OLD', 8), bus_input('SEL', 3)
        OE = inputs('/OE')
        decoder = Part('Decoder 3to8')
        SEL >> decoder.A
        ~OE >> decoder.E

        sign_extend = lambda bits, width: bits + [bits[-1]] * (width - len(bits))
        choices = {'IMM6': sign_extend(IR[:6], 16),
                   'IMM8': bus(IR[:8], [low()] * 8),
                   'LHI': bus(OLD, IR[:8]),
                   'OFF8': bus(low(), sign_extend(IR[:8], 15)),
                   'OFF12': bus(low(), sign_extend(IR, 15)),
                   'TWO': bus(low(), high(), [low()] * 14),
                   'ZERO': [low()] * 16}
        Y = output('Y', width=16)
        for index, kind in enumerate(IMMEDIATES):
            drive(choices[kind], decoder['/Y'][index]) >> Y


# ---------------------------------------------------------------------------- status

def make_status():
    with component('Status',
                   'The flags and the interrupt enable, all loaded on the rising edge of CLK. WZN, WC and WV store '
                   'the ALU\'s flags (MOV, LDI and loads store none). SAVE copies the flags and IE into the '
                   'exception copies (on an interrupt or TRAP); RESTORE copies them back (RETI). IESET and IECLR '
                   'are EI and DI.'):
        flags_in = inputs('ZIN', 'NIN', 'CIN', 'VIN')
        WZN, WC, WV = inputs('WZN', 'WC', 'WV')
        RESTORE, SAVE, IESET, IECLR, CLK = inputs('RESTORE', 'SAVE', 'IESET', 'IECLR', 'CLK')
        flags = [flip_flop(CLK) for _ in range(5)]  # Z N C V IE
        saved = [flip_flop(CLK) for _ in range(5)]  # their exception copies
        RESTORE_not, SAVE_not = ~RESTORE, ~SAVE
        for i, write in enumerate([WZN, WZN, WC, WV]):
            source = mux(RESTORE, flags_in[i], saved[i].Q, RESTORE_not)
            mux(write | RESTORE, flags[i].Q, source) >> flags[i].D
        # IE: set by EI, cleared by DI (and on entering an exception), restored by RETI.
        kept = flags[4].Q & ~(IECLR | RESTORE)
        any_of([IESET, kept, RESTORE & saved[4].Q]) >> flags[4].D
        for flag, copy in zip(flags, saved):
            mux(SAVE, copy.Q, flag.Q, SAVE_not) >> copy.D
        for name, flag in zip(['Z', 'N', 'C', 'V', 'IE'], flags):
            output(name, flag.Q)


# ---------------------------------------------------------------------------- control unit

# Control signals, in ROM data-bit order, with their idle values. Names starting with / are
# active-low enables and loads.
CONTROL_A = [('/XRF', 0), ('/XPC', 1), ('/XEPC', 1), ('/XT', 1), ('/XSH', 1), ('CSH', 0), ('/YRF', 0), ('/YIMM', 1), ('/APC', 0),
             ('/AMAR', 1), ('/WALU', 1), ('MLOAD', 0), ('/LDPC', 1), ('/LDMAR', 1), ('/LDEPC', 1), ('/LDT', 1),
             ('/LDIR', 1), ('PCINC', 0), ('RFWE', 0), ('WSEL7', 0), ('RASYS', 0), ('RBDEST', 0), ('OP0', 0), ('OP1', 0),
             ('OP2', 0), ('OP3', 0), ('CIN1', 0), ('CINC', 0), ('IMM0', 0), ('IMM1', 0), ('IMM2', 0)]
CONTROL_B = [('WZN', 0), ('WC', 0), ('WV', 0), ('RESTORE', 0), ('SAVE', 0), ('IESET', 0), ('IECLR', 0),
             ('MREAD', 0), ('STORE', 0), ('WSTROBE', 0), ('WORD', 0), ('TICK', 0), ('HALTED', 0), ('ILLEGAL', 0), ('N0', 0), ('N1', 0), ('N2', 0), ('N3', 0), ('N4', 0)]
CONTROL_ADDRESS = ['S0', 'S1', 'S2', 'S3', 'S4', 'OP0', 'OP1', 'OP2', 'OP3', 'F0', 'F1', 'F2', 'F3', 'TAKE', 'IRQ']

STATES = ['RESET', 'FETCH', 'FETCH2', 'E1', 'E2', 'E3', 'E4', 'INT1', 'INT2', 'HALT', 'ILLEGAL']
S = {name: index for index, name in enumerate(STATES)}
ALU = {name.upper(): code for name, code in AluOperations.items()}  # ADD, SUB, ..., PASSA, PASSB, SXB
ALL_FLAGS = ('WZN', 'WC', 'WV')

# The instruction set (Source/Kestrel/Instructions.h), by name.
Op, Alu2, Sys = Opcodes, Alu2Functions, SystemFunctions
# The ALU operation each ALU-opcode function uses.
AluOperationOf = {AluFunctions[name]: operation for name, operation in (
    ('Add', 'ADD'), ('Subtract', 'SUB'), ('And', 'AND'), ('Or', 'OR'), ('Xor', 'XOR'),
    ('ShiftLeft', 'SHL'), ('ShiftRightLogical', 'SHR'), ('ShiftRightArithmetic', 'SAR'))}
Shifts = {AluFunctions[name] for name in ('ShiftLeft', 'ShiftRightLogical', 'ShiftRightArithmetic')}


class Step:
    """One cycle's control signals, built up from idle values."""

    def __init__(self, next_state):
        self.signals = dict(CONTROL_A + CONTROL_B)
        self.next = S[next_state]

    def on(self, *names):
        for name in names:
            self.signals[name] = 0 if name.startswith('/') else 1
        return self

    def x(self, source):  # the ALU's A input: RF, PC, EPC, T or SH (the shifter)
        self.signals['/XRF'] = 1
        return self.on('/X' + source)

    def y_imm(self, kind):  # the ALU's B input from the immediate unit instead of the register file
        self.signals['/YRF'] = 1
        self.on('/YIMM')
        for bit in range(3):
            self.signals['IMM%d' % bit] = IMMEDIATES.index(kind) >> bit & 1
        return self

    def alu(self, operation, carry_in=0):  # carry_in: 0, 1 or 'C' (the C flag)
        code = ALU[operation]
        for bit in range(4):
            self.signals['OP%d' % bit] = code >> bit & 1
        if carry_in == 1:
            self.on('CIN1')
        elif carry_in == 'C':
            self.on('CINC')
        return self

    def write_register(self, link=False):  # register file <- ALU
        self.on('/WALU', 'RFWE')
        return self.on('WSEL7') if link else self

    def address_mar(self):  # memory address from MAR instead of PC
        self.signals['/APC'] = 1
        return self.on('/AMAR')

    def word(self):
        bits = [self.signals[name] for name, _ in CONTROL_A]
        bits_b = [self.signals[name] for name, _ in CONTROL_B[:-5]] + [self.next >> bit & 1 for bit in range(5)]
        return sum(bit << i for i, bit in enumerate(bits)), sum(bit << i for i, bit in enumerate(bits_b))


def fetch(tick, step=None):
    """IR <- memory[PC], PC += 2 (in the Program Counter), on top of step's signals if given."""
    step = (step or Step('E1')).on('MREAD', 'WORD', '/LDIR', 'PCINC')
    step.next = S['E1']
    return step.on('TICK') if tick else step


# A last cycle can fetch the next instruction only if it leaves the memory bus and PC alone and
# doesn't change IE (an interrupt check right after EI, DI or RETI has to see the new IE).
BUSY = {'/LDPC': 0, '/AMAR': 0, 'MREAD': 1, 'STORE': 1, 'WSTROBE': 1, 'IESET': 1, 'IECLR': 1, 'RESTORE': 1,
        'SAVE': 1, 'HALTED': 1}


def overlap(step, irq):
    """step, an instruction's last cycle, also doing the next FETCH (or going for an interrupt)."""
    if step.next != S['FETCH'] or any(step.signals[name] == value for name, value in BUSY.items()):
        return step
    if irq:  # what FETCH would do: tick, then the interrupt
        step.next = S['INT1']
        return step.on('TICK')
    return fetch(True, step)


def execute(state, opcode, function, take):
    """The cycle of an instruction after FETCH, in state E1..E4."""
    illegal = Step('ILLEGAL')

    # how many E states the instruction uses
    twoStates = (Op['LoadWord'], Op['StoreWord'], Op['LoadByte'], Op['StoreByte'], Op['JumpAndLink'])
    states = 2 if opcode in twoStates else 1
    if opcode == Op['System']:
        states = {Sys['JumpAndLinkRegister']: 3, Sys['Trap']: 2}.get(function, 1)
    if state not in ('E1', 'E2', 'E3', 'E4')[:states]:
        return illegal

    if opcode == Op['Alu']:
        if function not in AluOperationOf:  # F3 is always 0 here; these ROM words are never used
            return illegal
        if function in Shifts:  # shifts: the shifter's result through the ALU, its carry to C
            return Step('FETCH').x('SH').alu('PASSA').write_register().on('WZN', 'WC', 'CSH')
        operation = AluOperationOf[function]
        flags = ('WZN',) if operation in ('AND', 'OR', 'XOR') else ALL_FLAGS
        carry = 1 if function == AluFunctions['Subtract'] else 0
        return Step('FETCH').x('RF').alu(operation, carry).write_register().on(*flags)
    if opcode == Op['Alu2']:
        if function == Alu2['AddWithCarry']:
            return Step('FETCH').x('RF').alu('ADD', 'C').write_register().on(*ALL_FLAGS)
        if function == Alu2['SubtractWithCarry']:
            return Step('FETCH').x('RF').alu('SUB', 'C').write_register().on(*ALL_FLAGS)
        if function == Alu2['Compare']:
            return Step('FETCH').x('RF').alu('SUB', 1).on(*ALL_FLAGS)
        if function == Alu2['Move']:
            return Step('FETCH').x('RF').alu('PASSA').write_register()
        if function == Alu2['Not']:
            return Step('FETCH').x('RF').alu('NOT').write_register().on('WZN')
        if function == Alu2['Negate']:
            return Step('FETCH').x('RF').alu('NEG', 1).write_register().on(*ALL_FLAGS)
        if function == Alu2['SignExtendByte']:
            return Step('FETCH').x('RF').alu('SXB').write_register().on('WZN')
        return illegal
    if opcode == Op['AddImmediate']:
        return Step('FETCH').x('RF').y_imm('IMM6').alu('ADD').write_register().on(*ALL_FLAGS)
    if opcode == Op['LoadImmediate']:
        return Step('FETCH').y_imm('IMM8').alu('PASSB').write_register()
    if opcode == Op['LoadHighImmediate']:
        return Step('FETCH').on('RBDEST').y_imm('LHI').alu('PASSB').write_register()
    if opcode in (Op['LoadWord'], Op['LoadByte']):  # MAR <- a + imm6, then register <- memory
        if state == 'E1':
            return Step('E2').x('RF').y_imm('IMM6').alu('ADD').on('/LDMAR')
        step = Step('FETCH').address_mar().on('MREAD', 'MLOAD', 'RFWE')
        return step.on('WORD') if opcode == Op['LoadWord'] else step
    if opcode in (Op['StoreWord'], Op['StoreByte']):  # MAR <- a + imm6, then a write cycle (/WE pulses while CLK is low)
        if state == 'E1':
            return Step('E2').x('RF').y_imm('IMM6').alu('ADD').on('/LDMAR')
        step = Step('FETCH').address_mar().on('RBDEST', 'STORE', 'WSTROBE')
        return step.on('WORD') if opcode == Op['StoreWord'] else step
    if opcode == Op['Branch']:
        return Step('FETCH').x('PC').y_imm('OFF8').alu('ADD').on('/LDPC') if take else Step('FETCH')
    if opcode == Op['Jump']:
        return Step('FETCH').x('PC').y_imm('OFF12').alu('ADD').on('/LDPC')
    if opcode == Op['JumpAndLink']:  # R7 <- PC, then PC <- PC + offset
        if state == 'E1':
            return Step('E2').x('PC').alu('PASSA').write_register(link=True)
        return Step('FETCH').x('PC').y_imm('OFF12').alu('ADD').on('/LDPC')
    if opcode == Op['System']:
        if function == Sys['JumpRegister']:
            return Step('FETCH').on('RASYS').x('RF').alu('PASSA').on('/LDPC')
        if function == Sys['JumpAndLinkRegister']:  # T <- register, R7 <- PC, PC <- T
            if state == 'E1':
                return Step('E2').on('RASYS').x('RF').alu('PASSA').on('/LDT')
            if state == 'E2':
                return Step('E3').x('PC').alu('PASSA').write_register(link=True)
            return Step('FETCH').x('T').alu('PASSA').on('/LDPC')
        if function == Sys['ReturnFromInterrupt']:
            return Step('FETCH').x('EPC').alu('PASSA').on('/LDPC', 'RESTORE')
        if function == Sys['EnableInterrupts']:
            return Step('FETCH').on('IESET')
        if function == Sys['DisableInterrupts']:
            return Step('FETCH').on('IECLR')
        if function == Sys['Halt']:
            return Step('HALT')
        if function == Sys['Trap']:  # like an interrupt, returning to the next instruction
            if state == 'E1':
                return Step('E2').x('PC').alu('PASSA').on('/LDEPC', 'SAVE', 'IECLR')
            return Step('FETCH').y_imm('TWO').alu('PASSB').on('/LDPC')
        return illegal
    if opcode == Op['CompareImmediate']:
        return Step('FETCH').x('RF').y_imm('IMM6').alu('SUB', 1).on(*ALL_FLAGS)
    return illegal


def control(state, opcode, function, take, irq):
    name = STATES[state] if state < len(STATES) else 'ILLEGAL'
    if name == 'RESET':
        return Step('FETCH').y_imm('ZERO').alu('PASSB').on('/LDPC')
    if name == 'FETCH':  # each instruction ticks the timer once; an interrupt goes in before it
        return Step('INT1').on('TICK') if irq else fetch(tick=True)
    if name == 'FETCH2':  # the first instruction of the handler: its tick already happened
        return fetch(tick=False)
    if name == 'INT1':
        return Step('INT2').x('PC').alu('PASSA').on('/LDEPC', 'SAVE', 'IECLR')
    if name == 'INT2':
        return Step('FETCH2').y_imm('TWO').alu('PASSB').on('/LDPC')
    if name == 'HALT':  # sleeps, ticking, until an interrupt
        return Step('INT1').on('TICK', 'HALTED') if irq else Step('HALT').on('TICK', 'HALTED')
    if name == 'ILLEGAL':
        return Step('ILLEGAL').on('ILLEGAL')
    return overlap(execute(name, opcode, function, take), irq)


def make_control_images():
    image_a, image_b = bytearray(), bytearray()
    for address in range(1 << len(CONTROL_ADDRESS)):
        state, opcode, function = address & 31, (address >> 5) & 15, (address >> 9) & 15
        take, irq = (address >> 13) & 1, (address >> 14) & 1
        word_a, word_b = control(state, opcode, function, take, irq).word()
        image_a += word_a.to_bytes(4, 'little')
        image_b += word_b.to_bytes(3, 'little')
    for name, image in (('Control A.bin', image_a), ('Control B.bin', image_b)):
        with open(os.path.join(COMPONENTS, name), 'wb') as file:
            file.write(image)


def make_control_unit():
    names_a = [name for name, _ in CONTROL_A]
    names_b = [name for name, _ in CONTROL_B][:-5]
    with component('Control Unit',
                   'The microcode sequencer: a 5-bit state register and two 32K-word ROMs, Control A.bin (31 '
                   'bits) and Control B.bin (19 bits), both made by Tools/make_cpu.py. The ROMs see the state, '
                   'the opcode, the function (IR2-0, or IR11-8 for SYS), TAKE (the branch condition holds) and '
                   'IRQ; they give every control signal plus the next state, '
                   'which loads on the rising edge of CLK. Power-up state 0 is RESET.'):
        IR = {i: inputs('IR%d' % i) for i in list(range(3)) + list(range(8, 16))}
        TAKE, IRQ, CLK = inputs('TAKE', 'IRQ', 'CLK')

        # The function field: IR2-0 for ALU and ALU2, IR11-8 for SYS (opcode 12 = 1100).
        assert Opcodes['System'] == 0b1100, 'the SYS decode below is wired for opcode 1100'
        is_sys = (IR[15] & IR[14]) & nor(IR[13], IR[12])
        function = mux_bus(is_sys, [IR[0], IR[1], IR[2]], [IR[8], IR[9], IR[10], IR[11]])

        state = [flip_flop(CLK) for _ in range(5)]
        address = bus([ff.Q for ff in state], [IR[12], IR[13], IR[14], IR[15]], function, TAKE, IRQ)
        rom_a = rom(CONTROL_ADDRESS, names_a, image='Control A.bin')
        rom_b = rom(CONTROL_ADDRESS, names_b + ['N0', 'N1', 'N2', 'N3', 'N4'], image='Control B.bin')
        for chip in (rom_a, rom_b):
            address >> [chip[name] for name in CONTROL_ADDRESS]
        rom_b.N >> [ff.D for ff in state]
        for name in names_a:
            output(name, rom_a[name])
        for name in names_b:
            output(name, rom_b[name])


# ---------------------------------------------------------------------------- CPU

def condition_rows():
    """The condition ROM: address C0-3, Z, N, C, V; data TAKE (0 for a condition that doesn't exist)."""
    name = {number: name for name, number in Conditions.items()}
    words = []
    for address in range(256):
        condition, z, n, c, v = address & 15, address >> 4 & 1, address >> 5 & 1, address >> 6 & 1, address >> 7 & 1
        holds = {'Always': 1, 'Equal': z, 'NotEqual': not z, 'LessUnsigned': not c, 'GreaterOrEqualUnsigned': c,
                 'Less': n != v, 'GreaterOrEqual': n == v, 'Greater': not z and n == v, 'LessOrEqual': z or n != v,
                 'Minus': n, 'Plus': not n, 'OverflowSet': v, 'OverflowClear': not v}
        words.append(int(holds[name[condition]]) if condition in name else 0)
    return ['%04X: %s' % (start, ' '.join('%X' % w for w in words[start:start + 16])) for start in range(0, 256, 16)]


def make_cpu():
    with component('Kestrel16 CPU',
                   'The Kestrel16 processor. Memory interface: A0-15 address, DO0-15 data out (driven only while '
                   'storing), DI0-15 data in, /OE read strobe, /WEL and /WEH write strobes for the even (low) and '
                   'odd (high) byte, low only while CLK is low; /WRL is /WEL for the whole write cycle, for parts that '
                   'latch on the clock (the I/O write strobe). IRQ is the interrupt controller\'s request, TICK marks '
                   'each instruction. HALTED: HALT ran (waiting for an interrupt); ILLEGAL: stopped on an illegal '
                   'instruction.'):
        CLK = inputs('CLK')
        DI = bus_input('DI', 16)
        IRQ = inputs('IRQ')
        c = Part('Control Unit')

        def register():
            part = Part('Register 16')
            CLK >> part.CLK
            return part

        ir, mar, t, epc = (register() for _ in range(4))
        pc = Part('Program Counter')
        CLK >> pc.CLK
        c.PCINC >> pc.INC
        DI >> ir.D
        c['/LDIR'] >> ir['/LOAD']
        low() >> ir['/OEA']
        high() >> ir['/OEB']
        IR = ir.QA

        alu = Part('ALU 16')
        for part, load in ((pc, '/LDPC'), (mar, '/LDMAR'), (t, '/LDT'), (epc, '/LDEPC')):
            alu.Y >> part.D
            c[load] >> part['/LOAD']
        # X bus (the ALU's A): register file, PC, EPC, T or the shifter. Y bus (its B): register file or
        # immediate. Address bus: PC or MAR. MAR, T and EPC don't use their second read port.
        X, Y = alu.A, alu.B
        c['/XPC'] >> pc['/OEA']
        c['/APC'] >> pc['/OEB']
        c['/AMAR'] >> mar['/OEA']
        c['/XT'] >> t['/OEA']
        c['/XEPC'] >> epc['/OEA']
        high() >> [mar['/OEB'], t['/OEB'], epc['/OEB']]
        for part in (pc, t, epc):
            part.QA >> X
        address = mar.QA  # the address bus, as a net MAR and PC both drive

        # Register file and its address multiplexers.
        registers = Part('Register File')
        CLK >> registers.CLK
        c.RFWE >> registers.WE
        mux_bus(c.RASYS, IR[6:9], IR[5:8]) >> registers.RA
        mux_bus(c.RBDEST, IR[3:6], IR[9:12]) >> registers.RB
        mux_bus(c.WSEL7, IR[9:12], [high()] * 3) >> registers.W  # WSEL7 picks R7, the link register
        drive(registers.QA, c['/XRF']) >> X
        drive(registers.QB, c['/YRF']) >> Y

        immediate = Part('Immediate Unit')
        IR[:12] >> immediate.IR
        registers.QB[:8] >> immediate.OLD
        bus(c.IMM0, c.IMM1, c.IMM2) >> immediate.SEL
        c['/YIMM'] >> immediate['/OE']
        immediate.Y >> Y

        # ALU control, flags and the branch condition.
        status = Part('Status')
        bus(c.OP0, c.OP1, c.OP2, c.OP3) >> alu.OP
        (c.CIN1 | (c.CINC & status.C)) >> alu.Cin
        shifter = Part('Shifter 16')
        registers.QA >> shifter.A
        shifter.Y >> X
        registers.QB[:4] >> shifter.N
        ~IR[1] >> shifter.LEFT  # SHL 101, SHR 110, SAR 111
        (IR[1] & IR[0]) >> shifter.ARITH
        status.C >> shifter.CIN
        c['/XSH'] >> shifter['/OE']
        for flag in 'ZNV':
            alu[flag] >> status[flag + 'IN']
        mux(c.CSH, alu.C, shifter.C) >> status.CIN
        for name in ('WZN', 'WC', 'WV', 'RESTORE', 'SAVE', 'IESET', 'IECLR'):
            c[name] >> status[name]
        CLK >> status.CLK
        condition = rom(['C0', 'C1', 'C2', 'C3', 'Z', 'N', 'C', 'V'], ['TAKE'], rows=condition_rows())
        IR[8:12] >> [condition['C%d' % i] for i in range(4)]
        for flag in 'ZNCV':
            status[flag] >> condition[flag]

        # Control unit inputs.
        for i in list(range(3)) + list(range(8, 16)):
            IR[i] >> c['IR%d' % i]
        condition.TAKE >> c.TAKE
        (IRQ & status.IE) >> c.IRQ
        CLK >> c.CLK

        # Byte lanes. A word uses both; a byte at an even address the low (even) lane, at an odd one the high.
        word, a0 = c.WORD, address[0]
        even_ok = word | ~a0  # the low lane takes part
        odd_ok = word | a0
        byte_odd = ~word & a0
        byte = ~word

        write_back = registers.D
        drive(alu.Y, c['/WALU']) >> write_back  # register file <- ALU

        def lane(sources, targets, enable_a, enable_b):
            drive(sources, nand(enable_a, enable_b)) >> targets

        # Loads: register file <- the addressed byte(s), zero-extended for LB.
        lane(DI[:8], write_back[:8], c.MLOAD, even_ok)  # low from low
        lane(DI[8:], write_back[:8], c.MLOAD, byte_odd)  # low from high
        lane(DI[8:], write_back[8:], c.MLOAD, word)  # high from high
        lane([low()] * 8, write_back[8:], c.MLOAD, byte)  # high is zero

        A = output('A', pc.QB)
        address >> A
        # Stores: data out <- the register (port B), its low byte on the odd lane for SB to an odd address.
        DO = output('DO', width=16)
        lane(registers.QB[:8], DO[:8], c.STORE, even_ok)  # low to low
        lane(registers.QB[8:], DO[8:], c.STORE, word)  # high to high
        lane(registers.QB[:8], DO[8:], c.STORE, byte_odd)  # low to high

        clock_low = ~CLK
        output('/OE', ~c.MREAD)
        output('/WEL', nand(c.WSTROBE & clock_low, even_ok))
        output('/WEH', nand(c.WSTROBE & clock_low, odd_ok))
        output('/WRL', nand(c.WSTROBE, even_ok))
        for name in ('TICK', 'HALTED', 'ILLEGAL'):
            output(name, c[name])


# ---------------------------------------------------------------------------- I/O

# The I/O backplane's slot connector, 32 pins. Every card sees the same signals except /CS and IRQ:
#   D0-D15        data, both ways. I/O is word-wide: there are no byte enables, a byte write to an
#                 even address writes the register with the high byte undriven, and one to an odd
#                 address doesn't strobe /WR at all.
#   A0-A6         the address within the slot (128 bytes)
#   /CS           this slot is addressed (from the main board's slot decoder)
#   /RD           read: the card drives D while /CS and /RD are low
#   /WR           write, low for the whole write cycle: a clocked card latches D at the rising CLK
#                 edge that ends it; an asynchronous one gates it with CLK low (/WR | CLK), so its
#                 strobe ends before the address can change
#   IRQ           the card requests an interrupt, as a level, until its driver clears it at the card
#   READY         the card holds it low to stretch the cycle (the main board holds back CLK)
#   /RESET        reset (driven high: the machine has no reset yet)
#   CLK           the CPU's (stretched) clock
#   VCC, GND
SLOT_ADDRESS_BITS = IoBus['SlotSize'].bit_length() - 1
SLOT_PINS = 16 + SLOT_ADDRESS_BITS + 7 + 2
assert SLOT_PINS == 32 and Addresses['IoBase'] + IoBus['SlotSize'] * IoBus['SlotCount'] == 0x10000


def selected(CS, A, used):
    """1 when /CS is low and the address bits above the card's registers (A<used>-A6) are 0, so a
    card's registers appear once in its slot, as the emulator has them, rather than repeating."""
    return ~(CS | any_of(A[used:]))


def make_timer():
    with component('Timer',
                   'The timer, on the main board in the system slot. Writing N (/CE and /WE low at a rising CLK '
                   'edge) starts it: every Nth TICK (one per instruction) it requests an interrupt on IRQ until ACK '
                   'is 1 at a rising CLK edge; N = 0 stops it. Reading (/CE and /OE low) gives N back on Q.'):
        D = bus_input('D', 16)
        CE, WE, OE = inputs('/CE', '/WE', '/OE')
        TICK, ACK, CLK = inputs('TICK', 'ACK', 'CLK')
        write_not = CE | WE
        write = ~write_not

        reload = Part('Register 16')  # N, the period
        D >> reload.D
        write_not >> reload['/LOAD']
        (CE | OE) >> reload['/OEB']
        low() >> reload['/OEA']
        CLK >> reload.CLK
        period = reload.QA

        counter = Part('Counter 16')
        drive(D, write_not) >> counter.D  # count <- D on a write, otherwise the period
        drive(period, write) >> counter.D
        running = any_of(period)
        fire_now = running & counter.ONE  # this tick is the Nth
        fires = TICK & fire_now
        (write | fires) >> counter.LOAD
        (TICK & running) >> counter.DEC
        CLK >> counter.CLK

        pending = flip_flop(CLK)
        ((pending.Q | fires) & ~ACK) >> pending.D
        output('Q', reload.QB)
        output('IRQ', pending.Q | fire_now)


def make_interrupt_controller():
    with component('Interrupt Controller',
                   'Gathers the slots\' interrupt requests: IRQn is slot n\'s. INT is 1 while any request is also '
                   'enabled in the mask. Two registers, picked by RS: 0 PENDING (read: the IRQ lines as they are), '
                   '1 MASK (read and write; bit n lets slot n interrupt). The mask loads from D0-D7 at a rising CLK '
                   'edge with /CE and /WE low, and is 0 at power-up. Reading (/CE and /OE low) drives Q0-Q7.'):
        D, IRQ = bus_input('D', 8), bus_input('IRQ', 8)
        CE, WE, OE, RS, CLK = inputs('/CE', '/WE', '/OE', 'RS', 'CLK')
        load = ~(CE | WE) & RS
        mask = []
        for d in D:
            ff = flip_flop(CLK)
            mux(load, ff.Q, d) >> ff.D
            mask.append(ff.Q)
        read = ~(CE | OE)
        Q = output('Q', width=8)
        drive(IRQ, nand(read, ~RS)) >> Q
        drive(mask, nand(read, RS)) >> Q
        output('INT', any_of([irq & m for irq, m in zip(IRQ, mask)]))


def make_system_slot():
    with component('System Slot',
                   'Slot 0 (0x%04X), on the main board: the Interrupt Controller and the Timer behind the slot '
                   'interface. Registers by A1-A2: 0 interrupt pending, 1 interrupt mask, 2 timer reload, 3 timer '
                   'acknowledge (a write drops the timer\'s request). IRQ1-7 are the backplane slots\' requests; '
                   'the timer is interrupt 0 (IRQ0 is unused). INT goes to the CPU. TICK comes from the CPU, one '
                   'per instruction.' % Addresses['IoBase']):
        D, A = bus_input('D', 16), bus_input('A', SLOT_ADDRESS_BITS)
        CS, RD, WR, CLK, TICK = inputs('/CS', '/RD', '/WR', 'CLK', 'TICK')
        IRQ = bus_input('IRQ', IoBus['SlotCount'])
        here = selected(CS, A, 3)
        Q = output('Q', width=16)

        timer = Part('Timer')
        D >> timer.D
        nand(here, A[2] & ~A[1]) >> timer['/CE']
        WR >> timer['/WE']
        RD >> timer['/OE']
        TICK >> timer.TICK
        (here & (A[2] & A[1]) & ~WR) >> timer.ACK
        CLK >> timer.CLK
        timer.Q >> Q

        controller = Part('Interrupt Controller')
        D[:8] >> controller.D
        bus(timer.IRQ, IRQ[1:]) >> controller.IRQ
        nand(here, ~A[2]) >> controller['/CE']
        WR >> controller['/WE']
        RD >> controller['/OE']
        A[1] >> controller.RS
        CLK >> controller.CLK
        controller.Q >> Q[:8]
        output('INT', controller.INT)


def make_terminal_card():
    with component('Terminal Card',
                   'The UART as a backplane card (slot %d in the computer): the Terminal behind the slot interface. '
                   'Registers by A1: 0 data, 1 status. IRQ is 1 while a typed key is waiting. It never needs a '
                   'wait, so READY is 1.' % Slots['Uart']):
        D, A = bus_input('D', 16), bus_input('A', SLOT_ADDRESS_BITS)
        CS, RD, WR, CLK = inputs('/CS', '/RD', '/WR', 'CLK')
        terminal = primitive('Terminal')
        A[1] >> terminal.RS
        ~selected(CS, A, 2) >> terminal['/CE']
        RD >> terminal['/OE']
        (WR | CLK) >> terminal['/WE']  # the write strobe ends at the rising edge, before the address moves
        D[:8] >> terminal.D  # a Terminal's D pins are one wire both ways, so this is its read path too
        output('IRQ', terminal.IRQ)
        output('READY', high())


def make_clock_stretcher():
    with component('Clock Stretcher',
                   'Wait states for the I/O backplane. Y is CLK, except that a rising edge is held back while READY '
                   'was 0 at the falling edge before it: a slow card pulls READY low during the cycle, and Y stays '
                   'low (the CPU and the cards wait, mid-cycle) until READY goes back to 1.'):
        CLK, READY = inputs('CLK', 'READY')
        hold = flip_flop(~CLK)  # changes only while CLK is low, so Y never glitches
        ~READY >> hold.D
        output('Y', CLK & hold['/Q'])


def decode_image():
    """Address decode: A10-A15 -> /RAM (below IoBase) and /IO (from it up); 1 = not selected."""
    image = bytearray()
    for index in range(1 << 6):
        io = index << 10 >= Addresses['IoBase']
        image.append(int(io) | int(not io) << 1)
    with open(os.path.join(COMPONENTS, 'Address Decode.bin'), 'wb') as file:
        file.write(image)


def make_computer():
    slot_base = lambda slot: Addresses['IoBase'] + slot * IoBus['SlotSize']
    with component('Kestrel16 Computer',
                   'The whole machine. Main board: the Kestrel16 CPU, 64K of RAM as two 32K x 8 chips (even and odd '
                   'bytes, preloaded from %s), the clock (through the Clock Stretcher, for wait states), the address '
                   'decode (a ROM, Address Decode.bin, picks RAM or I/O; a Decoder 3to8 on A7-A9 picks the slot) and '
                   'the System Slot at 0x%04X. The I/O backplane: slots 1-7, 128 bytes each, with the Terminal Card '
                   'in slot %d (0x%04X). Simulate it and run the clock; the program prints in the Terminal pane. To '
                   'run another program, change the two RAMs\' image.'
                   % (BINARY, slot_base(0), Slots['Uart'], slot_base(Slots['Uart']))):
        CLK = clock_source()
        stretcher = Part('Clock Stretcher')
        CLK >> stretcher.CLK
        clock = stretcher.Y  # what the CPU and the cards run on
        cpu = Part('Kestrel16 CPU')
        clock >> cpu.CLK

        decode = rom(['A%d' % i for i in range(10, 16)], ['/RAM', '/IO'], image='Address Decode.bin')
        cpu.A[10:] >> [decode['A%d' % i] for i in range(10, 16)]
        slot_decoder = Part('Decoder 3to8')
        cpu.A[SLOT_ADDRESS_BITS:SLOT_ADDRESS_BITS + 3] >> slot_decoder.A
        ~decode['/IO'] >> slot_decoder.E
        CS = slot_decoder['/Y']

        for lane in range(2):  # even bytes, then odd
            chip = ram(15, BINARY, ('even', 'odd')[lane])
            cpu.A[1:] >> chip.A
            cpu.DO[8 * lane:8 * lane + 8] >> chip.D  # the data bus: one net per bit
            chip.D >> cpu.DI[8 * lane:8 * lane + 8]
            decode['/RAM'] >> chip['/CE']
            cpu['/OE'] >> chip['/OE']
            cpu['/WEH' if lane else '/WEL'] >> chip['/WE']

        def slot(card, number):
            """The signals every card gets: D, A0-A6, /CS, /RD, /WR (/WRL: words and even bytes), CLK."""
            cpu.DO >> card.D
            cpu.A[:SLOT_ADDRESS_BITS] >> card.A
            CS[number] >> card['/CS']
            cpu['/OE'] >> card['/RD']
            cpu['/WRL'] >> card['/WR']
            clock >> card.CLK

        system = Part('System Slot')
        slot(system, Slots['System'])
        system.Q >> cpu.DI
        cpu.TICK >> system.TICK
        system.INT >> cpu.IRQ

        irq = [low()] * IoBus['SlotCount']
        terminal = Part('Terminal Card')
        slot(terminal, Slots['Uart'])
        irq[Slots['Uart']] = terminal.IRQ
        bus(irq) >> system.IRQ
        terminal.READY >> stretcher.READY  # with more cards: the AND of their READYs

        output('HALTED', cpu.HALTED)
        output('ILLEGAL', cpu.ILLEGAL)


if __name__ == '__main__':
    make_bus_driver(8)
    make_bus_driver(16)
    make_mux()
    make_counter(4)
    make_counter(16)
    make_shifter()
    make_program_counter()
    make_immediate_unit()
    make_status()
    make_control_images()
    make_control_unit()
    make_cpu()
    make_timer()
    make_interrupt_controller()
    make_system_slot()
    make_terminal_card()
    make_clock_stretcher()
    decode_image()
    make_computer()
    print('wrote the CPU and the computer')
