# Circuit editor and gate-level simulator -> Binary/KestrelSimulator. It loads components from
# COMPONENTS_DIR (set at the root), where the hardware generators write them and the editor saves
# new ones.
add_executable(KestrelSimulator
    Circuit.cpp
    ComponentFile.cpp
    Editor.cpp
    EditorCanvas.cpp
    EditorMemory.cpp
    EditorPanels.cpp
    HardwareCheck.cpp
    HeadlessRun.cpp
    Main.cpp
    SelfTest.cpp
    Simulation.cpp
)
target_link_libraries(KestrelSimulator PRIVATE KestrelToolchain raylib rlimgui imnodes yaml-cpp::yaml-cpp)
target_compile_definitions(KestrelSimulator PRIVATE COMPONENTS_PATH="${COMPONENTS_DIR}/")
