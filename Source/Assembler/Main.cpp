// KestrelAssembler -- Kestrel16 assembly to a binary image (see Assembler.h for the language).
//
// Run:    KestrelAssembler prog.s             writes prog.bin
//         KestrelAssembler -o out.bin prog.s  choose the output name
//         KestrelAssembler -l prog.s          also print a listing + symbol table

#include "Assembler.h"
#include "Kestrel/Files.h"

#include <cstdio>
#include <cstring>

static void Usage(const char* programName)
{
    fprintf(stderr,
            "usage: %s [-o output.bin] [-l] source.s\n"
            "  -o file   output file (default: source name with .bin)\n"
            "  -l        print a listing and symbol table to stdout\n",
            programName);
}

int main(int argc, char** argv)
{
    std::string sourceName, outputPath;
    bool listing = false;

    for (int index = 1; index < argc; index++) {
        const char* argument = argv[index];
        if (!strcmp(argument, "-o") && index + 1 < argc) outputPath = argv[++index];
        else if (!strcmp(argument, "-l")) listing = true;
        else if (argument[0] == '-' || !sourceName.empty()) { Usage(argv[0]); return 2; }
        else sourceName = argument;
    }
    if (sourceName.empty()) { Usage(argv[0]); return 2; }
    if (outputPath.empty()) outputPath = Kestrel::ReplaceExtension(sourceName, ".bin");

    std::string source;
    if (!Kestrel::ReadFile(sourceName, source)) { perror(sourceName.c_str()); return 1; }

    Kestrel::AssemblyResult result = Kestrel::Assemble(sourceName, source, listing);
    if (listing) fputs(result.listing.c_str(), stdout);
    for (const std::string& error : result.errors) fprintf(stderr, "%s\n", error.c_str());
    if (!result.errors.empty()) {
        size_t count = result.errors.size();
        fprintf(stderr, "%zu error%s, no output written\n", count, count == 1 ? "" : "s");
        return 1;
    }

    if (!Kestrel::WriteFile(outputPath, result.image)) {
        fprintf(stderr, "%s: write failed\n", outputPath.c_str());
        return 1;
    }
    fprintf(stderr, "%s: %zu bytes -> %s\n", sourceName.c_str(), result.image.size(), outputPath.c_str());
    return 0;
}
