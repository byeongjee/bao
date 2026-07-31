"""Unit tests for MILP compile-side helpers."""

from __future__ import annotations

import json
from pathlib import Path

import pytest
from ckpt.compile.milp import _merge_strip_mining_reclamp_stats

pytestmark = pytest.mark.unit


def test_merge_strip_mining_reclamp_stats_updates_chosen_k(tmp_path: Path) -> None:
    base_path = tmp_path / "strip.json"
    reclamp_path = tmp_path / "reclamp.json"

    base_data = {
        "functions": [
            {
                "function": "main",
                "summary": {
                    "loops_seen": 4,
                    "loops_eligible": 2,
                    "loops_rewritten": 1,
                    "loops_chunked": 1,
                },
                "skipped_reasons": {},
                "chosen_k_values": [
                    {"loop_header": "for.cond11.i.preheader", "chosen_k": 13},
                ],
                "loop_details": [
                    {
                        "loop_header": "for.cond11.i.preheader",
                        "decision": "selected",
                        "chosen_k_valid": True,
                        "chosen_k": 13,
                        "post_chunk_reclamp_attempted": True,
                        "post_chunk_reclamp_succeeded": True,
                        "post_chunk_reclamp_applied": False,
                        "post_chunk_max_k_valid": True,
                        "post_chunk_max_k": 13,
                        "post_chunk_iter_energy_valid": True,
                        "post_chunk_iter_energy": 17606.36,
                        "post_chunk_reclamp_error": "",
                    },
                ],
            },
        ],
    }
    reclamp_data = {
        "functions": [
            {
                "function": "main",
                "summary": {
                    "loops_seen": 1,
                    "loops_eligible": 1,
                    "loops_rewritten": 1,
                    "loops_chunked": 1,
                },
                "skipped_reasons": {},
                "chosen_k_values": [
                    {"loop_header": "for.cond11.i.preheader", "chosen_k": 12},
                ],
                "loop_details": [
                    {
                        "loop_header": "for.cond11.i.preheader",
                        "decision": "reclamped",
                        "chosen_k_valid": True,
                        "chosen_k": 12,
                        "post_chunk_reclamp_attempted": True,
                        "post_chunk_reclamp_succeeded": True,
                        "post_chunk_reclamp_applied": True,
                        "post_chunk_max_k_valid": True,
                        "post_chunk_max_k": 12,
                        "post_chunk_iter_energy_valid": True,
                        "post_chunk_iter_energy": 19686.82,
                        "post_chunk_reclamp_error": "",
                    },
                ],
            },
        ],
    }

    base_path.write_text(json.dumps(base_data))
    reclamp_path.write_text(json.dumps(reclamp_data))

    _merge_strip_mining_reclamp_stats(base_path, reclamp_path)

    merged = json.loads(base_path.read_text())
    chosen = merged["functions"][0]["chosen_k_values"]
    assert chosen == [{"loop_header": "for.cond11.i.preheader", "chosen_k": 12}]

    detail = merged["functions"][0]["loop_details"][0]
    assert detail["decision"] == "reclamped"
    assert detail["chosen_k"] == 12
    assert detail["post_chunk_reclamp_applied"] is True
    assert detail["post_chunk_max_k"] == 12
    assert detail["post_chunk_iter_energy"] == 19686.82


def test_merge_strip_mining_reclamp_stats_matches_by_function(tmp_path: Path) -> None:
    base_path = tmp_path / "strip.json"
    reclamp_path = tmp_path / "reclamp.json"

    base_data = {
        "functions": [
            {
                "function": "helper",
                "summary": {},
                "skipped_reasons": {},
                "chosen_k_values": [
                    {"loop_header": "helper.loop", "chosen_k": 5},
                ],
                "loop_details": [
                    {
                        "loop_header": "helper.loop",
                        "decision": "selected",
                        "chosen_k_valid": True,
                        "chosen_k": 5,
                    },
                ],
            },
            {
                "function": "main",
                "summary": {},
                "skipped_reasons": {},
                "chosen_k_values": [
                    {"loop_header": "for.cond11.i.preheader", "chosen_k": 13},
                ],
                "loop_details": [
                    {
                        "loop_header": "for.cond11.i.preheader",
                        "decision": "selected",
                        "chosen_k_valid": True,
                        "chosen_k": 13,
                    },
                ],
            },
        ],
    }
    reclamp_data = {
        "functions": [
            {
                "function": "main",
                "summary": {},
                "skipped_reasons": {},
                "chosen_k_values": [
                    {"loop_header": "for.cond11.i.preheader", "chosen_k": 12},
                ],
                "loop_details": [
                    {
                        "loop_header": "for.cond11.i.preheader",
                        "decision": "reclamped",
                        "chosen_k_valid": True,
                        "chosen_k": 12,
                        "post_chunk_reclamp_applied": True,
                    },
                ],
            },
        ],
    }

    base_path.write_text(json.dumps(base_data))
    reclamp_path.write_text(json.dumps(reclamp_data))

    _merge_strip_mining_reclamp_stats(base_path, reclamp_path)

    merged = json.loads(base_path.read_text())
    helper_entry = merged["functions"][0]
    main_entry = merged["functions"][1]

    assert helper_entry["chosen_k_values"] == [
        {"loop_header": "helper.loop", "chosen_k": 5}
    ]
    assert main_entry["chosen_k_values"] == [
        {"loop_header": "for.cond11.i.preheader", "chosen_k": 12},
    ]
    assert main_entry["loop_details"][0]["decision"] == "reclamped"
    assert main_entry["loop_details"][0]["post_chunk_reclamp_applied"] is True
