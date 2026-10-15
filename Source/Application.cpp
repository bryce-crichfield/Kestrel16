/*
 * Circuit Workbench -- the hardware side of the ISA-16 project.
 *
 * Build circuits out of logic gates, wrap them up as components with their own
 * input and output pins, and use those components as black boxes inside bigger
 * ones. Then simulate the result gate by gate with a controllable clock.
 *
 * Naming: ProperCase for types, functions and constants; camelCase for variables.
 *
 * ---------------------------------------------------------------------------
 * Model
 *   A Component is a graph of Nodes joined by Wires. A node is either a
 *   primitive (gate, flip-flop, constant, clock, pin) or an instance of another
 *   component. A component's Input and Output nodes are its pins, in the order
 *   they were added; that is the order its pins appear on the black box.
 *
 *   Wires run from an output pin to an input pin. An input pin may have several
 *   wires: that ties those outputs together, which is how a bus is made.
 *
 * Simulation
 *   The component being edited is flattened: every nested component is expanded
 *   until only primitives are left, and connected pins are merged into nets.
 *
 *   Timing is "unit delay": in each step every gate looks at its inputs as they
 *   were at the end of the previous step, and all gates update together. So one
 *   step is one gate delay, and the number of steps a circuit needs to settle
 *   after the clock ticks is the length of its slowest path. That number is
 *   shown in the simulation panel. Feedback is allowed (latches work); a circuit
 *   that never settles is reported as unstable.
 *
 *   A net is 0, 1, Z (nothing drives it: every driver is off, as a tri-state
 *   output is when disabled) or X (drivers disagree: a bus conflict, which would
 *   short two chips in real life). A gate reading Z or X sees 0, as does an
 *   unconnected input.
 *
 *   A RAM's data pins are bidirectional, so they're drawn on both sides of it:
 *   D0 on the left (for whatever writes) and D0 on the right (for whatever reads)
 *   are the same wire. The RAM is one more driver on it, and lets go when it
 *   isn't being read.
 *
 *   The Terminal stands in for a UART: it is the emulator's 0xFF00 (data) and 0xFF02
 *   (status) registers, with its screen and keyboard in the simulation panel.
 *
 *   D flip-flops copy D to Q when their CLK input goes from 0 to 1. Because all
 *   gates update together, every flip-flop clocked by the same edge samples its
 *   D input at the same moment, as in real synchronous logic.
 *
 * Files
 *   Each component is saved as Components/<name>.yaml (format described at the
 *   Files section below). It is meant to be read and written by hand, or by an AI
 *   working from a datasheet. Components refer to the components they use by name.
 *
 * Command line
 *   Application                     run the editor
 *   Application --components DIR    load and save components in DIR
 *   Application --self-test         check the simulator without opening a window
 *   Application --screenshot FILE   render a few frames, save a PNG, and exit
 *   Application --run COMPONENT --program FILE
 *                                   simulate without a window; the Terminal prints to
 *                                   stdout. FILE is a .c, .s or .bin (see --help)
 * ---------------------------------------------------------------------------
 */

#include "imgui.h"
#include "imnodes.h"
#include "raylib.h"
#include "rlImGui.h"
#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

#ifndef COMPONENTS_PATH
#define COMPONENTS_PATH "Components/"
#endif

// ===========================================================================
// Primitives
// ===========================================================================

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

static const PrimitiveInfo Primitives[PrimitiveCount] = {
    {"Input",  0, 1, {},         {""},        "A pin into this component. Simulated on its own, it is a switch."},
    {"Output", 1, 0, {""},       {},          "A pin out of this component. Simulated on its own, it is an LED."},
    {"Low",    0, 1, {},         {"0"},       "Always 0 (tied to ground)."},
    {"High",   0, 1, {},         {"1"},       "Always 1 (tied to the supply)."},
    {"Clock",  0, 1, {},         {"clk"},     "The simulation clock."},
    {"Buffer", 1, 1, {"A"},      {"Y"},       "Y = A."},
    {"NOT",    1, 1, {"A"},      {"Y"},       "Y = not A.  (74HC04 has six)"},
    {"AND",    2, 1, {"A", "B"}, {"Y"},       "Y = A and B.  (74HC08 has four)"},
    {"OR",     2, 1, {"A", "B"}, {"Y"},       "Y = A or B.  (74HC32 has four)"},
    {"NAND",   2, 1, {"A", "B"}, {"Y"},       "Y = not (A and B).  (74HC00 has four)"},
    {"NOR",    2, 1, {"A", "B"}, {"Y"},       "Y = not (A or B).  (74HC02 has four)"},
    {"XOR",    2, 1, {"A", "B"}, {"Y"},       "Y = A xor B.  (74HC86 has four)"},
    {"XNOR",   2, 1, {"A", "B"}, {"Y"},       "Y = not (A xor B)."},
    {"D Flip-Flop", 2, 2, {"D", "CLK"}, {"Q", "/Q"},
     "Q takes the value of D when CLK goes from 0 to 1.  (74HC74 has two)"},
    {"Tri-State Buffer", 2, 1, {"A", "/OE"}, {"Y"},
     "Y = A while /OE is 0; with /OE at 1, Y lets go (Z) so something else can drive the wire.  (74HC125 has four)"},
    {"ROM", 0, 0, {}, {}, "D = contents[A]: a lookup table, like an EEPROM (AT28C64/AT28C256). "
                          "Use it for opcode decoding, or to hold a program. D lets go (Z) unless /CE and /OE "
                          "are both 0; left unconnected they read 0."},
    {"RAM", 0, 0, {}, {}, "Static RAM like the 62256 (32K x 8). D0.. appear on both sides: they are one "
                          "bidirectional wire. /CE=1: D lets go. /WE=0: stores D at A every step it stays 0 "
                          "(D isn't driven, whatever /OE says). /OE=0 and /WE=1: drives contents[A] on D. "
                          "Contents exist only while simulating; Reset clears or randomizes them, or loads a "
                          ".bin. Wire /WE: unconnected it reads 0, which means write."},
    {"Terminal", 0, 0, {}, {}, "A UART standing in for the screen and keyboard, like the emulator's 0xFF00. "
                               "RS=0 (wire A1): write prints the byte on D, read takes the next typed key (0 if none). "
                               "RS=1: read gives status, bit 0 = a key is waiting, bit 1 = ready to print. "
                               "D0-D7 are on both sides, like a RAM's. A write prints when /WE (or /CE) goes back "
                               "to 1; a key is used up when the read ends. IRQ is 1 while a key is waiting."},
};

static bool IsPrimitive(NodeKind kind) { return static_cast<int>(kind) < PrimitiveCount; }
static bool IsMemory(NodeKind kind) { return kind == NodeKind::Rom || kind == NodeKind::Ram; }

constexpr int TerminalControlPins = 4;  // RS, /CE, /OE, /WE; then D0-D7 in
constexpr int TerminalDataBits = 8;
static const char* const TerminalControlNames[TerminalControlPins] = {"RS", "/CE", "/OE", "/WE"};

// ===========================================================================
// Component model
// ===========================================================================

constexpr int MaxNameLength = 32;   // including the terminating zero
constexpr int MaxPinsPerSide = 64;  // pin ids are packed as node * 128 + side * 64 + index
constexpr int MaxNestingDepth = 64;
constexpr int MaxMemoryAddressBits = 16;
constexpr int MaxMemoryDataBits = 32;
constexpr int RomControlPins = 2;  // /CE and /OE, after the address pins
constexpr int RamControlPins = 3;  // /CE, /OE and /WE, after the address pins; then D0.. in

// A ROM's or RAM's shape and pin names. For a ROM, words are its contents, shared so a running
// simulation sees edits as they're made. For a RAM, words are what it's preloaded with (from
// image) when a simulation starts; the simulation keeps its own copy.
struct MemoryData {
    int addressBits = 4;
    int dataBits = 8;
    std::vector<uint32_t> words = std::vector<uint32_t>(16, 0);  // always 2^addressBits long
    std::vector<std::string> addressNames, dataNames;            // optional pin names; "" means A0, D0, ...
    std::string image;  // a binary file the contents come from, relative to the components directory
    int lane = 0;       // 0: every byte of image; 1: only its even bytes; 2: only its odd bytes

    uint32_t DataMask() const { return dataBits >= 32 ? 0xFFFFFFFFu : (1u << dataBits) - 1; }
    void Resize(int newAddressBits, int newDataBits)
    {
        addressBits = std::clamp(newAddressBits, 1, MaxMemoryAddressBits);
        dataBits = std::clamp(newDataBits, 1, MaxMemoryDataBits);
        words.resize(size_t{1} << addressBits, 0);
        for (uint32_t& word : words) word &= DataMask();
        addressNames.resize(static_cast<size_t>(addressBits));
        dataNames.resize(static_cast<size_t>(dataBits));
    }
};

struct Node {
    uint32_t id = 0;
    NodeKind kind = NodeKind::And;
    std::string typeName;  // for NodeKind::Component: which component this is
    std::string label;     // for Input/Output: the pin's name
    int width = 1;         // for Input/Output: how many pins; a bus of N names them label0..labelN-1
    ImVec2 position{0, 0}; // editor grid-space position, at zoom 1
    std::shared_ptr<MemoryData> memory;  // for NodeKind::Rom and NodeKind::Ram

    enum class Placement { None, Grid, Screen };
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

    Node* FindNode(uint32_t id)
    {
        for (Node& node : nodes)
            if (node.id == id) return &node;
        return nullptr;
    }
    const Node* FindNode(uint32_t id) const { return const_cast<Component*>(this)->FindNode(id); }
};

static std::vector<std::unique_ptr<Component>> library;
static std::string componentsDirectory = COMPONENTS_PATH;

static Component* FindComponent(const std::string& name)
{
    for (auto& component : library)
        if (component->name == name) return component.get();
    return nullptr;
}

constexpr int MaxBusWidth = 32;

// The Input (or Output) nodes of a component, in pin order.
static std::vector<const Node*> PinNodes(const Component& component, NodeKind side)
{
    std::vector<const Node*> pins;
    for (const Node& node : component.nodes)
        if (node.kind == side) pins.push_back(&node);
    return pins;
}

// A component's pins on one side, one per bit: a bus Input of width 16 gives 16 of them.
struct PinBit {
    const Node* node;
    int bit;
};

static std::vector<PinBit> PinBits(const Component& component, NodeKind side)
{
    std::vector<PinBit> pins;
    for (const Node* node : PinNodes(component, side))
        for (int bit = 0; bit < node->width && static_cast<int>(pins.size()) < MaxPinsPerSide; bit++)
            pins.push_back({node, bit});
    return pins;
}

static std::string BitLabel(const Node& node, int bit)
{
    return node.width == 1 ? node.label : node.label + std::to_string(bit);
}

static int InputCount(const Node& node)
{
    if (node.kind == NodeKind::Terminal) return TerminalControlPins + TerminalDataBits;
    if (node.kind == NodeKind::Input) return 0;
    if (node.kind == NodeKind::Output) return node.width;
    if (node.kind == NodeKind::Rom) return node.memory ? node.memory->addressBits + RomControlPins : 0;
    if (node.kind == NodeKind::Ram)
        return node.memory ? node.memory->addressBits + RamControlPins + node.memory->dataBits : 0;
    if (IsPrimitive(node.kind)) return Primitives[static_cast<int>(node.kind)].inputCount;
    const Component* type = FindComponent(node.typeName);
    return type ? static_cast<int>(PinBits(*type, NodeKind::Input).size()) : 0;
}

static int OutputCount(const Node& node)
{
    if (node.kind == NodeKind::Terminal) return TerminalDataBits + 1;  // and IRQ
    if (node.kind == NodeKind::Input) return node.width;
    if (node.kind == NodeKind::Output) return 0;
    if (IsMemory(node.kind)) return node.memory ? node.memory->dataBits : 0;
    if (IsPrimitive(node.kind)) return Primitives[static_cast<int>(node.kind)].outputCount;
    const Component* type = FindComponent(node.typeName);
    return type ? static_cast<int>(PinBits(*type, NodeKind::Output).size()) : 0;
}

static std::string PinName(const Node& node, bool output, int index)
{
    if (node.kind == NodeKind::Input || node.kind == NodeKind::Output) return BitLabel(node, index);
    if (node.kind == NodeKind::Terminal) {
        if (output) return index < TerminalDataBits ? "D" + std::to_string(index) : "IRQ";
        return index < TerminalControlPins ? TerminalControlNames[index] : "D" + std::to_string(index - TerminalControlPins);
    }
    if (IsMemory(node.kind)) {
        static const char* const ControlNames[] = {"/CE", "/OE", "/WE"};
        int control = output ? -1 : index - node.memory->addressBits;
        int controls = node.kind == NodeKind::Rom ? RomControlPins : RamControlPins;
        if (control >= 0 && control < controls) return ControlNames[control];
        if (control >= controls) {  // a RAM's data pins, input side
            output = true;
            index = control - controls;
        }
        const std::vector<std::string>& names = output ? node.memory->dataNames : node.memory->addressNames;
        if (index < static_cast<int>(names.size()) && !names[index].empty()) return names[index];
        return (output ? "D" : "A") + std::to_string(index);
    }
    if (IsPrimitive(node.kind)) {
        const PrimitiveInfo& info = Primitives[static_cast<int>(node.kind)];
        return output ? info.outputNames[index] : info.inputNames[index];
    }
    const Component* type = FindComponent(node.typeName);
    if (!type) return "?";
    std::vector<PinBit> pins = PinBits(*type, output ? NodeKind::Output : NodeKind::Input);
    return index < static_cast<int>(pins.size()) ? BitLabel(*pins[index].node, pins[index].bit) : "?";
}

static std::string NodeTitle(const Node& node)
{
    switch (node.kind) {
    case NodeKind::Input:     return "IN  " + node.label + (node.width > 1 ? " [" + std::to_string(node.width) + "]" : "");
    case NodeKind::Output:    return "OUT " + node.label + (node.width > 1 ? " [" + std::to_string(node.width) + "]" : "");
    case NodeKind::Component: return FindComponent(node.typeName) ? node.typeName : "Missing: " + node.typeName;
    case NodeKind::Ram:
        return "RAM " + std::to_string(size_t{1} << node.memory->addressBits) + " x " + std::to_string(node.memory->dataBits);
    case NodeKind::Rom:
        return "ROM " + std::to_string(size_t{1} << node.memory->addressBits) + " x " + std::to_string(node.memory->dataBits);
    default:                 return Primitives[static_cast<int>(node.kind)].name;
    }
}

// Pins named like A0, A1, A2, ... one after another form a bus: the editor shows them as one
// pin, and wires along them as one bus wire. A group of one is a plain pin.
struct PinGroup {
    int first = 0, count = 1;
};

static bool SplitBitName(const std::string& name, std::string& prefix, int& number)
{
    size_t digits = name.size();
    while (digits > 0 && std::isdigit(static_cast<unsigned char>(name[digits - 1]))) digits--;
    if (digits == name.size() || digits == 0 || name.size() - digits > 3) return false;
    prefix = name.substr(0, digits);
    number = atoi(name.c_str() + digits);
    return true;
}

static std::vector<PinGroup> PinGroups(const Node& node, bool output)
{
    std::vector<PinGroup> groups;
    int count = output ? OutputCount(node) : InputCount(node);
    std::string prefix, previous;
    int number = 0, previousNumber = -2;
    for (int index = 0; index < count; index++) {
        bool numbered = SplitBitName(PinName(node, output, index), prefix, number);
        if (numbered && !groups.empty() && prefix == previous && number == previousNumber + 1) groups.back().count++;
        else groups.push_back({index, 1});
        previous = numbered ? prefix : "";
        previousNumber = numbered ? number : -2;
    }
    return groups;
}

static PinGroup GroupOfPin(const std::vector<PinGroup>& groups, int pin)
{
    for (const PinGroup& group : groups)
        if (pin >= group.first && pin < group.first + group.count) return group;
    return {pin, 1};
}

// Joins wires that run side by side (A0 -> B0, A1 -> B1, ...) into one bus wire. A bus
// stays within one group at each end.
static void MergeBusWires(Component& component)
{
    std::unordered_map<uint64_t, std::vector<PinGroup>> cache;
    auto groupFirst = [&](uint32_t nodeId, bool output, uint32_t pin) {
        uint64_t key = uint64_t{nodeId} * 2 + output;
        auto found = cache.find(key);
        if (found == cache.end()) {
            const Node* node = component.FindNode(nodeId);
            found = cache.emplace(key, node ? PinGroups(*node, output) : std::vector<PinGroup>{}).first;
        }
        return GroupOfPin(found->second, static_cast<int>(pin)).first;
    };
    std::vector<Wire> singles, result;
    for (const Wire& wire : component.wires) (wire.width == 1 ? singles : result).push_back(wire);
    auto offset = [](const Wire& wire) { return static_cast<int64_t>(wire.toPin) - static_cast<int64_t>(wire.fromPin); };
    std::sort(singles.begin(), singles.end(), [&](const Wire& a, const Wire& b) {
        return std::make_tuple(a.fromNode, a.toNode, offset(a), a.fromPin) <
               std::make_tuple(b.fromNode, b.toNode, offset(b), b.fromPin);
    });
    for (size_t start = 0, end; start < singles.size(); start = end) {
        const Wire& first = singles[start];
        int fromGroup = groupFirst(first.fromNode, true, first.fromPin);
        int toGroup = groupFirst(first.toNode, false, first.toPin);
        for (end = start + 1; end < singles.size(); end++) {
            const Wire& next = singles[end];
            if (next.fromNode != first.fromNode || next.toNode != first.toNode || offset(next) != offset(first) ||
                next.fromPin != first.fromPin + (end - start) ||
                groupFirst(next.fromNode, true, next.fromPin) != fromGroup ||
                groupFirst(next.toNode, false, next.toPin) != toGroup)
                break;
        }
        Wire merged = first;
        merged.width = static_cast<uint32_t>(end - start);
        result.push_back(merged);
    }
    std::sort(result.begin(), result.end(), [](const Wire& a, const Wire& b) { return a.id < b.id; });
    component.wires = std::move(result);
}

// Does `component` use `target` anywhere inside it?
static bool Uses(const Component& component, const std::string& target, int depth = 0)
{
    if (depth > MaxNestingDepth) return true;  // treat runaway nesting as a cycle
    for (const Node& node : component.nodes) {
        if (node.kind != NodeKind::Component) continue;
        if (node.typeName == target) return true;
        const Component* child = FindComponent(node.typeName);
        if (child && Uses(*child, target, depth + 1)) return true;
    }
    return false;
}

static std::string UniquePinLabel(const Component& component, NodeKind side)
{
    const char* prefix = side == NodeKind::Input ? "In" : "Out";
    for (int number = 1;; number++) {
        std::string label = prefix + std::to_string(number);
        bool taken = false;
        for (const Node& node : component.nodes)
            if ((node.kind == NodeKind::Input || node.kind == NodeKind::Output) && node.label == label) taken = true;
        if (!taken) return label;
    }
}

static uint32_t AddNode(Component& component, NodeKind kind, const std::string& typeName = "",
                        const std::string& label = "")
{
    Node node;
    node.id = component.nextId++;
    node.kind = kind;
    node.typeName = typeName;
    if (kind == NodeKind::Input || kind == NodeKind::Output)
        node.label = label.empty() ? UniquePinLabel(component, kind) : label;
    if (IsMemory(kind)) {
        node.memory = std::make_shared<MemoryData>();
        if (kind == NodeKind::Ram) node.memory->Resize(15, 8);  // a 62256
        else node.memory->Resize(node.memory->addressBits, node.memory->dataBits);
    }
    node.placement = Node::Placement::Grid;
    node.position = ImVec2(40.0f + 30.0f * static_cast<float>(component.nodes.size() % 10),
                           40.0f + 30.0f * static_cast<float>(component.nodes.size() % 10));
    component.nodes.push_back(node);
    component.dirty = true;
    return node.id;
}

// Connects an output pin to an input pin. An input that already has wires keeps them: the
// outputs are tied together, as on a bus.
static void Connect(Component& component, uint32_t fromNode, int fromPin, uint32_t toNode, int toPin, int width = 1)
{
    auto& wires = component.wires;
    for (const Wire& wire : wires)
        if (wire.fromNode == fromNode && wire.fromPin == static_cast<uint32_t>(fromPin) && wire.toNode == toNode &&
            wire.toPin == static_cast<uint32_t>(toPin) && wire.width == static_cast<uint32_t>(width))
            return;
    Wire wire;
    wire.id = component.nextId++;
    wire.fromNode = fromNode;
    wire.fromPin = static_cast<uint32_t>(fromPin);
    wire.toNode = toNode;
    wire.toPin = static_cast<uint32_t>(toPin);
    wire.width = static_cast<uint32_t>(width);
    wires.push_back(wire);
    component.dirty = true;
}

static void RemoveNode(Component& component, uint32_t id)
{
    auto& nodes = component.nodes;
    nodes.erase(std::remove_if(nodes.begin(), nodes.end(), [&](const Node& node) { return node.id == id; }),
                nodes.end());
    auto& wires = component.wires;
    wires.erase(std::remove_if(wires.begin(), wires.end(),
                               [&](const Wire& wire) { return wire.fromNode == id || wire.toNode == id; }),
                wires.end());
    component.dirty = true;
}

static void RemoveWire(Component& component, uint32_t id)
{
    auto& wires = component.wires;
    wires.erase(std::remove_if(wires.begin(), wires.end(), [&](const Wire& wire) { return wire.id == id; }),
                wires.end());
    component.dirty = true;
}

// Drops wires whose pins no longer exist (for example after a used component lost a pin).
static void PruneWires(Component& component)
{
    auto& wires = component.wires;
    size_t before = wires.size();
    wires.erase(std::remove_if(wires.begin(), wires.end(),
                               [&](const Wire& wire) {
                                   const Node* from = component.FindNode(wire.fromNode);
                                   const Node* to = component.FindNode(wire.toNode);
                                   return !from || !to || wire.width == 0 ||
                                          static_cast<int>(wire.fromPin + wire.width) > OutputCount(*from) ||
                                          static_cast<int>(wire.toPin + wire.width) > InputCount(*to);
                               }),
                wires.end());
    if (wires.size() != before) component.dirty = true;
}

// After a ROM or RAM changes size, moves wires on its control and data-in pins to where those
// pins now are, and drops wires on pins that went away.
static void RemapMemoryWires(Component& component, const Node& node, int oldAddressBits)
{
    int controls = node.kind == NodeKind::Rom ? RomControlPins : RamControlPins;
    int newAddressBits = node.memory->addressBits, newDataBits = node.memory->dataBits;
    auto& wires = component.wires;
    auto remap = [&](int pin) {
        if (pin < oldAddressBits) return pin < newAddressBits ? pin : -1;
        if (pin < oldAddressBits + controls) return pin - oldAddressBits + newAddressBits;
        return pin - oldAddressBits - controls < newDataBits ? pin - oldAddressBits + newAddressBits : -1;
    };
    for (Wire& wire : wires) {
        if (wire.toNode != node.id) continue;
        int first = remap(static_cast<int>(wire.toPin)), last = remap(static_cast<int>(wire.toPin + wire.width) - 1);
        bool keep = first >= 0 && last - first == static_cast<int>(wire.width) - 1;  // a bus stays in one piece
        wire.toPin = keep ? static_cast<uint32_t>(first) : UINT32_MAX;
    }
    wires.erase(std::remove_if(wires.begin(), wires.end(), [](const Wire& wire) { return wire.toPin == UINT32_MAX; }),
                wires.end());
    PruneWires(component);  // outputs that went away
    component.dirty = true;
}

