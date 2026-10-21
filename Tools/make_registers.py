"""Generates the register file (run from anywhere):

    python Tools/make_registers.py

Chip-level parts, each exactly one 74HC chip so tools/eda/parts.yaml can place the real chip: Decoder 3to8 (a
74HC138), Bus Driver 8 (a 74HC541), Register 8 (a 74HC377), Compare 8 (a 74HC688) and Nand 8 (a 74HC30). Built from them: Bus Driver 16, Word Register (two
Register 8s), Register 16 (a Word Register plus four Bus Driver 8s) and Register File (eight Register 16s: R0-R7 with two read ports and one
write port).
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
                   'loads D.'):
        D, W = bus_input('D', 16), bus_input('W', 3)
        WE, CLK = inputs('WE', 'CLK')
        RA, RB = bus_input('RA', 3), bus_input('RB', 3)

        def decoder(address, enable, name):
            chip = Part('Decoder 3to8', name=name)
            address >> chip.A
            enable >> chip.E
            return chip['/Y']

        write, read_a, read_b = decoder(W, WE, 'Write select'), decoder(RA, high(), 'Read A select'), \
            decoder(RB, high(), 'Read B select')
        QA, QB = output('QA', width=16), output('QB', width=16)
        for r in range(8):
            register = Part('Register 16', name='R%d' % r)
            D >> register.D
            register.QA >> QA
            register.QB >> QB
            write[r] >> register['/LOAD']
            read_a[r] >> register['/OEA']
            read_b[r] >> register['/OEB']
            CLK >> register.CLK


if __name__ == '__main__':
    make_decoder()
    make_bus_driver_8()
    make_bus_driver_16()
    make_register_8()
    make_compare_8()
    make_nand_8()
    make_word_register()
    make_register()
    make_register_file()
    print('wrote Decoder 3to8, Bus Driver 8 and 16, Register 8, Compare 8, Nand 8, Word Register, Register 16 and Register File')
