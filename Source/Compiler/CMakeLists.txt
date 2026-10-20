# KestrelCompilerCore (Compile()) + a thin main -> Binary/KestrelCompiler
add_library(KestrelCompilerCore STATIC
    Generator.cpp
    Compiler.cpp
    Expressions.cpp
    Lexer.cpp
    Parser.cpp
    Preprocessor.cpp
    Program.cpp
    Types.cpp
)
target_link_libraries(KestrelCompilerCore PUBLIC KestrelCore)

add_executable(KestrelCompiler Main.cpp)
target_link_libraries(KestrelCompiler PRIVATE KestrelCompilerCore)
