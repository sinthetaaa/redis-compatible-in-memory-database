"""Owned RESP2 servers: default loopback, explicit wildcard and rejected input."""
import select
import socket
import subprocess
import sys


def check_binding(options, address):
    proc = subprocess.Popen([sys.argv[1], '--resp2-server', '--port', '0', *options],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        assert select.select([proc.stdout], [], [], 10)[0], 'readiness timeout'
        line = proc.stdout.readline()
        assert line.startswith(f'RESP_READY {address}:'.encode()), (line, proc.poll())
        port = int(line.rsplit(b':', 1)[1])
        with socket.create_connection(('127.0.0.1', port), timeout=3) as sock:
            sock.sendall(b'*1\r\n$4\r\nPING\r\n')
            assert sock.recv(64) == b'+PONG\r\n'
        proc.terminate()
        out, err = proc.communicate(timeout=5)
        assert proc.returncode == 0 and b'RESP_STOPPED' in out, (out, err)
    finally:
        if proc.poll() is None:
            proc.kill()
        proc.communicate(timeout=5)


if __name__ == '__main__':
    check_binding([], '127.0.0.1')
    check_binding(['--bind', '127.0.0.1'], '127.0.0.1')
    check_binding(['--bind', '0.0.0.0'], '0.0.0.0')
    for value in ('localhost', '::', '::1', '127.1', '127.0.0.2', '0.0.0.0 ', ''):
        result = subprocess.run([sys.argv[1], '--resp2-server', '--bind', value],
                                capture_output=True, timeout=3)
        assert result.returncode == 2, value
    for options in (['--smoke-echo', '--bind', '0.0.0.0'],
                    ['--resp2-server', '--bind', '0.0.0.0', '--bind', '127.0.0.1']):
        assert subprocess.run([sys.argv[1], *options], capture_output=True, timeout=3).returncode == 2
    print('PASS explicit bind CLI, default/wildcard TCP and graceful termination')
