"""Energy cost estimation for LLVM IR instructions."""

# Energy cost lookup table for LLVM IR opcodes
# Costs are in abstract energy units
INSTRUCTION_COSTS: dict[str, int] = {
    # Arithmetic (simple) - cost 1
    "add": 1,
    "sub": 1,
    "and": 1,
    "or": 1,
    "xor": 1,
    "shl": 1,
    "lshr": 1,
    "ashr": 1,
    # Arithmetic (complex) - cost 5
    "mul": 5,
    "sdiv": 5,
    "udiv": 5,
    "srem": 5,
    "urem": 5,
    # Floating point - cost 8
    "fadd": 8,
    "fsub": 8,
    "fmul": 8,
    "fdiv": 8,
    "frem": 8,
    # Memory operations
    "load": 4,
    "store": 5,
    "alloca": 2,
    # Control flow - cost 1
    "br": 1,
    "switch": 1,
    "ret": 1,
    "unreachable": 0,
    # Comparison - cost 1
    "icmp": 1,
    "fcmp": 1,
    # Conversions - cost 2
    "trunc": 2,
    "zext": 2,
    "sext": 2,
    "fptrunc": 2,
    "fpext": 2,
    "fptoui": 2,
    "fptosi": 2,
    "uitofp": 2,
    "sitofp": 2,
    "ptrtoint": 2,
    "inttoptr": 2,
    "bitcast": 1,
    "addrspacecast": 2,
    # Function call - cost 10 (configurable)
    "call": 10,
    "invoke": 10,
    # PHI and Select - cost 1
    "phi": 1,
    "select": 1,
    # GEP - cost 2
    "getelementptr": 2,
    # Atomics and other
    "atomicrmw": 6,
    "cmpxchg": 8,
    "fence": 2,
    "extractelement": 1,
    "insertelement": 1,
    "shufflevector": 2,
    "extractvalue": 1,
    "insertvalue": 1,
    "landingpad": 1,
    "resume": 1,
    "freeze": 1,
}

# Default cost for unknown instructions
DEFAULT_COST = 1


def estimate_instruction_cost(opcode: str) -> int:
    """Return energy cost for an LLVM opcode.

    Args:
        opcode: The LLVM instruction opcode (e.g., 'add', 'mul', 'load').

    Returns:
        Integer energy cost in abstract units.
    """
    return INSTRUCTION_COSTS.get(opcode.lower(), DEFAULT_COST)


def get_call_cost() -> int:
    """Return the energy cost for a function call instruction."""
    return INSTRUCTION_COSTS["call"]


def set_call_cost(cost: int) -> None:
    """Set the energy cost for function call instructions.

    Args:
        cost: New energy cost for call/invoke instructions.
    """
    INSTRUCTION_COSTS["call"] = cost
    INSTRUCTION_COSTS["invoke"] = cost
