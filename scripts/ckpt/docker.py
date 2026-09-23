"""Running ckpt inside the artifact Docker image, which has the compilation
toolchain (LLVM, Gurobi, msp430-gcc) that the host does not need."""

from __future__ import annotations

import os
import subprocess
from pathlib import Path

from .errors import ConfigError

IMAGE = "bao"

# Set by the Dockerfile.
IN_DOCKER = os.environ.get("CKPT_IN_DOCKER") == "1"


def run_in_docker(args: list[str], mounts: list[Path]) -> int:
    """Run ``ckpt ARGS`` in the image and return its exit code.

    The current directory and *mounts* are mounted at the same paths, so path
    arguments mean the same inside the container.
    """
    license_file = os.environ.get("GRB_LICENSE_FILE")
    if not license_file:
        raise ConfigError("GRB_LICENSE_FILE must point to a Gurobi WLS license")
    cmd = [
        "docker",
        "run",
        "--rm",
        "--user",
        f"{os.getuid()}:{os.getgid()}",
        "-v",
        f"{license_file}:/licenses/gurobi.lic:ro",
    ]
    for path in [Path.cwd(), *mounts]:
        cmd += ["-v", f"{path}:{path}"]
    cmd += ["-w", str(Path.cwd()), IMAGE, "ckpt", *args]
    return subprocess.run(cmd, check=False).returncode
