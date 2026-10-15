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
    Kestrel16 CPU           the datapath: register file, ALU, PC, IR, MAR, T, EPC and the buses between them
    Timer               the emulator's timer: an interrupt every N instructions
    Kestrel16 Computer      the CPU, two 32K x 8 RAMs (even and odd bytes), the Terminal and the Timer

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
from circuit import Builder, COMPONENTS
from definitions import Addresses, AluFunctions, Alu2Functions, AluOperations, Conditions, Devices, Opcodes, SystemFunctions

BINARY = '../Binary/Programs/Fibonacci.bin'  # what the computer's RAM starts out holding


# ---------------------------------------------------------------------------- small parts

def make_bus_driver(bits):
    b = Builder('Bus Driver %d' % bits, '%d tri-state buffers: Y = A while /OE is 0, otherwise Y lets go. '
                                        'Like a 74HC541.' % bits)
    a = b.bus_input('A', bits)
    enable = b.input('/OE')
    outputs = b.bus_output('Y', bits, 2)
    for i in range(bits):
        buffer = b.gate('TriState', 1)
        b.wire(a[i], '%d.A' % buffer)
        b.wire(enable, '%d./OE' % buffer)
        b.wire('%d.Y' % buffer, outputs[i])
    b.save()


def make_mux():
    b = Builder('Mux 2x4', 'Four 2-input multiplexers: Yn = S ? Bn : An. Like a 74HC157.')
    a = b.bus_input('A', 4)
    bb = b.bus_input('B', 4)
    select = b.input('S')
    select_not = b.gate1('Not', 1, select)
    outputs = b.bus_output('Y', 4, 5)
    for i in range(4):
        b.wire(b.mux(2, a[i], bb[i], select, select_not), outputs[i])
    b.save()


def make_counter(bits):
    b = Builder('Counter %d' % bits,
                '%d-bit down-counter. On the rising edge of CLK: LOAD = 1 loads D; otherwise DEC = 1 counts '
                'down by one. ZERO is 1 when Q is 0, ONE when Q is 1. Like 74HC193s.' % bits)
    d = b.bus_input('D', bits)
    load, decrement, clock = b.input('LOAD'), b.input('DEC'), b.input('CLK')
    q_outputs = b.bus_output('Q', bits, 8)
    zero_output, one_output = b.output('ZERO', 8), b.output('ONE', 8)
    load_not = b.gate1('Not', 1, load)
    flip_flops = [b.gate('FlipFlop', 5) for _ in range(bits)]
    q = ['%d.Q' % flip_flop for flip_flop in flip_flops]
    borrow = decrement
    for i in range(bits):
        counted = b.gate2('Xor', 2, q[i], borrow)  # a bit flips when every bit below it is 0
        if i + 1 < bits:
            borrow = b.gate2('And', 2, borrow, '%d./Q' % flip_flops[i])
        b.wire(b.mux(3, counted, d[i], load, load_not), '%d.D' % flip_flops[i])
        b.wire(clock, '%d.CLK' % flip_flops[i])
        b.wire(q[i], q_outputs[i])
    upper = b.or_tree(6, q[1:])
    upper_zero = b.gate1('Not', 7, upper)
    b.wire(b.gate2('And', 7, upper_zero, '%d./Q' % flip_flops[0]), zero_output)
    b.wire(b.gate2('And', 7, upper_zero, q[0]), one_output)
    b.save()


def make_program_counter():
    b = Builder('Program Counter',
                'PC: a 16-bit register that can also count. On the rising edge of CLK it loads D if /LOAD is 0, '
                'otherwise adds 2 if INC is 1 (bit 0 stays as it is), otherwise keeps its value. Two tri-state '
                'read ports like Register 16\'s: QA while /OEA is 0, QB while /OEB is 0. In 74HC terms: four '
                '74HC161 counters for bits 1-15 plus a flip-flop for bit 0, and four 74HC541s.')
    d = b.bus_input('D', 16)
    load, increment, clock = b.input('/LOAD'), b.input('INC'), b.input('CLK')
    read_a, read_b = b.input('/OEA'), b.input('/OEB')
    take = b.gate1('Not', 1, load)
    outputs = {port: b.bus_output(port, 16, 6 if port == 'QA' else 7) for port in ('QA', 'QB')}
    carry = increment
    for i in range(16):
        flip_flop = b.gate('FlipFlop', 4)
        q = '%d.Q' % flip_flop
        counted = q
        if i > 0:  # a bit flips when every bit from 1 up to it is 1
            counted = b.gate2('Xor', 2, q, carry)
            carry = b.gate2('And', 2, carry, q)
        b.wire(b.mux(3, counted, d[i], take, load), '%d.D' % flip_flop)
        b.wire(clock, '%d.CLK' % flip_flop)
        for port, enable in (('QA', read_a), ('QB', read_b)):
            buffer = b.gate('TriState', 5)
            b.wire(q, '%d.A' % buffer)
            b.wire(enable, '%d./OE' % buffer)
            b.wire('%d.Y' % buffer, outputs[port][i])
    b.save()


