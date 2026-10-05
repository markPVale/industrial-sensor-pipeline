// Keep explicit null distinct from absent legacy wrms. JS arithmetic otherwise
// coerces null axes to zero and fabricates a healthy-looking vector magnitude.
export interface SensorValues {
  ax: number | null; ay: number | null; az: number | null;
  gx: number | null; gy: number | null; gz: number | null;
  wrms?: number | null;
}

function finite(value: unknown): value is number {
  return typeof value === "number" && Number.isFinite(value);
}

export function displayedRms(raw: SensorValues, isFault: boolean): number | null {
  if (isFault) return null;
  if (raw.wrms !== undefined) return finite(raw.wrms) ? raw.wrms : null;
  if (!finite(raw.ax) || !finite(raw.ay) || !finite(raw.az)) return null;
  const magnitude = Math.hypot(raw.ax, raw.ay, raw.az);
  return finite(magnitude) ? magnitude : null;
}

export function incompleteReadings(raw: SensorValues): boolean {
  return ![raw.ax, raw.ay, raw.az, raw.gx, raw.gy, raw.gz].every(finite)
    || (raw.wrms !== undefined && !finite(raw.wrms));
}
