"""Toolchain resolution — replaces tool variable definitions in common.sh."""

from __future__ import annotations

import shutil
from dataclasses import dataclass
from pathlib import Path

from .env import ProjectEnv

# Canonical definition lives in errors.py; re-exported for compatibility.
from .errors import ToolNotFoundError


@dataclass(frozen=True)
class Toolchain:
    """Resolved paths to all toolchain binaries."""

    clang: str
    opt: str
    llc: str
    gcc: str
    nm: str
    size: str

    @classmethod
    def resolve(cls, env: ProjectEnv) -> Toolchain:
        """Resolve tool paths from the environment, raising on missing tools."""
        if env.llvm_dir:
            bin_dir = env.llvm_dir / "bin"
            clang = str(bin_dir / "clang")
            opt = str(bin_dir / "opt")
            llc = str(bin_dir / "llc")
        else:
            clang = shutil.which("clang") or "clang"
            opt = shutil.which("opt") or "opt"
            llc = shutil.which("llc") or "llc"

        gcc_path = str(env.msp430gcc_toolchain_path / "bin" / "msp430-elf-gcc")
        if not Path(gcc_path).exists():
            gcc_path = shutil.which("msp430-elf-gcc") or "msp430-elf-gcc"

        nm_path = str(env.msp430gcc_toolchain_path / "bin" / "msp430-elf-nm")
        if not Path(nm_path).exists():
            nm_path = shutil.which("msp430-elf-nm") or "msp430-elf-nm"

        size_path = str(env.msp430gcc_toolchain_path / "bin" / "msp430-elf-size")
        if not Path(size_path).exists():
            size_path = shutil.which("msp430-elf-size") or "msp430-elf-size"

        return cls(
            clang=clang,
            opt=opt,
            llc=llc,
            gcc=gcc_path,
            nm=nm_path,
            size=size_path,
        )

    def validate(self) -> None:
        """Check that critical LLVM tools exist."""
        for name, path in [("clang", self.clang), ("opt", self.opt), ("llc", self.llc)]:
            if not Path(path).exists() and not shutil.which(path):
                raise ToolNotFoundError(name, path)
