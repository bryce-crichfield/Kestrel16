// EditorMemory.cpp -- the ROM and RAM contents window.

#include "EditorInternal.h"
#include "ComponentFile.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;

namespace Kestrel {

void OpenRomEditor(uint32_t nodeId)
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

void DrawRomEditor()
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

}  // namespace Kestrel
