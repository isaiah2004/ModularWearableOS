"""USB command framing shared by watchctl, sdsync and the checked flasher.

Only explicit read-only verbs may be replayed, after a completed resynchronization.
Legacy firmware uses DONE; v1 firmware negotiates hostcaps and req/BEGIN/DONE IDs.
Importing this module never imports serial or opens a device.
"""
from __future__ import annotations

import errno
import json
from pathlib import Path
import re
import secrets
import time


class DeviceError(OSError):
    def __init__(self, line, command="", request_id=None):
        self.line, self.command, self.request_id = line, redact(command), request_id
        parts = line.split()
        self.phase = parts[2] if len(parts) > 2 and parts[1] == "ERR" else "command"
        self.code = parts[3] if len(parts) > 3 and parts[1] == "ERR" else "EDEVICE"
        if line.startswith('ERR result '):
            self.phase = 'result'
            self.code = next((part[5:] for part in parts if part.startswith('code=')), 'EDEVICE')
        try:
            number = int(self.code, 0)
        except ValueError:
            number = getattr(errno, self.code, errno.EIO)
        super().__init__(number, redact(line))


class CommandRejected(DeviceError):
    """The device explicitly rejected command input before dispatch."""


class ProtocolError(OSError):
    pass


class CommandTimeout(TimeoutError):
    def __init__(self, command, request_id=None):
        self.command, self.request_id = redact(command), request_id
        self.ambiguous = not is_read_only(command)
        suffix = "; outcome is unknown, mutation was not replayed" if self.ambiguous else ""
        super().__init__(f"No complete response to {self.command!r}{suffix}")


def redact(text):
    private = re.match(r"^(?:req [0-9a-f]+ )?chat\s+(key|wifi)(?:\s|$)", text, re.I)
    if private:
        return "chat " + private[1].lower() + " <redacted>"
    if re.match(r"^(?:req [0-9a-f]+ )?wifi\s+", text, re.I):
        return "wifi <redacted>"
    text = re.sub(r'(?i)("?(?:password|pass|keyMaterial)"?)([=:\s]+)("(?:\\.|[^"\\])*"|\S+)',
                  r"\1\2<redacted>", text)
    return text


def validate_command(command, limit=191):
    if not isinstance(command, str):
        raise ValueError("Command must be text")
    data = command.encode("utf-8")
    if not data or len(data) > limit or command != command.lstrip() or any(c < 32 or c == 127 for c in data):
        raise ValueError(f"Use one command of 1..{limit} UTF-8 bytes without control bytes")
    return data

def jsonl_trace(path):
    """Create a lazy redacted diagnostic sink; no files until an event arrives."""
    if path is None: return None
    path = Path(path)
    def append(record):
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open('a', encoding='utf-8') as stream:
            stream.write(json.dumps(record) + '\n')
    return append


def is_read_only(command):
    # Exact zero-argument diagnostics, plus the three path-only SD readers.
    # Unknown verbs/subcommands are mutations until explicitly reviewed here.
    command = command.strip()
    if command in {"help", "hostcaps", "board", "info", "time", "prefs", "apps",
                   "sd", "sdfeatures", "imu", "chat", "chat status", "chat history", "te"}:
        return True
    return bool(re.fullmatch(r"(?:sdcrc|sdget|sdget2|sdls)(?: [^\r\n\x00]+)?", command))


def device_error(line, command="", request_id=None):
    if line.startswith("ERR command input"):
        return CommandRejected(line, command, request_id)
    if line.startswith(("ERR ", "? ")) or re.match(r"^[A-Z][A-Z0-9_]* ERR(?: |$)", line):
        return DeviceError(line, command, request_id)
    return None


