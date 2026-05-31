#!/usr/bin/env python3
"""
Pomodoro Sync — PC-side daemon for Flipper Zero.

Listens to phase notifications from Flipper and shows native OS
desktop notifications (osascript / notify-send / winotify).

Usage:
    python pomodoro.py                            # auto-detect
    python pomodoro.py COM7                       # explicit (Windows)
    python pomodoro.py /dev/tty.usbmodem...       # explicit (macOS/Linux)
"""
import sys
import time
import platform
import subprocess
import serial
from serial.tools import list_ports

FLIPPER_VID = 0x0483
FLIPPER_PID = 0x5740
BAUDRATE = 115200
SYSTEM = platform.system()


def find_second_flipper_port():
    ports = [p.device for p in list_ports.comports()
             if p.vid == FLIPPER_VID and p.pid == FLIPPER_PID]
    ports.sort()
    if len(ports) >= 2:
        return ports[1]
    if len(ports) == 1:
        print(f"Only one Flipper port found ({ports[0]}). "
              "Is Pomodoro Sync running on Flipper?")
    return None


def notify(title, message):
    """Cross-platform desktop notification."""
    print(f"[NOTIFY] {title}: {message}")
    try:
        if SYSTEM == "Darwin":
            subprocess.run(
                ["osascript", "-e",
                 f'display notification "{message}" with title "{title}"'],
                check=False)
        elif SYSTEM == "Linux":
            subprocess.run(["notify-send", title, message], check=False)
        elif SYSTEM == "Windows":
            try:
                from winotify import Notification, audio
                n = Notification(app_id="Pomodoro Sync",
                                 title=title, msg=message)
                n.set_audio(audio.Default, loop=False)
                n.show()
            except ImportError:
                # Fallback: just print
                pass
    except Exception as e:
        print(f"Notify error: {e}")


def handle(line):
    if line.startswith("PHASE,WORK"):
        notify("Pomodoro", "Work time — 25 minutes. Focus.")
    elif line.startswith("PHASE,BREAK"):
        notify("Pomodoro", "Break — 5 minutes. Step away.")
    elif line.startswith("PHASE,PAUSE"):
        notify("Pomodoro", "Paused")
    elif line.startswith("PHASE,RESUME"):
        notify("Pomodoro", "Resumed")
    elif line.startswith("PHASE,STOP"):
        notify("Pomodoro", "Stopped")


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else find_second_flipper_port()
    if not port:
        print("Flipper not found. Plug it in and start Pomodoro Sync.")
        sys.exit(1)

    print(f"Listening on {port} @ {BAUDRATE} (OS: {SYSTEM})")
    try:
        ser = serial.Serial(port, BAUDRATE, timeout=1)
    except serial.SerialException as e:
        print(f"Cannot open {port}: {e}")
        sys.exit(1)

    try:
        while True:
            line = ser.readline().decode(errors="ignore").strip()
            if line:
                print(f"<- {line}")
                handle(line)
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        ser.close()


if __name__ == "__main__":
    main()