def make_shifter():
    b = Builder('Shifter 16',
                'A barrel shifter: Y = A shifted by N (0-15) while /OE is 0, left when LEFT is 1, otherwise right, '
                'filling with A15 when ARITH is 1 (SAR). C is the last bit shifted out, or CIN when N is 0. A left '
                'shift reverses A, shifts right and reverses back; the right shift is 17 bits wide, with a guard '
                'bit below A0 that catches the bit shifted out. Four stages of Mux 2x4s (74HC157s), one per bit '
                'of N.')
    a = b.bus_input('A', 16)
    amount = b.bus_input('N', 4)
    left, arith, carry_in, enable = b.input('LEFT'), b.input('ARITH'), b.input('CIN'), b.input('/OE')
    outputs = b.bus_output('Y', 16, 8)
    carry_out = b.output('C', 8)
    low = b.gate('Low', 1)

    def muxes(column, a_bits, b_bits, select):
        """select ? b_bits : a_bits, bit by bit, through Mux 2x4s."""
        result = []
        for start in range(0, len(a_bits), 4):
            mux = b.part('Mux 2x4', column, 9)
            for i in range(4):
                index = start + i
                b.wire(a_bits[index] if index < len(a_bits) else low, '%d.A%d' % (mux, i))
                b.wire(b_bits[index] if index < len(b_bits) else low, '%d.B%d' % (mux, i))
            b.wire(select, '%d.S' % mux)
            result += ['%d.Y%d' % (mux, i) for i in range(4)]
        return result[:len(a_bits)]

    ordered = muxes(1, a, a[::-1], left)
    fill = b.gate2('And', 1, arith, a[15])
    bits = [low] + ordered  # bit 0 is the guard
    for stage in range(4):
        distance = 1 << stage
        shifted = [bits[i + distance] if i + distance < 17 else fill for i in range(17)]
        bits = muxes(2 + stage, bits, shifted, amount[stage])
    result = muxes(6, bits[1:], bits[1:][::-1], left)

    driver = b.part('Bus Driver 16', 7, 17)
    for i in range(16):
        b.wire(result[i], '%d.A%d' % (driver, i))
        b.wire('%d.Y%d' % (driver, i), outputs[i])
    b.wire(enable, '%d./OE' % driver)
    b.wire(b.mux(7, carry_in, bits[0], b.or_tree(6, amount)), carry_out)
    b.save()


# ---------------------------------------------------------------------------- immediate unit

IMMEDIATES = ['IMM6', 'IMM8', 'LHI', 'OFF8', 'OFF12', 'TWO', 'ZERO']


