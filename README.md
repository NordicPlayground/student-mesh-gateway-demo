# nRF Cloud Gateway for Bluetooth Mesh Network

> **Important:** This repository does not contain production-ready features. This demo is experimental, has known limitations, and may contain significant bugs. It is not intended as a reference architecture or as implementation guidance for a Bluetooth Mesh gateway. For production systems, design your own architecture and address security and operational requirements for your use case.

This demonstration is jointly created by students Mathias Schiøtz, Ali Amlashi, Amund Grønli and Håvard Barlindhaug as a part of their bachelor degree project and summer job. The project demonstrates an edge-to-cloud solution integrating a Bluetooth Mesh network with Nordic Semiconductor's cellular IoT and cloud platforms. The system bridges a local Bluetooth Mesh network of smart nodes (like smart lights) via an nRF54L15 Bluetooth Gateway and an nRF9151 Cellular Gateway up to nRF Cloud, where the data and controls are accessed via a responsive web application.

## System Architecture

The architecture consists of four main components seamlessly connected:

1. **Mesh Light Endpoints (`light_ctrl` / `light_dimmer`)**:
   - The Bluetooth Mesh endpoints operating within the local mesh network: a dimmable light fixture (`light_ctrl`) and a switch/dimmer (`light_dimmer`).
   - Provisioned and controlled over Bluetooth Mesh; auto-detected by the gateway from Composition Data.
2. **Bluetooth Gateway (`nRF54L15_Gateway`)**:
   - Acts as "main node" in the mesh network.
   - Extracts data from the Bluetooth Mesh network and translates it to UART data to communicate with the cellular gateway.
3. **Cellular Gateway (`nRF9151_Gateway`)**:
   - A cellular IoT device that connects to the internet via LTE/NB-IoT.
   - Receives data over UART from the nRF54L15 Bluetooth Gateway and forwards it directly to **nRF Cloud** using MQTT.
   - Distributes downlink messages (from the web/cloud) over UART back to the mesh gateway.
4. **Cloud Website (`Frontend`)**:
   - A Flask-based back-end and vanilla JS/HTML/CSS web front-end. Commands are sent to nodes via the nRF Cloud REST API; device-to-cloud messages are streamed to the browser in real time over a dedicated MQTT connection.
   - Allows users to monitor the state of the mesh network and remotely send commands directly to the nodes from anywhere.

### Data Flow
`Bluetooth Mesh Nodes` <--> `nRF54L15 (BT Gateway)` <-- UART --> `nRF9151 (Cellular Gateway)` <-- LTE/MQTT --> `nRF Cloud` <-- REST API --> `Flask Web Dashboard`

---

## Hardware Requirements

- **nRF9151 DK**  for the Cellular part of the Gateway.
- **nRF54L15 DK** for the Bluetooth Mesh part of the Gateway.
- **nRF54L15 DK** for the Mesh Nodes.
- Jumper wires to physically connect the UART TX/RX/GND pins between the nRF54L15 and nRF9151 DKs.

## Software Dependencies

- **nRF Connect SDK (NCS)** (Zephyr RTOS) for building the firmware applications.
- **Python 3** for running the `Frontend` Flask server.

## Component Overview

### 1. Mesh Light Endpoints (`light_ctrl` / `light_dimmer`)
The Bluetooth Mesh endpoints are Nordic's NLC samples: **light_ctrl** (a dimmable light fixture — Lightness, Scene and Sensor servers) and **light_dimmer** (a switch/dimmer ), found under `<ncs>/nrf/samples/bluetooth/mesh/` in the nRF Connect SDK. The gateway auto-detects each device by its Composition Data and configures it accordingly. The earlier generic `Mesh_node` application is no longer supported.

Before building these samples, apply `mesh_nodes.patch` (in the repo root) to your NCS checkout — it adds a Remote Provisioning Server model to both samples' primary element, which the gateway's auto-provisioning/replace features rely on to provision nodes that are out of the provisioner's direct radio range:
```bash
cd <ncs>/nrf
git apply /path/to/student-mesh-demo/mesh_nodes.patch
```

### 2. Bluetooth Gateway (`/nRF54L15_Gateway`)
Runs on an nRF54L15 DK. It translates Bluetooth Mesh data into a UART protocol, communicating locally with the cellular nRF9151 gateway. Ensure you have properly declared UART pins connected appropriately.
- **Debugging (RTT)**: To view debug logs for the nRF54L15, use SEGGER J-Link RTT Viewer or the nRF Connect Serial Terminal configured for RTT. Connect to the nRF54L15 DK via the onboard J-Link (SWD) to view real-time traces and Bluetooth Mesh activity without occupying the UART pins used for communication with the nRF9151.

### 3. Cellular Gateway (`/nRF9151_Gateway`)
Runs on an nRF9151 DK and takes the UART data to forward it to nRF Cloud using MQTT device messages.
- You must onboard your device to an **[nRF Cloud](https://nrfcloud.com/)** account.
**Debugging**> To view debug logs for nRF9151 use nRF Connect serial terminal and select nRF9151 DK
### 4. Cloud Dashboard (`/Frontend`)
A Python/Flask project that serves a vanilla JS/HTML/CSS web app. It bridges user inputs via the nRF Cloud REST API to remotely toggle nodes and reads real-time device data over a MQTT subscription.
- **Setup**:
  ```bash
  cd Frontend
  pip install -r requirements.txt
  ```
- **Environment config**: Create a `.env` file in the folder with the following:
  ```env
  NRF_CLOUD_API_KEY=your_nrf_cloud_api_key_here
  DEVICE_ID=your_flashed_device_id_here
  ```
  *(To switch boards, just change `DEVICE_ID` in `.env`)*
- **Run**:
  ```bash
  python server.py
  ```
  Then access the application interface from your browser (typically `http://localhost:5000`).

---

