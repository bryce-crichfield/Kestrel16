# KestrelEmulatorCore: the reference machine (Machine, and the ALU and flags it computes with), the
# definition of correct behaviour. KestrelValidator holds the gate-level computer to it.
add_library(KestrelEmulatorCore STATIC
    Alu.cpp
    Machine.cpp
)
target_link_libraries(KestrelEmulatorCore PUBLIC KestrelCore)

# The reference machine running a program -> Binary/KestrelEmulator
add_executable(KestrelEmulator Main.cpp)
target_link_libraries(KestrelEmulator PRIVATE KestrelEmulatorCore KestrelToolchain)
