"""Generates the register file (run from anywhere):

    python Tools/make_registers.py

Decoder 3to8 (a 74HC138), Register 16 (two 74HC377s plus four 74HC541s) and Register File
(eight Register 16s: R0-R7 with two read ports and one write port).
"""
from circuit import Builder


def make_decoder():
    b = Builder('Decoder 3to8', '3-to-8 decoder like a 74HC138: /Yn is 0 when A2-A0 = n and E is 1; '
                                'every other output is 1.')
    a = b.bus_input('A', 3)
    enable = b.input('E')
    inverted = [b.gate1('Not', 1, a[i]) for i in range(3)]
    outputs = b.bus_output('/Y', 8, 5)
    for n in range(8):
        bits = [a[i] if n >> i & 1 else inverted[i] for i in range(3)]
        match = b.gate2('And', 3, b.gate2('And', 2, bits[0], bits[1]), bits[2])
        b.wire(b.gate2('Nand', 4, match, enable), outputs[n])
    b.save()


def make_register():
    b = Builder('Register 16',
                '16-bit register. On the rising edge of CLK it loads D if /LOAD is 0 and keeps its value '
                'otherwise. Two tri-state read ports: QA drives while /OEA is 0, QB while /OEB is 0. '
                'In 74HC terms: two 74HC377s and four 74HC541s.')
    d = b.bus_input('D', 16)
    load, clock = b.input('/LOAD'), b.input('CLK')
    read_a, read_b = b.input('/OEA'), b.input('/OEB')
    keep = load  # /LOAD = 1 keeps Q, 0 takes D: next = /LOAD ? Q : D
    take = b.gate1('Not', 1, load)
    outputs = {port: b.bus_output(port, 16, 5 if port == 'QA' else 6) for port in ('QA', 'QB')}
    for i in range(16):
        flip_flop = b.gate('FlipFlop', 3)
        q = '%d.Q' % flip_flop
        b.wire(b.gate2('Or', 2, b.gate2('And', 1, d[i], take), b.gate2('And', 1, q, keep)), '%d.D' % flip_flop)
        b.wire(clock, '%d.CLK' % flip_flop)
        for port, enable in (('QA', read_a), ('QB', read_b)):
            buffer = b.gate('TriState', 4)
            b.wire(q, '%d.A' % buffer)
            b.wire(enable, '%d./OE' % buffer)
            b.wire('%d.Y' % buffer, outputs[port][i])
    b.save()


def make_register_file():
    b = Builder('Register File',
                'R0-R7, 16 bits each. Two read ports: QA shows the register RA picks, QB the one RB picks, '
                'all the time. One write port: on the rising edge of CLK, if WE is 1, the register W picks '
                'loads D.')
    d = b.bus_input('D', 16)
    w = b.bus_input('W', 3)
    write_enable, clock = b.input('WE'), b.input('CLK')
    ra = b.bus_input('RA', 3)
    rb = b.bus_input('RB', 3)
    high = b.gate('High', 1)

    decoders = {}
    for name, address, enable in (('write', w, write_enable), ('A', ra, high), ('B', rb, high)):
        decoder = b.part('Decoder 3to8', 1, 8)
        for i in range(3):
            b.wire(address[i], '%d.A%d' % (decoder, i))
        b.wire(enable, '%d.E' % decoder)
        decoders[name] = decoder

    qa = b.bus_output('QA', 16, 4)
    qb = b.bus_output('QB', 16, 5)
    for r in range(8):
        register = b.part('Register 16', 2 + r % 2, 36)
        for i in range(16):
            b.wire(d[i], '%d.D%d' % (register, i))
            b.wire('%d.QA%d' % (register, i), qa[i])
            b.wire('%d.QB%d' % (register, i), qb[i])
        b.wire('%d./Y%d' % (decoders['write'], r), '%d./LOAD' % register)
        b.wire('%d./Y%d' % (decoders['A'], r), '%d./OEA' % register)
        b.wire('%d./Y%d' % (decoders['B'], r), '%d./OEB' % register)
        b.wire(clock, '%d.CLK' % register)
    b.save()


if __name__ == '__main__':
    make_decoder()
    make_register()
    make_register_file()
    print('wrote Decoder 3to8, Register 16 and Register File')