static Component* CreateComponent(const std::string& name)
{
    auto component = std::make_unique<Component>();
    component->name = name;
    component->dirty = true;
    library.push_back(std::move(component));
    return library.back().get();
}

static std::string UniqueComponentName(const std::string& base)
{
    if (!FindComponent(base)) return base;
    for (int number = 2;; number++) {
        std::string name = base + " " + std::to_string(number);
        if (!FindComponent(name)) return name;
    }
}

static bool IsValidComponentName(const std::string& name)
{
    if (name.empty() || name.size() >= MaxNameLength) return false;
    if (name.front() == ' ' || name.back() == ' ') return false;
    for (char character : name)
        if (!isalnum(static_cast<unsigned char>(character)) && character != ' ' && character != '_' && character != '-')
            return false;
    return true;
}

// Renames a component and every reference to it in the library.
static void RenameComponent(Component& component, const std::string& newName)
{
    for (auto& other : library) {
        for (Node& node : other->nodes) {
            if (node.kind == NodeKind::Component && node.typeName == component.name) {
                node.typeName = newName;
                other->dirty = true;
            }
        }
    }
    component.name = newName;
    component.dirty = true;
}

// ===========================================================================
// Files: one YAML file per component
// ===========================================================================
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

static const char* const KindKeys[] = {"Input", "Output", "Low", "High", "Clock", "Buffer", "Not",
                                       "And",   "Or",     "Nand", "Nor",  "Xor",   "Xnor",   "FlipFlop",
                                       "TriState", "ROM",    "RAM",  "Terminal"};
static_assert(sizeof KindKeys / sizeof KindKeys[0] == PrimitiveCount, "one key per primitive");

static std::string ComponentPath(const std::string& name) { return componentsDirectory + name + ".yaml"; }

static std::string Trim(const std::string& text)
{
    size_t first = text.find_first_not_of(" \t"), last = text.find_last_not_of(" \t");
    return first == std::string::npos ? "" : text.substr(first, last - first + 1);
}

static int RomBytesPerWord(const MemoryData& rom) { return (rom.dataBits + 7) / 8; }

// Reads rom.image into rom.words. Returns a problem, or "".
static std::string LoadRomImage(MemoryData& rom)
{
    FILE* file = fopen((fs::path(componentsDirectory) / rom.image).string().c_str(), "rb");
    if (!file) return "can't open image '" + rom.image + "'";
    std::vector<uint8_t> bytes;
    uint8_t buffer[4096];
    for (size_t count; (count = fread(buffer, 1, sizeof buffer, file)) > 0;) bytes.insert(bytes.end(), buffer, buffer + count);
    fclose(file);
    if (rom.lane) {  // one byte lane of a 16-bit memory, as an EPROM programmer's even/odd split does
        std::vector<uint8_t> lane;
        for (size_t index = static_cast<size_t>(rom.lane - 1); index < bytes.size(); index += 2) lane.push_back(bytes[index]);
        bytes.swap(lane);
    }

    size_t bytesPerWord = static_cast<size_t>(RomBytesPerWord(rom));
    std::fill(rom.words.begin(), rom.words.end(), 0);
    size_t wordCount = (bytes.size() + bytesPerWord - 1) / bytesPerWord;
    for (size_t address = 0; address < std::min(wordCount, rom.words.size()); address++) {
        uint32_t word = 0;
        for (size_t byte = 0; byte < bytesPerWord && address * bytesPerWord + byte < bytes.size(); byte++)
            word |= static_cast<uint32_t>(bytes[address * bytesPerWord + byte]) << (8 * byte);
        rom.words[address] = word & rom.DataMask();
    }
    if (wordCount > rom.words.size())
        return "image '" + rom.image + "' has " + std::to_string(wordCount) + " words; only the first " +
               std::to_string(rom.words.size()) + " fit";
    return "";
}

static bool SaveRomImage(const MemoryData& rom, const std::string& path)
{
    FILE* file = fopen(path.c_str(), "wb");
    if (!file) return false;
    size_t bytesPerWord = static_cast<size_t>(RomBytesPerWord(rom));
    std::vector<uint8_t> bytes;
    for (uint32_t word : rom.words)
        for (size_t byte = 0; byte < bytesPerWord; byte++) bytes.push_back(static_cast<uint8_t>(word >> (8 * byte)));
    bool ok = fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    return (fclose(file) == 0) && ok;
}

constexpr int RomWordsPerRow = 16;

// "0010: 01 02 FF" for every row of the ROM that isn't all zero.
static std::vector<std::string> RomContentRows(const MemoryData& rom)
{
    std::vector<std::string> rows;
    int addressDigits = std::max(4, (rom.addressBits + 3) / 4), wordDigits = (rom.dataBits + 3) / 4;
    for (size_t row = 0; row < rom.words.size(); row += RomWordsPerRow) {
        size_t end = std::min(row + RomWordsPerRow, rom.words.size());
        while (end > row && rom.words[end - 1] == 0) end--;  // trailing zeros needn't be written
        if (end == row) continue;
        char text[32];
        snprintf(text, sizeof text, "%0*zX:", addressDigits, row);
        std::string line = text;
        for (size_t address = row; address < end; address++) {
            snprintf(text, sizeof text, " %0*X", wordDigits, static_cast<unsigned>(rom.words[address]));
            line += text;
        }
        rows.push_back(line);
    }
    return rows;
}

static void ParseRomContentRow(MemoryData& rom, const std::string& line)
{
    size_t colon = line.find(':');
    if (colon == std::string::npos) throw std::runtime_error("ROM row '" + line + "' has no 'address:'");
    std::string addressText = Trim(line.substr(0, colon));
    char* end = nullptr;
    unsigned long address = strtoul(addressText.c_str(), &end, 16);
    if (addressText.empty() || *end) throw std::runtime_error("ROM row '" + line + "' has a bad address");
    const char* cursor = line.c_str() + colon + 1;
    for (;;) {
        while (*cursor == ' ' || *cursor == '\t') cursor++;
        if (!*cursor) break;
        unsigned long word = strtoul(cursor, &end, 16);
        if (end == cursor) throw std::runtime_error("ROM row '" + line + "' has a word that isn't hex");
        if (address >= rom.words.size()) throw std::runtime_error("ROM row '" + line + "' runs past the end of the ROM");
        rom.words[address++] = static_cast<uint32_t>(word) & rom.DataMask();
        cursor = end;
    }
}

// How a wire end is written: "4" for all of a pin node, "4.Y" by name when that's unambiguous,
// else "4.1"; a bus is "4.A0..A15" (or "4.0..15").
static std::string EndpointText(const Node& node, bool output, int index, int width = 1)
{
    std::string text = std::to_string(node.id);
    bool pinNode = node.kind == NodeKind::Input || node.kind == NodeKind::Output;
    if (pinNode && index == 0 && width == node.width) return text;
    auto pinText = [&](int pin) {
        std::string name = PinName(node, output, pin);
        int count = output ? OutputCount(node) : InputCount(node), matches = 0;
        for (int other = 0; other < count; other++)
            if (PinName(node, output, other) == name) matches++;
        bool byName = !name.empty() && name != "?" && matches == 1 && name.find("->") == std::string::npos &&
                      name.find("..") == std::string::npos;
        return byName ? name : std::to_string(pin);
    };
    std::string first = pinText(index), last = pinText(index + width - 1);
    bool numbers = std::isdigit(static_cast<unsigned char>(first[0])) || std::isdigit(static_cast<unsigned char>(last[0]));
    if (numbers) first = std::to_string(index), last = std::to_string(index + width - 1);  // don't mix the two
    return text + "." + first + (width > 1 ? ".." + last : "");
}

static std::string ComponentToYaml(const Component& component)
{
    YAML::Emitter out;
    out << YAML::BeginMap;
    out << YAML::Key << "name" << YAML::Value << component.name;
    if (!component.description.empty()) out << YAML::Key << "description" << YAML::Value << component.description;

    out << YAML::Key << "nodes" << YAML::Value << YAML::BeginSeq;
    for (const Node& node : component.nodes) {
        if (!IsMemory(node.kind)) out << YAML::Flow;
        out << YAML::BeginMap;
        out << YAML::Key << "id" << YAML::Value << node.id;
        if (node.kind == NodeKind::Component) out << YAML::Key << "component" << YAML::Value << node.typeName;
        else out << YAML::Key << "kind" << YAML::Value << KindKeys[static_cast<int>(node.kind)];
        if (node.kind == NodeKind::Input || node.kind == NodeKind::Output) {
            out << YAML::Key << "label" << YAML::Value << node.label;
            if (node.width > 1) out << YAML::Key << "width" << YAML::Value << node.width;
        }
        out << YAML::Key << "at" << YAML::Value << YAML::Flow << YAML::BeginSeq << std::lround(node.position.x)
            << std::lround(node.position.y) << YAML::EndSeq;
        if (IsMemory(node.kind)) {
            const MemoryData& rom = *node.memory;
            out << YAML::Key << "address" << YAML::Value << rom.addressBits;
            out << YAML::Key << "data" << YAML::Value << rom.dataBits;
            auto names = [&](const char* key, const std::vector<std::string>& list) {
                if (std::all_of(list.begin(), list.end(), [](const std::string& name) { return name.empty(); })) return;
                out << YAML::Key << key << YAML::Value << YAML::Flow << YAML::BeginSeq;
                for (const std::string& name : list) out << YAML::DoubleQuoted << name;
                out << YAML::EndSeq;
            };
            names("inputs", rom.addressNames);
            names("outputs", rom.dataNames);
            if (!rom.image.empty()) {
                out << YAML::Key << "image" << YAML::Value << rom.image;
                if (rom.lane) out << YAML::Key << "lane" << YAML::Value << (rom.lane == 1 ? "even" : "odd");
            } else if (node.kind == NodeKind::Rom) {
                out << YAML::Key << "contents" << YAML::Value << YAML::BeginSeq;
                for (const std::string& row : RomContentRows(rom)) out << YAML::DoubleQuoted << row;
                out << YAML::EndSeq;
            }
        }
        out << YAML::EndMap;
    }
    out << YAML::EndSeq;

    out << YAML::Key << "wires" << YAML::Value << YAML::BeginSeq;
    for (const Wire& wire : component.wires) {
        const Node* from = component.FindNode(wire.fromNode);
        const Node* to = component.FindNode(wire.toNode);
        if (!from || !to) continue;
        int width = static_cast<int>(wire.width);
        out << EndpointText(*from, true, static_cast<int>(wire.fromPin), width) + " -> " +
                   EndpointText(*to, false, static_cast<int>(wire.toPin), width);
    }
    out << YAML::EndSeq;
    out << YAML::EndMap;
    return std::string(out.c_str()) + "\n";
}

static bool SaveComponent(Component& component)
{
    std::error_code error;
    fs::create_directories(componentsDirectory, error);

    MergeBusWires(component);
    std::string text = ComponentToYaml(component);
    FILE* file = fopen(ComponentPath(component.name).c_str(), "wb");
    if (!file) return false;
    bool ok = fwrite(text.data(), 1, text.size(), file) == text.size();
    ok = (fclose(file) == 0) && ok;
    if (!ok) return false;

    // A renamed component leaves its old file behind; remove it now that the new one is safe.
    if (!component.savedName.empty() && component.savedName != component.name)
        fs::remove(ComponentPath(component.savedName), error);
    component.savedName = component.name;
    component.dirty = false;
    return true;
}

// Parses component YAML. Wires are kept as text in pendingWires until ResolveWires, because
// pin names can only be looked up once every component they refer to has been loaded.
// Throws YAML::Exception (or std::runtime_error) with a message on bad input.
static std::unique_ptr<Component> ComponentFromYaml(const std::string& text)
{
    YAML::Node root = YAML::Load(text);
    auto component = std::make_unique<Component>();
    component->name = root["name"].as<std::string>("");
    component->description = root["description"].as<std::string>("");
    if (!IsValidComponentName(component->name))
        throw std::runtime_error("bad or missing name '" + component->name + "'");

    for (const YAML::Node& entry : root["nodes"]) {
        Node node;
        node.id = entry["id"].as<uint32_t>();
        if (node.id == 0 || component->FindNode(node.id))
            throw std::runtime_error("node id " + std::to_string(node.id) + " is zero or used twice");
        if (entry["component"]) {
            node.kind = NodeKind::Component;
            node.typeName = entry["component"].as<std::string>();
        } else {
            std::string kind = entry["kind"].as<std::string>("");
            const char* const* found = std::find(std::begin(KindKeys), std::end(KindKeys), kind);
            if (found == std::end(KindKeys)) throw std::runtime_error("node " + std::to_string(node.id) + ": unknown kind '" + kind + "'");
            node.kind = static_cast<NodeKind>(found - std::begin(KindKeys));
        }
        if (node.kind == NodeKind::Input || node.kind == NodeKind::Output) {
            node.label = entry["label"].as<std::string>(UniquePinLabel(*component, node.kind));
            node.width = entry["width"].as<int>(1);
            if (node.width < 1 || node.width > MaxBusWidth)
                throw std::runtime_error("node " + std::to_string(node.id) + ": width is 1-" + std::to_string(MaxBusWidth));
        }
        if (IsMemory(node.kind)) {
            node.memory = std::make_shared<MemoryData>();
            MemoryData& rom = *node.memory;
            bool ram = node.kind == NodeKind::Ram;
            int addressBits = entry["address"].as<int>(ram ? 15 : 4), dataBits = entry["data"].as<int>(8);
            if (addressBits < 1 || addressBits > MaxMemoryAddressBits || dataBits < 1 || dataBits > MaxMemoryDataBits)
                throw std::runtime_error("node " + std::to_string(node.id) + ": a ROM or RAM has 1-" +
                                         std::to_string(MaxMemoryAddressBits) + " address bits and 1-" +
                                         std::to_string(MaxMemoryDataBits) + " data bits");
            rom.Resize(addressBits, dataBits);
            auto names = [&](const char* key, std::vector<std::string>& list) {
                size_t index = 0;
                for (const YAML::Node& name : entry[key])
                    if (index < list.size()) list[index++] = name.as<std::string>();
            };
            names("inputs", rom.addressNames);
            names("outputs", rom.dataNames);
            rom.image = entry["image"].as<std::string>("");
            std::string lane = entry["lane"].as<std::string>("");
            if (!lane.empty() && lane != "even" && lane != "odd")
                throw std::runtime_error("node " + std::to_string(node.id) + ": lane is even or odd");
            rom.lane = lane == "even" ? 1 : lane == "odd" ? 2 : 0;
            if (!rom.image.empty()) {
                std::string problem = LoadRomImage(rom);
                if (!problem.empty())
                    component->loadWarnings.push_back(component->name + ": node " + std::to_string(node.id) + ": " + problem);
            } else if (!ram) {
                for (const YAML::Node& row : entry["contents"]) ParseRomContentRow(rom, row.as<std::string>());
            }
        }
        if (const YAML::Node at = entry["at"]) {
            node.position = ImVec2(at[0].as<float>(), at[1].as<float>());
        } else {
            // No position given: lay out left to right in columns of eight.
            size_t index = component->nodes.size();
            node.position = ImVec2(60.0f + 200.0f * static_cast<float>(index / 8), 40.0f + 90.0f * static_cast<float>(index % 8));
        }
        node.placement = Node::Placement::Grid;
        component->nodes.push_back(node);
        component->nextId = std::max(component->nextId, node.id + 1);
    }

    for (const YAML::Node& entry : root["wires"]) {
        std::string line = entry.as<std::string>();
        size_t arrow = line.find("->");
        if (arrow == std::string::npos) throw std::runtime_error("wire '" + line + "' has no '->'");
        component->pendingWires.push_back({Trim(line.substr(0, arrow)), Trim(line.substr(arrow + 2))});
    }
    return component;
}

// Turns "4.Y" (or a run of pins, "4.A0..A7") into a node id, first pin and pin count.
// Returns an error message, or "" on success.
static std::string ResolveEndpoint(const Component& component, const std::string& text, bool output,
                                   uint32_t& nodeId, uint32_t& pin, uint32_t& width)
{
    size_t dot = text.find('.');
    std::string idText = text.substr(0, dot), pinText = dot == std::string::npos ? "" : text.substr(dot + 1);
    char* end = nullptr;
    unsigned long id = strtoul(idText.c_str(), &end, 10);
    const Node* node = idText.empty() || *end ? nullptr : component.FindNode(static_cast<uint32_t>(id));
    if (!node) return "no node '" + idText + "'";
    nodeId = node->id;
    pin = 0;
    width = 1;
    if (pinText.empty()) {
        if (node->kind == NodeKind::Input || node->kind == NodeKind::Output) width = static_cast<uint32_t>(node->width);
        return "";
    }

    auto find = [&](const std::string& name, uint32_t& found) -> std::string {
        int count = output ? OutputCount(*node) : InputCount(*node);
        for (int index = 0; index < count; index++) {
            if (PinName(*node, output, index) == name) {
                found = static_cast<uint32_t>(index);
                return "";
            }
        }
        unsigned long number = strtoul(name.c_str(), &end, 10);
        if (name.empty() || *end) return "node " + idText + " has no " + (output ? "output" : "input") + " pin '" + name + "'";
        found = static_cast<uint32_t>(number);  // out-of-range numbers are dropped later by PruneWires
        return "";
    };
    size_t range = pinText.find("..");
    if (range == std::string::npos) return find(pinText, pin);
    uint32_t last = 0;
    std::string problem = find(pinText.substr(0, range), pin);
    if (problem.empty()) problem = find(pinText.substr(range + 2), last);
    if (problem.empty() && last < pin) problem = "'" + pinText + "' runs backwards";
    width = last - pin + 1;
    return problem;
}

// Turns pendingWires into wires. Returns a message for each wire that couldn't be resolved.
static std::vector<std::string> ResolveWires(Component& component)
{
    std::vector<std::string> problems;
    for (const PendingWire& pending : component.pendingWires) {
        Wire wire;
        uint32_t toWidth = 0;
        std::string problem = ResolveEndpoint(component, pending.from, true, wire.fromNode, wire.fromPin, wire.width);
        if (problem.empty()) problem = ResolveEndpoint(component, pending.to, false, wire.toNode, wire.toPin, toWidth);
        if (problem.empty() && toWidth != wire.width)
            problem = "one end is " + std::to_string(wire.width) + " bits wide, the other " + std::to_string(toWidth);
        if (!problem.empty()) {
            problems.push_back(component.name + ": wire '" + pending.from + " -> " + pending.to + "': " + problem);
            continue;
        }
        wire.id = component.nextId++;
        component.wires.push_back(wire);
    }
    component.pendingWires.clear();
    MergeBusWires(component);
    return problems;
}

static std::unique_ptr<Component> LoadComponent(const std::string& path, std::string& problem)
{
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) {
        problem = "can't open it";
        return nullptr;
    }
    std::string text;
    char buffer[4096];
    for (size_t count; (count = fread(buffer, 1, sizeof buffer, file)) > 0;) text.append(buffer, count);
    fclose(file);

    try {
        std::unique_ptr<Component> component = ComponentFromYaml(text);
        component->savedName = component->name;
        return component;
    } catch (const std::exception& exception) {
        problem = exception.what();
        return nullptr;
    }
}

// Loads every .yaml file in the components directory. Returns a message for each problem.
static std::vector<std::string> LoadLibrary()
{
    std::vector<std::string> problems;
    std::error_code error;
    if (!fs::is_directory(componentsDirectory, error)) return problems;

    std::vector<fs::path> paths;
    for (const auto& entry : fs::directory_iterator(componentsDirectory, error))
        if (entry.path().extension() == ".yaml") paths.push_back(entry.path());
    std::sort(paths.begin(), paths.end());

    for (const fs::path& path : paths) {
        std::string problem;
        std::unique_ptr<Component> component = LoadComponent(path.string(), problem);
        if (!component) {
            problems.push_back("Couldn't read " + path.filename().string() + ": " + problem);
        } else if (FindComponent(component->name)) {
            problems.push_back("Skipped " + path.filename().string() + ": duplicate name '" + component->name + "'");
        } else {
            library.push_back(std::move(component));
        }
    }
    for (auto& component : library) {
        std::vector<std::string> wireProblems = ResolveWires(*component);
        problems.insert(problems.end(), component->loadWarnings.begin(), component->loadWarnings.end());
        problems.insert(problems.end(), wireProblems.begin(), wireProblems.end());
        component->loadWarnings.clear();
    }
    return problems;
}

static int SaveAll()
{
    int failures = 0;
    for (auto& component : library)
        if (component->dirty && !SaveComponent(*component)) failures++;
    return failures;
}

static bool AnyDirty()
{
    for (auto& component : library)
        if (component->dirty) return true;
    return false;
}

// ===========================================================================
// Simulation
// ===========================================================================

enum class GateKind : uint8_t { Switch, Low, High, Clock, Buffer, Not, And, Or, Nand, Nor, Xor, Xnor, FlipFlop, TriState };

struct Gate {
    GateKind kind;
    int inputs[2] = {-1, -1};   // net ids (endpoint ids while flattening)
    int outputs[2] = {-1, -1};
    uint8_t state = 0;          // switch position, or flip-flop contents
    uint8_t lastClock = 0;      // flip-flop: CLK as of the previous step
};

// A ROM in the flattened circuit: reads its address nets, drives its data nets.
struct RomUnit {
    std::shared_ptr<const MemoryData> data;
    std::vector<int> address, outputs;  // net ids (endpoint ids while flattening)
    int chipEnable = -1, outputEnable = -1;
};

// A RAM in the flattened circuit. Its contents belong to the simulation.
struct RamUnit {
    std::shared_ptr<const MemoryData> memory;
    std::vector<int> address, data;  // net ids; data nets are both read and driven
    int chipEnable = -1, outputEnable = -1, writeEnable = -1;
    std::vector<uint32_t> words;
};

// A Terminal in the flattened circuit. Its screen and keyboard are the simulation's console and keys.
struct TerminalUnit {
    int registerSelect = -1, chipEnable = -1, outputEnable = -1, writeEnable = -1, irq = -1;
    std::vector<int> data;  // net ids; read when written, driven when read
    bool writing = false, reading = false;  // in the middle of a write or read cycle
    int writeRegister = 0, readRegister = 0;
    uint8_t writeByte = 0;
};

constexpr int MaxSettleSteps = 1000;

