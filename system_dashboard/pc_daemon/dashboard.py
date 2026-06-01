#!/usr/bin/env python3
"""
System Dashboard — PC-side daemon for Flipper Zero.

Sends CPU / RAM / GPU / Network metrics to Flipper via the second CDC
interface (the one not used by qFlipper RPC).

GPU usage is read via `nvidia-smi` (NVIDIA cards). If nvidia-smi is not
available, GPU shows as 0.

Usage:
    python dashboard.py            # auto-detect Flipper, use second port
    python dashboard.py COM7       # explicit port (Windows)
    python dashboard.py /dev/tty.usbmodem142103   # explicit (macOS/Linux)
"""
import os
import sys
import time
import platform
import subprocess
import serial
import psutil
from pathlib import Path
from serial.tools import list_ports

# When launched via pythonw.exe (autostart, no console) stdout is None.
# Redirect to a log file so prints don't blow up.
if sys.stdout is None or not hasattr(sys.stdout, 'write'):
    _log_path = Path.home() / ".flipper_dashboard.log"
    _log = open(_log_path, "a", buffering=1, encoding="utf-8")
    sys.stdout = _log
    sys.stderr = _log
    print(f"\n=== Started {time.strftime('%Y-%m-%d %H:%M:%S')} (pythonw mode) ===")

FLIPPER_VID = 0x0483
FLIPPER_PID = 0x5740
BAUDRATE = 115200
SAMPLE_INTERVAL = 1.0  # seconds


_gpu_warned = False


def get_gpu_pct():
    """Reads GPU utilization % via nvidia-smi. Returns 0 if unavailable."""
    global _gpu_warned
    try:
        r = subprocess.run(
            ["nvidia-smi",
             "--query-gpu=utilization.gpu",
             "--format=csv,noheader,nounits"],
            capture_output=True, text=True, timeout=2,
            creationflags=subprocess.CREATE_NO_WINDOW
            if hasattr(subprocess, 'CREATE_NO_WINDOW') else 0)
        if r.returncode == 0:
            # Multiple GPUs → take the first
            val = r.stdout.strip().split("\n")[0].strip()
            return int(val) if val.isdigit() else 0
    except FileNotFoundError:
        if not _gpu_warned:
            print("nvidia-smi not found — GPU stays at 0. Install NVIDIA "
                  "drivers or use a non-NVIDIA GPU reader.")
            _gpu_warned = True
    except Exception as e:
        if not _gpu_warned:
            print(f"GPU read error: {e}")
            _gpu_warned = True
    return 0


def find_second_flipper_port():
    """Returns path of Flipper's second CDC interface, or None."""
    ports = [p.device for p in list_ports.comports()
             if p.vid == FLIPPER_VID and p.pid == FLIPPER_PID]
    ports.sort()
    if len(ports) >= 2:
        return ports[1]
    if len(ports) == 1:
        # Single port — might mean Flipper is not in dual-CDC mode.
        print(f"Only one Flipper port found ({ports[0]}). "
              "Is System Dashboard running on Flipper?")
    return None


def stream_loop(ser):
    """Send stats until the serial errors out. Returns on disconnect."""
    last_net = psutil.net_io_counters()
    last_t = time.time()
    print("Streaming. Ctrl+C to stop.")
    while True:
        cpu = int(psutil.cpu_percent(interval=SAMPLE_INTERVAL))
        ram = int(psutil.virtual_memory().percent)
        gpu = get_gpu_pct()

        now_net = psutil.net_io_counters()
        now_t = time.time()
        dt = now_t - last_t
        delta = (now_net.bytes_sent + now_net.bytes_recv) - \
                (last_net.bytes_sent + last_net.bytes_recv)
        net_kbps = int(delta / dt / 1024) if dt > 0 else 0
        last_net, last_t = now_net, now_t

        line = f"STATS,{cpu},{ram},{gpu},{net_kbps}\n"
        try:
            ser.write(line.encode())
        except (serial.SerialException, OSError) as e:
            print(f"Write failed (Flipper likely closed app): {e}")
            return
        print(line.strip())


def main():
    explicit = sys.argv[1] if len(sys.argv) > 1 else None

    print(f"OS: {platform.system()}, baud {BAUDRATE}")
    print("Waiting for Flipper... (Ctrl+C to stop)")

    while True:
        try:
            port = explicit or find_second_flipper_port()
            if not port:
                time.sleep(2)
                continue

            print(f"Opening {port}")
            try:
                ser = serial.Serial(port, BAUDRATE, timeout=1)
            except serial.SerialException as e:
                print(f"Cannot open {port}: {e}. Retrying in 2s.")
                time.sleep(2)
                continue

            try:
                stream_loop(ser)
            finally:
                ser.close()

            print("Disconnected. Waiting for Flipper to reappear...")
            time.sleep(2)
        except KeyboardInterrupt:
            print("\nStopped.")
            return


if __name__ == "__main__":
    main()
