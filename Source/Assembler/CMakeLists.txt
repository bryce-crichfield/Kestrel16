# KestrelAssemblerCore (Assemble()) + a thin main -> Binary/KestrelAssembler
add_library(KestrelAssemblerCore STATIC Assembler.cpp)
target_link_libraries(KestrelAssemblerCore PUBLIC KestrelCore)

add_executable(KestrelAssembler Main.cpp)
target_link_libraries(KestrelAssembler PRIVATE KestrelAssemblerCore)
