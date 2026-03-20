"""Logging configuration for the ckpt package."""

from __future__ import annotations

import logging
import sys

_DEBUG_FMT = logging.Formatter("%(levelname)s [%(name)s]: %(message)s")
_DEFAULT_FMT = logging.Formatter("%(levelname)s: %(message)s")


class _Formatter(logging.Formatter):
    """Minimal for INFO+, module-prefixed for DEBUG. Thread-safe."""

    def format(self, record: logging.LogRecord) -> str:
        if record.levelno <= logging.DEBUG:
            return _DEBUG_FMT.format(record)
        return _DEFAULT_FMT.format(record)


def setup_logging(level_name: str) -> None:
    """Configure the ckpt root logger. Called once from CLI entry point."""
    level = getattr(logging, level_name.upper())
    root = logging.getLogger("ckpt")
    root.setLevel(level)
    if not root.handlers:
        handler = logging.StreamHandler(sys.stderr)
        handler.setFormatter(_Formatter())
        root.addHandler(handler)


def python_to_cpp_log_level(python_level: str) -> str:
    """Map Python log level name to C++ plog level name.

    Python CRITICAL has no plog equivalent; map to 'error'.
    """
    _MAP = {
        "debug": "debug",
        "info": "info",
        "warning": "warning",
        "error": "error",
        "critical": "error",
    }
    return _MAP[python_level.lower()]