class SerialLines:
    """Retain partial serial lines across timeouts, including SD base64 records."""
    block = 2048

    def __init__(self, serial_port):
        self.s, self._rx = serial_port, bytearray()

    @classmethod
    def open(cls, port, *, timeout=0.15, write_timeout=10):
        import serial
        device = serial.Serial(port=None, baudrate=115200, timeout=timeout,
                               write_timeout=write_timeout)
        device.dtr = device.rts = False
        device.port = port
        device.open()
        return cls(device)

    def write(self, data):
        count = self.s.write(data)
        if count is not None and count != len(data):
            raise ProtocolError("Short USB write; command outcome is unknown")
        if hasattr(self.s, "flush"):
            self.s.flush()

    def send(self, command):
        self.write(validate_command(command) + b"\n")

    def readline(self, timeout):
        end = time.monotonic() + max(0, timeout)
        while True:
            newline = self._rx.find(b"\n")
            if newline >= 0:
                line = bytes(self._rx[:newline])
                del self._rx[:newline + 1]
                return line.decode("utf-8", "replace").strip()
            remaining = end - time.monotonic()
            if remaining <= 0:
                return ""
            if hasattr(self.s, "read"):
                self.s.timeout = min(remaining, 0.2)
                chunk = self.s.read(max(1, min(getattr(self.s, "in_waiting", 0), 65536)))
            else:  # small serial-like test adapters
                chunk = self.s.readline()
            self._rx.extend(chunk)
            if len(self._rx) > 65536:
                raise ProtocolError("USB reply exceeds maximum line length")

    def close(self):
        self.s.close()


