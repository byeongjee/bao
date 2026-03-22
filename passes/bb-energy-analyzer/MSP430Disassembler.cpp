#include "MSP430Disassembler.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include "common/Logger.h"

#include <cstdlib>
#include <regex>
#include <sstream>

using namespace llvm;

namespace bbanalyzer {

MSP430Disassembler::MSP430Disassembler() = default;
MSP430Disassembler::~MSP430Disassembler() = default;

std::vector<Instruction> MSP430Disassembler::disassemble(const std::string &elfPath) {
    std::vector<Instruction> result;

    // Find msp430-elf-objdump in common locations
    std::vector<std::string> searchPaths = {
        // Environment variable takes precedence
        std::string(std::getenv("MSP430_GCC_DIR") ? std::getenv("MSP430_GCC_DIR") : "") +
            "/bin/msp430-elf-objdump",
        // Common TI installation paths
        "/Users/byeongjee/ti/msp430-gcc/bin/msp430-elf-objdump",
        "/opt/ti/msp430-gcc/bin/msp430-elf-objdump",
        "/usr/local/bin/msp430-elf-objdump",
        "msp430-elf-objdump", // In PATH
    };

    std::string objdumpPath;
    for (const auto &path : searchPaths) {
        if (path.empty() || path[0] == '/') {
            // Absolute path - check if exists
            if (!path.empty() && sys::fs::exists(path)) {
                objdumpPath = path;
                break;
            }
        } else {
            // Search in PATH
            ErrorOr<std::string> found = sys::findProgramByName(path);
            if (found) {
                objdumpPath = *found;
                break;
            }
        }
    }

    if (objdumpPath.empty()) {
        PLOGE << "error: msp430-elf-objdump not found. Set MSP430_GCC_DIR environment variable.";
        return result;
    }

    PLOGI << "Using objdump: " << objdumpPath;

    // Create temporary file for objdump output
    SmallString<128> tempPath;
    std::error_code ec = sys::fs::createTemporaryFile("objdump", "txt", tempPath);
    if (ec) {
        PLOGE << "error: failed to create temp file: " << ec.message();
        return result;
    }

    // Run objdump -d -r <elfPath>
    StringRef args[] = {objdumpPath, "-d", "-r", elfPath};
    std::optional<StringRef> redirects[] = {std::nullopt, StringRef(tempPath), std::nullopt};

    int rc = sys::ExecuteAndWait(objdumpPath, args, std::nullopt, redirects);
    if (rc != 0) {
        PLOGE << "error: objdump failed with exit code " << rc;
        sys::fs::remove(tempPath);
        return result;
    }

    // Read objdump output
    ErrorOr<std::unique_ptr<MemoryBuffer>> bufOrErr = MemoryBuffer::getFile(tempPath);
    if (!bufOrErr) {
        PLOGE << "error: failed to read objdump output: " << bufOrErr.getError().message();
        sys::fs::remove(tempPath);
        return result;
    }

    std::string output = (*bufOrErr)->getBuffer().str();
    sys::fs::remove(tempPath);

    // Parse objdump output
    // Format:
    //    1234:       0f 4c           mov     r12, r15
    //    1236:       12 c3           clrc
    // Pattern: hex_addr: hex_bytes mnemonic operands
    std::regex instrPattern(R"(^\s*([0-9a-fA-F]+):\s+([0-9a-fA-F ]+)\s+(\w+)(.*)$)");
    std::regex relocPattern(R"(^\s*([0-9a-fA-F]+):\s+R_\S+\s+(\S+)\s*$)");

    std::istringstream stream(output);
    std::string line;

    while (std::getline(stream, line)) {
        // Try relocation line first
        std::smatch relocMatch;
        if (!result.empty() && std::regex_match(line, relocMatch, relocPattern)) {
            auto &lastInstr = result.back();
            if (lastInstr.mnemonic == "call") {
                lastInstr.callTarget = relocMatch[2].str();
            }
            continue;
        }

        std::smatch match;
        if (std::regex_match(line, match, instrPattern)) {
            Instruction instr;

            // Parse address
            instr.address = std::stoull(match[1].str(), nullptr, 16);

            // Parse byte count from hex bytes string
            std::string hexBytes = match[2].str();
            // Count hex byte pairs (each pair is one byte)
            size_t byteCount = 0;
            for (size_t i = 0; i < hexBytes.size(); ++i) {
                if (std::isxdigit(hexBytes[i])) {
                    byteCount++;
                }
            }
            instr.size = byteCount / 2;

            // Parse mnemonic (convert to lowercase)
            instr.mnemonic = match[3].str();
            std::transform(instr.mnemonic.begin(), instr.mnemonic.end(), instr.mnemonic.begin(),
                           [](unsigned char c) { return std::tolower(c); });

            // Skip padding/data bytes that objdump displays as hex values
            // (e.g., "00", "ff", etc. are not valid MSP430 mnemonics)
            bool isHexOnly =
                !instr.mnemonic.empty() && std::all_of(instr.mnemonic.begin(), instr.mnemonic.end(),
                                                       [](char c) { return std::isxdigit(c); });
            if (isHexOnly) {
                continue;
            }

            // Parse operands (strip trailing ';' comment, then trim whitespace)
            instr.operands = match[4].str();
            auto semi = instr.operands.find(';');
            if (semi != std::string::npos)
                instr.operands.erase(semi);
            if (!instr.operands.empty()) {
                instr.operands.erase(0, instr.operands.find_first_not_of(" \t"));
                if (!instr.operands.empty()) {
                    instr.operands.erase(instr.operands.find_last_not_of(" \t") + 1);
                }
            }

            // Determine addressing mode
            instr.addrMode = determineAddressingMode(instr.mnemonic, instr.operands);

            result.push_back(instr);
        }
    }

    // Resolve section-relative call targets (.text+0xNN) to function names
    auto funcLabels = parseFunctionLabels(output);
    PLOGI << "Found " << funcLabels.size() << " function labels for relocation resolution";
    resolveCallTargets(result, funcLabels);

    PLOGI << "Disassembled " << result.size() << " instructions";
    return result;
}

std::map<uint64_t, std::string>
MSP430Disassembler::parseFunctionLabels(const std::string &objdumpOutput) {
    std::map<uint64_t, std::string> result;
    std::regex labelPattern(R"(^([0-9a-fA-F]+)\s+<([^>]+)>:\s*$)");
    std::istringstream stream(objdumpOutput);
    std::string line;
    std::smatch match;

    while (std::getline(stream, line)) {
        if (std::regex_match(line, match, labelPattern)) {
            uint64_t offset = std::stoull(match[1].str(), nullptr, 16);
            result[offset] = match[2].str();
        }
    }
    return result;
}

void MSP430Disassembler::resolveCallTargets(std::vector<Instruction> &instructions,
                                            const std::map<uint64_t, std::string> &offsetToFunc) {
    if (offsetToFunc.empty()) {
        return;
    }

    for (auto &instr : instructions) {
        if (instr.mnemonic != "call" || instr.callTarget.rfind(".text", 0) != 0) {
            continue;
        }

        // Parse offset from .text+0xNN or bare .text (offset 0)
        uint64_t offset = 0;
        auto plusPos = instr.callTarget.find('+');
        if (plusPos != std::string::npos) {
            offset = std::stoull(instr.callTarget.substr(plusPos + 1), nullptr, 0);
        }

        // Find containing function (largest label address <= offset)
        auto it = offsetToFunc.upper_bound(offset);
        if (it != offsetToFunc.begin()) {
            --it;
            instr.callTarget = it->second;
        }
    }
}

std::string MSP430Disassembler::determineAddressingMode(const std::string &mnemonic,
                                                        const std::string &operands) {
    if (operands.empty()) {
        // Instructions like "ret", "reti" have no operands
        return "";
    }

    // Split operands by comma
    std::vector<std::string> ops;
    std::string current;
    int parenDepth = 0;

    for (char c : operands) {
        if (c == '(') {
            parenDepth++;
            current += c;
        } else if (c == ')') {
            parenDepth--;
            current += c;
        } else if (c == ',' && parenDepth == 0) {
            // Trim and add
            current.erase(0, current.find_first_not_of(" \t"));
            if (!current.empty()) {
                current.erase(current.find_last_not_of(" \t") + 1);
                ops.push_back(current);
            }
            current.clear();
        } else {
            current += c;
        }
    }
    // Add last operand
    current.erase(0, current.find_first_not_of(" \t"));
    if (!current.empty()) {
        current.erase(current.find_last_not_of(" \t") + 1);
        ops.push_back(current);
    }

    // Build addressing mode string
    std::string mode;
    for (size_t i = 0; i < ops.size(); ++i) {
        if (i > 0) {
            mode += "_";
        }
        mode += parseOperandMode(ops[i]);
    }

    return mode;
}

std::string MSP430Disassembler::parseOperandMode(const std::string &operand) {
    if (operand.empty()) {
        return "unknown";
    }

    // Check patterns in order of specificity

    // Immediate: #N or #0xN
    if (operand[0] == '#') {
        return "immediate";
    }

    // Autoincrement indirect: @Rn+
    if (operand[0] == '@' && operand.back() == '+') {
        return "autoincrement";
    }

    // Indirect: @Rn
    if (operand[0] == '@') {
        return "indirect";
    }

    // Absolute: &ADDR
    if (operand[0] == '&') {
        return "absolute";
    }

    // Indexed: X(Rn) or offset(Rn)
    // Pattern: something followed by (Rn) or (rn)
    std::regex indexedPattern(R"(.+\([rR]\d+\))");
    if (std::regex_match(operand, indexedPattern)) {
        return "indexed";
    }

    // Register: Rn, rN, SP, SR, PC, etc.
    std::regex registerPattern(R"([rR]\d+|[sS][pPrR]|[pP][cC])");
    if (std::regex_match(operand, registerPattern)) {
        return "register";
    }

    // Symbolic: label or address (anything else - memory reference)
    // This includes direct addresses like 0x1234
    return "symbolic";
}

} // namespace bbanalyzer
