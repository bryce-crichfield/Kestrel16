#include "Toolchain.h"

#include "Assembler/Assembler.h"
#include "Compiler/Compiler.h"
#include "Kestrel/Files.h"

#include <cerrno>
#include <cstring>

namespace Kestrel {

bool BuildProgram(const std::string& path, std::vector<uint8_t>& image, FILE* diagnostics, const AssemblyEdit& editAssembly) {
    size_t dot = path.rfind('.');
    std::string extension = dot == std::string::npos ? "" : path.substr(dot);
    if (extension != ".c" && extension != ".s" && extension != ".bin") {
        fprintf(diagnostics, "%s: expected a .c, .s or .bin file\n", path.c_str());
        return false;
    }
    if (extension == ".bin") {
        if (ReadFile(path, image)) {
            return true;
        }
        fprintf(diagnostics, "%s: %s\n", path.c_str(), strerror(errno));
        return false;
    }

    std::string text;
    if (!ReadFile(path, text)) {
        fprintf(diagnostics, "%s: %s\n", path.c_str(), strerror(errno));
        return false;
    }
    std::string assemblyName = path;
    if (extension == ".c") {
        CompileResult compiled = Compile(path, text);
        for (const std::string& warning : compiled.warnings) {
            fprintf(diagnostics, "%s\n", warning.c_str());
        }
        if (!compiled.ok) {
            fprintf(diagnostics, "%s\ncompiling %s failed\n", compiled.error.c_str(), path.c_str());
            return false;
        }
        text = compiled.output;
        assemblyName = ReplaceExtension(path, ".s");
    }
    if (editAssembly) {
        editAssembly(text, extension == ".c");
    }
    AssemblyResult assembled = Assemble(assemblyName, text);
    for (const std::string& error : assembled.errors) {
        fprintf(diagnostics, "%s\n", error.c_str());
    }
    if (!assembled.errors.empty()) {
        fprintf(diagnostics, "assembling %s failed\n", path.c_str());
        return false;
    }
    image = assembled.image;
    return true;
}

}  // namespace Kestrel
