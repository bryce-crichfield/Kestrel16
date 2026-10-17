// EditorCanvas.cpp -- the node canvas: nodes, pins, wires, zoom, and the add-node menu.

#include "ComponentFile.h"
#include "EditorInternal.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace Kestrel {

// Pin attribute ids for imnodes: unique per pin within a component.
static int PinId(uint32_t node, bool output, int index) {
    return static_cast<int>(node) * (2 * MaxPinsPerSide) + (output ? MaxPinsPerSide : 0) + index;
}

// A bus pin (one pin standing for a group) has the negative of its first bit's id, less one.
static int BusPinId(uint32_t node, bool output, int first) {
    return -1 - PinId(node, output, first);
}

static void DecodePinId(int pinId, uint32_t& node, bool& output, int& index, bool* bus = nullptr) {
    if (bus) {
        *bus = pinId < 0;
    }
    if (pinId < 0) {
        pinId = -1 - pinId;
    }
    node = static_cast<uint32_t>(pinId / (2 * MaxPinsPerSide));
    output = (pinId % (2 * MaxPinsPerSide)) >= MaxPinsPerSide;
    index = pinId % MaxPinsPerSide;
}

// Pins with a one-bit wire on them, by PinId. A bus with any of these shows its bits.
static std::unordered_set<int> singleWirePins;

static void FindSingleWirePins(const Component& component) {
    singleWirePins.clear();
    for (const Wire& wire : component.wires) {
        if (wire.width != 1) {
            continue;
        }
        singleWirePins.insert(PinId(wire.fromNode, true, static_cast<int>(wire.fromPin)));
        singleWirePins.insert(PinId(wire.toNode, false, static_cast<int>(wire.toPin)));
    }
}

// Is this group drawn as one bus pin only? Otherwise its bits are listed under the bus pin.
static bool BusCollapsed(const Component& component, uint32_t node, bool output, const PinGroup& group) {
    if (group.count < 2 || component.expandedBuses.count(BusPinId(node, output, group.first))) {
        return false;
    }
    for (int pin = group.first; pin < group.first + group.count; pin++) {
        if (singleWirePins.count(PinId(node, output, pin))) {
            return false;
        }
    }
    return true;
}

// The imnodes attribute a wire end attaches to: the bus pin for a bus wire on a collapsed group
// or one spanning its whole group, else the first bit's own pin.
static int WireEndAttribute(const Component& component, const Node& node, bool output, int pin, int width) {
    PinGroup group = GroupOfPin(PinGroups(node, output), pin);
    if (group.count > 1 && (BusCollapsed(component, node.id, output, group) ||
                            (width > 1 && pin == group.first && width == group.count))) {
        return BusPinId(node.id, output, group.first);
    }
    return PinId(node.id, output, pin);
}

std::string BusText(const Simulation& simulation, const std::vector<int>& nets) {
    if (std::all_of(nets.begin(), nets.end(), [&](int net) { return simulation.Level(net) == Floating; })) {
        return "Z";
    }
    std::string text;
    for (int digit = (static_cast<int>(nets.size()) + 3) / 4 - 1; digit >= 0; digit--) {
        int value = 0;
        bool known = true;
        for (int bit = 0; bit < 4 && digit * 4 + bit < static_cast<int>(nets.size()); bit++) {
            uint8_t level = simulation.Level(nets[static_cast<size_t>(digit * 4 + bit)]);
            if (level == High) {
                value |= 1 << bit;
            }
            else if (level != Low) {
                known = false;
            }
        }
        text += known ? "0123456789ABCDEF"[value] : '?';
    }
    return text;
}

static uint8_t BusLevel(const Simulation& simulation, const std::vector<int>& nets) {
    bool anyHigh = false, allFloating = true;
    for (int net : nets) {
        uint8_t level = simulation.Level(net);
        if (level == Conflict) {
            return Conflict;
        }
        anyHigh |= level == High;
        allFloating &= level == Floating;
    }
    if (allFloating) {
        return Floating;
    }
    if (anyHigh) {
        return High;
    }
    return Low;
}

std::vector<int> PinNets(const Simulation& simulation, uint32_t node, bool output, int first, int count) {
    std::vector<int> nets;
    for (int pin = first; pin < first + count; pin++) {
        nets.push_back(output ? simulation.NetOfOutput(node, pin) : simulation.NetOfInput(node, pin));
    }
    return nets;
}

