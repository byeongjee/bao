#include "DWARFParser.h"
#include "EnergyModel.h"
#include "MSP430Disassembler.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

using namespace llvm;
using namespace bbanalyzer;
using json = nlohmann::json;

// Command line options
static cl::opt<std::string> InputELF(cl::Positional, cl::Required,
                                      cl::desc("<input.elf>"));

static cl::opt<std::string>
    EnergyConfig("energy-config", cl::Required,
                 cl::desc("Path to assembly energy config JSON"));

static cl::opt<std::string> OutputFile("o", cl::init("-"),
                                        cl::desc("Output JSON file (default: stdout)"));

int main(int argc, char **argv) {
    cl::ParseCommandLineOptions(
        argc, argv,
        "BB Energy Analyzer for MSP430\n"
        "Analyzes DWARF debug info and computes per-BB energy costs.\n"
        "\n"
        "This tool parses DWARF debug information from an MSP430 ELF file\n"
        "to extract basic block to address mappings, then disassembles\n"
        "the code and computes energy costs using an assembly-level model.\n");

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
    errs() << "Loading energy model from " << EnergyConfig << "...\n";
    EnergyModel model(EnergyConfig);

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

            if (instrCount == 0) {
                errs() << "warning: BB " << bbIndex << " in '" << funcName
                       << "' has no instructions\n";
            }

            funcOutput["bb_energy"][std::to_string(bbIndex)] = {
                {"energy", bbEnergy}, {"instruction_count", instrCount}};
        }

        output["functions"][funcName] = funcOutput;
    }

    // Check for unmapped instructions
    for (size_t i = 0; i < instructions.size(); ++i) {
        if (!instructionMapped[i]) {
            const auto &insn = instructions[i];
            errs() << "warning: instruction at 0x" << Twine::utohexstr(insn.address)
                   << " (" << insn.mnemonic << ") not mapped to any BB\n";
            unmappedCount++;
            unmappedEnergy += model.getEnergy(insn.mnemonic, insn.addrMode);
        }
    }

    output["unmapped"] = {{"instruction_count", unmappedCount},
                          {"energy", unmappedEnergy}};

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

    errs() << "Done. Analyzed " << bbMaps.size() << " functions, "
           << instructions.size() << " instructions.\n";

    if (unmappedCount > 0) {
        errs() << "Note: " << unmappedCount
               << " instructions not mapped to any basic block.\n";
    }

    return 0;
}