def make_immediate_unit():
    b = Builder('Immediate Unit',
                'Puts one constant from the instruction onto Y (while /OE is 0), picked by SEL: 0 imm6 sign-'
                'extended, 1 imm8 zero-extended, 2 LHI\'s value (imm8 in the high byte, OLD0-7 in the low), '
                '3 offset8 x 2 sign-extended, 4 offset12 x 2 sign-extended, 5 the constant 2 (instruction size '
                'and interrupt vector), 6 zero. All wiring: each choice is a Bus Driver 16 whose inputs are '
                'instruction bits, OLD bits (the destination register\'s old value, for LHI) or Low/High.')
    ir = b.bus_input('IR', 12)
    old = b.bus_input('OLD', 8)
    select = b.bus_input('SEL', 3)
    enable_not = b.input('/OE')
    outputs = b.bus_output('Y', 16, 5)
    low, high = b.gate('Low', 1), b.gate('High', 1)
    decoder = b.part('Decoder 3to8', 2, 8)
    for i in range(3):
        b.wire(select[i], '%d.A%d' % (decoder, i))
    b.wire(b.gate1('Not', 1, enable_not), '%d.E' % decoder)

    def bits(kind):
        if kind == 'IMM6':
            return [ir[min(i, 5)] for i in range(16)]
        if kind == 'IMM8':
            return ir[:8] + [low] * 8
        if kind == 'LHI':
            return old + ir[:8]
        if kind == 'OFF8':
            return [low] + [ir[min(i, 7)] for i in range(15)]
        if kind == 'OFF12':
            return [low] + [ir[min(i, 11)] for i in range(15)]
        if kind == 'TWO':
            return [low, high] + [low] * 14
        return [low] * 16

    for index, kind in enumerate(IMMEDIATES):
        driver = b.part('Bus Driver 16', 3 + index % 2, 17)
        for i, source in enumerate(bits(kind)):
            b.wire(source, '%d.A%d' % (driver, i))
        b.wire('%d./Y%d' % (decoder, index), '%d./OE' % driver)
        for i in range(16):
            b.wire('%d.Y%d' % (driver, i), outputs[i])
    b.save()


# ---------------------------------------------------------------------------- status

def make_status():
    b = Builder('Status',
                'The flags and the interrupt enable, all loaded on the rising edge of CLK. WZN, WC and WV store '
                'the ALU\'s flags (MOV, LDI and loads store none). SAVE copies the flags and IE into the '
                'exception copies (on an interrupt or TRAP); RESTORE copies them back (RETI). IESET and IECLR '
                'are EI and DI.')
    flags_in = [b.input(name + 'IN') for name in 'ZNCV']
    write_zn, write_c, write_v = b.input('WZN'), b.input('WC'), b.input('WV')
    restore, save, ie_set, ie_clear, clock = (b.input(name) for name in ('RESTORE', 'SAVE', 'IESET', 'IECLR', 'CLK'))
    outputs = [b.output(name, 9) for name in 'ZNCV'] + [b.output('IE', 9)]

    flag_ff = [b.gate('FlipFlop', 6) for _ in range(5)]  # Z N C V IE
    saved_ff = [b.gate('FlipFlop', 8) for _ in range(5)]
    q = ['%d.Q' % ff for ff in flag_ff]
    saved = ['%d.Q' % ff for ff in saved_ff]
    restore_not, save_not = b.gate1('Not', 1, restore), b.gate1('Not', 1, save)
    for i, write in enumerate([write_zn, write_zn, write_c, write_v]):
        source = b.mux(2, flags_in[i], saved[i], restore, restore_not)
        load = b.gate2('Or', 2, write, restore)
        b.wire(b.mux(4, q[i], source, load), '%d.D' % flag_ff[i])
    # IE: set by EI, cleared by DI (and on entering an exception), restored by RETI.
    kept = b.gate2('And', 3, q[4], b.gate1('Not', 2, b.gate2('Or', 1, ie_clear, restore)))
    b.wire(b.or_tree(4, [ie_set, kept, b.gate2('And', 3, restore, saved[4])]), '%d.D' % flag_ff[4])
    for i in range(5):
        b.wire(b.mux(7, saved[i], q[i], save, save_not), '%d.D' % saved_ff[i])
        b.wire(clock, '%d.CLK' % flag_ff[i])
        b.wire(clock, '%d.CLK' % saved_ff[i])
        b.wire(q[i], outputs[i])
    b.save()


# ---------------------------------------------------------------------------- control unit

# Control signals, in ROM data-bit order, with their idle values. Names starting with / are
# active-low enables and loads.
CONTROL_A = [('/XRF', 0), ('/XPC', 1), ('/XEPC', 1), ('/XT', 1), ('/XSH', 1), ('CSH', 0), ('/YRF', 0), ('/YIMM', 1), ('/APC', 0),
             ('/AMAR', 1), ('/WALU', 1), ('MLOAD', 0), ('/LDPC', 1), ('/LDMAR', 1), ('/LDEPC', 1), ('/LDT', 1),
             ('/LDIR', 1), ('PCINC', 0), ('RFWE', 0), ('WSEL7', 0), ('RASYS', 0), ('RBDEST', 0), ('OP0', 0), ('OP1', 0),
             ('OP2', 0), ('OP3', 0), ('CIN1', 0), ('CINC', 0), ('IMM0', 0), ('IMM1', 0), ('IMM2', 0)]
