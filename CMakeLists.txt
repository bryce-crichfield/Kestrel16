cmake_minimum_required(VERSION 3.20)
project(Kestrel16 LANGUAGES C CXX)

# Layout (Source/ is the include root: #include "Kestrel/Isa.h"):
#   Source/Kestrel/      KestrelCore: the machine. Instructions.h, MemoryMap.h and AluOperations.h
#                        define it as data (the Python generators in Tools/ read them too); Isa,
#                        Alu, Machine (the reference implementation) and Disassembler build on them
#   Source/Assembler/    KestrelAssemblerCore (Assemble())  + Main.cpp -> Binary/KestrelAssembler
#   Source/Compiler/     KestrelCompilerCore (Compile())    + Main.cpp -> Binary/KestrelCompiler
#   Source/Toolchain/    KestrelToolchain: a .c, .s or .bin to a memory image, in-process
#   Source/Emulator/     Main.cpp                                      -> Binary/KestrelEmulator
#   Source/Simulator/    the circuit editor and gate-level simulator   -> Binary/KestrelSimulator
#   Programs/*.s         -> Binary/Programs/*.bin (assembled with our own assembler)
#   Programs/*.c         -> Binary/Programs/*_c.bin (compiled, then assembled)
#   Components/*.yaml    circuit components saved by the simulator (and made by Tools/make_*.py)
#
# Configure and build:   cmake -S . -B Build && cmake --build Build
# Run a program:         cmake --build Build --target run_Fibonacci_c       (emulator)
#                        cmake --build Build --target simulate_Fibonacci_c  (gate-level Kestrel16 Computer)
# Any source, by hand:   Binary/KestrelEmulator --run Programs/Fibonacci.c
#                        Binary/KestrelSimulator --run "Kestrel16 Computer" --program Programs/Fibonacci.c
# Tests:                 ctest --test-dir Build

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Choose the type of build." FORCE)
endif()

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Executables land in Binary/ for every build type (no Debug/Release subfolders).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/Binary)
foreach(config ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE})
    string(TOUPPER ${config} config)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${config} ${CMAKE_SOURCE_DIR}/Binary)
endforeach()

option(Kestrel16_WARNINGS_AS_ERRORS "Treat compiler warnings as errors (our code only)" OFF)
option(Kestrel16_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer (GCC/Clang)" OFF)

# ---------------------------------------------------------------------------
# Vendor libraries, fetched into Build/_deps on configure and pinned to exact commits.
include(FetchContent)

set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG c1ab645ca298a2801097931d1079b10ff7eb9df8  # 5.5
    GIT_SHALLOW FALSE
)
# ImGui, rlImGui and imnodes have no CMake build we want; SOURCE_SUBDIR points at nothing so
# they are only downloaded, and we compile them ourselves below.
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG 5d4126876bc10396d4c6511853ff10964414c776  # v1.92.1
    SOURCE_SUBDIR none
)
FetchContent_Declare(rlimgui
    GIT_REPOSITORY https://github.com/raylib-extras/rlImGui.git
    GIT_TAG 118221c8a532501a3c2c236b62950877cbefc2b4  # Raylib_5_5
    SOURCE_SUBDIR none
)
FetchContent_Declare(imnodes
    GIT_REPOSITORY https://github.com/Nelarius/imnodes.git
    GIT_TAG eb36902c892548ef94f88f51ad7e7c9c7058a71c  # master, works with ImGui 1.92
    SOURCE_SUBDIR none
)
# yaml-cpp reads and writes the component files.
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)
set(YAML_CPP_FORMAT_SOURCE OFF CACHE BOOL "" FORCE)
set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(yaml-cpp
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
    GIT_TAG 56e3bb550c91fd7005566f19c079cb7a503223cf  # yaml-cpp-0.9.0
)
FetchContent_MakeAvailable(raylib imgui rlimgui imnodes yaml-cpp)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR})
# Big components (the ALU has ~1000 nodes) overflow 16-bit draw-list indices.
target_compile_definitions(imgui PUBLIC "ImDrawIdx=unsigned int")

add_library(rlimgui STATIC ${rlimgui_SOURCE_DIR}/rlImGui.cpp)
target_include_directories(rlimgui PUBLIC ${rlimgui_SOURCE_DIR})
target_link_libraries(rlimgui PUBLIC raylib imgui)

