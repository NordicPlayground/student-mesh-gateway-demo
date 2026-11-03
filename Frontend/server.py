# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause

from flask import Flask, request, jsonify, send_from_directory, Response
from flask_cors import CORS
import requests
import os
import re
import time
import json
import queue
import threading
from datetime import datetime, timezone
from dotenv import load_dotenv
load_dotenv()
from pathlib import Path

from nrfcloud_mqtt import provision_account_mqtt, build_mqtt_client

app = Flask(__name__, static_folder="static", static_url_path="/static")

CORS(
    app,
    resources={r"/api/*": {
        "origins": [
            "http://localhost:5500",
            "http://127.0.0.1:5500",
            "http://localhost:5000",
            "http://127.0.0.1:5000",
        ]
    }}
)

# NRF Cloud API configuration

API_KEY = os.getenv("NRF_CLOUD_API_KEY")
BASE_URL = "https://api.nrfcloud.com/v1"
DEVICE_ID = os.getenv("DEVICE_ID")

DELAY = 0

if not API_KEY:
    raise ValueError("NRF_CLOUD_API_KEY environment variable is not set. Please set it in a .env file or your environment.")

if not DEVICE_ID:
    raise ValueError("DEVICE_ID environment variable is not set. Please set it in a .env file or your environment.")

# Reuse one HTTPS connection to api.nrfcloud.com across requests so repeated
# calls (sending commands, polling for replies) skip the TLS handshake. Cuts
# ~100-200 ms off each round trip to the cloud.
SESSION = requests.Session()


def auth_headers():
    return {
        "Authorization": f"Bearer {API_KEY}",
        "Content-Type": "application/json",
    }


def bearer_headers():
    return {
        "Authorization": f"Bearer {API_KEY}",
    }


def safe_json(response):
    try:
        return response.json()
    except ValueError:
        return {"raw": response.text}


@app.route("/")
def index():
    return send_from_directory(".", "index.html")

@app.route("/api/health", methods=["GET"])
def health():
    return jsonify({"status": "ok"}), 200


def publish_uart_command(message):
    """Publish a UART command to the board via the nRF Cloud c2d channel.

    Returns (result_dict, http_status). Raises requests.RequestException on a
    transport failure so callers can map it to a 500.
    """
    payload = {
        "topic": f"d/{DEVICE_ID}/c2d",
        "message": {
            "appId": "uart",
            "data": message,
        },
    }

    url = f"{BASE_URL}/devices/{DEVICE_ID}/messages"

    t0 = time.perf_counter()
    response = SESSION.post(url, headers=auth_headers(), json=payload)
    # elapsed_ms = round trip from this server to the nRF Cloud REST API
    # (the frontend<->cloud leg used by the latency test).
    elapsed_ms = (time.perf_counter() - t0) * 1000.0

    try:
        body = response.json()
    except ValueError:
        body = {"raw": response.text}

    return {"status": response.status_code, "response": body, "elapsed_ms": elapsed_ms}, response.status_code


@app.route("/api/send", methods=["POST"])
def send_message():
    """Send a message to the nRF Cloud board and return the response."""
    data = request.get_json()
    message = data.get("message", "")

    if not message:
        return jsonify({"error": "Message is required"}), 400

    try:
        result, status_code = publish_uart_command(message)
        return jsonify(result), status_code
    except requests.exceptions.RequestException as e:
        return jsonify({"error": str(e)}), 500


# Accept a mesh unicast address like "0x0002" (or "2"/"0002", normalized below).
EVICT_ADDR_RE = re.compile(r"^0x[0-9a-fA-F]{1,4}$")

# Longest "gtw keyrefresh ..." line the gateway can take
KEYREFRESH_CMD_MAX = 255


def _parse_idx_list(raw_list, name):
    """Parse a list of non-negative integer key indices, de-duped and
    order-preserving. Returns (list, None) on success or (None, error) on a
    malformed entry."""
    out = []
    for raw in raw_list:
        try:
            val = int(str(raw).strip(), 10)
        except (TypeError, ValueError):
            return None, f"Invalid {name} '{raw}'; expected a non-negative integer"
        if val < 0:
            return None, f"Invalid {name} '{raw}'; expected a non-negative integer"
        if val not in out:  # de-dupe, preserve first-seen order
            out.append(val)
    return out, None


