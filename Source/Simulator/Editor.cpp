// Editor.cpp -- the editor's state and actions, its menu bar and shortcuts, and the window.

#include "Editor.h"

#include "ComponentFile.h"
#include "EditorInternal.h"

#include "raylib.h"
#include "rlImGui.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>

namespace Kestrel {

App app;

void SetStatus(const char* format, ...)
{
    char text[256];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(text, sizeof text, format, arguments);
    va_end(arguments);
    app.status = text;
    app.statusUntil = GetTime() + 4.0;
}

void EnsureEditor(Component& component)
{
    if (!component.editor) {
        component.editor = ImNodes::EditorContextCreate();
        for (Node& node : component.nodes) node.placement = Node::Placement::Grid;
    }
}

void OpenComponent(Component* component)
{
    if (!component || component == app.current) return;
    app.current = component;
    EnsureEditor(*component);
    app.hoveredNode = -1;
}

void NewComponent()
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

void EnterSimulation()
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

int RunEditor(const char* screenshotPath, bool simulateOnStart)
{
    std::vector<std::string> problems = LoadLibrary();
    for (const std::string& problem : problems) fprintf(stderr, "%s\n", problem.c_str());
    if (library.empty()) CreateComponent("Untitled");

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    InitWindow(1440, 880, "Kestrel16 Circuit Workbench");
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

}  // namespace Kestrel
