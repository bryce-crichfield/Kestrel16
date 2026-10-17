# The reference machine running a program -> Binary/KestrelEmulator
add_executable(KestrelEmulator Main.cpp)
target_link_libraries(KestrelEmulator PRIVATE KestrelToolchain)
