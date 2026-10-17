// EditorPanels.cpp -- the side panels: palette, properties and simulation.

#include "ComponentFile.h"
#include "EditorInternal.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace Kestrel {

static void BeginPanel(const char* name, ImVec2 position, ImVec2 size) {
    ImGui::SetNextWindowPos(position);
    ImGui::SetNextWindowSize(size);
    ImGui::Begin(name, nullptr,
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
}

// A draggable divider at x. Dragging it changes panelWidth; direction is +1 for a panel on
// the left (drag right to grow) and -1 for one on the right.
void DrawSplitter(const char* id, float x, float top, float height, float& panelWidth, float direction) {
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
    if (ImGui::IsItemActive()) {
        panelWidth += ImGui::GetIO().MouseDelta.x * direction;
    }
    ImGui::End();
}

void DrawPalette(ImVec2 position, ImVec2 size) {
    BeginPanel("Palette", position, size);

    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Click to add, or right-click the canvas.");
    ImGui::PopStyleColor();
    DrawAddNodeMenu(ImVec2(0, 0), true);

    ImGui::Spacing();
    ImGui::SeparatorText("Library");
    if (ImGui::Button("New component", ImVec2(-1, 0))) {
        NewComponent();
    }

    std::vector<Component*> sorted;
    for (auto& component : library) {
        sorted.push_back(component.get());
    }
    std::sort(sorted.begin(), sorted.end(), [](Component* a, Component* b) { return a->name < b->name; });

    for (Component* component : sorted) {
        ImGui::PushID(component);
        std::string label = component->name + (component->dirty ? " *" : "");
        if (ImGui::Selectable(label.c_str(), component == app.current)) {
            OpenComponent(component);
        }
        ImGui::SetItemTooltip("Open '%s' for editing", component->name.c_str());
        ImGui::PopID();
    }

    ImGui::End();
}

static void DrawSelectedNodeProperties(Node& node) {
    Component& component = *app.current;
    ImGui::SeparatorText("Selected");
    ImGui::Text("%s", NodeTitle(node).c_str());
    if (IsPrimitive(node.kind)) {
        ImGui::TextWrapped("%s", Primitives[static_cast<int>(node.kind)].description);
    }

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
            if (pin == &node) {
                break;
            }
            pinNumber += pin->width;
        }
        const char* side = node.kind == NodeKind::Input ? "Input" : "Output";
        if (node.width == 1) {
            ImGui::TextDisabled("%s pin %d of this component", side, pinNumber + 1);
        }
        else {
            ImGui::TextDisabled("%s pins %d-%d of this component", side, pinNumber + 1, pinNumber + node.width);
        }
    }

    if (IsMemory(node.kind)) {
        MemoryData& rom = *node.memory;
        int addressBits = rom.addressBits, dataBits = rom.dataBits;
        bool resized = ImGui::SliderInt("Address bits", &addressBits, 1, MaxMemoryAddressBits);
        resized |= ImGui::SliderInt("Data bits", &dataBits, 1, MaxMemoryDataBits);
        if (resized) {
            int oldAddressBits = rom.addressBits;
            rom.Resize(addressBits, dataBits);
            if (!rom.image.empty()) {
                LoadRomImage(rom);
            }
            RemapMemoryWires(component, node, oldAddressBits);
        }
        if (node.kind == NodeKind::Rom) {
            ImGui::TextDisabled("%zu words; AT28C64 is 13 x 8, AT28C256 is 15 x 8", rom.words.size());
        }
        else {
            ImGui::TextDisabled("%zu words; 62256 is 15 x 8, 6264 is 13 x 8", rom.words.size());
        }
        if (!rom.image.empty()) {
            ImGui::TextDisabled("%s %s", node.kind == NodeKind::Rom ? "Contents from" : "Preloaded from",
                                rom.image.c_str());
        }
        if (ImGui::Button(node.kind == NodeKind::Rom ? "Edit contents..." : "Contents...")) {
            OpenRomEditor(node.id);
        }
        ImGui::SetItemTooltip("Or double-click the ROM");
    }

    if (node.kind == NodeKind::Component) {
        Component* type = FindComponent(node.typeName);
        if (type) {
            if (ImGui::Button("Open component")) {
                OpenComponent(type);
            }
        }
        else {
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "No component named '%s'", node.typeName.c_str());
        }
    }

    if (ImGui::Button("Delete")) {
        RemoveNode(component, node.id);
        ImNodes::EditorContextSet(component.editor);
        ImNodes::ClearNodeSelection();
    }
}

