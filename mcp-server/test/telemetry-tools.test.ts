import assert from "node:assert/strict";
import test from "node:test";
import { createTelemetryTools, FluxRow } from "../src/telemetry-tools.js";
import { QueryError } from "../src/tool-utils.js";

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

function partialRig(vibration: FluxRow[] | Error, faults: FluxRow[] | Error) {
  return createTelemetryTools(async query => {
    const value = query.includes('_measurement == "sensor_faults"') ? faults : vibration;
    if (value instanceof Error) throw value;
    return value;
  }, "sensors", () => NOW);
}

test("fault query failure preserves recent reporting while health and absent flags stay unknown", async () => {
  const tools = partialRig([complete()], new QueryError("QUERY_TIMEOUT"));
  const r = await tools.getSensorHealth("node01");
  assert.equal(r.query_status, "partial");
  assert.equal(r.assessment_complete, false);
  assert.equal(r.is_online, true);
  assert.match(r.health_summary, /^UNKNOWN/);
  assert.equal(r.window_rms_mps2, 10);
  assert.equal(r.data_complete, true);
  assert.equal(r.data_status, "complete");
  assert.deepEqual(r.unavailable_fields, []);
  assert.equal(r.flag_sensor_fault, null);
  assert.equal(r.sources.vibration.timestamp, new Date(NOW).toISOString());
  assert.equal(r.sources.vibration.age_seconds, 0);
  assert.equal(r.sources.sensor_faults.error?.code, "QUERY_TIMEOUT");
});

test("partial fault evidence retains positive flags without inventing healthy telemetry", async () => {
  const fault = { _time: new Date(NOW - 10000).toISOString(), flags: 0x40, whoami_raw: 255 };
  const r = await partialRig(new Error("database failed"), [fault]).getSensorHealth("node01");
  assert.equal(r.flag_sensor_unavailable, true);
  assert.equal(r.flag_interlock_open, null);
  assert.equal(r.is_online, true);
  assert.equal(r.window_rms_mps2, null);
  assert.equal(r.data_complete, null);
  assert.equal(r.data_status, null);
  assert.equal(r.unavailable_fields, null);
  assert.equal(r.reason, null);
  assert.equal(r.sources.sensor_faults.age_seconds, 10);
  assert.match(r.health_summary, /^UNKNOWN/);
});

test("empty successful source with a failed source is unknown, never offline", async () => {
  const cases: [FluxRow[] | Error, FluxRow[] | Error][] = [
    [[], new QueryError("QUERY_TIMEOUT")], [new QueryError("QUERY_BUSY"), []],
  ];
  for (const sources of cases) {
    const r = await partialRig(sources[0], sources[1]).getSensorHealth("node01");
    assert.equal(r.is_online, null);
    assert.equal(r.assessment_complete, false);
    assert.match(r.health_summary, /^UNKNOWN/);
  }
});

test("partial anomaly results preserve events and declare missing source coverage", async () => {
  const r = await partialRig([invalid()], new QueryError("QUERY_TIMEOUT")).getRecentAnomalies("node01", 60);
  assert.equal(r.query_status, "partial");
  assert.equal(r.query_complete, false);
  assert.equal(r.events.length, 1);
  assert.equal(r.sources.sensor_faults.error?.code, "QUERY_TIMEOUT");
  assert.equal(r.coverage_status, "not_assessed");
});

test("partial empty anomaly results do not claim no matching events", async () => {
  const r = await partialRig([], new Error("failed")).getRecentAnomalies("node01", 60);
  assert.equal(r.query_status, "partial");
  assert.equal(r.returned_event_count, 0);
  assert.match(r.message!, /unavailable source/);
  assert.doesNotMatch(r.message!, /No flagged/);
});

