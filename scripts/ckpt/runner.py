"""Subprocess wrapper — replaces raw shell execution in bash scripts."""

from __future__ import annotations

import logging
import subprocess
import sys
import time
from dataclasses import dataclass


# Exception classes — canonical definitions live in errors.py.
# Re-exported here for backward compatibility.
from .errors import (  # noqa: F401
    CompilationError,
    ConfigError,
    DeviceError,
    InfeasibleError,
    ToolError,
)

logger = logging.getLogger(__name__)


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
    if step_name:
        logger.info("Running %s...", step_name)

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

    if result.stdout:
        logger.debug("%s stdout:\n%s", step_name or "subprocess", result.stdout.rstrip())
    if result.stderr:
        sys.stderr.write(result.stderr)
        if not result.stderr.endswith("\n"):
            sys.stderr.write("\n")

    step = StepResult(
        returncode=result.returncode,
        stdout=result.stdout,
        stderr=result.stderr,
        duration_ms=elapsed_ms,
    )

    if check and result.returncode != 0:
        raise CompilationError(step_name or " ".join(cmd[:3]), step)

    return step
