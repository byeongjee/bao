#include "MSP430Disassembler.h"

#include <gtest/gtest.h>

using namespace bbanalyzer;

TEST(MSP430Disassembler, NormalizesMnemonicWidthSuffixes) {
    const auto instructions = MSP430Disassembler::parseObjdumpOutput(
        "   0:\t4c 43       \tclr.b\tr12\t\t;\n"
        "   2:\t4d 4d       \tmov.b\tr13,\tr13\t;\n"
        "   4:\t7e 90 0a 00\tcmp.b\t#10,\tr14\t;#0x000a\n"
        "   8:\t4d 11       \trra.b\tr13\t\t;\n"
        "   a:\t0b 4a       \tmov.w\tr10,\tr11\t;\n"
        "   c:\tf2 40 a5 ff\tmov.b\t#-91,\t&0x0000\t;#0xffa5\n");

    ASSERT_EQ(instructions.size(), 6U);

    EXPECT_EQ(instructions[0].mnemonic, "clr");
    EXPECT_EQ(instructions[0].operands, "r12");
    EXPECT_EQ(instructions[0].addrMode, "register");

    EXPECT_EQ(instructions[1].mnemonic, "mov");
    EXPECT_EQ(instructions[1].operands, "r13,\tr13");
    EXPECT_EQ(instructions[1].addrMode, "register_register");

    EXPECT_EQ(instructions[2].mnemonic, "cmp");
    EXPECT_EQ(instructions[2].operands, "#10,\tr14");
    EXPECT_EQ(instructions[2].addrMode, "immediate_register");

    EXPECT_EQ(instructions[3].mnemonic, "rra");
    EXPECT_EQ(instructions[3].operands, "r13");
    EXPECT_EQ(instructions[3].addrMode, "register");

    EXPECT_EQ(instructions[4].mnemonic, "mov");
    EXPECT_EQ(instructions[4].operands, "r10,\tr11");
    EXPECT_EQ(instructions[4].addrMode, "register_register");

    EXPECT_EQ(instructions[5].mnemonic, "mov");
    EXPECT_EQ(instructions[5].operands, "#-91,\t&0x0000");
    EXPECT_EQ(instructions[5].addrMode, "immediate_absolute");
}

TEST(MSP430Disassembler, PreservesGenuineSymbolicOperands) {
    const auto instructions = MSP430Disassembler::parseObjdumpOutput(
        "   0:\t1d 40 00 00\tmov\t0x0000,\tr13\t;PC rel. 0x0002\n"
        "   4:\t5d 40 00 00\tmov.b\t0x0000,\tr13\t;PC rel. 0x0006\n");

    ASSERT_EQ(instructions.size(), 2U);
    EXPECT_EQ(instructions[0].operands, "0x0000,\tr13");
    EXPECT_EQ(instructions[0].addrMode, "symbolic_register");
    EXPECT_EQ(instructions[1].operands, "0x0000,\tr13");
    EXPECT_EQ(instructions[1].addrMode, "symbolic_register");
}
