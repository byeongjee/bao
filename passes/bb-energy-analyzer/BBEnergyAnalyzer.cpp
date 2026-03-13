#include "DWARFParser.h"
#include "EnergyModel.h"
#include "MSP430Disassembler.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

using namespace llvm;
using namespace bbanalyzer;
using json = nlohmann::json;

// Load BB mapping from JSON file (function -> BB index -> BB name)
std::map<std::string, std::map<std::string, std::string>> loadBBMapping(const std::string &path) {
    std::map<std::string, std::map<std::string, std::string>> result;
    std::ifstream file(path);
    if (!file.is_open()) {
        errs() << "error: cannot open BB mapping file: " << path << "\n";
        return result;
    }
    json mapping = json::parse(file, nullptr, false);
    if (mapping.is_discarded()) {
        errs() << "error: JSON parse error in BB mapping file: " << path << "\n";
        return result;
    }
    for (auto &[funcName, funcMapping] : mapping.items()) {
        for (auto &[idx, bbName] : funcMapping.items()) {
            result[funcName][idx] = bbName.get<std::string>();
        }
    }
    return result;
}

// Command line options
static cl::opt<std::string> InputELF(cl::Positional, cl::Required, cl::desc("<input.elf>"));

static cl::opt<std::string> EnergyParams("energy-params",
                                         cl::desc("Path to assembly energy parameters JSON"));

static cl::opt<std::string>
    BBMappingFile("bb-mapping",
                  cl::desc("Path to BB index-to-name mapping JSON (from bb-debuginfo pass)"));

static cl::opt<std::string> OutputFile("o", cl::init("-"),
                                       cl::desc("Output JSON file (default: stdout)"));

static cl::opt<bool> DumpLineMap("dump-line-map", cl::init(false),
                                 cl::desc("Dump resolved address->BB line map and exit"));