// Net levels. Only High counts as 1 to whatever reads the net.
constexpr uint8_t Low = 0, High = 1, Floating = 2, Conflict = 3;

struct Simulation {
    std::vector<Gate> gates;
    std::vector<RomUnit> roms;
    std::vector<RamUnit> rams;
    std::vector<TerminalUnit> terminals;
    std::string console;       // what the terminals have printed
    std::deque<uint8_t> keys;  // typed, not yet read
    bool keysWanted = false;   // a status read found no key waiting (headless runs read stdin then)
    std::vector<uint8_t> signals, nextSignals;
    int netCount = 0;

    // The edited component's own nodes, for drawing and inspecting: pin -> net.
    std::unordered_map<uint32_t, std::vector<int>> nodeInputNets, nodeOutputNets;
    std::unordered_map<uint32_t, std::vector<int>> switchGates;  // Input node -> its switch gates, bit 0 first
    std::unordered_map<uint32_t, int> topRams;      // RAM node -> its unit in rams

    uint8_t clock = 0;
    uint64_t halfCycles = 0;
    int lastSettleSteps = 0;
    int slowestSettleSteps = 0;
    int conflicts = 0;  // nets driven to 0 and 1 at once, after the last settle
    bool unstable = false;
    int missingComponents = 0;
    bool nestingTooDeep = false;

    bool running = false;
    float frequency = 2.0f;  // Hz
    bool randomRam = false;  // RAM powers up holding random bytes, as real RAM does, rather than zeros
    double pendingHalfCycles = 0;

    int NetOfOutput(uint32_t node, int pin) const
    {
        auto found = nodeOutputNets.find(node);
        return found != nodeOutputNets.end() && pin < static_cast<int>(found->second.size()) ? found->second[pin] : -1;
    }
    int NetOfInput(uint32_t node, int pin) const
    {
        auto found = nodeInputNets.find(node);
        return found != nodeInputNets.end() && pin < static_cast<int>(found->second.size()) ? found->second[pin] : -1;
    }
    int Value(int net) const { return net >= 0 && signals[net] == High; }
    uint8_t Level(int net) const { return net >= 0 ? signals[net] : Floating; }
};

// Expands a component into primitive gates. Pins are "endpoints"; connecting two
// endpoints merges them (union-find), and each merged group becomes one net.
class Flattener {
public:
    std::vector<Gate> gates;
    std::vector<RomUnit> roms;
    std::vector<RamUnit> rams;
    std::vector<TerminalUnit> terminals;
    std::unordered_map<uint32_t, int> topRams;
    std::unordered_map<uint32_t, std::vector<int>> topInputs, topOutputs;
    std::unordered_map<uint32_t, std::vector<int>> topSwitches;
    int missingComponents = 0;
    bool nestingTooDeep = false;

    int NewEndpoint()
    {
        parent.push_back(static_cast<int>(parent.size()));
        return static_cast<int>(parent.size()) - 1;
    }

    int Find(int endpoint)
    {
        while (parent[endpoint] != endpoint) {
            parent[endpoint] = parent[parent[endpoint]];
            endpoint = parent[endpoint];
        }
        return endpoint;
    }

    void Merge(int a, int b) { parent[Find(a)] = Find(b); }

    // boundaryInputs/Outputs are the endpoints of the pins on the instance being expanded.
    void Expand(const Component& component, const std::vector<int>& boundaryInputs,
                const std::vector<int>& boundaryOutputs, int depth)
    {
        struct Endpoints { std::vector<int> inputs, outputs; };
        std::unordered_map<uint32_t, Endpoints> endpoints;
        size_t inputPin = 0, outputPin = 0;

        for (const Node& node : component.nodes) {
            Endpoints& pins = endpoints[node.id];
            for (int index = 0; index < InputCount(node); index++) pins.inputs.push_back(NewEndpoint());
            for (int index = 0; index < OutputCount(node); index++) pins.outputs.push_back(NewEndpoint());

            switch (node.kind) {
            case NodeKind::Input:
                for (size_t bit = 0; bit < pins.outputs.size(); bit++, inputPin++) {
                    if (depth == 0) {
                        Gate gate{GateKind::Switch};
                        gate.outputs[0] = pins.outputs[bit];
                        topSwitches[node.id].push_back(static_cast<int>(gates.size()));
                        gates.push_back(gate);
                    } else if (inputPin < boundaryInputs.size()) {
                        Merge(pins.outputs[bit], boundaryInputs[inputPin]);
                    }
                }
                break;
            case NodeKind::Output:
                for (size_t bit = 0; bit < pins.inputs.size(); bit++, outputPin++)
                    if (depth > 0 && outputPin < boundaryOutputs.size()) Merge(pins.inputs[bit], boundaryOutputs[outputPin]);
                break;
            case NodeKind::Rom:
            {
                RomUnit rom{node.memory, pins.inputs, pins.outputs};
                rom.address.resize(static_cast<size_t>(node.memory->addressBits));
                rom.chipEnable = pins.inputs[static_cast<size_t>(node.memory->addressBits)];
                rom.outputEnable = pins.inputs[static_cast<size_t>(node.memory->addressBits) + 1];
                roms.push_back(rom);
                break;
            }
            case NodeKind::Ram: {
                size_t addressBits = static_cast<size_t>(node.memory->addressBits);
                RamUnit ram;
                ram.memory = node.memory;
                ram.address.assign(pins.inputs.begin(), pins.inputs.begin() + static_cast<long>(addressBits));
                ram.chipEnable = pins.inputs[addressBits];
                ram.outputEnable = pins.inputs[addressBits + 1];
                ram.writeEnable = pins.inputs[addressBits + 2];
                for (size_t bit = 0; bit < pins.outputs.size(); bit++) {
                    Merge(pins.inputs[addressBits + RamControlPins + bit], pins.outputs[bit]);  // one wire, two sides
                    ram.data.push_back(pins.outputs[bit]);
                }
                if (depth == 0) topRams[node.id] = static_cast<int>(rams.size());
                rams.push_back(std::move(ram));
                break;
            }
            case NodeKind::Terminal: {
                TerminalUnit terminal;
                terminal.registerSelect = pins.inputs[0];
                terminal.chipEnable = pins.inputs[1];
                terminal.outputEnable = pins.inputs[2];
                terminal.writeEnable = pins.inputs[3];
                for (int bit = 0; bit < TerminalDataBits; bit++) {
                    Merge(pins.inputs[static_cast<size_t>(TerminalControlPins + bit)], pins.outputs[static_cast<size_t>(bit)]);
                    terminal.data.push_back(pins.outputs[static_cast<size_t>(bit)]);
                }
                terminal.irq = pins.outputs[TerminalDataBits];
                terminals.push_back(std::move(terminal));
                break;
            }
            case NodeKind::Component: {
                const Component* child = FindComponent(node.typeName);
                if (!child) missingComponents++;
                else if (depth >= MaxNestingDepth) nestingTooDeep = true;
                else Expand(*child, pins.inputs, pins.outputs, depth + 1);
                break;
            }
            default: {
                static const GateKind GateKinds[PrimitiveCount] = {
                    GateKind::Switch, GateKind::Switch, GateKind::Low,  GateKind::High, GateKind::Clock,
                    GateKind::Buffer, GateKind::Not,    GateKind::And,  GateKind::Or,   GateKind::Nand,
                    GateKind::Nor,    GateKind::Xor,    GateKind::Xnor, GateKind::FlipFlop, GateKind::TriState,
                    GateKind::Low,  GateKind::Low,  GateKind::Low};  // ROM, RAM and Terminal aren't gates
                Gate gate{GateKinds[static_cast<int>(node.kind)]};
                for (size_t index = 0; index < pins.inputs.size(); index++) gate.inputs[index] = pins.inputs[index];
                for (size_t index = 0; index < pins.outputs.size(); index++) gate.outputs[index] = pins.outputs[index];
                gates.push_back(gate);
                break;
            }
            }

            if (depth == 0) {
                topInputs[node.id] = pins.inputs;
                topOutputs[node.id] = pins.outputs;
            }
        }

        for (const Wire& wire : component.wires) {
            auto from = endpoints.find(wire.fromNode), to = endpoints.find(wire.toNode);
            if (from == endpoints.end() || to == endpoints.end()) continue;
            if (wire.fromPin + wire.width > from->second.outputs.size() || wire.toPin + wire.width > to->second.inputs.size())
                continue;
            for (uint32_t bit = 0; bit < wire.width; bit++)
                Merge(from->second.outputs[wire.fromPin + bit], to->second.inputs[wire.toPin + bit]);
        }
    }

    // Turns endpoint ids into compact net ids everywhere. Returns the net count.
    int AssignNets()
    {
        std::vector<int> netOfRoot(parent.size(), -1);
        int netCount = 0;
        auto toNet = [&](int endpoint) {
            if (endpoint < 0) return -1;
            int root = Find(endpoint);
            if (netOfRoot[root] < 0) netOfRoot[root] = netCount++;
            return netOfRoot[root];
        };
        for (Gate& gate : gates) {
            for (int& input : gate.inputs) input = toNet(input);
            for (int& output : gate.outputs) output = toNet(output);
        }
        for (RomUnit& rom : roms) {
            for (int& input : rom.address) input = toNet(input);
            rom.chipEnable = toNet(rom.chipEnable);
            rom.outputEnable = toNet(rom.outputEnable);
            for (int& output : rom.outputs) output = toNet(output);
        }
        for (TerminalUnit& terminal : terminals) {
            for (int* net : {&terminal.registerSelect, &terminal.chipEnable, &terminal.outputEnable,
                             &terminal.writeEnable, &terminal.irq})
                *net = toNet(*net);
            for (int& data : terminal.data) data = toNet(data);
        }
        for (RamUnit& ram : rams) {
            for (int& input : ram.address) input = toNet(input);
            for (int& data : ram.data) data = toNet(data);
            ram.chipEnable = toNet(ram.chipEnable);
            ram.outputEnable = toNet(ram.outputEnable);
            ram.writeEnable = toNet(ram.writeEnable);
        }
        for (auto& entry : topInputs)
            for (int& endpoint : entry.second) endpoint = toNet(endpoint);
        for (auto& entry : topOutputs)
            for (int& endpoint : entry.second) endpoint = toNet(endpoint);
        return netCount;
    }

private:
    std::vector<int> parent;
};

// Lets every gate react until nothing changes. Returns the number of gate delays it took.
// With edgesEnabled false (at power-up), flip-flops only track their clock level and RAM doesn't
// write: every net starts out floating, which would otherwise look like /WE low for a moment.
static int Settle(Simulation& simulation, bool edgesEnabled = true)
{
    std::vector<uint8_t>& current = simulation.signals;
    std::vector<uint8_t>& next = simulation.nextSignals;

    // Every net starts each step undriven; each driver then adds its level.
    auto drive = [&](int net, int level) {
        if (net < 0 || level == Floating) return;
        uint8_t& now = next[net];
        now = now == Floating || now == level ? static_cast<uint8_t>(level) : Conflict;
    };
    auto read = [&](int net) -> int { return net >= 0 && current[net] == High; };

    for (int step = 1; step <= MaxSettleSteps; step++) {
        bool changed = false;
        next.assign(current.size(), Floating);

        for (Gate& gate : simulation.gates) {
            auto in = [&](int index) -> int { return read(gate.inputs[index]); };
            int value;
            switch (gate.kind) {
            case GateKind::Switch: value = gate.state; break;
            case GateKind::Low:    value = 0; break;
            case GateKind::High:   value = 1; break;
            case GateKind::Clock:  value = simulation.clock; break;
            case GateKind::Buffer: value = in(0); break;
            case GateKind::Not:    value = !in(0); break;
            case GateKind::And:    value = in(0) & in(1); break;
            case GateKind::Or:     value = in(0) | in(1); break;
            case GateKind::Nand:   value = !(in(0) & in(1)); break;
            case GateKind::Nor:    value = !(in(0) | in(1)); break;
            case GateKind::Xor:    value = in(0) ^ in(1); break;
            case GateKind::Xnor:   value = !(in(0) ^ in(1)); break;
            case GateKind::FlipFlop: {
                int clockNow = in(1);
                if (edgesEnabled && clockNow && !gate.lastClock && gate.state != in(0)) {
                    gate.state = static_cast<uint8_t>(in(0));
                    changed = true;
                }
                if (gate.lastClock != clockNow) changed = true;
                gate.lastClock = static_cast<uint8_t>(clockNow);
                value = gate.state;
                drive(gate.outputs[1], !value);
                break;
            }
            case GateKind::TriState: value = in(1) ? Floating : in(0); break;
            default: value = 0; break;
            }
            drive(gate.outputs[0], value);
        }
        for (const RomUnit& rom : simulation.roms) {
            if (read(rom.chipEnable) || read(rom.outputEnable)) continue;  // not selected: outputs let go
            size_t address = 0;
            for (size_t bit = 0; bit < rom.address.size(); bit++)
                if (read(rom.address[bit])) address |= size_t{1} << bit;
            uint32_t word = address < rom.data->words.size() ? rom.data->words[address] : 0;
            for (size_t bit = 0; bit < rom.outputs.size(); bit++) drive(rom.outputs[bit], (word >> bit) & 1);
        }
        for (RamUnit& ram : simulation.rams) {
            if (read(ram.chipEnable)) continue;  // deselected: ignore everything, drive nothing
            size_t address = 0;
            for (size_t bit = 0; bit < ram.address.size(); bit++)
                if (read(ram.address[bit])) address |= size_t{1} << bit;
            if (!read(ram.writeEnable)) {
                if (!edgesEnabled) continue;
                // Writing: store whatever is on D, every step, as the real chip does while /WE is low.
                uint32_t word = 0;
                for (size_t bit = 0; bit < ram.data.size(); bit++)
                    if (read(ram.data[bit])) word |= 1u << bit;
                if (ram.words[address] != word) {
                    ram.words[address] = word;
                    changed = true;
                }
            } else if (!read(ram.outputEnable)) {
                uint32_t word = ram.words[address];
                for (size_t bit = 0; bit < ram.data.size(); bit++) drive(ram.data[bit], (word >> bit) & 1);
            }
        }
        for (TerminalUnit& terminal : simulation.terminals) {
            // A write or read takes effect once, when its cycle ends, as a real UART's does: the
            // byte is latched while /WE is low and printed when /WE (or /CE) goes back to 1.
            bool selected = !read(terminal.chipEnable), writeEnable = !read(terminal.writeEnable);
            bool writeNow = edgesEnabled && selected && writeEnable;
            bool readNow = edgesEnabled && selected && !writeEnable && !read(terminal.outputEnable);
            int registerNow = read(terminal.registerSelect);
            if (writeNow) {
                terminal.writing = true;
                terminal.writeRegister = registerNow;
                terminal.writeByte = 0;
                for (int bit = 0; bit < TerminalDataBits; bit++)
                    if (read(terminal.data[static_cast<size_t>(bit)])) terminal.writeByte |= static_cast<uint8_t>(1 << bit);
            } else if (terminal.writing) {
                terminal.writing = false;
                if (terminal.writeRegister == 0) {
                    simulation.console += static_cast<char>(terminal.writeByte);
                    changed = true;
                }
            }
            if (readNow) {
                // Latch the register as the read starts: when it ends, the address can move on
                // before the decoder lets go of /CE, which would turn a data read into a status one.
                if (!terminal.reading) terminal.readRegister = registerNow;
                terminal.reading = true;
                if (terminal.readRegister && simulation.keys.empty()) simulation.keysWanted = true;
                int byte = registerNow ? (simulation.keys.empty() ? 0 : 1) | 2  // status: key waiting, ready to print
                                       : (simulation.keys.empty() ? 0 : simulation.keys.front());
                for (int bit = 0; bit < TerminalDataBits; bit++) drive(terminal.data[static_cast<size_t>(bit)], (byte >> bit) & 1);
            } else if (terminal.reading) {
                terminal.reading = false;
                if (terminal.readRegister == 0 && !simulation.keys.empty()) {
                    simulation.keys.pop_front();
                    changed = true;
                }
            }
            drive(terminal.irq, !simulation.keys.empty());
        }

        if (next != current) changed = true;
        current.swap(next);
        if (!changed) {
            simulation.unstable = false;
            simulation.conflicts = static_cast<int>(std::count(current.begin(), current.end(), Conflict));
            return step - 1;
        }
    }
    simulation.unstable = true;
    return MaxSettleSteps;
}

static void RecordSettle(Simulation& simulation)
{
    simulation.lastSettleSteps = Settle(simulation);
    simulation.slowestSettleSteps = std::max(simulation.slowestSettleSteps, simulation.lastSettleSteps);
}

static void BuildSimulation(Simulation& simulation, const Component& top)
{
    Flattener flattener;
    flattener.Expand(top, {}, {}, 0);
    int netCount = flattener.AssignNets();

    bool keepRunning = simulation.running, keepRandomRam = simulation.randomRam;
    float keepFrequency = simulation.frequency;
    simulation = Simulation{};
    simulation.running = keepRunning;
    simulation.frequency = keepFrequency;
    simulation.randomRam = keepRandomRam;
    simulation.gates = std::move(flattener.gates);
    simulation.roms = std::move(flattener.roms);
    simulation.rams = std::move(flattener.rams);
    simulation.topRams = std::move(flattener.topRams);
    simulation.terminals = std::move(flattener.terminals);
    // RAM powers up preloaded from its image, or else holding zeros or random bytes.
    std::mt19937 random(std::random_device{}());
    for (RamUnit& ram : simulation.rams) {
        if (!ram.memory->image.empty()) {
            ram.words = ram.memory->words;
        } else {
            ram.words.assign(ram.memory->words.size(), 0);
            if (simulation.randomRam)
                for (uint32_t& word : ram.words) word = static_cast<uint32_t>(random()) & ram.memory->DataMask();
        }
    }
    simulation.netCount = netCount;
    simulation.signals.assign(static_cast<size_t>(netCount), Floating);
    simulation.nodeInputNets = std::move(flattener.topInputs);
    simulation.nodeOutputNets = std::move(flattener.topOutputs);
    simulation.switchGates = std::move(flattener.topSwitches);
    simulation.missingComponents = flattener.missingComponents;
    simulation.nestingTooDeep = flattener.nestingTooDeep;
    // Power-up: let everything settle without treating initial clock levels as edges.
    simulation.lastSettleSteps = Settle(simulation, false);
}

static void HalfStep(Simulation& simulation)
{
    simulation.clock ^= 1;
    simulation.halfCycles++;
    RecordSettle(simulation);
}

// Sets an Input node's switches (bit n of value for its pin n) without settling.
static void SetSwitchBits(Simulation& simulation, uint32_t inputNode, uint32_t value)
{
    auto found = simulation.switchGates.find(inputNode);
    if (found == simulation.switchGates.end()) return;
    for (size_t bit = 0; bit < found->second.size(); bit++)
        simulation.gates[static_cast<size_t>(found->second[bit])].state = static_cast<uint8_t>((value >> bit) & 1);
}

static void SetSwitch(Simulation& simulation, uint32_t inputNode, uint32_t value)
{
    SetSwitchBits(simulation, inputNode, value);
    RecordSettle(simulation);
}

static uint32_t SwitchValue(const Simulation& simulation, uint32_t inputNode)
{
    auto found = simulation.switchGates.find(inputNode);
    if (found == simulation.switchGates.end()) return 0;
    uint32_t value = 0;
    for (size_t bit = 0; bit < found->second.size(); bit++)
        value |= static_cast<uint32_t>(simulation.gates[static_cast<size_t>(found->second[bit])].state) << bit;
    return value;
}

// ===========================================================================
// Self-test (--self-test): builds small circuits and checks the simulator
// ===========================================================================

static int testFailures = 0;

static void Check(bool condition, const char* what)
{
    if (!condition) {
        printf("  FAIL: %s\n", what);
        testFailures++;
    }
}

static uint32_t PinNodeByLabel(const Component& component, NodeKind side, const char* label)
{
    for (const Node& node : component.nodes)
        if (node.kind == side && node.label == label) return node.id;
    return 0;
}

static void SetInput(Simulation& simulation, const Component& component, const char* label, int value)
{
    SetSwitch(simulation, PinNodeByLabel(component, NodeKind::Input, label), value);
}

static int ReadOutput(const Simulation& simulation, const Component& component, const char* label)
{
    return simulation.Value(simulation.NetOfInput(PinNodeByLabel(component, NodeKind::Output, label), 0));
}

static uint8_t ReadLevel(const Simulation& simulation, const Component& component, const char* label)
{
    return simulation.Level(simulation.NetOfInput(PinNodeByLabel(component, NodeKind::Output, label), 0));
}

// Sets several inputs at once without settling: "A" with 16 bits sets the bus Input A, or else
// the Inputs A0..A15.
static void SetBus(Simulation& simulation, const Component& component, const char* name, int bits, uint32_t value)
{
    if (uint32_t bus = PinNodeByLabel(component, NodeKind::Input, name); bus && component.FindNode(bus)->width == bits) {
        SetSwitchBits(simulation, bus, value);
        return;
    }
    for (int bit = 0; bit < bits; bit++) {
        std::string label = bits == 1 ? name : name + std::to_string(bit);
        SetSwitchBits(simulation, PinNodeByLabel(component, NodeKind::Input, label.c_str()), (value >> bit) & 1);
    }
}

static uint32_t ReadBus(const Simulation& simulation, const Component& component, const char* name, int bits)
{
    uint32_t value = 0;
    uint32_t bus = PinNodeByLabel(component, NodeKind::Output, name);
    for (int bit = 0; bit < bits; bit++) {
        int net = bus ? simulation.NetOfInput(bus, bit)
                      : simulation.NetOfInput(PinNodeByLabel(component, NodeKind::Output, (name + std::to_string(bit)).c_str()), 0);
        value |= static_cast<uint32_t>(simulation.Value(net)) << bit;
    }
    return value;
}

static void TestFullAdder(const char* heading)
{
    Component* fullAdder = FindComponent("Full Adder");
    Simulation simulation;
    BuildSimulation(simulation, *fullAdder);
    printf("%s: %zu gates, %d nets\n", heading, simulation.gates.size(), simulation.netCount);
    for (int value = 0; value < 8; value++) {
        int a = value & 1, b = (value >> 1) & 1, carryIn = (value >> 2) & 1;
        SetInput(simulation, *fullAdder, "A", a);
        SetInput(simulation, *fullAdder, "B", b);
        SetInput(simulation, *fullAdder, "Cin", carryIn);
        int total = a + b + carryIn;
        Check(ReadOutput(simulation, *fullAdder, "S") == (total & 1), "full adder sum");
        Check(ReadOutput(simulation, *fullAdder, "Cout") == (total >> 1), "full adder carry");
    }
}

