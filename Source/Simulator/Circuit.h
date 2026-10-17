// Circuit.h -- the circuit model: components made of nodes joined by wires.
//
// A Component is a graph of Nodes joined by Wires. A node is either a primitive (gate,
// flip-flop, constant, clock, pin, memory, terminal) or an instance of another component. A
// component's Input and Output nodes are its pins, in the order they were added; that is the
// order its pins appear on the black box.
//
// Wires run from an output pin to an input pin. An input pin may have several wires: that ties
// those outputs together, which is how a bus is made.
//
// A RAM's data pins are bidirectional, so they're drawn on both sides of it: D0 on the left
// (for whatever writes) and D0 on the right (for whatever reads) are the same wire. The RAM is
// one more driver on it, and lets go when it isn't being read.
//
// The Terminal stands in for a UART: it is the emulator's UART data and status registers (see
// Source/Kestrel/MemoryMap.h), with its screen and keyboard in the simulation panel.

#pragma once

#include "imgui.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

struct ImNodesEditorContext;

namespace Kestrel {

// ---------------------------------------------------------------------------
// Primitives

enum class NodeKind : uint32_t {
    Input,
    Output,
    Low,
    High,
    Clock,
    Buffer,
    Not,
    And,
    Or,
    Nand,
    Nor,
    Xor,
    Xnor,
    FlipFlop,
    TriState,
    Rom,        // lookup table; its pin counts come from its MemoryData
    Ram,        // static RAM; likewise
    Terminal,   // the screen and keyboard, as a UART on the bus
    Component,  // an instance of another component; not a primitive
};

constexpr int PrimitiveCount = static_cast<int>(NodeKind::Component);

struct PrimitiveInfo {
    const char* name;
    int inputCount;
    int outputCount;
    const char* inputNames[2];
    const char* outputNames[2];
    const char* description;
};

extern const PrimitiveInfo Primitives[PrimitiveCount];

inline bool IsPrimitive(NodeKind kind) {
    return static_cast<int>(kind) < PrimitiveCount;
}
inline bool IsMemory(NodeKind kind) {
    return kind == NodeKind::Rom || kind == NodeKind::Ram;
}

constexpr int TerminalControlPins = 4;  // RS, /CE, /OE, /WE; then D0-D7 in
constexpr int TerminalDataBits = 8;

// ---------------------------------------------------------------------------
// Components

constexpr int MaxNameLength = 32;   // including the terminating zero
constexpr int MaxPinsPerSide = 64;  // pin ids are packed as node * 128 + side * 64 + index
constexpr int MaxNestingDepth = 64;
constexpr int MaxMemoryAddressBits = 16;
constexpr int MaxMemoryDataBits = 32;
constexpr int RomControlPins = 2;  // /CE and /OE, after the address pins
constexpr int RamControlPins = 3;  // /CE, /OE and /WE, after the address pins; then D0.. in
constexpr int MaxBusWidth = 32;

// A ROM's or RAM's shape and pin names. For a ROM, words are its contents, shared so a running
// simulation sees edits as they're made. For a RAM, words are what it's preloaded with (from
// image) when a simulation starts; the simulation keeps its own copy.
struct MemoryData {
    int addressBits = 4;
    int dataBits = 8;
    std::vector<uint32_t> words = std::vector<uint32_t>(16, 0);  // always 2^addressBits long
    std::vector<std::string> addressNames, dataNames;            // optional pin names; "" means A0, D0, ...
    std::string image;                                           // a binary file the contents come from, relative to the components directory
    int lane = 0;                                                // 0: every byte of image; 1: only its even bytes; 2: only its odd bytes

    uint32_t DataMask() const { return dataBits >= 32 ? 0xFFFFFFFFu : (1u << dataBits) - 1; }
    void Resize(int newAddressBits, int newDataBits) {
        addressBits = std::clamp(newAddressBits, 1, MaxMemoryAddressBits);
        dataBits = std::clamp(newDataBits, 1, MaxMemoryDataBits);
        words.resize(size_t{1} << addressBits, 0);
        for (uint32_t& word : words) {
            word &= DataMask();
        }
        addressNames.resize(static_cast<size_t>(addressBits));
        dataNames.resize(static_cast<size_t>(dataBits));
    }
};

struct Node {
    uint32_t id = 0;
    NodeKind kind = NodeKind::And;
    std::string typeName;                // for NodeKind::Component: which component this is
    std::string label;                   // for Input/Output: the pin's name
    int width = 1;                       // for Input/Output: how many pins; a bus of N names them label0..labelN-1
    ImVec2 position{0, 0};               // editor grid-space position, at zoom 1
    std::shared_ptr<MemoryData> memory;  // for NodeKind::Rom and NodeKind::Ram