// Places a new node from the palette or the right-click menu.
static void PlaceNode(NodeKind kind, const std::string& typeName, ImVec2 screenPosition) {
    uint32_t id = AddNode(*app.current, kind, typeName);
    Node* node = app.current->FindNode(id);
    node->placement = Node::Placement::Screen;
    node->position = screenPosition;
}

static void PlaceNodeAtCenter(NodeKind kind, const std::string& typeName = "") {
    float offset = static_cast<float>(app.spawnCount++ % 8) * 18.0f;
    PlaceNode(kind, typeName, ImVec2(app.canvasCenter.x - 60 + offset, app.canvasCenter.y - 40 + offset));
}

static bool CanPlace(const Component& component) {
    return &component != app.current && !Uses(component, app.current->name);
}

// ---------------------------------------------------------------------------
// Colors

static const ImU32 ColorHigh = IM_COL32(90, 230, 110, 255);
static const ImU32 ColorLow = IM_COL32(70, 95, 80, 255);
static const ImU32 ColorFloating = IM_COL32(90, 110, 170, 255);
static const ImU32 ColorConflict = IM_COL32(255, 60, 60, 255);

ImU32 LevelColor(uint8_t level) {
    switch (level) {
        case High:
            return ColorHigh;
        case Floating:
            return ColorFloating;
        case Conflict:
            return ColorConflict;
        default:
            return ColorLow;
    }
}

const char* LevelText(uint8_t level) {
    static const char* const Texts[] = {"0", "1", "Z", "X (conflict)"};
    return Texts[level & 3];
}

static ImU32 TitleColor(const Node& node) {
    switch (node.kind) {
        case NodeKind::Input:
            return IM_COL32(40, 120, 70, 255);
        case NodeKind::Output:
            return IM_COL32(150, 60, 50, 255);
        case NodeKind::Component:
            return FindComponent(node.typeName) ? IM_COL32(50, 85, 150, 255) : IM_COL32(170, 40, 40, 255);
        case NodeKind::Clock:
            return IM_COL32(150, 110, 30, 255);
        case NodeKind::FlipFlop:
            return IM_COL32(110, 70, 140, 255);
        default:
            return IM_COL32(75, 75, 85, 255);
    }
}

static ImU32 Lighten(ImU32 color, int amount) {
    int r = std::min(255, static_cast<int>((color >> IM_COL32_R_SHIFT) & 0xFF) + amount);
    int g = std::min(255, static_cast<int>((color >> IM_COL32_G_SHIFT) & 0xFF) + amount);
    int b = std::min(255, static_cast<int>((color >> IM_COL32_B_SHIFT) & 0xFF) + amount);
    return IM_COL32(r, g, b, 255);
}

// ---------------------------------------------------------------------------
// Canvas

void DrawLed(bool on) {
    float radius = ImGui::GetTextLineHeight() * 0.5f;
    ImVec2 corner = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(radius * 2, radius * 2));
    ImVec2 center(corner.x + radius, corner.y + radius);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddCircleFilled(center, radius, on ? IM_COL32(255, 70, 50, 255) : IM_COL32(70, 25, 25, 255));
    if (on) {
        drawList->AddCircle(center, radius + 2, IM_COL32(255, 120, 90, 120), 0, 2.0f);
    }
}