static int RunSelfTest()
{
    // Half adder: S = A xor B, C = A and B
    Component* halfAdder = CreateComponent("Half Adder");
    {
        uint32_t a = AddNode(*halfAdder, NodeKind::Input, "", "A");
        uint32_t b = AddNode(*halfAdder, NodeKind::Input, "", "B");
        uint32_t xorGate = AddNode(*halfAdder, NodeKind::Xor);
        uint32_t andGate = AddNode(*halfAdder, NodeKind::And);
        uint32_t sum = AddNode(*halfAdder, NodeKind::Output, "", "S");
        uint32_t carry = AddNode(*halfAdder, NodeKind::Output, "", "C");
        Connect(*halfAdder, a, 0, xorGate, 0);
        Connect(*halfAdder, b, 0, xorGate, 1);
        Connect(*halfAdder, a, 0, andGate, 0);
        Connect(*halfAdder, b, 0, andGate, 1);
        Connect(*halfAdder, xorGate, 0, sum, 0);
        Connect(*halfAdder, andGate, 0, carry, 0);
    }

    // Full adder from two half adders: tests nesting.
    Component* fullAdder = CreateComponent("Full Adder");
    {
        uint32_t a = AddNode(*fullAdder, NodeKind::Input, "", "A");
        uint32_t b = AddNode(*fullAdder, NodeKind::Input, "", "B");
        uint32_t carryIn = AddNode(*fullAdder, NodeKind::Input, "", "Cin");
        uint32_t first = AddNode(*fullAdder, NodeKind::Component, "Half Adder");
        uint32_t second = AddNode(*fullAdder, NodeKind::Component, "Half Adder");
        uint32_t orGate = AddNode(*fullAdder, NodeKind::Or);
        uint32_t sum = AddNode(*fullAdder, NodeKind::Output, "", "S");
        uint32_t carryOut = AddNode(*fullAdder, NodeKind::Output, "", "Cout");
        Connect(*fullAdder, a, 0, first, 0);
        Connect(*fullAdder, b, 0, first, 1);
        Connect(*fullAdder, first, 0, second, 0);       // S of the first
        Connect(*fullAdder, carryIn, 0, second, 1);
        Connect(*fullAdder, first, 1, orGate, 0);       // C of the first
        Connect(*fullAdder, second, 1, orGate, 1);
        Connect(*fullAdder, second, 0, sum, 0);
        Connect(*fullAdder, orGate, 0, carryOut, 0);
    }

    {
        Simulation simulation;
        BuildSimulation(simulation, *halfAdder);
        printf("Half adder: %zu gates, %d nets\n", simulation.gates.size(), simulation.netCount);
        for (int value = 0; value < 4; value++) {
            int a = value & 1, b = value >> 1;
            SetInput(simulation, *halfAdder, "A", a);
            SetInput(simulation, *halfAdder, "B", b);
            Check(ReadOutput(simulation, *halfAdder, "S") == (a ^ b), "half adder sum");
            Check(ReadOutput(simulation, *halfAdder, "C") == (a & b), "half adder carry");
        }
    }

    TestFullAdder("Full adder");

    // SR latch from two cross-coupled NANDs: tests feedback.
    Component* latch = CreateComponent("SR Latch");
    {
        uint32_t set = AddNode(*latch, NodeKind::Input, "", "/S");
        uint32_t reset = AddNode(*latch, NodeKind::Input, "", "/R");
        uint32_t top = AddNode(*latch, NodeKind::Nand);
        uint32_t bottom = AddNode(*latch, NodeKind::Nand);
        uint32_t q = AddNode(*latch, NodeKind::Output, "", "Q");
        Connect(*latch, set, 0, top, 0);
        Connect(*latch, bottom, 0, top, 1);
        Connect(*latch, reset, 0, bottom, 0);
        Connect(*latch, top, 0, bottom, 1);
        Connect(*latch, top, 0, q, 0);

        Simulation simulation;
        BuildSimulation(simulation, *latch);
        printf("SR latch: %zu gates\n", simulation.gates.size());
        SetInput(simulation, *latch, "/S", 1);
        SetInput(simulation, *latch, "/R", 0);  // reset
        SetInput(simulation, *latch, "/R", 1);  // hold
        Check(ReadOutput(simulation, *latch, "Q") == 0, "latch holds 0 after reset");
        SetInput(simulation, *latch, "/S", 0);  // set
        SetInput(simulation, *latch, "/S", 1);  // hold
        Check(ReadOutput(simulation, *latch, "Q") == 1, "latch holds 1 after set");
        Check(!simulation.unstable, "latch settles");
    }

    // 2-bit ripple counter: each flip-flop's /Q feeds its own D, so it toggles.
    // Bit 0 is clocked by the clock, bit 1 by bit 0's /Q (which rises when Q0 falls).
    Component* counter = CreateComponent("Counter");
    {
        uint32_t clock = AddNode(*counter, NodeKind::Clock);
        uint32_t bit0 = AddNode(*counter, NodeKind::FlipFlop);
        uint32_t bit1 = AddNode(*counter, NodeKind::FlipFlop);
        uint32_t q0 = AddNode(*counter, NodeKind::Output, "", "Q0");
        uint32_t q1 = AddNode(*counter, NodeKind::Output, "", "Q1");
        Connect(*counter, bit0, 1, bit0, 0);
        Connect(*counter, clock, 0, bit0, 1);
        Connect(*counter, bit1, 1, bit1, 0);
        Connect(*counter, bit0, 1, bit1, 1);
        Connect(*counter, bit0, 0, q0, 0);
        Connect(*counter, bit1, 0, q1, 0);

        Simulation simulation;
        BuildSimulation(simulation, *counter);
        bool counted = true;
        for (int cycle = 1; cycle <= 8; cycle++) {
            HalfStep(simulation);  // rising edge
            HalfStep(simulation);  // falling edge
            int value = ReadOutput(simulation, *counter, "Q0") | (ReadOutput(simulation, *counter, "Q1") << 1);
            if (value != (cycle & 3)) counted = false;
        }
        Check(counted, "ripple counter counts 1, 2, 3, 0, ...");
        printf("Counter: slowest path %d gate delays\n", simulation.slowestSettleSteps);
    }

    // A NOT gate feeding itself never settles.
    Component* ring = CreateComponent("Ring");
    {
        uint32_t inverter = AddNode(*ring, NodeKind::Not);
        Connect(*ring, inverter, 0, inverter, 0);
        Simulation simulation;
        BuildSimulation(simulation, *ring);
        Check(simulation.unstable, "oscillator is reported unstable");
    }

    // A shift register written by hand in YAML, the way a chip would be described from its
    // datasheet: no positions, wires by pin name.
    {
        static const char* const ShiftRegisterYaml = R"(
name: Shift Register 4
description: 4-bit serial in, parallel out. Q0 takes D on each rising CLK; the rest shift along.
nodes:
  - {id: 1, kind: Input, label: D}
  - {id: 2, kind: Input, label: CLK}
  - {id: 3, kind: FlipFlop}
  - {id: 4, kind: FlipFlop}
  - {id: 5, kind: FlipFlop}
  - {id: 6, kind: FlipFlop}
  - {id: 7, kind: Output, label: Q0}
  - {id: 8, kind: Output, label: Q1}
  - {id: 9, kind: Output, label: Q2}
  - {id: 10, kind: Output, label: Q3}
wires:
  - 1 -> 3.D
  - 3.Q -> 4.D
  - 4.Q -> 5.D
  - 5.Q -> 6.D
  - 2 -> 3.CLK
  - 2 -> 4.CLK
  - 2 -> 5.CLK
  - 2 -> 6.CLK
  - 3.Q -> 7
  - 4.Q -> 8
  - 5.Q -> 9
  - 6.0 -> 10
)";
        std::unique_ptr<Component> parsed;
        try {
            parsed = ComponentFromYaml(ShiftRegisterYaml);
        } catch (const std::exception& exception) {
            printf("  shift register YAML: %s\n", exception.what());
        }
        Check(parsed != nullptr, "shift register YAML parses");
        if (parsed) {
            library.push_back(std::move(parsed));
            Component& shifter = *library.back();
            Check(ResolveWires(shifter).empty() && shifter.wires.size() == 12, "shift register wires resolve");

            Simulation simulation;
            BuildSimulation(simulation, shifter);
            auto clockIn = [&](int bit) {
                SetInput(simulation, shifter, "D", bit);
                SetInput(simulation, shifter, "CLK", 1);
                SetInput(simulation, shifter, "CLK", 0);
            };
            auto read = [&] {
                int value = 0;
                for (int bit = 0; bit < 4; bit++)
                    value |= ReadOutput(simulation, shifter, ("Q" + std::to_string(bit)).c_str()) << bit;
                return value;
            };
            // Shift in 1, 0, 1, 1: the first bit in ends up in Q3.
            for (int bit : {1, 0, 1, 1}) clockIn(bit);
            Check(read() == 0b1011, "shift register holds 1,0,1,1 (Q3..Q0)");
            clockIn(0);
            Check(read() == 0b0110, "shift register shifts once more");
            printf("Shift register (from YAML): %zu gates\n", simulation.gates.size());

            // Saving and parsing again must give the same text.
            std::string text = ComponentToYaml(shifter);
            std::unique_ptr<Component> again = ComponentFromYaml(text);
            ResolveWires(*again);
            Check(ComponentToYaml(*again) == text, "shift register YAML round-trips exactly");
            shifter.dirty = true;  // include it in the save and reload below
        }
    }

    // Buses: bus Input/Output nodes, range wires, and side-by-side wires merging into one.
    {
        static const char* const PassYaml = R"(
name: Bus Pass
nodes:
  - {id: 1, kind: Input, label: A, width: 4}
  - {id: 2, kind: Output, label: Y, width: 4}
wires:
  - 1 -> 2
)";
        static const char* const UseYaml = R"(
name: Bus Use
nodes:
  - {id: 1, kind: Input, label: X, width: 4}
  - {id: 3, component: Bus Pass}
  - {id: 5, kind: Output, label: Q, width: 2}
  - {id: 6, kind: Output, label: Z, width: 2}
  - {id: 7, kind: Output, label: TOP}
wires:
  - 1 -> 3.A0..A3
  - 3.Y2..Y3 -> 5
  - 3.Y0 -> 6.Z0
  - 3.Y1 -> 6.Z1
  - 3.Y3 -> 7
)";
        std::unique_ptr<Component> pass, use;
        try {
            pass = ComponentFromYaml(PassYaml);
            use = ComponentFromYaml(UseYaml);
        } catch (const std::exception& exception) {
            printf("  bus YAML: %s\n", exception.what());
        }
        Check(pass && use, "bus YAML parses");
        if (pass && use) {
            library.push_back(std::move(pass));
            Component& passComponent = *library.back();
            library.push_back(std::move(use));
            Component& useComponent = *library.back();
            Check(ResolveWires(passComponent).empty() && passComponent.wires.size() == 1 &&
                      passComponent.wires[0].width == 4,
                  "a bare bus node id wires all its bits");
            Check(ResolveWires(useComponent).empty() && useComponent.wires.size() == 4,
                  "range wires resolve, and Y0 -> Z0, Y1 -> Z1 merge into one bus wire");
            Check(InputCount(*useComponent.FindNode(3)) == 4 && PinName(*useComponent.FindNode(3), true, 2) == "Y2",
                  "a bus Input gives its user pins A0..A3");

            Simulation simulation;
            BuildSimulation(simulation, useComponent);
            SetSwitch(simulation, PinNodeByLabel(useComponent, NodeKind::Input, "X"), 0b1011);
            Check(ReadBus(simulation, useComponent, "Q", 2) == 0b10 && ReadBus(simulation, useComponent, "Z", 2) == 0b11 &&
                      ReadOutput(simulation, useComponent, "TOP") == 1,
                  "bus wires carry every bit");

            std::string text = ComponentToYaml(useComponent);
            Check(text.find("3.Y0..Y1 -> 6") != std::string::npos && text.find("1 -> 3.A0..A3") != std::string::npos,
                  "bus wires save as ranges");
            std::unique_ptr<Component> again = ComponentFromYaml(text);
            ResolveWires(*again);
            Check(ComponentToYaml(*again) == text, "bus YAML round-trips exactly");
            passComponent.dirty = useComponent.dirty = true;
        }

        std::unique_ptr<Component> bad = ComponentFromYaml("name: Bad Bus\nnodes:\n  - {id: 1, kind: Input, label: A, width: 4}\n"
                                                           "  - {id: 2, kind: Output, label: Y, width: 3}\nwires:\n  - 1 -> 2\n");
        Check(ResolveWires(*bad).size() == 1, "a wire whose ends differ in width is reported");
    }

    // A ROM as a 2-to-4 decoder, the way an opcode decoder EEPROM would be set up.
    {
        static const char* const DecoderYaml = R"(
name: Decoder ROM
nodes:
  - {id: 1, kind: Input, label: S0}
  - {id: 2, kind: Input, label: S1}
  - id: 3
    kind: ROM
    address: 2
    data: 4
    outputs: [Y0, Y1, "", Y3]
    contents:
      - "0000: 1 2 4 8"
  - {id: 4, kind: Output, label: Y0}
  - {id: 5, kind: Output, label: Y1}
  - {id: 6, kind: Output, label: Y2}
  - {id: 7, kind: Output, label: Y3}
  - {id: 8, kind: Input, label: OE}
wires:
  - 8 -> 3./OE
  - 1 -> 3.A0
  - 2 -> 3.A1
  - 3.Y0 -> 4
  - 3.Y1 -> 5
  - 3.D2 -> 6
  - 3.Y3 -> 7
)";
        std::unique_ptr<Component> parsed;
        try {
            parsed = ComponentFromYaml(DecoderYaml);
        } catch (const std::exception& exception) {
            printf("  decoder YAML: %s\n", exception.what());
        }
        Check(parsed != nullptr, "ROM decoder YAML parses");
        if (parsed) {
            library.push_back(std::move(parsed));
            Component& decoder = *library.back();
            Check(ResolveWires(decoder).empty() && decoder.wires.size() == 7, "ROM decoder wires resolve by pin name");

            Simulation simulation;
            BuildSimulation(simulation, decoder);
            bool decodes = true;
            for (int select = 0; select < 4; select++) {
                SetInput(simulation, decoder, "S0", select & 1);
                SetInput(simulation, decoder, "S1", select >> 1);
                for (int line = 0; line < 4; line++)
                    if (ReadOutput(simulation, decoder, ("Y" + std::to_string(line)).c_str()) != (line == select)) decodes = false;
            }
            Check(decodes, "ROM decodes 2 bits to one of 4 lines");
            SetInput(simulation, decoder, "OE", 1);
            Check(ReadLevel(simulation, decoder, "Y3") == Floating && ReadLevel(simulation, decoder, "Y0") == Floating,
                  "ROM outputs let go (Z) when /OE is 1");
            SetInput(simulation, decoder, "OE", 0);

            // Edits to the contents show up in a running simulation.
            const Node* rom = decoder.FindNode(3);
            rom->memory->words[3] = 0x1;
            RecordSettle(simulation);
            Check(ReadOutput(simulation, decoder, "Y0") == 1 && ReadOutput(simulation, decoder, "Y3") == 0,
                  "ROM edits reach a running simulation");
            rom->memory->words[3] = 0x8;

            std::string text = ComponentToYaml(decoder);
            std::unique_ptr<Component> again = ComponentFromYaml(text);
            ResolveWires(*again);
            Check(ComponentToYaml(*again) == text, "ROM YAML round-trips exactly");
            decoder.dirty = true;
        }
    }

    // Two tri-state buffers sharing one wire: a two-device bus.
    Component* bus = CreateComponent("Bus");
    {
        uint32_t a = AddNode(*bus, NodeKind::Input, "", "A");
        uint32_t b = AddNode(*bus, NodeKind::Input, "", "B");
        uint32_t enableA = AddNode(*bus, NodeKind::Input, "", "/OEA");
        uint32_t enableB = AddNode(*bus, NodeKind::Input, "", "/OEB");
        uint32_t driverA = AddNode(*bus, NodeKind::TriState);
        uint32_t driverB = AddNode(*bus, NodeKind::TriState);
        uint32_t line = AddNode(*bus, NodeKind::Output, "", "BUS");
        Connect(*bus, a, 0, driverA, 0);
        Connect(*bus, enableA, 0, driverA, 1);
        Connect(*bus, b, 0, driverB, 0);
        Connect(*bus, enableB, 0, driverB, 1);
        Connect(*bus, driverA, 0, line, 0);
        Connect(*bus, driverB, 0, line, 0);  // a second wire into the same input
        Check(bus->wires.size() == 6, "an input keeps several wires");

        Simulation simulation;
        BuildSimulation(simulation, *bus);
        SetInput(simulation, *bus, "A", 1);
        SetInput(simulation, *bus, "B", 0);
        SetInput(simulation, *bus, "/OEA", 0);
        SetInput(simulation, *bus, "/OEB", 1);
        Check(ReadLevel(simulation, *bus, "BUS") == High && simulation.conflicts == 0, "bus carries A when A drives");
        SetInput(simulation, *bus, "/OEA", 1);
        SetInput(simulation, *bus, "/OEB", 0);
        Check(ReadLevel(simulation, *bus, "BUS") == Low, "bus carries B when B drives");
        SetInput(simulation, *bus, "/OEB", 1);
        Check(ReadLevel(simulation, *bus, "BUS") == Floating, "bus floats (Z) with nothing driving");
        SetInput(simulation, *bus, "/OEA", 0);
        SetInput(simulation, *bus, "/OEB", 0);
        Check(ReadLevel(simulation, *bus, "BUS") == Conflict && simulation.conflicts == 1,
              "two drivers disagreeing are a conflict");
        SetInput(simulation, *bus, "B", 1);
        Check(ReadLevel(simulation, *bus, "BUS") == High && simulation.conflicts == 0, "two drivers agreeing are fine");
    }

    // A RAM on a data bus: tri-state buffers drive DIN onto D when DRIVE is 0, and the RAM's
    // D pins (the same wires) come out to D0 and D1.
    {
        static const char* const RamYaml = R"(
name: RAM Test
nodes:
  - {id: 1, kind: Input, label: A0}
  - {id: 2, kind: Input, label: A1}
  - {id: 3, kind: Input, label: /CE}
  - {id: 4, kind: Input, label: /OE}
  - {id: 5, kind: Input, label: /WE}
  - {id: 6, kind: Input, label: DIN0}
  - {id: 7, kind: Input, label: DIN1}
  - {id: 8, kind: Input, label: DRIVE}
  - {id: 9, kind: TriState}
  - {id: 10, kind: TriState}
  - {id: 11, kind: RAM, address: 2, data: 2}
  - {id: 12, kind: Output, label: D0}
  - {id: 13, kind: Output, label: D1}
wires:
  - 1 -> 11.A0
  - 2 -> 11.A1
  - 3 -> 11./CE
  - 4 -> 11./OE
  - 5 -> 11./WE
  - 6 -> 9.A
  - 7 -> 10.A
  - 8 -> 9./OE
  - 8 -> 10./OE
  - 9.Y -> 11.D0
  - 10.Y -> 11.D1
  - 11.D0 -> 12
  - 11.D1 -> 13
)";
        std::unique_ptr<Component> parsed;
        try {
            parsed = ComponentFromYaml(RamYaml);
        } catch (const std::exception& exception) {
            printf("  RAM YAML: %s\n", exception.what());
        }
        Check(parsed != nullptr, "RAM YAML parses");
        if (parsed) {
            library.push_back(std::move(parsed));
            Component& ramTest = *library.back();
            std::vector<std::string> wireProblems = ResolveWires(ramTest);
            for (const std::string& problem : wireProblems) printf("  %s\n", problem.c_str());
            Check(wireProblems.empty() && ramTest.wires.size() == 13, "RAM wires resolve, D0 on both sides");

            Simulation simulation;  // randomRam is off: RAM starts as zeros
            BuildSimulation(simulation, ramTest);
            auto set = [&](const char* label, int value) { SetInput(simulation, ramTest, label, value); };
            auto level = [&](const char* label) { return ReadLevel(simulation, ramTest, label); };
            // RAM never writes while powering up, though /WE starts low here; it starts as zeros.
            set("/WE", 1);
            set("DRIVE", 1);
            Check(level("D0") == Low && level("D1") == Low, "RAM reads zeros at power-up");
            set("/OE", 1);

            // Write 2 (D1=1, D0=0) at address 1.
            set("A0", 1);
            set("DIN1", 1);
            set("DRIVE", 0);
            set("/WE", 0);
            set("/WE", 1);
            set("DRIVE", 1);
            Check(level("D0") == Floating && level("D1") == Floating, "D floats when nothing drives it");
            set("/OE", 0);
            Check(level("D0") == Low && level("D1") == High, "RAM reads back what was written");
            set("A0", 0);
            Check(level("D1") == Low, "other addresses are untouched");
            set("/CE", 1);
            Check(level("D0") == Floating && level("D1") == Floating, "deselected RAM lets go of D");
            set("/CE", 0);

            // /WE low beats /OE low: the RAM stops driving and stores what's on D (nothing: 0).
            set("A0", 1);
            set("/WE", 0);
            Check(level("D1") == Floating, "RAM doesn't drive D while writing, even with /OE low");
            set("/WE", 1);
            Check(level("D1") == Low, "writing a floating bus stores 0");

            // Reading while the buffers drive something else is a bus conflict.
            set("A0", 0);
            set("A1", 1);
            set("DIN1", 0);
            set("DIN0", 1);
            set("DRIVE", 0);
            set("/WE", 0);  // store 1 at address 2
            set("/WE", 1);  // now the RAM drives 01 alongside the buffers' 01: no conflict
            Check(simulation.conflicts == 0, "RAM and a buffer agreeing is fine");
            set("DIN0", 0);  // the buffers drive 00 while the RAM drives 01
            Check(simulation.conflicts == 1 && level("D0") == Conflict && level("D1") == Low,
                  "RAM and a buffer driving the same wire differently conflict");

            std::string text = ComponentToYaml(ramTest);
            std::unique_ptr<Component> again = ComponentFromYaml(text);
            ResolveWires(*again);
            Check(ComponentToYaml(*again) == text, "RAM YAML round-trips exactly");
            ramTest.dirty = true;
        }
    }

    // The Terminal on a data bus, driven through tri-state buffers like the RAM test.
    {
        Component terminalTest;
        uint32_t select = AddNode(terminalTest, NodeKind::Input, "", "RS");
        uint32_t chipEnable = AddNode(terminalTest, NodeKind::Input, "", "/CE");
        uint32_t outputEnable = AddNode(terminalTest, NodeKind::Input, "", "/OE");
        uint32_t writeEnable = AddNode(terminalTest, NodeKind::Input, "", "/WE");
        uint32_t drive = AddNode(terminalTest, NodeKind::Input, "", "/DRIVE");
        uint32_t terminal = AddNode(terminalTest, NodeKind::Terminal);
        Connect(terminalTest, select, 0, terminal, 0);
        Connect(terminalTest, chipEnable, 0, terminal, 1);
        Connect(terminalTest, outputEnable, 0, terminal, 2);
        Connect(terminalTest, writeEnable, 0, terminal, 3);
        for (int bit = 0; bit < 8; bit++) {
            uint32_t in = AddNode(terminalTest, NodeKind::Input, "", "DIN" + std::to_string(bit));
            uint32_t buffer = AddNode(terminalTest, NodeKind::TriState);
            uint32_t out = AddNode(terminalTest, NodeKind::Output, "", "D" + std::to_string(bit));
            Connect(terminalTest, in, 0, buffer, 0);
            Connect(terminalTest, drive, 0, buffer, 1);
            Connect(terminalTest, buffer, 0, terminal, TerminalControlPins + bit);
            Connect(terminalTest, terminal, bit, out, 0);
        }
        Connect(terminalTest, terminal, TerminalDataBits, AddNode(terminalTest, NodeKind::Output, "", "IRQ"), 0);

        Simulation simulation;
        BuildSimulation(simulation, terminalTest);
        auto set = [&](const char* label, int value) { SetInput(simulation, terminalTest, label, value); };
        auto setByte = [&](int byte) {
            for (int bit = 0; bit < 8; bit++) set(("DIN" + std::to_string(bit)).c_str(), (byte >> bit) & 1);
        };
        auto readByte = [&] {
            int byte = 0;
            for (int bit = 0; bit < 8; bit++) byte |= ReadOutput(simulation, terminalTest, ("D" + std::to_string(bit)).c_str()) << bit;
            return byte;
        };
        // Every switch starts at 0, which is a write cycle in progress: end it, then start clean.
        set("/WE", 1);
        set("/OE", 1);
        simulation.console.clear();

        for (char character : {'H', 'i'}) {
            setByte(character);
            set("/WE", 0);
            set("/WE", 1);
        }
        Check(simulation.console == "Hi", "terminal prints each byte written to RS=0 once");
        set("RS", 1);
        set("/WE", 0);
        set("/WE", 1);
        Check(simulation.console == "Hi", "writing RS=1 (status) prints nothing");

        set("/DRIVE", 1);  // the buffers let go so the terminal can drive D
        simulation.keys = {'a', 'b'};
        RecordSettle(simulation);
        Check(ReadOutput(simulation, terminalTest, "IRQ") == 1, "IRQ is 1 while a key is waiting");
        set("/OE", 0);
        Check(readByte() == 0x03, "status: key waiting, ready to print");
        set("/OE", 1);
        set("RS", 0);
        set("/OE", 0);
        bool first = readByte() == 'a';
        set("/OE", 1);
        set("/OE", 0);
        Check(first && readByte() == 'b', "reads take typed keys in order, one per read");
        set("/OE", 1);
        Check(simulation.keys.empty() && ReadOutput(simulation, terminalTest, "IRQ") == 0, "IRQ drops once keys are read");
        set("RS", 1);
        set("/OE", 0);
        Check(readByte() == 0x02, "status: nothing waiting");
        set("/CE", 1);
        Check(ReadLevel(simulation, terminalTest, "D0") == Floating, "deselected terminal lets go of D");
    }

    // Recursion guard.
    Check(Uses(*fullAdder, "Half Adder"), "full adder uses half adder");
    Check(!Uses(*halfAdder, "Full Adder"), "half adder doesn't use full adder");

    // Save, reload and test again. Also renames a used component on the way.
    componentsDirectory = (fs::temp_directory_path() / "isa16-selftest/").string();
    std::error_code error;
    fs::remove_all(componentsDirectory, error);
    RenameComponent(*halfAdder, "HA");
    Check(SaveAll() == 0, "save all");
    library.clear();
    std::vector<std::string> problems = LoadLibrary();
    for (const std::string& problem : problems) printf("  %s\n", problem.c_str());
    Check(problems.empty() && library.size() == 11, "reload all eleven components");
    Check(FindComponent("HA") && FindComponent("Full Adder"), "renamed reference survives the round trip");
    if (FindComponent("Full Adder")) TestFullAdder("Full adder (reloaded)");
    if (Component* decoder = FindComponent("Decoder ROM")) {
        const Node* rom = decoder->FindNode(3);
        Check(rom && rom->memory->words == std::vector<uint32_t>({1, 2, 4, 8}) && PinName(*rom, true, 3) == "Y3",
              "ROM contents and pin names survive the round trip");
    }

    // A ROM can take its contents from a binary file, such as an assembled program.
    {
        MemoryData image;
        image.Resize(2, 16);
        image.words = {0x1234, 0xBEEF, 0, 7};
        Check(SaveRomImage(image, componentsDirectory + "program.bin"), "write a ROM image");
        MemoryData linked;
        linked.Resize(1, 16);
        linked.image = "program.bin";
        std::string problem = LoadRomImage(linked);
        Check(linked.words == std::vector<uint32_t>({0x1234, 0xBEEF}) && !problem.empty(),
              "ROM image loads little-endian words, and says when it doesn't all fit");

        // The same file preloads a RAM when its simulation starts.
        Component machine;
        uint32_t ramNode = AddNode(machine, NodeKind::Ram);
        MemoryData& ram = *machine.FindNode(ramNode)->memory;
        ram.Resize(2, 16);
        Connect(machine, AddNode(machine, NodeKind::High), 0, ramNode, 2 + 2);  // /WE high: never write
        ram.image = "program.bin";
        Check(LoadRomImage(ram).empty(), "RAM preload image loads");
        Simulation simulation;
        simulation.randomRam = true;  // preloaded words win over random ones
        BuildSimulation(simulation, machine);
        Check(simulation.rams.size() == 1 && simulation.rams[0].words == std::vector<uint32_t>({0x1234, 0xBEEF, 0, 7}),
              "RAM starts out holding its preload image");

    }
    fs::remove_all(componentsDirectory, error);

    printf(testFailures ? "%d check(s) FAILED\n" : "All checks passed\n", testFailures);
    return testFailures ? 1 : 0;
}

