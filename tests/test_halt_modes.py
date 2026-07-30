import click
import pytest

from ckpt.cli import (
    HALT_MODES,
    bench_milp_cmd,
    bench_rockclimb_cmd,
    bench_schematic_cmd,
    bench_schematic_o3_cmd,
    compile_milp_cmd,
    compile_rockclimb_cmd,
    compile_schematic_cmd,
    compile_schematic_o3_cmd,
    verify_milp_cmd,
    verify_rockclimb_cmd,
    verify_schematic_cmd,
    verify_schematic_o3_cmd,
)


pytestmark = pytest.mark.unit

MEASUREMENT_COMMANDS = (
    compile_milp_cmd,
    compile_rockclimb_cmd,
    compile_schematic_cmd,
    compile_schematic_o3_cmd,
    bench_milp_cmd,
    bench_rockclimb_cmd,
    bench_schematic_cmd,
    bench_schematic_o3_cmd,
)

VERIFICATION_COMMANDS = (
    verify_milp_cmd,
    verify_rockclimb_cmd,
    verify_schematic_cmd,
    verify_schematic_o3_cmd,
)


def _halt_option(command: click.Command) -> click.Option:
    return next(
        parameter
        for parameter in command.params
        if isinstance(parameter, click.Option) and parameter.name == "halt_mode"
    )


@pytest.mark.parametrize("command", MEASUREMENT_COMMANDS)
def test_measurement_commands_default_to_swbor(command: click.Command):
    assert _halt_option(command).default == "swbor"


@pytest.mark.parametrize("command", VERIFICATION_COMMANDS)
def test_verification_commands_default_to_bor(command: click.Command):
    assert _halt_option(command).default == "bor"


@pytest.mark.parametrize("command", MEASUREMENT_COMMANDS + VERIFICATION_COMMANDS)
def test_public_halt_modes_exclude_nop(command: click.Command):
    halt_option = _halt_option(command)

    assert isinstance(halt_option.type, click.Choice)
    assert tuple(halt_option.type.choices) == HALT_MODES
