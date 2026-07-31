"""Unit tests for Saleae timing extraction and launch command wiring."""

from __future__ import annotations

import sys
import types
from pathlib import Path
from typing import Self
from unittest.mock import MagicMock

import pytest
from ckpt.device.saleae import _extract_timing, _wait_for_capture, saleae_run
from ckpt.errors import DeviceError

pytestmark = pytest.mark.unit


def write_capture_csv(tmp_path: Path, rows: list[tuple[float, int]]) -> Path:
    """Write a minimal Saleae CSV with timestamp/value rows."""
    csv_path = tmp_path / "digital.csv"
    lines = capture_csv_lines(rows)
    csv_path.write_text("".join(lines))
    return csv_path


def capture_csv_lines(rows: list[tuple[float, int]]) -> list[str]:
    """Return CSV lines for a minimal Saleae digital capture."""
    lines = ["Time [s],Channel 0\n"]
    for timestamp, value in rows:
        lines.append(f"{timestamp:.9f},{value}\n")
    return lines


class TestExtractTiming:
    def test_single_start_and_stop(self, tmp_path: Path):
        csv_path = write_capture_csv(
            tmp_path,
            [
                (0.000000000, 0),
                (0.001000000, 1),
                (0.001010000, 0),
                (0.379125720, 1),
                (0.384125720, 0),
            ],
        )

        assert _extract_timing(csv_path) == pytest.approx(378115.72)

    def test_multiple_start_pulses_before_stop_is_ambiguous(self, tmp_path: Path):
        csv_path = write_capture_csv(
            tmp_path,
            [
                (0.000000000, 0),
                (0.001000000, 1),
                (0.001010000, 0),
                (0.168091450, 1),
                (0.168101450, 0),
                (0.379125720, 1),
                (0.384125720, 0),
            ],
        )

        with pytest.raises(DeviceError, match="Ambiguous Saleae capture"):
            _extract_timing(csv_path)

    def test_short_pulses_after_stop_are_ignored(self, tmp_path: Path):
        csv_path = write_capture_csv(
            tmp_path,
            [
                (0.000000000, 0),
                (0.001000000, 1),
                (0.001010000, 0),
                (0.379125720, 1),
                (0.384125720, 0),
                (0.500000000, 1),
                (0.500010000, 0),
            ],
        )

        assert _extract_timing(csv_path) == pytest.approx(378115.72)


class FakeCapture:
    def __init__(self, csv_lines: list[str]):
        self.csv_lines = csv_lines

    def __enter__(self) -> Self:
        return self

    def __exit__(self, exc_type: object, exc: object, tb: object) -> bool:
        del exc_type, exc, tb
        return False

    def wait(self) -> None:
        return None

    def stop(self) -> None:
        return None

    def export_raw_data_csv(self, directory: str, digital_channels: list[int]) -> None:
        del digital_channels
        Path(directory, "digital.csv").write_text("".join(self.csv_lines))


class FakeManager:
    def __init__(self, capture_rows: list[list[tuple[float, int]]]):
        self.capture_rows = capture_rows
        self.calls = 0

    def start_capture(
        self, device_configuration: object, capture_configuration: object
    ) -> FakeCapture:
        del device_configuration, capture_configuration
        csv_lines = capture_csv_lines(self.capture_rows[self.calls])
        self.calls += 1
        return FakeCapture(csv_lines)


class FakeFlakyManager:
    def __init__(
        self,
        failures_before_success: int,
        capture_rows: list[tuple[float, int]],
        error_message: str,
    ):
        self.failures_before_success = failures_before_success
        self.capture_rows = capture_rows
        self.error_message = error_message
        self.calls = 0

    def start_capture(
        self, device_configuration: object, capture_configuration: object
    ) -> FakeCapture:
        del device_configuration, capture_configuration
        self.calls += 1
        if self.calls <= self.failures_before_success:
            raise RuntimeError(self.error_message)
        return FakeCapture(capture_csv_lines(self.capture_rows))


def install_fake_saleae_automation(monkeypatch: pytest.MonkeyPatch) -> None:
    """Install a stub ``saleae.automation`` module for pure unit tests."""
    fake_automation = types.ModuleType("saleae.automation")

    class CaptureConfiguration:
        def __init__(self, capture_mode: object):
            self.capture_mode = capture_mode

    class DigitalTriggerCaptureMode:
        def __init__(
            self,
            trigger_type: object,
            trigger_channel_index: int,
            min_pulse_width_seconds: float,
            max_pulse_width_seconds: float,
            after_trigger_seconds: float,
        ):
            self.trigger_type = trigger_type
            self.trigger_channel_index = trigger_channel_index
            self.min_pulse_width_seconds = min_pulse_width_seconds
            self.max_pulse_width_seconds = max_pulse_width_seconds
            self.after_trigger_seconds = after_trigger_seconds

    class DigitalTriggerType:
        PULSE_HIGH = "pulse_high"

    class LogicDeviceConfiguration:
        def __init__(
            self, enabled_digital_channels: list[int], digital_sample_rate: int
        ):
            self.enabled_digital_channels = enabled_digital_channels
            self.digital_sample_rate = digital_sample_rate

    fake_automation.CaptureConfiguration = CaptureConfiguration
    fake_automation.DigitalTriggerCaptureMode = DigitalTriggerCaptureMode
    fake_automation.DigitalTriggerType = DigitalTriggerType
    fake_automation.LogicDeviceConfiguration = LogicDeviceConfiguration

    fake_saleae = types.ModuleType("saleae")
    fake_saleae.__path__ = []
    fake_saleae.automation = fake_automation

    monkeypatch.setitem(sys.modules, "saleae", fake_saleae)
    monkeypatch.setitem(sys.modules, "saleae.automation", fake_automation)
    monkeypatch.setattr(
        "ckpt.device.saleae._wait_for_capture",
        lambda capture, timeout_seconds: capture.wait(),
    )


