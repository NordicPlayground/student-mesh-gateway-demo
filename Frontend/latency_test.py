# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause

#!/usr/bin/env python3
"""Barebones nRF Cloud latency test.

Sends N "latency <id>" pings to the device (cloud-to-device) and times each
round trip until the device's reply comes back over MQTT in real time. Prints
each RTT and a min/avg/max summary.

Only option:  -n <count>   (number of pings, default 10)

Needs NRF_CLOUD_API_KEY in .env. Replies are received over the nRF Cloud account
MQTT endpoint (a credential is minted/cached in .mqtt_creds/ on first run).
"""

import argparse
import json
import os
import random
import re
import statistics
import sys
import threading
import time
from pathlib import Path

import requests

try:
    from dotenv import load_dotenv
    load_dotenv()
except ImportError:
    pass

from nrfcloud_mqtt import provision_account_mqtt, build_mqtt_client, NRF_CLOUD_API, MQTT_PORT

API_KEY = os.getenv("NRF_CLOUD_API_KEY")
DEVICE_ID = os.getenv("DEVICE_ID")
CREDS_DIR = Path(__file__).resolve().parent / ".mqtt_creds"   # always next to this script
REPLY_TIMEOUT = 30.0   # seconds to wait for each reply
PING_INTERVAL = 1.0    # seconds between pings


class ReplyListener:
    """Subscribes to the account MQTT stream and fires an event when a
    'latency <id>' reply arrives, matched by id."""

    def __init__(self):
        self._lock = threading.Lock()
        self._events = {}      # id -> threading.Event
        self._arrived = set()  # ids already received
        self._client = None

    def start(self):
        endpoint, messages_prefix, client_id, cert, key, ca = \
            provision_account_mqtt(API_KEY, CREDS_DIR)
        topic = messages_prefix + "#"
        client = build_mqtt_client(client_id, cert, key, ca)
        client.on_connect = lambda c, *a: c.subscribe(topic)
        client.on_message = self._on_message
        client.connect(endpoint, MQTT_PORT, keepalive=60)
        client.loop_start()
        self._client = client

    def _on_message(self, client, userdata, msg):
        try:
            payload = json.loads(msg.payload.decode("utf-8", "replace"))
        except (ValueError, AttributeError):
            return
        if payload.get("appId") != "latency":
            return
        m = re.match(r"\s*(\d+)", str(payload.get("data", "")))
        if not m:
            return
        pid = int(m.group(1))
        with self._lock:
            self._arrived.add(pid)
            ev = self._events.get(pid)
        if ev:
            ev.set()

    def wait(self, pid, timeout):
        with self._lock:
            if pid in self._arrived:
                return True
            ev = threading.Event()
            self._events[pid] = ev
        return ev.wait(timeout)

    def stop(self):
        if self._client:
            try:
                self._client.loop_stop()
                self._client.disconnect()
            except Exception:
                pass


def send_ping(pid):
    """Publish a c2d 'latency <id>' command to the device via nRF Cloud."""
    requests.post(
        f"{NRF_CLOUD_API}/devices/{DEVICE_ID}/messages",
        headers={"Authorization": f"Bearer {API_KEY}", "Content-Type": "application/json"},
        json={"topic": f"d/{DEVICE_ID}/c2d",
              "message": {"appId": "uart", "data": f"latency {pid}"}},
        timeout=30,
    ).raise_for_status()


def main():
    ap = argparse.ArgumentParser(description="Barebones nRF Cloud latency test.")
    ap.add_argument("-n", "--count", type=int, default=10, help="number of pings (default: 10)")
    args = ap.parse_args()

    if not API_KEY:
        print("error: set NRF_CLOUD_API_KEY in .env")
        return 2

    if not DEVICE_ID:
        print("error: set DEVICE_ID in .env")
        return 2

    listener = ReplyListener()
    try:
        listener.start()
    except ImportError:
        print("error: pip install paho-mqtt")
        return 2
    except RuntimeError as e:
        print(f"error: {e}")
        return 2

    print(f"Latency test: {args.count} pings via nRF Cloud")
    rtts = []
    try:
        for i in range(1, args.count + 1):
            pid = random.randint(1, 2_000_000_000)
            t0 = time.perf_counter()
            try:
                send_ping(pid)
            except requests.exceptions.RequestException as e:
                print(f"  [{i:>3}] send error: {e}")
                continue

            if listener.wait(pid, REPLY_TIMEOUT):
                rtt = (time.perf_counter() - t0) * 1000.0
                rtts.append(rtt)
                print(f"  [{i:>3}] id={pid} rtt={rtt:.0f} ms")
            else:
                print(f"  [{i:>3}] id={pid} TIMEOUT")

            if i < args.count:
                time.sleep(PING_INTERVAL)
    finally:
        listener.stop()

    if not rtts:
        print("\n  no replies")
        return 1

    print(f"\n  {len(rtts)}/{args.count} ok   "
          f"min {min(rtts):.0f}   avg {statistics.mean(rtts):.0f}   max {max(rtts):.0f}   ms")
    return 0


if __name__ == "__main__":
    sys.exit(main())
