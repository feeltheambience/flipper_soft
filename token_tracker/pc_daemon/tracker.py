#!/usr/bin/env python3
"""
Token Tracker — PC-side daemon.

Reads LLM API usage from a local JSONL file (and optionally OpenAI Admin
API), aggregates by day/month, sends to Flipper Token Tracker FAP.

JSONL file format (one record per request):
    {"ts": ISO8601, "model": "...", "input": N, "output": N, "cost": USD}
"""
import os
import sys
import json
import time
import serial
import platform
from datetime import datetime, date
from pathlib import Path
from serial.tools import list_ports

FLIPPER_VID = 0x0483
FLIPPER_PID = 0x5740
BAUDRATE = 115200
PUSH_INTERVAL = 5  # seconds between updates to Flipper

BUDGET = float(os.environ.get("FLIPPER_BUDGET_USD", "20"))
TOKENS_FILE = Path(os.environ.get(
    "FLIPPER_TOKENS_FILE",
    str(Path.home() / ".flipper_tokens.jsonl")
))
OPENAI_ADMIN_KEY = os.environ.get("OPENAI_ADMIN_KEY")


def find_second_flipper_port():
    ports = [p.device for p in list_ports.comports()
             if p.vid == FLIPPER_VID and p.pid == FLIPPER_PID]
    ports.sort()
    return ports[1] if len(ports) >= 2 else None


def read_local_log():
    """Read all entries from jsonl file. Skip malformed lines."""
    if not TOKENS_FILE.exists():
        return []
    entries = []
    for line in TOKENS_FILE.read_text(encoding="utf-8", errors="ignore").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            e = json.loads(line)
            if "cost" in e and "ts" in e:
                entries.append(e)
        except json.JSONDecodeError:
            continue
    return entries


def fetch_openai_usage():
    """Optional: fetch today's usage from OpenAI."""
    if not OPENAI_ADMIN_KEY:
        return 0.0, 0
    try:
        import urllib.request
        url = "https://api.openai.com/v1/organization/usage/completions" \
              f"?start_time={int(datetime.now().replace(hour=0,minute=0,second=0).timestamp())}"
        req = urllib.request.Request(url,
            headers={"Authorization": f"Bearer {OPENAI_ADMIN_KEY}"})
        with urllib.request.urlopen(req, timeout=5) as resp:
            data = json.loads(resp.read())
            # API shape: data.data[*].results[*].amount.value
            cost = 0.0
            reqs = 0
            for bucket in data.get("data", []):
                for r in bucket.get("results", []):
                    cost += r.get("amount", {}).get("value", 0)
                    reqs += r.get("num_model_requests", 0)
            return cost, reqs
    except Exception as e:
        print(f"OpenAI fetch error: {e}")
        return 0.0, 0


def aggregate():
    """Compute today_usd, month_usd, requests_today from log + APIs."""
    entries = read_local_log()
    today = date.today()
    today_str = today.isoformat()
    month_prefix = today.strftime("%Y-%m")

    day_cost = sum(e["cost"] for e in entries if e["ts"].startswith(today_str))
    month_cost = sum(e["cost"] for e in entries if e["ts"].startswith(month_prefix))
    reqs_today = sum(1 for e in entries if e["ts"].startswith(today_str))

    if OPENAI_ADMIN_KEY:
        oai_today, oai_reqs = fetch_openai_usage()
        day_cost += oai_today
        month_cost += oai_today  # approximation; ideally fetch month-to-date
        reqs_today += oai_reqs

    return day_cost, month_cost, reqs_today


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else find_second_flipper_port()
    if not port:
        print("Flipper not found. Plug in and start Token Tracker on Flipper.")
        sys.exit(1)

    print(f"Connecting to {port} @ {BAUDRATE}")
    print(f"OS: {platform.system()}")
    print(f"Budget: ${BUDGET:.2f}/day")
    print(f"Log file: {TOKENS_FILE}")
    if OPENAI_ADMIN_KEY:
        print("OpenAI Admin API: enabled")

    try:
        ser = serial.Serial(port, BAUDRATE, timeout=1)
    except serial.SerialException as e:
        print(f"Cannot open {port}: {e}")
        sys.exit(1)

    print("Streaming. Ctrl+C to stop.")
    try:
        while True:
            day, month, reqs = aggregate()
            line = f"TOKENS,{day:.2f},{month:.2f},{BUDGET:.2f},{reqs}\n"
            ser.write(line.encode())
            print(line.strip())
            time.sleep(PUSH_INTERVAL)
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        ser.close()


if __name__ == "__main__":
    main()
