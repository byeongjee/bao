"""Exception hierarchy for the ckpt package.

All package-specific errors inherit from :class:`CkptError` so that
``cli.py`` can catch them in a single handler and map to exit codes.
"""

from __future__ import annotations

from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from pathlib import Path

    from .runner import StepResult


class CkptError(Exception):
    """Base class for all ckpt package errors."""


class ToolError(CkptError):
    """A tool invocation failed."""

    def __init__(self, step: str, result: StepResult) -> None:
        self.step = step
        self.result = result
        super().__init__(
            f"{step} failed (exit code {result.returncode})\n"
            f"stderr: {result.stderr[:500]}"
        )


class CompilationError(ToolError):
    """A compilation step failed.

    When a post-pass step (e.g. linking) fails, ``pass_output`` may carry
    the earlier LLVM pass output so that statistics are not lost.
    """

    pass_output: str = ""
    stats_json: Path | None = None


class ToolNotFoundError(CkptError):
    """A required toolchain binary was not found."""

    def __init__(self, name: str, path: str) -> None:
        self.name = name
        self.path = path
        self.step = name
        self.result = None
        super().__init__(f"{name} not found at: {path}")


class DeviceError(CkptError):
    """A device interaction failed."""


class ConfigError(CkptError):
    """A configuration error."""


class InfeasibleError(CkptError):
    """The optimization problem is infeasible."""

    def __init__(self, reason: str) -> None:
        self.reason = reason
        super().__init__(f"Infeasible: {reason}")


