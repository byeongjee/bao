"""Subprocess wrapper — replaces raw shell execution in bash scripts."""

from __future__ import annotations

import subprocess
import time
from dataclasses import dataclass


class ToolError(Exception):
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


class InfeasibleError(Exception):
    """The optimization problem is infeasible."""

    def __init__(self, reason: str) -> None:
        self.reason = reason
        super().__init__(f"Infeasible: {reason}")


class DeviceError(Exception):
    """A device interaction failed."""


class ConfigError(Exception):
    """A configuration error."""


@dataclass(frozen=True)
class StepResult:
    """Result of a subprocess invocation."""

    returncode: int
    stdout: str
    stderr: str
    duration_ms: int

    @property
    def output(self) -> str:
        """Combined stdout + stderr."""
        return self.stdout + self.stderr


def run(
    cmd: list[str],
    *,
    check: bool = True,
    step_name: str = "",
    cwd: str | None = None,
    timeout: int = 300,
    input: str | None = None,
) -> StepResult:
    """Run a subprocess, capturing output and timing.

    Raises CompilationError on non-zero exit if check=True.
    """
    start = time.monotonic()
    result = subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        cwd=cwd,
        timeout=timeout,
        input=input,
    )
    elapsed_ms = int((time.monotonic() - start) * 1000)

    step = StepResult(
        returncode=result.returncode,
        stdout=result.stdout,
        stderr=result.stderr,
        duration_ms=elapsed_ms,
    )

    if check and result.returncode != 0:
        raise CompilationError(step_name or " ".join(cmd[:3]), step)

    return step
