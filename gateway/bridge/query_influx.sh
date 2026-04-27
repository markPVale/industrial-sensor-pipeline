#!/bin/bash
# Query the last 5 minutes of the sensors bucket to verify InfluxDB writes.
# Run from the Pi (or any host with access to port 8086).
# Non-empty output confirms the bridge pipeline is writing correctly.
curl -s -X POST "http://localhost:8086/api/v2/query?org=industrial" -H "Authorization: Token dev-token-change-in-production" -H "Content-Type: application/vnd.flux" -d 'from(bucket:"sensors") |> range(start: -5m) |> limit(n:3)'