@app.route("/api/keyrefresh", methods=["POST"])
def key_refresh():
    """Start the Bluetooth Mesh Key Refresh procedure on the gateway.

    Rolls each NetKey in {"subnets": [{"net_idx": N, "addrs": ["0x0002", ...]},
    ...]} (always at least one - Key Refresh runs per subnet) across the remote
    nodes listed with it. Optionally rolls the AppKeys in {"app_idxs": [N, ...]}, each
    with its own subnet's NetKey (firmware "gtw keyrefresh
    <net_idx>[:<addr>,...][/...] [<app_idx>,...]" command). An optional
    {"evict": "0x0002"} leaves that node out of the distribution so it is
    removed from the network once the old key is revoked.
    """
    # get_json can return a non-dict (a bare JSON string/number/array/bool) for a
    # malformed body; coerce anything that isn't an object to empty so the .get
    # below can't raise AttributeError (which would surface as a 500).
    data = request.get_json(silent=True)
    if not isinstance(data, dict):
        data = {}

    # subnets is mandatory and non-empty: a Key Refresh always rolls a NetKey.
    raw_subnets = data.get("subnets", None)
    if not isinstance(raw_subnets, list) or len(raw_subnets) == 0:
        return jsonify({"error": "subnets must be a non-empty list of {net_idx, addrs}"}), 400
    subnet_specs = []
    seen_nets = set()
    for entry in raw_subnets:
        if not isinstance(entry, dict):
            return jsonify({"error": "each subnet must be an object {net_idx, addrs}"}), 400
        nets, err = _parse_idx_list([entry.get("net_idx")], "net_idx")
        if err:
            return jsonify({"error": err}), 400
        net = nets[0]
        if net in seen_nets:
            continue
        seen_nets.add(net)

        raw_addrs = entry.get("addrs", []) or []
        if not isinstance(raw_addrs, list):
            return jsonify({"error": f"addrs for net_idx {net} must be a list"}), 400
        addrs = []
        for raw in raw_addrs:
            addr = str(raw).strip().lower()
            if not addr.startswith("0x"):
                addr = "0x" + addr
            if not EVICT_ADDR_RE.match(addr):
                return jsonify({"error": f"Invalid node address '{raw}'; expected a hex unicast like 0x0002"}), 400
            # Bare hex keeps the command short; the firmware parses it as hex.
            addr = format(int(addr, 16), "x")
            if addr not in addrs:
                addrs.append(addr)
        subnet_specs.append(f"{net}:{','.join(addrs)}" if addrs else str(net))

    # app_idxs is optional: NetKeys can be rolled without any AppKey.
    raw_apps = data.get("app_idxs", []) or []
    if not isinstance(raw_apps, list):
        return jsonify({"error": "app_idxs must be a list of AppKey indices"}), 400
    app_idxs, err = _parse_idx_list(raw_apps, "app_idx")
    if err:
        return jsonify({"error": err}), 400

    command = "gtw keyrefresh " + "/".join(subnet_specs)
    if app_idxs:
        command += " " + ",".join(str(v) for v in app_idxs)

    evict = str(data.get("evict", "")).strip()
    if evict:
        if not evict.lower().startswith("0x"):
            evict = "0x" + evict
        evict = evict.lower()
        if not EVICT_ADDR_RE.match(evict):
            return jsonify({"error": "Invalid evict address; expected a hex unicast like 0x0002"}), 400
        command = f"{command} {evict}"

    # The gateway's UART line and shell command buffers are 256 bytes.
    if len(command) > KEYREFRESH_CMD_MAX:
        return jsonify({"error": f"Too many nodes for one key refresh ({len(command)} > "
                                 f"{KEYREFRESH_CMD_MAX} chars); refresh fewer subnets at a time"}), 400

    try:
        result, status_code = publish_uart_command(command)
        result["command"] = command
        return jsonify(result), status_code
    except requests.exceptions.RequestException as e:
        return jsonify({"error": str(e)}), 500



