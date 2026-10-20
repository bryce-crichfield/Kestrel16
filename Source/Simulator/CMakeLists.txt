# KestrelSimulatorCore: components, their files and the gate-level simulation, without the editor
# (KestrelValidator uses it too). It loads components from COMPONENTS_DIR (set at the root), where
# the hardware generators write them and the editor saves new ones.
add_library(KestrelSimulatorCore STATIC
    Circuit.cpp
    ComponentFile.cpp
    Simulation.cpp
)
target_include_directories(KestrelSimulatorCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(KestrelSimulatorCore PUBLIC KestrelCore imgui yaml-cpp::yaml-cpp)
target_compile_definitions(KestrelSimulatorCore PRIVATE COMPONENTS_PATH="${COMPONENTS_DIR}/")

# Circuit editor and gate-level simulator -> Binary/KestrelSimulator
add_executable(KestrelSimulator
    Editor.cpp
    EditorCanvas.cpp
    EditorMemory.cpp
    EditorPanels.cpp
    HeadlessRun.cpp
    Main.cpp
    SelfTest.cpp
)
target_link_libraries(KestrelSimulator PRIVATE KestrelSimulatorCore KestrelToolchain raylib rlimgui imnodes)
