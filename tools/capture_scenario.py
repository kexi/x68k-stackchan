"""Send timed key events without letting serial log reads postpone them."""

import math
import time


def emit_line(text, *, output, log=None, flush=False):
    print(text, file=output, flush=flush)
    has_log = log is not None
    if has_log:
        print(text, file=log, flush=True)


def observe(port, started_at, duration, start_at, events, *, clock=time.monotonic, emit=print):
    valid_duration = math.isfinite(duration) and duration >= 0
    valid_events = all(
        math.isfinite(e["at"])
        and 0 <= e["at"] <= duration
        and isinstance(e["code"], int)
        and 0 <= e["code"] <= 255
        and e["code"] & 127
        for e in events
    )
    ordered_events = events == sorted(events, key=lambda e: e["at"])
    valid_start = start_at is None or started_at <= start_at <= started_at + duration
    if not (valid_duration and valid_events and ordered_events and valid_start):
        raise ValueError("Scenario events and START_AFTER must lie within the observation window")

    held = set()
    next_event = 0
    pending = bytearray()
    original_timeout = port.timeout
    deadline = started_at + duration
    try:
        while True:
            now = clock()
            elapsed = now - started_at
            start_due = start_at is not None and now >= start_at
            if start_due:
                port.write(b"\r")
                emit(f"Scenario: pressed RETURN elapsed_s={elapsed:.3f}", flush=True)
                start_at = None
            while next_event < len(events) and now >= started_at + events[next_event]["at"]:
                code = events[next_event]["code"]
                port.write(bytes((0x1B, code)))
                is_release = bool(code & 128)
                if is_release:
                    held.discard(code & 127)
                else:
                    held.add(code)
                emit(f"Scenario: scan={code:02X} elapsed_s={elapsed:.3f}", flush=True)
                next_event += 1
            observation_done = now >= deadline
            if observation_done:
                return bytes(pending)

            wake_at = min(deadline, now + 0.05)
            has_next_event = next_event < len(events)
            if has_next_event:
                wake_at = min(wake_at, started_at + events[next_event]["at"])
            has_start = start_at is not None
            if has_start:
                wake_at = min(wake_at, start_at)
            port.timeout = max(0, wake_at - clock())
            # readline with a short timeout splits diagnostic/ack lines; retain all fragments.
            pending.extend(port.read(min(4096, max(1, port.in_waiting))))
            while b"\n" in pending:
                raw, _, remainder = pending.partition(b"\n")
                pending = bytearray(remainder)
                line = raw.decode("utf-8", errors="replace").strip()
                has_line = bool(line)
                if has_line:
                    emit(line, flush=True)
                rejected_key = "[remote-key]" in line and "accepted=0" in line
                if rejected_key:
                    raise RuntimeError("Remote key was rejected")
    finally:
        port.timeout = original_timeout
        for code in sorted(held):
            port.write(bytes((0x1B, code | 128)))
            emit(f"Scenario: cleanup release={code:02X}", flush=True)