void DrawProperties(ImVec2 position, ImVec2 size) {
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
        }
        else if (!IsValidComponentName(newName)) {
            SetStatus("Names use letters, digits, spaces, '_' and '-' (up to %d characters)", MaxNameLength - 1);
        }
        else if (FindComponent(newName)) {
            SetStatus("There is already a component named '%s'", newName.c_str());
        }
        else {
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
        if (node.kind == NodeKind::Component) {
            parts++;
        }
        else if (node.kind != NodeKind::Input && node.kind != NodeKind::Output) {
            gates++;
        }
    }
    ImGui::Text("Pins: %zu in, %zu out", PinBits(component, NodeKind::Input).size(),
                PinBits(component, NodeKind::Output).size());
    ImGui::Text("Parts: %d primitives, %d components", gates, parts);
    ImGui::Text("Wires: %zu", component.wires.size());

    ImNodes::EditorContextSet(component.editor);
    if (ImNodes::NumSelectedNodes() == 1) {
        int selected;
        ImNodes::GetSelectedNodes(&selected);
        if (Node* node = component.FindNode(static_cast<uint32_t>(selected))) {
            DrawSelectedNodeProperties(*node);
        }
    }
    else if (ImNodes::NumSelectedNodes() > 1) {
        ImGui::SeparatorText("Selected");
        ImGui::Text("%d nodes", ImNodes::NumSelectedNodes());
        if (ImGui::Button("Delete")) {
            DeleteSelection();
        }
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

void DrawSimulationPanel(ImVec2 position, ImVec2 size) {
    Component& component = *app.current;
    Simulation& simulation = app.simulation;
    BeginPanel("Simulation", position, size);

    ImGui::Text("Simulating '%s'", component.name.c_str());
    if (ImGui::Button("Back to editor (F5)", ImVec2(-1, 0))) {
        app.mode = Mode::Edit;
    }

    ImGui::SeparatorText("Clock");
    if (ImGui::Button(simulation.running ? "Pause (Space)" : "Run (Space)", ImVec2(-1, 0))) {
        simulation.running = !simulation.running;
    }
    if (ImGui::Button("Step one cycle")) {
        HalfStep(simulation);
        HalfStep(simulation);
    }
    ImGui::SameLine();
    if (ImGui::Button("Half step")) {
        HalfStep(simulation);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
        BuildSimulation(simulation, component);
    }
    ImGui::SetItemTooltip("Power-cycle: flip-flops and RAM start over");
    ImGui::SliderFloat("Hz", &simulation.frequency, 0.1f, 10000.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
    ImGui::Checkbox("RAM powers up random", &simulation.randomRam);
    ImGui::SetItemTooltip(
        "As real RAM does: catches reading memory before writing it.\n"
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
    if (simulation.unstable) {
        ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1), "Unstable: still changing after %d gate delays\n(something oscillates)", MaxSettleSteps);
    }
    if (simulation.conflicts) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.35f, 0.35f, 1));
        ImGui::TextWrapped("%d bus conflict(s): a wire driven to 0 and 1 at once (shown red)", simulation.conflicts);
        ImGui::PopStyleColor();
    }
    if (simulation.missingComponents) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%d missing component(s) left out", simulation.missingComponents);
    }
    if (simulation.nestingTooDeep) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Nesting deeper than %d levels was cut off", MaxNestingDepth);
    }

    if (!simulation.terminals.empty()) {
        ImGui::SeparatorText("Terminal");
        ImGui::BeginChild("console", ImVec2(-1, ImGui::GetTextLineHeightWithSpacing() * 10), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(simulation.console.c_str(), simulation.console.c_str() + simulation.console.size());
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
            ImGui::SetScrollHereY(1.0f);  // follow new output
        }
        ImGui::EndChild();
        ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Clear").x - ImGui::GetStyle().FramePadding.x * 2 -
                                ImGui::GetStyle().ItemSpacing.x);
        if (ImGui::InputTextWithHint("##type", "type here; Enter sends", app.typeBuffer, sizeof app.typeBuffer,
                                     ImGuiInputTextFlags_EnterReturnsTrue)) {
            for (const char* character = app.typeBuffer; *character; character++) {
                simulation.keys.push_back(static_cast<uint8_t>(*character));
            }
            simulation.keys.push_back('\n');
            app.typeBuffer[0] = 0;
            ImGui::SetKeyboardFocusHere(-1);
            RecordSettle(simulation);  // IRQ and the status register change
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear")) {
            simulation.console.clear();
        }
        ImGui::TextDisabled("%zu key(s) waiting to be read", simulation.keys.size());
    }

    ImGui::SeparatorText("Inputs");
    bool anyInput = false;
    for (const Node* node : PinNodes(component, NodeKind::Input)) {
        anyInput = true;
        ImGui::PushID(static_cast<int>(node->id));
        if (node->width == 1) {
            bool on = SwitchValue(simulation, node->id) != 0;
            if (ImGui::Checkbox(node->label.c_str(), &on)) {
                SetSwitch(simulation, node->id, on);
            }
        }
        else {
            uint32_t value = SwitchValue(simulation, node->id);
            ImGui::SetNextItemWidth(ImGui::CalcTextSize("00000000").x + 16);
            if (ImGui::InputScalar(node->label.c_str(), ImGuiDataType_U32, &value, nullptr, nullptr, "%X",
                                   ImGuiInputTextFlags_CharsHexadecimal)) {
                SetSwitch(simulation, node->id, value);
            }
        }
        ImGui::PopID();
    }
    if (!anyInput) {
        ImGui::TextDisabled("(no Input nodes)");
    }

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
    if (!anyOutput) {
        ImGui::TextDisabled("(no Output nodes)");
    }

    ImGui::SeparatorText("Inspector");
    int inspected = app.hoveredNode;
    ImNodes::EditorContextSet(component.editor);
    if (inspected < 0 && ImNodes::NumSelectedNodes() == 1) {
        ImNodes::GetSelectedNodes(&inspected);
    }
    if (const Node* node = inspected >= 0 ? component.FindNode(static_cast<uint32_t>(inspected)) : nullptr) {
        ImGui::Text("%s", NodeTitle(*node).c_str());
        if (IsMemory(node->kind) && ImGui::SmallButton("Show contents")) {
            OpenRomEditor(node->id);
        }
        for (int index = 0; index < InputCount(*node); index++) {
            ImGui::Text("  in  %-8s = %s", PinName(*node, false, index).c_str(),
                        LevelText(simulation.Level(simulation.NetOfInput(node->id, index))));
        }
        for (int index = 0; index < OutputCount(*node); index++) {
            ImGui::Text("  out %-8s = %s", PinName(*node, true, index).c_str(),
                        LevelText(simulation.Level(simulation.NetOfOutput(node->id, index))));
        }
    }
    else {
        ImGui::TextDisabled("Hover or select a node");
    }

    ImGui::End();
}

}  // namespace Kestrel
