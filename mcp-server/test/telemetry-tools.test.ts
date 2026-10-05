import assert from "node:assert/strict";
import test from "node:test";
import { createTelemetryTools, FluxRow } from "../src/telemetry-tools.js";

const NOW = Date.parse("2026-10-05T12:00:00Z");
const complete = (): FluxRow => ({ _time: new Date(NOW).toISOString(),
  boot_id: 7, sequence_id: 42, flags: 0, ax: 0, ay: 0, az: 9.81,
  gx: 0, gy: 0, gz: 0, vibration_rms: 9.81, window_rms: 10, data_complete: true });
const invalid = (): FluxRow => ({ _time: new Date(NOW).toISOString(),
  boot_id: 7, sequence_id: 43, flags: 0, data_complete: false,
  unavailable_fields: "ax,ay,az,gx,gy,gz,wrms", reason: "invalid_telemetry" });
function rig(vibration: FluxRow[], faults: FluxRow[] = []) {
  const queries: string[] = [];
  const tools = createTelemetryTools(async query => {
    queries.push(query);
    return query.includes('_measurement == "sensor_faults"') ? faults : vibration;
  }, "sensors", () => NOW);
  return { tools, queries };
}

test("latest retains all-invalid identity and does not filter missing measurements", async () => {
  const { tools, queries } = rig([invalid()]);
  const r = await tools.getLatestTelemetry("node01");
  assert.equal(r.sequence_id, 43);
  assert.equal(r.data_status, "unavailable");
  assert.equal(r.window_rms_mps2, null);
  assert.equal(r.ax, null);
  assert.doesNotMatch(queries[0], /exists r\.(ax|gx|vibration_rms)/);
});
test("fresh invalid evidence is online but never healthy", async () => {
  const { tools, queries } = rig([invalid()]);
  const r = await tools.getSensorHealth("node01");
  assert.equal(r.is_online, true);
  assert.equal(r.last_seen_seconds_ago, 0);
  assert.match(r.health_summary!, /DATA UNAVAILABLE/);
  assert.equal(r.data_complete, false);
  assert.equal(r.window_rms_mps2, null);
  assert.doesNotMatch(queries[0], /exists r\.vibration_rms/);
});
test("missing window RMS is not replaced by vector magnitude in any tool", async () => {
  const row = complete(); delete row.window_rms;
  row.data_complete = false; row.unavailable_fields = "wrms"; row.flags = 8;
  const { tools } = rig([row]);
  assert.equal((await tools.getLatestTelemetry("node01")).window_rms_mps2, null);
  const health = await tools.getSensorHealth("node01");
  assert.equal(health.window_rms_mps2, null);
  assert.match(health.health_summary!, /DATA UNAVAILABLE/);
  const result = await tools.getRecentAnomalies("node01", 60);
  assert.equal(result.events[0].window_rms_mps2, null);
});
test("legacy points without a window metric remain unknown rather than inferred", async () => {
  const row = complete(); delete row.window_rms; delete row.data_complete;
  const { tools } = rig([row]);
  assert.equal((await tools.getLatestTelemetry("node01")).window_rms_mps2, null);
});
test("bridge legacy fallback marked unavailable is not relabelled measured RMS", async () => {
  const row = complete(); row.data_complete = false; row.unavailable_fields = "wrms";
  const { tools } = rig([row]);
  assert.equal((await tools.getLatestTelemetry("node01")).window_rms_mps2, null);
});
test("gyro-only invalidity cannot produce an OK health summary", async () => {
  const row = complete(); delete row.gx; row.data_complete = false;
  const { tools } = rig([row]);
  const r = await tools.getSensorHealth("node01");
  assert.match(r.health_summary!, /DATA UNAVAILABLE/);
  assert.equal(r.window_rms_mps2, 10);
});
test("valid and old complete records keep healthy metrics", async () => {
  for (const legacy of [false, true]) {
    const row = complete(); if (legacy) delete row.data_complete;
    const { tools } = rig([row]);
    const r = await tools.getSensorHealth("node01");
    assert.match(r.health_summary!, /^OK/);
    assert.equal(r.window_rms_mps2, 10);
    assert.equal(r.vector_magnitude_mps2, 9.81);
  }
});
test("critical interlock and fault flags retain priority over data quality", async () => {
  const row = invalid(); row.flags = 4;
  assert.match((await rig([row]).tools.getSensorHealth("node01")).health_summary!, /E-Stop/);
  const fault = { _time: new Date(NOW).toISOString(), flags: 0x40 };
  assert.match((await rig([], [fault]).tools.getSensorHealth("node01")).health_summary!, /Sensor unavailable/);
});
test("old invalid data remains offline, and no data remains no data", async () => {
  const row = invalid(); row._time = new Date(NOW - 60000).toISOString();
  const r = await rig([row]).tools.getSensorHealth("node01");
  assert.equal(r.is_online, false); assert.match(r.health_summary!, /^OFFLINE/);
  assert.equal((await rig([]).tools.getLatestTelemetry("node01")).status, "no_data");
});
test("invalid unflagged records qualify as diagnostic events without numeric filters", async () => {
  const { tools, queries } = rig([invalid()]);
  const r = await tools.getRecentAnomalies("node01", 60);
  assert.equal(r.events[0].data_status, "unavailable");
  assert.match(queries[0], /data_complete == false/);
  assert.doesNotMatch(queries[0], /exists r\.flags and exists r\.vibration_rms/);
});
test("empty event queries do not certify healthy coverage", async () => {
  const r = await rig([]).tools.getRecentAnomalies("node01", 60);
  assert.equal(r.coverage_status, "not_assessed");
  assert.doesNotMatch(r.message!, /No anomalies detected/);
});

test("nonfinite values override a stale complete marker and never crash formatting", async () => {
  const row = complete(); row.window_rms = NaN; row.gy = Infinity;
  const { tools } = rig([row]);
  const latest = await tools.getLatestTelemetry("node01");
  assert.equal(latest.data_complete, false);
  assert.equal(latest.gy, null);
  assert.equal(latest.window_rms_mps2, null);
  assert.match((await tools.getSensorHealth("node01")).health_summary!, /DATA UNAVAILABLE/);
});

test("database errors propagate instead of becoming healthy or empty results", async () => {
  const tools = createTelemetryTools(async () => { throw new Error("database offline"); }, "sensors", () => NOW);
  await assert.rejects(tools.getLatestTelemetry("node01"), /database offline/);
  await assert.rejects(tools.getSensorHealth("node01"), /database offline/);
  await assert.rejects(tools.getRecentAnomalies("node01", 60), /database offline/);
});
