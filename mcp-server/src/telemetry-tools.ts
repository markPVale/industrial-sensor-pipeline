import {
  FLAG_ACCEL_CLIPPED,
  FLAG_ANOMALY,
  FLAG_DEGRADED_REBOOT_REQUIRED,
  FLAG_GYRO_CLIPPED,
  FLAG_INTERLOCK_OPEN,
  FLAG_SENSOR_FAULT,
  FLAG_SENSOR_UNAVAILABLE,
  buildAnomalyQueries,
  capAnomalyEvents,
  classifyToolError,
  decodeFlags,
  normalizeWindowMinutes,
  validateNodeId,
} from "./tool-utils.js";

export type FluxRow = Record<string, unknown>;
export type FluxRunner = (query: string) => Promise<FluxRow[]>;

function finite(value: unknown): number | null {
  return typeof value === "number" && Number.isFinite(value) ? value : null;
}

// Preserve evidence even when measurements are unavailable. Older rows without
// quality metadata are checked from their fields; absence never proves a legacy
// vector value was a measured window RMS.
function measurements(row: FluxRow) {
  const reported = new Set(typeof row.unavailable_fields === "string"
    ? row.unavailable_fields.split(",") : []);
  const value = (wire: string, field = wire) => reported.has(wire) ? null : finite(row[field]);
  const axes = { ax: value("ax"), ay: value("ay"), az: value("az"),
                 gx: value("gx"), gy: value("gy"), gz: value("gz") };
  const windowRms = value("wrms", "window_rms");
  const all = { ...axes, wrms: windowRms };
  const unavailable = Object.entries(all).filter(([, v]) => v === null).map(([key]) => key);
  const complete = row.data_complete !== false && unavailable.length === 0;
  return {
    ...axes,
    vibration_rms_mps2: windowRms, // retained alias, never a vector fallback
    window_rms_mps2: windowRms,
    vector_magnitude_mps2: [axes.ax, axes.ay, axes.az].some(v => v === null)
      ? null : finite(row.vibration_rms),
    data_complete: complete,
    data_status: complete ? "complete" : unavailable.length === 7 ? "unavailable" : "incomplete",
    unavailable_fields: unavailable,
    reason: typeof row.reason === "string" ? row.reason : null,
  };
}

