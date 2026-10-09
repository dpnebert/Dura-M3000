"""Ordinary UART safety/JSON tests; retired reboot expectations are archived in lane evidence."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "m3000_bench_control.py"


def load_module():
    spec = importlib.util.spec_from_file_location("m3000_bench_control", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def assert_raises(exc_type, fn, *args):
    try:
        fn(*args)
    except exc_type:
        return
    raise AssertionError(f"expected {exc_type.__name__}")


class FakeClock:
    def __init__(self):
        self.value = 0

    def monotonic(self):
        self.value += 0.001
        return self.value


class FakeSerial:
    def __init__(self, chunks=()):
        self.is_open = False
        self.chunks = list(chunks)
        self.events = []
        self.writes = []
        self.in_waiting = 0

    def __setattr__(self, name, value):
        if hasattr(self, "events"):
            self.events.append((name, value))
        object.__setattr__(self, name, value)

    def open(self):
        self.events.append(("open", True))
        self.is_open = True

    def close(self):
        self.events.append(("close", True))
        self.is_open = False

    def reset_input_buffer(self):
        self.events.append(("reset", True))

    def write(self, payload):
        self.writes.append(payload)
        self.events.append(("write", payload))
        return len(payload)

    def flush(self):
        self.events.append(("flush", True))

    def read(self, count):
        return self.chunks.pop(0) if self.chunks else b""


def port(**overrides):
    values = dict(vid=0x303A, pid=0x1002, serial_number="206EF1ABC344", device="COM9")
    values.update(overrides)
    return SimpleNamespace(**values)


def observed_client(chunks=(), ports=None):
    m = load_module()
    ser = FakeSerial(chunks)
    client = m.BenchClient.__new__(m.BenchClient)
    client.timeout = 0.03
    client.ser = None
    client.transcript = []
    client._clock = FakeClock()
    client._serial_mod = SimpleNamespace(Serial=mock.Mock(return_value=ser))
    client._list_ports = SimpleNamespace(comports=mock.Mock(return_value=[port()] if ports is None else ports))
    client._init_evidence()
    return m, client, ser


def test_serial_open_orders_control_lines_before_port_and_open():
    m = load_module()
    ser = FakeSerial()
    m.open_serial_safely(ser, "COM9")
    events = ser.events
    for line in ("dtr", "rts", "rtscts", "dsrdtr", "xonxoff"):
        assert events.index((line, False)) < events.index(("port", "COM9"))
        assert events.count((line, False)) == 2
        assert events.index(("open", True)) < len(events) - 5
    assert events.index(("port", "COM9")) < events.index(("open", True))


def test_open_serial_rejects_already_open_without_reconfiguration():
    m = load_module()
    ser = FakeSerial()
    ser.is_open = True
    before = list(ser.events)
    assert_raises(m.BenchError, m.open_serial_safely, ser, "COM9")
    assert ser.events == before


def test_target_filter_is_exact_and_ambiguous_targets_fail_closed():
    m = load_module()
    wrong = [port(vid=0), port(pid=0), port(serial_number="other"), SimpleNamespace(device="COM9")]
    assert m.select_target(wrong + [port()]) == "COM9"
    for targets in ([], wrong, [port(), port(device="COM10")]):
        assert_raises(m.BenchError, m.select_target, targets)


def test_missing_or_ambiguous_target_never_constructs_serial_or_writes():
    for ports in ([], [port(), port(device="COM10")]):
        m, client, ser = observed_client(ports=ports)
        assert_raises(m.BenchError, client.command, "ping")
        client._serial_mod.Serial.assert_not_called()
        assert ser.writes == []
        assert not ser.is_open


def test_ping_uses_closed_constructor_finite_io_timeouts_and_one_write():
    m, client, ser = observed_client([b'{"pong":true}\r\n> '])
    assert client.command("ping") == {"pong": True}
    client._serial_mod.Serial.assert_called_once_with(port=None, baudrate=115200, timeout=0.1, write_timeout=1.0)
    assert ser.writes == [b"ping\n"]
    assert ser.events.index(("reset", True)) < ser.events.index(("write", b"ping\n")) < ser.events.index(("flush", True))
    assert client.evidence["terminal_verdict"] == "success"
    client.close()
    assert not ser.is_open


def test_fragmented_json_and_prompt_are_reassembled():
    m, client, ser = observed_client([b'log\xff\r\n{"po', b'ng":true}\r', b'\n>', b' '])
    assert client.command("ping")["pong"] is True
    assert ser.writes == [b"ping\n"]
    assert len(client.transcript) == 1


def test_json_parser_ignores_logs_but_rejects_missing_or_multiple_objects():
    m = load_module()
    assert m.parse_json_line(['log', '{broken}', '{"pong":true}']) == {"pong": True}
    for lines in ([], ['pong'], ['[]'], ['{bad}'], ['{}', '{}']):
        assert_raises(m.BenchError, m.parse_json_line, lines)


def test_invalid_complete_response_fails_without_retry():
    for payload in (b'not-json\r\n> ', b'{}\n{}\r\n> ', b'{bad}\r\n> '):
        m, client, ser = observed_client([payload])
        assert_raises(m.BenchError, client.command, "ping")
        assert ser.writes == [b"ping\n"]
        assert client.evidence["terminal_verdict"] == "command_invalid_response"


def test_ping_rejects_negative_or_non_boolean_pong():
    for reply in ({"pong": False}, {"pong": 1}, {"ok": False}, {}):
        m, client, ser = observed_client([(json.dumps(reply) + '\r\n> ').encode()])
        assert_raises(m.BenchError, client.command, "ping")
        assert ser.writes == [b"ping\n"]


def test_ping_timeout_fails_closed_without_retry():
    m, client, ser = observed_client([b'partial\xff'])
    assert_raises(m.BenchError, client.command, "ping")
    assert ser.writes == [b"ping\n"]
    assert client.evidence["terminal_verdict"] == "command_timeout"
    assert client._list_ports.comports.call_count == 1


def test_receive_overflow_fails_closed_with_bounded_evidence():
    import base64
    m, client, ser = observed_client()
    ser.chunks = [b'x' * (m.MAX_RAW_RX + 1), b'\n{"pong":true}\r\n> ']
    assert_raises(m.BenchError, client.command, "ping")
    rx = [e for e in client.evidence["events"] if 'raw_rx_b64' in e][-1]
    assert len(base64.b64decode(rx['raw_rx_b64'])) <= m.MAX_RAW_RX
    assert rx['raw_rx_truncated'] is True
    assert ser.writes == [b"ping\n"]


def test_short_write_fails_without_retry_or_read():
    m, client, ser = observed_client([b'{"pong":true}\r\n> '])
    ser.write = mock.Mock(return_value=2)
    ser.read = mock.Mock(side_effect=AssertionError("must not read after short write"))
    assert_raises(m.BenchError, client.command, "ping")
    ser.write.assert_called_once_with(b"ping\n")
    ser.read.assert_not_called()


def test_io_failure_fails_closed_without_retry():
    m, client, ser = observed_client()
    ser.write = mock.Mock(side_effect=OSError("disconnected"))
    assert_raises(m.BenchError, client.command, "ping")
    ser.write.assert_called_once_with(b"ping\n")
    assert client.evidence['terminal_verdict'] == 'command_io_error'


def test_cli_ping_json_and_cleanup_on_success_and_failure():
    m = load_module()
    for result in ({"pong": True}, m.BenchError("timeout")):
        client = mock.Mock()
        if isinstance(result, Exception):
            client.command.side_effect = result
        else:
            client.command.return_value = result
        output = io.StringIO()
        with mock.patch.object(m, 'BenchClient', return_value=client), mock.patch('sys.argv', [str(SCRIPT), 'ping']), contextlib.redirect_stdout(output):
            status = m.main()
        body = json.loads(output.getvalue())
        assert status == (2 if isinstance(result, Exception) else 0)
        assert body == ({"ok": False, "error": "timeout"} if status else result)
        client.command.assert_called_once_with('ping')
        client.close.assert_called_once_with()


def run_tests(namespace, marker):
    tests = sorted((name, fn) for name, fn in namespace.items() if name.startswith('test_') and callable(fn))
    failures = []
    for name, test in tests:
        try:
            test()
            print(f'PASS {name}')
        except Exception as exc:
            failures.append(name)
            print(f'FAIL {name}: {type(exc).__name__}: {exc}')
    print(f'{marker} {len(tests)-len(failures)}/{len(tests)}')
    return int(bool(failures))


if __name__ == '__main__':
    raise SystemExit(run_tests(globals(), 'M3000_UART_TRANSPORT_TEST_PASS'))
