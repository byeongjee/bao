"""Flash and read NVM symbols from MSP430 via mspdebug."""

from __future__ import annotations

from pathlib import Path

from ..runner import DeviceError, run
from ..toolchain import Toolchain
from . import nvm



def flash(
    tc: Toolchain,
    elf_path: Path,
    timeout: int,
) -> None:
    """Flash ELF to MSP430 via mspdebug. Does not run the program.

    Executes ``mspdebug tilib "prog <elf>"`` in a single session.
    The device resets after programming.

    Raises DeviceError on failure.
    """
    mspdebug_result = run(
        [
            "timeout", str(timeout),
            "mspdebug", "tilib",
            f"prog {elf_path}",
        ],
        step_name="mspdebug flash",
        check=False,
        timeout=timeout + 10,
    )

    if mspdebug_result.returncode != 0:
        raise DeviceError(
            f"mspdebug flash failed (exit {mspdebug_result.returncode}): "
            f"{mspdebug_result.stderr.strip()}"
        )


def read_nvm(
    tc: Toolchain,
    elf_path: Path,
    timeout: int,
    symbols: list[str],
) -> dict[str, int]:
    """Read NVM symbol values from a halted MSP430 via mspdebug.

    Starts a new mspdebug session, resolves symbol addresses from the ELF,
    and uses ``md`` to dump memory. Assumes the device is accessible
    (on continuous JTAG/USB power, program has completed).

    Returns ``{symbol_name: value}``.
    Raises DeviceError on failure.
    """
    sym_info = nvm.resolve_symbols(tc.nm, elf_path, symbols)
    base_addr, total_len = nvm.compute_md_region(sym_info)
    md_cmd = nvm.format_md_command(base_addr, total_len)

    mspdebug_result = run(
        [
            "timeout", str(timeout),
            "mspdebug", "tilib",
            md_cmd,
        ],
        step_name="mspdebug read-nvm",
        check=False,
        timeout=timeout + 10,
    )

    if mspdebug_result.returncode != 0:
        raise DeviceError(
            f"mspdebug read-nvm failed (exit {mspdebug_result.returncode}): "
            f"{mspdebug_result.stderr.strip()}"
        )

    data = nvm.parse_hex_dump(mspdebug_result.stdout)
    if len(data) < total_len:
        raise DeviceError(
            f"Expected {total_len} bytes from hex dump, got {len(data)}"
        )

    return nvm.extract_symbol_values(data, sym_info, symbols, base_addr)