CONTROL_B = [('WZN', 0), ('WC', 0), ('WV', 0), ('RESTORE', 0), ('SAVE', 0), ('IESET', 0), ('IECLR', 0),
             ('ACK', 0), ('MREAD', 0), ('STORE', 0), ('WSTROBE', 0), ('WORD', 0), ('TICK', 0), ('HALTED', 0), ('ILLEGAL', 0), ('N0', 0), ('N1', 0), ('N2', 0), ('N3', 0), ('N4', 0)]
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
        return Step('INT2').x('PC').alu('PASSA').on('/LDEPC', 'SAVE', 'IECLR', 'ACK')
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
    b = Builder('Control Unit',
                'The microcode sequencer: a 5-bit state register and two 32K-word ROMs, Control A.bin (31 '
                'bits) and Control B.bin (20 bits), both made by Tools/make_cpu.py. The ROMs see the state, '
                'the opcode, the function (IR2-0, or IR11-8 for SYS), TAKE (the branch condition holds) and '
                'IRQ; they give every control signal plus the next state, '
                'which loads on the rising edge of CLK. Power-up state 0 is RESET.')
    ir = {i: b.input('IR%d' % i) for i in list(range(3)) + list(range(8, 16))}
    take, irq, clock = b.input('TAKE'), b.input('IRQ'), b.input('CLK')
    low = b.gate('Low', 1)

    # The function field: IR2-0 for ALU and ALU2, IR11-8 for SYS (opcode 12 = 1100).
    assert Opcodes['System'] == 0b1100, 'the SYS decode below is wired for opcode 1100'
    is_sys = b.gate2('And', 2, b.gate2('And', 1, ir[15], ir[14]), b.gate2('Nor', 1, ir[13], ir[12]))
    function = b.part('Mux 2x4', 2, 9)
    for i in range(4):
        b.wire(ir[i] if i < 3 else low, '%d.A%d' % (function, i))
        b.wire(ir[8 + i], '%d.B%d' % (function, i))
    b.wire(is_sys, '%d.S' % function)

    state = [b.gate('FlipFlop', 3) for _ in range(5)]
    roms = [b.rom(4, CONTROL_ADDRESS, names_a, image='Control A.bin'),
            b.rom(4, CONTROL_ADDRESS, names_b + ['N0', 'N1', 'N2', 'N3', 'N4'], image='Control B.bin')]
    for rom in roms:
        sources = (['%d.Q' % ff for ff in state] + [ir[12 + i] for i in range(4)] +
                   ['%d.Y%d' % (function, i) for i in range(4)] + [take, irq])
        for name, source in zip(CONTROL_ADDRESS, sources):
            b.wire(source, '%d.%s' % (rom, name))
        b.wire(low, '%d./CE' % rom)
        b.wire(low, '%d./OE' % rom)
    for i in range(5):
        b.wire('%d.N%d' % (roms[1], i), '%d.D' % state[i])
        b.wire(clock, '%d.CLK' % state[i])
    for name in names_a:
        b.wire('%d.%s' % (roms[0], name), b.output(name, 6))
    for name in names_b:
        b.wire('%d.%s' % (roms[1], name), b.output(name, 7))
    b.save()


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
    b = Builder('Kestrel16 CPU',
                'The Kestrel16 processor. Memory interface: A0-15 address, DO0-15 data out (driven only while '
                'storing), DI0-15 data in, /OE read strobe, /WEL and /WEH write strobes for the even (low) and '
                'odd (high) byte, low only while CLK is low; /WRL is /WEL for the whole write cycle, for parts that '
                'latch on the clock (the Timer). TIRQ is the timer\'s interrupt request, ACK says it was taken, TICK marks '
                'each instruction. HALTED: HALT ran (waiting for an interrupt); ILLEGAL: stopped on an illegal '
                'instruction.')
    clock = b.input('CLK')
    di = b.bus_input('DI', 16)
    timer_irq = b.input('TIRQ')
    a_out = b.bus_output('A', 16, 14)
    do_out = b.bus_output('DO', 16, 15)
    strobes = {name: b.output(name, 16) for name in ('/OE', '/WEL', '/WEH', '/WRL', 'ACK', 'TICK', 'HALTED', 'ILLEGAL')}
    low, high = b.gate('Low', 1), b.gate('High', 1)

    control = b.part('Control Unit', 2, 50)
    c = lambda name: '%d.%s' % (control, name)

    def register(column):
        node = b.part('Register 16', column, 36)
        b.wire(clock, '%d.CLK' % node)
        return node

    ir, mar, t, epc = (register(3) for _ in range(4))
    pc = b.part('Program Counter', 3, 36)
    b.wire(clock, '%d.CLK' % pc)
    b.wire(c('PCINC'), '%d.INC' % pc)
    for i in range(16):
        b.wire(di[i], '%d.D%d' % (ir, i))
    b.wire(c('/LDIR'), '%d./LOAD' % ir)
    b.wire(low, '%d./OEA' % ir)
    b.wire(high, '%d./OEB' % ir)
    ir_bit = ['%d.QA%d' % (ir, i) for i in range(16)]

    alu = b.part('ALU 16', 6, 40)
    for node, load in ((pc, '/LDPC'), (mar, '/LDMAR'), (t, '/LDT'), (epc, '/LDEPC')):
        for i in range(16):
            b.wire('%d.Y%d' % (alu, i), '%d.D%d' % (node, i))
        b.wire(c(load), '%d./LOAD' % node)
    # X bus: register file, PC, EPC or T. Y bus: register file or immediate. Address bus: PC or MAR.
    b.wire(c('/XPC'), '%d./OEA' % pc)
    b.wire(c('/APC'), '%d./OEB' % pc)
    b.wire(c('/AMAR'), '%d./OEA' % mar)
    b.wire(high, '%d./OEB' % mar)
    b.wire(c('/XT'), '%d./OEA' % t)
    b.wire(high, '%d./OEB' % t)
    b.wire(c('/XEPC'), '%d./OEA' % epc)
    b.wire(high, '%d./OEB' % epc)
    x_bus = ['%d.A%d' % (alu, i) for i in range(16)]
    y_bus = ['%d.B%d' % (alu, i) for i in range(16)]
    address = ['%d.QA%d' % (mar, i) for i in range(16)]  # the address bus, as a net MAR and PC both drive
    for i in range(16):
        b.wire('%d.QA%d' % (pc, i), x_bus[i])
        b.wire('%d.QA%d' % (t, i), x_bus[i])
        b.wire('%d.QA%d' % (epc, i), x_bus[i])
        b.wire('%d.QB%d' % (pc, i), a_out[i])
        b.wire(address[i], a_out[i])

    # Register file and its address multiplexers.
    registers = b.part('Register File', 5, 40)
    b.wire(clock, '%d.CLK' % registers)
    b.wire(c('RFWE'), '%d.WE' % registers)
    muxes = {}
    for name, a_bits, b_bits, select in (('RA', (6, 7, 8), (5, 6, 7), 'RASYS'), ('RB', (3, 4, 5), (9, 10, 11), 'RBDEST'),
                                         ('W', (9, 10, 11), None, 'WSEL7')):
        mux = b.part('Mux 2x4', 4, 9)
        for i in range(3):
            b.wire(ir_bit[a_bits[i]], '%d.A%d' % (mux, i))
            b.wire(ir_bit[b_bits[i]] if b_bits else high, '%d.B%d' % (mux, i))
            b.wire('%d.Y%d' % (mux, i), '%d.%s%d' % (registers, name, i))
        b.wire(low, '%d.A3' % mux)
        b.wire(low, '%d.B3' % mux)
        b.wire(c(select), '%d.S' % mux)
        muxes[name] = mux
    qa = ['%d.QA%d' % (registers, i) for i in range(16)]
    qb = ['%d.QB%d' % (registers, i) for i in range(16)]
    drivers = {}
    for bus, port, enable in ((x_bus, qa, '/XRF'), (y_bus, qb, '/YRF')):
        driver = drivers[enable] = b.part('Bus Driver 16', 5, 17)
        for i in range(16):
            b.wire(port[i], '%d.A%d' % (driver, i))
            b.wire('%d.Y%d' % (driver, i), bus[i])
        b.wire(c(enable), '%d./OE' % driver)

    immediate = b.part('Immediate Unit', 5, 23)
    for i in range(12):
        b.wire(ir_bit[i], '%d.IR%d' % (immediate, i))
    for i in range(8):
        b.wire(qb[i], '%d.OLD%d' % (immediate, i))
    for i in range(3):
        b.wire(c('IMM%d' % i), '%d.SEL%d' % (immediate, i))
    b.wire(c('/YIMM'), '%d./OE' % immediate)
    for i in range(16):
        b.wire('%d.Y%d' % (immediate, i), y_bus[i])

    # ALU control, flags and the branch condition.
    status = b.part('Status', 7, 15)
    for i in range(4):
        b.wire(c('OP%d' % i), '%d.OP%d' % (alu, i))
    carry_in = b.gate2('Or', 7, c('CIN1'), b.gate2('And', 7, c('CINC'), '%d.C' % status))
    b.wire(carry_in, '%d.Cin' % alu)
    shifter = b.part('Shifter 16', 6, 40)
    for i in range(16):
        b.wire(qa[i], '%d.A%d' % (shifter, i))
        b.wire('%d.Y%d' % (shifter, i), x_bus[i])
    for i in range(4):
        b.wire(qb[i], '%d.N%d' % (shifter, i))
    b.wire(b.gate1('Not', 5, ir_bit[1]), '%d.LEFT' % shifter)  # SHL 101, SHR 110, SAR 111
    b.wire(b.gate2('And', 5, ir_bit[1], ir_bit[0]), '%d.ARITH' % shifter)
    b.wire('%d.C' % status, '%d.CIN' % shifter)
    b.wire(c('/XSH'), '%d./OE' % shifter)
    for flag in 'ZNV':
        b.wire('%d.%s' % (alu, flag), '%d.%sIN' % (status, flag))
    b.wire(b.mux(7, '%d.C' % alu, '%d.C' % shifter, c('CSH')), '%d.CIN' % status)
    for name in ('WZN', 'WC', 'WV', 'RESTORE', 'SAVE', 'IESET', 'IECLR'):
        b.wire(c(name), '%d.%s' % (status, name))
    b.wire(clock, '%d.CLK' % status)
    condition = b.rom(8, ['C0', 'C1', 'C2', 'C3', 'Z', 'N', 'C', 'V'], ['TAKE'], rows=condition_rows())
    for i in range(4):
        b.wire(ir_bit[8 + i], '%d.C%d' % (condition, i))
    for flag in 'ZNCV':
        b.wire('%d.%s' % (status, flag), '%d.%s' % (condition, flag))
    b.wire(low, '%d./CE' % condition)
    b.wire(low, '%d./OE' % condition)

    # Control unit inputs.
    for i in list(range(3)) + list(range(8, 16)):
        b.wire(ir_bit[i], c('IR%d' % i))
    b.wire('%d.TAKE' % condition, c('TAKE'))
    b.wire(b.gate2('And', 8, timer_irq, '%d.IE' % status), c('IRQ'))
    b.wire(clock, c('CLK'))

    # Byte lanes. A word uses both; a byte at an even address the low (even) lane, at an odd one the high.
    word, a0 = c('WORD'), address[0]
    even_ok = b.gate2('Or', 9, word, b.gate1('Not', 8, a0))  # the low lane takes part
    odd_ok = b.gate2('Or', 9, word, a0)
    byte_odd = b.gate2('And', 9, b.gate1('Not', 8, word), a0)
    byte = b.gate1('Not', 8, word)

    write_back = ['%d.D%d' % (registers, i) for i in range(16)]
    driver = b.part('Bus Driver 16', 10, 17)  # register file <- ALU
    for i in range(16):
        b.wire('%d.Y%d' % (alu, i), '%d.A%d' % (driver, i))
        b.wire('%d.Y%d' % (driver, i), write_back[i])
    b.wire(c('/WALU'), '%d./OE' % driver)

    def lane(column, sources, targets, enable_a, enable_b, purpose):
        node = b.part('Bus Driver 8', column, 9)
        for i in range(8):
            b.wire(sources[i], '%d.A%d' % (node, i))
            b.wire('%d.Y%d' % (node, i), targets[i])
        b.wire(b.gate2('Nand', column - 1, enable_a, enable_b), '%d./OE' % node)

    # Loads: register file <- the addressed byte(s), zero-extended for LB.
    lane(11, di[:8], write_back[:8], c('MLOAD'), even_ok, 'low from low')
    lane(11, di[8:], write_back[:8], c('MLOAD'), byte_odd, 'low from high')
    lane(11, di[8:], write_back[8:], c('MLOAD'), word, 'high from high')
    lane(11, [low] * 8, write_back[8:], c('MLOAD'), byte, 'high is zero')
    # Stores: data out <- the register (port B), its low byte on the odd lane for SB to an odd address.
    lane(13, qb[:8], do_out[:8], c('STORE'), even_ok, 'low to low')
    lane(13, qb[8:], do_out[8:], c('STORE'), word, 'high to high')
    lane(13, qb[:8], do_out[8:], c('STORE'), byte_odd, 'low to high')

    b.wire(b.gate1('Not', 15, c('MREAD')), strobes['/OE'])
    clock_low = b.gate1('Not', 14, clock)
    b.wire(b.gate2('Nand', 15, c('WSTROBE'), even_ok), strobes['/WRL'])
    b.wire(b.gate2('Nand', 15, b.gate2('And', 14, c('WSTROBE'), clock_low), even_ok), strobes['/WEL'])
    b.wire(b.gate2('Nand', 15, b.gate2('And', 14, c('WSTROBE'), clock_low), odd_ok), strobes['/WEH'])
    for name in ('ACK', 'TICK', 'HALTED', 'ILLEGAL'):
        b.wire(c(name), strobes[name])
    b.save()