add_library(imnodes STATIC ${imnodes_SOURCE_DIR}/imnodes.cpp)
target_include_directories(imnodes PUBLIC ${imnodes_SOURCE_DIR})
target_link_libraries(imnodes PUBLIC imgui)

# raylib's GLFW pulls in OpenGL/X11 on Linux and can't link fully static there.
if(UNIX AND NOT APPLE AND CMAKE_EXE_LINKER_FLAGS MATCHES "(^| )-static( |$)")
    string(REGEX REPLACE "(^| )-static( |$)" " " _fixed_flags "${CMAKE_EXE_LINKER_FLAGS}")
    string(STRIP "${_fixed_flags}" _fixed_flags)
    set(CMAKE_EXE_LINKER_FLAGS "${_fixed_flags}" CACHE STRING "Linker flags for executables" FORCE)
    message(WARNING "Removed -static from CMAKE_EXE_LINKER_FLAGS (unsupported with OpenGL on Linux)")
endif()

# ---------------------------------------------------------------------------
# Settings every Kestrel16 target shares (our code only; vendor code keeps its own flags).
add_library(Kestrel16Build INTERFACE)
if(MSVC)
    target_compile_options(Kestrel16Build INTERFACE /W4)
    target_compile_definitions(Kestrel16Build INTERFACE _CRT_SECURE_NO_WARNINGS)
    if(Kestrel16_WARNINGS_AS_ERRORS)
        target_compile_options(Kestrel16Build INTERFACE /WX)
    endif()
