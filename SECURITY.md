# Security

This repository is an **experimental demo**, not production firmware or cloud software.

## Demo-specific risks

- The Flask dashboard (`Frontend/server.py`) exposes control and mesh-management
  APIs without authentication when you run it locally or on a network.
- Credentials belong in a local `.env` file (never commit `.env` or MQTT private keys).
