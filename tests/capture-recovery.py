#!/usr/bin/env python3
"""Remove the test transports from Linux, just as the Portal worker does."""
import pathlib
import sys
import time

directory = pathlib.Path(sys.argv[1])
request = directory / "uur-selftest-remove-request"
deadline = time.monotonic() + 25
while not request.exists():
    if time.monotonic() >= deadline:
        raise TimeoutError("Wine did not request transport removal")
    time.sleep(0.01)
for name in ("uur-selftest-frames.bin", "uur-selftest-frames.bin.1"):
    (directory / name).unlink()
(directory / "uur-selftest-remove-complete").touch()
