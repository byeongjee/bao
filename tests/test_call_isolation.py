"""Tests for the SCHEMATIC call-isolation pass (-passes=schematic-isolate).

Faithful port of the reference's cfg_modification.create_bb_for_function /
isolate_function_calls (cfg_modification.py:45-153) and the recursion rejection
(__init__.py:115-117). Each isolatable call to a defined function is split into
  pre -> call_entry(call) -> call_exit(empty) -> post
with call_entry/call_exit marked via metadata so the (separately-invoked) solve
pass can recognize them.
"""

import re
import subprocess

import pytest

pytestmark = pytest.mark.schematic


def _isolate(tools, ir: str, *, expect_ok: bool = True) -> subprocess.CompletedProcess:
    r = subprocess.run(
        [
            tools["opt"],
            f"-load-pass-plugin={tools['pass_lib']}",
            "-passes=schematic-isolate",
            "-S",
            "-o",
            "-",
        ],
        input=ir,
        capture_output=True,
        text=True,
        check=False,
    )
    if expect_ok:
        assert r.returncode == 0, f"schematic-isolate failed: {r.stderr}"
    return r


def _count(pattern: str, text: str) -> int:
    return len(re.findall(pattern, text))


# main calls foo (a defined, checkpoint-free leaf). foo is noinline so it
# survives to be isolated.
TWO_FUNC_IR = """\
define i32 @foo() noinline {
entry:
  ret i32 7
}

define i32 @main() {
entry:
  %x = call i32 @foo()
  %y = add i32 %x, 1
  ret i32 %y
}
"""


def test_call_isolated_and_marked(tools):
    """The call to @foo is isolated into its own block and metadata-marked."""
    out = _isolate(tools, TWO_FUNC_IR).stdout

    # The call is preserved (not deleted / not inlined).
    assert _count(r"call i32 @foo\(\)", out) == 1, out

    # Both isolation markers are present (call_entry on the call, call_exit on
    # the empty exit block's terminator).
    assert "schematic.call_entry" in out, out
    assert "schematic.call_exit" in out, out

    # @main was split: it had 1 block (entry); after isolation it must have
    # several (pre/call_entry/call_exit/post chain → >= 3 extra labels).
    main_body = re.search(r"define i32 @main\(\).*?\n\}", out, re.DOTALL)
    assert main_body, out
    labels = re.findall(r"^([\w.]+):", main_body.group(0), re.MULTILINE)
    assert len(labels) >= 3, (
        f"expected @main split into multiple blocks, got labels={labels}\n{out}"
    )


SELF_RECURSION_IR = """\
define void @r() noinline {
entry:
  call void @r()
  ret void
}

define void @main() {
entry:
  call void @r()
  ret void
}
"""


def test_direct_recursion_rejected(tools):
    """A recursive call graph is a hard error (reference __init__.py:115-117)."""
    r = _isolate(tools, SELF_RECURSION_IR, expect_ok=False)
    assert r.returncode != 0, f"expected failure on recursion, got 0\n{r.stdout}"
    assert "recursion" in r.stderr.lower(), r.stderr


SINGLE_FUNC_IR = """\
define i32 @main() {
entry:
  %a = add i32 1, 2
  ret i32 %a
}
"""


def test_no_calls_is_noop(tools):
    """A function with no isolatable calls is left structurally unchanged."""
    out = _isolate(tools, SINGLE_FUNC_IR).stdout
    # No isolation markers and no split ("ci.*") blocks were created.
    assert "schematic.call_entry" not in out, out
    assert "schematic.call_exit" not in out, out
    assert "ci." not in out, out
