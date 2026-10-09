"""Retirement regressions, replacing reboot-specific requirements (see evidence mapping)."""
import contextlib
import io
from unittest import mock
from test_m3000_bench_automation import ROOT, SCRIPT, load_module, assert_raises, run_tests


def test_retired_reboot_method_never_opens_enumerates_reads_or_writes():
    m = load_module()
    client = m.BenchClient.__new__(m.BenchClient)
    client.ser = mock.Mock(is_open=True)
    client.open = mock.Mock(side_effect=AssertionError('must not open'))
    client.command = mock.Mock(side_effect=AssertionError('must not ping'))
    client._list_ports = mock.Mock()
    assert_raises(m.BenchError, client.reboot)
    client.open.assert_not_called()
    client.command.assert_not_called()
    assert client.ser.mock_calls == []
    assert client._list_ports.mock_calls == []


def test_non_ping_commands_rejected_before_any_transport_side_effect():
    m = load_module()
    for command in ('reboot', 'restart', 'REBOOT', ' reboot ', 'ping\nreboot', 'ping\rrestart', '', 'whoami'):
        for opened in (False, True):
            client = m.BenchClient.__new__(m.BenchClient)
            client.ser = mock.Mock(is_open=opened)
            client.open = mock.Mock(side_effect=AssertionError('must not open'))
            client._list_ports = mock.Mock()
            assert_raises(m.BenchError, client.command, command)
            client.open.assert_not_called()
            assert client.ser.mock_calls == []
            assert client._list_ports.mock_calls == []


def test_cli_reboot_restart_and_retired_options_reject_before_client_creation():
    m = load_module()
    for args in (['reboot'], ['restart'], ['ping', '--expected-reset-reason', '3'], ['ping', '--evidence-path', 'unused.json']):
        with mock.patch.object(m, 'BenchClient') as constructor, mock.patch('sys.argv', [str(SCRIPT)] + args), contextlib.redirect_stderr(io.StringIO()):
            try:
                m.main()
            except SystemExit as exc:
                assert exc.code == 2
            else:
                raise AssertionError('retired command/options accepted')
            constructor.assert_not_called()


def test_invalid_timeout_rejects_before_importing_serial():
    m = load_module()
    for timeout in (0, -1, float('nan'), float('inf')):
        with mock.patch('builtins.__import__', side_effect=AssertionError('must not load transport')):
            assert_raises(m.BenchError, m.BenchClient, timeout)


def test_application_uart_has_no_reboot_commands_gate_or_restart_hooks():
    app = (ROOT / 'main/app_main.c').read_text()
    for forbidden in ('esp_restart', 'deferred_restart', 'DURA_DEV_BENCH_COMMANDS', '"reboot"', '"restart"'):
        assert forbidden not in app
    assert 'config DURA_DEV_BENCH_COMMANDS' not in (ROOT / 'main/Kconfig.projbuild').read_text()
    start = app.index('if (strcmp(command, "help")')
    help_block = app[start:app.index('return ESP_OK;', start)]
    assert 'reboot' not in help_block and 'restart' not in help_block


def test_firmware_ordinary_uart_submit_response_flush_and_timeout_remain():
    app = (ROOT / 'main/app_main.c').read_text()
    serial = app[app.index('static void serial_console_task'):app.index('static void start_serial_console')]
    assert 'dura_submit_command(DURA_COMMAND_SOURCE_UART' in serial
    assert 'pdMS_TO_TICKS(DURA_APP_COMMAND_TIMEOUT_MS)' in serial
    ack = serial.index('printf("\\r\\n%s\\r\\n> ", response')
    assert serial.index('fflush(stdout);', ack) > ack
    assert 'error: unknown command' in serial
    assert 'release_deferred_restart' not in serial


def test_active_generic_ping_and_debug_gate_remain():
    generic = (ROOT / 'components/gama_blefob/gama_blefob_commands.c').read_text()
    assert 'strcasecmp(cmd, "ping") == 0' in generic
    assert 's_cfg.ping_debug_enabled' in generic
    assert 'ble_shell_notify_response("pong")' in generic
    assert 'error: ping debug mode disabled' in generic
    app = (ROOT / 'main/app_main.c').read_text()
    assert 'gama_blefob_dispatch_text' in app


def test_retired_orchestration_helpers_are_absent():
    m = load_module()
    for name in ('validate_fresh_boot', 'RESTART_SETTLE_SECONDS', 'READY_MARKER'):
        assert not hasattr(m, name)
    for name in ('_wait_for_post_restart_ready', '_wait_for_target', '_record_optional_enumeration'):
        assert not hasattr(m.BenchClient, name)


if __name__ == '__main__':
    raise SystemExit(run_tests(globals(), 'UART_REBOOT_RETIREMENT_TEST_PASS'))