// ===========================================================================
// Editor
// ===========================================================================

enum class Mode { Edit, Simulate };

struct App {
    Component* current = nullptr;
    Mode mode = Mode::Edit;
    Simulation simulation;

    std::string status;
    double statusUntil = 0;

    ImVec2 canvasCenter{400, 300};
    ImVec2 popupScreenPosition{0, 0};
    int spawnCount = 0;
    int hoveredNode = -1;
    bool showMiniMap = true;
    bool quitRequested = false;
    char typeBuffer[256] = "";
    bool quit = false;

    char nameBuffer[MaxNameLength] = "";
    const Component* nameBufferOwner = nullptr;
    char labelBuffer[MaxNameLength] = "";
    uint32_t labelBufferNode = 0;
    char descriptionBuffer[1024] = "";
    const Component* descriptionBufferOwner = nullptr;

    // Side panel widths; the splitters between panels and canvas change them.
    float paletteWidth = 220, propertiesWidth = 290, simulationWidth = 300;
    ImNodesStyle baseNodeStyle;  // imnodes sizes at zoom 1

    // ROM contents window
    bool romEditorOpen = false;
    Component* romComponent = nullptr;
    uint32_t romNode = 0;
    int romView = 0;  // 0 hex, 1 bits
    bool romFollow = true;
    int romLastAddress = -1;
    char romPath[260] = "";
};

static App app;

static void SetStatus(const char* format, ...)
{
    char text[256];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(text, sizeof text, format, arguments);
    va_end(arguments);
    app.status = text;
    app.statusUntil = GetTime() + 4.0;
}

// Pin attribute ids for imnodes: unique per pin within a component.
static int PinId(uint32_t node, bool output, int index)
{
    return static_cast<int>(node) * (2 * MaxPinsPerSide) + (output ? MaxPinsPerSide : 0) + index;
}

// A bus pin (one pin standing for a group) has the negative of its first bit's id, less one.
static int BusPinId(uint32_t node, bool output, int first) { return -1 - PinId(node, output, first); }

static void DecodePinId(int pinId, uint32_t& node, bool& output, int& index, bool* bus = nullptr)
{
    if (bus) *bus = pinId < 0;
    if (pinId < 0) pinId = -1 - pinId;
    node = static_cast<uint32_t>(pinId / (2 * MaxPinsPerSide));
    output = (pinId % (2 * MaxPinsPerSide)) >= MaxPinsPerSide;
    index = pinId % MaxPinsPerSide;
}

// Pins with a one-bit wire on them, by PinId. A bus with any of these shows its bits.
static std::unordered_set<int> singleWirePins;

static void FindSingleWirePins(const Component& component)
{
    singleWirePins.clear();
    for (const Wire& wire : component.wires) {
        if (wire.width != 1) continue;
        singleWirePins.insert(PinId(wire.fromNode, true, static_cast<int>(wire.fromPin)));
        singleWirePins.insert(PinId(wire.toNode, false, static_cast<int>(wire.toPin)));
    }
}

// Is this group drawn as one bus pin only? Otherwise its bits are listed under the bus pin.
static bool BusCollapsed(const Component& component, uint32_t node, bool output, const PinGroup& group)
{
    if (group.count < 2 || component.expandedBuses.count(BusPinId(node, output, group.first))) return false;
    for (int pin = group.first; pin < group.first + group.count; pin++)
        if (singleWirePins.count(PinId(node, output, pin))) return false;
    return true;
}

// The imnodes attribute a wire end attaches to: the bus pin for a bus wire on a collapsed group
// or one spanning its whole group, else the first bit's own pin.
static int WireEndAttribute(const Component& component, const Node& node, bool output, int pin, int width)
{
    PinGroup group = GroupOfPin(PinGroups(node, output), pin);
    if (group.count > 1 && (BusCollapsed(component, node.id, output, group) ||
                            (width > 1 && pin == group.first && width == group.count)))
        return BusPinId(node.id, output, group.first);
    return PinId(node.id, output, pin);
}

// Several bits as hex: "Z" when nothing drives any of them, "?" for a digit with a bit that's
// floating or in conflict.
static std::string BusText(const Simulation& simulation, const std::vector<int>& nets)
{
    if (std::all_of(nets.begin(), nets.end(), [&](int net) { return simulation.Level(net) == Floating; })) return "Z";
    std::string text;
    for (int digit = (static_cast<int>(nets.size()) + 3) / 4 - 1; digit >= 0; digit--) {
        int value = 0;
        bool known = true;
        for (int bit = 0; bit < 4 && digit * 4 + bit < static_cast<int>(nets.size()); bit++) {
            uint8_t level = simulation.Level(nets[static_cast<size_t>(digit * 4 + bit)]);
            if (level == High) value |= 1 << bit;
            else if (level != Low) known = false;
        }
        text += known ? "0123456789ABCDEF"[value] : '?';
    }
    return text;
}

static uint8_t BusLevel(const Simulation& simulation, const std::vector<int>& nets)
{
    bool anyHigh = false, allFloating = true;
    for (int net : nets) {
        uint8_t level = simulation.Level(net);
        if (level == Conflict) return Conflict;
        anyHigh |= level == High;
        allFloating &= level == Floating;
    }
    return allFloating ? Floating : anyHigh ? High : Low;
}

static std::vector<int> PinNets(const Simulation& simulation, uint32_t node, bool output, int first, int count)
{
    std::vector<int> nets;
    for (int pin = first; pin < first + count; pin++)
        nets.push_back(output ? simulation.NetOfOutput(node, pin) : simulation.NetOfInput(node, pin));
    return nets;
}

static void EnsureEditor(Component& component)
{
    if (!component.editor) {
        component.editor = ImNodes::EditorContextCreate();
        for (Node& node : component.nodes) node.placement = Node::Placement::Grid;
    }
}

static void OpenComponent(Component* component)
{
    if (!component || component == app.current) return;
    app.current = component;
    EnsureEditor(*component);
    app.hoveredNode = -1;
}

static void NewComponent()
{
    Component* component = CreateComponent(UniqueComponentName("Untitled"));
    OpenComponent(component);
    SetStatus("Created '%s'", component->name.c_str());
}

static void SaveAllWithStatus()
{
    int failures = SaveAll();
    if (failures) SetStatus("Couldn't save %d component(s) to %s", failures, componentsDirectory.c_str());
    else SetStatus("Saved to %s", componentsDirectory.c_str());
}

static void EnterSimulation()
{
    PruneWires(*app.current);
    BuildSimulation(app.simulation, *app.current);
    app.simulation.running = false;
    app.mode = Mode::Simulate;
}

static void ToggleMode()
{
    if (app.mode == Mode::Edit) EnterSimulation();
    else app.mode = Mode::Edit;
}

// Places a new node from the palette or the right-click menu.
static void PlaceNode(NodeKind kind, const std::string& typeName, ImVec2 screenPosition)
{
    uint32_t id = AddNode(*app.current, kind, typeName);
    Node* node = app.current->FindNode(id);
    node->placement = Node::Placement::Screen;
    node->position = screenPosition;
}

static void PlaceNodeAtCenter(NodeKind kind, const std::string& typeName = "")
{
    float offset = static_cast<float>(app.spawnCount++ % 8) * 18.0f;
    PlaceNode(kind, typeName, ImVec2(app.canvasCenter.x - 60 + offset, app.canvasCenter.y - 40 + offset));
}

static bool CanPlace(const Component& component)
{
    return &component != app.current && !Uses(component, app.current->name);
}

// ---------------------------------------------------------------------------
// Colors

static const ImU32 ColorHigh = IM_COL32(90, 230, 110, 255);
static const ImU32 ColorLow = IM_COL32(70, 95, 80, 255);
static const ImU32 ColorFloating = IM_COL32(90, 110, 170, 255);
static const ImU32 ColorConflict = IM_COL32(255, 60, 60, 255);

static ImU32 LevelColor(uint8_t level)
{
    switch (level) {
    case High:     return ColorHigh;
    case Floating: return ColorFloating;
    case Conflict: return ColorConflict;
    default:       return ColorLow;
    }
}

static const char* LevelText(uint8_t level)
{
    static const char* const Texts[] = {"0", "1", "Z", "X (conflict)"};
    return Texts[level & 3];
}

static ImU32 TitleColor(const Node& node)
{
    switch (node.kind) {
    case NodeKind::Input:     return IM_COL32(40, 120, 70, 255);
    case NodeKind::Output:    return IM_COL32(150, 60, 50, 255);
    case NodeKind::Component: return FindComponent(node.typeName) ? IM_COL32(50, 85, 150, 255) : IM_COL32(170, 40, 40, 255);
    case NodeKind::Clock:     return IM_COL32(150, 110, 30, 255);
    case NodeKind::FlipFlop:  return IM_COL32(110, 70, 140, 255);
    default:                  return IM_COL32(75, 75, 85, 255);
    }
}

static ImU32 Lighten(ImU32 color, int amount)
{
    int r = std::min(255, static_cast<int>((color >> IM_COL32_R_SHIFT) & 0xFF) + amount);
    int g = std::min(255, static_cast<int>((color >> IM_COL32_G_SHIFT) & 0xFF) + amount);
    int b = std::min(255, static_cast<int>((color >> IM_COL32_B_SHIFT) & 0xFF) + amount);
    return IM_COL32(r, g, b, 255);
}

// ---------------------------------------------------------------------------
// Canvas

static void DrawLed(bool on)
{
    float radius = ImGui::GetTextLineHeight() * 0.5f;
    ImVec2 corner = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(radius * 2, radius * 2));
    ImVec2 center(corner.x + radius, corner.y + radius);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddCircleFilled(center, radius, on ? IM_COL32(255, 70, 50, 255) : IM_COL32(70, 25, 25, 255));
    if (on) drawList->AddCircle(center, radius + 2, IM_COL32(255, 120, 90, 120), 0, 2.0f);
}

