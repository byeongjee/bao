"""Flash, run, and read NVM symbols from MSP430 via mspdebug.

Wraps the flash_run_and_read() shell function from common.sh as pure Python.
Executes a single mspdebug session that programs the ELF, sets a breakpoint,
runs the program, and reads back NVM symbol values.
"""

from __future__ import annotations

from pathlib import Path

from ..runner import DeviceError, run
from ..toolchain import Toolchain
from . import nvm


def flash_run_and_read(
    tc: Toolchain,
    elf_path: Path,
    timeout: int,
    symbols: list[str],
) -> dict[str, int]:
    """Flash ELF, run program, and read NVM symbols via mspdebug.

    Executes a single ``mspdebug tilib`` session with four commands:

    1. ``prog <elf>``          -- flash the program
    2. ``setbreak <bkpt_addr>`` -- set breakpoint at ``__nvm_breakpoint``
    3. ``run``                  -- execute until breakpoint
    4. ``md <addr> <len>``      -- dump the NVM region

    Steps:
        1. Resolve ``__nvm_breakpoint`` address from the ELF.
        2. Resolve requested symbol addresses and compute the ``md`` command.
        3. Run mspdebug with all four commands in one session.
        4. Parse the hex dump output and extract symbol values.

    Returns ``{symbol_name: value}``.

    Raises DeviceError on any failure (missing breakpoint symbol, mspdebug
    error, insufficient hex dump data, etc.).
    """
    # 1. Resolve the breakpoint address
    bkpt_symbols = nvm.resolve_symbols(
        tc.nm, elf_path, ["__nvm_breakpoint"],
    )
    bkpt_addr = bkpt_symbols["__nvm_breakpoint"][0]

    # 2. Resolve requested symbols and build the md command
    sym_info = nvm.resolve_symbols(tc.nm, elf_path, symbols)
    base_addr, total_len = nvm.compute_md_region(sym_info)
    md_cmd = nvm.format_md_command(base_addr, total_len)

    # 3. Run mspdebug in a single session
    mspdebug_result = run(
        [
            "timeout", str(timeout),
            "mspdebug", "tilib",
            f"prog {elf_path}",
            f"setbreak 0x{bkpt_addr:04x}",
            "run",
            md_cmd,
        ],
        step_name="mspdebug flash+run+read",
        check=False,
        timeout=timeout + 10,
    )

    if mspdebug_result.returncode != 0:
        raise DeviceError(
            f"mspdebug failed (exit {mspdebug_result.returncode}): "
            f"{mspdebug_result.stderr.strip()}"
        )

    # 4. Parse hex dump from mspdebug output and extract values
    data = nvm.parse_hex_dump(mspdebug_result.stdout)
    if len(data) < total_len:
        raise DeviceError(
            f"Expected {total_len} bytes from hex dump, got {len(data)}"
        )

    return nvm.extract_symbol_values(data, sym_info, symbols, base_addr)
