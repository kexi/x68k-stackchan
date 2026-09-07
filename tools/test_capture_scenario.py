"""Guarantees for reproducible input while serial diagnostics arrive intermittently."""

import io
import unittest

from capture_scenario import emit_line, observe


class FakePort:
    def __init__(self, chunks=()):
        self.timeout = 1
        self.now = 100.0
        self.chunks = list(chunks)
        self.writes = []
        self.reads = 0

    @property
    def in_waiting(self):
        ready = bool(self.chunks) and self.chunks[0][0] <= self.now
        return len(self.chunks[0][1]) if ready else 0

    def read(self, count):
        self.reads += 1
        stalled = self.reads > 1000
        if stalled:
            raise AssertionError("Observation loop failed to advance")
        wake_at = self.now + self.timeout
        ready = bool(self.chunks) and self.chunks[0][0] <= wake_at
        if not ready:
            self.now = wake_at
            return b""
        at, data = self.chunks.pop(0)
        self.now = max(self.now, at)
        has_remainder = len(data) > count
        if has_remainder:
            self.chunks.insert(0, (at, data[count:]))
        return data[:count]

    def write(self, data):
        self.writes.append((self.now - 100, data))
        return len(data)


class ScenarioTests(unittest.TestCase):
    def test_log_copy_preserves_lines_without_changing_console_output(self):
        output, log = io.StringIO(), io.StringIO()
        emit_line("[event] value=1", output=output, log=log, flush=True)
        emit_line("[event] value=2", output=output, log=log)
        self.assertEqual(output.getvalue(), "[event] value=1\n[event] value=2\n")
        self.assertEqual(log.getvalue(), output.getvalue())

    def test_log_is_optional(self):
        output = io.StringIO()
        emit_line("line", output=output)
        self.assertEqual(output.getvalue(), "line\n")

    def run_scenario(self, port, events, duration=1, start_at=None):
        self.lines = []
        return observe(
            port,
            100,
            duration,
            start_at,
            events,
            clock=lambda: port.now,
            emit=lambda text, **kwargs: self.lines.append(text),
        )

    def test_silent_serial_preserves_press_hold_and_end_boundary(self):
        port = FakePort()
        events = [{"at": 0.1, "code": 0x25}, {"at": 0.6, "code": 0xA5}]
        self.run_scenario(port, events, duration=0.6, start_at=100.3)
        self.assertEqual([data for _, data in port.writes], [b"\x1b%", b"\r", b"\x1b\xa5"])
        for (actual, _), expected in zip(port.writes, [0.1, 0.3, 0.6], strict=True):
            self.assertAlmostEqual(actual, expected)
        self.assertEqual(port.timeout, 1)

    def test_split_utf8_and_ack_lines_remain_whole_and_tail_is_returned(self):
        port = FakePort(
            [(100.1, b"[remote-"), (100.2, b"key] accepted=1\n\xe9"), (100.3, b"\x9f\xb3\ntail")]
        )
        tail = self.run_scenario(port, [])
        self.assertEqual(self.lines, ["[remote-key] accepted=1", "音"])
        self.assertEqual(tail, b"tail")

    def test_split_rejection_releases_held_keys_and_restores_timeout(self):
        port = FakePort([(100.2, b"[remote-key] accepted="), (100.3, b"0\n")])
        with self.assertRaisesRegex(RuntimeError, "rejected"):
            self.run_scenario(port, [{"at": 0, "code": 0x20}])
        self.assertEqual([data for _, data in port.writes], [b"\x1b ", b"\x1b\xa0"])
        self.assertEqual(port.timeout, 1)

    def test_unreleased_keys_are_released_at_window_end(self):
        port = FakePort()
        self.run_scenario(port, [{"at": 0, "code": 0x20}])
        self.assertAlmostEqual(port.writes[-1][0], 1)
        self.assertEqual(port.writes[-1][1], b"\x1b\xa0")

    def test_invalid_scenarios_do_not_send_input(self):
        for events in [
            [{"at": -1, "code": 32}],
            [{"at": 2, "code": 32}],
            [{"at": 0, "code": 128}],
            [{"at": 0, "code": 256}],
            [{"at": 0.8, "code": 32}, {"at": 0.2, "code": 160}],
        ]:
            with self.subTest(events=events):
                port = FakePort()
                with self.assertRaises(ValueError):
                    self.run_scenario(port, events)
                self.assertEqual(port.writes, [])


if __name__ == "__main__":
    unittest.main()