int main(int argc, char **argv) {
    cl::ParseCommandLineOptions(
        argc, argv,
        "BB Energy Analyzer for MSP430\n"
        "Analyzes DWARF debug info and computes per-BB energy costs.\n"
        "\n"
        "This tool parses DWARF debug information from an MSP430 ELF file\n"
        "to extract basic block to address mappings, then disassembles\n"
        "the code and computes energy costs using an assembly-level model.\n"
        "\n"
        "Use --dump-line-map to output resolved address->BB mappings.\n");

    // Handle --dump-line-map mode
    if (DumpLineMap) {
        auto lineMaps = DWARFParser::parseLineMap(InputELF);
        if (lineMaps.empty()) {
            errs() << "error: no line mappings found\n";
            return 1;
        }

        // Output format: address BB (hex address, decimal BB)
        // Sorted by address within each function
        for (const auto &[funcName, funcLineMap] : lineMaps) {
            for (const auto &entry : funcLineMap.entries) {
                // Output: 8-digit hex address, space, BB index
                outs() << format("%08x", entry.address) << " " << entry.line << "\n";
            }
        }
        return 0;
    }

    // Energy analysis mode requires config and BB mapping
    if (EnergyParams.empty()) {
        errs() << "error: --energy-params is required for energy analysis\n";
        errs() << "Use --dump-line-map for line map output without energy params\n";
        return 1;
    }

    if (BBMappingFile.empty()) {
        errs() << "error: --bb-mapping is required for energy analysis\n";
        return 1;
    }

    // 1. Parse DWARF to get BB->address mappings
    errs() << "Parsing DWARF from " << InputELF << "...\n";
    auto bbMaps = DWARFParser::parse(InputELF);

    if (bbMaps.empty()) {
        errs() << "error: no basic block mappings found in '" << InputELF << "'\n";
        errs() << "Make sure the file was compiled with debug info (-g) and\n";
        errs() << "processed by the assign-bb-debuginfo pass.\n";
        return 1;
    }

    // 2. Disassemble MSP430 code
    errs() << "Disassembling MSP430 code...\n";
    MSP430Disassembler disasm;
    auto instructions = disasm.disassemble(InputELF);

    if (instructions.empty()) {
        errs() << "error: no instructions found in '" << InputELF << "'\n";
        return 1;
    }

    // 3. Load energy model
    errs() << "Loading energy model from " << EnergyParams << "...\n";
    EnergyModel model(EnergyParams);

    // 3.5. Load BB mapping
    errs() << "Loading BB mapping from " << BBMappingFile << "...\n";
    auto bbMapping = loadBBMapping(BBMappingFile);
    if (bbMapping.empty()) {
        return 1; // Error already printed
    }

    // 4. Compute per-BB energy
    json output;
    output["source_file"] = InputELF.getValue();
    output["target"] = "msp430fr5994";
    output["functions"] = json::object();

    unsigned unmappedCount = 0;
    double unmappedEnergy = 0.0;

    // Track which instructions are mapped
    std::vector<bool> instructionMapped(instructions.size(), false);

    for (const auto &[funcName, funcMap] : bbMaps) {
        json funcOutput;
        funcOutput["bb_count"] = funcMap.bbToAddresses.size();
        funcOutput["bb_energy"] = json::object();

        for (const auto &[bbIndex, ranges] : funcMap.bbToAddresses) {
            double bbEnergy = 0.0;
            int instrCount = 0;

            for (const auto &range : ranges) {
                for (size_t i = 0; i < instructions.size(); ++i) {
                    const auto &insn = instructions[i];
                    if (insn.address >= range.start && insn.address < range.end) {
                        bbEnergy += model.getEnergy(insn.mnemonic, insn.addrMode);
                        instrCount++;
                        instructionMapped[i] = true;
                    }
                }
            }

            // Lookup BB name from mapping
            std::string bbName;
            auto funcIt = bbMapping.find(funcName);
            if (funcIt != bbMapping.end()) {
                auto bbIt = funcIt->second.find(std::to_string(bbIndex));
                if (bbIt != funcIt->second.end()) {
                    bbName = bbIt->second;
                } else {
                    bbName = "bb" + std::to_string(bbIndex - 1); // fallback
                    errs() << "warning: BB " << bbIndex << " not found in mapping for '" << funcName
                           << "'\n";
                }
            } else {
                bbName = "bb" + std::to_string(bbIndex - 1); // fallback
                errs() << "warning: function '" << funcName << "' not found in BB mapping\n";
            }

            if (instrCount == 0) {
                errs() << "warning: BB '" << bbName << "' in '" << funcName
                       << "' has no instructions\n";
            }

            funcOutput["bb_energy"][bbName] = {{"energy", bbEnergy},
                                               {"instruction_count", instrCount}};
        }

        output["functions"][funcName] = funcOutput;
    }

    // Add zero-energy entries for BBs in the mapping but not found in DWARF.
    // These are structural BBs (preheaders, loop exits) that produce no machine
    // instructions after codegen — their energy is correctly zero.
    for (const auto &[funcName, funcBBs] : bbMapping) {
        if (!output["functions"].contains(funcName)) {
            output["functions"][funcName] = {{"bb_count", 0}, {"bb_energy", json::object()}};
        }
        auto &funcEntry = output["functions"][funcName];
        for (const auto &[bbIdx, bbName] : funcBBs) {
            if (!funcEntry["bb_energy"].contains(bbName)) {
                funcEntry["bb_energy"][bbName] = {{"energy", 0.0}, {"instruction_count", 0}};
            }
        }
        funcEntry["bb_count"] = funcEntry["bb_energy"].size();
    }

    // Check for unmapped instructions
    for (size_t i = 0; i < instructions.size(); ++i) {
        if (!instructionMapped[i]) {
            const auto &insn = instructions[i];
            errs() << "warning: instruction at 0x" << Twine::utohexstr(insn.address) << " ("
                   << insn.mnemonic << ") not mapped to any BB\n";
            unmappedCount++;
            unmappedEnergy += model.getEnergy(insn.mnemonic, insn.addrMode);
        }
    }

    output["unmapped"] = {{"instruction_count", unmappedCount}, {"energy", unmappedEnergy}};

    // Add required/missing energy parameter keys
    json requiredParams = json::array();
    for (const auto &key : model.getRequiredKeys()) {
        requiredParams.push_back(key);
    }
    output["required_parameters"] = requiredParams;

    json missingParams = json::array();
    for (const auto &key : model.getMissingKeys()) {
        missingParams.push_back(key);
    }
    output["missing_parameters"] = missingParams;

    // Print summary to stderr (flows into pass_output for bench runners)
    errs() << "\n--- Energy parameters ---\n";
    errs() << "  Required (" << model.getRequiredKeys().size() << " keys):";
    {
        bool first = true;
        for (const auto &key : model.getRequiredKeys()) {
            errs() << (first ? " " : ", ") << key;
            first = false;
        }
    }
    errs() << "\n";
    errs() << "  Missing  (" << model.getMissingKeys().size() << " keys):";
    {
        bool first = true;
        for (const auto &key : model.getMissingKeys()) {
            errs() << (first ? " " : ", ") << key;
            first = false;
        }
    }
    errs() << "\n";

    // 5. Write output
    std::ostream *out = &std::cout;
    std::ofstream outFile;
    if (OutputFile != "-") {
        outFile.open(OutputFile);
        if (!outFile.is_open()) {
            errs() << "error: failed to open output file '" << OutputFile << "'\n";
            return 1;
        }
        out = &outFile;
    }

    *out << output.dump(2) << "\n";

    errs() << "Done. Analyzed " << bbMaps.size() << " functions, " << instructions.size()
           << " instructions.\n";

    if (unmappedCount > 0) {
        errs() << "Note: " << unmappedCount << " instructions not mapped to any basic block.\n";
    }

    return 0;
}