static void DrawNode(Node& node) {
    Component& component = *app.current;
    Simulation& simulation = app.simulation;
    bool simulating = app.mode == Mode::Simulate;

    // Positions are kept at zoom 1, so they're handed to imnodes scaled every frame.
    float zoom = component.zoom;
    if (node.placement == Node::Placement::Screen) {
        ImNodes::SetNodeScreenSpacePos(static_cast<int>(node.id), node.position);
    }
    else {
        ImNodes::SetNodeGridSpacePos(static_cast<int>(node.id), ImVec2(node.position.x * zoom, node.position.y * zoom));
    }
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
    for (int index = 0; index < inputs; index++) {
        width = std::max(width, ImGui::CalcTextSize(PinName(node, false, index).c_str()).x + 30 * zoom);
    }
    for (int index = 0; index < outputs; index++) {
        width = std::max(width, ImGui::CalcTextSize(PinName(node, true, index).c_str()).x + 30 * zoom);
    }
    for (bool output : {false, true}) {
        for (const PinGroup& group : PinGroups(node, output)) {
            if (group.count < 2) {
                continue;
            }
            std::string name = PinName(node, output, group.first) + ".." + PinName(node, output, group.first + group.count - 1);
            if (simulating) {
                name += " = " + std::string(static_cast<size_t>((group.count + 3) / 4), '0');
            }
            width = std::max(width, ImGui::CalcTextSize(name.c_str()).x + 40 * zoom);
        }
    }
    width = std::max(width, 60.0f * zoom);

    if (simulating && node.kind == NodeKind::Input && node.width == 1) {
        uint32_t value = SwitchValue(simulation, node.id);
        ImGui::PushID(static_cast<int>(node.id));
        ImGui::PushStyleColor(ImGuiCol_Button, value ? ColorHigh : ColorLow);
        if (ImGui::Button(value ? " 1 " : " 0 ")) {
            SetSwitch(simulation, node.id, !value);
        }
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
    if (simulating && node.kind == NodeKind::Input && node.width > 1) {
        uint32_t value = SwitchValue(simulation, node.id);
        ImGui::PushID(static_cast<int>(node.id));
        ImGui::SetNextItemWidth(ImGui::CalcTextSize("00000000").x + 10 * zoom);
        if (ImGui::InputScalar("##value", ImGuiDataType_U32, &value, nullptr, nullptr, "%X",
                               ImGuiInputTextFlags_CharsHexadecimal)) {
            SetSwitch(simulation, node.id, value);
        }
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
        for (int bit = 0; bit < node.memory->addressBits; bit++) {
            address |= static_cast<size_t>(simulation.Value(simulation.NetOfInput(node.id, bit))) << bit;
        }
        const std::vector<uint32_t>* words = &node.memory->words;
        if (node.kind == NodeKind::Ram) {
            auto found = simulation.topRams.find(node.id);
            words = found == simulation.topRams.end() ? nullptr : &simulation.rams[static_cast<size_t>(found->second)].words;
        }
        if (words) {
            ImGui::TextDisabled("[%zX] = %X", address, static_cast<unsigned>((*words)[address]));
        }
    }

    // One pin. A bus pin is square, and has a +/- to show or hide its bits.
    auto drawPin = [&](bool output, int first, int count, bool bus, bool collapsed) {
        int attribute = bus ? BusPinId(node.id, output, first) : PinId(node.id, output, first);
        std::string name = PinName(node, output, first);
        if (bus) {
            name += ".." + PinName(node, output, first + count - 1);
        }
        if (simulating) {
            std::vector<int> nets = PinNets(simulation, node.id, output, first, count);
            ImU32 color = LevelColor(BusLevel(simulation, nets));
            ImNodes::PushColorStyle(ImNodesCol_Pin, color);
            ImNodes::PushColorStyle(ImNodesCol_PinHovered, color);
            if (bus) {
                name += " = " + BusText(simulation, nets);
            }
        }
        ImNodesPinShape shape = bus ? ImNodesPinShape_QuadFilled : ImNodesPinShape_CircleFilled;
        if (output) {
            ImNodes::BeginOutputAttribute(attribute, shape);
        }
        else {
            ImNodes::BeginInputAttribute(attribute, shape);
        }
        float buttonWidth = 0;
        if (bus) {
            bool forced = !collapsed && !component.expandedBuses.count(attribute);  // single wires on its bits
            ImGui::PushID(attribute);
            ImGui::BeginDisabled(forced);
            if (!output && ImGui::SmallButton(collapsed ? "+" : "-")) {
                if (collapsed) {
                    component.expandedBuses.insert(attribute);
                }
                else {
                    component.expandedBuses.erase(attribute);
                }
            }
            if (!output) {
                ImGui::SameLine();
            }
            if (output) {
                buttonWidth = ImGui::CalcTextSize("+").x + ImGui::GetStyle().FramePadding.x * 2 +
                              ImGui::GetStyle().ItemSpacing.x;
            }
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
                    if (collapsed) {
                        component.expandedBuses.insert(attribute);
                    }
                    else {
                        component.expandedBuses.erase(attribute);
                    }
                }
                ImGui::EndDisabled();
                ImGui::PopID();
            }
        }
        else {
            ImGui::TextUnformatted(name.c_str());
        }
        if (output) {
            ImNodes::EndOutputAttribute();
        }
        else {
            ImNodes::EndInputAttribute();
        }
        if (simulating) {
            ImNodes::PopColorStyle();
            ImNodes::PopColorStyle();
        }
    };
    for (bool output : {false, true}) {
        for (const PinGroup& group : PinGroups(node, output)) {
            bool collapsed = BusCollapsed(component, node.id, output, group);
            if (group.count > 1) {
                drawPin(output, group.first, group.count, true, collapsed);
            }
            if (collapsed) {
                continue;
            }
            if (group.count > 1) {
                ImGui::Indent(10 * zoom);
            }
            for (int pin = group.first; pin < group.first + group.count; pin++) {
                drawPin(output, pin, 1, false, false);
            }
            if (group.count > 1) {
                ImGui::Unindent(10 * zoom);
            }
        }
    }
    if (inputs == 0 && outputs == 0) {
        ImGui::Dummy(ImVec2(width, 1));
    }

    ImNodes::EndNode();
    ImNodes::PopColorStyle();
    ImNodes::PopColorStyle();
    ImNodes::PopColorStyle();
}

