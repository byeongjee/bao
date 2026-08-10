"""Project environment resolution — replaces top of common.sh."""

from __future__ import annotations

import os
import shutil
import subprocess
from dataclasses import dataclass, field
from pathlib import Path


@dataclass(frozen=True)
class ProjectEnv:
    """Resolved project paths and environment settings."""

    project_dir: Path
    llvm_dir: Path | None
    msp430gcc_toolchain_path: Path
    msp430gcc_support_path: Path
    device: str

    # Pass libraries
    pass_lib: Path = field(init=False)
    bb_debuginfo_lib: Path = field(init=False)
    bb_analyzer: Path = field(init=False)
    machine_pass_lib: Path = field(init=False)

    # Runtime files
    milp_runtime: Path = field(init=False)
    milp_boot: Path = field(init=False)
    milp_linker: Path = field(init=False)

    rockclimb_boot: Path = field(init=False)
    rockclimb_runtime: Path = field(init=False)
    rockclimb_linker: Path = field(init=False)

    schematic_trace_runtime: Path = field(init=False)
    schematic_runtime: Path = field(init=False)
    schematic_boot: Path = field(init=False)
    schematic_linker: Path = field(init=False)

    uninstrumented_boot: Path = field(init=False)
    uninstrumented_runtime: Path = field(init=False)

    bb_freq_runtime: Path = field(init=False)
    debug_common_c: Path = field(init=False)
    vcc_wait_c: Path = field(init=False)

    sysroot_flags: list[str] = field(init=False)

    def __post_init__(self) -> None:
        rt = self.project_dir / "passes" / "runtime"
        bld = self.project_dir / "passes" / "build"

        object.__setattr__(self, "pass_lib", bld / "CheckpointPass.so")
        object.__setattr__(
            self, "bb_debuginfo_lib", bld / "bb-debuginfo" / "BBDebugInfoPass.so"
        )
        object.__setattr__(
            self, "bb_analyzer", bld / "bb-energy-analyzer" / "bb-energy-analyzer"
        )
        object.__setattr__(
            self,
            "machine_pass_lib",
            bld / "rockclimb-backend" / "RockClimbMachinePass.so",
        )

        object.__setattr__(self, "milp_runtime", rt / "milp_runtime.c")
        object.__setattr__(self, "milp_boot", rt / "milp_boot.S")
        object.__setattr__(self, "milp_linker", rt / "milp_msp430fr5994.ld")
        object.__setattr__(self, "rockclimb_boot", rt / "rockclimb_boot.S")
        object.__setattr__(self, "rockclimb_runtime", rt / "rockclimb_runtime.c")
        object.__setattr__(self, "rockclimb_linker", rt / "rockclimb_msp430fr5994.ld")

        object.__setattr__(
            self, "schematic_trace_runtime", rt / "schematic_trace_runtime.c"
        )
        object.__setattr__(self, "schematic_runtime", rt / "schematic_runtime.c")
        object.__setattr__(self, "schematic_boot", rt / "schematic_boot.S")
        object.__setattr__(self, "schematic_linker", rt / "rockclimb_msp430fr5994.ld")
        object.__setattr__(self, "uninstrumented_boot", rt / "uninstrumented_boot.S")
        object.__setattr__(
            self, "uninstrumented_runtime", rt / "uninstrumented_runtime.c"
        )
        object.__setattr__(self, "bb_freq_runtime", rt / "bb_freq_runtime.c")
        object.__setattr__(self, "debug_common_c", rt / "debug_common.c")
        object.__setattr__(self, "vcc_wait_c", rt / "vcc_wait.c")

        # macOS SDK detection
        flags: list[str] = []
        if shutil.which("xcrun"):
            try:
                sdk = subprocess.check_output(
                    ["xcrun", "--show-sdk-path"], text=True, timeout=5
                ).strip()
                flags = ["-isysroot", sdk]
            except subprocess.CalledProcessError, subprocess.TimeoutExpired:
                pass
        object.__setattr__(self, "sysroot_flags", flags)

    @classmethod
    def from_environ(cls, project_dir: Path | None = None) -> ProjectEnv:
        """Construct from environment variables."""
        if project_dir is None:
            # Walk up from this file: scripts/ckpt/env.py -> scripts/ckpt -> scripts -> project
            project_dir = Path(__file__).resolve().parent.parent.parent

        llvm_dir_str = os.environ.get("LLVM_DIR", "")
        llvm_dir = Path(llvm_dir_str) if llvm_dir_str else None

        msp430gcc = Path(
            os.environ.get(
                "MSP430GCC_TOOLCHAIN_PATH", os.path.expanduser("~/ti/msp430-gcc")
            )
        )
        msp430gcc_support = Path(
            os.environ.get("MSP430GCC_SUPPORT_PATH", str(msp430gcc))
        )

        return cls(
            project_dir=project_dir,
            llvm_dir=llvm_dir,
            msp430gcc_toolchain_path=msp430gcc,
            msp430gcc_support_path=msp430gcc_support,
            device="MSP430FR5994",
        )
