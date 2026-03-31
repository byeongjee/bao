"""Unit tests for ckpt.output_parser — pure functions, no mocking needed."""

from __future__ import annotations

import pytest

from ckpt.output_parser import (
    NvmCounters,
    PassStatistics,
    _parse_int,
    detect_infeasibility,
    extract_stat,
    has_pass_statistics,
    load_stats_json,
    nvm_counters_to_labels,
    parse_nvm_output,
    parse_pass_output,
)


# ---------------------------------------------------------------------------
# extract_stat
# ---------------------------------------------------------------------------

class TestExtractStat:
    def test_first_label_match(self):
        text = "Basic blocks: 42\nEdges: 10"
        assert extract_stat(text, "Basic blocks") == "42"

    def test_second_label_match(self):
        text = "Edges: 10\nBasic blocks (concrete): 42"
        assert extract_stat(text, "Basic blocks (concrete)", "Basic blocks") == "42"

    def test_fallback_label(self):
        text = "Basic blocks: 7"
        assert extract_stat(text, "Basic blocks (concrete)", "Basic blocks") == "7"

    def test_no_match(self):
        assert extract_stat("nothing here", "Basic blocks") is None

    def test_empty_text(self):
        assert extract_stat("", "Basic blocks") is None

    def test_multi_colon_line(self):
        text = "Optimal solution: obj=3.0: extra"
        assert extract_stat(text, "Optimal solution") == "obj=3.0:"

    def test_whitespace_variations(self):
        text = "  Solve time (ms):   150  "
        assert extract_stat(text, "Solve time (ms)") == "150"

    def test_empty_value_after_colon(self):
        text = "Basic blocks:"
        assert extract_stat(text, "Basic blocks") is None


# ---------------------------------------------------------------------------
# _parse_int
# ---------------------------------------------------------------------------

class TestParseInt:
    @pytest.mark.parametrize(
        "val, expected",
        [
            ("42", 42),
            ("123.0", 123),
            ("abc", None),
            (None, None),
            ("0", 0),
        ],
        ids=["int-string", "float-string", "non-numeric", "none", "zero"],
    )
    def test_parse_int(self, val, expected):
        assert _parse_int(val) == expected


# ---------------------------------------------------------------------------
# parse_pass_output
# ---------------------------------------------------------------------------

class TestParsePassOutput:
    REALISTIC_OUTPUT = """\
--- Checkpoint Insertion Statistics ---
Basic blocks (concrete): 24
Edges (concrete): 30
Abstract CFG size: 11
Regions: 3
Candidate globals (V_elig): 5
MILP allocation mode: coarse
MILP variables (before presolve): 100
MILP constraints (before presolve): 200
MILP variables (after presolve): 60
MILP constraints (after presolve): 120
Optimal solution: 3.0
Region boundaries: 4
Distributed checkpoints inserted: 2
Solve time (ms): 150
Compilation time (ms): 500
Peak RSS (KB): 10240
"""

    def test_realistic_output(self):
        stats = parse_pass_output(self.REALISTIC_OUTPUT)
        assert stats.basic_blocks == 24
        assert stats.edges == 30
        assert stats.abstract_cfg_size == 11
        assert stats.regions == 3
        assert stats.candidate_globals == 5
        assert stats.milp_allocation_mode == "coarse"
        assert stats.milp_variables == 100
        assert stats.milp_constraints == 200
        assert stats.milp_presolved_variables == 60
        assert stats.milp_presolved_constraints == 120
        assert stats.optimal_solution == "3.0"
        assert stats.region_boundaries == 4
        assert stats.distributed_checkpoints == 2
        assert stats.solve_time_ms == 150
        assert stats.compilation_time_ms == 500
        assert stats.peak_rss_kb == 10240

    def test_partial_output(self):
        text = "Basic blocks: 10\nEdges: 5\n"
        stats = parse_pass_output(text)
        assert stats.basic_blocks == 10
        assert stats.edges == 5
        assert stats.abstract_cfg_size is None
        assert stats.regions is None
        assert stats.milp_variables is None
        assert stats.milp_presolved_variables is None

    def test_empty_string(self):
        stats = parse_pass_output("")
        assert stats == PassStatistics()

    def test_legacy_abstract_cfg_label(self):
        stats = parse_pass_output("Basic blocks (abstract): 9\n")
        assert stats.abstract_cfg_size == 9

    def test_legacy_milp_size_labels(self):
        stats = parse_pass_output("MILP variables: 17\nMILP constraints: 31\n")
        assert stats.milp_variables == 17
        assert stats.milp_constraints == 31


# ---------------------------------------------------------------------------
# parse_nvm_output
# ---------------------------------------------------------------------------

