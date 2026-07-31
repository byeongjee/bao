"""Unit tests for ckpt.device.nvm — pure parsing functions, no device needed."""

from __future__ import annotations

import pytest
from ckpt.device.nvm import (
    compute_md_region,
    extract_symbol_values,
    format_md_command,
    parse_hex_dump,
)
from ckpt.errors import DeviceError

# ---------------------------------------------------------------------------
# parse_hex_dump
# ---------------------------------------------------------------------------


class TestParseHexDump:
    def test_single_line(self):
        line = "    04000: 0a 00 05 00 01 00 00 00  00 00 00 00 00 00 00 00  |................|"
        data = parse_hex_dump(line)
        assert data == bytearray(
            [
                0x0A,
                0x00,
                0x05,
                0x00,
                0x01,
                0x00,
                0x00,
                0x00,
                0x00,
                0x00,
                0x00,
                0x00,
                0x00,
                0x00,
                0x00,
                0x00,
            ]
        )

    def test_multi_line(self):
        output = (
            "    04000: 0a 00 05 00 01 00 00 00  00 00 00 00 00 00 00 00  |................|\n"
            "    04010: ff 01 02 03 04 05 06 07  08 09 0a 0b 0c 0d 0e 0f  |................|"
        )
        data = parse_hex_dump(output)
        assert len(data) == 32
        assert data[0] == 0x0A
        assert data[16] == 0xFF

    def test_lines_without_pipe_ignored(self):
        output = "Reading memory...\n    04000: 0a 00  |..|\nDone.\n"
        data = parse_hex_dump(output)
        assert data == bytearray([0x0A, 0x00])

    def test_empty(self):
        assert parse_hex_dump("") == bytearray()


# ---------------------------------------------------------------------------
# extract_symbol_values
# ---------------------------------------------------------------------------


class TestExtractSymbolValues:
    def test_one_byte(self):
        data = bytearray([0x42, 0x00])
        symbols = {"x": (0x4000, 1)}
        result = extract_symbol_values(data, symbols, ["x"], base_addr=0x4000)
        assert result == {"x": 0x42}

    def test_two_byte_little_endian(self):
        data = bytearray([0x0A, 0x00])
        symbols = {"x": (0x4000, 2)}
        result = extract_symbol_values(data, symbols, ["x"], base_addr=0x4000)
        assert result == {"x": 0x000A}

    def test_two_byte_high(self):
        # 0x0305 in little-endian is [0x05, 0x03]
        data = bytearray([0x05, 0x03])
        symbols = {"x": (0x4000, 2)}
        result = extract_symbol_values(data, symbols, ["x"], base_addr=0x4000)
        assert result == {"x": 0x0305}

    def test_four_byte(self):
        data = bytearray([0x01, 0x02, 0x03, 0x04])
        symbols = {"x": (0x4000, 4)}
        result = extract_symbol_values(data, symbols, ["x"], base_addr=0x4000)
        assert result == {"x": 0x04030201}

    def test_out_of_bounds_raises(self):
        data = bytearray([0x01])
        symbols = {"x": (0x4000, 2)}
        with pytest.raises(DeviceError, match="Not enough data"):
            extract_symbol_values(data, symbols, ["x"], base_addr=0x4000)

    def test_multiple_symbols_with_offset(self):
        data = bytearray([0x0A, 0x00, 0x05, 0x00])
        symbols = {"a": (0x4000, 2), "b": (0x4002, 2)}
        result = extract_symbol_values(data, symbols, ["a", "b"], base_addr=0x4000)
        assert result == {"a": 0x000A, "b": 0x0005}

    def test_mixed_width_symbols_with_offset(self):
        data = bytearray([0x34, 0x12, 0x78, 0x56, 0xBC, 0x9A])
        symbols = {"a": (0x4000, 2), "b": (0x4002, 4)}
        result = extract_symbol_values(data, symbols, ["a", "b"], base_addr=0x4000)
        assert result == {"a": 0x1234, "b": 0x9ABC5678}


# ---------------------------------------------------------------------------
# compute_md_region
# ---------------------------------------------------------------------------


class TestComputeMdRegion:
    def test_single_symbol(self):
        symbols = {"x": (0x4000, 2)}
        addr, length = compute_md_region(symbols)
        assert addr == 0x4000
        assert length == 2

    def test_multiple_non_contiguous(self):
        symbols = {"a": (0x4000, 2), "b": (0x4010, 4)}
        addr, length = compute_md_region(symbols)
        assert addr == 0x4000
        assert length == 0x4014 - 0x4000  # 20 bytes


# ---------------------------------------------------------------------------
# format_md_command
# ---------------------------------------------------------------------------


class TestFormatMdCommand:
    def test_basic(self):
        assert format_md_command(0x4000, 16) == "md 0x4000 16"

    def test_small_address(self):
        assert format_md_command(0x0010, 2) == "md 0x0010 2"
