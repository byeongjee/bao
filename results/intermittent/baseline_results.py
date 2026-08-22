"""Reference return codes: run the uninstrumented baseline of each benchmark
under continuous power and record __nvm_result (VERIFY_BUILD shrinks the same
benchmarks as INTERMITTENT_BUILD). Writes baseline.csv next to this file."""

import csv
import logging
from contextlib import closing
from pathlib import Path

from ckpt.device.otii import debugger_connection
from ckpt.device.saleae import discover_saleae
from ckpt.env import ProjectEnv
from ckpt.log import setup_logging
from ckpt.tempdir import compilation_workdir
from ckpt.toolchain import Toolchain
from ckpt.verify.common import _run_baseline

OUT = Path(__file__).parent
BENCHMARKS = [
    "aes",
    "crc",
    "rsa",
    "dijkstra",
    "qsort",
    "activity_recognition",
    "bitcount",
    "chacha20",
    "sensor_fusion",
    "poly1305",
    "cuckoo_filter",
    "sha256_fixed",
    "stringsearch",
]

setup_logging("INFO")
logger = logging.getLogger(__name__)
env = ProjectEnv.from_environ()
tc = Toolchain.resolve(env)
rows = []
with debugger_connection() as otii, closing(discover_saleae()) as saleae:
    for b in BENCHMARKS:
        bench_path = env.project_dir / "benchmarks" / "intermittent" / f"{b}.c"
        with compilation_workdir(prefix=f"baseline_{b}_") as tmp:
            result, err = _run_baseline(
                tc,
                env,
                bench_path=bench_path,
                workdir=tmp,
                cpu_freq=16_000_000,
                saleae_manager=saleae,
                otii=otii,
                capture_timeout_seconds=300.0,
            )
        logger.info("%s baseline result=%s err=%s", b, result, err)
        rows.append({"benchmark": b, "result": result or "", "error": err or ""})
with open(OUT / "baseline.csv", "w", newline="") as f:
    w = csv.DictWriter(f, fieldnames=["benchmark", "result", "error"])
    w.writeheader()
    w.writerows(rows)