class HostTransport:
    def __init__(self, link, *, require_done=True, trace=None):
        self.link, self.require_done, self.trace = link, require_done, trace
        self.capabilities, self.correlated = {}, False
        self.active = None
        self.last_result = None

    def _record(self, direction, text):
        if self.trace:
            envelope = re.match(r'^(?:req|BEGIN|DONE) ([0-9a-f]{8})(?: |$)', text)
            self.trace({"monotonic": time.monotonic(), "direction": direction,
                        "line": redact(text), 'request_id': envelope.group(1) if envelope else
                        (self.active['id'] if self.active else None)})

    def negotiate(self, timeout=3):
        # A missing reply is not evidence of legacy support: fail closed. Only
        # an explicit unknown-command reply with DONE enables the old protocol.
        try:
            lines = self.execute("hostcaps", timeout=timeout)
        except DeviceError as exc:
            if not exc.line.startswith(("ERR unknown:", "? ")):
                raise
            if self.active is not None:
                raise ProtocolError('Legacy capability rejection did not complete with DONE') from exc
            return self.capabilities
        found = [line for line in lines if line.startswith("NOSHOST ")]
        if not found:
            raise ProtocolError("hostcaps completed without capabilities or explicit legacy rejection")
        self.capabilities = dict(field.split("=", 1) for field in found[-1].split()[1:] if "=" in field)
        if self.capabilities.get("v") != "1" or self.capabilities.get("req") != "1":
            raise ProtocolError("Unsupported host command protocol: " + found[-1])
        self.correlated = True
        return self.capabilities

    def begin(self, command):
        if self.active is not None:
            raise ProtocolError("Previous command is incomplete; resynchronize or reconnect before continuing")
        validate_command(command)
        request_id = secrets.token_hex(4) if self.correlated else None
        wire = f"req {request_id} {command}" if request_id else command
        validate_command(wire, int(self.capabilities.get("max_command", 191)))
        self.active = {"id": request_id, "command": command, "begun": not request_id,
                       "lines": [], "ok": None}
        self._record("tx", wire)
        self.link.send(wire)
        return request_id

    def read_payload(self, timeout):
        if self.active is None:
            raise ProtocolError("No command in flight")
        state = self.active
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            line = self.link.readline(max(0, end - time.monotonic()))
            if not line:
                continue
            self._record("rx", line)
            if state["id"]:
                if line.startswith("BEGIN "):
                    if line == "BEGIN " + state["id"]:
                        if state["begun"]:
                            raise ProtocolError("Duplicate BEGIN for active request")
                        state["begun"] = True
                    continue
                if line.startswith("DONE "):
                    fields = line.split()
                    if len(fields) < 3 or fields[1] != state["id"]:
                        continue
                    if not state["begun"] or fields[2] not in ("ok=1", "ok=0"):
                        raise ProtocolError("Invalid correlated completion: " + line)
                    state["ok"] = fields[2] == "ok=1"
                    self.active, self.last_result = None, state
                    if not state["ok"]:
                        raise DeviceError("ERR result " + " ".join(fields[2:]), state["command"], state["id"])
                    return None
                if not state["begun"]:
                    # An envelope rejected by the input parser has no BEGIN.
                    error = device_error(line, state["command"], state["id"])
                    if error:
                        raise error
                    continue
                if line == "DONE":  # delayed legacy completion cannot finish a req
                    continue
            elif line == "DONE":
                if not self.require_done:
                    continue  # BLE and protocol-only fixtures have no outer completion
                state["ok"] = True
                self.active, self.last_result = None, state
                return None
            elif line.startswith(("BEGIN ", "DONE ")):
                continue
            state["lines"].append(line)
            error = device_error(line, state["command"], state["id"])
            if error:
                raise error
            return line
        raise CommandTimeout(state["command"], state["id"])

    def reply(self, prefixes, timeout=10):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            line = self.read_payload(max(0, end - time.monotonic()))
            if line is None:
                raise ProtocolError(f"Command completed before expected {prefixes!r} reply")
            if line.startswith(prefixes):
                return line
        raise CommandTimeout(self.active["command"], self.active["id"])

    def finish(self, timeout=10):
        if not self.require_done and not self.correlated:
            self.last_result, self.active = self.active, None
            return
        end = time.monotonic() + timeout
        while self.active is not None:
            self.read_payload(max(0, end - time.monotonic()))

    def resynchronize(self, timeout=10):
        """Invalidate a partial command; never append/replay a mutation suffix."""
        self._record("tx", "<NUL> <newline> (resynchronize)")
        self.link.write(b"\x00\n")
        barrier_id = secrets.token_hex(4) if self.correlated else None
        barrier = f'req {barrier_id} hostcaps' if barrier_id else 'hostcaps'
        self._record('tx', barrier)
        self.link.send(barrier)
        end = time.monotonic() + timeout
        begun, marker = not barrier_id, False
        while time.monotonic() < end:
            line = self.link.readline(max(0, end - time.monotonic()))
            if line:
                self._record("rx", line)
            if barrier_id:
                if line == 'BEGIN ' + barrier_id: begun = True
                if begun and line.startswith('NOSHOST '): marker = True
                complete = line == f'DONE {barrier_id} ok=1' and begun and marker
            else:
                if line.startswith(('NOSHOST ', 'ERR unknown: hostcaps', '? hostcaps')): marker = True
                complete = line == 'DONE' and marker
            # A delayed DONE from either the old command or NUL cannot release
            # this barrier. Require a fresh read-only probe payload first.
            if complete:
                self.active = None
                return
        raise ProtocolError("Command resynchronization did not complete; reconnect before continuing")

    def execute(self, command, timeout=10, *, retries=0):
        if retries and not is_read_only(command):
            raise ValueError("Retries are permitted only for explicit read-only commands")
        for attempt in range(retries + 1):
            self.begin(command)
            lines = []
            try:
                end = time.monotonic() + timeout
                while self.active is not None:
                    line = self.read_payload(max(0, end - time.monotonic()))
                    if line is not None:
                        lines.append(line)
                return lines
            except (CommandTimeout, CommandRejected):
                if attempt == retries:
                    raise
                self.resynchronize()
            except DeviceError:
                # Explicit device failure is useful immediately; bounded drain
                # keeps sequential legacy negotiation safe without a 60s wait.
                try:
                    self.finish(min(timeout, 1))
                except (OSError, TimeoutError):
                    pass
                raise
