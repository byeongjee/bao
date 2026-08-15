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

// add/addc/adc/dadd/dec/decd are spelled with hex digits only, so a parser that
// guesses where the byte column ends drops every one of them.
TEST(MSP430Disassembler, KeepsMnemonicsSpelledWithHexDigitsOnly) {
    const auto instructions =
        MSP430Disassembler::parseObjdumpOutput("   a:\t0d 5c       \tadd\tr12,\tr13\t;\n"
                                               "   c:\t0d 6c       \taddc\tr12,\tr13\t;\n"
                                               "   e:\t1d 63       \tadc\tr13\t\t;\n"
                                               "  10:\t0d ac       \tdadd\tr12,\tr13\t;\n"
                                               "  12:\t3d 53       \tdec\tr13\t\t;\n"
                                               "  14:\t2d 53       \tdecd\tr13\t\t;\n");

    ASSERT_EQ(instructions.size(), 6U);
    EXPECT_EQ(instructions[0].mnemonic, "add");
    EXPECT_EQ(instructions[0].addrMode, "register_register");
    EXPECT_EQ(instructions[1].mnemonic, "addc");
    EXPECT_EQ(instructions[2].mnemonic, "adc");
    EXPECT_EQ(instructions[2].addrMode, "register");
    EXPECT_EQ(instructions[3].mnemonic, "dadd");
    EXPECT_EQ(instructions[4].mnemonic, "dec");
    EXPECT_EQ(instructions[5].mnemonic, "decd");
}

// objdump wraps byte listings after four bytes; the tail line carries an
// address and bytes but no instruction text.
TEST(MSP430Disassembler, WrappedByteListingExtendsInstructionSize) {
    const auto instructions = MSP430Disassembler::parseObjdumpOutput(
        "   0:\t8c 01 45 23 \tmova\t#74565,\tr12\t;0x12345\n"
        "   4:\t80 18 5c 4a \tmovx.a\t74565(r10),r12\t;0x12345\n"
        "   8:\t45 23 \n"
        "   a:\t82 18 fa 50 \taddx.a\t#74565,\t144470(r10);0x12345, 0x23456\n"
        "   e:\t45 23 56 34 \n"
        "  12:\t\n");

    ASSERT_EQ(instructions.size(), 3U);
    EXPECT_EQ(instructions[0].size, 4U);
    EXPECT_EQ(instructions[1].mnemonic, "movx");
    EXPECT_EQ(instructions[1].size, 6U);
    EXPECT_EQ(instructions[2].mnemonic, "addx");
    EXPECT_EQ(instructions[2].size, 8U);
}

// Trailing data bytes that objdump cannot decode are not instructions, and must
// not extend an instruction they do not continue.
TEST(MSP430Disassembler, IgnoresDataBytesThatFollowNoInstruction) {
    const auto instructions =
        MSP430Disassembler::parseObjdumpOutput("   0:\t30 41       \tret\t\t\t\n"
                                               "   4:\t11 34 \n"
                                               "   6:\t\n");

    ASSERT_EQ(instructions.size(), 1U);
    EXPECT_EQ(instructions[0].mnemonic, "ret");
    EXPECT_EQ(instructions[0].size, 2U);
}

// Relocation offsets point at the patched operand word, not at the start of the
// instruction, and only calls carry a call target: a .rodata relocation that
// follows a mov must not be attributed to the last call seen.
TEST(MSP430Disassembler, AttachesRelocationToTheInstructionItPatches) {
    const auto instructions = MSP430Disassembler::parseObjdumpOutput(
        "   0:\t0a 15       \tpushm\t#1,\tr10\t;16-bit words\n"
        "   2:\t0a 4c       \tmov\tr12,\tr10\t;\n"
        "   4:\tb0 12 00 00 \tcall\t#0\t\t;\n"
        "\t\t\t6: R_MSP430X_ABS16\text\n"
        "   8:\t1c 42 00 00 \tmov\t&0x0000,r12\t;0x0000\n"
        "\t\t\ta: R_MSP430X_ABS16\tg\n"
        "   c:\t0c 5a       \tadd\tr10,\tr12\t;\n"
        "   e:\t82 4c 00 00 \tmov\tr12,\t&0x0000\t;\n"
        "\t\t\t10: R_MSP430X_ABS16\tg\n");

    ASSERT_EQ(instructions.size(), 6U);
    EXPECT_EQ(instructions[2].mnemonic, "call");
    EXPECT_EQ(instructions[2].callTarget, "ext");
    for (size_t i = 0; i < instructions.size(); ++i) {
        if (i != 2)
            EXPECT_EQ(instructions[i].callTarget, "") << "instruction " << i;
    }
}

// A relocation for an instruction whose byte listing wrapped is printed after
// the continuation line, so the covering search must span the full instruction.
TEST(MSP430Disassembler, AttachesRelocationPrintedAfterAContinuationLine) {
    const auto instructions =
        MSP430Disassembler::parseObjdumpOutput("   0:\tb0 12 00 00 \tcall\t#0\t\t;\n"
                                               "\t\t\t2: R_MSP430X_ABS16\text2\n"
                                               "   4:\t00 18 fa 40 \tmovx.a\t#0,\t4(r10)\t;\n"
                                               "   8:\t00 00 04 00 \n"
                                               "\t\t\t8: R_MSP430X_ABS20_EXT_SRC\tsym\n");

    ASSERT_EQ(instructions.size(), 2U);
    EXPECT_EQ(instructions[0].callTarget, "ext2");
    EXPECT_EQ(instructions[1].size, 8U);
    EXPECT_EQ(instructions[1].callTarget, "");
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
