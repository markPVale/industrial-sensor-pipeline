import assert from "node:assert/strict";
import test from "node:test";
import {
  FLAG_ANOMALY,
  FLAG_INTERLOCK_OPEN,
  FLAG_SENSOR_UNAVAILABLE,
  InputError,
  MAX_ANOMALY_EVENTS,
  buildAnomalyQueries,
  capAnomalyEvents,
  classifyDependencyError,
  decodeFlags,
  normalizeWindowMinutes,
  parseNodeId,
} from "../src/tool-utils.js";

test("parseNodeId applies the default and accepts valid IDs", () => {
  assert.equal(parseNodeId(undefined), "node01");
  assert.equal(parseNodeId(null), "node01");
  assert.equal(parseNodeId("line_1-sensor"), "line_1-sensor");
});

test("parseNodeId rejects invalid IDs and non-string values", () => {
  for (const value of ["", "bad id", 'node" or true', {}, 42]) {
    assert.throws(() => parseNodeId(value), InputError);
  }
});

test("normalizeWindowMinutes defaults, rounds, and clamps numeric values", () => {
  assert.equal(normalizeWindowMinutes(undefined), 60);
  assert.equal(normalizeWindowMinutes(null), 60);
  assert.equal(normalizeWindowMinutes(10.6), 11);
  assert.equal(normalizeWindowMinutes(-20), 1);
  assert.equal(normalizeWindowMinutes(99_999), 1440);
});

test("normalizeWindowMinutes rejects non-finite and non-number values", () => {
  for (const value of ["abc", true, Number.NaN, Infinity, -Infinity]) {
    assert.throws(() => normalizeWindowMinutes(value), InputError);
  }
});

test("classifyDependencyError identifies non-retryable Influx configuration failures", () => {
  for (const statusCode of [401, 403, 404]) {
    assert.deepEqual(classifyDependencyError(statusCode), {
      code: "DEPENDENCY_MISCONFIGURED",
      message: "Sensor data service configuration is invalid. Check InfluxDB credentials, organization, bucket, and URL.",
    });
  }

  assert.deepEqual(classifyDependencyError(500), {
    code: "DEPENDENCY_FAILURE",
    message: "Sensor data is temporarily unavailable.",
  });
  assert.deepEqual(classifyDependencyError(), {
    code: "DEPENDENCY_FAILURE",
    message: "Sensor data is temporarily unavailable.",
  });
});

test("capAnomalyEvents bounds the response and reports truncation", () => {
  const exact = Array.from({ length: MAX_ANOMALY_EVENTS }, (_, index) => index);
  const overflow = [...exact, MAX_ANOMALY_EVENTS];

  assert.deepEqual(capAnomalyEvents(exact), {
    events: exact,
    truncated: false,
  });

  const capped = capAnomalyEvents(overflow);
  assert.equal(capped.events.length, MAX_ANOMALY_EVENTS);
  assert.equal(capped.events.at(-1), MAX_ANOMALY_EVENTS - 1);
  assert.equal(capped.truncated, true);
});

test("decodeFlags names every set flag and nothing else", () => {
  assert.deepEqual(decodeFlags(0), []);
  assert.deepEqual(decodeFlags(FLAG_ANOMALY), ["anomaly"]);
  assert.deepEqual(decodeFlags(FLAG_INTERLOCK_OPEN | FLAG_SENSOR_UNAVAILABLE), [
    "interlock_open",
    "sensor_unavailable",
  ]);
});

// The database-side bound is what actually protects Influx and the Pi. Without
// these assertions, deleting limit() from either query would leave every other
// test green while the fetch went unbounded again.
test("buildAnomalyQueries row-bounds both queries at the cap plus one", () => {
  const { vibration, faults } = buildAnomalyQueries({
    bucket: "sensors",
    nodeId: "node01",
    windowMinutes: 60,
  });

  for (const query of [vibration, faults]) {
    assert.match(query, new RegExp(`\\|> limit\\(n: ${MAX_ANOMALY_EVENTS + 1}\\)`));
  }
});

test("buildAnomalyQueries sorts before limiting so the newest rows survive", () => {
  const { vibration, faults } = buildAnomalyQueries({
    bucket: "sensors",
    nodeId: "node01",
    windowMinutes: 60,
  });

  for (const query of [vibration, faults]) {
    const sortAt = query.indexOf('sort(columns: ["_time"], desc: true)');
    const limitAt = query.indexOf("limit(n:");
    assert.ok(sortAt > -1, "query must sort by time");
    assert.ok(limitAt > sortAt, "limit must follow sort or it keeps the oldest rows");
  }
});

test("buildAnomalyQueries interpolates the window and targets both measurements", () => {
  const { vibration, faults } = buildAnomalyQueries({
    bucket: "sensors",
    nodeId: "node01",
    windowMinutes: 10.6,
  });

  // Window is normalized before interpolation, so 10.6 reaches Flux as 11m.
  assert.match(vibration, /\|> range\(start: -11m\)/);
  assert.match(faults, /\|> range\(start: -11m\)/);
  assert.match(vibration, /r\._measurement == "vibration"/);
  assert.match(faults, /r\._measurement == "sensor_faults"/);
  assert.match(vibration, /r\.flags >= 4/);
});

test("buildAnomalyQueries rejects an injected node_id before interpolation", () => {
  assert.throws(
    () =>
      buildAnomalyQueries({
        bucket: "sensors",
        nodeId: 'node01" or r.node_id != "',
        windowMinutes: 60,
      }),
    InputError,
  );
});
