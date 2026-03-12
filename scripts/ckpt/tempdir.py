"""Temporary directory context manager for compilation workdirs."""

from __future__ import annotations

import tempfile
from contextlib import contextmanager
from pathlib import Path


@contextmanager
def compilation_workdir(prefix: str = "ckpt_"):
    """Create a temporary directory for compilation artifacts.

    Automatically cleaned up on exit.
    """
    with tempfile.TemporaryDirectory(prefix=prefix) as tmp:
        yield Path(tmp)