// The production tools with injected database/clock boundaries, no server startup.
export function createTelemetryTools(runFlux: FluxRunner, INFLUX_BUCKET: string,
                                     now: () => number = Date.now) {
  /** Normalize the timestamp supplied by the Influx client. */
  function toISOString(t: unknown): string {
    if (t instanceof Date) return t.toISOString();
    return String(t);
  }

  function source(result: PromiseSettledResult<FluxRow[]>) {
    if (result.status === "rejected") {
      return { status: "error", timestamp: null, age_seconds: null,
        error: classifyToolError(result.reason) };
    }
    const timestamp = result.value[0] ? toISOString(result.value[0]._time) : null;
    return { status: timestamp ? "ok" : "no_data", timestamp,
      age_seconds: timestamp ? Math.max(0, Math.round((now() - Date.parse(timestamp)) / 1000)) : null };
  }

  async function paired(vibration: string, faults: string) {
    const [vib, fault] = await Promise.allSettled([runFlux(vibration), runFlux(faults)]);
    // Complete failure is an MCP error, never an empty result or sensor status.
    if (vib.status === "rejected" && fault.status === "rejected") throw vib.reason;
    const complete = vib.status === "fulfilled" && fault.status === "fulfilled";
    return {
      vibRows: vib.status === "fulfilled" ? vib.value : [],
      faultRows: fault.status === "fulfilled" ? fault.value : [],
      metadata: {
        status: complete ? "ok" : "partial",
        query_complete: complete,
        queried_at: new Date(now()).toISOString(),
        dependency_status: { influxdb: complete ? "available" : "degraded" },
        sources: { vibration: source(vib), sensor_faults: source(fault) },
      },
    };
  }

  // ---------------------------------------------------------------------------
  // Tool implementations
  // ---------------------------------------------------------------------------

  /**
   * get_latest_telemetry
   * Returns the single most recent vibration record for a node.
   */
  async function getLatestTelemetry(nodeId: string) {
    validateNodeId(nodeId);
    const flux = `
      from(bucket: "${INFLUX_BUCKET}")
        |> range(start: -1h)
        |> filter(fn: (r) => r._measurement == "vibration" and r.node_id == "${nodeId}")
        |> pivot(rowKey: ["_time"], columnKey: ["_field"], valueColumn: "_value")
        |> sort(columns: ["_time"], desc: true)
        |> limit(n: 1)
    `;

    const rows = await runFlux(flux);
    const metadata = { queried_at: new Date(now()).toISOString(), query_complete: true,
      sources: { vibration: source({ status: "fulfilled", value: rows }) } };
    if (rows.length === 0) {
      return { ...metadata, node_id: nodeId, status: "no_data", message: "No telemetry found in the last hour." };
    }

    const r     = rows[0];
    const flags = r["flags"] as number;
    return {
      ...metadata,
      node_id:                        nodeId,
      timestamp:                      toISOString(r["_time"]),
      ...measurements(r),
      boot_id:                        r["boot_id"],
      sequence_id:                    r["sequence_id"],
      flags,
      flag_accel_clipped:             (flags & FLAG_ACCEL_CLIPPED)            !== 0,
      flag_gyro_clipped:              (flags & FLAG_GYRO_CLIPPED)             !== 0,
      flag_interlock_open:            (flags & FLAG_INTERLOCK_OPEN)           !== 0,
      flag_anomaly:                   (flags & FLAG_ANOMALY)                  !== 0,
      flag_sensor_fault:              (flags & FLAG_SENSOR_FAULT)             !== 0,
      flag_degraded_reboot_required:  (flags & FLAG_DEGRADED_REBOOT_REQUIRED) !== 0,
      flag_sensor_unavailable:        (flags & FLAG_SENSOR_UNAVAILABLE)       !== 0,
    };
  }

  /**
   * get_sensor_health
   * Returns a human-readable health summary for a node.
   */
  async function getSensorHealth(nodeId: string) {
    validateNodeId(nodeId);

    // Query both measurements in parallel — fault records (0x10/0x20/0x40) are
    // routed to sensor_faults by the bridge and would be invisible to a
    // vibration-only query, causing health to report stale "OK" then OFFLINE
    // instead of DEGRADED or UNAVAILABLE during an I2C fault.
    const { vibRows, faultRows, metadata } = await paired(`
        from(bucket: "${INFLUX_BUCKET}")
          |> range(start: -5m)
          |> filter(fn: (r) => r._measurement == "vibration" and r.node_id == "${nodeId}")
          |> pivot(rowKey: ["_time"], columnKey: ["_field"], valueColumn: "_value")
          |> sort(columns: ["_time"], desc: true)
          |> limit(n: 1)
      `, `
        from(bucket: "${INFLUX_BUCKET}")
          |> range(start: -5m)
          |> filter(fn: (r) => r._measurement == "sensor_faults" and r.node_id == "${nodeId}")
          |> pivot(rowKey: ["_time"], columnKey: ["_field"], valueColumn: "_value")
          |> sort(columns: ["_time"], desc: true)
          |> limit(n: 1)
      `);

    const latestVib   = vibRows[0]   ?? null;
    const latestFault = faultRows[0] ?? null;

    if (!latestVib && !latestFault) {
      return {
        ...metadata,
        assessment_complete: metadata.query_complete,
        node_id:   nodeId,
        is_online: metadata.query_complete ? false : null,
        health_summary: metadata.query_complete ? "OFFLINE — no recent telemetry."
          : "UNKNOWN — database query incomplete; sensor health cannot be assessed.",
        message: metadata.query_complete
          ? "No telemetry received in the last 5 minutes. Node may be offline or disconnected."
          : "One data source could not be queried; absence of telemetry cannot be established.",
      };
    }

    const vibTime    = latestVib   ? new Date(toISOString(latestVib["_time"])).getTime()   : 0;
    const faultTime  = latestFault ? new Date(toISOString(latestFault["_time"])).getTime() : 0;
    const latestTime = Math.max(vibTime, faultTime);
    const vibFlags   = latestVib ? latestVib["flags"] as number : 0;
    const faultFlags = latestFault
      ? (latestFault["flags"] != null ? latestFault["flags"] : FLAG_SENSOR_FAULT) as number
      : 0;
    const mergedFlags = vibFlags | faultFlags;
    const metrics = measurements(latestVib ?? {});
    const rms = metrics.window_rms_mps2;

    const age = Math.round((now() - latestTime) / 1000);
    // An observed flag remains evidence; missing flags are unknown when a source
    // failed, because the unavailable source may contain a fault or interlock.
    const flag = (mask: number) => (mergedFlags & mask) !== 0 ? true
      : metadata.query_complete ? false : null;
    return {
      ...metadata,
      assessment_complete: metadata.query_complete,
      node_id:                        nodeId,
      is_online:                      metadata.query_complete ? age < 30 : null,
      last_seen_seconds_ago:          age,
      vibration_rms_mps2:             rms,
      window_rms_mps2:                rms,
      vector_magnitude_mps2:          metrics.vector_magnitude_mps2,
      data_complete:                 metrics.data_complete,
      data_status:                   metrics.data_status,
      unavailable_fields:            metrics.unavailable_fields,
      reason:                        metrics.reason,
      flag_interlock_open:            flag(FLAG_INTERLOCK_OPEN),
      flag_anomaly:                   flag(FLAG_ANOMALY),
      flag_sensor_fault:              flag(FLAG_SENSOR_FAULT),
      flag_degraded_reboot_required:  flag(FLAG_DEGRADED_REBOOT_REQUIRED),
      flag_sensor_unavailable:        flag(FLAG_SENSOR_UNAVAILABLE),
      whoami_raw:                     latestFault ? latestFault["whoami_raw"] ?? null : null,
      health_summary:                 metadata.query_complete
        ? buildHealthSummary(age, mergedFlags, rms, metrics.data_complete)
        : "UNKNOWN — database query incomplete; observed readings and flags are partial evidence.",
    };
  }

  function buildHealthSummary(ageSeconds: number, flags: number, rms: number | null, complete: boolean): string {
    if (ageSeconds >= 30)                      return "OFFLINE — no recent telemetry.";
    if (flags & FLAG_INTERLOCK_OPEN)           return "CRITICAL — E-Stop / safety interlock is active.";
    if (flags & FLAG_SENSOR_UNAVAILABLE)       return "CRITICAL — Sensor unavailable. Max auto-reboots exhausted. Power-cycle required.";
    if (flags & FLAG_DEGRADED_REBOOT_REQUIRED) return "DEGRADED — I2C fault detected. Auto-reboot pending.";
    if (flags & FLAG_SENSOR_FAULT)             return "DEGRADED — I2C sensor fault (legacy flag).";
    if (!complete || rms === null)            return "DATA UNAVAILABLE — recent telemetry contains missing or invalid measurements.";
    if (flags & FLAG_ANOMALY)                  return `WARNING — Anomaly detected. Vibration RMS: ${rms.toFixed(4)} m/s²`;
    return `OK — Normal operation. Vibration RMS: ${rms.toFixed(4)} m/s²`;
  }

  /**
   * get_recent_anomalies
   * Returns bounded flagged or invalid-data events; does not certify coverage.
   * Queries both vibration and sensor_faults so I2C fault events are not missed.
   */
  async function getRecentAnomalies(nodeId: string, windowMinutes: unknown) {
    validateNodeId(nodeId);
    const window = normalizeWindowMinutes(windowMinutes);

    const queries = buildAnomalyQueries({
      bucket: INFLUX_BUCKET,
      nodeId,
      windowMinutes: window,
    });

    const { vibRows, faultRows, metadata } = await paired(queries.vibration, queries.faults);

    if (vibRows.length === 0 && faultRows.length === 0) {
      return {
        ...metadata,
        node_id:              nodeId,
        window_minutes:       window,
        returned_event_count: 0,
        truncated:            false,
        coverage_status:      "not_assessed",
        message: metadata.query_complete
          ? `No flagged or invalid-data events found in the last ${window} minutes; data coverage has not been assessed.`
          : "Query incomplete; events from an unavailable source may be missing. Data coverage has not been assessed.",
        events:               [],
      };
    }

    // Invalid telemetry events retain quality evidence and
    // nullable measurements. Fault events omit accel fields (ax encodes WHO_AM_I).
    const vibEvents = vibRows.map((r) => {
      const flags = r["flags"] as number;
      return {
        timestamp:             toISOString(r["_time"]),
        source:                "vibration",
        ...measurements(r),
        flags,
        active_flags:          decodeFlags(flags),
      };
    });

    const faultEvents = faultRows.map((r) => {
      const flags = (r["flags"] != null ? r["flags"] : FLAG_SENSOR_FAULT) as number;
      return {
        timestamp:    toISOString(r["_time"]),
        source:       "sensor_faults",
        flags,
        active_flags: decodeFlags(flags),
      };
    });

    const allEvents = [...vibEvents, ...faultEvents].sort(
      (a, b) => new Date(b.timestamp).getTime() - new Date(a.timestamp).getTime(),
    );
    const { events, truncated } = capAnomalyEvents(allEvents);

    // `returned_event_count` is deliberately not named anomaly_count: when
    // truncated is true this is the size of this page, not the number of matches.
    // The true total is not computed — both queries are row-bounded — so it is
    // omitted rather than guessed.
    return {
      ...metadata,
      node_id:              nodeId,
      window_minutes:       window,
      returned_event_count: events.length,
      truncated,
      coverage_status:      "not_assessed",
      events,
    };
  }

  return { getLatestTelemetry, getSensorHealth, getRecentAnomalies };
}