else()
    target_compile_options(Kestrel16Build INTERFACE -Wall -Wextra)
    if(Kestrel16_WARNINGS_AS_ERRORS)
        target_compile_options(Kestrel16Build INTERFACE -Werror)
    endif()
    if(Kestrel16_SANITIZE)
        target_compile_options(Kestrel16Build INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(Kestrel16Build INTERFACE -fsanitize=address,undefined)
    endif()
endif()
if(MINGW)
    # std::filesystem needs this on older MinGW GCCs; harmless on newer ones.
    target_link_libraries(Kestrel16Build INTERFACE $<$<VERSION_LESS:$<CXX_COMPILER_VERSION>,9.1>:stdc++fs>)
endif()

# ---------------------------------------------------------------------------
# Libraries: each tool is a library with a thin main, so the others can use it in-process.
add_library(KestrelCore STATIC
    Source/Kestrel/Alu.cpp
    Source/Kestrel/Disassembler.cpp
    Source/Kestrel/Files.cpp
    Source/Kestrel/Isa.cpp
    Source/Kestrel/Machine.cpp
)
target_include_directories(KestrelCore PUBLIC ${CMAKE_SOURCE_DIR}/Source)
target_link_libraries(KestrelCore PUBLIC Kestrel16Build)

add_library(KestrelAssemblerCore STATIC Source/Assembler/Assembler.cpp)
target_link_libraries(KestrelAssemblerCore PUBLIC KestrelCore)

add_library(KestrelCompilerCore STATIC
    Source/Compiler/CodeGenerator.cpp
    Source/Compiler/Compiler.cpp
    Source/Compiler/Expressions.cpp
    Source/Compiler/Lexer.cpp
    Source/Compiler/Parser.cpp
    Source/Compiler/Preprocessor.cpp
    Source/Compiler/Program.cpp
    Source/Compiler/Types.cpp
)
target_link_libraries(KestrelCompilerCore PUBLIC KestrelCore)

add_library(KestrelToolchain STATIC Source/Toolchain/Toolchain.cpp)
target_link_libraries(KestrelToolchain PUBLIC KestrelAssemblerCore KestrelCompilerCore)

# ---------------------------------------------------------------------------
# Tools
add_executable(KestrelAssembler Source/Assembler/Main.cpp)
target_link_libraries(KestrelAssembler PRIVATE KestrelAssemblerCore)

add_executable(KestrelCompiler Source/Compiler/Main.cpp)
target_link_libraries(KestrelCompiler PRIVATE KestrelCompilerCore)

add_executable(KestrelEmulator Source/Emulator/Main.cpp)
target_link_libraries(KestrelEmulator PRIVATE KestrelToolchain)

# Circuit editor and simulator. Components are saved next to the sources, not in Binary/,
# so they live in version control with everything else.
add_executable(KestrelSimulator
    Source/Simulator/Circuit.cpp
    Source/Simulator/ComponentFile.cpp
    Source/Simulator/Editor.cpp
    Source/Simulator/EditorCanvas.cpp
    Source/Simulator/EditorMemory.cpp
    Source/Simulator/EditorPanels.cpp
    Source/Simulator/HardwareCheck.cpp
    Source/Simulator/HeadlessRun.cpp
    Source/Simulator/Main.cpp
    Source/Simulator/SelfTest.cpp
    Source/Simulator/Simulation.cpp
)
target_link_libraries(KestrelSimulator PRIVATE KestrelToolchain raylib rlimgui imnodes yaml-cpp::yaml-cpp)
target_compile_definitions(KestrelSimulator PRIVATE
    COMPONENTS_PATH="${CMAKE_SOURCE_DIR}/Components/")

# ---------------------------------------------------------------------------
# Programs: every Programs/*.s is assembled into Binary/Programs/<name>.bin, and gets a
# run_<name> target. They rebuild when the source or the assembler changes.
set(PROGRAM_OUTPUT_DIR ${CMAKE_SOURCE_DIR}/Binary/Programs)
file(GLOB PROGRAM_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/Programs/*.s)

set(PROGRAM_BINARIES)
foreach(source ${PROGRAM_SOURCES})
    get_filename_component(name ${source} NAME_WE)
    set(binary ${PROGRAM_OUTPUT_DIR}/${name}.bin)

    add_custom_command(
        OUTPUT ${binary}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PROGRAM_OUTPUT_DIR}
        COMMAND $<TARGET_FILE:KestrelAssembler> -o ${binary} ${source}
        DEPENDS ${source} KestrelAssembler
        COMMENT "Assembling ${name}.s"
        VERBATIM
    )
    list(APPEND PROGRAM_BINARIES ${binary})

    add_custom_target(run_${name}
        COMMAND $<TARGET_FILE:KestrelEmulator> ${binary}
        DEPENDS ${binary} KestrelEmulator
        USES_TERMINAL
        COMMENT "Running ${name}"
    )
    add_custom_target(simulate_${name}
        COMMAND $<TARGET_FILE:KestrelSimulator> --components ${CMAKE_SOURCE_DIR}/Components
                --run "Kestrel16 Computer" --program ${binary} --stats
        DEPENDS ${binary} KestrelSimulator
        USES_TERMINAL
        COMMENT "Simulating ${name} on the Kestrel16 Computer"
    )
endforeach()

# Every Programs/*.c is compiled to Build/Programs/<name>.s, then assembled into
# Binary/Programs/<name>_c.bin (the _c keeps it apart from a <name>.s of the same name).
file(GLOB C_PROGRAM_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/Programs/*.c)
# Any header change rebuilds every C program (the compiler does not report its includes).
file(GLOB C_PROGRAM_HEADERS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/Programs/*.h)
set(C_PROGRAM_ASM_DIR ${CMAKE_BINARY_DIR}/Programs)

foreach(source ${C_PROGRAM_SOURCES})
    get_filename_component(name ${source} NAME_WE)
    set(assembly ${C_PROGRAM_ASM_DIR}/${name}.s)
    set(binary ${PROGRAM_OUTPUT_DIR}/${name}_c.bin)

    add_custom_command(
        OUTPUT ${assembly}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${C_PROGRAM_ASM_DIR}
        COMMAND $<TARGET_FILE:KestrelCompiler> -o ${assembly} ${source}
        DEPENDS ${source} ${C_PROGRAM_HEADERS} KestrelCompiler
        COMMENT "Compiling ${name}.c"
        VERBATIM
    )
    add_custom_command(
        OUTPUT ${binary}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PROGRAM_OUTPUT_DIR}
        COMMAND $<TARGET_FILE:KestrelAssembler> -o ${binary} ${assembly}
        DEPENDS ${assembly} KestrelAssembler
        COMMENT "Assembling ${name}.s (from ${name}.c)"
        VERBATIM
    )
    list(APPEND PROGRAM_BINARIES ${binary})

    add_custom_target(run_${name}_c
        COMMAND $<TARGET_FILE:KestrelEmulator> ${binary}
        DEPENDS ${binary} KestrelEmulator
        USES_TERMINAL
        COMMENT "Running ${name}_c"
    )
    add_custom_target(simulate_${name}_c
        COMMAND $<TARGET_FILE:KestrelSimulator> --components ${CMAKE_SOURCE_DIR}/Components
                --run "Kestrel16 Computer" --program ${binary} --stats
        DEPENDS ${binary} KestrelSimulator
        USES_TERMINAL
        COMMENT "Simulating ${name}_c on the Kestrel16 Computer"
    )
endforeach()

add_custom_target(programs ALL DEPENDS ${PROGRAM_BINARIES})

# Tests: run each program through the emulator. A test passes when it halts cleanly
# (an illegal instruction or the step limit fails it).
enable_testing()
foreach(binary ${PROGRAM_BINARIES})
    get_filename_component(name ${binary} NAME_WE)
    if(name STREQUAL "Shell_c")
        continue()  # reads commands; tested below with some typed in
    endif()
    # Through RunWithInput so stdin is an empty file: reading the UART status must not wait on
    # whatever stdin ctest hands down.
    add_test(NAME program_${name} COMMAND ${CMAKE_COMMAND} "-DCOMMAND=$<TARGET_FILE:KestrelEmulator>;${binary}"
             -DINPUT= -DOUTPUT_DIRECTORY=${CMAKE_BINARY_DIR}/${name} -P ${CMAKE_SOURCE_DIR}/Tests/RunWithInput.cmake)
    set_tests_properties(program_${name} PROPERTIES FAIL_REGULAR_EXPRESSION "illegal instruction;step limit")
endforeach()

# The circuit simulator and the component file format check themselves.
add_test(NAME simulator_self_test COMMAND KestrelSimulator --self-test)
# The machine's components (ALU, ...) against the emulator.
add_test(NAME hardware_check COMMAND KestrelSimulator --components ${CMAKE_SOURCE_DIR}/Components --check-hardware)

# What each program prints. The ones also listed in GATE_LEVEL_TESTS run on the gate-level
# computer too, which has to print the same.
set(EXPECTED_Fibonacci_c "28657 46368")
set(EXPECTED_Shifts_c "3EFE 1CA6")     # every shift amount, byte and word stores
set(EXPECTED_Interrupts "1FE4 0708")   # timer interrupts landing on every kind of instruction
# The shell reads commands: type a couple in, and --run stops once they run out.
if(EXISTS ${CMAKE_SOURCE_DIR}/Programs/Shell.c)
    add_test(NAME program_Shell_c COMMAND ${CMAKE_COMMAND} "-DCOMMAND=$<TARGET_FILE:KestrelEmulator>;--run;${PROGRAM_OUTPUT_DIR}/Shell_c.bin"
             "-DINPUT=calc 7 * 6|rev Kestrel|" -DOUTPUT_DIRECTORY=${CMAKE_BINARY_DIR}/Shell_c
             -P ${CMAKE_SOURCE_DIR}/Tests/RunWithInput.cmake)
    set_tests_properties(program_Shell_c PROPERTIES PASS_REGULAR_EXPRESSION "42.*lertseK")
endif()
set(GATE_LEVEL_TESTS Fibonacci_c Shifts_c Interrupts)
foreach(name Fibonacci_c Shifts_c Interrupts)
    if(TEST program_${name})
        set_tests_properties(program_${name} PROPERTIES PASS_REGULAR_EXPRESSION "${EXPECTED_${name}}")
    endif()
endforeach()
foreach(name ${GATE_LEVEL_TESTS})
    if(TEST program_${name})
        add_test(NAME simulate_${name} COMMAND KestrelSimulator --components ${CMAKE_SOURCE_DIR}/Components
                 --run "Kestrel16 Computer" --program ${PROGRAM_OUTPUT_DIR}/${name}.bin)
        set_tests_properties(simulate_${name} PROPERTIES PASS_REGULAR_EXPRESSION "${EXPECTED_${name}}" TIMEOUT 600)
    endif()
endforeach()