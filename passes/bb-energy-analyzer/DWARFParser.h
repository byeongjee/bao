#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bbanalyzer {

/// Represents a contiguous range of addresses [start, end)
struct AddressRange {
    uint64_t start;
    uint64_t end; // exclusive
};

/// Mapping from BB indices to their address ranges for a single function
struct FunctionBBMap {
    std::string functionName;
    std::map<unsigned, std::vector<AddressRange>> bbToAddresses; // BB index -> address ranges
};

/// Parser for DWARF debug information to extract BB->address mappings.
/// Relies on the AssignBBDebugInfoPass having encoded BB index as line number.
class DWARFParser {
public:
    /// Parse object/ELF file and extract BB->address mappings
    /// @param objectPath Path to .o or .elf file
    /// @return Map of function name -> BB address mappings
    static std::map<std::string, FunctionBBMap> parse(const std::string &objectPath);
};

} // namespace bbanalyzer