static void DrawNode(Node& node)
{
    Component& component = *app.current;
    Simulation& simulation = app.simulation;
    bool simulating = app.mode == Mode::Simulate;

    // Positions are kept at zoom 1, so they're handed to imnodes scaled every frame.
    float zoom = component.zoom;
    if (node.placement == Node::Placement::Screen) ImNodes::SetNodeScreenSpacePos(static_cast<int>(node.id), node.position);
    else ImNodes::SetNodeGridSpacePos(static_cast<int>(node.id), ImVec2(node.position.x * zoom, node.position.y * zoom));
    node.placement = Node::Placement::None;

    ImU32 title = TitleColor(node);
    ImNodes::PushColorStyle(ImNodesCol_TitleBar, title);
    ImNodes::PushColorStyle(ImNodesCol_TitleBarHovered, Lighten(title, 25));
    ImNodes::PushColorStyle(ImNodesCol_TitleBarSelected, Lighten(title, 45));

    ImNodes::BeginNode(static_cast<int>(node.id));

    std::string titleText = NodeTitle(node);
    ImNodes::BeginNodeTitleBar();
    ImGui::TextUnformatted(titleText.c_str());
    ImNodes::EndNodeTitleBar();

    int inputs = InputCount(node), outputs = OutputCount(node);
    float width = ImGui::CalcTextSize(titleText.c_str()).x;
    for (int index = 0; index < inputs; index++)
        width = std::max(width, ImGui::CalcTextSize(PinName(node, false, index).c_str()).x + 30 * zoom);
    for (int index = 0; index < outputs; index++)
        width = std::max(width, ImGui::CalcTextSize(PinName(node, true, index).c_str()).x + 30 * zoom);
    for (bool output : {false, true}) {
        for (const PinGroup& group : PinGroups(node, output)) {
            if (group.count < 2) continue;
            std::string name = PinName(node, output, group.first) + ".." + PinName(node, output, group.first + group.count - 1);
            if (simulating) name += " = " + std::string(static_cast<size_t>((group.count + 3) / 4), '0');
            width = std::max(width, ImGui::CalcTextSize(name.c_str()).x + 40 * zoom);
        }
    }
    width = std::max(width, 60.0f * zoom);

    if (simulating && node.kind == NodeKind::Input && node.width == 1) {
        uint32_t value = SwitchValue(simulation, node.id);
        ImGui::PushID(static_cast<int>(node.id));
        ImGui::PushStyleColor(ImGuiCol_Button, value ? ColorHigh : ColorLow);
        if (ImGui::Button(value ? " 1 " : " 0 ")) SetSwitch(simulation, node.id, !value);
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
    if (simulating && node.kind == NodeKind::Input && node.width > 1) {
        uint32_t value = SwitchValue(simulation, node.id);
        ImGui::PushID(static_cast<int>(node.id));
        ImGui::SetNextItemWidth(ImGui::CalcTextSize("00000000").x + 10 * zoom);
        if (ImGui::InputScalar("##value", ImGuiDataType_U32, &value, nullptr, nullptr, "%X",
                               ImGuiInputTextFlags_CharsHexadecimal))
            SetSwitch(simulation, node.id, value);
        ImGui::PopID();
    }
    if (simulating && node.kind == NodeKind::Output && node.width == 1) {
        DrawLed(simulation.Value(simulation.NetOfInput(node.id, 0)) != 0);
    }
    if (simulating && node.kind == NodeKind::Output && node.width > 1) {
        ImGui::Text("%s", BusText(simulation, PinNets(simulation, node.id, false, 0, node.width)).c_str());
    }
    if (simulating && IsMemory(node.kind)) {
        size_t address = 0;
        for (int bit = 0; bit < node.memory->addressBits; bit++)
            address |= static_cast<size_t>(simulation.Value(simulation.NetOfInput(node.id, bit))) << bit;
        const std::vector<uint32_t>* words = &node.memory->words;
        if (node.kind == NodeKind::Ram) {
            auto found = simulation.topRams.find(node.id);
            words = found == simulation.topRams.end() ? nullptr : &simulation.rams[static_cast<size_t>(found->second)].words;
        }
        if (words) ImGui::TextDisabled("[%zX] = %X", address, static_cast<unsigned>((*words)[address]));
    }

    // One pin. A bus pin is square, and has a +/- to show or hide its bits.
    auto drawPin = [&](bool output, int first, int count, bool bus, bool collapsed) {
        int attribute = bus ? BusPinId(node.id, output, first) : PinId(node.id, output, first);
        std::string name = PinName(node, output, first);
        if (bus) name += ".." + PinName(node, output, first + count - 1);
        if (simulating) {
            std::vector<int> nets = PinNets(simulation, node.id, output, first, count);
            ImU32 color = LevelColor(BusLevel(simulation, nets));
            ImNodes::PushColorStyle(ImNodesCol_Pin, color);
            ImNodes::PushColorStyle(ImNodesCol_PinHovered, color);
            if (bus) name += " = " + BusText(simulation, nets);
        }
        ImNodesPinShape shape = bus ? ImNodesPinShape_QuadFilled : ImNodesPinShape_CircleFilled;
        if (output) ImNodes::BeginOutputAttribute(attribute, shape);
        else ImNodes::BeginInputAttribute(attribute, shape);
        float buttonWidth = 0;
        if (bus) {
            bool forced = !collapsed && !component.expandedBuses.count(attribute);  // single wires on its bits
            ImGui::PushID(attribute);
            ImGui::BeginDisabled(forced);
            if (!output && ImGui::SmallButton(collapsed ? "+" : "-")) {
                if (collapsed) component.expandedBuses.insert(attribute);
                else component.expandedBuses.erase(attribute);
            }
            if (!output) ImGui::SameLine();
            if (output) buttonWidth = ImGui::CalcTextSize("+").x + ImGui::GetStyle().FramePadding.x * 2 +
                                      ImGui::GetStyle().ItemSpacing.x;
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        if (output) {
            float indent = std::max(0.0f, width - ImGui::CalcTextSize(name.c_str()).x - buttonWidth);
            ImGui::Indent(indent);
            ImGui::TextUnformatted(name.c_str());
            ImGui::Unindent(indent);
            if (bus) {
                ImGui::SameLine();
                ImGui::PushID(attribute);
                ImGui::BeginDisabled(!collapsed && !component.expandedBuses.count(attribute));
                if (ImGui::SmallButton(collapsed ? "+" : "-")) {
                    if (collapsed) component.expandedBuses.insert(attribute);
                    else component.expandedBuses.erase(attribute);
                }
                ImGui::EndDisabled();
                ImGui::PopID();
            }
        } else {
            ImGui::TextUnformatted(name.c_str());
        }
        if (output) ImNodes::EndOutputAttribute();
        else ImNodes::EndInputAttribute();
        if (simulating) {
            ImNodes::PopColorStyle();
            ImNodes::PopColorStyle();
        }
    };
    for (bool output : {false, true}) {
        for (const PinGroup& group : PinGroups(node, output)) {
            bool collapsed = BusCollapsed(component, node.id, output, group);
            if (group.count > 1) drawPin(output, group.first, group.count, true, collapsed);
            if (collapsed) continue;
            if (group.count > 1) ImGui::Indent(10 * zoom);
            for (int pin = group.first; pin < group.first + group.count; pin++) drawPin(output, pin, 1, false, false);
            if (group.count > 1) ImGui::Unindent(10 * zoom);
        }
    }
    if (inputs == 0 && outputs == 0) ImGui::Dummy(ImVec2(width, 1));

    ImNodes::EndNode();
    ImNodes::PopColorStyle();
    ImNodes::PopColorStyle();
    ImNodes::PopColorStyle();
}

static void DrawAddNodeMenu(ImVec2 screenPosition, bool atCenter)
{
    auto place = [&](NodeKind kind, const std::string& typeName) {
        if (atCenter) PlaceNodeAtCenter(kind, typeName);
        else PlaceNode(kind, typeName, screenPosition);
    };

    ImGui::SeparatorText("Pins");
    for (NodeKind kind : {NodeKind::Input, NodeKind::Output}) {
        if (ImGui::MenuItem(Primitives[static_cast<int>(kind)].name)) place(kind, "");
        ImGui::SetItemTooltip("%s", Primitives[static_cast<int>(kind)].description);
    }
    ImGui::SeparatorText("Gates");
    for (NodeKind kind : {NodeKind::Not, NodeKind::And, NodeKind::Or, NodeKind::Nand, NodeKind::Nor, NodeKind::Xor,
                          NodeKind::Xnor, NodeKind::Buffer, NodeKind::TriState}) {
        if (ImGui::MenuItem(Primitives[static_cast<int>(kind)].name)) place(kind, "");
        ImGui::SetItemTooltip("%s", Primitives[static_cast<int>(kind)].description);
    }
    ImGui::SeparatorText("Memory and timing");
    for (NodeKind kind : {NodeKind::FlipFlop, NodeKind::Rom, NodeKind::Ram, NodeKind::Clock, NodeKind::Low,
                          NodeKind::High}) {
        if (ImGui::MenuItem(Primitives[static_cast<int>(kind)].name)) place(kind, "");
        ImGui::SetItemTooltip("%s", Primitives[static_cast<int>(kind)].description);
    }
    ImGui::SeparatorText("Input/output");
    if (ImGui::MenuItem(Primitives[static_cast<int>(NodeKind::Terminal)].name)) place(NodeKind::Terminal, "");
    ImGui::SetItemTooltip("%s", Primitives[static_cast<int>(NodeKind::Terminal)].description);
    ImGui::SeparatorText("Components");
    bool any = false;
    for (auto& component : library) {
        if (component.get() == app.current) continue;
        any = true;
        bool allowed = CanPlace(*component);
        if (ImGui::MenuItem(component->name.c_str(), nullptr, false, allowed)) place(NodeKind::Component, component->name);
        if (!allowed) ImGui::SetItemTooltip("It uses '%s', so it can't go inside it", app.current->name.c_str());
        else if (!component->description.empty()) ImGui::SetItemTooltip("%s", component->description.c_str());
    }
    if (!any) ImGui::TextDisabled("(none yet)");
}

// ---------------------------------------------------------------------------
// ROM and RAM contents window

static void OpenRomEditor(uint32_t nodeId)
{
    Node* node = app.current->FindNode(nodeId);
    if (!node || !IsMemory(node->kind)) return;
    app.romEditorOpen = true;
    app.romComponent = app.current;
    app.romNode = nodeId;
    app.romLastAddress = -1;
    snprintf(app.romPath, sizeof app.romPath, "%s", node->memory->image.c_str());
    ImGui::SetWindowFocus("###RomEditor");
}

// The address on a ROM's pins in the running simulation, or -1 when it isn't being simulated.
static int RomAddressNow(const Node& node)
{
    if (app.mode != Mode::Simulate || app.romComponent != app.current) return -1;
    int address = 0;
    for (int bit = 0; bit < node.memory->addressBits; bit++)
        address |= app.simulation.Value(app.simulation.NetOfInput(node.id, bit)) << bit;
    return address;
}

// The words the window shows: a ROM's contents, or a simulated RAM's (nullptr when not simulating it).
static std::vector<uint32_t>* MemoryWords(Node& node)
{
    if (node.kind == NodeKind::Rom) return &node.memory->words;
    if (app.mode != Mode::Simulate || app.romComponent != app.current) return nullptr;
    auto found = app.simulation.topRams.find(node.id);
    return found == app.simulation.topRams.end() ? nullptr : &app.simulation.rams[static_cast<size_t>(found->second)].words;
}

static void RomContentsChanged(Component& component, const Node& node)
{
    if (node.kind == NodeKind::Rom) component.dirty = true;  // a RAM's live contents aren't saved
    if (app.mode == Mode::Simulate) RecordSettle(app.simulation);  // the simulation sees the change
}

static void DrawRamToolbar(Component& component, Node& node, std::vector<uint32_t>* live)
{
    MemoryData& ram = *node.memory;
    ImGui::SetNextItemWidth(std::max(160.0f, ImGui::GetContentRegionAvail().x - 260));
    ImGui::InputTextWithHint("##path", "preload .bin, relative to the Components folder", app.romPath, sizeof app.romPath);
    ImGui::SetItemTooltip("For example ../Binary/Programs/Fibonacci.bin, so the RAM starts out holding\n"
                          "the same program the emulator runs. Loaded when the simulation starts or resets.");
    std::string path = Trim(app.romPath);
    ImGui::SameLine();
    ImGui::BeginDisabled(path.empty());
    if (ImGui::Button("Preload")) {
        ram.image = path;
        std::string problem = LoadRomImage(ram);
        component.dirty = true;
        if (app.mode == Mode::Simulate) BuildSimulation(app.simulation, *app.current);
        SetStatus("%s", problem.empty() ? ("RAM now starts out holding " + path).c_str() : problem.c_str());
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!live);
    if (ImGui::Button("Export") && live) {
        MemoryData copy = ram;
        copy.words = *live;
        bool ok = SaveRomImage(copy, (fs::path(componentsDirectory) / path).string());
        SetStatus(ok ? "Wrote %s" : "Couldn't write %s", path.c_str());
    }
    ImGui::SetItemTooltip("Write what the RAM holds right now to this file");
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    if (!ram.image.empty()) {
        ImGui::TextColored(ImVec4(0.55f, 0.75f, 1, 1), "Preloaded from %s", ram.image.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Don't preload")) {
            ram.image.clear();
            component.dirty = true;
        }
    } else {
        ImGui::TextDisabled("Starts out holding %s", app.simulation.randomRam ? "random bytes" : "zeros");
    }
    if (!live) {
        ImGui::TextWrapped("A RAM's contents exist only while it is being simulated. Simulate this component (F5) "
                           "to see and edit them.");
    } else {
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear all")) {
            std::fill(live->begin(), live->end(), 0);
            RomContentsChanged(component, node);
        }
    }
}

static void DrawRomToolbar(Component& component, Node& node)
{
    MemoryData& rom = *node.memory;
    bool linked = !rom.image.empty();

    ImGui::SetNextItemWidth(std::max(160.0f, ImGui::GetContentRegionAvail().x - 330));
    ImGui::InputTextWithHint("##path", "binary file, relative to the Components folder", app.romPath, sizeof app.romPath);
    ImGui::SetItemTooltip("For example ../Binary/Programs/Fibonacci.bin\nWords are little-endian, %d byte(s) each at this width.",
                          RomBytesPerWord(rom));
    std::string path = Trim(app.romPath);
    ImGui::BeginDisabled(path.empty());
    ImGui::SameLine();
    if (ImGui::Button("Link")) {
        rom.image = path;
        std::string problem = LoadRomImage(rom);
        SetStatus("%s", problem.empty() ? ("Contents now come from " + path).c_str() : problem.c_str());
        RomContentsChanged(component, node);
    }
    ImGui::SetItemTooltip("Read the contents from this file every time the component loads (good for programs)");
    ImGui::SameLine();
    if (ImGui::Button("Import")) {
        rom.image = path;
        std::string problem = LoadRomImage(rom);
        rom.image.clear();  // the words are now this ROM's own
        SetStatus("%s", problem.empty() ? ("Imported " + path).c_str() : problem.c_str());
        RomContentsChanged(component, node);
    }
    ImGui::SetItemTooltip("Copy the file's words into this ROM, to be saved with the component");
    ImGui::SameLine();
    if (ImGui::Button("Export")) {
        bool ok = SaveRomImage(rom, (fs::path(componentsDirectory) / path).string());
        SetStatus(ok ? "Wrote %s" : "Couldn't write %s", path.c_str());
    }
    ImGui::SetItemTooltip("Write the contents to this file, e.g. to program a real EEPROM with");
    ImGui::EndDisabled();

    if (linked) {
        ImGui::TextColored(ImVec4(0.55f, 0.75f, 1, 1), "From %s", rom.image.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Reload")) {
            std::string problem = LoadRomImage(rom);
            SetStatus("%s", problem.empty() ? "Reloaded" : problem.c_str());
            RomContentsChanged(component, node);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Unlink")) {
            rom.image.clear();
            RomContentsChanged(component, node);
        }
        ImGui::SetItemTooltip("Keep the current words as this ROM's own, editable contents");
    } else {
        ImGui::TextDisabled("Contents are saved in the component file");
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear all")) {
            std::fill(rom.words.begin(), rom.words.end(), 0);
            RomContentsChanged(component, node);
        }
    }

    if (ImGui::CollapsingHeader("Pin names")) {
        auto nameEditor = [&](const char* id, std::vector<std::string>& names, const char* prefix) {
            if (!ImGui::BeginTable(id, 4, ImGuiTableFlags_SizingStretchSame)) return;
            for (size_t bit = 0; bit < names.size(); bit++) {
                ImGui::TableNextColumn();
                char buffer[MaxNameLength];
                snprintf(buffer, sizeof buffer, "%s", names[bit].c_str());
                std::string hint = prefix + std::to_string(bit);
                ImGui::PushID(static_cast<int>(bit));
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputTextWithHint("##name", hint.c_str(), buffer, sizeof buffer)) {
                    names[bit] = Trim(buffer);
                    component.dirty = true;
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        };
        ImGui::TextDisabled("Outputs (data)");
        nameEditor("data names", rom.dataNames, "D");
        ImGui::TextDisabled("Inputs (address)");
        nameEditor("address names", rom.addressNames, "A");
    }
}

static void DrawRomHexView(Component& component, Node& node, std::vector<uint32_t>& words, int current,
                           bool scrollToCurrent)
{
    MemoryData& rom = *node.memory;
    int perRow = rom.dataBits > 16 ? 8 : 16;
    int wordDigits = (rom.dataBits + 3) / 4, addressDigits = std::max(4, (rom.addressBits + 3) / 4);
    char wordFormat[8];
    snprintf(wordFormat, sizeof wordFormat, "%%0%dX", wordDigits);
    float cellWidth = ImGui::CalcTextSize("F").x * static_cast<float>(wordDigits) + ImGui::GetStyle().FramePadding.x * 2;
    int rows = static_cast<int>((words.size() + perRow - 1) / perRow);

    if (!ImGui::BeginTable("hex", perRow + 1,
                           ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit))
        return;
    ImGui::TableSetupScrollFreeze(1, 1);
    ImGui::TableSetupColumn("Address");
    for (int column = 0; column < perRow; column++) {
        char label[8];
        snprintf(label, sizeof label, "%X", column);
        ImGui::TableSetupColumn(label);
    }
    ImGui::TableHeadersRow();

    ImGuiListClipper clipper;
    clipper.Begin(rows);
    if (scrollToCurrent) clipper.IncludeItemByIndex(current / perRow);
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%0*X", addressDigits, row * perRow);
            if (scrollToCurrent && row == current / perRow) ImGui::SetScrollHereY(0.5f);
            for (int column = 0; column < perRow; column++) {
                size_t address = static_cast<size_t>(row * perRow + column);
                ImGui::TableNextColumn();
                if (static_cast<int>(address) == current) ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, IM_COL32(60, 140, 70, 200));
                uint32_t value = words[address];
                ImGui::PushID(static_cast<int>(address));
                ImGui::SetNextItemWidth(cellWidth);
                if (ImGui::InputScalar("##word", ImGuiDataType_U32, &value, nullptr, nullptr, wordFormat,
                                       ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_AutoSelectAll)) {
                    words[address] = value & rom.DataMask();
                    RomContentsChanged(component, node);
                }
                ImGui::PopID();
            }
        }
    }
    ImGui::EndTable();
}

// One row per address and one checkbox column per output: the easy way to fill in a decoder.
static void DrawRomBitsView(Component& component, Node& node, std::vector<uint32_t>& words, int current,
                            bool scrollToCurrent)
{
    MemoryData& rom = *node.memory;
    int wordDigits = (rom.dataBits + 3) / 4, addressDigits = std::max(4, (rom.addressBits + 3) / 4);

    if (!ImGui::BeginTable("bits", rom.dataBits + 3,
                           ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit |
                               ImGuiTableFlags_HighlightHoveredColumn))
        return;
    ImGui::TableSetupScrollFreeze(3, 1);
    ImGui::TableSetupColumn("Address");
    ImGui::TableSetupColumn("Address bits");
    ImGui::TableSetupColumn("Word");
    for (int bit = rom.dataBits - 1; bit >= 0; bit--)
        ImGui::TableSetupColumn(PinName(node, true, bit).c_str());
    ImGui::TableHeadersRow();

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(words.size()));
    if (scrollToCurrent) clipper.IncludeItemByIndex(current);
    while (clipper.Step()) {
        for (int address = clipper.DisplayStart; address < clipper.DisplayEnd; address++) {
            ImGui::TableNextRow();
            if (address == current) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, IM_COL32(60, 140, 70, 200));
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%0*X", addressDigits, address);
            if (scrollToCurrent && address == current) ImGui::SetScrollHereY(0.5f);
            ImGui::TableNextColumn();
            char binary[MaxMemoryAddressBits + MaxMemoryAddressBits / 4 + 1];
            int length = 0;
            for (int bit = rom.addressBits - 1; bit >= 0; bit--) {
                binary[length++] = (address >> bit) & 1 ? '1' : '0';
                if (bit && bit % 4 == 0) binary[length++] = ' ';
            }
            binary[length] = 0;
            ImGui::TextUnformatted(binary);
            ImGui::TableNextColumn();
            ImGui::Text("%0*X", wordDigits, static_cast<unsigned>(words[static_cast<size_t>(address)]));
            ImGui::PushID(address);
            for (int bit = rom.dataBits - 1; bit >= 0; bit--) {
                ImGui::TableNextColumn();
                bool on = (words[static_cast<size_t>(address)] >> bit) & 1;
                ImGui::PushID(bit);
                if (ImGui::Checkbox("##bit", &on)) {
                    words[static_cast<size_t>(address)] ^= 1u << bit;
                    RomContentsChanged(component, node);
                }
                ImGui::PopID();
            }
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

static void DrawRomEditor()
{
    if (!app.romEditorOpen) return;
    bool exists = std::any_of(library.begin(), library.end(),
                              [](const std::unique_ptr<Component>& component) { return component.get() == app.romComponent; });
    Node* node = exists ? app.romComponent->FindNode(app.romNode) : nullptr;
    if (!node || !IsMemory(node->kind)) {
        app.romEditorOpen = false;
        return;
    }
    Component& component = *app.romComponent;
    MemoryData& rom = *node->memory;

    ImGui::SetNextWindowSize(ImVec2(760, 560), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(1.0f);
    bool ram = node->kind == NodeKind::Ram;
    std::vector<uint32_t>* words = MemoryWords(*node);
    std::string title = (ram ? "RAM " : "ROM ") + std::to_string(rom.words.size()) + " x " + std::to_string(rom.dataBits) +
                        " in " + component.name + "###RomEditor";
    if (!ImGui::Begin(title.c_str(), &app.romEditorOpen)) {
        ImGui::End();
        return;
    }

    if (ram) DrawRamToolbar(component, *node, words);
    else DrawRomToolbar(component, *node);

    ImGui::RadioButton("Hex", &app.romView, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Bits", &app.romView, 1);
    ImGui::SetItemTooltip("One row per address, one column per output: for control/decoder ROMs");
    int current = RomAddressNow(*node);
    if (current >= 0 && words) {
        ImGui::SameLine();
        ImGui::Checkbox("Follow the address", &app.romFollow);
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.4f, 0.95f, 0.5f, 1), "Address [%X] = %X", current,
                           static_cast<unsigned>((*words)[static_cast<size_t>(current)]));
    }
    bool scrollToCurrent = current >= 0 && app.romFollow && current != app.romLastAddress;
    app.romLastAddress = current;

    if (words) {
        ImGui::BeginDisabled(!ram && !rom.image.empty());
        if (app.romView == 0) DrawRomHexView(component, *node, *words, current, scrollToCurrent);
        else DrawRomBitsView(component, *node, *words, current, scrollToCurrent);
        ImGui::EndDisabled();
    }

    ImGui::End();
}

static void DeleteSelection()
{
    Component& component = *app.current;
    int linkCount = ImNodes::NumSelectedLinks();
    if (linkCount > 0) {
        std::vector<int> links(static_cast<size_t>(linkCount));
        ImNodes::GetSelectedLinks(links.data());
        for (int link : links) RemoveWire(component, static_cast<uint32_t>(link));
    }
    int nodeCount = ImNodes::NumSelectedNodes();
    if (nodeCount > 0) {
        std::vector<int> nodes(static_cast<size_t>(nodeCount));
        ImNodes::GetSelectedNodes(nodes.data());
        for (int node : nodes) RemoveNode(component, static_cast<uint32_t>(node));
    }
    ImNodes::ClearLinkSelection();
    ImNodes::ClearNodeSelection();
}

constexpr float MinZoom = 0.25f, MaxZoom = 3.0f;

// imnodes has no zoom of its own: its sizes are scaled here, and node positions in DrawNode.
static void ScaleNodeStyle(float zoom)
{
    const ImNodesStyle& base = app.baseNodeStyle;
    ImNodesStyle& style = ImNodes::GetStyle();
    style.GridSpacing = base.GridSpacing * zoom;
    style.NodeCornerRounding = base.NodeCornerRounding * zoom;
    style.NodePadding = ImVec2(base.NodePadding.x * zoom, base.NodePadding.y * zoom);
    style.NodeBorderThickness = base.NodeBorderThickness * zoom;
    style.LinkThickness = base.LinkThickness * zoom;
    style.LinkHoverDistance = base.LinkHoverDistance * zoom;
    style.PinCircleRadius = base.PinCircleRadius * zoom;
    style.PinQuadSideLength = base.PinQuadSideLength * zoom;
    style.PinTriangleSideLength = base.PinTriangleSideLength * zoom;
    style.PinLineThickness = base.PinLineThickness * zoom;
    style.PinHoverRadius = base.PinHoverRadius * zoom;
    style.PinOffset = base.PinOffset * zoom;
}

static void ResetZoom(Component& component)
{
    if (!component.editor) return;
    ImNodes::EditorContextSet(component.editor);
    ImVec2 panning = ImNodes::EditorContextGetPanning();
    ImNodes::EditorContextResetPanning(ImVec2(panning.x / component.zoom, panning.y / component.zoom));
    component.zoom = 1.0f;
}

static void DrawCanvas(ImVec2 position, ImVec2 size)
{
    Component& component = *app.current;
    Simulation& simulation = app.simulation;
    bool editing = app.mode == Mode::Edit;

    ImGui::SetNextWindowPos(position);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("Canvas", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar();
    app.canvasCenter = ImVec2(position.x + size.x * 0.5f, position.y + size.y * 0.5f);

    EnsureEditor(component);
    ImNodes::EditorContextSet(component.editor);
    ImNodes::GetIO().LinkDetachWithModifierClick.Modifier = editing ? &ImGui::GetIO().KeyCtrl : nullptr;

    if (editing) PruneWires(component);

    // Mouse wheel zooms about the point under the cursor: that point stays put on screen.
    ImGuiIO& io = ImGui::GetIO();
    if (io.MouseWheel != 0 && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
        float oldZoom = component.zoom;
        float newZoom = std::clamp(oldZoom * std::pow(1.15f, io.MouseWheel), MinZoom, MaxZoom);
        ImVec2 mouse(io.MousePos.x - position.x, io.MousePos.y - position.y);
        ImVec2 panning = ImNodes::EditorContextGetPanning();
        float ratio = newZoom / oldZoom;
        ImNodes::EditorContextResetPanning(
            ImVec2(mouse.x - (mouse.x - panning.x) * ratio, mouse.y - (mouse.y - panning.y) * ratio));
        component.zoom = newZoom;
    }
    float zoom = component.zoom;
    ScaleNodeStyle(zoom);
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * zoom);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * zoom, 4 * zoom));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4 * zoom, 3 * zoom));

    ImNodes::BeginNodeEditor();
    FindSingleWirePins(component);
    for (Node& node : component.nodes) DrawNode(node);

    for (const Wire& wire : component.wires) {
        const Node* from = component.FindNode(wire.fromNode);
        const Node* to = component.FindNode(wire.toNode);
        if (!from || !to) continue;
        int width = static_cast<int>(wire.width);
        if (!editing) {
            ImU32 color = LevelColor(BusLevel(simulation, PinNets(simulation, wire.fromNode, true,
                                                                  static_cast<int>(wire.fromPin), width)));
            ImNodes::PushColorStyle(ImNodesCol_Link, color);
            ImNodes::PushColorStyle(ImNodesCol_LinkHovered, Lighten(color, 30));
            ImNodes::PushColorStyle(ImNodesCol_LinkSelected, Lighten(color, 30));
        }
        if (width > 1) ImNodes::PushStyleVar(ImNodesStyleVar_LinkThickness, ImNodes::GetStyle().LinkThickness * 2.5f);
        ImNodes::Link(static_cast<int>(wire.id), WireEndAttribute(component, *from, true, static_cast<int>(wire.fromPin), width),
                      WireEndAttribute(component, *to, false, static_cast<int>(wire.toPin), width));
        if (width > 1) ImNodes::PopStyleVar();
        if (!editing) {
            ImNodes::PopColorStyle();
            ImNodes::PopColorStyle();
            ImNodes::PopColorStyle();
        }
    }

    if (app.showMiniMap) ImNodes::MiniMap(0.15f, ImNodesMiniMapLocation_BottomRight);
    ImNodes::EndNodeEditor();
    ImGui::PopStyleVar(2);
    ImGui::PopFont();
    ScaleNodeStyle(1.0f);

    // Positions: remember where nodes were dragged.
    for (Node& node : component.nodes) {
        ImVec2 now = ImNodes::GetNodeGridSpacePos(static_cast<int>(node.id));
        now = ImVec2(now.x / zoom, now.y / zoom);
        if (std::fabs(now.x - node.position.x) > 0.5f || std::fabs(now.y - node.position.y) > 0.5f) {
            node.position = now;
            component.dirty = true;
        }
    }

    int hovered = -1;
    app.hoveredNode = ImNodes::IsNodeHovered(&hovered) ? hovered : -1;
    int hoveredLink;
    if (ImNodes::IsLinkHovered(&hoveredLink)) {
        for (const Wire& wire : component.wires) {
            const Node* from = component.FindNode(wire.fromNode);
            const Node* to = component.FindNode(wire.toNode);
            if (wire.id != static_cast<uint32_t>(hoveredLink) || !from || !to) continue;
            int width = static_cast<int>(wire.width);
            std::string text = EndpointText(*from, true, static_cast<int>(wire.fromPin), width) + " -> " +
                               EndpointText(*to, false, static_cast<int>(wire.toPin), width);
            if (width > 1) text += "  (" + std::to_string(width) + " bits)";
            if (!editing) text += " = " + BusText(simulation, PinNets(simulation, wire.fromNode, true, static_cast<int>(wire.fromPin), width));
            ImGui::SetTooltip("%s", text.c_str());
        }
    }
    if (app.hoveredNode >= 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        OpenRomEditor(static_cast<uint32_t>(app.hoveredNode));  // does nothing unless it's a ROM

    if (editing) {
        int startPin, endPin;
        if (ImNodes::IsLinkCreated(&startPin, &endPin)) {
            uint32_t fromNode, toNode;
            bool fromOutput, toOutput, fromBus, toBus;
            int fromIndex, toIndex;
            DecodePinId(startPin, fromNode, fromOutput, fromIndex, &fromBus);
            DecodePinId(endPin, toNode, toOutput, toIndex, &toBus);
            const Node* from = component.FindNode(fromNode);
            const Node* to = component.FindNode(toNode);
            if (fromOutput && !toOutput && from && to) {
                // A bus pin carries its whole group. A bit pin wired to a bus pin starts a bus of
                // the same width there.
                int fromWidth = fromBus ? GroupOfPin(PinGroups(*from, true), fromIndex).count : 1;
                int toWidth = toBus ? GroupOfPin(PinGroups(*to, false), toIndex).count : 1;
                int width = std::max(fromWidth, toWidth);
                bool fits = fromIndex + width <= OutputCount(*from) && toIndex + width <= InputCount(*to);
                if ((fromWidth == toWidth || fromWidth == 1 || toWidth == 1) && fits)
                    Connect(component, fromNode, fromIndex, toNode, toIndex, width);
                else
                    SetStatus("Can't wire %d bits to %d: show the bus's bits (+) to wire some of them", fromWidth, toWidth);
            }
        }
        int destroyedLink;
        if (ImNodes::IsLinkDestroyed(&destroyedLink)) RemoveWire(component, static_cast<uint32_t>(destroyedLink));

        bool canvasFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (canvasFocused && !ImGui::GetIO().WantTextInput &&
            (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)))
            DeleteSelection();

        if (ImNodes::IsEditorHovered() && app.hoveredNode < 0 && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
            app.popupScreenPosition = ImGui::GetMousePos();
            ImGui::OpenPopup("Add node");
        }
        if (ImGui::BeginPopup("Add node")) {
            DrawAddNodeMenu(app.popupScreenPosition, false);
            ImGui::EndPopup();
        }
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Side panels

static void BeginPanel(const char* name, ImVec2 position, ImVec2 size)
{
    ImGui::SetNextWindowPos(position);
    ImGui::SetNextWindowSize(size);
    ImGui::Begin(name, nullptr,
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
}

// A draggable divider at x. Dragging it changes panelWidth; direction is +1 for a panel on
// the left (drag right to grow) and -1 for one on the right.
static void DrawSplitter(const char* id, float x, float top, float height, float& panelWidth, float direction)
{
    const float grip = 8;
    ImGui::SetNextWindowPos(ImVec2(x - grip * 0.5f, top));
    ImGui::SetNextWindowSize(ImVec2(grip, height));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin(id, nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::PopStyleVar(3);
    ImGui::InvisibleButton("grip", ImVec2(grip, height));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(x - 1.5f, top), ImVec2(x + 1.5f, top + height),
                                                  ImGui::GetColorU32(ImGui::IsItemActive() ? ImGuiCol_SeparatorActive
                                                                                           : ImGuiCol_SeparatorHovered));
    }
    if (ImGui::IsItemActive()) panelWidth += ImGui::GetIO().MouseDelta.x * direction;
    ImGui::End();
}

static void DrawPalette(ImVec2 position, ImVec2 size)
{
    BeginPanel("Palette", position, size);

    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Click to add, or right-click the canvas.");
    ImGui::PopStyleColor();
    DrawAddNodeMenu(ImVec2(0, 0), true);

    ImGui::Spacing();
    ImGui::SeparatorText("Library");
    if (ImGui::Button("New component", ImVec2(-1, 0))) NewComponent();

    std::vector<Component*> sorted;
    for (auto& component : library) sorted.push_back(component.get());
    std::sort(sorted.begin(), sorted.end(), [](Component* a, Component* b) { return a->name < b->name; });

    for (Component* component : sorted) {
        ImGui::PushID(component);
        std::string label = component->name + (component->dirty ? " *" : "");
        if (ImGui::Selectable(label.c_str(), component == app.current)) OpenComponent(component);
        ImGui::SetItemTooltip("Open '%s' for editing", component->name.c_str());
        ImGui::PopID();
    }

    ImGui::End();
}

static void DrawSelectedNodeProperties(Node& node)
{
    Component& component = *app.current;
    ImGui::SeparatorText("Selected");
    ImGui::Text("%s", NodeTitle(node).c_str());
    if (IsPrimitive(node.kind)) ImGui::TextWrapped("%s", Primitives[static_cast<int>(node.kind)].description);

    if (node.kind == NodeKind::Input || node.kind == NodeKind::Output) {
        if (app.labelBufferNode != node.id) {
            snprintf(app.labelBuffer, sizeof app.labelBuffer, "%s", node.label.c_str());
            app.labelBufferNode = node.id;
        }
        if (ImGui::InputText("Pin name", app.labelBuffer, sizeof app.labelBuffer)) {
            node.label = app.labelBuffer;
            component.dirty = true;
        }
        if (ImGui::SliderInt("Bits", &node.width, 1, MaxBusWidth)) {
            node.width = std::clamp(node.width, 1, MaxBusWidth);
            component.dirty = true;
            PruneWires(component);
        }
        ImGui::SetItemTooltip("More than 1 makes a bus: pins %s0, %s1, ...", node.label.c_str(), node.label.c_str());
        int pinNumber = 0;
        for (const Node* pin : PinNodes(component, node.kind)) {
            if (pin == &node) break;
            pinNumber += pin->width;
        }
        const char* side = node.kind == NodeKind::Input ? "Input" : "Output";
        if (node.width == 1) ImGui::TextDisabled("%s pin %d of this component", side, pinNumber + 1);
        else ImGui::TextDisabled("%s pins %d-%d of this component", side, pinNumber + 1, pinNumber + node.width);
    }

    if (IsMemory(node.kind)) {
        MemoryData& rom = *node.memory;
        int addressBits = rom.addressBits, dataBits = rom.dataBits;
        bool resized = ImGui::SliderInt("Address bits", &addressBits, 1, MaxMemoryAddressBits);
        resized |= ImGui::SliderInt("Data bits", &dataBits, 1, MaxMemoryDataBits);
        if (resized) {
            int oldAddressBits = rom.addressBits;
            rom.Resize(addressBits, dataBits);
            if (!rom.image.empty()) LoadRomImage(rom);
            RemapMemoryWires(component, node, oldAddressBits);
        }
        if (node.kind == NodeKind::Rom)
            ImGui::TextDisabled("%zu words; AT28C64 is 13 x 8, AT28C256 is 15 x 8", rom.words.size());
        else
            ImGui::TextDisabled("%zu words; 62256 is 15 x 8, 6264 is 13 x 8", rom.words.size());
        if (!rom.image.empty()) ImGui::TextDisabled("%s %s", node.kind == NodeKind::Rom ? "Contents from" : "Preloaded from",
                                                    rom.image.c_str());
        if (ImGui::Button(node.kind == NodeKind::Rom ? "Edit contents..." : "Contents...")) OpenRomEditor(node.id);
        ImGui::SetItemTooltip("Or double-click the ROM");
    }

    if (node.kind == NodeKind::Component) {
        Component* type = FindComponent(node.typeName);
        if (type) {
            if (ImGui::Button("Open component")) OpenComponent(type);
        } else {
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "No component named '%s'", node.typeName.c_str());
        }
    }

    if (ImGui::Button("Delete")) {
        RemoveNode(component, node.id);
        ImNodes::EditorContextSet(component.editor);
        ImNodes::ClearNodeSelection();
    }
}

