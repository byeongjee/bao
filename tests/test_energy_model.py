"""Tests for energy model."""

from src.energy_model import (
    estimate_instruction_cost,
    get_call_cost,
    set_call_cost,
)


def test_simple_arithmetic():
    """Test simple arithmetic instructions have cost 1."""
    assert estimate_instruction_cost("add") == 1
    assert estimate_instruction_cost("sub") == 1
    assert estimate_instruction_cost("and") == 1
    assert estimate_instruction_cost("or") == 1
    assert estimate_instruction_cost("xor") == 1


def test_complex_arithmetic():
    """Test complex arithmetic instructions have cost 5."""
    assert estimate_instruction_cost("mul") == 5
    assert estimate_instruction_cost("sdiv") == 5
    assert estimate_instruction_cost("udiv") == 5


def test_memory_operations():
    """Test memory operation costs."""
    assert estimate_instruction_cost("load") == 4
    assert estimate_instruction_cost("store") == 5


def test_floating_point():
    """Test floating point operations have cost 8."""
    assert estimate_instruction_cost("fadd") == 8
    assert estimate_instruction_cost("fmul") == 8
    assert estimate_instruction_cost("fdiv") == 8


def test_call_cost():
    """Test call instruction cost and configurability."""
    assert estimate_instruction_cost("call") == 10
    assert get_call_cost() == 10

    set_call_cost(20)
    assert estimate_instruction_cost("call") == 20
    assert get_call_cost() == 20

    # Reset to default
    set_call_cost(10)


def test_unknown_instruction():
    """Test unknown instructions get default cost."""
    assert estimate_instruction_cost("unknownop") == 1


def test_case_insensitive():
    """Test opcode lookup is case-insensitive."""
    assert estimate_instruction_cost("ADD") == 1
    assert estimate_instruction_cost("Load") == 4
    assert estimate_instruction_cost("STORE") == 5
