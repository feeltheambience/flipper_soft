#!/usr/bin/env python3
"""
BLE VPN Button — PC-side daemon (Windows).

Connects to Flipper Zero via BLE (you must pair first via OS Bluetooth
settings). Listens for ASCII commands: TOGGLE / UP / DOWN. Drives the
named Windows VPN connection via rasdial / PowerShell.

Usage:
    python button_daemon.py "MyVPN"
    python button_daemon.py "MyVPN" --name "Flipper ABCD"
"""
import sys
import asyncio
import argparse
import subprocess
import platform
from bleak import BleakScanner, BleakClient

FLIPPER_NAME_PREFIX = "Flipper"


def run_ps(cmd, timeout=15):
    try:
        return subprocess.run(
            ["powershell", "-NoProfile", "-Command", cmd],
            capture_output=True, text=True, timeout=timeout,
            creationflags=subprocess.CREATE_NO_WINDOW
            if hasattr(subprocess, 'CREATE_NO_WINDOW') else 0)
    except Exception as e:
        print(f"PS error: {e}")
        return None


def vpn_status(profile):
    r = run_ps(f"(Get-VpnConnection -Name '{profile}' "
               f"-ErrorAction Stop).ConnectionStatus")
    if r and r.returncode == 0:
        return r.stdout.strip()
    return "Unknown"


def vpn_connect(profile):
    try:
        r = subprocess.run(["rasdial", profile], capture_output=True,
                           text=True, timeout=30,
                           creationflags=subprocess.CREATE_NO_WINDOW
                           if hasattr(subprocess, 'CREATE_NO_WINDOW') else 0)
        if r.returncode == 0:
            print(f"VPN UP: {profile}")
            return True
    except Exception:
        pass
    r = run_ps(f"Connect-VpnConnection -Name '{profile}' -Force", timeout=30)
    return r and r.returncode == 0


def vpn_disconnect(profile):
    try:
        r = subprocess.run(["rasdial", profile, "/disconnect"],
                           capture_output=True, text=True, timeout=15,
                           creationflags=subprocess.CREATE_NO_WINDOW
                           if hasattr(subprocess, 'CREATE_NO_WINDOW') else 0)
        if r.returncode == 0:
            print(f"VPN DOWN: {profile}")
            return True
    except Exception:
        pass
    r = run_ps(f"Disconnect-VpnConnection -Name '{profile}' -Force", timeout=15)
    return r and r.returncode == 0


def handle_command(cmd, profile):
    cmd = cmd.strip().upper()
    print(f"<- {cmd}")
    if cmd == "TOGGLE":
        st = vpn_status(profile)
        print(f"   current status: {st}")
        if st == "Connected":
            vpn_disconnect(profile)
        else:
            vpn_connect(profile)
    elif cmd == "UP":
        vpn_connect(profile)
    elif cmd == "DOWN":
        vpn_disconnect(profile)
    else:
        print(f"   unknown command: {cmd}")


async def find_flipper(name_filter):
    print("Scanning for Flipper... (5s)")
    devices = await BleakScanner.discover(timeout=5.0)
    for d in devices:
        if d.name and (
            (name_filter and d.name == name_filter) or
            (not name_filter and d.name.startswith(FLIPPER_NAME_PREFIX))
        ):
            print(f"Found: {d.name} [{d.address}]")
            return d
    return None


async def discover_serial_char(client):
    """Find a characteristic that supports notify — that's our TX (Flipper -> PC)."""
    for svc in client.services:
        for char in svc.characteristics:
            if "notify" in char.properties or "indicate" in char.properties:
                print(f"  notify char: {char.uuid} in svc {svc.uuid}")
                # Heuristic: pick the first notify-capable characteristic
                return char
    return None


async def session(device, profile):
    async with BleakClient(device) as client:
        if not client.is_connected:
            print("Failed to connect.")
            return
        print(f"Connected to {device.name}")

        char = await discover_serial_char(client)
        if not char:
            print("No notify characteristic found — wrong profile?")
            return

        rx_buf = bytearray()

        def on_notify(_sender, data):
            rx_buf.extend(data)
            while b"\n" in rx_buf:
                idx = rx_buf.index(b"\n")
                line = rx_buf[:idx].decode("utf-8", errors="ignore")
                del rx_buf[:idx + 1]
                if line:
                    handle_command(line, profile)

        await client.start_notify(char, on_notify)
        print("Listening for commands. Ctrl+C to stop.")

        try:
            while client.is_connected:
                await asyncio.sleep(1.0)
        except asyncio.CancelledError:
            pass
        finally:
            try:
                await client.stop_notify(char)
            except Exception:
                pass


async def main(profile, name_filter):
    while True:
        try:
            device = await find_flipper(name_filter)
            if device is None:
                print("Flipper not found. Retrying in 5s...")
                await asyncio.sleep(5)
                continue
            await session(device, profile)
            print("Disconnected. Reconnecting in 3s...")
            await asyncio.sleep(3)
        except KeyboardInterrupt:
            break
        except Exception as e:
            print(f"Session error: {e}. Retrying in 5s...")
            await asyncio.sleep(5)


if __name__ == "__main__":
    if platform.system() != "Windows":
        print("This daemon supports only Windows Native VPN control.")
        sys.exit(1)

    ap = argparse.ArgumentParser()
    ap.add_argument("profile", help="Windows VPN connection name")
    ap.add_argument("--name", default=None,
                    help="Exact Flipper BLE name (default: any 'Flipper *')")
    args = ap.parse_args()

    try:
        asyncio.run(main(args.profile, args.name))
    except KeyboardInterrupt:
        print("\nStopped.")
