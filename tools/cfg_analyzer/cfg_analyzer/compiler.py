"""Handle C to LLVM IR compilation and CFG DOT file generation."""

import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path


@dataclass
class CompilationResult:
    """Result of compiling a C file to LLVM IR."""

    ir_path: Path
    success: bool
    stderr: str


@dataclass
class LLVMTools:
    """Paths to LLVM tools."""

    clang: Path
    opt: Path


def find_llvm_tools() -> LLVMTools:
    """Locate LLVM tools (clang, opt) in system PATH or common locations."""
    clang = shutil.which("clang")
    opt = shutil.which("opt")

    if not clang:
        raise RuntimeError(
            "clang not found. Please install LLVM and ensure clang is in PATH."
        )
    if not opt:
        raise RuntimeError(
            "opt not found. Please install LLVM and ensure opt is in PATH."
        )

    return LLVMTools(clang=Path(clang), opt=Path(opt))


def compile_to_ir(
    c_file: Path,
    output_dir: Path,
    include_paths: list[Path] | None = None,
    extra_flags: list[str] | None = None,
    include_debug_info: bool = False,
) -> CompilationResult:
    """Compile C file to LLVM IR (.ll file).

    Args:
        c_file: Path to C source file
        output_dir: Directory for output .ll file
        include_paths: Additional include paths for compilation
        extra_flags: Additional compiler flags
        include_debug_info: If True, compile with -g for debug info
                           (required for per-loop bound mapping)
    """
    tools = find_llvm_tools()

    output_path = output_dir / f"{c_file.stem}.ll"

    cmd = [
        str(tools.clang),
        "-S",
        "-emit-llvm",
        "-O0",
        "-fno-discard-value-names",
        "-Wno-everything",
    ]

    # Add debug info flag for per-loop bounds support
    if include_debug_info:
        cmd.append("-g")

    # Add include paths
    if include_paths:
        for inc_path in include_paths:
            cmd.extend(["-I", str(inc_path)])

    # Add extra flags
    if extra_flags:
        cmd.extend(extra_flags)

    cmd.extend([str(c_file), "-o", str(output_path)])

    result = subprocess.run(cmd, capture_output=True, text=True)

    return CompilationResult(
        ir_path=output_path,
        success=result.returncode == 0,
        stderr=result.stderr,
    )


def generate_dot_files(ir_path: Path, output_dir: Path) -> list[Path]:
    """Run opt -passes=dot-cfg and return generated DOT files."""
    tools = find_llvm_tools()

    # Convert to absolute path and get relative path from output_dir
    ir_path = ir_path.resolve()
    output_dir = output_dir.resolve()

    # Use the file name only since we'll run from output_dir
    ir_filename = ir_path.name

    cmd = [
        str(tools.opt),
        "-passes=dot-cfg",
        "-disable-output",
        ir_filename,
    ]

    subprocess.run(cmd, cwd=str(output_dir), capture_output=True)

    # Find generated .*.dot files (LLVM uses hidden file prefix)
    dot_files = list(output_dir.glob(".*.dot"))

    return dot_files
