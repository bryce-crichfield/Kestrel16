// KestrelCompiler -- C to Kestrel16 assembly (see Compiler.h for the language).
//
// Run:    KestrelCompiler prog.c             writes prog.s
//         KestrelCompiler -o out.s prog.c    choose the output name
//         KestrelCompiler -E prog.c          writes the preprocessed text, prog.i
// Then:   KestrelAssembler prog.s            writes prog.bin
//
// -I DIR adds an include folder, -D NAME[=VALUE] defines a macro.

#include "Compiler.h"
#include "Kestrel/Files.h"

#include <cstdio>

static void Usage(const char* program) {
    fprintf(stderr, "usage: %s [-o output.s] [-E] [-I dir] [-D name[=value]] source.c\n", program);
}

int main(int argc, char** argv) {
    std::string sourceName, outputPath;
    bool preprocessOnly = false;
    Kestrel::CompileOptions options;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-o" && i + 1 < argc) {
            outputPath = argv[++i];
        }
        else if (arg == "-E") {
            preprocessOnly = true;
        }
        else if (arg.rfind("-I", 0) == 0 && (arg.size() > 2 || i + 1 < argc)) {
            options.includePaths.push_back(arg.size() > 2 ? arg.substr(2) : argv[++i]);
        }
        else if (arg.rfind("-D", 0) == 0 && (arg.size() > 2 || i + 1 < argc)) {
            std::string definition = arg.size() > 2 ? arg.substr(2) : argv[++i];
            size_t equals = definition.find('=');
            options.defines.push_back({definition.substr(0, equals), equals == std::string::npos ? "1" : definition.substr(equals + 1)});
        }
        else if (arg[0] == '-' || !sourceName.empty()) {
            Usage(argv[0]);
            return 2;
        }
        else {
            sourceName = arg;
        }
    }
    if (sourceName.empty()) {
        Usage(argv[0]);
        return 2;
    }
    if (outputPath.empty()) {
        outputPath = Kestrel::ReplaceExtension(sourceName, preprocessOnly ? ".i" : ".s");
    }

    std::string text;
    if (!Kestrel::ReadFile(sourceName, text)) {
        perror(sourceName.c_str());
        return 1;
    }
    Kestrel::CompileResult result = preprocessOnly ? Kestrel::Preprocess(sourceName, text, options)
                                                   : Kestrel::Compile(sourceName, text, options);
    for (const std::string& warning : result.warnings) {
        fprintf(stderr, "%s\n", warning.c_str());
    }
    if (!result.ok) {
        fprintf(stderr, "%s\n", result.error.c_str());
        return 1;
    }
    if (!Kestrel::WriteFile(outputPath, result.output)) {
        perror(outputPath.c_str());
        return 1;
    }
    return 0;
}
