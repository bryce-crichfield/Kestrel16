"""Generates the register file (run from anywhere):

    python Tools/make_registers.py

Chip-level parts, each exactly one 74HC chip so tools/eda/parts_*.yaml can place the real chip: Decoder 3to8 (a
74HC138), Bus Driver 8 (a 74HC541), Register 8 (a 74HC377), Compare 8 (a 74HC688), Nand 8 (a 74HC30) and Register
File 4x4 (a 74HC670). Built from them: Bus Driver 16, Word Register (two Register 8s), Register 16 (a Word Register
plus four Bus Driver 8s) and Register File (R0-R7 with two read ports and one write port, in sixteen 74HC670s).
"""
from hdl import *


def make_decoder():
    with component('Decoder 3to8', '3-to-8 decoder like a 74HC138: /Yn is 0 when A2-A0 = n and E is 1; '
                                   'every other output is 1.'):
        A = bus_input('A', 3)
        E = inputs('E')
        A_not = [~a for a in A]
        bit = lambda n, i: A[i] if n >> i & 1 else A_not[i]
        output('/Y', [nand(bit(n, 0) & bit(n, 1) & bit(n, 2), E) for n in range(8)])


def make_bus_driver_8():
    with component('Bus Driver 8', '8 tri-state buffers: Y = A while /OE is 0, otherwise Y lets go. A 74HC541 '
                                   '(both of its enables are /OE).'):
        A = bus_input('A', 8)
        OE = inputs('/OE')
        output('Y', [tristate(a, OE) for a in A])


def make_bus_driver_16():
    with component('Bus Driver 16', '16 tri-state buffers: Y = A while /OE is 0, otherwise Y lets go. Two Bus '
                                    'Driver 8s (74HC541s).'):
        A = bus_input('A', 16)
        OE = inputs('/OE')
        output('Y', bus(drive(A[:8], OE, 'Low byte'), drive(A[8:], OE, 'High byte')))


def make_register_8():
    with component('Register 8', '8-bit register, a 74HC377: on the rising edge of CLK it loads D if /E is 0 and '
                                 'keeps its value otherwise.'):
        D = bus_input('D', 8)
        E_not, CLK = inputs('/E', 'CLK')
        take = ~E_not
        Q = []
        for d in D:  # next = /E ? Q : D
            ff = flip_flop(CLK)
            mux(take, ff.Q, d, select_not=E_not) >> ff.D
            Q.append(ff.Q)
        output('Q', Q)


def make_compare_8():
    with component('Compare 8', '8-bit equality comparator, a 74HC688: /EQ is 0 when P = Q and /G is 0, '
                                'otherwise 1. Against Q = 0 it is a zero test.'):
        P, Q = bus_input('P', 8), bus_input('Q', 8)
        G = inputs('/G')
        output('/EQ', any_of([G] + [p ^ q for p, q in zip(P, Q)]))


def make_nand_8():
    with component('Nand 8', '8-input NAND, a 74HC30: Y is 0 when every A is 1. Fed NANDs, it is a wide OR '
                             'of ANDs.'):
        A = bus_input('A', 8)
        output('Y', ~all_of(A))


def latch(enable, enable_not, d):
    """A transparent latch from gates: Q follows d while enable is 1 and holds while it is 0. The d & Q term keeps Q
    from glitching as enable falls."""
    q = gate('Or')
    node = int(q.split('.')[0])
    (enable & d) >> Signal('%d.A' % node)
    ((enable_not & q) | (d & q)) >> Signal('%d.B' % node)
    return q


def make_register_file_4x4():
    with component('Register File 4x4',
                   'Four 4-bit words, a 74HC670. Write: while /GW is 0 the word W picks takes D (a latch, not an '
                   'edge: it follows D for as long as /GW stays 0). Read: Q shows the word R picks while /GR is 0, '
                   'otherwise Q lets go; reading the word being written shows D.'):
        D, W, R = bus_input('D', 4), bus_input('W', 2), bus_input('R', 2)
        GW, GR = inputs('/GW', '/GR')
        write = ~GW
        W_not, R_not = [~w for w in W], [~r for r in R]
        pick = lambda n, A, A_not: (A[0] if n & 1 else A_not[0]) & (A[1] if n & 2 else A_not[1])
        words = []
        for n in range(4):
            enable = write & pick(n, W, W_not)
            enable_not = ~enable
            words.append([latch(enable, enable_not, d) for d in D])
        read = [pick(n, R, R_not) for n in range(4)]
        output('Q', [tristate(any_of(read[n] & words[n][bit] for n in range(4)), GR) for bit in range(4)])


