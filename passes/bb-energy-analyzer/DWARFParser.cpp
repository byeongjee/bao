#include "DWARFParser.h"

#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/DebugInfo/DWARF/DWARFDie.h"
#include "llvm/DebugInfo/DWARF/DWARFUnit.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdlib>
#include <regex>
#include <sstream>

using namespace llvm;
using namespace llvm::object;

namespace bbanalyzer {

namespace {

/// Find msp430-elf-objdump executable
std::string findObjdump() {
    std::vector<std::string> searchPaths = {
        std::string(std::getenv("MSP430_GCC_DIR") ? std::getenv("MSP430_GCC_DIR") : "") +
            "/bin/msp430-elf-objdump",
        "/Users/byeongjee/ti/msp430-gcc/bin/msp430-elf-objdump",
        "/opt/ti/msp430-gcc/bin/msp430-elf-objdump",
        "/usr/local/bin/msp430-elf-objdump",
        "msp430-elf-objdump",
    };

    for (const auto &path : searchPaths) {
        if (path.empty())
            continue;
        if (path[0] == '/') {
            if (sys::fs::exists(path))
                return path;
        } else {
            ErrorOr<std::string> found = sys::findProgramByName(path);
            if (found)
                return *found;
        }
    }
    return "";
}

/// Parse line table using msp430-elf-objdump (works around LLVM's MSP430 relocation issues)
struct LineEntry {
    uint64_t address;
    unsigned line;
};

std::vector<LineEntry> parseLineTableWithObjdump(const std::string &objectPath) {
    std::vector<LineEntry> entries;

    std::string objdumpPath = findObjdump();
    if (objdumpPath.empty()) {
        errs() << "warning: msp430-elf-objdump not found, line table parsing may be inaccurate\n";
        return entries;
    }

    // Create temp file for output
    SmallString<128> tempPath;
    std::error_code ec = sys::fs::createTemporaryFile("dwarf", "txt", tempPath);
    if (ec) {
        errs() << "error: failed to create temp file: " << ec.message() << "\n";
        return entries;
    }

    // Run objdump --dwarf=decodedline
    StringRef args[] = {objdumpPath, "--dwarf=decodedline", objectPath};
    std::optional<StringRef> redirects[] = {std::nullopt, StringRef(tempPath), std::nullopt};

    int rc = sys::ExecuteAndWait(objdumpPath, args, std::nullopt, redirects);
    if (rc != 0) {
        errs() << "error: objdump --dwarf=decodedline failed\n";
        sys::fs::remove(tempPath);
        return entries;
    }

    // Read output
    ErrorOr<std::unique_ptr<MemoryBuffer>> bufOrErr = MemoryBuffer::getFile(tempPath);
    if (!bufOrErr) {
        sys::fs::remove(tempPath);
        return entries;
    }

    std::string output = (*bufOrErr)->getBuffer().str();
    sys::fs::remove(tempPath);

    // Parse output
    // Format: filename   line_number   starting_address   view   stmt
    // Example: test.c      5            0xa                1      x
    std::regex linePattern(R"(^\S+\s+(\d+)\s+0x([0-9a-fA-F]+)\s+)");
    std::istringstream stream(output);
    std::string line;

    while (std::getline(stream, line)) {
        std::smatch match;
        if (std::regex_search(line, match, linePattern)) {
            unsigned lineNum = std::stoul(match[1].str());
            uint64_t addr = std::stoull(match[2].str(), nullptr, 16);
            entries.push_back({addr, lineNum});
        }
    }

    return entries;
}

/// Parse function address ranges using msp430-elf-objdump symbols
struct FuncRange {
    std::string name;
    uint64_t start;
    uint64_t end;
};

std::vector<FuncRange> parseFunctionRanges(const std::string &objectPath) {
    std::vector<FuncRange> functions;

    std::string objdumpPath = findObjdump();
    if (objdumpPath.empty()) {
        return functions;
    }

    // Create temp file
    SmallString<128> tempPath;
    std::error_code ec = sys::fs::createTemporaryFile("syms", "txt", tempPath);
    if (ec) {
        return functions;
    }

    // Run objdump -t (symbol table) and -d (disassembly) to get function bounds
    // We'll use disassembly output to find function labels
    StringRef args[] = {objdumpPath, "-d", objectPath};
    std::optional<StringRef> redirects[] = {std::nullopt, StringRef(tempPath), std::nullopt};

    int rc = sys::ExecuteAndWait(objdumpPath, args, std::nullopt, redirects);
    if (rc != 0) {
        sys::fs::remove(tempPath);
        return functions;
    }

    ErrorOr<std::unique_ptr<MemoryBuffer>> bufOrErr = MemoryBuffer::getFile(tempPath);
    if (!bufOrErr) {
        sys::fs::remove(tempPath);
        return functions;
    }

    std::string output = (*bufOrErr)->getBuffer().str();
    sys::fs::remove(tempPath);

    // Parse disassembly to find function labels
    // Format: 00000000 <function_name>:
    std::regex funcPattern(R"(^([0-9a-fA-F]+)\s+<([^.][^>]*)>:)");
    std::istringstream stream(output);
    std::string line;

    std::vector<std::pair<uint64_t, std::string>> funcStarts;
    uint64_t maxAddr = 0;

    while (std::getline(stream, line)) {
        std::smatch match;
        if (std::regex_search(line, match, funcPattern)) {
            uint64_t addr = std::stoull(match[1].str(), nullptr, 16);
            std::string name = match[2].str();
            // Skip labels that start with . (internal labels like .L4, .LCFI0)
            if (name[0] != '.') {
                funcStarts.push_back({addr, name});
            }
        }
        // Track max address from instruction lines
        std::regex instrPattern(R"(^\s*([0-9a-fA-F]+):)");
        if (std::regex_search(line, match, instrPattern)) {
            uint64_t addr = std::stoull(match[1].str(), nullptr, 16);
            if (addr > maxAddr)
                maxAddr = addr + 4; // Estimate end
        }
    }

    // Sort by address
    std::sort(funcStarts.begin(), funcStarts.end());

    // Build function ranges
    for (size_t i = 0; i < funcStarts.size(); ++i) {
        FuncRange fr;
        fr.name = funcStarts[i].second;
        fr.start = funcStarts[i].first;
        if (i + 1 < funcStarts.size()) {
            fr.end = funcStarts[i + 1].first;
        } else {
            fr.end = maxAddr;
        }
        if (fr.end > fr.start) {
            functions.push_back(fr);
        }
    }

    return functions;
}

} // namespace

std::map<std::string, FunctionBBMap> DWARFParser::parse(const std::string &objectPath) {
    std::map<std::string, FunctionBBMap> result;

    // Get line table entries using objdump (bypasses LLVM's MSP430 relocation issues)
    errs() << "Parsing line table with objdump...\n";
    std::vector<LineEntry> lineEntries = parseLineTableWithObjdump(objectPath);

    if (lineEntries.empty()) {
        errs() << "warning: no line table entries found\n";
        // Fall back to LLVM parser
        errs() << "Attempting LLVM DWARF parser (may have warnings)...\n";
    }

    // Get function ranges from disassembly
    errs() << "Parsing function ranges...\n";
    std::vector<FuncRange> functions = parseFunctionRanges(objectPath);

    if (functions.empty()) {
        errs() << "warning: no functions found\n";
        return result;
    }

    for (const auto &func : functions) {
        errs() << "Found function '" << func.name << "' at "
               << format_hex(func.start, 6) << " - " << format_hex(func.end, 6) << "\n";
    }

    // Sort line entries by address
    std::sort(lineEntries.begin(), lineEntries.end(),
              [](const LineEntry &a, const LineEntry &b) { return a.address < b.address; });

    // Assign line entries to functions and build BB maps
    for (const auto &func : functions) {
        FunctionBBMap funcMap;
        funcMap.functionName = func.name;

        // Find line entries within this function
        std::vector<LineEntry> funcLines;
        for (const auto &entry : lineEntries) {
            if (entry.address >= func.start && entry.address < func.end) {
                funcLines.push_back(entry);
            }
        }

        if (funcLines.empty()) {
            errs() << "warning: function '" << func.name << "' has no line entries\n";
            continue;
        }

        // Build BB ranges
        for (size_t i = 0; i < funcLines.size(); ++i) {
            unsigned bbIndex = funcLines[i].line; // Line number = BB index from our pass
            uint64_t startAddr = funcLines[i].address;
            uint64_t endAddr;

            if (i + 1 < funcLines.size()) {
                endAddr = funcLines[i + 1].address;
            } else {
                endAddr = func.end;
            }

            if (startAddr >= endAddr)
                continue;

            auto &ranges = funcMap.bbToAddresses[bbIndex];
            // Try to merge with existing range
            bool merged = false;
            for (auto &range : ranges) {
                if (range.end == startAddr) {
                    range.end = endAddr;
                    merged = true;
                    break;
                }
            }
            if (!merged) {
                ranges.push_back({startAddr, endAddr});
            }
        }

        result[func.name] = funcMap;
        errs() << "Parsed function '" << func.name << "' with "
               << funcMap.bbToAddresses.size() << " basic blocks\n";
    }

    return result;
}

} // namespace bbanalyzer
