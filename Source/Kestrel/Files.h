// Files.h -- whole-file reads and writes, and output names, for the tools.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Kestrel {

// Each returns false on failure, with errno set (so perror can say why).
bool ReadFile(const std::string& path, std::string& text);
bool ReadFile(const std::string& path, std::vector<uint8_t>& bytes);
bool WriteFile(const std::string& path, const std::string& text);
bool WriteFile(const std::string& path, const std::vector<uint8_t>& bytes);

// "dir/prog.c" with ".s" is "dir/prog.s"; a name without an extension gets one.
std::string ReplaceExtension(const std::string& path, const std::string& extension);

}  // namespace Kestrel