void DrawAddNodeMenu(ImVec2 screenPosition, bool atCenter) {
    auto place = [&](NodeKind kind, const std::string& typeName) {
        if (atCenter) {
            PlaceNodeAtCenter(kind, typeName);
        }
        else {
            PlaceNode(kind, typeName, screenPosition);
        }
    };

    ImGui::SeparatorText("Pins");
    for (NodeKind kind : {NodeKind::Input, NodeKind::Output}) {
        if (ImGui::MenuItem(Primitives[static_cast<int>(kind)].name)) {
            place(kind, "");
        }
        ImGui::SetItemTooltip("%s", Primitives[static_cast<int>(kind)].description);
    }
    ImGui::SeparatorText("Gates");
    for (NodeKind kind : {NodeKind::Not, NodeKind::And, NodeKind::Or, NodeKind::Nand, NodeKind::Nor, NodeKind::Xor,
                          NodeKind::Xnor, NodeKind::Buffer, NodeKind::TriState}) {
        if (ImGui::MenuItem(Primitives[static_cast<int>(kind)].name)) {
            place(kind, "");
        }
        ImGui::SetItemTooltip("%s", Primitives[static_cast<int>(kind)].description);
    }
    ImGui::SeparatorText("Memory and timing");
    for (NodeKind kind : {NodeKind::FlipFlop, NodeKind::Rom, NodeKind::Ram, NodeKind::Clock, NodeKind::Low,
                          NodeKind::High}) {
        if (ImGui::MenuItem(Primitives[static_cast<int>(kind)].name)) {
            place(kind, "");
        }
        ImGui::SetItemTooltip("%s", Primitives[static_cast<int>(kind)].description);
    }
    ImGui::SeparatorText("Input/output");
    if (ImGui::MenuItem(Primitives[static_cast<int>(NodeKind::Terminal)].name)) {
        place(NodeKind::Terminal, "");
    }
    ImGui::SetItemTooltip("%s", Primitives[static_cast<int>(NodeKind::Terminal)].description);
    ImGui::SeparatorText("Components");
    bool any = false;
    for (auto& component : library) {
        if (component.get() == app.current) {
            continue;
        }
        any = true;
        bool allowed = CanPlace(*component);
        if (ImGui::MenuItem(component->name.c_str(), nullptr, false, allowed)) {
            place(NodeKind::Component, component->name);
        }
        if (!allowed) {
            ImGui::SetItemTooltip("It uses '%s', so it can't go inside it", app.current->name.c_str());
        }
        else if (!component->description.empty()) {
            ImGui::SetItemTooltip("%s", component->description.c_str());
        }
    }
    if (!any) {
        ImGui::TextDisabled("(none yet)");
    }
}

