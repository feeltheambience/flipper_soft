#!/usr/bin/env python3
"""
VPN Keeper — Windows VPN watchdog for Flipper Zero.

Monitors the named Windows VPN connection. If it drops, tries to reconnect.
Streams status to Flipper Token Tracker FAP via CDC interface 1.

Usage:
    python keeper.py "MyVPN"
    python keeper.py "MyVPN" --port COM7
    python keeper.py "MyVPN" --stop-on-disconnect
"""
import sys
import time
import argparse
import subprocess
import platform
import serial
from serial.tools import list_ports

FLIPPER_VID = 0x0483
FLIPPER_PID = 0x5740
BAUDRATE = 115200
CHECK_INTERVAL = 5  # seconds


def find_second_flipper_port():
    ports = [p.device for p in list_ports.comports()
             if p.vid == FLIPPER_VID and p.pid == FLIPPER_PID]
    ports.sort()
    return ports[1] if len(ports) >= 2 else None


def get_vpn_status(profile):
    """Returns 'Connected' / 'Disconnected' / 'Connecting' / 'Unknown'."""
    try:
        r = subprocess.run(
            ["powershell", "-NoProfile", "-Command",
             f"(Get-VpnConnection -Name '{profile}' -ErrorAction Stop)"
             f".ConnectionStatus"],
            capture_output=True, text=True, timeout=10,
            creationflags=subprocess.CREATE_NO_WINDOW if hasattr(
                subprocess, 'CREATE_NO_WINDOW') else 0)
        out = r.stdout.strip()
        if r.returncode != 0:
            return "Unknown"
        return out or "Unknown"
    except Exception:
        return "Unknown"


def connect_vpn(profile):
    """Attempt to bring up the VPN. Tries rasdial first (saved creds),
    then PowerShell Connect-VpnConnection."""
    # rasdial works with saved credentials
    try:
        r = subprocess.run(
            ["rasdial", profile],
            capture_output=True, text=True, timeout=30,
            creationflags=subprocess.CREATE_NO_WINDOW if hasattr(
                subprocess, 'CREATE_NO_WINDOW') else 0)
        if r.returncode == 0:
            return True
        print(f"rasdial failed: {r.stdout} {r.stderr}")
    except Exception as e:
        print(f"rasdial exception: {e}")

    # Fallback: PowerShell
    try:
        r = subprocess.run(
            ["powershell", "-NoProfile", "-Command",
             f"Connect-VpnConnection -Name '{profile}' -Force"],
            capture_output=True, text=True, timeout=30,
            creationflags=subprocess.CREATE_NO_WINDOW if hasattr(
                subprocess, 'CREATE_NO_WINDOW') else 0)
        return r.returncode == 0
    except Exception as e:
        print(f"Connect-VpnConnection exception: {e}")
        return False


def main():
    if platform.system() != "Windows":
        print("This daemon supports only Windows Native VPN.")
        sys.exit(1)

    ap = argparse.ArgumentParser()
    ap.add_argument("profile", help="Name of the Windows VPN connection")
    ap.add_argument("--port", default=None,
                    help="Flipper CDC port (default: auto-detect second port)")
    ap.add_argument("--stop-on-disconnect", action="store_true",
                    help="Stop the daemon when Flipper is unplugged")
    args = ap.parse_args()

    profile = args.profile

    while True:
        port = args.port or find_second_flipper_port()
        if not port:
            print("Flipper not connected. Waiting 5s...")
            time.sleep(5)
            if args.stop_on_disconnect:
                print("--stop-on-disconnect set; exiting.")
                sys.exit(0)
            continue

        try:
            ser = serial.Serial(port, BAUDRATE, timeout=1)
        except serial.SerialException as e:
            print(f"Cannot open {port}: {e}")
            time.sleep(3)
            continue

        print(f"Connected to {port}. Watching '{profile}'.")
        last_up_tick = None
        uptime_sec = 0
        reconnects = 0

        try:
            while True:
                status = get_vpn_status(profile)
                state = "OK"
                if status == "Connected":
                    state = "OK"
                    if last_up_tick is None:
                        last_up_tick = time.time()
                    uptime_sec = int(time.time() - last_up_tick)
                else:
                    last_up_tick = None
                    uptime_sec = 0
                    if status in ("Disconnected", "Unknown"):
                        state = "RECON"
                        print(f"VPN is {status}, attempting reconnect...")
                        ok = connect_vpn(profile)
                        reconnects += 1
                        time.sleep(2)
                        # Re-check
                        status = get_vpn_status(profile)
                        if status == "Connected":
                            state = "OK"
                            last_up_tick = time.time()
                            uptime_sec = 0
                        else:
                            state = "DOWN"
                    elif status == "Connecting":
                        state = "RECON"

                line = f"VPN,{state},{profile},{uptime_sec},{reconnects}\n"
                ser.write(line.encode())
                print(line.strip())
                time.sleep(CHECK_INTERVAL)
        except (serial.SerialException, OSError) as e:
            print(f"Serial dropped: {e}. Reconnecting in 3s...")
            try:
                ser.close()
            except Exception:
                pass
            time.sleep(3)
            if args.stop_on_disconnect:
                print("--stop-on-disconnect set; exiting.")
                sys.exit(0)
        except KeyboardInterrupt:
            print("\nStopped.")
            try:
                ser.close()
            except Exception:
                pass
            sys.exit(0)


if __name__ == "__main__":
    main()
