// ComponentFile.cpp -- components as YAML files, and ROM/RAM images.

#include "ComponentFile.h"
#include "Kestrel/Files.h"

#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <stdexcept>

namespace fs = std::filesystem;

namespace Kestrel {

static const char* const KindKeys[] = {"Input", "Output", "Low", "High", "Clock", "Buffer", "Not",
                                       "And", "Or", "Nand", "Nor", "Xor", "Xnor", "FlipFlop",
                                       "TriState", "ROM", "RAM", "Terminal"};
static_assert(sizeof KindKeys / sizeof KindKeys[0] == PrimitiveCount, "one key per primitive");

static std::string ComponentPath(const std::string& name) {
    return componentsDirectory + name + ".yaml";
}

std::string Trim(const std::string& text) {
    size_t first = text.find_first_not_of(" \t"), last = text.find_last_not_of(" \t");
    return first == std::string::npos ? "" : text.substr(first, last - first + 1);
}

int RomBytesPerWord(const MemoryData& rom) {
    return (rom.dataBits + 7) / 8;
}

std::string LoadRomImage(MemoryData& rom) {
    std::vector<uint8_t> bytes;
    if (!ReadFile((fs::path(componentsDirectory) / rom.image).string(), bytes)) {
        return "can't open image '" + rom.image + "'";
    }
    return LoadRomBytes(rom, bytes);
}

std::string LoadRomBytes(MemoryData& rom, const std::vector<uint8_t>& image) {
    std::vector<uint8_t> bytes = image;
    if (rom.lane) {  // one byte lane of a 16-bit memory, as an EPROM programmer's even/odd split does
        std::vector<uint8_t> lane;
        for (size_t index = static_cast<size_t>(rom.lane - 1); index < bytes.size(); index += 2) {
            lane.push_back(bytes[index]);
        }
        bytes.swap(lane);
    }

    size_t bytesPerWord = static_cast<size_t>(RomBytesPerWord(rom));
    std::fill(rom.words.begin(), rom.words.end(), 0);
    size_t wordCount = (bytes.size() + bytesPerWord - 1) / bytesPerWord;
    for (size_t address = 0; address < std::min(wordCount, rom.words.size()); address++) {
        uint32_t word = 0;
        for (size_t byte = 0; byte < bytesPerWord && address * bytesPerWord + byte < bytes.size(); byte++) {
            word |= static_cast<uint32_t>(bytes[address * bytesPerWord + byte]) << (8 * byte);
        }
        rom.words[address] = word & rom.DataMask();
    }
    if (wordCount > rom.words.size()) {
        return "image '" + rom.image + "' has " + std::to_string(wordCount) + " words; only the first " +
               std::to_string(rom.words.size()) + " fit";
    }
    return "";
}

bool SaveRomImage(const MemoryData& rom, const std::string& path) {
    FILE* file = fopen(path.c_str(), "wb");
    if (!file) {
        return false;
    }
    size_t bytesPerWord = static_cast<size_t>(RomBytesPerWord(rom));
    std::vector<uint8_t> bytes;
    for (uint32_t word : rom.words) {
        for (size_t byte = 0; byte < bytesPerWord; byte++) {
            bytes.push_back(static_cast<uint8_t>(word >> (8 * byte)));
        }
    }
    bool ok = fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    return (fclose(file) == 0) && ok;
}

constexpr int RomWordsPerRow = 16;

// "0010: 01 02 FF" for every row of the ROM that isn't all zero.
static std::vector<std::string> RomContentRows(const MemoryData& rom) {
    std::vector<std::string> rows;
    int addressDigits = std::max(4, (rom.addressBits + 3) / 4), wordDigits = (rom.dataBits + 3) / 4;
    for (size_t row = 0; row < rom.words.size(); row += RomWordsPerRow) {
        size_t end = std::min(row + RomWordsPerRow, rom.words.size());
        while (end > row && rom.words[end - 1] == 0) {
            end--;  // trailing zeros needn't be written
        }
        if (end == row) {
            continue;
        }
        char text[32];
        snprintf(text, sizeof text, "%0*zX:", addressDigits, row);
        std::string line = text;
        for (size_t address = row; address < end; address++) {
            snprintf(text, sizeof text, " %0*X", wordDigits, static_cast<unsigned>(rom.words[address]));
            line += text;
        }
        rows.push_back(line);
    }
    return rows;
}

static void ParseRomContentRow(MemoryData& rom, const std::string& line) {
    size_t colon = line.find(':');
    if (colon == std::string::npos) {
        throw std::runtime_error("ROM row '" + line + "' has no 'address:'");
    }
    std::string addressText = Trim(line.substr(0, colon));
    char* end = nullptr;
    unsigned long address = strtoul(addressText.c_str(), &end, 16);
    if (addressText.empty() || *end) {
        throw std::runtime_error("ROM row '" + line + "' has a bad address");
    }
    const char* cursor = line.c_str() + colon + 1;
    for (;;) {
        while (*cursor == ' ' || *cursor == '\t') {
            cursor++;
        }
        if (!*cursor) {
            break;
        }
        unsigned long word = strtoul(cursor, &end, 16);
        if (end == cursor) {
            throw std::runtime_error("ROM row '" + line + "' has a word that isn't hex");
        }
        if (address >= rom.words.size()) {
            throw std::runtime_error("ROM row '" + line + "' runs past the end of the ROM");
        }
        rom.words[address++] = static_cast<uint32_t>(word) & rom.DataMask();
        cursor = end;
    }
}

std::string EndpointText(const Node& node, bool output, int index, int width) {
    std::string text = std::to_string(node.id);
    bool pinNode = node.kind == NodeKind::Input || node.kind == NodeKind::Output;
    if (pinNode && index == 0 && width == node.width) {
        return text;
    }
    auto pinText = [&](int pin) {
        std::string name = PinName(node, output, pin);
        int count = output ? OutputCount(node) : InputCount(node);
        int matches = 0;
        for (int other = 0; other < count; other++) {
            if (PinName(node, output, other) == name) {
                matches++;
            }
        }
        // A name is only usable when it is unique and can't be mistaken for the "->" and ".."
        // syntax of the wire text itself.
        bool hasName = !name.empty() && name != "?";
        bool isUnique = matches == 1;
        bool hasWireSyntax = name.find("->") != std::string::npos || name.find("..") != std::string::npos;
        if (hasName && isUnique && !hasWireSyntax) {
            return name;
        }
        return std::to_string(pin);
    };
    std::string first = pinText(index);
    std::string last = pinText(index + width - 1);
    bool numbers = std::isdigit(static_cast<unsigned char>(first[0])) || std::isdigit(static_cast<unsigned char>(last[0]));
    if (numbers) {
        // don't mix a name and a number
        first = std::to_string(index);
        last = std::to_string(index + width - 1);
    }
    return text + "." + first + (width > 1 ? ".." + last : "");
}

std::string ComponentToYaml(const Component& component) {
    YAML::Emitter out;
    out << YAML::BeginMap;
    out << YAML::Key << "name" << YAML::Value << component.name;
    if (!component.description.empty()) {
        out << YAML::Key << "description" << YAML::Value << component.description;
    }

    out << YAML::Key << "nodes" << YAML::Value << YAML::BeginSeq;
    for (const Node& node : component.nodes) {
        if (!IsMemory(node.kind)) {
            out << YAML::Flow;
        }
        out << YAML::BeginMap;
        out << YAML::Key << "id" << YAML::Value << node.id;
        if (node.kind == NodeKind::Component) {
            out << YAML::Key << "component" << YAML::Value << node.typeName;
        }
        else {
            out << YAML::Key << "kind" << YAML::Value << KindKeys[static_cast<int>(node.kind)];
        }
        if (node.kind == NodeKind::Input || node.kind == NodeKind::Output) {
            out << YAML::Key << "label" << YAML::Value << node.label;
            if (node.width > 1) {
                out << YAML::Key << "width" << YAML::Value << node.width;
            }
        }
        out << YAML::Key << "at" << YAML::Value << YAML::Flow << YAML::BeginSeq << std::lround(node.position.x)
            << std::lround(node.position.y) << YAML::EndSeq;
        if (IsMemory(node.kind)) {
            const MemoryData& rom = *node.memory;
            out << YAML::Key << "address" << YAML::Value << rom.addressBits;
            out << YAML::Key << "data" << YAML::Value << rom.dataBits;
            auto names = [&](const char* key, const std::vector<std::string>& list) {
                if (std::all_of(list.begin(), list.end(), [](const std::string& name) { return name.empty(); })) {
                    return;
                }
                out << YAML::Key << key << YAML::Value << YAML::Flow << YAML::BeginSeq;
                for (const std::string& name : list) {
                    out << YAML::DoubleQuoted << name;
                }
                out << YAML::EndSeq;
            };
            names("inputs", rom.addressNames);
            names("outputs", rom.dataNames);
            if (!rom.image.empty()) {
                out << YAML::Key << "image" << YAML::Value << rom.image;
                if (rom.lane) {
                    out << YAML::Key << "lane" << YAML::Value << (rom.lane == 1 ? "even" : "odd");
                }
            }
            else if (node.kind == NodeKind::Rom) {
                out << YAML::Key << "contents" << YAML::Value << YAML::BeginSeq;
                for (const std::string& row : RomContentRows(rom)) {
                    out << YAML::DoubleQuoted << row;
                }
                out << YAML::EndSeq;
            }
        }
        out << YAML::EndMap;
    }
    out << YAML::EndSeq;

    out << YAML::Key << "wires" << YAML::Value << YAML::BeginSeq;
    for (const Wire& wire : component.wires) {
        const Node* from = component.FindNode(wire.fromNode);
        const Node* to = component.FindNode(wire.toNode);
        if (!from || !to) {
            continue;
        }
        int width = static_cast<int>(wire.width);
        out << EndpointText(*from, true, static_cast<int>(wire.fromPin), width) + " -> " +
                   EndpointText(*to, false, static_cast<int>(wire.toPin), width);
    }
    out << YAML::EndSeq;
    out << YAML::EndMap;
    return std::string(out.c_str()) + "\n";
}

static bool SaveComponent(Component& component) {
    std::error_code error;
    fs::create_directories(componentsDirectory, error);

    MergeBusWires(component);
    std::string text = ComponentToYaml(component);
    FILE* file = fopen(ComponentPath(component.name).c_str(), "wb");
    if (!file) {
        return false;
    }
    bool ok = fwrite(text.data(), 1, text.size(), file) == text.size();
    ok = (fclose(file) == 0) && ok;
    if (!ok) {
        return false;
    }

    // A renamed component leaves its old file behind; remove it now that the new one is safe.
    if (!component.savedName.empty() && component.savedName != component.name) {
        fs::remove(ComponentPath(component.savedName), error);
    }
    component.savedName = component.name;
    component.dirty = false;
    return true;
}

std::unique_ptr<Component> ComponentFromYaml(const std::string& text) {
    YAML::Node root = YAML::Load(text);
    auto component = std::make_unique<Component>();
    component->name = root["name"].as<std::string>("");
    component->description = root["description"].as<std::string>("");
    if (!IsValidComponentName(component->name)) {
        throw std::runtime_error("bad or missing name '" + component->name + "'");
    }

    for (const YAML::Node& entry : root["nodes"]) {
        Node node;
        node.id = entry["id"].as<uint32_t>();
        if (node.id == 0 || component->FindNode(node.id)) {
            throw std::runtime_error("node id " + std::to_string(node.id) + " is zero or used twice");
        }
        if (entry["component"]) {
            node.kind = NodeKind::Component;
            node.typeName = entry["component"].as<std::string>();
        }
        else {
            std::string kind = entry["kind"].as<std::string>("");
            const char* const* found = std::find(std::begin(KindKeys), std::end(KindKeys), kind);
            if (found == std::end(KindKeys)) {
                throw std::runtime_error("node " + std::to_string(node.id) + ": unknown kind '" + kind + "'");
            }
            node.kind = static_cast<NodeKind>(found - std::begin(KindKeys));
        }
        if (node.kind == NodeKind::Input || node.kind == NodeKind::Output) {
            node.label = entry["label"].as<std::string>(UniquePinLabel(*component, node.kind));
            node.width = entry["width"].as<int>(1);
            if (node.width < 1 || node.width > MaxBusWidth) {
                throw std::runtime_error("node " + std::to_string(node.id) + ": width is 1-" + std::to_string(MaxBusWidth));
            }
        }
        if (IsMemory(node.kind)) {
            node.memory = std::make_shared<MemoryData>();
            MemoryData& rom = *node.memory;
            bool ram = node.kind == NodeKind::Ram;
            int addressBits = entry["address"].as<int>(ram ? 15 : 4), dataBits = entry["data"].as<int>(8);
            if (addressBits < 1 || addressBits > MaxMemoryAddressBits || dataBits < 1 || dataBits > MaxMemoryDataBits) {
                throw std::runtime_error("node " + std::to_string(node.id) + ": a ROM or RAM has 1-" +
                                         std::to_string(MaxMemoryAddressBits) + " address bits and 1-" +
                                         std::to_string(MaxMemoryDataBits) + " data bits");
            }
            rom.Resize(addressBits, dataBits);
            auto names = [&](const char* key, std::vector<std::string>& list) {
                size_t index = 0;
                for (const YAML::Node& name : entry[key]) {
                    if (index < list.size()) {
                        list[index++] = name.as<std::string>();
                    }
                }
            };
            names("inputs", rom.addressNames);
            names("outputs", rom.dataNames);
            rom.image = entry["image"].as<std::string>("");
            std::string lane = entry["lane"].as<std::string>("");
            if (!lane.empty() && lane != "even" && lane != "odd") {
                throw std::runtime_error("node " + std::to_string(node.id) + ": lane is even or odd");
            }
            if (lane == "even") {
                rom.lane = 1;
            }
            else if (lane == "odd") {
                rom.lane = 2;
            }
            else {
                rom.lane = 0;
            }
            if (!rom.image.empty()) {
                std::string problem = LoadRomImage(rom);
                if (!problem.empty()) {
                    component->loadWarnings.push_back(component->name + ": node " + std::to_string(node.id) + ": " + problem);
                }
            }
            else if (!ram) {
                for (const YAML::Node& row : entry["contents"]) {
                    ParseRomContentRow(rom, row.as<std::string>());
                }
            }
        }
        if (const YAML::Node at = entry["at"]) {
            node.position = ImVec2(at[0].as<float>(), at[1].as<float>());
        }
        else {
            // No position given: lay out left to right in columns of eight.
            size_t index = component->nodes.size();
            node.position = ImVec2(60.0f + 200.0f * static_cast<float>(index / 8), 40.0f + 90.0f * static_cast<float>(index % 8));
        }
        node.placement = Node::Placement::Grid;
        component->nodes.push_back(node);
        component->nextId = std::max(component->nextId, node.id + 1);
    }

    for (const YAML::Node& entry : root["wires"]) {
        std::string line = entry.as<std::string>();
        size_t arrow = line.find("->");
        if (arrow == std::string::npos) {
            throw std::runtime_error("wire '" + line + "' has no '->'");
        }
        component->pendingWires.push_back({Trim(line.substr(0, arrow)), Trim(line.substr(arrow + 2))});
    }
    return component;
}

// Turns "4.Y" (or a run of pins, "4.A0..A7") into a node id, first pin and pin count.
// Returns an error message, or "" on success.
static std::string ResolveEndpoint(const Component& component, const std::string& text, bool output, uint32_t& nodeId, uint32_t& pin, uint32_t& width) {
    size_t dot = text.find('.');
    std::string idText = text.substr(0, dot), pinText = dot == std::string::npos ? "" : text.substr(dot + 1);
    char* end = nullptr;
    unsigned long id = strtoul(idText.c_str(), &end, 10);
    const Node* node = idText.empty() || *end ? nullptr : component.FindNode(static_cast<uint32_t>(id));
    if (!node) {
        return "no node '" + idText + "'";
    }
    nodeId = node->id;
    pin = 0;
    width = 1;
    if (pinText.empty()) {
        if (node->kind == NodeKind::Input || node->kind == NodeKind::Output) {
            width = static_cast<uint32_t>(node->width);
        }
        return "";
    }

    auto find = [&](const std::string& name, uint32_t& found) -> std::string {
        int count = output ? OutputCount(*node) : InputCount(*node);
        for (int index = 0; index < count; index++) {
            if (PinName(*node, output, index) == name) {
                found = static_cast<uint32_t>(index);
                return "";
            }
        }
        unsigned long number = strtoul(name.c_str(), &end, 10);
        if (name.empty() || *end) {
            return "node " + idText + " has no " + (output ? "output" : "input") + " pin '" + name + "'";
        }
        found = static_cast<uint32_t>(number);  // out-of-range numbers are dropped later by PruneWires
        return "";
    };
    size_t range = pinText.find("..");
    if (range == std::string::npos) {
        return find(pinText, pin);
    }
    uint32_t last = 0;
    std::string problem = find(pinText.substr(0, range), pin);
    if (problem.empty()) {
        problem = find(pinText.substr(range + 2), last);
    }
    if (problem.empty() && last < pin) {
        problem = "'" + pinText + "' runs backwards";
    }
    width = last - pin + 1;
    return problem;
}

std::vector<std::string> ResolveWires(Component& component) {
    std::vector<std::string> problems;
    for (const PendingWire& pending : component.pendingWires) {
        Wire wire;
        uint32_t toWidth = 0;
        std::string problem = ResolveEndpoint(component, pending.from, true, wire.fromNode, wire.fromPin, wire.width);
        if (problem.empty()) {
            problem = ResolveEndpoint(component, pending.to, false, wire.toNode, wire.toPin, toWidth);
        }
        if (problem.empty() && toWidth != wire.width) {
            problem = "one end is " + std::to_string(wire.width) + " bits wide, the other " + std::to_string(toWidth);
        }
        if (!problem.empty()) {
            problems.push_back(component.name + ": wire '" + pending.from + " -> " + pending.to + "': " + problem);
            continue;
        }
        wire.id = component.nextId++;
        component.wires.push_back(wire);
    }
    component.pendingWires.clear();
    MergeBusWires(component);
    return problems;
}

static std::unique_ptr<Component> LoadComponent(const std::string& path, std::string& problem) {
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) {
        problem = "can't open it";
        return nullptr;
    }
    std::string text;
    char buffer[4096];
    for (size_t count; (count = fread(buffer, 1, sizeof buffer, file)) > 0;) {
        text.append(buffer, count);
    }
    fclose(file);