static void DrawProperties(ImVec2 position, ImVec2 size)
{
    Component& component = *app.current;
    BeginPanel("Properties", position, size);

    ImGui::SeparatorText("Component");
    if (app.nameBufferOwner != &component || !ImGui::IsAnyItemActive()) {
        if (app.nameBufferOwner != &component || strcmp(app.nameBuffer, component.name.c_str()) != 0) {
            snprintf(app.nameBuffer, sizeof app.nameBuffer, "%s", component.name.c_str());
            app.nameBufferOwner = &component;
        }
    }
    ImGui::InputText("Name", app.nameBuffer, sizeof app.nameBuffer);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        std::string newName = app.nameBuffer;
        if (newName == component.name) {
        } else if (!IsValidComponentName(newName)) {
            SetStatus("Names use letters, digits, spaces, '_' and '-' (up to %d characters)", MaxNameLength - 1);
        } else if (FindComponent(newName)) {
            SetStatus("There is already a component named '%s'", newName.c_str());
        } else {
            RenameComponent(component, newName);
            SetStatus("Renamed to '%s' (and updated everything that uses it)", newName.c_str());
        }
        snprintf(app.nameBuffer, sizeof app.nameBuffer, "%s", component.name.c_str());
    }

    if (app.descriptionBufferOwner != &component) {
        snprintf(app.descriptionBuffer, sizeof app.descriptionBuffer, "%s", component.description.c_str());
        app.descriptionBufferOwner = &component;
    }
    if (ImGui::InputTextMultiline("##description", app.descriptionBuffer, sizeof app.descriptionBuffer,
                                  ImVec2(-1, ImGui::GetTextLineHeight() * 4))) {
        component.description = app.descriptionBuffer;
        component.dirty = true;
    }
    if (component.description.empty() && !ImGui::IsItemActive()) {
        ImVec2 corner = ImGui::GetItemRectMin();
        ImGui::GetWindowDrawList()->AddText(ImVec2(corner.x + 4, corner.y + 3), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                            "Description");
    }

    int gates = 0, parts = 0;
    for (const Node& node : component.nodes) {
        if (node.kind == NodeKind::Component) parts++;
        else if (node.kind != NodeKind::Input && node.kind != NodeKind::Output) gates++;
    }
    ImGui::Text("Pins: %zu in, %zu out", PinBits(component, NodeKind::Input).size(),
                PinBits(component, NodeKind::Output).size());
    ImGui::Text("Parts: %d primitives, %d components", gates, parts);
    ImGui::Text("Wires: %zu", component.wires.size());

    ImNodes::EditorContextSet(component.editor);
    if (ImNodes::NumSelectedNodes() == 1) {
        int selected;
        ImNodes::GetSelectedNodes(&selected);
        if (Node* node = component.FindNode(static_cast<uint32_t>(selected))) DrawSelectedNodeProperties(*node);
    } else if (ImNodes::NumSelectedNodes() > 1) {
        ImGui::SeparatorText("Selected");
        ImGui::Text("%d nodes", ImNodes::NumSelectedNodes());
        if (ImGui::Button("Delete")) DeleteSelection();
    }

    ImGui::SeparatorText("How to");
    ImGui::TextWrapped(
        "Drag from an output pin to an input pin to wire them. Several wires into one input make a bus.\n"
        "Pins named A0, A1, ... show as one square bus pin, and a wire between two of them carries every bit. "
        "+ shows a bus's bits, to wire them one at a time.\n"
        "Ctrl+click a wire end to pull it off.\n"
        "Delete removes the selection.\n"
        "Middle-drag pans, the mouse wheel zooms.\n"
        "Double-click a ROM or RAM to see what it holds.\n"
        "Input and Output nodes become this component's pins when you use it inside another one.\n\n"
        "F5 simulate   Ctrl+S save all   Ctrl+N new");

    ImGui::End();
}

static void DrawSimulationPanel(ImVec2 position, ImVec2 size)
{
    Component& component = *app.current;
    Simulation& simulation = app.simulation;
    BeginPanel("Simulation", position, size);

    ImGui::Text("Simulating '%s'", component.name.c_str());
    if (ImGui::Button("Back to editor (F5)", ImVec2(-1, 0))) app.mode = Mode::Edit;

    ImGui::SeparatorText("Clock");
    if (ImGui::Button(simulation.running ? "Pause (Space)" : "Run (Space)", ImVec2(-1, 0)))
        simulation.running = !simulation.running;
    if (ImGui::Button("Step one cycle")) {
        HalfStep(simulation);
        HalfStep(simulation);
    }
    ImGui::SameLine();
    if (ImGui::Button("Half step")) HalfStep(simulation);
    ImGui::SameLine();
    if (ImGui::Button("Reset")) BuildSimulation(simulation, component);
    ImGui::SetItemTooltip("Power-cycle: flip-flops and RAM start over");
    ImGui::SliderFloat("Hz", &simulation.frequency, 0.1f, 10000.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
    ImGui::Checkbox("RAM powers up random", &simulation.randomRam);
    ImGui::SetItemTooltip("As real RAM does: catches reading memory before writing it.\n"
                          "Off: RAM starts as zeros. Takes effect on Reset.");

    ImGui::Text("Clock is ");
    ImGui::SameLine(0, 0);
    ImGui::TextColored(simulation.clock ? ImVec4(0.35f, 0.9f, 0.43f, 1) : ImVec4(0.5f, 0.6f, 0.5f, 1),
                       simulation.clock ? "HIGH" : "LOW");
    ImGui::Text("Cycles: %llu", static_cast<unsigned long long>(simulation.halfCycles / 2));

    ImGui::SeparatorText("Circuit");
    ImGui::Text("%zu gates, %d nets", simulation.gates.size(), simulation.netCount);
    ImGui::Text("Last change settled in %d gate delays", simulation.lastSettleSteps);
    ImGui::Text("Slowest clock edge: %d gate delays", simulation.slowestSettleSteps);
    if (simulation.unstable)
        ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1), "Unstable: still changing after %d gate delays\n(something oscillates)", MaxSettleSteps);
    if (simulation.conflicts) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.35f, 0.35f, 1));
        ImGui::TextWrapped("%d bus conflict(s): a wire driven to 0 and 1 at once (shown red)", simulation.conflicts);
        ImGui::PopStyleColor();
    }
    if (simulation.missingComponents)
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%d missing component(s) left out", simulation.missingComponents);
    if (simulation.nestingTooDeep)
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Nesting deeper than %d levels was cut off", MaxNestingDepth);

    if (!simulation.terminals.empty()) {
        ImGui::SeparatorText("Terminal");
        ImGui::BeginChild("console", ImVec2(-1, ImGui::GetTextLineHeightWithSpacing() * 10), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(simulation.console.c_str(), simulation.console.c_str() + simulation.console.size());
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);  // follow new output
        ImGui::EndChild();
        ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Clear").x - ImGui::GetStyle().FramePadding.x * 2 -
                                ImGui::GetStyle().ItemSpacing.x);
        if (ImGui::InputTextWithHint("##type", "type here; Enter sends", app.typeBuffer, sizeof app.typeBuffer,
                                     ImGuiInputTextFlags_EnterReturnsTrue)) {
            for (const char* character = app.typeBuffer; *character; character++)
                simulation.keys.push_back(static_cast<uint8_t>(*character));
            simulation.keys.push_back('\n');
            app.typeBuffer[0] = 0;
            ImGui::SetKeyboardFocusHere(-1);
            RecordSettle(simulation);  // IRQ and the status register change
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear")) simulation.console.clear();
        ImGui::TextDisabled("%zu key(s) waiting to be read", simulation.keys.size());
    }

    ImGui::SeparatorText("Inputs");
    bool anyInput = false;
    for (const Node* node : PinNodes(component, NodeKind::Input)) {
        anyInput = true;
        ImGui::PushID(static_cast<int>(node->id));
        if (node->width == 1) {
            bool on = SwitchValue(simulation, node->id) != 0;
            if (ImGui::Checkbox(node->label.c_str(), &on)) SetSwitch(simulation, node->id, on);
        } else {
            uint32_t value = SwitchValue(simulation, node->id);
            ImGui::SetNextItemWidth(ImGui::CalcTextSize("00000000").x + 16);
            if (ImGui::InputScalar(node->label.c_str(), ImGuiDataType_U32, &value, nullptr, nullptr, "%X",
                                   ImGuiInputTextFlags_CharsHexadecimal))
                SetSwitch(simulation, node->id, value);
        }
        ImGui::PopID();
    }
    if (!anyInput) ImGui::TextDisabled("(no Input nodes)");

    ImGui::SeparatorText("Outputs");
    bool anyOutput = false;
    for (const Node* node : PinNodes(component, NodeKind::Output)) {
        anyOutput = true;
        if (node->width > 1) {
            ImGui::Text("%s = %s", node->label.c_str(), BusText(simulation, PinNets(simulation, node->id, false, 0, node->width)).c_str());
            continue;
        }
        uint8_t level = simulation.Level(simulation.NetOfInput(node->id, 0));
        DrawLed(level == High);
        ImGui::SameLine();
        ImGui::TextUnformatted(node->label.c_str());
        if (level == Floating || level == Conflict) {
            ImGui::SameLine();
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(LevelColor(level)), "%s", LevelText(level));
        }
    }
    if (!anyOutput) ImGui::TextDisabled("(no Output nodes)");

    ImGui::SeparatorText("Inspector");
    int inspected = app.hoveredNode;
    ImNodes::EditorContextSet(component.editor);
    if (inspected < 0 && ImNodes::NumSelectedNodes() == 1) ImNodes::GetSelectedNodes(&inspected);
    if (const Node* node = inspected >= 0 ? component.FindNode(static_cast<uint32_t>(inspected)) : nullptr) {
        ImGui::Text("%s", NodeTitle(*node).c_str());
        if (IsMemory(node->kind) && ImGui::SmallButton("Show contents")) OpenRomEditor(node->id);
        for (int index = 0; index < InputCount(*node); index++)
            ImGui::Text("  in  %-8s = %s", PinName(*node, false, index).c_str(),
                        LevelText(simulation.Level(simulation.NetOfInput(node->id, index))));
        for (int index = 0; index < OutputCount(*node); index++)
            ImGui::Text("  out %-8s = %s", PinName(*node, true, index).c_str(),
                        LevelText(simulation.Level(simulation.NetOfOutput(node->id, index))));
    } else {
        ImGui::TextDisabled("Hover or select a node");
    }

    ImGui::End();
}

static void DrawMenuBar()
{
    if (!ImGui::BeginMainMenuBar()) return;
    bool editing = app.mode == Mode::Edit;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New component", "Ctrl+N", false, editing)) NewComponent();
        if (ImGui::MenuItem("Save all", "Ctrl+S")) SaveAllWithStatus();
        ImGui::Separator();
        if (ImGui::MenuItem("Quit")) app.quitRequested = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Mode")) {
        if (ImGui::MenuItem("Edit", "F5", editing)) app.mode = Mode::Edit;
        if (ImGui::MenuItem("Simulate", "F5", !editing) && editing) EnterSimulation();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Minimap", nullptr, &app.showMiniMap);
        if (ImGui::MenuItem("Reset zoom", "Ctrl+0")) ResetZoom(*app.current);
        ImGui::EndMenu();
    }

    ImGui::Separator();
    ImGui::TextColored(editing ? ImVec4(0.6f, 0.75f, 1, 1) : ImVec4(0.4f, 0.95f, 0.5f, 1), "%s: %s%s",
                       editing ? "Editing" : "Simulating", app.current->name.c_str(), app.current->dirty ? " *" : "");
    if (!app.status.empty() && GetTime() < app.statusUntil) {
        ImGui::Separator();
        ImGui::TextUnformatted(app.status.c_str());
    }
    ImGui::EndMainMenuBar();
}

static void DrawQuitDialog()
{
    if (app.quitRequested) {
        app.quitRequested = false;
        if (AnyDirty()) ImGui::OpenPopup("Unsaved changes");
        else app.quit = true;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Some components have unsaved changes.");
        if (ImGui::Button("Save all and quit")) {
            if (SaveAll() == 0) app.quit = true;
            else SetStatus("Couldn't save everything; not quitting");
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Quit without saving")) app.quit = true;
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

static void HandleShortcuts()
{
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsKeyPressed(ImGuiKey_F5)) ToggleMode();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) SaveAllWithStatus();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_N, false) && app.mode == Mode::Edit) NewComponent();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_0, false)) ResetZoom(*app.current);
    if (app.mode == Mode::Simulate && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Space, false))
        app.simulation.running = !app.simulation.running;
}

// Advances the clock in real time while running, without stalling the UI.
static void RunClock(float frameSeconds)
{
    Simulation& simulation = app.simulation;
    if (app.mode != Mode::Simulate || !simulation.running) {
        simulation.pendingHalfCycles = 0;
        return;
    }
    simulation.pendingHalfCycles += frameSeconds * simulation.frequency * 2.0;
    double deadline = GetTime() + 0.010;  // spend at most ~10 ms per frame simulating
    while (simulation.pendingHalfCycles >= 1.0 && GetTime() < deadline) {
        HalfStep(simulation);
        simulation.pendingHalfCycles -= 1.0;
    }
    simulation.pendingHalfCycles = std::min(simulation.pendingHalfCycles, 2.0);  // drop what we can't keep up with
}

static void DrawFrame()
{
    float width = static_cast<float>(GetScreenWidth());
    float height = static_cast<float>(GetScreenHeight());

    DrawMenuBar();
    float top = ImGui::GetFrameHeight();
    float bodyHeight = height - top;

    // Keep the panels a usable size and leave the canvas at least MinCanvasWidth.
    const float MinPanelWidth = 150, MinCanvasWidth = 200;
    auto clampWidth = [&](float& panelWidth, float others) {
        panelWidth = std::max(MinPanelWidth, std::min(panelWidth, width - others - MinCanvasWidth));
    };

    if (app.mode == Mode::Edit) {
        clampWidth(app.paletteWidth, app.propertiesWidth);
        clampWidth(app.propertiesWidth, app.paletteWidth);
        float paletteWidth = app.paletteWidth, propertiesWidth = app.propertiesWidth;
        DrawPalette(ImVec2(0, top), ImVec2(paletteWidth, bodyHeight));
        DrawCanvas(ImVec2(paletteWidth, top), ImVec2(width - paletteWidth - propertiesWidth, bodyHeight));
        DrawProperties(ImVec2(width - propertiesWidth, top), ImVec2(propertiesWidth, bodyHeight));
        DrawSplitter("##palette splitter", paletteWidth, top, bodyHeight, app.paletteWidth, +1);
        DrawSplitter("##properties splitter", width - propertiesWidth, top, bodyHeight, app.propertiesWidth, -1);
    } else {
        clampWidth(app.simulationWidth, 0);
        float panelWidth = app.simulationWidth;
        DrawCanvas(ImVec2(0, top), ImVec2(width - panelWidth, bodyHeight));
        DrawSimulationPanel(ImVec2(width - panelWidth, top), ImVec2(panelWidth, bodyHeight));
        DrawSplitter("##simulation splitter", width - panelWidth, top, bodyHeight, app.simulationWidth, -1);
    }
    DrawRomEditor();

    DrawQuitDialog();
    HandleShortcuts();
}

