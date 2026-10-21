"""Writes templates/Eurocard 3U.kicad_pcb, the board place.py --template fits a card onto.

Run it with KiCad's own Python, which has the pcbnew module:

  "C:/Program Files/KiCad/9.0/bin/python.exe" Tools/eda/template.py

A 3U Eurocard is 160 mm deep and 100 mm high. The backplane connector, a DIN 41612 96-pin male right-angle (P1),
goes on the left 100 mm edge with its 32 columns centred on it, so pins a1 and a32 sit 10.63 mm from the corners.
Its body's front is flush with the edge: check that against the connector's datasheet and the card cage before
ordering boards. Add mounting holes or anything else in KiCad and save; place.py keeps whatever is on the template.
"""
import os

import pcbnew

WIDTH, HEIGHT = 160.0, 100.0  # depth into the cage, and height along the connector edge
ORIGIN = (50.0, 50.0)  # where the board's top-left corner sits on the KiCad page
CONNECTOR = ('Connector_DIN', 'DIN41612_C_3x32_Male_Horizontal_THT')
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'templates', 'Eurocard 3U.kicad_pcb')


def mm(value):
    return pcbnew.FromMM(value)


def main():
    from kicad_env import find_kicad
    board = pcbnew.BOARD()
    board.SetCopperLayerCount(4)
    ox, oy = ORIGIN
    corners = [(0, 0), (WIDTH, 0), (WIDTH, HEIGHT), (0, HEIGHT)]
    for (x0, y0), (x1, y1) in zip(corners, corners[1:] + corners[:1]):
        segment = pcbnew.PCB_SHAPE(board)
        segment.SetShape(pcbnew.SHAPE_T_SEGMENT)
        segment.SetStart(pcbnew.VECTOR2I(mm(ox + x0), mm(oy + y0)))
        segment.SetEnd(pcbnew.VECTOR2I(mm(ox + x1), mm(oy + y1)))
        segment.SetLayer(pcbnew.Edge_Cuts)
        segment.SetWidth(mm(0.1))
        board.Add(segment)

    library, name = CONNECTOR
    footprint = pcbnew.FootprintLoad(os.path.join(find_kicad('share/kicad/footprints'), library + '.pretty'), name)
    footprint.SetFPIDAsString('%s:%s' % CONNECTOR)
    footprint.SetReference('P1')
    footprint.SetValue('DIN 41612 96-pin')
    # Rotated 90 degrees, the mating face (the footprint's -y side) points left and a1..a32 run up the edge.
    footprint.SetOrientationDegrees(90)
    footprint.SetPosition(pcbnew.VECTOR2I(0, 0))
    pads = {pad.GetNumber(): pad.GetPosition() for pad in footprint.Pads()}
    body = footprint.GetBoundingBox(False)
    middle = (pads['a1'].y + pads['a32'].y) // 2
    footprint.SetPosition(pcbnew.VECTOR2I(mm(ox) - body.GetLeft(), mm(oy + HEIGHT / 2) - middle))
    footprint.SetLocked(True)
    board.Add(footprint)

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    board.Save(OUT)
    a1 = {pad.GetNumber(): pad.GetPosition() for pad in footprint.Pads()}['a1']
    print('wrote %s: %g x %g mm, P1 a1 at %.2f mm from the bottom corner'
          % (OUT, WIDTH, HEIGHT, oy + HEIGHT - pcbnew.ToMM(a1.y)))


if __name__ == '__main__':
    import sys
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    main()
