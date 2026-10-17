# KestrelCore: the machine. Instructions.h, MemoryMap.h and AluOperations.h define it as data (the
# Python generators in Tools/ read them too); Isa, Alu, Machine (the reference implementation) and
# Disassembler build on them. Source/ is the include root: #include "Kestrel/Isa.h".
add_library(KestrelCore STATIC
    Alu.cpp
    Disassembler.cpp
    Files.cpp
    Isa.cpp
    Machine.cpp
)
target_include_directories(KestrelCore PUBLIC ${PROJECT_SOURCE_DIR}/Source)
target_link_libraries(KestrelCore PUBLIC Kestrel16Build)
