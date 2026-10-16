"""Generates the register file (run from anywhere):

    python Tools/make_registers.py

Decoder 3to8 (a 74HC138), Register 16 (two 74HC377s plus four 74HC541s) and Register File
(eight Register 16s: R0-R7 with two read ports and one write port).
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


def make_register():
    with component('Register 16',
                   '16-bit register. On the rising edge of CLK it loads D if /LOAD is 0 and keeps its value '
                   'otherwise. Two tri-state read ports: QA drives while /OEA is 0, QB while /OEB is 0. '
                   'In 74HC terms: two 74HC377s and four 74HC541s.'):
        D = bus_input('D', 16)
        LOAD_not, CLK, OEA, OEB = inputs('/LOAD', 'CLK', '/OEA', '/OEB')
        take = ~LOAD_not
        Q = []
        for d in D:  # next = /LOAD ? Q : D
            ff = flip_flop(CLK)
            mux(take, ff.Q, d, select_not=LOAD_not) >> ff.D
            Q.append(ff.Q)
        output('QA', [tristate(q, OEA) for q in Q])
        output('QB', [tristate(q, OEB) for q in Q])


def make_register_file():
    with component('Register File',
                   'R0-R7, 16 bits each. Two read ports: QA shows the register RA picks, QB the one RB picks, '
                   'all the time. One write port: on the rising edge of CLK, if WE is 1, the register W picks '
                   'loads D.'):
        D, W = bus_input('D', 16), bus_input('W', 3)
        WE, CLK = inputs('WE', 'CLK')
        RA, RB = bus_input('RA', 3), bus_input('RB', 3)

        def decoder(address, enable):
            chip = Part('Decoder 3to8')
            address >> chip.A
            enable >> chip.E
            return chip['/Y']

        write, read_a, read_b = decoder(W, WE), decoder(RA, high()), decoder(RB, high())
        QA, QB = output('QA', width=16), output('QB', width=16)
        for r in range(8):
            register = Part('Register 16')
            D >> register.D
            register.QA >> QA
            register.QB >> QB
            write[r] >> register['/LOAD']
            read_a[r] >> register['/OEA']
            read_b[r] >> register['/OEB']
            CLK >> register.CLK


if __name__ == '__main__':
    make_decoder()
    make_register()
    make_register_file()
    print('wrote Decoder 3to8, Register 16 and Register File')
