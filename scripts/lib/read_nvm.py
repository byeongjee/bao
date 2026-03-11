#!/usr/bin/env python3
"""Read NVM symbols from a flashed MSP430 via mspdebug memory dump.

Resolves symbol addresses from the ELF via msp430-elf-nm, then reads
memory via a single `mspdebug tilib "md <addr> <len>"` call.
Outputs key=value pairs (uint16_t, little-endian).

Modes:
    Default:       Run mspdebug to read memory directly.
    --md-cmd-only: Print the mspdebug "md" command without running it.
    --parse-md:    Parse mspdebug output from stdin (hex dump lines).

Usage:
    read_nvm.py --elf <path> --symbols sym1,sym2,...
    read_nvm.py --elf <path> --symbols sym1,sym2,... --md-cmd-only
    cat mspdebug_output | read_nvm.py --elf <path> --symbols sym1,sym2,... --parse-md

Exit codes:
    0 - success (all symbols read)
    1 - error (symbol not found, mspdebug failure, etc.)
"""

import argparse
import re
import subprocess
import sys


def resolve_symbols(elf_path, symbol_names):
    """Run msp430-elf-nm to get symbol addresses.

    Returns dict of {name: (address, size)} where size defaults to 2
    (uint16_t) for data symbols.
    """
    result = subprocess.run(
        ["msp430-elf-nm", "--print-size", elf_path],
        capture_output=True, text=True, timeout=10,
    )
    if result.returncode != 0:
        print(f"Error: msp430-elf-nm failed: {result.stderr}", file=sys.stderr)
        sys.exit(1)

    # Parse lines like: "00004006 00000002 D __nvm_result"
    # or without size:  "00004006 D __nvm_result"
    symbols = {}
    for line in result.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 3:
            # Try format with size: addr size type name
            if len(parts) >= 4 and len(parts[0]) >= 4 and len(parts[1]) >= 2:
                try:
                    addr = int(parts[0], 16)
                    size = int(parts[1], 16)
                    name = parts[3]
                    symbols[name] = (addr, size)
                    continue
                except ValueError:
                    pass
            # Format without size: addr type name
            try:
                addr = int(parts[0], 16)
                name = parts[2]
                symbols[name] = (addr, 2)  # default uint16_t
            except ValueError:
                pass

    resolved = {}
    for name in symbol_names:
        if name in symbols:
            resolved[name] = symbols[name]
        else:
            print(f"Error: Symbol '{name}' not found in {elf_path}", file=sys.stderr)
            sys.exit(1)

    return resolved


def compute_region(symbols):
    """Compute the contiguous memory region spanning all symbols.

    Returns (min_addr, total_len).
    """
    min_addr = min(addr for addr, _ in symbols.values())
    max_end = max(addr + size for addr, size in symbols.values())
    return min_addr, max_end - min_addr


def read_memory(addr, length, timeout=10):
    """Read raw memory via mspdebug tilib 'md <addr> <len>'.

    Returns bytes read from device.
    """
    cmd = f"md 0x{addr:04x} {length}"
    result = subprocess.run(
        ["mspdebug", "tilib", cmd],
        capture_output=True, text=True, timeout=timeout,
    )
    if result.returncode != 0:
        print(f"Error: mspdebug failed: {result.stderr}", file=sys.stderr)
        sys.exit(1)

    return parse_hex_dump(result.stdout)


def parse_hex_dump(output):
    """Parse mspdebug hex dump (md) output.

    Format: '    04000: 0a 00 05 00 01 00 00 00  00 00 00 00 00 00 00 00  |................|'
    Only matches lines with the '|...|' ASCII dump suffix to avoid
    false matches on disassembly output from 'run'.
    Returns bytearray of the raw data.
    """
    data = bytearray()
    for line in output.splitlines():
        # Match md output: address, hex bytes, then |ascii| at end
        if "|" not in line:
            continue
        m = re.match(r"\s*[0-9a-fA-F]+:\s+((?:[0-9a-fA-F]{2}\s)+)", line)
        if m:
            hex_part = m.group(1).strip()
            for byte_str in hex_part.split():
                data.append(int(byte_str, 16))
    return data


def extract_values(data, symbols, symbol_names, min_addr):
    """Extract symbol values from raw memory data."""
    for name in symbol_names:
        addr, size = symbols[name]
        offset = addr - min_addr
        if offset + size > len(data):
            print(f"Error: Not enough data for {name} (need offset {offset}+{size}, have {len(data)})", file=sys.stderr)
            sys.exit(1)
        if size == 2:
            value = data[offset] | (data[offset + 1] << 8)
        elif size == 1:
            value = data[offset]
        else:
            value = int.from_bytes(data[offset:offset + size], "little")
        print(f"{name}={value}")


def main():
    parser = argparse.ArgumentParser(
        description="Read NVM symbols from MSP430 via mspdebug."
    )
    parser.add_argument("--elf", required=True, help="Path to ELF file")
    parser.add_argument(
        "--symbols", required=True,
        help="Comma-separated list of symbol names to read",
    )
    parser.add_argument(
        "--timeout", type=int, default=10,
        help="mspdebug timeout in seconds (default: 10)",
    )
    parser.add_argument(
        "--md-cmd-only", action="store_true",
        help="Print the mspdebug md command instead of running it",
    )
    parser.add_argument(
        "--parse-md", action="store_true",
        help="Parse mspdebug hex dump output from stdin",
    )
    args = parser.parse_args()

    symbol_names = [s.strip() for s in args.symbols.split(",")]
    symbols = resolve_symbols(args.elf, symbol_names)
    min_addr, total_len = compute_region(symbols)

    if args.md_cmd_only:
        # Output the md command for use in a single mspdebug session
        print(f"md 0x{min_addr:04x} {total_len}")
        return

    if args.parse_md:
        # Parse hex dump from stdin (piped mspdebug output)
        stdin_data = sys.stdin.read()
        data = parse_hex_dump(stdin_data)
        if len(data) < total_len:
            print(
                f"Error: Expected {total_len} bytes from hex dump, got {len(data)}",
                file=sys.stderr,
            )
            sys.exit(1)
        extract_values(data, symbols, symbol_names, min_addr)
        return

    # Default: run mspdebug directly
    data = read_memory(min_addr, total_len, timeout=args.timeout)

    if len(data) < total_len:
        print(
            f"Error: Expected {total_len} bytes, got {len(data)}",
            file=sys.stderr,
        )
        sys.exit(1)

    extract_values(data, symbols, symbol_names, min_addr)


if __name__ == "__main__":
    main()
