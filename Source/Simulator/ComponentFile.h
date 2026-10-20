// ComponentFile.h -- components as YAML files, one per component, and ROM/RAM images.
//
//   name: Half Adder
//   description: S = A xor B, C = A and B.
//   nodes:
//     - {id: 1, kind: Input, label: A, at: [40, 40]}
//     - {id: 2, kind: Input, label: B}
//     - {id: 3, kind: Xor}
//     - {id: 4, kind: And}
//     - {id: 5, component: Some Other Component}
//     - {id: 6, kind: Output, label: S}
//     - {id: 7, kind: Output, label: C}
//   wires:
//     - 1 -> 3.A
//     - 2 -> 3.B
//     - 3.Y -> 6
//
// A wire runs "node.pin -> node.pin". The pin is a pin name or a number (0 is the
// first pin on that side); an Input or Output node has one pin, so its id alone is
// enough. "at" is the editor position and may be left out. The order of the Input
// and Output nodes is the order of the component's pins.
//
// Buses: `{id: 9, kind: Input, label: A, width: 16}` is one node with 16 pins, A0..A15; it
// gives the component pins A0..A15, in that order. A wire can join a run of pins at once:
//
//     - 9 -> 12.A0..A15        # all of node 9 (16 bits) to pins A0 through A15 of node 12
//     - 9.A4..A7 -> 13.D0..D3  # four bits of it
//
// Both ends of a bus wire are the same width. Wires that run side by side are saved this way.
//
// A ROM has address pins A0.. and data pins D0.. (or the names given):
//
//     - id: 8
//       kind: ROM
//       address: 4                  # address bits: 2^4 = 16 words
//       data: 8                     # bits per word
//       outputs: [ALU0, ALU1, "", REG_WE]   # optional pin names; "" keeps the default
//       contents:                   # hex; a row is "address: word word ...", 16 to a row;
//         - "0000: 01 02 04 08"     #   rows left out are zero
//         - "0010: FF"
//
// or, in place of contents, `image: ../Binary/Programs/Fibonacci.bin` takes the words from a
// binary file (little-endian, as many bytes per word as the data width needs), relative to
// the components directory. The file is read when the component loads and is never written.
// `lane: even` (or odd) takes only the image's even (odd) bytes: the two 8-bit chips of a 16-bit
// memory each hold one lane.

#pragma once

#include "Circuit.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Kestrel {

std::string Trim(const std::string& text);

// ROM and RAM images.
int RomBytesPerWord(const MemoryData& rom);
std::string LoadRomImage(MemoryData& rom);                                     // reads rom.image into rom.words; returns a problem, or ""
std::string LoadRomBytes(MemoryData& rom, const std::vector<uint8_t>& bytes);  // the same, from bytes
bool SaveRomImage(const MemoryData& rom, const std::string& path);

// How a wire end is written: "4" for all of a pin node, "4.Y" by name when that's unambiguous,
// else "4.1"; a bus is "4.A0..A15" (or "4.0..15").
std::string EndpointText(const Node& node, bool output, int index, int width = 1);

std::string ComponentToYaml(const Component& component);
// Parses component YAML. Wires are kept as text in pendingWires until ResolveWires, because
// pin names can only be looked up once every component they refer to has been loaded.
// Throws YAML::Exception (or std::runtime_error) with a message on bad input.
std::unique_ptr<Component> ComponentFromYaml(const std::string& text);
// Turns pendingWires into wires. Returns a message for each wire that couldn't be resolved.
std::vector<std::string> ResolveWires(Component& component);

// Loads every .yaml file in the components directory into the library. Returns a message for
// each problem.
std::vector<std::string> LoadLibrary();
int SaveAll();  // saves every changed component; returns how many couldn't be
bool AnyDirty();

}  // namespace Kestrel
