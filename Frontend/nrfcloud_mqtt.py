# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause

import json
import os
import ssl
from pathlib import Path

import requests

NRF_CLOUD_API = "https://api.nrfcloud.com/v1"
MQTT_PORT = 8883


def provision_account_mqtt(api_key, creds_dir, ca_file=None, client_id=None):
    """Return (endpoint, messages_prefix, client_id, cert_file, key_file, ca_file).

    On first use, mints an account "MQTT team device" and caches
    cert.pem / key.pem / ca.pem + the clientId (in account_mqtt.json) under
    `creds_dir`. On later calls, reuses the cached files. Raises RuntimeError on
    failure (callers decide how to surface it).
    """
    auth = {"Authorization": f"Bearer {api_key}"}

    acct = requests.get(f"{NRF_CLOUD_API}/account", headers=auth, timeout=30)
    acct.raise_for_status()
    acct = acct.json()
    endpoint = acct["mqttEndpoint"]
    prefix = acct["mqttTopicPrefix"]                       # e.g. prod/<tenant>/
    messages_prefix = (acct.get("topics", {}).get("messagesPrefix")
                       or prefix + "m/")

    creds_dir = Path(creds_dir)
    creds_dir.mkdir(parents=True, exist_ok=True)
    cert_file = creds_dir / "cert.pem"
    key_file = creds_dir / "key.pem"
    ca_path = creds_dir / "ca.pem"
    meta_file = creds_dir / "account_mqtt.json"

    meta = {}
    if meta_file.exists():
        try:
            meta = json.loads(meta_file.read_text())
        except ValueError:
            pass

    # Reuse cached credentials if present.
    if cert_file.exists() and key_file.exists():
        cid = client_id or meta.get("clientId")
        if not cid:
            raise RuntimeError(
                f"Found {cert_file} and {key_file} but no clientId. nRF Cloud ties the "
                "MQTT connection to the cert's exact clientId. Put it in "
                f"{meta_file} as {{\"clientId\": \"...\"}} or pass client_id.")
        cafile = ca_file or (str(ca_path) if ca_path.exists() else None)
        return endpoint, messages_prefix, cid, str(cert_file), str(key_file), cafile

    # Mint a new account "MQTT team device" (admin API key is sufficient).
    resp = requests.post(f"{NRF_CLOUD_API}/devices/mqtt-team", headers=auth, timeout=30)
    if resp.status_code == 404:
        resp = requests.post(f"{NRF_CLOUD_API}/devices/account", headers=auth, timeout=30)
    if resp.status_code == 404:
        raise RuntimeError(
            "Could not create an account MQTT device (POST /v1/devices/mqtt-team and "
            "/v1/devices/account both 404). Export MQTT credentials from the nRF Cloud "
            f"portal to {cert_file} + {key_file} (clientId in {meta_file}) instead.")
    resp.raise_for_status()
    creds = resp.json()

    cid = creds.get("clientId") or creds.get("clientID") or client_id
    cert = creds.get("clientCert") or creds.get("certificate")
    key = creds.get("privateKey") or creds.get("private_key")
    ca = creds.get("caCert") or creds.get("ca") or creds.get("caCertificate")
    if not (cid and cert and key):
        raise RuntimeError(f"Unexpected account-device response keys: {list(creds.keys())}")

    cert_file.write_text(cert)
    key_file.write_text(key)
    meta_file.write_text(json.dumps({"clientId": cid}))   # non-secret; key stays in key.pem
    for f in (cert_file, key_file, meta_file):
        try:
            os.chmod(f, 0o600)
        except OSError:
            pass

    cafile = ca_file
    if ca and not cafile:
        ca_path.write_text(ca)
        cafile = str(ca_path)

    return endpoint, messages_prefix, cid, str(cert_file), str(key_file), cafile


def build_mqtt_client(client_id, cert_file, key_file, ca_file=None, clean_session=True):
    """Build a paho‑mqtt client configured for mutual‑TLS to the nRF Cloud broker.

    The caller sets callbacks, connects to (endpoint, MQTT_PORT) and subscribes.
    Raises ImportError if paho‑mqtt is not installed.
    """
    import paho.mqtt.client as mqtt

    try:
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                             client_id=client_id, clean_session=clean_session)
    except (AttributeError, TypeError):
        client = mqtt.Client(client_id=client_id, clean_session=clean_session)

    ctx = ssl.create_default_context()        # Amazon Root CA 1 is in the system store
    if ca_file:
        ctx.load_verify_locations(ca_file)
    ctx.load_cert_chain(cert_file, key_file)
    client.tls_set_context(ctx)
    return client