test("old positive evidence cannot establish offline status while another source is missing", async () => {
  const row = complete(); row._time = new Date(NOW - 60000).toISOString();
  const fault = { _time: new Date(NOW - 60000).toISOString(), flags: 0x40 };
  for (const tools of [partialRig([row], new QueryError("QUERY_TIMEOUT")),
    partialRig(new QueryError("QUERY_TIMEOUT"), [fault])]) {
    const r = await tools.getSensorHealth("node01");
    assert.equal(r.is_online, null);
    assert.equal(r.query_status, "partial");
    assert.equal(r.assessment_complete, false);
    assert.equal(r.last_seen_seconds_ago, 60);
    assert.match(r.health_summary, /^UNKNOWN/);
  }
});

test("query status is consistent across tools and independent of data presence or health", async () => {
  const tools = rig([]).tools;
  const latest = await tools.getLatestTelemetry("node01");
  assert.equal(latest.query_status, "ok");
  assert.equal(latest.status, "no_data"); // legacy data-presence status
  const health = await tools.getSensorHealth("node01");
  assert.equal(health.query_status, "ok");
  assert.equal(health.is_online, false);
  assert.match(health.health_summary, /^OFFLINE/);
  assert.equal(health.data_complete, null);
  assert.equal(health.data_status, null);
  assert.equal(health.unavailable_fields, null);
  const anomalies = await tools.getRecentAnomalies("node01", 60);
  assert.equal(anomalies.query_status, "ok");
  assert.equal((await rig([complete()]).tools.getLatestTelemetry("node01")).query_status, "ok");
  assert.equal("status" in health, false);
  assert.equal("status" in anomalies, false);
});

test("successful vibration query with no rows leaves quality unassessed", async () => {
  const fault = { _time: new Date(NOW).toISOString(), flags: 0x40 };
  const r = await rig([], [fault]).tools.getSensorHealth("node01");
  assert.equal(r.query_complete, true);
  assert.equal(r.sources.vibration.status, "no_data");
  assert.equal(r.data_status, null);
  assert.equal(r.data_complete, null);
  assert.equal(r.unavailable_fields, null);
  assert.equal(r.window_rms_mps2, null);
  assert.equal(r.is_online, true);
  assert.equal(r.flag_sensor_unavailable, true);
});

test("invalid telemetry stays invalid when only the fault query fails", async () => {
  const r = await partialRig([invalid()], new QueryError("QUERY_TIMEOUT")).getSensorHealth("node01");
  assert.equal(r.data_status, "unavailable");
  assert.equal(r.data_complete, false);
  assert.deepEqual(r.unavailable_fields, ["ax", "ay", "az", "gx", "gy", "gz", "wrms"]);
  assert.equal(r.is_online, true);
});

test("query completion, measurement completeness, and observation age are independent", async () => {
  const row = invalid(); row._time = new Date(NOW - 60000).toISOString();
  const r = await rig([row]).tools.getSensorHealth("node01");
  assert.equal(r.query_complete, true);
  assert.equal(r.assessment_complete, true);
  assert.equal(r.data_complete, false);
  assert.equal(r.sources.vibration.age_seconds, 60);
  assert.equal(r.queried_at, new Date(NOW).toISOString());
});

test("partial results attribute queue expiry to local capacity, whichever source expired", async () => {
  const fault = { _time: new Date(NOW).toISOString(), flags: 0x40 };
  for (const tools of [
    partialRig([complete()], new QueryError("QUERY_QUEUE_TIMEOUT")),
    partialRig(new QueryError("QUERY_QUEUE_TIMEOUT"), [fault]),
  ]) {
    const health = await tools.getSensorHealth("node01");
    assert.equal(health.query_status, "partial");
    assert.equal(health.assessment_complete, false);
    assert.equal(health.is_online, true);
    assert.equal(health.dependency_status.influxdb, "available");
    assert.equal(health.dependency_status.query_service, "busy");
    assert.match(health.health_summary, /^UNKNOWN/);
    const anomalies = await tools.getRecentAnomalies("node01", 60);
    assert.equal(anomalies.query_status, "partial");
    assert.equal(anomalies.dependency_status.influxdb, "available");
    assert.equal(anomalies.dependency_status.query_service, "busy");
    assert.equal(anomalies.events.length, 1);
  }
});
