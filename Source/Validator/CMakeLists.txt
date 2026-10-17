# Differential testing -> Binary/KestrelValidator: builds a program, runs it on the emulator (the
# reference) and on the gate-level computer, and compares the state they end in.
add_executable(KestrelValidator
    Dump.cpp
    EmulatorTarget.cpp
    Main.cpp
    SimulatorTarget.cpp
    State.cpp
)
target_link_libraries(KestrelValidator PRIVATE KestrelToolchain KestrelSimulatorCore)
