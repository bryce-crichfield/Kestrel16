// Circuit.cpp -- the circuit model: primitives, components, pins and wires.

#include "Circuit.h"
#include <cctype>
#include <cstdlib>
#include <tuple>
#include <unordered_map>

#ifndef COMPONENTS_PATH
#define COMPONENTS_PATH "Components/"
#endif

namespace Kestrel {

const PrimitiveInfo Primitives[PrimitiveCount] = {
    {"Input", 0, 1, {}, {""}, "A pin into this component. Simulated on its own, it is a switch."},
    {"Output", 1, 0, {""}, {}, "A pin out of this component. Simulated on its own, it is an LED."},
    {"Low", 0, 1, {}, {"0"}, "Always 0 (tied to ground)."},
    {"High", 0, 1, {}, {"1"}, "Always 1 (tied to the supply)."},
    {"Clock", 0, 1, {}, {"clk"}, "The simulation clock."},
    {"Buffer", 1, 1, {"A"}, {"Y"}, "Y = A."},
    {"NOT", 1, 1, {"A"}, {"Y"}, "Y = not A.  (74HC04 has six)"},
    {"AND", 2, 1, {"A", "B"}, {"Y"}, "Y = A and B.  (74HC08 has four)"},
    {"OR", 2, 1, {"A", "B"}, {"Y"}, "Y = A or B.  (74HC32 has four)"},
    {"NAND", 2, 1, {"A", "B"}, {"Y"}, "Y = not (A and B).  (74HC00 has four)"},
    {"NOR", 2, 1, {"A", "B"}, {"Y"}, "Y = not (A or B).  (74HC02 has four)"},
    {"XOR", 2, 1, {"A", "B"}, {"Y"}, "Y = A xor B.  (74HC86 has four)"},
    {"XNOR", 2, 1, {"A", "B"}, {"Y"}, "Y = not (A xor B)."},
    {"D Flip-Flop", 2, 2, {"D", "CLK"}, {"Q", "/Q"}, "Q takes the value of D when CLK goes from 0 to 1.  (74HC74 has two)"},
    {"Tri-State Buffer", 2, 1, {"A", "/OE"}, {"Y"}, "Y = A while /OE is 0; with /OE at 1, Y lets go (Z) so something else can drive the wire.  (74HC125 has four)"},
    {"ROM", 0, 0, {}, {},
     "D = contents[A]: a lookup table, like an EEPROM (AT28C64/AT28C256). "
     "Use it for opcode decoding, or to hold a program. D lets go (Z) unless /CE and /OE "
     "are both 0; left unconnected they read 0."},
    {"RAM", 0, 0, {}, {},
     "Static RAM like the 62256 (32K x 8). D0.. appear on both sides: they are one "
     "bidirectional wire. /CE=1: D lets go. /WE=0: stores D at A every step it stays 0 "
     "(D isn't driven, whatever /OE says). /OE=0 and /WE=1: drives contents[A] on D. "
     "Contents exist only while simulating; Reset clears or randomizes them, or loads a "
     ".bin. Wire /WE: unconnected it reads 0, which means write."},
    {"Terminal", 0, 0, {}, {},
     "A UART standing in for the screen and keyboard, like the emulator's UART. "
     "RS=0 (wire A1): write prints the byte on D, read takes the next typed key (0 if none). "
     "RS=1: read gives status, bit 0 = a key is waiting, bit 1 = ready to print. "
     "D0-D7 are on both sides, like a RAM's. A write prints when /WE (or /CE) goes back "
     "to 1; a key is used up when the read ends. IRQ is 1 while a key is waiting."},
};

static const char* const TerminalControlNames[TerminalControlPins] = {"RS", "/CE", "/OE", "/WE"};

// ---------------------------------------------------------------------------
// Components

std::vector<std::unique_ptr<Component>> library;
std::string componentsDirectory = COMPONENTS_PATH;

Component* FindComponent(const std::string& name) {
    for (auto& component : library) {
        if (component->name == name) {
            return component.get();
        }
    }
    return nullptr;
}

std::vector<const Node*> PinNodes(const Component& component, NodeKind side) {
    std::vector<const Node*> pins;
    for (const Node& node : component.nodes) {
        if (node.kind == side) {
            pins.push_back(&node);
        }
    }
    return pins;
}

std::vector<PinBit> PinBits(const Component& component, NodeKind side) {
    std::vector<PinBit> pins;
    for (const Node* node : PinNodes(component, side)) {
        for (int bit = 0; bit < node->width && static_cast<int>(pins.size()) < MaxPinsPerSide; bit++) {
            pins.push_back({node, bit});
        }
    }
    return pins;
}

static std::string BitLabel(const Node& node, int bit) {
    return node.width == 1 ? node.label : node.label + std::to_string(bit);
}

int InputCount(const Node& node) {
    if (node.kind == NodeKind::Terminal) {
        return TerminalControlPins + TerminalDataBits;
    }
    if (node.kind == NodeKind::Input) {
        return 0;
    }
    if (node.kind == NodeKind::Output) {
        return node.width;
    }
    if (node.kind == NodeKind::Rom) {
        return node.memory ? node.memory->addressBits + RomControlPins : 0;
    }
    if (node.kind == NodeKind::Ram) {
        return node.memory ? node.memory->addressBits + RamControlPins + node.memory->dataBits : 0;
    }
    if (IsPrimitive(node.kind)) {
        return Primitives[static_cast<int>(node.kind)].inputCount;
    }
    const Component* type = FindComponent(node.typeName);
    return type ? static_cast<int>(PinBits(*type, NodeKind::Input).size()) : 0;
}

int OutputCount(const Node& node) {
    if (node.kind == NodeKind::Terminal) {
        return TerminalDataBits + 1;  // and IRQ
    }
    if (node.kind == NodeKind::Input) {
        return node.width;
    }
    if (node.kind == NodeKind::Output) {
        return 0;
    }
    if (IsMemory(node.kind)) {
        return node.memory ? node.memory->dataBits : 0;
    }
    if (IsPrimitive(node.kind)) {
        return Primitives[static_cast<int>(node.kind)].outputCount;
    }
    const Component* type = FindComponent(node.typeName);
    return type ? static_cast<int>(PinBits(*type, NodeKind::Output).size()) : 0;
}

std::string PinName(const Node& node, bool output, int index) {
    if (node.kind == NodeKind::Input || node.kind == NodeKind::Output) {
        return BitLabel(node, index);
    }
    if (node.kind == NodeKind::Terminal) {
        if (output) {
            return index < TerminalDataBits ? "D" + std::to_string(index) : "IRQ";
        }
        return index < TerminalControlPins ? TerminalControlNames[index] : "D" + std::to_string(index - TerminalControlPins);
    }
    if (IsMemory(node.kind)) {
        static const char* const ControlNames[] = {"/CE", "/OE", "/WE"};
        int control = output ? -1 : index - node.memory->addressBits;
        int controls = node.kind == NodeKind::Rom ? RomControlPins : RamControlPins;
        if (control >= 0 && control < controls) {
            return ControlNames[control];
        }
        if (control >= controls) {  // a RAM's data pins, input side
            output = true;
            index = control - controls;
        }
        const std::vector<std::string>& names = output ? node.memory->dataNames : node.memory->addressNames;
        if (index < static_cast<int>(names.size()) && !names[index].empty()) {
            return names[index];
        }
        return (output ? "D" : "A") + std::to_string(index);
    }
    if (IsPrimitive(node.kind)) {
        const PrimitiveInfo& info = Primitives[static_cast<int>(node.kind)];
        return output ? info.outputNames[index] : info.inputNames[index];
    }
    const Component* type = FindComponent(node.typeName);
    if (!type) {
        return "?";
    }
    std::vector<PinBit> pins = PinBits(*type, output ? NodeKind::Output : NodeKind::Input);
    return index < static_cast<int>(pins.size()) ? BitLabel(*pins[index].node, pins[index].bit) : "?";
}

std::string NodeTitle(const Node& node) {
    switch (node.kind) {
        case NodeKind::Input:
            return "IN  " + node.label + (node.width > 1 ? " [" + std::to_string(node.width) + "]" : "");
        case NodeKind::Output:
            return "OUT " + node.label + (node.width > 1 ? " [" + std::to_string(node.width) + "]" : "");
        case NodeKind::Component:
            return FindComponent(node.typeName) ? node.typeName : "Missing: " + node.typeName;
        case NodeKind::Ram:
            return "RAM " + std::to_string(size_t{1} << node.memory->addressBits) + " x " + std::to_string(node.memory->dataBits);
        case NodeKind::Rom:
            return "ROM " + std::to_string(size_t{1} << node.memory->addressBits) + " x " + std::to_string(node.memory->dataBits);
        default:
            return Primitives[static_cast<int>(node.kind)].name;
    }
}

static bool SplitBitName(const std::string& name, std::string& prefix, int& number) {
    size_t digits = name.size();
    while (digits > 0 && std::isdigit(static_cast<unsigned char>(name[digits - 1]))) {
        digits--;
    }
    if (digits == name.size() || digits == 0 || name.size() - digits > 3) {
        return false;
    }
    prefix = name.substr(0, digits);
    number = atoi(name.c_str() + digits);
    return true;
}

std::vector<PinGroup> PinGroups(const Node& node, bool output) {
    std::vector<PinGroup> groups;
    int count = output ? OutputCount(node) : InputCount(node);
    std::string prefix, previous;
    int number = 0, previousNumber = -2;
    for (int index = 0; index < count; index++) {
        bool numbered = SplitBitName(PinName(node, output, index), prefix, number);
        if (numbered && !groups.empty() && prefix == previous && number == previousNumber + 1) {
            groups.back().count++;
        }
        else {
            groups.push_back({index, 1});
        }
        previous = numbered ? prefix : "";
        previousNumber = numbered ? number : -2;
    }
    return groups;
}

PinGroup GroupOfPin(const std::vector<PinGroup>& groups, int pin) {
    for (const PinGroup& group : groups) {
        if (pin >= group.first && pin < group.first + group.count) {
            return group;
        }
    }
    return {pin, 1};
}

// Joins wires that run side by side (A0 -> B0, A1 -> B1, ...) into one bus wire. A bus
// stays within one group at each end.
void MergeBusWires(Component& component) {
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
    for (const Wire& wire : component.wires) {
        if (wire.width == 1) {
            singles.push_back(wire);
        }
        else {
            result.push_back(wire);
        }
    }
    auto offset = [](const Wire& wire) { return static_cast<int64_t>(wire.toPin) - static_cast<int64_t>(wire.fromPin); };
    std::sort(singles.begin(), singles.end(), [&](const Wire& a, const Wire& b) {
        return std::make_tuple(a.fromNode, a.toNode, offset(a), a.fromPin) <
               std::make_tuple(b.fromNode, b.toNode, offset(b), b.fromPin);
    });
    size_t start = 0;
    while (start < singles.size()) {
        const Wire& first = singles[start];
        int fromGroup = groupFirst(first.fromNode, true, first.fromPin);
        int toGroup = groupFirst(first.toNode, false, first.toPin);
        // A wire joins the run when it connects the same two nodes, on the next pin along at
        // both ends, without crossing into another pin group.
        auto continuesRun = [&](const Wire& next, size_t position) {
            bool sameNodes = next.fromNode == first.fromNode && next.toNode == first.toNode;
            bool nextPin = offset(next) == offset(first) && next.fromPin == first.fromPin + position;
            bool sameGroups = groupFirst(next.fromNode, true, next.fromPin) == fromGroup &&
                              groupFirst(next.toNode, false, next.toPin) == toGroup;
            return sameNodes && nextPin && sameGroups;
        };
        size_t end = start + 1;
        while (end < singles.size() && continuesRun(singles[end], end - start)) {
            end++;
        }
        Wire merged = first;
        merged.width = static_cast<uint32_t>(end - start);
        result.push_back(merged);
        start = end;
    }
    std::sort(result.begin(), result.end(), [](const Wire& a, const Wire& b) { return a.id < b.id; });
    component.wires = std::move(result);
}

// Does `component` use `target` anywhere inside it?
bool Uses(const Component& component, const std::string& target, int depth) {
    if (depth > MaxNestingDepth) {
        return true;  // treat runaway nesting as a cycle
    }
    for (const Node& node : component.nodes) {
        if (node.kind != NodeKind::Component) {
            continue;
        }
        if (node.typeName == target) {
            return true;
        }
        const Component* child = FindComponent(node.typeName);
        if (child && Uses(*child, target, depth + 1)) {
            return true;
        }
    }
    return false;
}

std::string UniquePinLabel(const Component& component, NodeKind side) {
    const char* prefix = side == NodeKind::Input ? "In" : "Out";
    for (int number = 1;; number++) {
        std::string label = prefix + std::to_string(number);
        bool taken = false;
        for (const Node& node : component.nodes) {
            if ((node.kind == NodeKind::Input || node.kind == NodeKind::Output) && node.label == label) {
                taken = true;
            }
        }
        if (!taken) {
            return label;
        }
    }
}

uint32_t AddNode(Component& component, NodeKind kind, const std::string& typeName, const std::string& label) {
    Node node;
    node.id = component.nextId++;
    node.kind = kind;
    node.typeName = typeName;
    if (kind == NodeKind::Input || kind == NodeKind::Output) {
        node.label = label.empty() ? UniquePinLabel(component, kind) : label;
    }
    if (IsMemory(kind)) {
        node.memory = std::make_shared<MemoryData>();
        if (kind == NodeKind::Ram) {
            node.memory->Resize(15, 8);  // a 62256
        }
        else {
            node.memory->Resize(node.memory->addressBits, node.memory->dataBits);
        }
    }
    node.placement = Node::Placement::Grid;
    node.position = ImVec2(40.0f + 30.0f * static_cast<float>(component.nodes.size() % 10),
                           40.0f + 30.0f * static_cast<float>(component.nodes.size() % 10));
    component.nodes.push_back(node);
    component.dirty = true;
    return node.id;
}

void Connect(Component& component, uint32_t fromNode, int fromPin, uint32_t toNode, int toPin, int width) {
    auto& wires = component.wires;
    for (const Wire& wire : wires) {
        bool sameFrom = wire.fromNode == fromNode && wire.fromPin == static_cast<uint32_t>(fromPin);
        bool sameTo = wire.toNode == toNode && wire.toPin == static_cast<uint32_t>(toPin);
        bool sameWidth = wire.width == static_cast<uint32_t>(width);
        if (sameFrom && sameTo && sameWidth) {
            return;  // already connected
        }
    }
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

void RemoveNode(Component& component, uint32_t id) {
    auto& nodes = component.nodes;
    nodes.erase(std::remove_if(nodes.begin(), nodes.end(), [&](const Node& node) { return node.id == id; }),
                nodes.end());
    auto& wires = component.wires;
    wires.erase(std::remove_if(wires.begin(), wires.end(),
                               [&](const Wire& wire) { return wire.fromNode == id || wire.toNode == id; }),
                wires.end());
    component.dirty = true;
}

void RemoveWire(Component& component, uint32_t id) {
    auto& wires = component.wires;
    wires.erase(std::remove_if(wires.begin(), wires.end(), [&](const Wire& wire) { return wire.id == id; }),
                wires.end());
    component.dirty = true;
}

// Drops wires whose pins no longer exist (for example after a used component lost a pin).
void PruneWires(Component& component) {
    auto& wires = component.wires;
    size_t before = wires.size();
    auto isBroken = [&](const Wire& wire) {
        const Node* from = component.FindNode(wire.fromNode);
        const Node* to = component.FindNode(wire.toNode);
        if (!from || !to || wire.width == 0) {
            return true;
        }
        bool fromFits = static_cast<int>(wire.fromPin + wire.width) <= OutputCount(*from);
        bool toFits = static_cast<int>(wire.toPin + wire.width) <= InputCount(*to);
        return !fromFits || !toFits;
    };
    wires.erase(std::remove_if(wires.begin(), wires.end(), isBroken), wires.end());
    if (wires.size() != before) {
        component.dirty = true;
    }
}

// After a ROM or RAM changes size, moves wires on its control and data-in pins to where those
// pins now are, and drops wires on pins that went away.
void RemapMemoryWires(Component& component, const Node& node, int oldAddressBits) {
    int controls = node.kind == NodeKind::Rom ? RomControlPins : RamControlPins;
    int newAddressBits = node.memory->addressBits, newDataBits = node.memory->dataBits;
    auto& wires = component.wires;
    auto remap = [&](int pin) {
        if (pin < oldAddressBits) {
            return pin < newAddressBits ? pin : -1;
        }
        if (pin < oldAddressBits + controls) {
            return pin - oldAddressBits + newAddressBits;
        }
        return pin - oldAddressBits - controls < newDataBits ? pin - oldAddressBits + newAddressBits : -1;
    };
    for (Wire& wire : wires) {
        if (wire.toNode != node.id) {
            continue;
        }
        int first = remap(static_cast<int>(wire.toPin)), last = remap(static_cast<int>(wire.toPin + wire.width) - 1);
        bool keep = first >= 0 && last - first == static_cast<int>(wire.width) - 1;  // a bus stays in one piece
        wire.toPin = keep ? static_cast<uint32_t>(first) : UINT32_MAX;
    }
    wires.erase(std::remove_if(wires.begin(), wires.end(), [](const Wire& wire) { return wire.toPin == UINT32_MAX; }),
                wires.end());
    PruneWires(component);  // outputs that went away
    component.dirty = true;
}

Component* CreateComponent(const std::string& name) {
    auto component = std::make_unique<Component>();
    component->name = name;
    component->dirty = true;
    library.push_back(std::move(component));
    return library.back().get();
}

std::string UniqueComponentName(const std::string& base) {
    if (!FindComponent(base)) {
        return base;
    }
    for (int number = 2;; number++) {
        std::string name = base + " " + std::to_string(number);
        if (!FindComponent(name)) {
            return name;
        }
    }
}

bool IsValidComponentName(const std::string& name) {
    if (name.empty() || name.size() >= MaxNameLength) {
        return false;
    }
    if (name.front() == ' ' || name.back() == ' ') {
        return false;
    }
    for (char character : name) {
        if (!isalnum(static_cast<unsigned char>(character)) && character != ' ' && character != '_' && character != '-') {
            return false;
        }
    }
    return true;
}

// Renames a component and every reference to it in the library.
void RenameComponent(Component& component, const std::string& newName) {
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

}  // namespace Kestrel
