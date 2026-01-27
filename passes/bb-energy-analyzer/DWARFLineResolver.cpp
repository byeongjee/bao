#include "DWARFLineResolver.h"

#include <algorithm>

namespace bbanalyzer {

unsigned HeuristicLineResolver::resolve(uint64_t address, unsigned rawLine,
                                         const LineContext &context) const {
    // If already mapped, return as-is
    if (rawLine != 0) {
        return rawLine;
    }

    // Heuristic 1: Prologue Attribution
    // Instructions before the first labeled address belong to entry BB
    if (context.prevLabeledLine == 0 && context.nextLabeledLine != 0) {
        return context.entryBB;
    }

    // Heuristic 4: Epilogue Attribution
    // Instructions after the last labeled address belong to exit BB
    if (context.prevLabeledLine != 0 && context.nextLabeledLine == 0) {
        return context.exitBB;
    }

    // Both previous and next are labeled
    if (context.prevLabeledLine != 0 && context.nextLabeledLine != 0) {
        // Heuristic 2: Same-BB Gap Fill
        // Line 0 between two regions of the SAME BB belongs to that BB
        if (context.prevLabeledLine == context.nextLabeledLine) {
            return context.prevLabeledLine;
        }

        // Heuristic 3: Cross-BB Attribution
        // Line 0 between DIFFERENT BBs belongs to previous BB
        // Rationale: PHI copies are inserted in predecessors
        return context.prevLabeledLine;
    }

    // No context available (shouldn't happen in practice)
    return 0;
}

void resolveUnmappedLines(std::vector<LineEntry> &entries,
                          uint64_t funcStart, uint64_t funcEnd,
                          const DWARFLineResolver &resolver) {
    if (entries.empty()) {
        return;
    }

    // Sort entries by address (should already be sorted, but ensure)
    std::sort(entries.begin(), entries.end(),
              [](const LineEntry &a, const LineEntry &b) {
                  return a.address < b.address;
              });

    // Find entry BB (first non-zero line) and exit BB (last non-zero line)
    unsigned entryBB = 0;
    unsigned exitBB = 0;
    for (const auto &entry : entries) {
        if (entry.line != 0) {
            if (entryBB == 0) {
                entryBB = entry.line;
            }
            exitBB = entry.line;
        }
    }

    // Default to 1 if no labeled entries found
    if (entryBB == 0) entryBB = 1;
    if (exitBB == 0) exitBB = 1;

    // Resolve each unmapped entry
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].line != 0) {
            continue; // Already mapped
        }

        // Build context for this entry
        LineContext ctx;
        ctx.funcStartAddr = funcStart;
        ctx.funcEndAddr = funcEnd;
        ctx.entryBB = entryBB;
        ctx.exitBB = exitBB;

        // Find previous labeled line
        ctx.prevLabeledLine = 0;
        ctx.prevLabeledAddr = 0;
        for (size_t j = i; j > 0; --j) {
            if (entries[j - 1].line != 0) {
                ctx.prevLabeledLine = entries[j - 1].line;
                ctx.prevLabeledAddr = entries[j - 1].address;
                break;
            }
        }

        // Find next labeled line
        ctx.nextLabeledLine = 0;
        ctx.nextLabeledAddr = 0;
        for (size_t j = i + 1; j < entries.size(); ++j) {
            if (entries[j].line != 0) {
                ctx.nextLabeledLine = entries[j].line;
                ctx.nextLabeledAddr = entries[j].address;
                break;
            }
        }

        // Resolve and update
        entries[i].line = resolver.resolve(entries[i].address, entries[i].line, ctx);
    }
}

} // namespace bbanalyzer