    enum class Placement { None,
                           Grid,
                           Screen };
    Placement placement = Placement::Grid;  // tells the editor to move the node on next draw
};

// A wire of width N is a bus: it joins pins fromPin..fromPin+N-1 to toPin..toPin+N-1.
struct Wire {
    uint32_t id = 0;
    uint32_t fromNode = 0, fromPin = 0;  // an output pin
    uint32_t toNode = 0, toPin = 0;      // an input pin
    uint32_t width = 1;
};

struct PendingWire {
    std::string from, to;  // wire ends as written in the file, e.g. "4.Y"
};

struct Component {
    std::string name;
    std::string description;
    std::string savedName;  // the file this was loaded from or last saved to
    std::vector<Node> nodes;
    std::vector<Wire> wires;
    std::vector<PendingWire> pendingWires;  // read from a file, not yet turned into wires
    std::vector<std::string> loadWarnings;  // things that went wrong loading it that didn't stop it
    std::set<int> expandedBuses;            // editor only: bus pins (by header id) showing their bits
    uint32_t nextId = 1;
    bool dirty = false;
    ImNodesEditorContext* editor = nullptr;  // keeps this component's pan and selection
    float zoom = 1.0f;

    Node* FindNode(uint32_t id) {
        for (Node& node : nodes) {
            if (node.id == id) {
                return &node;
            }
        }
        return nullptr;
    }
    const Node* FindNode(uint32_t id) const { return const_cast<Component*>(this)->FindNode(id); }
};

// Every component loaded or made, and the folder they're saved in.
extern std::vector<std::unique_ptr<Component>> library;
extern std::string componentsDirectory;

Component* FindComponent(const std::string& name);
Component* CreateComponent(const std::string& name);
std::string UniqueComponentName(const std::string& base);
bool IsValidComponentName(const std::string& name);
void RenameComponent(Component& component, const std::string& newName);           // and every use of it
bool Uses(const Component& component, const std::string& target, int depth = 0);  // anywhere inside it?

// ---------------------------------------------------------------------------
// Pins

// The Input (or Output) nodes of a component, in pin order.
std::vector<const Node*> PinNodes(const Component& component, NodeKind side);

// A component's pins on one side, one per bit: a bus Input of width 16 gives 16 of them.
struct PinBit {
    const Node* node;
    int bit;
};

std::vector<PinBit> PinBits(const Component& component, NodeKind side);
int InputCount(const Node& node);
int OutputCount(const Node& node);
std::string PinName(const Node& node, bool output, int index);
std::string NodeTitle(const Node& node);

// Pins named like A0, A1, A2, ... one after another form a bus: the editor shows them as one
// pin, and wires along them as one bus wire. A group of one is a plain pin.
struct PinGroup {
    int first = 0, count = 1;
};

std::vector<PinGroup> PinGroups(const Node& node, bool output);
PinGroup GroupOfPin(const std::vector<PinGroup>& groups, int pin);

// ---------------------------------------------------------------------------
// Editing

std::string UniquePinLabel(const Component& component, NodeKind side);
uint32_t AddNode(Component& component, NodeKind kind, const std::string& typeName = "", const std::string& label = "");
// Connects an output pin to an input pin. An input that already has wires keeps them: the
// outputs are tied together, as on a bus.
void Connect(Component& component, uint32_t fromNode, int fromPin, uint32_t toNode, int toPin, int width = 1);
void RemoveNode(Component& component, uint32_t id);
void RemoveWire(Component& component, uint32_t id);
void PruneWires(Component& component);                                              // drops wires whose pins no longer exist
void RemapMemoryWires(Component& component, const Node& node, int oldAddressBits);  // after a ROM or RAM resizes
void MergeBusWires(Component& component);                                           // joins wires that run side by side into bus wires

}  // namespace Kestrel