// ===========================================================================

// ===========================================================================
// Hardware check (--check-hardware): tests the machine's components in the components
// directory against the emulator's behaviour.

struct AluResult {
    uint16_t y;
    int carry, overflow;  // overflow is only checked for the adder operations
};

// The ALU 16 operations, as the emulator computes them (Emulator.c, AluAddWithFlags and friends).
static AluResult ReferenceAlu(int op, uint16_t a, uint16_t b, int carryIn)
{
    auto add = [](uint16_t x, uint16_t y, int c) {
        uint32_t sum = static_cast<uint32_t>(x) + y + c;
        uint16_t result = static_cast<uint16_t>(sum);
        return AluResult{result, static_cast<int>(sum >> 16) & 1, ((~(x ^ y) & (x ^ result)) >> 15) & 1};
    };
    switch (op) {
    case 0: return add(a, b, carryIn);
    case 1: return add(a, static_cast<uint16_t>(~b), carryIn);
    case 2: return {static_cast<uint16_t>(a & b), carryIn, 0};
    case 3: return {static_cast<uint16_t>(a | b), carryIn, 0};
    case 4: return {static_cast<uint16_t>(a ^ b), carryIn, 0};
    // Shifts move one bit; the control unit repeats them (b & 15) times.
    case 5: return {static_cast<uint16_t>(a << 1), a >> 15, 0};
    case 6: return {static_cast<uint16_t>(a >> 1), a & 1, 0};
    case 7: return {static_cast<uint16_t>(static_cast<int16_t>(a) >> 1), a & 1, 0};
    case 8: return {static_cast<uint16_t>(~a), carryIn, 0};
    case 9: return add(0, static_cast<uint16_t>(~a), carryIn);
    case 11: return {b, carryIn, 0};
    case 12: return {static_cast<uint16_t>((a & 0x80) ? (a | 0xFF00) : (a & 0xFF)), -1, 0};  // C: don't care
    default: return {a, carryIn, 0};  // 10 PASSA, and 13-15
    }
}

static int RunHardwareCheck()
{
    std::vector<std::string> problems = LoadLibrary();
    for (const std::string& problem : problems) printf("  %s\n", problem.c_str());
    Check(problems.empty(), "the components load");

    if (Component* alu = FindComponent("ALU 16")) {
        Simulation simulation;
        BuildSimulation(simulation, *alu);
        printf("ALU 16: %zu gates, %d nets\n", simulation.gates.size(), simulation.netCount);
        Check(simulation.missingComponents == 0, "ALU 16 has all its parts");

        std::vector<uint16_t> values = {0, 1, 2, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF, 0x8000, 0x8001, 0xFFFE, 0xFFFF};
        uint32_t seed = 12345;
        auto random = [&] { return seed = seed * 1103515245u + 12345u, static_cast<uint16_t>(seed >> 8); };
        for (int i = 0; i < 40; i++) values.push_back(random());
        for (int shift = 0; shift < 16; shift++) values.push_back(static_cast<uint16_t>(0x9000 + shift));

        int failures = 0, vectors = 0, slowest = 0;
        for (int op = 0; op < 16; op++) {
            for (uint16_t a : values) {
                for (uint16_t b : values) {
                    for (int carryIn = 0; carryIn < 2; carryIn++) {
                        SetBus(simulation, *alu, "A", 16, a);
                        SetBus(simulation, *alu, "B", 16, b);
                        SetBus(simulation, *alu, "OP", 4, op);
                        SetBus(simulation, *alu, "Cin", 1, carryIn);
                        RecordSettle(simulation);
                        slowest = std::max(slowest, simulation.lastSettleSteps);
                        vectors++;

                        AluResult expected = ReferenceAlu(op, a, b, carryIn);
                        uint32_t y = ReadBus(simulation, *alu, "Y", 16);
                        int carry = ReadOutput(simulation, *alu, "C"), overflow = ReadOutput(simulation, *alu, "V");
                        int zero = ReadOutput(simulation, *alu, "Z"), negative = ReadOutput(simulation, *alu, "N");
                        bool adder = op == 0 || op == 1 || op == 9;
                        if (y != expected.y || (expected.carry >= 0 && carry != expected.carry) || (adder && overflow != expected.overflow) ||
                            zero != (expected.y == 0) || negative != (expected.y >> 15)) {
                            if (failures++ < 10)
                                printf("  ALU op %d a=%04X b=%04X cin=%d: got Y=%04X C%d V%d Z%d N%d, want Y=%04X C%d V%d\n",
                                       op, a, b, carryIn, y, carry, overflow, zero, negative, expected.y,
                                       expected.carry, expected.overflow);
                        }
                    }
                }
            }
        }
        printf("ALU 16: %d vectors, slowest settle %d steps\n", vectors, slowest);
        Check(failures == 0, "ALU 16 matches the emulator");

        // A shift by n is n one-bit shifts, Y fed back to A, the last carry ending up in C.
        int shiftFailures = 0;
        for (int op = 5; op <= 7; op++) {
            for (uint16_t value : values) {
                for (int amount = 1; amount < 16; amount++) {
                    uint16_t a = value;
                    int carry = 0;
                    for (int step = 0; step < amount; step++) {
                        SetBus(simulation, *alu, "A", 16, a);
                        SetBus(simulation, *alu, "OP", 4, op);
                        RecordSettle(simulation);
                        a = static_cast<uint16_t>(ReadBus(simulation, *alu, "Y", 16));
                        carry = ReadOutput(simulation, *alu, "C");
                    }
                    uint16_t want = op == 5 ? static_cast<uint16_t>(value << amount)
                                  : op == 6 ? static_cast<uint16_t>(value >> amount)
                                            : static_cast<uint16_t>(static_cast<int16_t>(value) >> amount);
                    int wantCarry = op == 5 ? (value >> (16 - amount)) & 1 : (value >> (amount - 1)) & 1;
                    if ((a != want || carry != wantCarry) && shiftFailures++ < 10)
                        printf("  shift op %d %04X by %d: got %04X C%d, want %04X C%d\n", op, value, amount, a,
                               carry, want, wantCarry);
                }
            }
        }
        Check(shiftFailures == 0, "repeated one-bit shifts match the emulator's shifts");
    } else {
        Check(false, "there is an ALU 16 component");
    }

    if (Component* file = FindComponent("Register File")) {
        Simulation simulation;
        BuildSimulation(simulation, *file);
        printf("Register File: %zu gates, %d nets\n", simulation.gates.size(), simulation.netCount);
        Check(simulation.missingComponents == 0, "Register File has all its parts");

        uint16_t registers[8];
        uint32_t seed = 777;
        auto random = [&] { return seed = seed * 1103515245u + 12345u, static_cast<uint16_t>(seed >> 8); };
        auto clock = [&] {
            SetBus(simulation, *file, "CLK", 1, 1);
            RecordSettle(simulation);
            SetBus(simulation, *file, "CLK", 1, 0);
            RecordSettle(simulation);
        };
        auto write = [&](int r, uint16_t value, int enable) {
            SetBus(simulation, *file, "W", 3, r);
            SetBus(simulation, *file, "D", 16, value);
            SetBus(simulation, *file, "WE", 1, enable);
            RecordSettle(simulation);
            clock();
            if (enable) registers[r] = value;
        };
        for (int r = 0; r < 8; r++) write(r, random(), 1);

        int failures = 0;
        for (int i = 0; i < 2000; i++) {
            write(random() & 7, random(), (random() & 3) != 0);  // a quarter of the cycles don't write
            int ra = random() & 7, rb = random() & 7;
            SetBus(simulation, *file, "RA", 3, ra);
            SetBus(simulation, *file, "RB", 3, rb);
            RecordSettle(simulation);
            uint32_t qa = ReadBus(simulation, *file, "QA", 16), qb = ReadBus(simulation, *file, "QB", 16);
            if ((qa != registers[ra] || qb != registers[rb]) && failures++ < 10)
                printf("  R%d=%04X R%d=%04X: read %04X %04X\n", ra, registers[ra], rb, registers[rb], qa, qb);
        }
        Check(failures == 0, "Register File keeps R0-R7 like the emulator");
        Check(simulation.conflicts == 0, "Register File read ports never conflict");
    } else {
        Check(false, "there is a Register File component");
    }

    // The whole machine runs Fibonacci.bin and prints what the emulator prints.
    if (Component* computer = FindComponent("ISA16 Computer")) {
        Simulation simulation;
        BuildSimulation(simulation, *computer);
        printf("ISA16 Computer: %zu gates, %d nets\n", simulation.gates.size(), simulation.netCount);
        Check(simulation.missingComponents == 0, "ISA16 Computer has all its parts");
        auto start = std::chrono::steady_clock::now();
        int cycles = 0, slowest = 0;
        while (cycles < 200000 && !ReadOutput(simulation, *computer, "HALTED") && !ReadOutput(simulation, *computer, "ILLEGAL")) {
            for (int edge = 0; edge < 2; edge++) {
                simulation.clock ^= 1;
                RecordSettle(simulation);
                slowest = std::max(slowest, simulation.lastSettleSteps);
            }
            cycles++;
        }
        double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        printf("ISA16 Computer: %d cycles in %.1f s, slowest settle %d steps, %d conflicting nets%s\n", cycles, seconds,
               slowest, simulation.conflicts, simulation.unstable ? ", UNSTABLE" : "");
        printf("--- terminal ---\n%s\n----------------\n", simulation.console.c_str());
        static const char* const Expected =
            "Fibonacci numbers that fit in 16 bits:\n0 1 1 2 3 5 8 13 21 34 55 89 144 233 377 610 987 1597 2584 "
            "4181 6765 10946 17711 28657 46368\ntimer interrupts: 105\n";
        Check(!ReadOutput(simulation, *computer, "ILLEGAL"), "ISA16 Computer doesn't hit an illegal instruction");
        Check(simulation.console == Expected, "ISA16 Computer runs Fibonacci.bin like the emulator");
    } else {
        Check(false, "there is an ISA16 Computer component");
    }

    if (testFailures) {
        printf("%d check(s) failed\n", testFailures);
        return 1;
    }
    printf("All hardware checks passed\n");
    return 0;
}

// ===========================================================================
// Headless run (--run): simulates a component with no window, gate by gate, and
// prints what its Terminal prints to stdout. With --program, the program (a .c,
// .s or .bin; compiled and assembled with the Compiler and Assembler next to this
// executable) replaces the image of every top-level RAM that has one, keeping
// each RAM's byte lane. Piped stdin is typed into the Terminal as the program asks for
// it; the run stops (status 0) once it has run out and the program waits for more.
// ===========================================================================

struct RunOptions {
    std::string component;
    std::string program;
    std::vector<std::string> stopPins, failPins;  // 1-bit Output labels
    uint64_t maxCycles = 10000000;
    double hz = 0;  // clock rate to hold to; 0 = as fast as the simulation goes
    bool stats = false;
};

static std::string Quoted(const std::string& text) { return "\"" + text + "\""; }

// Runs a command line whose arguments are already quoted. Its output goes to stderr, so
// stdout carries only what the program prints.
static int RunTool(const std::string& commandLine)
{
#ifdef _WIN32
    return std::system(("\"" + commandLine + " 1>&2\"").c_str());  // cmd.exe strips one outer pair of quotes
#else
    return std::system((commandLine + " 1>&2").c_str());
#endif
}

// Turns a .c or .s into a .bin in a temporary folder. Returns the .bin, or "" (after saying why).
static std::string BuildProgram(const std::string& program, const fs::path& toolDirectory)
{
    fs::path source = fs::absolute(program);
    std::string extension = source.extension().string();
    if (extension == ".bin") return source.string();
    if (extension != ".c" && extension != ".s") {
        fprintf(stderr, "%s: expected a .c, .s or .bin file\n", program.c_str());
        return "";
    }
    std::error_code error;
    fs::path work = fs::temp_directory_path(error) / "isa16-run";
    fs::create_directories(work, error);
    fs::path assembly = source, binary = work / (source.stem().string() + ".bin");
    if (extension == ".c") {
        assembly = work / (source.stem().string() + ".s");
        std::string compiler = (toolDirectory / "Compiler").string();
        if (RunTool(Quoted(compiler) + " -o " + Quoted(assembly.string()) + " " + Quoted(source.string())) != 0) {
            fprintf(stderr, "compiling %s failed\n", program.c_str());
            return "";
        }
    }
    std::string assembler = (toolDirectory / "Assembler").string();
    if (RunTool(Quoted(assembler) + " -o " + Quoted(binary.string()) + " " + Quoted(assembly.string())) != 0) {
        fprintf(stderr, "assembling %s failed\n", program.c_str());
        return "";
    }
    return binary.string();
}

// Exit status: 0 stopped on a --until pin, 1 a --fail-on pin went high, 2 a usage or load
// problem, 3 the cycle limit, 4 the circuit oscillates.
static int RunHeadless(const RunOptions& options, const fs::path& toolDirectory)
{
    std::vector<std::string> problems = LoadLibrary();
    for (const std::string& problem : problems) fprintf(stderr, "%s\n", problem.c_str());
    Component* component = FindComponent(options.component);
    if (!component) {
        fprintf(stderr, "no component named '%s' in %s\n", options.component.c_str(), componentsDirectory.c_str());
        return 2;
    }

    if (!options.program.empty()) {
        std::string binary = BuildProgram(options.program, toolDirectory);
        if (binary.empty()) return 2;
        int loaded = 0;
        for (Node& node : component->nodes) {
            if (node.kind != NodeKind::Ram || !node.memory || node.memory->image.empty()) continue;
            node.memory->image = binary;  // absolute, so the components directory doesn't matter
            std::string problem = LoadRomImage(*node.memory);
            if (!problem.empty()) {
                fprintf(stderr, "%s\n", problem.c_str());
                return 2;
            }
            loaded++;
        }
        if (!loaded) {
            fprintf(stderr, "'%s' has no RAM with an image to load the program into\n", component->name.c_str());
            return 2;
        }
    }

    // With no --until or --fail-on, stop on HALTED and fail on ILLEGAL, if the component has them.
    std::vector<std::string> stopPins = options.stopPins, failPins = options.failPins;
    auto hasOutput = [&](const std::string& label) {
        for (const Node* node : PinNodes(*component, NodeKind::Output))
            if (node->label == label && node->width == 1) return true;
        return false;
    };
    if (stopPins.empty() && failPins.empty()) {
        if (hasOutput("HALTED")) stopPins.push_back("HALTED");
        if (hasOutput("ILLEGAL")) failPins.push_back("ILLEGAL");
    }
    for (const std::vector<std::string>* pins : {&stopPins, &failPins})
        for (const std::string& label : *pins)
            if (!hasOutput(label)) {
                fprintf(stderr, "'%s' has no 1-bit Output named '%s'\n", component->name.c_str(), label.c_str());
                return 2;
            }

    Simulation simulation;
    BuildSimulation(simulation, *component);
    if (simulation.missingComponents) fprintf(stderr, "%d missing component(s) left out\n", simulation.missingComponents);
    // Stdin (a pipe or the console) is read a line at a time, only once the program asks for a
    // key that isn't there, so a program that never reads doesn't wait on input.
    bool endOfInput = false;
    size_t starvedAt = 0;
    int starvedPolls = 0;

    auto firstHigh = [&](const std::vector<std::string>& labels) -> const std::string* {
        for (const std::string& label : labels)
            if (ReadOutput(simulation, *component, label.c_str())) return &label;
        return nullptr;
    };
    size_t printed = 0;
    auto flushConsole = [&] {
        if (simulation.console.size() == printed) return;
        fwrite(simulation.console.data() + printed, 1, simulation.console.size() - printed, stdout);
        fflush(stdout);
        printed = simulation.console.size();
    };

    auto start = std::chrono::steady_clock::now();
    uint64_t cycles = 0;
    int status = 3;
    std::string reason = "the cycle limit";
    // --hz paces the clock against wall time from paceStart; waiting on input restarts it.
    auto paceStart = start;
    uint64_t paceCycles = 0;
    double waitedSeconds = 0;
    for (;;) {
        if (const std::string* pin = firstHigh(failPins)) { status = 1; reason = *pin; break; }
        if (const std::string* pin = firstHigh(stopPins)) { status = 0; reason = *pin; break; }
        if (simulation.unstable) { status = 4; reason = "an oscillation"; break; }
        if (simulation.keysWanted) {
            simulation.keysWanted = false;
            // Printing polls the status register too, so the program only counts as waiting for
            // a key once it has gone on polling without printing anything.
            if (simulation.console.size() != starvedAt) starvedAt = simulation.console.size(), starvedPolls = 0;
            if (simulation.keys.empty() && ++starvedPolls > 100) {
                starvedPolls = 0;
                if (endOfInput) { status = 0; reason = "the end of input"; break; }
                flushConsole();  // show the prompt before waiting on a console line
                auto waitStart = std::chrono::steady_clock::now();
                int character;
                while ((character = getchar()) != EOF) {
                    simulation.keys.push_back(static_cast<uint8_t>(character));
                    if (character == '\n') break;
                }
                if (character == EOF) endOfInput = true;
                paceStart = std::chrono::steady_clock::now(), paceCycles = cycles;
                waitedSeconds += std::chrono::duration<double>(paceStart - waitStart).count();
            }
        }
        if (cycles >= options.maxCycles) break;
        HalfStep(simulation);
        HalfStep(simulation);
        cycles++;
        flushConsole();
        if (options.hz > 0) {
            auto due = paceStart + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                       std::chrono::duration<double>((cycles - paceCycles) / options.hz));
            if (due > std::chrono::steady_clock::now()) std::this_thread::sleep_until(due);
        }
    }
    flushConsole();

    // Time spent waiting for a typed line isn't the machine's.
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() - waitedSeconds;
    if (status != 0 || options.stats)
        fprintf(stderr, "%s: stopped on %s after %llu cycles (%.1f s, %.0f Hz; %zu gates, slowest settle %d steps, %d conflicting nets)\n",
                component->name.c_str(), reason.c_str(), static_cast<unsigned long long>(cycles), seconds,
                seconds > 0 ? cycles / seconds : 0.0, simulation.gates.size(), simulation.slowestSettleSteps,
                simulation.conflicts);
    return status;
}

static int Usage(const char* program, int status)
{
    fprintf(stderr,
            "usage: %s [--components DIR] [--self-test] [--check-hardware] [--screenshot FILE] [--simulate]\n"
            "       %s [--components DIR] --run COMPONENT [--program FILE] [--until PIN]... [--fail-on PIN]...\n"
            "                             [--max-cycles N] [--stats]\n"
            "--run simulates COMPONENT without a window: its Terminal prints to stdout, and piped stdin is typed into it.\n"
            "  --program FILE   a .c, .s or .bin, built and loaded into every RAM that has an image\n"
            "  --until PIN      stop (status 0) when this 1-bit Output goes high (default HALTED)\n"
            "  --fail-on PIN    stop (status 1) when this one does (default ILLEGAL)\n"
            "  --max-cycles N   give up after N clock cycles (status 3; default 10000000)\n"
            "  --hz N           hold the clock to N cycles per second (if the simulation can keep up)\n"
            "  --stats          report cycles, time, clock rate and gate counts on stderr\n",
            program, program);
    return status;
}

int main(int argc, char** argv)
{
    const char* screenshotPath = nullptr;
    bool simulateOnStart = false;
    RunOptions run;

    for (int index = 1; index < argc; index++) {
        std::string argument = argv[index];
        if (argument == "--self-test") return RunSelfTest();
        if (argument == "--check-hardware") return RunHardwareCheck();
        if (argument == "--components" && index + 1 < argc) {
            componentsDirectory = argv[++index];
            if (componentsDirectory.back() != '/' && componentsDirectory.back() != '\\') componentsDirectory += '/';
        } else if (argument == "--screenshot" && index + 1 < argc) {
            screenshotPath = argv[++index];
        } else if (argument == "--simulate") {
            simulateOnStart = true;
        } else if (argument == "--run" && index + 1 < argc) {
            run.component = argv[++index];
        } else if (argument == "--program" && index + 1 < argc) {
            run.program = argv[++index];
        } else if (argument == "--until" && index + 1 < argc) {
            run.stopPins.push_back(argv[++index]);
        } else if (argument == "--fail-on" && index + 1 < argc) {
            run.failPins.push_back(argv[++index]);
        } else if (argument == "--max-cycles" && index + 1 < argc) {
            run.maxCycles = std::strtoull(argv[++index], nullptr, 0);
        } else if (argument == "--hz" && index + 1 < argc) {
            run.hz = std::strtod(argv[++index], nullptr);
        } else if (argument == "--stats") {
            run.stats = true;
        } else if (argument == "--help" || argument == "-h") {
            return Usage(argv[0], 0);
        } else {
            return Usage(argv[0], 2);
        }
    }

    if (!run.component.empty()) return RunHeadless(run, fs::absolute(argv[0]).parent_path());
    if (!run.program.empty() || !run.stopPins.empty() || !run.failPins.empty() || run.stats) return Usage(argv[0], 2);

    std::vector<std::string> problems = LoadLibrary();
    for (const std::string& problem : problems) fprintf(stderr, "%s\n", problem.c_str());
    if (library.empty()) CreateComponent("Untitled");

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    InitWindow(1440, 880, "ISA-16 Circuit Workbench");
    SetExitKey(KEY_NULL);
    SetTargetFPS(60);

    rlImGuiSetup(true);
    ImGui::GetIO().IniFilename = nullptr;
    ImNodes::CreateContext();
    ImNodes::StyleColorsDark();
    ImNodes::GetStyle().Flags |= ImNodesStyleFlags_GridLines;
    app.baseNodeStyle = ImNodes::GetStyle();
    app.simulation.randomRam = true;

    std::vector<Component*> sorted;
    for (auto& component : library) sorted.push_back(component.get());
    std::sort(sorted.begin(), sorted.end(), [](Component* a, Component* b) { return a->name < b->name; });
    OpenComponent(sorted.front());
    if (!problems.empty()) SetStatus("%s", problems.front().c_str());
    if (simulateOnStart) EnterSimulation();

    int frame = 0;
    while (!app.quit) {
        if (WindowShouldClose()) app.quitRequested = true;
        RunClock(GetFrameTime());

        BeginDrawing();
        ClearBackground(Color{24, 26, 30, 255});
        rlImGuiBegin();
        DrawFrame();
        rlImGuiEnd();
        EndDrawing();

        if (screenshotPath && ++frame == 10) {
            TakeScreenshot(screenshotPath);
            break;
        }
    }

    for (auto& component : library)
        if (component->editor) ImNodes::EditorContextFree(component->editor);
    ImNodes::DestroyContext();
    rlImGuiShutdown();
    CloseWindow();
    return 0;
}