# KestrelCore: the shared definitions. Instructions.h, MemoryMap.h and AluOperations.h define the
# machine as data (the Python generators in Tools/ read them too); Isa and Disassembler build on them,
# and Files holds file helpers every tool uses. The reference implementation is in Source/Emulator. Source/ is the include root: #include "Kestrel/Isa.h".
add_library(KestrelCore STATIC
    Disassembler.cpp
    Files.cpp
    Isa.cpp
)
target_include_directories(KestrelCore PUBLIC ${PROJECT_SOURCE_DIR}/Source)
target_link_libraries(KestrelCore PUBLIC Kestrel16Build)
