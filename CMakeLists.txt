cmake_minimum_required(VERSION 3.20)
project(ISA16 LANGUAGES C CXX)

# Layout:
#   Source/Emulator/main.c              -> Binary/Emulator
#   Source/Assembler/asm.c              -> Binary/Assembler
#   Source/Application/Application.cpp  -> Binary/Application   (circuit editor + simulator)
#   Programs/*.s                        -> Binary/Programs/*.bin (assembled with our own assembler)
#   Components/*.yaml                   circuit components saved by the Application
#
# Configure and build:   cmake -S . -B Build && cmake --build Build
# Run a program:         cmake --build Build --target run_Fibonacci
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

option(ISA16_WARNINGS_AS_ERRORS "Treat compiler warnings as errors (our code only)" OFF)
option(ISA16_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer (GCC/Clang)" OFF)

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
# Settings every ISA16 target shares (our code only; vendor code keeps its own flags).
add_library(ISA16Build INTERFACE)
if(MSVC)
    target_compile_options(ISA16Build INTERFACE /W4)
    target_compile_definitions(ISA16Build INTERFACE _CRT_SECURE_NO_WARNINGS)
    if(ISA16_WARNINGS_AS_ERRORS)
        target_compile_options(ISA16Build INTERFACE /WX)
    endif()
else()
    target_compile_options(ISA16Build INTERFACE -Wall -Wextra $<$<COMPILE_LANGUAGE:C>:-pedantic>)
    # The emulator's non-blocking UART uses select()/read(), which strict C99 hides.
    target_compile_definitions(ISA16Build INTERFACE $<$<COMPILE_LANGUAGE:C>:_POSIX_C_SOURCE=200112L>)
    if(ISA16_WARNINGS_AS_ERRORS)
        target_compile_options(ISA16Build INTERFACE -Werror)
    endif()
    if(ISA16_SANITIZE)
        target_compile_options(ISA16Build INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(ISA16Build INTERFACE -fsanitize=address,undefined)
    endif()
endif()
if(MINGW)
    # std::filesystem needs this on older MinGW GCCs; harmless on newer ones.
    target_link_libraries(ISA16Build INTERFACE $<$<VERSION_LESS:$<CXX_COMPILER_VERSION>,9.1>:stdc++fs>)
endif()

# ---------------------------------------------------------------------------
# Tools
add_executable(Emulator Source/Emulator.c)
target_link_libraries(Emulator PRIVATE ISA16Build)

add_executable(Assembler Source/Assembler.c)
target_link_libraries(Assembler PRIVATE ISA16Build)

# Circuit editor and simulator. Components are saved next to the sources, not in Binary/,
# so they live in version control with everything else.
add_executable(Application Source/Application.cpp)
target_link_libraries(Application PRIVATE ISA16Build raylib rlimgui imnodes yaml-cpp::yaml-cpp)
target_compile_definitions(Application PRIVATE
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
        COMMAND $<TARGET_FILE:Assembler> -o ${binary} ${source}
        DEPENDS ${source} Assembler
        COMMENT "Assembling ${name}.s"
        VERBATIM
    )
    list(APPEND PROGRAM_BINARIES ${binary})

    add_custom_target(run_${name}
        COMMAND $<TARGET_FILE:Emulator> ${binary}
        DEPENDS ${binary} Emulator
        USES_TERMINAL
        COMMENT "Running ${name}"
    )
endforeach()

add_custom_target(programs ALL DEPENDS ${PROGRAM_BINARIES})

# Tests: run each program through the emulator. A test passes when it halts cleanly
# (an illegal instruction or the step limit fails it).
enable_testing()
foreach(binary ${PROGRAM_BINARIES})
    get_filename_component(name ${binary} NAME_WE)
    add_test(NAME program_${name} COMMAND Emulator ${binary})
    set_tests_properties(program_${name} PROPERTIES FAIL_REGULAR_EXPRESSION "illegal instruction;step limit")
endforeach()

# The circuit simulator and the component file format check themselves.
add_test(NAME application_self_test COMMAND Application --self-test)
# The machine's components (ALU, ...) against the emulator.
add_test(NAME hardware_check COMMAND Application --components ${CMAKE_SOURCE_DIR}/Components --check-hardware)

if(TEST program_Fibonacci)
    set_tests_properties(program_Fibonacci PROPERTIES PASS_REGULAR_EXPRESSION "28657 46368")
endif()