# ---------------------------------------------------------------------------- timer and computer

def make_timer():
    b = Builder('Timer',
                'The emulator\'s timer at 0x%04X. Writing N (/CE and /WE low at a rising CLK edge) starts it: '
                'every Nth TICK (one per instruction) it requests an interrupt on IRQ until ACK; N = 0 stops '
                'it. Reading (/CE and /OE low) gives N back on Q.' % Devices['Timer'][0])
    d = b.bus_input('D', 16)
    chip_enable, write_enable, output_enable = b.input('/CE'), b.input('/WE'), b.input('/OE')
    tick, ack, clock = b.input('TICK'), b.input('ACK'), b.input('CLK')
    q_out = b.bus_output('Q', 16, 9)
    irq_out = b.output('IRQ', 9)

    write_not = b.gate2('Or', 1, chip_enable, write_enable)
    write = b.gate1('Not', 2, write_not)
    reload = b.part('Register 16', 3, 36)
    for i in range(16):
        b.wire(d[i], '%d.D%d' % (reload, i))
        b.wire('%d.QB%d' % (reload, i), q_out[i])
    b.wire(write_not, '%d./LOAD' % reload)
    b.wire(b.gate2('Or', 2, chip_enable, output_enable), '%d./OEB' % reload)
    b.wire(b.gate('Low', 2), '%d./OEA' % reload)
    b.wire(clock, '%d.CLK' % reload)
    period = ['%d.QA%d' % (reload, i) for i in range(16)]

    counter = b.part('Counter 16', 5, 20)
    for source, enable in ((d, write_not), (period, write)):  # count <- D on a write, else the period
        driver = b.part('Bus Driver 16', 4, 17)
        for i in range(16):
            b.wire(source[i], '%d.A%d' % (driver, i))
            b.wire('%d.Y%d' % (driver, i), '%d.D%d' % (counter, i))
        b.wire(enable, '%d./OE' % driver)
    running = b.or_tree(4, period)
    fire_now = b.gate2('And', 6, running, '%d.ONE' % counter)  # this tick is the Nth
    ticking = b.gate2('And', 6, tick, running)
    fires = b.gate2('And', 7, tick, fire_now)
    b.wire(b.gate2('Or', 7, write, fires), '%d.LOAD' % counter)
    b.wire(ticking, '%d.DEC' % counter)
    b.wire(clock, '%d.CLK' % counter)

    pending = b.gate('FlipFlop', 8)
    held = b.gate2('Or', 7, '%d.Q' % pending, fires)
    b.wire(b.gate2('And', 8, held, b.gate1('Not', 7, ack)), '%d.D' % pending)
    b.wire(clock, '%d.CLK' % pending)
    b.wire(b.gate2('Or', 8, '%d.Q' % pending, fire_now), irq_out)
    b.save()


