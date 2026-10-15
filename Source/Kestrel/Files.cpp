#include "Files.h"

#include <cstdio>

namespace Kestrel {

namespace {

template <typename Buffer>
bool ReadAll(const std::string& path, Buffer& buffer)
{
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) return false;
    buffer.clear();
    char chunk[4096];
    for (size_t count; (count = fread(chunk, 1, sizeof chunk, file)) > 0;) buffer.insert(buffer.end(), chunk, chunk + count);
    bool ok = !ferror(file);
    fclose(file);
    return ok;
}

bool WriteAll(const std::string& path, const void* data, size_t size)
{
    FILE* file = fopen(path.c_str(), "wb");
    if (!file) return false;
    bool ok = fwrite(data, 1, size, file) == size;
    return (fclose(file) == 0) && ok;
}

}  // namespace

bool ReadFile(const std::string& path, std::string& text) { return ReadAll(path, text); }
bool ReadFile(const std::string& path, std::vector<uint8_t>& bytes) { return ReadAll(path, bytes); }
bool WriteFile(const std::string& path, const std::string& text) { return WriteAll(path, text.data(), text.size()); }
bool WriteFile(const std::string& path, const std::vector<uint8_t>& bytes) { return WriteAll(path, bytes.data(), bytes.size()); }

std::string ReplaceExtension(const std::string& path, const std::string& extension)
{
    size_t dot = path.rfind('.'), slash = path.find_last_of("/\\");
    bool hasExtension = dot != std::string::npos && (slash == std::string::npos || dot > slash);
    return (hasExtension ? path.substr(0, dot) : path) + extension;
}

}  // namespace Kestrel