def make_word_register():
    with component('Word Register', '16-bit register with no read ports: Q is always its value. On the rising '
                                    'edge of CLK it loads D if /LOAD is 0. Two Register 8s (74HC377s).'):
        D = bus_input('D', 16)
        LOAD_not, CLK = inputs('/LOAD', 'CLK')
        Q = Bus()
        for half, name in ((D[:8], 'Low byte'), (D[8:], 'High byte')):
            register = Part('Register 8', name=name)
            half >> register.D
            LOAD_not >> register['/E']
            CLK >> register.CLK
            Q += register.Q
        output('Q', Q)


def make_register():
    with component('Register 16',
                   '16-bit register. On the rising edge of CLK it loads D if /LOAD is 0 and keeps its value '
                   'otherwise. Two tri-state read ports: QA drives while /OEA is 0, QB while /OEB is 0. '
                   'A Word Register (two 74HC377s) and two Bus Driver 16s (four 74HC541s).'):
        D = bus_input('D', 16)
        LOAD_not, CLK, OEA, OEB = inputs('/LOAD', 'CLK', '/OEA', '/OEB')
        word = Part('Word Register', name='Value')
        D >> word.D
        LOAD_not >> word['/LOAD']
        CLK >> word.CLK
        output('QA', drive(word.Q, OEA, 'Port A'))
        output('QB', drive(word.Q, OEB, 'Port B'))


def make_register_file():
    with component('Register File',
                   'R0-R7, 16 bits each. Two read ports: QA shows the register RA picks, QB the one RB picks, '
                   'all the time. One write port: on the rising edge of CLK, if WE is 1, the register W picks '
                   'takes D, ready by the next rising edge. Sixteen 74HC670s: two copies, A for RA and B for RB, '
                   'each two banks (R0-R3, R4-R7) of four nibbles. A 670 writes while its /GW is low, like a latch, '
                   'so the edge latches D, W and WE into a result register and the write happens from it while '
                   'CLK is high: D stays put while a register being read is written, and nothing loops back.'):
        D, W = bus_input('D', 16), bus_input('W', 3)
        WE, CLK = inputs('WE', 'CLK')
        RA, RB = bus_input('RA', 3), bus_input('RB', 3)

        latched = Bus()
        for start, name in ((0, 'Result low'), (8, 'Result high'), (16, 'Result select')):
            register = Part('Register 8', name=name)
            bus(D, W, WE, [low()] * 4)[start:start + 8] >> register.D
            low() >> register['/E']
            CLK >> register.CLK
            latched += register.Q
        D_q, W_q, WE_q = latched[:16], latched[16:19], latched[19]

        # The write pulse: CLK's high half after the edge that latched the result, which holds still until the
        # next rising edge, so the pulse ends with nothing changing under it. It starts two inverters after the
        # edge, once the result register's new W has reached the 670s' address decoders.
        late_clock = ~~CLK
        strobe = WE_q & late_clock
        bank_write = [nand(strobe, ~W_q[2]), nand(strobe, W_q[2])]  # each bank's /GW

        QA, QB = output('QA', width=16), output('QB', width=16)
        for read, Q, copy in ((RA, QA, 'A'), (RB, QB, 'B')):
            bank_read = [read[2], ~read[2]]  # each bank's /GR
            for bank in range(2):
                for nibble in range(4):
                    chip = Part('Register File 4x4', name='R%d-R%d %s%d' % (4 * bank, 4 * bank + 3, copy, nibble))
                    D_q[4 * nibble:4 * nibble + 4] >> chip.D
                    W_q[:2] >> chip.W
                    bank_write[bank] >> chip['/GW']
                    read[:2] >> chip.R
                    bank_read[bank] >> chip['/GR']
                    chip.Q >> Q[4 * nibble:4 * nibble + 4]

if __name__ == '__main__':
    make_decoder()
    make_bus_driver_8()
    make_bus_driver_16()
    make_register_8()
    make_compare_8()
    make_nand_8()
    make_register_file_4x4()
    make_word_register()
    make_register()
    make_register_file()
    print('wrote Decoder 3to8, Bus Driver 8 and 16, Register 8, Compare 8, Nand 8, Register File 4x4, Word Register, Register 16 and Register File')
