cmake_minimum_required(VERSION 3.20)
project(Kestrel16 LANGUAGES C CXX)

# Layout (Source/ is the include root: #include "Kestrel/Isa.h"):
#   Source/Kestrel/      KestrelCore: the shared definitions, used by everything below
#   Source/Assembler/    KestrelAssemblerCore (Assemble())  + Main.cpp -> Binary/KestrelAssembler
#   Source/Compiler/     KestrelCompilerCore (Compile())    + Main.cpp -> Binary/KestrelCompiler
#   Source/Toolchain/    KestrelToolchain: a .c, .s or .bin to a memory image, in-process
#   Source/Emulator/     KestrelEmulatorCore (the reference machine) + Main.cpp -> Binary/KestrelEmulator
#   Source/Simulator/    KestrelSimulatorCore (the gate-level simulation) + the editor -> Binary/KestrelSimulator
#   Source/Validator/    emulator vs gate-level computer, same final state? -> Binary/KestrelValidator
#   Programs/*.s         -> Binary/Programs/*.bin (assembled with our own assembler)
#   Programs/*.c         -> Binary/Programs/*_c.bin (compiled, then assembled)
#   Tools/make_*.py      -> Build/Components/*.yaml and *.bin: the hardware, generated at build time
#                        (Python 3; target "hardware"). Not in version control.
#   Tools/eda/kicad.py   -> Build/KiCad/: the hardware as KiCad schematics (target "kicad", optional)
#
# Configure and build:   cmake -S . -B Build && cmake --build Build
# Run a program:         cmake --build Build --target run_Fibonacci_c       (emulator)
#                        cmake --build Build --target simulate_Fibonacci_c  (gate-level Kestrel16 Computer)
# Any source, by hand:   Binary/KestrelEmulator --run Programs/Fibonacci.c
#                        Binary/KestrelSimulator --run "Kestrel16 Computer" --program Programs/Fibonacci.c
# Check the hardware:    Binary/KestrelValidator Programs/Fibonacci.c
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
# Components: each has its own CMakeLists.txt. Each tool is a library with a thin main, so the
# others can use it in-process. Order matters only in that a library comes before its users.
set(COMPONENTS_DIR ${CMAKE_SOURCE_DIR}/Build/Components)
add_subdirectory(Source/Kestrel)
add_subdirectory(Source/Assembler)
add_subdirectory(Source/Compiler)
add_subdirectory(Source/Toolchain)
add_subdirectory(Source/Emulator)
add_subdirectory(Source/Simulator)
add_subdirectory(Source/Validator)

# ---------------------------------------------------------------------------
# The hardware: Tools/make_*.py write the components (yaml netlists) and ROM images into
# Build/Components/. They read the machine's definition headers, and make_cpu.py reads the components
# the other two write, so they run in this order.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(HARDWARE_NAMES "ALU 16" "Bus Driver 8" "Bus Driver 16" "Clock Stretcher" "Control Unit" "Counter 4"
    "Counter 16" "Decoder 3to8" "Immediate Unit" "Interrupt Controller" "Kestrel16 CPU" "Kestrel16 Computer"
    "Mux 2x4" "Program Counter" "Register 16" "Register File" "Shifter 16" "Status" "System Slot"
    "Terminal Card" "Timer")
set(HARDWARE_FILES)
foreach(name ${HARDWARE_NAMES})
    list(APPEND HARDWARE_FILES "${COMPONENTS_DIR}/${name}.yaml")
endforeach()
foreach(image "ALU Slice" "Control A" "Control B" "Address Decode")
    list(APPEND HARDWARE_FILES "${COMPONENTS_DIR}/${image}.bin")
