# KestrelToolchain: a .c, .s or .bin to a memory image, in-process.
add_library(KestrelToolchain STATIC Toolchain.cpp)
target_link_libraries(KestrelToolchain PUBLIC KestrelAssemblerCore KestrelCompilerCore)
