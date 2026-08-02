#include "DWARFParser.h"
#include "DWARFLineResolver.h"

#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/DebugInfo/DWARF/DWARFDie.h"
#include "llvm/DebugInfo/DWARF/DWARFUnit.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include "common/Logger.h"

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
    // MSP430_GCC_DIR pins the toolchain; otherwise fall back to PATH.
    if (const char *gccDir = std::getenv("MSP430_GCC_DIR"); gccDir && *gccDir) {
        SmallString<128> candidate(gccDir);
        sys::path::append(candidate, "bin", "msp430-elf-objdump");
        if (sys::fs::exists(candidate))
            return std::string(candidate);
    }

    ErrorOr<std::string> found = sys::findProgramByName("msp430-elf-objdump");
    if (found)
        return *found;
    return "";
}

/// Parse line table using msp430-elf-objdump (works around LLVM's MSP430 relocation issues)
std::vector<LineEntry> parseLineTableWithObjdump(const std::string &objectPath) {
    std::vector<LineEntry> entries;

    std::string objdumpPath = findObjdump();
    if (objdumpPath.empty()) {
        PLOGW << "warning: msp430-elf-objdump not found, line table parsing may be inaccurate";
        return entries;
    }

    // Create temp file for output
    SmallString<128> tempPath;
    std::error_code ec = sys::fs::createTemporaryFile("dwarf", "txt", tempPath);
    if (ec) {
        PLOGE << "error: failed to create temp file: " << ec.message();
        return entries;
    }

    // Run objdump --dwarf=decodedline
    StringRef args[] = {objdumpPath, "--dwarf=decodedline", objectPath};
    std::optional<StringRef> redirects[] = {std::nullopt, StringRef(tempPath), std::nullopt};

    int rc = sys::ExecuteAndWait(objdumpPath, args, std::nullopt, redirects);
    if (rc != 0) {
        PLOGE << "error: objdump --dwarf=decodedline failed";
        (void)sys::fs::remove(tempPath);
        return entries;
    }

    // Read output
    ErrorOr<std::unique_ptr<MemoryBuffer>> bufOrErr = MemoryBuffer::getFile(tempPath);
    if (!bufOrErr) {
        (void)sys::fs::remove(tempPath);
        return entries;
    }

    std::string output = (*bufOrErr)->getBuffer().str();
    (void)sys::fs::remove(tempPath);

    // Parse output
    // Format: filename   line_number   starting_address   view   stmt
    // Example: test.c      5            0xa                1      x
    // Note: Address can be "0" or "0xNNN" depending on objdump version
    // Match any filename (including <corrupt>) followed by line number and address
    std::regex linePattern(R"(^\S+\s+(\d+)\s+(?:0x)?([0-9a-fA-F]+))");
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
        (void)sys::fs::remove(tempPath);
        return functions;
    }

    ErrorOr<std::unique_ptr<MemoryBuffer>> bufOrErr = MemoryBuffer::getFile(tempPath);
    if (!bufOrErr) {
        (void)sys::fs::remove(tempPath);
        return functions;
    }

    std::string output = (*bufOrErr)->getBuffer().str();
    (void)sys::fs::remove(tempPath);

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
            // Estimate instruction end (assume max 4 bytes per instruction)
            uint64_t instrEnd = addr + 4;
            if (instrEnd > maxAddr)
                maxAddr = instrEnd;
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
    PLOGI << "Parsing line table with objdump...";
    std::vector<LineEntry> lineEntries = parseLineTableWithObjdump(objectPath);

    if (lineEntries.empty()) {
        PLOGW << "warning: no line table entries found";
        // Fall back to LLVM parser
        PLOGI << "Attempting LLVM DWARF parser (may have warnings)...";
    }

    // Get function ranges from disassembly
    PLOGI << "Parsing function ranges...";
    std::vector<FuncRange> functions = parseFunctionRanges(objectPath);

    if (functions.empty()) {
        PLOGW << "warning: no functions found";
        return result;
    }

    for (const auto &func : functions) {
        std::string startHex, endHex;
        llvm::raw_string_ostream(startHex) << format_hex(func.start, 6);
        llvm::raw_string_ostream(endHex) << format_hex(func.end, 6);
        PLOGD << "Found function '" << func.name << "' at " << startHex << " - " << endHex;
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
            PLOGW << "warning: function '" << func.name << "' has no line entries";
            continue;
        }

        // Resolve unmapped (line 0) entries using heuristics
        HeuristicLineResolver resolver;
        resolveUnmappedLines(funcLines, func.start, func.end, resolver);

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
        PLOGD << "Parsed function '" << func.name << "' with " << funcMap.bbToAddresses.size()
              << " basic blocks";
    }

    return result;
}

std::map<std::string, FunctionLineMap> DWARFParser::parseLineMap(const std::string &objectPath) {
    std::map<std::string, FunctionLineMap> result;

    // Get line table entries using objdump
    std::vector<LineEntry> lineEntries = parseLineTableWithObjdump(objectPath);

    if (lineEntries.empty()) {
        return result;
    }

    // Get function ranges from disassembly
    std::vector<FuncRange> functions = parseFunctionRanges(objectPath);

    if (functions.empty()) {
        return result;
    }

    // Sort line entries by address
    std::sort(lineEntries.begin(), lineEntries.end(),
              [](const LineEntry &a, const LineEntry &b) { return a.address < b.address; });

    // Assign line entries to functions and resolve
    for (const auto &func : functions) {
        FunctionLineMap funcLineMap;
        funcLineMap.functionName = func.name;

        // Find line entries within this function
        std::vector<LineEntry> funcLines;
        for (const auto &entry : lineEntries) {
            if (entry.address >= func.start && entry.address < func.end) {
                funcLines.push_back(entry);
            }
        }

        if (funcLines.empty()) {
            continue;
        }

        // Resolve unmapped (line 0) entries using heuristics
        HeuristicLineResolver resolver;
        resolveUnmappedLines(funcLines, func.start, func.end, resolver);

        funcLineMap.entries = std::move(funcLines);
        result[func.name] = funcLineMap;
    }

    return result;
}

} // namespace bbanalyzer