endforeach()
file(GLOB HARDWARE_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/Tools/*.py ${CMAKE_SOURCE_DIR}/Source/Kestrel/*.h)
add_custom_command(
    OUTPUT ${HARDWARE_FILES}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${COMPONENTS_DIR}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Tools/make_alu.py
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Tools/make_registers.py
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Tools/make_cpu.py
    DEPENDS ${HARDWARE_SOURCES}
    COMMENT "Generating the hardware components"
    VERBATIM
)
add_custom_target(hardware ALL DEPENDS ${HARDWARE_FILES})
add_dependencies(KestrelSimulator hardware)

# The schematics: Tools/eda/kicad.py turns the hardware into a KiCad project, one sheet per
# component, chips from Tools/eda/parts.yaml, in Build/KiCad/<component>. Not part of the default
# build: it needs KiCad 9 (for its symbol libraries) and PyYAML.
set(KICAD_TOP "Kestrel16 Computer" CACHE STRING "The component the kicad target exports")
add_custom_target(kicad
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Tools/eda/kicad.py ${KICAD_TOP}
            -o ${CMAKE_BINARY_DIR}/KiCad/${KICAD_TOP}
    COMMENT "Generating the KiCad schematics for ${KICAD_TOP}"
    VERBATIM
)
add_dependencies(kicad hardware)
find_program(KICAD_CLI kicad-cli PATHS "$ENV{ProgramFiles}/KiCad/9.0/bin" "/Applications/KiCad/KiCad.app/Contents/MacOS")

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
        COMMAND $<TARGET_FILE:KestrelSimulator> --components ${COMPONENTS_DIR}
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
        COMMAND $<TARGET_FILE:KestrelSimulator> --components ${COMPONENTS_DIR}
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
# The KiCad schematics, if KiCad is installed: kicad-cli's netlist of them must match the
# components net for net, with no ERC errors.
if(KICAD_CLI)
    add_test(NAME kicad_round_trip COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Tools/eda/kicad.py
             "Kestrel16 Computer" -o ${CMAKE_BINARY_DIR}/KiCad/Test --check --kicad-cli ${KICAD_CLI})
    set_tests_properties(kicad_round_trip PROPERTIES TIMEOUT 300)
endif()

# What each program prints (on the emulator; the validate_ tests below hold the hardware to it).
set(EXPECTED_Fibonacci "46368.timer interrupts: 109")
set(EXPECTED_Fibonacci_c "28657 46368")
set(EXPECTED_Shifts_c "3EFE 1CA6")     # every shift amount, byte and word stores
set(EXPECTED_Interrupts "3390 1065")   # timer interrupts landing on every kind of instruction
set(EXPECTED_IoBus "00A5 0000 0000 0001 0000 0000 0000 0034 0000 0000")  # the system slot's registers
# The shell reads commands: type a couple in, and --run stops once they run out.
if(EXISTS ${CMAKE_SOURCE_DIR}/Programs/Shell.c)
    add_test(NAME program_Shell_c COMMAND ${CMAKE_COMMAND} "-DCOMMAND=$<TARGET_FILE:KestrelEmulator>;--run;${PROGRAM_OUTPUT_DIR}/Shell_c.bin"
             "-DINPUT=calc 7 * 6|rev Kestrel|" -DOUTPUT_DIRECTORY=${CMAKE_BINARY_DIR}/Shell_c
             -P ${CMAKE_SOURCE_DIR}/Tests/RunWithInput.cmake)
    set_tests_properties(program_Shell_c PROPERTIES PASS_REGULAR_EXPRESSION "42.*lertseK")
endif()
foreach(name Fibonacci Fibonacci_c Shifts_c Interrupts IoBus)
    if(TEST program_${name})
        set_tests_properties(program_${name} PROPERTIES PASS_REGULAR_EXPRESSION "${EXPECTED_${name}}")
    endif()
endforeach()

# Differential tests: each program, built from source, has to end in the same state (memory,
# output, and registers and flags for programs ending in __dump) on the gate-level computer as on
# the emulator. See ValidatorDesign.md.
foreach(source ${PROGRAM_SOURCES} ${C_PROGRAM_SOURCES})
    get_filename_component(name ${source} NAME_WE)
    get_filename_component(extension ${source} EXT)
    if(extension STREQUAL ".c")
        set(name ${name}_c)
    endif()
    if(name STREQUAL "Shell_c")
        continue()  # waits for typed commands; it never halts
    endif()
    add_test(NAME validate_${name} COMMAND KestrelValidator --components ${COMPONENTS_DIR} ${source})
    set_tests_properties(validate_${name} PROPERTIES TIMEOUT 600)
endforeach()