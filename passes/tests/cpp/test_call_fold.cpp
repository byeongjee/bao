#include "schematic/CallFold.h"

#include <gtest/gtest.h>

using namespace checkpoint;

// Golden values ported verbatim from the reference unit test
// ScEpTIC tests/unit_tests/my_transformation/test_function_isolation.py
//   ::test_function_basic_block_update
//
// Inputs (energy_budget=50):
//   call_entry base energy c0 = 4   (bb.cost_all_nvm = 4)
//   call_exit  base energy cx0 = 0  (empty block)
//   callee first bb (START): energy_left = 10, energy_to_leave = 8
//   callee last  bb (END):   energy_left = 4,  energy_to_leave = 2
//
// These pin the folding formula independent of the energy model.

namespace {
constexpr double kC0 = 4.0;
constexpr double kCx0 = 0.0;
constexpr double kSfEToLeave = 8.0;
constexpr double kSfELeft = 10.0;
constexpr double kEfEToLeave = 2.0;
constexpr double kEfELeft = 4.0;
constexpr double kCapacity = 50.0;
} // namespace

// Reference: schematic.py:127-131 (checkpoint_in_function == False).
TEST(CallFold, DisabledRegime_CheckpointFreeCallee) {
    FoldedCallCosts r =
        computeFoldedCallCosts(kC0, kCx0, kSfEToLeave, kSfELeft, kEfEToLeave, kEfELeft, kCapacity,
                               /*checkpointInFunction=*/false);
    EXPECT_EQ(r.regime, FoldRegime::Disabled);
    EXPECT_DOUBLE_EQ(r.entryCost, 10.0); // c0 + sfEToLeave - efEToLeave = 4 + 8 - 2
    EXPECT_DOUBLE_EQ(r.exitCost, 0.0);   // cx0
}

// Reference: schematic.py:115-126 (checkpoint_in_function == True).
TEST(CallFold, VirtualRegime_CalleeHasCheckpoint) {
    FoldedCallCosts r =
        computeFoldedCallCosts(kC0, kCx0, kSfEToLeave, kSfELeft, kEfEToLeave, kEfELeft, kCapacity,
                               /*checkpointInFunction=*/true);
    EXPECT_EQ(r.regime, FoldRegime::Virtual);
    EXPECT_DOUBLE_EQ(r.entryCost, 12.0);     // sfEToLeave + c0 = 8 + 4
    EXPECT_DOUBLE_EQ(r.entryEToLeave, 12.0); // sfEToLeave + c0
    EXPECT_DOUBLE_EQ(r.entryELeft, 10.0);    // sfELeft
    EXPECT_DOUBLE_EQ(r.exitCost, 46.0);      // capacity - efELeft + cx0 = 50 - 4 + 0
    EXPECT_DOUBLE_EQ(r.exitEToLeave, 2.0);   // efEToLeave
    EXPECT_DOUBLE_EQ(r.exitELeft, 4.0);      // efELeft
}
