"""NVM symbol resolution and memory dump parsing for MSP430.

Absorbs scripts/lib/read_nvm.py into the ckpt package as a library module.
Resolves symbol addresses from ELF files via msp430-elf-nm, parses mspdebug
hex dump output, and extracts little-endian integer values.
"""

from __future__ import annotations

import re
from pathlib import Path

from ..errors import DeviceError
from ..runner import run


def resolve_symbols(
    nm_path: str,
    elf_path: Path,
    symbol_names: list[str],
) -> dict[str, tuple[int, int]]:
    """Resolve symbol addresses from ELF via msp430-elf-nm.

    Returns {name: (address, size)}.  Size defaults to 2 for symbols whose
    size is not reported by nm (typical for bare data labels).

    Raises DeviceError if nm fails or a requested symbol is not found.
    """
    result = run(
        [nm_path, "--print-size", str(elf_path)],
        step_name="msp430-elf-nm",
        check=False,
        timeout=10,
    )
    if result.returncode != 0:
        raise DeviceError(
            f"msp430-elf-nm failed (exit {result.returncode}): {result.stderr.strip()}"
        )

    # Parse lines in two formats:
    #   with size:    "00004006 00000002 D __nvm_result"
    #   without size: "00004006 D __nvm_result"
    all_symbols: dict[str, tuple[int, int]] = {}
    for line in result.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 4 and len(parts[0]) >= 4 and len(parts[1]) >= 2:
            try:
                addr = int(parts[0], 16)
                size = int(parts[1], 16)
                name = parts[3]
                all_symbols[name] = (addr, size)
                continue
            except ValueError:
                pass
        if len(parts) >= 3:
            try:
                addr = int(parts[0], 16)
                name = parts[2]
                all_symbols[name] = (addr, 2)  # default uint16_t
            except ValueError:
                pass

    resolved: dict[str, tuple[int, int]] = {}
    for name in symbol_names:
        if name not in all_symbols:
            raise DeviceError(f"Symbol '{name}' not found in {elf_path}")
        resolved[name] = all_symbols[name]

    return resolved


def compute_md_region(
    symbols: dict[str, tuple[int, int]],
) -> tuple[int, int]:
    """Compute contiguous memory region spanning all symbols.

    Returns (min_addr, total_len) where total_len covers from the lowest
    symbol address to the end of the highest symbol.
    """
    min_addr = min(addr for addr, _ in symbols.values())
    max_end = max(addr + size for addr, size in symbols.values())
    return min_addr, max_end - min_addr


def format_md_command(addr: int, length: int) -> str:
    """Format an mspdebug ``md`` command string."""
    return f"md 0x{addr:04x} {length}"


def parse_hex_dump(output: str) -> bytearray:
    """Parse mspdebug hex dump output into raw bytes.

    Expected line format::

        04000: 0a 00 05 00 01 00 00 00  00 00 00 00 00 00 00 00  |................|

    Only lines containing ``|`` (the ASCII dump suffix) are matched, which
    avoids false positives on disassembly output from ``run``.
    """
    data = bytearray()
    for line in output.splitlines():
        if "|" not in line:
            continue
        m = re.match(r"\s*[0-9a-fA-F]+:\s+(.*?)\s*\|", line)
        if m:
            for byte_str in re.findall(r"[0-9a-fA-F]{2}", m.group(1)):
                data.append(int(byte_str, 16))
    return data


def extract_symbol_values(
    data: bytearray,
    symbols: dict[str, tuple[int, int]],
    symbol_names: list[str],
    base_addr: int,
) -> dict[str, int]:
    """Extract symbol values from raw memory data.

    Returns {name: value} where each value is decoded as a little-endian
    integer of the symbol's declared size.

    Raises DeviceError if there is not enough data to cover a symbol.
    """
    values: dict[str, int] = {}
    for name in symbol_names:
        addr, size = symbols[name]
        offset = addr - base_addr
        if offset + size > len(data):
            raise DeviceError(
                f"Not enough data for {name} "
                f"(need offset {offset}+{size}, have {len(data)})"
            )
        if size == 1:
            values[name] = data[offset]
        elif size == 2:
            values[name] = data[offset] | (data[offset + 1] << 8)
        else:
            values[name] = int.from_bytes(data[offset : offset + size], "little")
    return values


def read_nvm_symbols(
    nm_path: str,
    elf_path: Path,
    symbol_names: list[str],
    *,
    md_output: str | None = None,
    timeout: int = 10,
) -> dict[str, int]:
    """High-level: resolve symbols and extract values from hex dump output.

    If *md_output* is provided, it is parsed directly as mspdebug ``md``
    output.  Otherwise a ``DeviceError`` is raised — callers should run
    mspdebug externally and pass the captured output here.

    Returns {symbol_name: value}.
    """
    symbols = resolve_symbols(nm_path, elf_path, symbol_names)
    base_addr, total_len = compute_md_region(symbols)

    if md_output is None:
        raise DeviceError(
            "No md_output provided; run mspdebug externally and pass the "
            "hex dump output via the md_output parameter"
        )

    data = parse_hex_dump(md_output)
    if len(data) < total_len:
        raise DeviceError(f"Expected {total_len} bytes from hex dump, got {len(data)}")

    return extract_symbol_values(data, symbols, symbol_names, base_addr)
