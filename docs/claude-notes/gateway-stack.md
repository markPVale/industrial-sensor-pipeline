# Gateway Stack — Docker Compose Architecture

## Context

The `gateway/` directory simulates the Raspberry Pi 5 production environment on a developer laptop. It provides the full data pipeline required to test firmware and dashboard integrations end-to-end without physical hardware.

---

## Services

| Service    | Image              | Host Port | Purpose                              |
|------------|--------------------|-----------|--------------------------------------|
| Mosquitto  | eclipse-mosquitto:2| 1883      | MQTT broker (firmware / mock script) |
| Mosquitto  | —                  | 9001      | MQTT over WebSocket (Next.js)        |
| InfluxDB   | influxdb:2.7       | 8086      | Time-series storage + UI             |
| Grafana    | grafana/grafana     | 3001      | Visualisation (avoids port 3000 conflict with Next.js) |

---

## Key Decisions

### Mosquitto dual-listener
Mosquitto exposes two listeners in `config/mosquitto.conf`:
- **1883** — standard MQTT TCP, used by the Python bridge, mock script, and eventually real firmware
- **9001** — MQTT over WebSocket, consumed directly by the Next.js dashboard via `mqtt.js`

This avoids a separate WebSocket server process. The dashboard subscribes to the broker directly, which is architecturally simpler and reduces latency.

### InfluxDB v2 (not v1)
InfluxDB v2 is used for its native HTTP API and organisation/bucket model. The `influxdb-client` Python library targets v2. All credentials are injected via `DOCKER_INFLUXDB_INIT_*` environment variables and applied only on first boot (init mode).

### Grafana on port 3001
Grafana's internal port (3000) is remapped to host port 3001 to prevent a collision with the Next.js development server.

### Named volumes
All three services use named Docker volumes for data persistence across `docker compose restart`. Use `docker compose down -v` only when a clean slate is required.

---

## MQTT Topic Schema

| Topic                      | Publisher       | Subscriber           | Payload                                           |
|----------------------------|-----------------|----------------------|---------------------------------------------------|
| `sensor/{nodeId}/telemetry`| Firmware / mock | Bridge, Dashboard    | `{ timestamp, vibration_rms, flags }`             |
| `sensor/{nodeId}/estop`    | Firmware / mock | Bridge, Dashboard    | `{ timestamp, triggered, reason }`                |

---

## Security Notes (Dev only)

- `allow_anonymous true` is set in Mosquitto — **not suitable for production**
- InfluxDB admin token is hardcoded as `dev-token-change-in-production`
- Grafana password is `admin`

For production: add Mosquitto `password_file`, rotate the InfluxDB token, and inject secrets via environment variables or a secrets manager.

---

## Usage

```bash
cd gateway
docker compose up -d          # start all services
docker compose logs -f        # tail logs
docker compose down           # stop (keeps volumes)
docker compose down -v        # stop + wipe volumes
```

---

## Next Steps

- [ ] Add Grafana provisioning files (`grafana/provisioning/`) to auto-configure InfluxDB datasource and default dashboards
- [ ] Add Mosquitto `password_file` and update config for authenticated mode
- [ ] Parameterise secrets via `.env` file and update `docker-compose.yml` to reference them
- [ ] Add `healthcheck` entries to services so dependent containers wait for readiness
