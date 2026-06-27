#pragma once

namespace checkpoint {

/// Which regime a folded call site falls into, mirroring the reference's
/// CheckpointTypeEnum.DISABLED vs VIRTUAL distinction set by
/// update_function_basic_blocks (schematic.py:115-131).
enum class FoldRegime { Disabled, Virtual };

/// The energy values a solved callee summary folds onto its caller's
/// call_entry / call_exit blocks. Pure arithmetic with no LLVM dependency, so
/// the reference golden (test_function_basic_block_update) can be asserted
/// directly in a unit test. The LLVM plumbing (writing these onto the
/// SchematicSolution / CFGAnalysis) is integration-tested separately.
struct FoldedCallCosts {
    FoldRegime regime;
    double entryCost;     // call_entry block energy (final_cost == cost_all_nvm)
    double exitCost;      // call_exit  block energy (final_cost == cost_all_nvm)
    double entryEToLeave; // call_entry E_to_leave  (VIRTUAL only)
    double entryELeft;    // call_entry E_left       (VIRTUAL only)
    double exitEToLeave;  // call_exit  E_to_leave  (VIRTUAL only)
    double exitELeft;     // call_exit  E_left       (VIRTUAL only)
};

/// Faithful port of update_function_basic_blocks (schematic.py:95-131).
///   c0   = call_entry base energy (the CALL instruction's own cost)
///   cx0  = call_exit  base energy (empty block ⇒ ~0)
///   sf*  = callee first-block (START) energy_to_leave / energy_left
///   ef*  = callee last-block  (END)   energy_to_leave / energy_left
///   capacity = energy budget (E_buf)
///   checkpointInFunction = callee has a non-DISABLED checkpoint (⇒ VIRTUAL)
///                          vs. checkpoint-free (⇒ DISABLED)
FoldedCallCosts computeFoldedCallCosts(double c0, double cx0, double sfEToLeave, double sfELeft,
                                       double efEToLeave, double efELeft, double capacity,
                                       bool checkpointInFunction);

} // namespace checkpoint
