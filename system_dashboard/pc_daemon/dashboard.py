#!/usr/bin/env python3
"""
System Dashboard — PC-side daemon for Flipper Zero.

Sends CPU / RAM / Disk / Network metrics to Flipper via the second CDC
interface (the one not used by qFlipper RPC).

Usage:
    python dashboard.py            # auto-detect Flipper, use second port
    python dashboard.py COM7       # explicit port (Windows)
    python dashboard.py /dev/tty.usbmodem142103   # explicit (macOS/Linux)
"""
import sys
import time
import platform
import serial
import psutil
from serial.tools import list_ports

FLIPPER_VID = 0x0483
FLIPPER_PID = 0x5740
BAUDRATE = 115200
SAMPLE_INTERVAL = 1.0  # seconds


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


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else find_second_flipper_port()
    if not port:
        print("Flipper not found. Plug it in and start System Dashboard.")
        print(f"OS: {platform.system()}")
        sys.exit(1)

    print(f"Connecting to {port} @ {BAUDRATE}")
    last_net = psutil.net_io_counters()
    last_t = time.time()

    try:
        ser = serial.Serial(port, BAUDRATE, timeout=1)
    except serial.SerialException as e:
        print(f"Cannot open {port}: {e}")
        sys.exit(1)

    print("Streaming. Ctrl+C to stop.")
    try:
        while True:
            cpu = int(psutil.cpu_percent(interval=SAMPLE_INTERVAL))
            ram = int(psutil.virtual_memory().percent)
            disk_path = "C:\\" if platform.system() == "Windows" else "/"
            disk = int(psutil.disk_usage(disk_path).percent)

            now_net = psutil.net_io_counters()
            now_t = time.time()
            dt = now_t - last_t
            delta = (now_net.bytes_sent + now_net.bytes_recv) - \
                    (last_net.bytes_sent + last_net.bytes_recv)
            net_kbps = int(delta / dt / 1024) if dt > 0 else 0
            last_net, last_t = now_net, now_t

            line = f"STATS,{cpu},{ram},{disk},{net_kbps}\n"
            ser.write(line.encode())
            print(line.strip())

    except KeyboardInterrupt:
        print("\nStopped.")
    except serial.SerialException as e:
        print(f"Serial error: {e}")
    finally:
        ser.close()


if __name__ == "__main__":
    main()