class TestSaleaeRun:
    def test_retries_ambiguous_capture_and_returns_clean_measurement(
        self,
        monkeypatch: pytest.MonkeyPatch,
    ):
        install_fake_saleae_automation(monkeypatch)
        flash_calls: list[tuple[Path, int]] = []

        def fake_flash(elf_path: Path, timeout: int) -> None:
            flash_calls.append((elf_path, timeout))

        monkeypatch.setattr("ckpt.device.saleae.flash", fake_flash)

        manager = FakeManager(
            [
                [
                    (0.000000000, 0),
                    (0.001000000, 1),
                    (0.001010000, 0),
                    (0.168091450, 1),
                    (0.168101450, 0),
                    (0.379125720, 1),
                    (0.384125720, 0),
                ],
                [
                    (0.000000000, 0),
                    (0.001000000, 1),
                    (0.001010000, 0),
                    (0.379125720, 1),
                    (0.384125720, 0),
                ],
            ]
        )

        result = saleae_run(Path("/tmp/app.elf"), manager, 30, 1.0, 60.0)

        assert result == pytest.approx(378115.72)
        assert flash_calls == [
            (Path("/tmp/app.elf"), 30),
            (Path("/tmp/app.elf"), 30),
        ]
        assert manager.calls == 2

    def test_retries_start_capture_failure_before_flashing(
        self,
        monkeypatch: pytest.MonkeyPatch,
    ):
        install_fake_saleae_automation(monkeypatch)
        flash_calls: list[tuple[Path, int]] = []
        sleep_calls: list[float] = []

        def fake_flash(elf_path: Path, timeout: int) -> None:
            flash_calls.append((elf_path, timeout))

        def fake_sleep(seconds: float) -> None:
            sleep_calls.append(seconds)

        monkeypatch.setattr("ckpt.device.saleae.flash", fake_flash)
        monkeypatch.setattr("ckpt.device.saleae.time.sleep", fake_sleep)

        manager = FakeFlakyManager(
            failures_before_success=1,
            capture_rows=[
                (0.000000000, 0),
                (0.001000000, 1),
                (0.001010000, 0),
                (0.379125720, 1),
                (0.384125720, 0),
            ],
            error_message="No physical devices found",
        )

        result = saleae_run(Path("/tmp/app.elf"), manager, 30, 1.0, 60.0)

        assert result == pytest.approx(378115.72)
        assert flash_calls == [(Path("/tmp/app.elf"), 30)]
        assert sleep_calls == [0.5]
        assert manager.calls == 2

    def test_wraps_terminal_start_capture_failure_as_device_error(
        self,
        monkeypatch: pytest.MonkeyPatch,
    ):
        install_fake_saleae_automation(monkeypatch)
        sleep_calls: list[float] = []

        def fake_sleep(seconds: float) -> None:
            sleep_calls.append(seconds)

        monkeypatch.setattr("ckpt.device.saleae.time.sleep", fake_sleep)

        manager = FakeFlakyManager(
            failures_before_success=3,
            capture_rows=[
                (0.000000000, 0),
                (0.001000000, 1),
                (0.001010000, 0),
                (0.379125720, 1),
                (0.384125720, 0),
            ],
            error_message="No physical devices found",
        )

        with pytest.raises(
            DeviceError, match="Cannot start Saleae capture after 3 attempts"
        ):
            saleae_run(Path("/tmp/app.elf"), manager, 30, 1.0, 60.0)

        assert sleep_calls == [0.5, 0.5]
        assert manager.calls == 3

    def test_timeout_stops_and_closes_capture(
        self,
        monkeypatch: pytest.MonkeyPatch,
    ):
        install_fake_saleae_automation(monkeypatch)
        monkeypatch.setattr("ckpt.device.saleae.flash", lambda elf_path, timeout: None)

        deadline_exceeded = object()

        class RpcError(Exception):
            def code(self) -> object:
                return deadline_exceeded

        fake_grpc = types.ModuleType("grpc")
        fake_grpc.RpcError = RpcError
        fake_grpc.StatusCode = types.SimpleNamespace(
            DEADLINE_EXCEEDED=deadline_exceeded
        )
        request = object()
        fake_saleae_grpc = types.ModuleType("saleae.grpc")
        fake_saleae_grpc.saleae_pb2 = MagicMock()
        fake_saleae_grpc.saleae_pb2.WaitCaptureRequest.return_value = request
        monkeypatch.setitem(sys.modules, "grpc", fake_grpc)
        monkeypatch.setitem(sys.modules, "saleae.grpc", fake_saleae_grpc)
        monkeypatch.setattr("ckpt.device.saleae._wait_for_capture", _wait_for_capture)

        capture = MagicMock()
        capture.__enter__.return_value = capture
        capture.capture_id = 7
        capture.manager.stub.WaitCapture.side_effect = RpcError()
        manager = MagicMock()
        manager.start_capture.return_value = capture

        with pytest.raises(DeviceError, match=r"timed out after 0\.01 seconds"):
            saleae_run(Path("/tmp/app.elf"), manager, 30, 1.0, 0.01)

        capture.manager.stub.WaitCapture.assert_called_once_with(request, timeout=0.01)
        capture.stop.assert_called_once_with()
        capture.__exit__.assert_called_once()
        capture.export_raw_data_csv.assert_not_called()
