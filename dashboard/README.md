# Sensor Dashboard

Next.js digital twin dashboard. Connects to the MQTT broker via WebSocket and displays live telemetry from the sensor node.

## Prerequisites

The gateway stack must be running before starting the dashboard:

```bash
cd ../gateway
docker compose up
```

## Setup

```bash
npm install
```

## Run (development)

```bash
npm run dev
```

Open [http://localhost:3000](http://localhost:3000).

## Configuration

| Environment variable | Default | Description |
|---------------------|---------|-------------|
| `NEXT_PUBLIC_MQTT_WS_URL` | `ws://localhost:9001` | MQTT broker WebSocket URL |

To override, create a `.env.local` file in this directory:

```
NEXT_PUBLIC_MQTT_WS_URL=ws://raspberrypi.local:9001
```

## Troubleshooting

**`next.config.ts` error on startup** — This version of Next.js does not support `.ts` config files. The config file should be `next.config.mjs`. If you see this error after a fresh checkout, rename the file.

**Dashboard shows "—" for all values** — The gateway stack is not running or the MQTT broker is unreachable. Confirm `docker compose up` is running and Mosquitto is listening on port 9001 (WebSocket).

**`(.venv)` or wrong venv activates** — The `npm run dev` command does not require a Python venv. If a venv prompt appears, it is from another project in your shell session and can be ignored.