void DeleteSelection() {
    Component& component = *app.current;
    int linkCount = ImNodes::NumSelectedLinks();
    if (linkCount > 0) {
        std::vector<int> links(static_cast<size_t>(linkCount));
        ImNodes::GetSelectedLinks(links.data());
        for (int link : links) {
            RemoveWire(component, static_cast<uint32_t>(link));
        }
    }
    int nodeCount = ImNodes::NumSelectedNodes();
    if (nodeCount > 0) {
        std::vector<int> nodes(static_cast<size_t>(nodeCount));
        ImNodes::GetSelectedNodes(nodes.data());
        for (int node : nodes) {
            RemoveNode(component, static_cast<uint32_t>(node));
        }
    }
    ImNodes::ClearLinkSelection();
    ImNodes::ClearNodeSelection();
}

constexpr float MinZoom = 0.25f, MaxZoom = 3.0f;

// imnodes has no zoom of its own: its sizes are scaled here, and node positions in DrawNode.
static void ScaleNodeStyle(float zoom) {
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

void ResetZoom(Component& component) {
    if (!component.editor) {
        return;
    }
    ImNodes::EditorContextSet(component.editor);
    ImVec2 panning = ImNodes::EditorContextGetPanning();
    ImNodes::EditorContextResetPanning(ImVec2(panning.x / component.zoom, panning.y / component.zoom));
    component.zoom = 1.0f;
}

void DrawCanvas(ImVec2 position, ImVec2 size) {
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

    if (editing) {
        PruneWires(component);
    }

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
    for (Node& node : component.nodes) {
        DrawNode(node);
    }

    for (const Wire& wire : component.wires) {
        const Node* from = component.FindNode(wire.fromNode);
        const Node* to = component.FindNode(wire.toNode);
        if (!from || !to) {
            continue;
        }
        int width = static_cast<int>(wire.width);
        if (!editing) {
            ImU32 color = LevelColor(BusLevel(simulation, PinNets(simulation, wire.fromNode, true,
                                                                  static_cast<int>(wire.fromPin), width)));
            ImNodes::PushColorStyle(ImNodesCol_Link, color);
            ImNodes::PushColorStyle(ImNodesCol_LinkHovered, Lighten(color, 30));
            ImNodes::PushColorStyle(ImNodesCol_LinkSelected, Lighten(color, 30));
        }
        if (width > 1) {
            ImNodes::PushStyleVar(ImNodesStyleVar_LinkThickness, ImNodes::GetStyle().LinkThickness * 2.5f);
        }
        ImNodes::Link(static_cast<int>(wire.id), WireEndAttribute(component, *from, true, static_cast<int>(wire.fromPin), width),
                      WireEndAttribute(component, *to, false, static_cast<int>(wire.toPin), width));
        if (width > 1) {
            ImNodes::PopStyleVar();
        }
        if (!editing) {
            ImNodes::PopColorStyle();
            ImNodes::PopColorStyle();
            ImNodes::PopColorStyle();
        }
    }

    if (app.showMiniMap) {
        ImNodes::MiniMap(0.15f, ImNodesMiniMapLocation_BottomRight);
    }
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
            if (wire.id != static_cast<uint32_t>(hoveredLink) || !from || !to) {
                continue;
            }
            int width = static_cast<int>(wire.width);
            std::string text = EndpointText(*from, true, static_cast<int>(wire.fromPin), width) + " -> " +
                               EndpointText(*to, false, static_cast<int>(wire.toPin), width);
            if (width > 1) {
                text += "  (" + std::to_string(width) + " bits)";
            }
            if (!editing) {
                text += " = " + BusText(simulation, PinNets(simulation, wire.fromNode, true, static_cast<int>(wire.fromPin), width));
            }
            ImGui::SetTooltip("%s", text.c_str());
        }
    }
    if (app.hoveredNode >= 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        OpenRomEditor(static_cast<uint32_t>(app.hoveredNode));  // does nothing unless it's a ROM
    }

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
                if ((fromWidth == toWidth || fromWidth == 1 || toWidth == 1) && fits) {
                    Connect(component, fromNode, fromIndex, toNode, toIndex, width);
                }
                else {
                    SetStatus("Can't wire %d bits to %d: show the bus's bits (+) to wire some of them", fromWidth, toWidth);
                }
            }
        }
        int destroyedLink;
        if (ImNodes::IsLinkDestroyed(&destroyedLink)) {
            RemoveWire(component, static_cast<uint32_t>(destroyedLink));
        }

        bool canvasFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (canvasFocused && !ImGui::GetIO().WantTextInput &&
            (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace))) {
            DeleteSelection();
        }

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

}  // namespace Kestrel
