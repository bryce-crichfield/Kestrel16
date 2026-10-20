// EditorInternal.h -- what the editor's parts share. Not for use outside the editor; Editor.h is
// its interface.
//
//   Editor.cpp        the app state, actions, menu bar, shortcuts, the frame and the window
//   EditorCanvas.cpp  the node canvas: nodes, pins, wires, zoom, and the add-node menu
//   EditorPanels.cpp  the palette, properties and simulation panels
//   EditorMemory.cpp  the ROM and RAM contents window

#pragma once

#include "Circuit.h"
#include "Simulation.h"

#include "imgui.h"
#include "imnodes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Kestrel {

enum class Mode { Edit,
                  Simulate };

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

extern App app;

// Editor.cpp
void SetStatus(const char* format, ...);
void EnsureEditor(Component& component);
void OpenComponent(Component* component);
void NewComponent();
void EnterSimulation();

// EditorCanvas.cpp
ImU32 LevelColor(uint8_t level);
const char* LevelText(uint8_t level);
void DrawLed(bool on);
// Several bits as hex: "Z" when nothing drives any of them, "?" for a digit with a bit that's
// floating or in conflict.
std::string BusText(const Simulation& simulation, const std::vector<int>& nets);
std::vector<int> PinNets(const Simulation& simulation, uint32_t node, bool output, int first, int count);
void DrawAddNodeMenu(ImVec2 screenPosition, bool atCenter);
void DeleteSelection();
void ResetZoom(Component& component);
void DrawCanvas(ImVec2 position, ImVec2 size);

// EditorPanels.cpp
void DrawSplitter(const char* id, float x, float top, float height, float& panelWidth, float direction);
void DrawPalette(ImVec2 position, ImVec2 size);
void DrawProperties(ImVec2 position, ImVec2 size);
void DrawSimulationPanel(ImVec2 position, ImVec2 size);

// EditorMemory.cpp
void OpenRomEditor(uint32_t nodeId);
void DrawRomEditor();

}  // namespace Kestrel