    try {
        std::unique_ptr<Component> component = ComponentFromYaml(text);
        component->savedName = component->name;
        return component;
    } catch (const std::exception& exception) {
        problem = exception.what();
        return nullptr;
    }
}

std::vector<std::string> LoadLibrary() {
    std::vector<std::string> problems;
    std::error_code error;
    if (!fs::is_directory(componentsDirectory, error)) {
        return problems;
    }

    std::vector<fs::path> paths;
    for (const auto& entry : fs::directory_iterator(componentsDirectory, error)) {
        if (entry.path().extension() == ".yaml") {
            paths.push_back(entry.path());
        }
    }
    std::sort(paths.begin(), paths.end());

    for (const fs::path& path : paths) {
        std::string problem;
        std::unique_ptr<Component> component = LoadComponent(path.string(), problem);
        if (!component) {
            problems.push_back("Couldn't read " + path.filename().string() + ": " + problem);
        }
        else if (FindComponent(component->name)) {
            problems.push_back("Skipped " + path.filename().string() + ": duplicate name '" + component->name + "'");
        }
        else {
            library.push_back(std::move(component));
        }
    }
    for (auto& component : library) {
        std::vector<std::string> wireProblems = ResolveWires(*component);
        problems.insert(problems.end(), component->loadWarnings.begin(), component->loadWarnings.end());
        problems.insert(problems.end(), wireProblems.begin(), wireProblems.end());
        component->loadWarnings.clear();
    }
    return problems;
}

int SaveAll() {
    int failures = 0;
    for (auto& component : library) {
        if (component->dirty && !SaveComponent(*component)) {
            failures++;
        }
    }
    return failures;
}

bool AnyDirty() {
    for (auto& component : library) {
        if (component->dirty) {
            return true;
        }
    }
    return false;
}

}  // namespace Kestrel
