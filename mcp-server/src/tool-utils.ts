// Bound tool responses so a sustained fault cannot flood an LLM context with
// an entire day of 2 Hz telemetry. A pretty-printed 500-event response measured
// ~236 KB (~59k tokens) — far too large for a single tool result. 50 compact
// events is ~8 KB; callers needing more should narrow window_minutes.
export const MAX_ANOMALY_EVENTS = 50;

export const NODE_ID_RE = /^[a-zA-Z0-9_-]{1,32}$/;

export class InputError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "InputError";
  }
}

export type DependencyError = {
  code: "DEPENDENCY_MISCONFIGURED" | "DEPENDENCY_FAILURE";
  message: string;
};

/**
 * Classify InfluxDB HTTP failures without exposing server response details.
 * Authentication, authorization, and missing-resource errors will not recover
 * through retries; other statuses may be transient or require log inspection.
 */
export function classifyDependencyError(statusCode?: number): DependencyError {
  if (statusCode === 401 || statusCode === 403 || statusCode === 404) {
    return {
      code: "DEPENDENCY_MISCONFIGURED",
      message: "Sensor data service configuration is invalid. Check InfluxDB credentials, organization, bucket, and URL.",
    };
  }

  return {
    code: "DEPENDENCY_FAILURE",
    message: "Sensor data is temporarily unavailable.",
  };
}

export function validateNodeId(nodeId: unknown): asserts nodeId is string {
  if (typeof nodeId !== "string" || !NODE_ID_RE.test(nodeId)) {
    throw new InputError(
      "node_id must contain 1–32 letters, numbers, underscores, or hyphens",
    );
  }
}

export function parseNodeId(value: unknown): string {
  const nodeId = value ?? "node01";
  validateNodeId(nodeId);
  return nodeId;
}

export function normalizeWindowMinutes(value: unknown): number {
  if (value === undefined || value === null) return 60;

  if (typeof value !== "number" || !Number.isFinite(value)) {
    throw new InputError("window_minutes must be a finite number");
  }

  return Math.min(Math.max(Math.round(value), 1), 1440); // 1 min – 24 h
}

export function capAnomalyEvents<T>(allEvents: readonly T[]): {
  events: T[];
  truncated: boolean;
} {
  return {
    events: allEvents.slice(0, MAX_ANOMALY_EVENTS),
    truncated: allEvents.length > MAX_ANOMALY_EVENTS,
  };
}

// ---------------------------------------------------------------------------
// Status flag bit masks — must match firmware/include/types.h and
// docs/telemetry-schema.md exactly.
// ---------------------------------------------------------------------------
export const FLAG_ACCEL_CLIPPED            = 0x01;
export const FLAG_GYRO_CLIPPED             = 0x02;
export const FLAG_INTERLOCK_OPEN           = 0x04;  // E-Stop / safety interlock
export const FLAG_ANOMALY                  = 0x08;
export const FLAG_SENSOR_FAULT             = 0x10;  // I2C dropout (legacy — use degraded/unavailable)
export const FLAG_DEGRADED_REBOOT_REQUIRED = 0x20;  // I2C fault; auto-reboot pending
export const FLAG_SENSOR_UNAVAILABLE       = 0x40;  // I2C fault; max reboots exhausted

const FLAG_NAMES: ReadonlyArray<readonly [number, string]> = [
  [FLAG_ACCEL_CLIPPED,            "accel_clipped"],
  [FLAG_GYRO_CLIPPED,             "gyro_clipped"],
  [FLAG_INTERLOCK_OPEN,           "interlock_open"],
  [FLAG_ANOMALY,                  "anomaly"],
  [FLAG_SENSOR_FAULT,             "sensor_fault"],
  [FLAG_DEGRADED_REBOOT_REQUIRED, "degraded_reboot_required"],
  [FLAG_SENSOR_UNAVAILABLE,       "sensor_unavailable"],
];

/**
 * Expand a status bitmask into the names of the flags that are set.
 * Used instead of one boolean per flag: a full boolean set costs ~185 bytes
 * per event, while the common case here is an array of one or two names.
 */
export function decodeFlags(flags: number): string[] {
  return FLAG_NAMES.filter(([mask]) => (flags & mask) !== 0).map(([, name]) => name);
}

/**
 * Build the two Flux queries backing get_recent_anomalies.
 *
 * Extracted as a pure function so the database-side row bound is testable —
 * without this, dropping `limit()` from either query would leave every unit
 * test green while Influx went back to returning unbounded rows.
 *
 * `sort` must precede `limit` so the limit keeps the NEWEST rows. Each query
 * fetches MAX_ANOMALY_EVENTS+1 rows so the merged result can detect truncation.
 *
 * The row bound deliberately reads MAX_ANOMALY_EVENTS directly rather than
 * taking it as a parameter: capAnomalyEvents() slices against that same
 * constant, and a caller-supplied limit could desync the two — bounding rows at
 * the database while the merged result still reported truncated: false.
 */
export function buildAnomalyQueries(opts: {
  bucket: string;
  nodeId: string;
  windowMinutes: number;
}): { vibration: string; faults: string } {
  const { bucket, nodeId, windowMinutes } = opts;
  validateNodeId(nodeId);
  const window = normalizeWindowMinutes(windowMinutes);
  const rowLimit = MAX_ANOMALY_EVENTS + 1;

  // flags >= 4 excludes accel/gyro-clip-only records (0x01, 0x02, 0x03) while
  // capturing all actionable flags: interlock (0x04), anomaly (0x08), and all
  // sensor-fault variants (0x10, 0x20, 0x40).
  const vibration = `
    from(bucket: "${bucket}")
      |> range(start: -${window}m)
      |> filter(fn: (r) => r._measurement == "vibration" and r.node_id == "${nodeId}")
      |> pivot(rowKey: ["_time"], columnKey: ["_field"], valueColumn: "_value")
      |> filter(fn: (r) =>
        exists r.flags and exists r.vibration_rms and
        exists r.ax and exists r.ay and exists r.az
      )
      |> filter(fn: (r) => r.flags >= 4)
      |> sort(columns: ["_time"], desc: true)
      |> limit(n: ${rowLimit})
  `;

  const faults = `
    from(bucket: "${bucket}")
      |> range(start: -${window}m)
      |> filter(fn: (r) => r._measurement == "sensor_faults" and r.node_id == "${nodeId}")
      |> pivot(rowKey: ["_time"], columnKey: ["_field"], valueColumn: "_value")
      |> sort(columns: ["_time"], desc: true)
      |> limit(n: ${rowLimit})
  `;

  return { vibration, faults };
}
