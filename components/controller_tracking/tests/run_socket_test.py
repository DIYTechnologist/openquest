#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Bounded service integration test; works on Linux / Android with a privileged test UID."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

if os.getuid() not in (0,1000):
    raise SystemExit('Socket credential test requires root or system UID; run via sudo in CI.')
with tempfile.TemporaryDirectory(prefix='oq-socket-') as directory:
    root=Path(directory)
    subprocess.run([sys.executable,str(Path(__file__).with_name('generate_config.py')),str(root)],check=True)
    endpoint=root/'tracking.sock'
    process=subprocess.Popen([sys.argv[1],str(root/'cameras.json'),str(root/'model.json'),str(root/'gyro.json'),str(endpoint)])
    try:
        deadline=time.monotonic()+5
        while not endpoint.exists():
            if process.poll() is not None:raise RuntimeError('Service exited before binding')
            if time.monotonic()>deadline:raise TimeoutError('Service did not bind')
            time.sleep(.02)
        subprocess.run([sys.argv[2],str(endpoint)],check=True,timeout=15)
    finally:
        process.terminate()
        try:process.wait(timeout=3)
        except subprocess.TimeoutExpired:process.kill();process.wait()