@app.route("/api/device", methods=["GET"])
def get_device_state():
    """Fetch the current device state from nRF Cloud."""
    headers = {
        "Authorization": f"Bearer {API_KEY}",
    }

    url = f"{BASE_URL}/devices/{DEVICE_ID}"

    try:
        response = SESSION.get(url, headers=headers)
        status_code = response.status_code

        try:
            body = response.json()
        except ValueError:
            body = {"raw": response.text}

        return jsonify({"status": status_code, "response": body}), 200

    except requests.exceptions.RequestException as e:
        return jsonify({"error": str(e)}), 500


MQTT_CREDS_DIR = Path(__file__).resolve().parent / ".mqtt_creds_web"

_sse_clients = []
_sse_lock = threading.Lock()

def _broadcast(event):
    data = json.dumps(event)
    with _sse_lock:
        dead = []
        for q in _sse_clients:
            try:
                q.put_nowait(data)
            except queue.Full:
                dead.append(q)
        for q in dead:
            _sse_clients.remove(q)


@app.route("/api/stream", methods=["GET"])
def stream():
    """Server-Sent Events stream of device (d2c) messages, shaped like the old
    REST items so the frontend can process them unchanged:
    {"message": {"appId", "data", ...}, "receivedAt": <iso>}."""
    def gen():
        q = queue.Queue(maxsize=1000)
        with _sse_lock:
            _sse_clients.append(q)
        try:
            yield ": connected\n\n"
            while True:
                try:
                    data = q.get(timeout=15)
                    yield f"data: {data}\n\n"
                except queue.Empty:
                    yield ": keepalive\n\n"   # comment ping keeps the connection open
        finally:
            with _sse_lock:
                if q in _sse_clients:
                    _sse_clients.remove(q)

    return Response(gen(), mimetype="text/event-stream",
                    headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"})


def _on_mqtt_message(client, userdata, msg):
    try:
        payload = json.loads(msg.payload.decode("utf-8", "replace"))
    except (ValueError, AttributeError):
        return
    item = {"message": payload,
            "receivedAt": datetime.now(timezone.utc).isoformat()}
    _broadcast(item)


def _run_mqtt():
    try:
        endpoint, messages_prefix, client_id, cert_file, key_file, ca_file = \
            provision_account_mqtt(API_KEY, MQTT_CREDS_DIR)
        sub_base = f"{messages_prefix}d/{DEVICE_ID}/d2c"

        def on_connect(c, *args):
            c.subscribe(sub_base)        # base d2c topic
            c.subscribe(sub_base + "/#")  # and any d2c subtopics
            print(f"MQTT connected; subscribed to {sub_base}")

        client = build_mqtt_client(client_id, cert_file, key_file, ca_file)
        client.on_connect = on_connect
        client.on_message = _on_mqtt_message
        client.connect(endpoint, 8883, keepalive=60)
        client.loop_forever()            # reconnects automatically after the first connect
    except ImportError:
        print("paho-mqtt not installed; real-time MQTT disabled. Run: pip install paho-mqtt")
    except Exception as e:
        print("MQTT listener disabled (real-time messages off):", e)


_mqtt_started = False
_mqtt_start_lock = threading.Lock()


def start_mqtt_once():
    global _mqtt_started
    with _mqtt_start_lock:
        if _mqtt_started:
            return
        _mqtt_started = True
    threading.Thread(target=_run_mqtt, name="mqtt-listener", daemon=True).start()


if __name__ != "__main__":
    start_mqtt_once()


def _serve(port=5000):
    #Run the dev server on a DUAL-STACK socket (IPv4 + IPv6).
    import socket
    try:
        from werkzeug.serving import ThreadedWSGIServer

        class _DualStackServer(ThreadedWSGIServer):
            def server_bind(self):
                # Accept IPv4-mapped addresses too (must be set before bind).
                try:
                    self.socket.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
                except OSError:
                    pass
                super().server_bind()

        srv = _DualStackServer("::", port, app)
        srv.serve_forever()
    except OSError as e:
        print(f"dual-stack bind failed ({e}); using default bind")
        app.run(debug=True, port=port, threaded=True, use_reloader=False)


if __name__ == "__main__":
    print("Server running at http://localhost:5000")
    start_mqtt_once()
    _serve()