class TestParseNvmOutput:
    def test_all_keys_present(self):
        text = """\
__nvm_result=1
__nvm_done=1
cnt_boundary=5
cnt_save_vreg=10
cnt_restore_vreg=8
cnt_store_mem=20
cnt_restore_mem=15
cnt_save_reg=3
cnt_restore_reg=2
"""
        counters = parse_nvm_output(text)
        assert counters.result == 1
        assert counters.done == 1
        assert counters.region_boundary == 5
        assert counters.save_vreg == 10
        assert counters.restore_vreg == 8
        assert counters.store_mem == 20
        assert counters.restore_mem == 15
        assert counters.save_reg == 3
        assert counters.restore_reg == 2

    def test_subset_of_keys(self):
        text = "__nvm_result=1\ncnt_boundary=3\n"
        counters = parse_nvm_output(text)
        assert counters.result == 1
        assert counters.region_boundary == 3
        assert counters.save_vreg is None
        assert counters.done is None

    def test_garbage_lines_ignored(self):
        text = "some random output\n__nvm_result=42\nmore noise\n"
        counters = parse_nvm_output(text)
        assert counters.result == 42
        assert counters.region_boundary is None

    def test_large_32bit_counter_values(self):
        text = "cnt_boundary=70000\ncnt_save_reg=131072\ncnt_restore_mem=4294967295\n"
        counters = parse_nvm_output(text)
        assert counters.region_boundary == 70000
        assert counters.save_reg == 131072
        assert counters.restore_mem == 4294967295

    def test_empty(self):
        counters = parse_nvm_output("")
        assert counters == NvmCounters()


# ---------------------------------------------------------------------------
# nvm_counters_to_labels
# ---------------------------------------------------------------------------

class TestNvmCountersToLabels:
    def test_full_counters(self):
        c = NvmCounters(
            result=1,
            region_boundary=5,
            save_vreg=10,
            restore_vreg=8,
            store_mem=20,
            restore_mem=15,
            save_reg=3,
            restore_reg=2,
        )
        text = nvm_counters_to_labels(c)
        assert "RESULT: 1" in text
        assert "__region_boundary: 5" in text
        assert "vreg_saves: 10" in text
        assert "vreg_restores: 8" in text
        assert "mem_stores: 20" in text
        assert "mem_restores: 15" in text
        assert "reg_saves: 3" in text
        assert "reg_restores: 2" in text

    def test_partial_counters(self):
        c = NvmCounters(result=1, region_boundary=5)
        text = nvm_counters_to_labels(c)
        assert "RESULT: 1" in text
        assert "__region_boundary: 5" in text
        assert "vreg_saves" not in text

    def test_large_counter_values(self):
        c = NvmCounters(region_boundary=70000, save_reg=131072)
        text = nvm_counters_to_labels(c)
        assert "__region_boundary: 70000" in text
        assert "reg_saves: 131072" in text

    def test_all_none(self):
        c = NvmCounters()
        assert nvm_counters_to_labels(c) == ""


# ---------------------------------------------------------------------------
# detect_infeasibility
# ---------------------------------------------------------------------------

class TestDetectInfeasibility:
    @pytest.mark.parametrize(
        "pattern, expected_reason",
        [
            ("blocks exceed energy capacity", "blocks exceed energy capacity"),
            ("Optimization failed", "solver found no feasible solution"),
            ("Region partitioning failed", "region partitioning failed"),
            ("blocks exceed E_safe", "blocks exceed E_safe"),
            ("SCHEMATIC infeasible", "energy capacity too small"),
        ],
        ids=[
            "energy-capacity",
            "optimization-failed",
            "region-partition",
            "e-safe",
            "schematic",
        ],
    )
    def test_each_pattern(self, pattern, expected_reason):
        text = f"some output\n{pattern}\nmore output"
        assert detect_infeasibility(text) == expected_reason

    def test_no_match(self):
        assert detect_infeasibility("all good here") is None

    def test_empty(self):
        assert detect_infeasibility("") is None


# ---------------------------------------------------------------------------
# has_pass_statistics
# ---------------------------------------------------------------------------

class TestHasPassStatistics:
    def test_present(self):
        assert has_pass_statistics("--- Checkpoint Insertion Statistics ---") is True

    def test_absent(self):
        assert has_pass_statistics("no stats here") is False


# ---------------------------------------------------------------------------
# load_stats_json
# ---------------------------------------------------------------------------

class TestLoadStatsJson:
    def test_loads_top_level_abstract_cfg_size(self):
        stats, feasible, infeasibility_reason = load_stats_json(
            {
                "basic_blocks": 24,
                "edges": 30,
                "abstract_cfg_size": 11,
                "milp_allocation_mode": "coarse",
                "milp_variables": 100,
                "milp_constraints": 200,
                "milp_presolved_variables": 60,
                "milp_presolved_constraints": 120,
                "compilation_time_ms": 500.0,
            }
        )
        assert feasible is True
        assert infeasibility_reason is None
        assert stats.abstract_cfg_size == 11
        assert stats.milp_allocation_mode == "coarse"
        assert stats.milp_variables == 100
        assert stats.milp_constraints == 200
        assert stats.milp_presolved_variables == 60
        assert stats.milp_presolved_constraints == 120
        assert stats.compilation_time_ms == 500

    def test_falls_back_to_nested_abstract_cfg_nodes(self):
        stats, feasible, infeasibility_reason = load_stats_json(
            {
                "abstract_cfg": {
                    "abstract_nodes": 7,
                },
                "feasible": False,
                "infeasibility_reason": "solver found no feasible solution",
            }
        )
        assert feasible is False
        assert infeasibility_reason == "solver found no feasible solution"
        assert stats.abstract_cfg_size == 7
