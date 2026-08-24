#include "MSP430Disassembler.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include "common/Logger.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <regex>
#include <sstream>
#include <utility>

using namespace llvm;

namespace bbanalyzer {

namespace {

/// Bytes objdump prints per byte column before wrapping onto the next line.
constexpr unsigned WrapWidth = 4;

/// Count whole bytes in an objdump byte column such as "82 5a 00 00 ".
unsigned countBytes(const std::string &hexBytes) {
    unsigned digits = 0;
    for (unsigned char c : hexBytes) {
        if (std::isxdigit(c))
            digits++;
    }
    return digits / 2;
}

/// Find the instruction whose bytes contain `offset`, or nullptr if none does.
/// The search runs backwards because objdump prints a relocation right after
/// the instruction it patches, and because every section of an object file
/// restarts addresses at zero: the nearest preceding match is the one in the
/// section currently being printed.
Instruction *findInstructionCovering(std::vector<Instruction> &instructions, uint64_t offset) {
    for (auto it = instructions.rbegin(); it != instructions.rend(); ++it) {
        if (it->address <= offset)
            return offset < it->address + it->size ? &*it : nullptr;
    }
    return nullptr;
}

} // namespace

MSP430Disassembler::MSP430Disassembler() = default;
MSP430Disassembler::~MSP430Disassembler() = default;

std::vector<Instruction> MSP430Disassembler::disassemble(const std::string &elfPath) {
    std::vector<Instruction> result;

    // MSP430_GCC_DIR pins the toolchain; otherwise fall back to PATH.
    std::string objdumpPath;
    if (const char *gccDir = std::getenv("MSP430_GCC_DIR"); gccDir && *gccDir) {
        SmallString<128> candidate(gccDir);
        sys::path::append(candidate, "bin", "msp430-elf-objdump");
        if (sys::fs::exists(candidate))
            objdumpPath = std::string(candidate);
    }
    if (objdumpPath.empty()) {
        ErrorOr<std::string> found = sys::findProgramByName("msp430-elf-objdump");
        if (found)
            objdumpPath = *found;
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
        (void)sys::fs::remove(tempPath);
        return result;
    }

    // Read objdump output
    ErrorOr<std::unique_ptr<MemoryBuffer>> bufOrErr = MemoryBuffer::getFile(tempPath);
    if (!bufOrErr) {
        PLOGE << "error: failed to read objdump output: " << bufOrErr.getError().message();
        (void)sys::fs::remove(tempPath);
        return result;
    }

    std::string output = (*bufOrErr)->getBuffer().str();
    (void)sys::fs::remove(tempPath);

    result = parseObjdumpOutput(output);

    // Resolve section-relative call targets (.text+0xNN) to function names
    auto funcLabels = parseFunctionLabels(output);
    PLOGI << "Found " << funcLabels.size() << " function labels for relocation resolution";
    resolveCallTargets(result, funcLabels);

    PLOGI << "Disassembled " << result.size() << " instructions";
    return result;
}

std::vector<Instruction> MSP430Disassembler::parseObjdumpOutput(const std::string &objdumpOutput) {
    std::vector<Instruction> result;

    // objdump writes tab-delimited fields:
    //    "   0:\t0f 4c       \tmov\tr12,\tr15\t;"
    //    "   2:\t4c 43       \tclr.b\tr12\t\t;"
    //     ^address ^byte column ^instruction text (contains further tabs)
    // The fields are split on tabs instead of by one regex over the whole line
    // because a regex cannot tell where the byte column ends: mnemonics spelled
    // with hex digits only (add, addc, adc, dadd, dec, decd) are also valid byte
    // columns, so the greedy match swallowed them and dropped the instruction.
    //
    // Byte listings wrap after four bytes, putting the tail of a longer
    // instruction on a line that carries an address and bytes but no text:
    //    "   4:\t80 18 5c 4a \tmovx.a\t74565(r10),r12\t;0x12345"
    //    "   8:\t45 23 "
    // Those bytes belong to the preceding instruction's size.
    std::regex addrPattern(R"(^\s*([0-9a-fA-F]+):$)");
    std::regex bytesPattern(R"(^[0-9a-fA-F ]*$)");
    // Capture the optional width suffix separately so it cannot become part
    // of the first operand and be misclassified as symbolic addressing.
    std::regex mnemonicPattern(R"(^(\w+)(?:\.(\w+))?(.*)$)");
    std::regex relocPattern(R"(^\s*([0-9a-fA-F]+):\s+R_\S+\s+(\S+)\s*$)");

    std::istringstream stream(objdumpOutput);
    std::string line;

    // Bytes on the previous line, to tell a wrapped tail from undecoded data
    // that happens to start where the previous instruction ended: objdump wraps
    // only after a full column, so a shorter column cannot be continued.
    unsigned prevColumnBytes = 0;

    while (std::getline(stream, line)) {
        std::smatch match;

        // Try relocation line first. Its offset points at the operand word the
        // linker patches, which is inside the instruction rather than at its
        // first byte: "16: R_MSP430X_ABS16 g" belongs to the instruction at 14.
        if (std::regex_match(line, match, relocPattern)) {
            uint64_t offset = std::stoull(match[1].str(), nullptr, 16);
            Instruction *target = findInstructionCovering(result, offset);
            if (target && target->mnemonic == "call") {
                target->callTarget = match[2].str();
            }
            continue;
        }

        unsigned columnBytes = std::exchange(prevColumnBytes, 0);

        auto addrEnd = line.find('\t');
        if (addrEnd == std::string::npos)
            continue;
        std::string addrField = line.substr(0, addrEnd);
        if (!std::regex_match(addrField, match, addrPattern))
            continue;
        uint64_t address = std::stoull(match[1].str(), nullptr, 16);

        std::string rest = line.substr(addrEnd + 1);
        auto bytesEnd = rest.find('\t');
        std::string bytesField = rest.substr(0, bytesEnd);
        if (!std::regex_match(bytesField, bytesPattern))
            continue;
        unsigned byteCount = countBytes(bytesField);
        prevColumnBytes = byteCount;

        if (bytesEnd == std::string::npos) {
            // Continuation of the previous instruction's wrapped byte listing.
            // Anything else on a bytes-only line is data objdump could not
            // decode, and belongs to no instruction.
            bool continues = columnBytes == WrapWidth && !result.empty() &&
                             result.back().address + result.back().size == address;
            if (continues)
                result.back().size += byteCount;
            continue;
        }

        std::string text = rest.substr(bytesEnd + 1);
        if (!std::regex_match(text, match, mnemonicPattern))
            continue;

        Instruction instr;
        instr.address = address;
        instr.size = byteCount;

        // Parse mnemonic (convert to lowercase)
        instr.mnemonic = match[1].str();
        std::transform(instr.mnemonic.begin(), instr.mnemonic.end(), instr.mnemonic.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        // Parse operands (strip trailing ';' comment, then trim whitespace)
        instr.operands = match[3].str();
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

unsigned MSP430Disassembler::countStackAccesses(const Instruction &instr, bool fpIsR4) {
    const std::string &m = instr.mnemonic;
    if (m == "pushm" || m == "popm") {
        static const std::regex countPattern(R"(#(\d+))");
        std::smatch match;
        if (std::regex_search(instr.operands, match, countPattern))
            return static_cast<unsigned>(std::stoul(match[1]));
        return 1;
    }

    static const std::regex spOrFpOperand(R"(@[rR][14]\+?|.*\([rR][14]\))");
    static const std::regex spOperand(R"(@[rR]1\+?|.*\([rR]1\))");
    const std::regex &pattern = fpIsR4 ? spOrFpOperand : spOperand;

    unsigned count = 0;
    std::string current;
    auto flush = [&]() {
        current.erase(0, current.find_first_not_of(" \t"));
        if (!current.empty()) {
            current.erase(current.find_last_not_of(" \t") + 1);
            if (std::regex_match(current, pattern))
                count++;
        }
        current.clear();
    };
    for (char ch : instr.operands) {
        if (ch == ',')
            flush();
        else
            current += ch;
    }
    flush();

    // These push or pop one word on top of whatever their operand addresses,
    // so a stack-relative operand costs an access of its own.
    if (m == "push" || m == "pop" || m == "call" || m == "calla" || m == "ret" || m == "reta" ||
        m == "reti")
        return count + 1;
    return count;
}

} // namespace bbanalyzer
