#!/usr/bin/env bash
# Run clang-tidy in parallel over the C++ sources.
#
# Usage:
#   scripts/run_clang_tidy.sh [BASE_REF]   # .cpp files changed since BASE_REF (default: HEAD)
#   scripts/run_clang_tidy.sh --all        # every .cpp under passes/
#
# If a header changed, falls back to --all (headers are only checked through
# the .cpp files that include them).
#
# Requires passes/build/compile_commands.json (cmake configure with
# -DCMAKE_EXPORT_COMPILE_COMMANDS=ON, which is the default here).
# Override the binary with CLANG_TIDY=/path/to/clang-tidy.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

TIDY="${CLANG_TIDY:-clang-tidy}"
BUILD_DIR="passes/build"

if [ ! -f "$BUILD_DIR/compile_commands.json" ]; then
    echo "error: $BUILD_DIR/compile_commands.json not found; run cmake first." >&2
    exit 1
fi

extra_args=()
if [ "$(uname)" = "Darwin" ]; then
    extra_args+=("--extra-arg=-isysroot$(xcrun --show-sdk-path)")
fi

all_files() {
    find passes/src passes/bb-debuginfo passes/bb-energy-analyzer -name '*.cpp'
}

if [ "${1:-}" = "--all" ]; then
    files=$(all_files)
else
    base="${1:-HEAD}"
    changed=$( (git diff --name-only --diff-filter=ACMR "$base" -- 'passes/*' &&
                git diff --cached --name-only --diff-filter=ACMR -- 'passes/*') | sort -u)
    if [ -z "$changed" ]; then
        echo "No changed files under passes/; nothing to check."
        exit 0
    fi
    if echo "$changed" | grep -q '\.h$'; then
        echo "Header changed; checking all files."
        files=$(all_files)
    else
        files=$(echo "$changed" | grep '\.cpp$' || true)
        if [ -z "$files" ]; then
            echo "No changed C++ sources; nothing to check."
            exit 0
        fi
    fi
fi

out_dir=$(mktemp -d)
trap 'rm -rf "$out_dir"' EXIT
export TIDY BUILD_DIR out_dir
export extra_args_0="${extra_args[0]:-}"

n_files=$(echo "$files" | wc -l | tr -d ' ')
echo "Running clang-tidy on $n_files file(s)..."
echo "$files" | tr '\n' '\0' | xargs -0 -P "$(getconf _NPROCESSORS_ONLN)" -n 1 bash -c '
    f="$1"
    args=()
    [ -n "$extra_args_0" ] && args+=("$extra_args_0")
    "$TIDY" -p "$BUILD_DIR" "${args[@]:-}" "$f" > "$out_dir/$(echo "$f" | tr / _).log" 2>&1 || true
' _

warnings=$(cat "$out_dir"/*.log 2>/dev/null | grep -c "warning:" || true)
if [ "$warnings" -gt 0 ]; then
    grep -h -A 3 "warning:" "$out_dir"/*.log >&2
    echo "clang-tidy: $warnings warning(s) found." >&2
    exit 1
fi
echo "clang-tidy: clean."
