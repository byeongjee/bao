#include "schematic/CallFold.h"

namespace checkpoint {

// Faithful port of update_function_basic_blocks (schematic.py:95-131).
FoldedCallCosts computeFoldedCallCosts(double c0, double cx0, double sfEToLeave, double sfELeft,
                                       double efEToLeave, double efELeft, double capacity,
                                       bool checkpointInFunction) {
    FoldedCallCosts r{};
    if (checkpointInFunction) {
        // VIRTUAL regime (schematic.py:115-126): the call is a region boundary.
        r.regime = FoldRegime::Virtual;
        // f_entry.final_cost = f_start.energy_to_leave + f_entry.cost_all_nvm
        r.entryCost = sfEToLeave + c0;
        r.entryEToLeave = sfEToLeave + c0; // f_entry.energy_to_leave (same value)
        r.entryELeft = sfELeft;            // f_entry.energy_left = f_start.energy_left
        // f_exit.final_cost = energy_budget - f_end.energy_left + f_exit.cost_all_nvm
        r.exitCost = capacity - efELeft + cx0;
        r.exitEToLeave = efEToLeave; // f_exit.energy_to_leave = f_end.energy_to_leave
        r.exitELeft = efELeft;       // f_exit.energy_left = f_end.energy_left
    } else {
        // DISABLED regime (schematic.py:127-131): callee folded as one scalar.
        r.regime = FoldRegime::Disabled;
        // f_entry.final_cost = f_entry.cost_all_nvm + f_start.E_to_leave - f_end.E_to_leave
        r.entryCost = c0 + sfEToLeave - efEToLeave;
        // f_exit.final_cost = f_exit.cost_all_nvm
        r.exitCost = cx0;
        // energy seeds unused (call blocks stay interior, not fixed)
    }
    return r;
}

} // namespace checkpoint