def decode_image():
    """Address decode: A2-A15 -> /RAM (below I/O), /TERMINAL (the UART), /TIMER; 1 = not selected."""
    within = lambda address, device: Devices[device][0] <= address < Devices[device][0] + Devices[device][1]
    image = bytearray()
    for index in range(1 << 14):
        address = index << 2
        ram = address < Addresses['IoBase']
        terminal, timer = within(address, 'Uart'), within(address, 'Timer')
        image.append(int(not ram) | int(not terminal) << 1 | int(not timer) << 2)
    with open(os.path.join(COMPONENTS, 'Address Decode.bin'), 'wb') as file:
        file.write(image)


def make_computer():
    b = Builder('Kestrel16 Computer',
                'The whole machine: Kestrel16 CPU, 64K of RAM as two 32K x 8 chips (even and odd bytes, preloaded '
                'from %s), the Terminal at 0x%04X and the Timer at 0x%04X, picked by an address-decode ROM '
                '(Address Decode.bin). Simulate it and run the clock; the program prints in the Terminal pane. '
                'To run another program, change the two RAMs\' image.' % (BINARY, Devices['Uart'][0], Devices['Timer'][0]))
    clock = b.gate('Clock', 0)
    cpu = b.part('Kestrel16 CPU', 1, 60)
    halted, illegal = b.output('HALTED', 5), b.output('ILLEGAL', 5)
    b.wire(clock, '%d.CLK' % cpu)

    decode = b.rom(2, ['A%d' % i for i in range(2, 16)], ['/RAM', '/TERMINAL', '/TIMER'], image='Address Decode.bin')
    for i in range(2, 16):
        b.wire('%d.A%d' % (cpu, i), '%d.A%d' % (decode, i))
    low = b.gate('Low', 2)
    b.wire(low, '%d./CE' % decode)
    b.wire(low, '%d./OE' % decode)

    rams = [b.ram(3, 15, BINARY, 'even'), b.ram(3, 15, BINARY, 'odd')]
    for lane, ram in enumerate(rams):
        for i in range(15):
            b.wire('%d.A%d' % (cpu, i + 1), '%d.A%d' % (ram, i))
        for i in range(8):
            b.wire('%d.DO%d' % (cpu, 8 * lane + i), '%d.D%d' % (ram, i))  # the data bus: one net per bit
            b.wire('%d.D%d' % (ram, i), '%d.DI%d' % (cpu, 8 * lane + i))
        b.wire('%d./RAM' % decode, '%d./CE' % ram)
        b.wire('%d./OE' % cpu, '%d./OE' % ram)
        b.wire('%d.%s' % (cpu, '/WEH' if lane else '/WEL'), '%d./WE' % ram)

    terminal = b._add('kind: Terminal', 4, 400)
    b.wire('%d.A1' % cpu, '%d.RS' % terminal)
    b.wire('%d./TERMINAL' % decode, '%d./CE' % terminal)
    b.wire('%d./OE' % cpu, '%d./OE' % terminal)
    b.wire('%d./WEL' % cpu, '%d./WE' % terminal)
    for i in range(8):
        b.wire('%d.DO%d' % (cpu, i), '%d.D%d' % (terminal, i))

    timer = b.part('Timer', 4, 22)
    for i in range(16):
        b.wire('%d.DO%d' % (cpu, i), '%d.D%d' % (timer, i))
        b.wire('%d.Q%d' % (timer, i), '%d.DI%d' % (cpu, i))
    b.wire('%d./TIMER' % decode, '%d./CE' % timer)
    b.wire('%d./WRL' % cpu, '%d./WE' % timer)
    b.wire('%d./OE' % cpu, '%d./OE' % timer)
    for name in ('TICK', 'ACK'):
        b.wire('%d.%s' % (cpu, name), '%d.%s' % (timer, name))
    b.wire(clock, '%d.CLK' % timer)
    b.wire('%d.IRQ' % timer, '%d.TIRQ' % cpu)
    b.wire('%d.HALTED' % cpu, halted)
    b.wire('%d.ILLEGAL' % cpu, illegal)
    b.save()


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
    decode_image()
    make_computer()
    print('wrote the CPU and the computer')
