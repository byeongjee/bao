"""Compile results saved to a directory, so that compilation (in Docker) and
device runs (on the host) can happen in separate invocations.

With ``--save-build DIR`` the compile outputs are written under DIR, and each
compile call's return value (or raised error) is pickled next to them. With
``--from-build DIR`` the same calls return the pickled values instead of
compiling. DIR has the same absolute path in the container and on the host,
so the paths inside the pickled values stay valid.
"""

from __future__ import annotations

import pickle
import subprocess
import time
from collections.abc import Callable, Iterator
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path

from .errors import CkptError
from .tempdir import compilation_workdir

# Errors from a compile call that are saved and raised again on load: those
# the callers handle. Any other error stops the run, as it did before.
SAVED_ERRORS = (CkptError, OSError, subprocess.SubprocessError)


@dataclass(frozen=True)
class SavedBuild:
    path: Path
    save: bool  # True: compile and save (--save-build); False: load (--from-build)

    def sub(self, name: str) -> SavedBuild:
        return SavedBuild(self.path / name, self.save)


def compile_or_load[T](
    saved_build: SavedBuild | None, key: str, compile: Callable[[], T]
) -> tuple[T, int]:
    """Return *compile*'s result and its duration in ms, saving or loading it
    according to *saved_build*. A saved error is raised again on load."""
    if saved_build is not None and not saved_build.save:
        with open(saved_build.path / f"{key}.pickle", "rb") as f:
            outcome, elapsed_ms = pickle.load(f)
        if isinstance(outcome, Exception):
            raise outcome
        return outcome, elapsed_ms

    t0 = time.monotonic()
    if saved_build is None:
        value = compile()
        return value, int((time.monotonic() - t0) * 1000)

    try:
        outcome = compile()
    except SAVED_ERRORS as exc:
        outcome = exc
    elapsed_ms = int((time.monotonic() - t0) * 1000)
    pickle_path = saved_build.path / f"{key}.pickle"
    pickle_path.parent.mkdir(parents=True, exist_ok=True)
    with open(pickle_path, "wb") as f:
        pickle.dump((outcome, elapsed_ms), f)
    if isinstance(outcome, Exception):
        raise outcome
    return outcome, elapsed_ms


@contextmanager
def build_workdir(saved_build: SavedBuild | None, prefix: str) -> Iterator[Path]:
    """The directory compile outputs go to: DIR when saving, else a temp dir."""
    if saved_build is not None and saved_build.save:
        saved_build.path.mkdir(parents=True, exist_ok=True)
        yield saved_build.path
    else:
        with compilation_workdir(prefix=prefix) as tmp:
            yield tmp
