"""MILP-based checkpoint optimization using Gurobi."""

import sys

import gurobipy as gp
from gurobipy import GRB

from .ir_parser import CFG


class InfeasibleBlockError(Exception):
    """Raised when a basic block's energy exceeds capacity."""

    def __init__(self, blocks: list[tuple[str, int, float]]) -> None:
        """Initialize with list of (block_name, energy_cost, capacity) tuples."""
        self.blocks = blocks
        block_msgs = [
            f"  - {name}: energy={cost}, capacity={cap}"
            for name, cost, cap in blocks
        ]
        msg = "The following blocks exceed energy capacity:\n" + "\n".join(block_msgs)
        super().__init__(msg)


class CheckpointOptimizer:
    """MILP optimizer for checkpoint placement.

    Solves the checkpoint insertion problem using Gurobi to minimize
    the runtime overhead (frequency-weighted checkpoints) while ensuring
    energy between checkpoints never exceeds capacity.
    """

    def __init__(self, cfg: CFG, capacity: float) -> None:
        """Initialize the optimizer.

        Args:
            cfg: Control Flow Graph of the function.
            capacity: Maximum energy capacity between checkpoints.
        """
        self.cfg = cfg
        self.capacity = capacity
        self.model: gp.Model | None = None
        self.x: dict[str, gp.Var] = {}  # Binary: checkpoint at block b
        self.y: dict[str, gp.Var] = {}  # Continuous: energy at start of block b
        self._solved = False

    def _validate_capacity(self) -> None:
        """Check that no single block exceeds capacity.

        Raises:
            InfeasibleBlockError: If any block's energy exceeds capacity.
        """
        oversized: list[tuple[str, int, float]] = []
        for block_name in self.cfg.blocks():
            info = self.cfg.get_block_info(block_name)
            if info.energy_cost > self.capacity:
                oversized.append((block_name, info.energy_cost, self.capacity))

        if oversized:
            raise InfeasibleBlockError(oversized)

    def build_model(self) -> None:
        """Construct the Gurobi MILP model.

        Variables:
            x[b]: Binary - 1 if checkpoint at start of block b
            y[b]: Continuous [0, E_C] - accumulated energy at start of block b

        Objective:
            Minimize sum of freq(b) * x[b] for all blocks

        Constraints:
            1. Energy propagation: y[v] >= y[u] + energy[u] - M * x[v]
               for each edge (u, v)
            2. Capacity check: y[b] <= E_C * (1 - x[b]) for each block
            3. Entry block: y[entry] = 0
        """
        # Validate first
        self._validate_capacity()

        # Create model
        self.model = gp.Model("checkpoint_insertion")
        self.model.Params.OutputFlag = 0  # Suppress output by default

        blocks = self.cfg.blocks()
        edges = self.cfg.edges()

        # Compute Big-M constant
        max_energy = max(
            self.cfg.get_block_info(b).energy_cost for b in blocks
        ) if blocks else 0
        big_m = self.capacity + max_energy + 1

        # Create variables
        for block in blocks:
            info = self.cfg.get_block_info(block)
            # x[b]: binary checkpoint variable
            self.x[block] = self.model.addVar(
                vtype=GRB.BINARY,
                name=f"x_{block}",
            )
            # y[b]: continuous energy variable
            self.y[block] = self.model.addVar(
                lb=0,
                ub=self.capacity,
                vtype=GRB.CONTINUOUS,
                name=f"y_{block}",
            )

        self.model.update()

        # Objective: minimize frequency-weighted checkpoints
        objective = gp.quicksum(
            self.cfg.get_block_info(b).freq * self.x[b]
            for b in blocks
        )
        self.model.setObjective(objective, GRB.MINIMIZE)

        # Constraint 1: Energy propagation for each edge
        for u, v in edges:
            energy_u = self.cfg.get_block_info(u).energy_cost
            self.model.addConstr(
                self.y[v] >= self.y[u] + energy_u - big_m * self.x[v],
                name=f"propagate_{u}_to_{v}",
            )

        # Constraint 2: Capacity check for each block
        for block in blocks:
            self.model.addConstr(
                self.y[block] <= self.capacity * (1 - self.x[block]),
                name=f"capacity_{block}",
            )

        # Constraint 3: Entry block energy is 0
        if self.cfg.entry_block:
            self.model.addConstr(
                self.y[self.cfg.entry_block] == 0,
                name="entry_energy",
            )

        self.model.update()

    def solve(self, verbose: bool = False) -> int:
        """Solve the MILP model.

        Args:
            verbose: If True, print Gurobi solver output.

        Returns:
            Gurobi optimization status code.

        Raises:
            RuntimeError: If model hasn't been built yet.
        """
        if self.model is None:
            raise RuntimeError("Model not built. Call build_model() first.")

        if verbose:
            self.model.Params.OutputFlag = 1

        self.model.optimize()
        self._solved = True

        return self.model.Status

    def get_checkpoints(self) -> list[str]:
        """Get the list of blocks where checkpoints should be placed.

        Returns:
            List of block names that require checkpoints.

        Raises:
            RuntimeError: If model hasn't been solved or is infeasible.
        """
        if not self._solved:
            raise RuntimeError("Model not solved. Call solve() first.")

        if self.model.Status != GRB.OPTIMAL:
            raise RuntimeError(
                f"Model not optimal. Status: {self.model.Status}"
            )

        checkpoints: list[str] = []
        for block in self.cfg.blocks():
            if self.x[block].X > 0.5:  # Binary variable is 1
                checkpoints.append(block)

        return checkpoints

    def get_objective_value(self) -> float:
        """Get the objective value (total frequency-weighted checkpoint cost).

        Returns:
            The optimal objective value.

        Raises:
            RuntimeError: If model hasn't been solved optimally.
        """
        if not self._solved or self.model.Status != GRB.OPTIMAL:
            raise RuntimeError("Model not solved optimally.")
        return self.model.ObjVal

    def get_energy_levels(self) -> dict[str, float]:
        """Get the accumulated energy at the start of each block.

        Returns:
            Dict mapping block name to energy level.

        Raises:
            RuntimeError: If model hasn't been solved optimally.
        """
        if not self._solved or self.model.Status != GRB.OPTIMAL:
            raise RuntimeError("Model not solved optimally.")

        return {block: self.y[block].X for block in self.cfg.blocks()}


def optimize_checkpoints(
    cfg: CFG,
    capacity: float,
    verbose: bool = False,
) -> list[str]:
    """Convenience function to optimize checkpoint placement.

    Args:
        cfg: Control Flow Graph of the function.
        capacity: Maximum energy capacity between checkpoints.
        verbose: If True, print solver output.

    Returns:
        List of block names where checkpoints should be placed.

    Raises:
        InfeasibleBlockError: If any block exceeds capacity.
        RuntimeError: If optimization fails.
    """
    optimizer = CheckpointOptimizer(cfg, capacity)
    optimizer.build_model()
    status = optimizer.solve(verbose=verbose)

    if status != GRB.OPTIMAL:
        raise RuntimeError(f"Optimization failed with status {status}")

    return optimizer.get_checkpoints()
