#!/usr/bin/env python3
"""Exercise the actual PE hook against a mock bridge inside the smoke prefix."""
import os
import socket
import struct
import subprocess
import threading

listener = socket.socket()
listener.bind(('127.0.0.1', 0))
listener.listen(1)
listener.settimeout(20)
records, failures = [], []

def read_exact(connection, count):
    result = b''
    while len(result) < count:
        chunk = connection.recv(count - len(result))
        if not chunk:
            raise EOFError('hook disconnected before forwarding every event')
        result += chunk
    return result

def receive():
    try:
        with listener.accept()[0] as connection:
            connection.settimeout(20)
            header = struct.unpack('<4I', read_exact(connection, 16))
            assert header == (0x50495555, 1, 1, 32), header
            assert read_exact(connection, 32) == bytes(32)
            for _ in range(16):
                header = struct.unpack('<4I', read_exact(connection, 16))
                record = struct.unpack('<IHHii', read_exact(connection, 16))
                assert header == (0x50495555, 1, record[0], 16), header
                records.append(record)
            assert connection.recv(1) == b'', 'hook forwarded an unmapped virtual key'
    except BaseException as error:
        failures.append(error)

worker = threading.Thread(target=receive)
worker.start()
environment = dict(os.environ, UUR_BRIDGE_PORT=str(listener.getsockname()[1]),
                   UUR_BRIDGE_TOKEN='0'*64, WINEDLLOVERRIDES='wevtapi=n',
                   WINEDEBUG='-all')
result = subprocess.run(['xvfb-run', '-a', 'wine', './selftest.exe',
                         '--input-forwarding'], env=environment,
                        capture_output=True, text=True, timeout=25)
worker.join(21)
listener.close()
assert not worker.is_alive(), 'mock bridge timed out'
assert not failures, failures
assert records[0] == (2, 0xffff, 0, 111, 77), records
assert records[1][0:3] == (2, 0xffff, 1), records
assert records[2] == (2, 1, 1, 0, 0), records
assert records[3] == (2, 1, 0, 0, 0), records
assert records[4:8] == [(2, 0xfffe, 0, 0, 120), (2, 0xfffe, 0, 0, -120),
                       (2, 0xfffe, 0, 1, 60), (2, 0xfffe, 0, 1, 65536)], records
assert records[8:12] == [(2, 8, 1, 0, 0), (2, 8, 0, 0, 0),
                        (2, 9, 1, 0, 0), (2, 9, 0, 0, 0)], records
assert records[12:] == [(3, 0x41, 1, 0, 0), (3, 0x41, 0, 0, 0),
                       (3, 0x111, 1, 0, 0), (3, 0x111, 0, 0, 0)], records
assert result.returncode == 0, result.stdout + result.stderr
assert 'hook input forwarding, click order, and keyboard fallback test passed' in result.stdout, result.stdout + result.stderr
print(result.stdout.strip())
