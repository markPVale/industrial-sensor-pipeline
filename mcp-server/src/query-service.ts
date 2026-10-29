import { InfluxDB } from "@influxdata/influxdb-client";
import { performance } from "node:perf_hooks";
import { classifyToolError, QueryError } from "./tool-utils.js";
import type { FluxRow } from "./telemetry-tools.js";

export function querySettings(env: NodeJS.ProcessEnv) {
  const integer = (name: string, fallback: number, allowZero = false) => {
    const value = env[name] === undefined ? fallback : Number(env[name]);
    if (!Number.isSafeInteger(value) || value < (allowZero ? 0 : 1) || value > 2_147_483_647) {
      throw new Error(`${name} must be ${allowZero ? "a nonnegative" : "a positive"} integer <= 2147483647`);
    }
    return value;
  };
  const settings = {
    deadlineMs: integer("MCP_QUERY_DEADLINE_MS", 4000),
    concurrency: integer("MCP_QUERY_CONCURRENCY", 4),
    maxQueue: integer("MCP_QUERY_MAX_QUEUE", 16, true),
  };
  requirePairedCapacity(settings);
  return settings;
}

function requirePairedCapacity(limits: { concurrency: number; maxQueue: number }) {
  if (limits.concurrency === 1 && limits.maxQueue === 0) {
    throw new Error("Paired telemetry/fault queries require MCP_QUERY_CONCURRENCY >= 2 or MCP_QUERY_MAX_QUEUE >= 1");
  }
}

// One scope per tool call: queueing and all its queries share the same budget.
export async function withQueryDeadline<T>(
  milliseconds: number, work: (signal: AbortSignal) => Promise<T>, parent?: AbortSignal,
): Promise<T> {
  const controller = new AbortController();
  const cancel = () => controller.abort(new QueryError("QUERY_CANCELLED"));
  const timer = setTimeout(() => controller.abort(new QueryError("QUERY_TIMEOUT")), milliseconds);
  parent?.addEventListener("abort", cancel, { once: true });
  if (parent?.aborted) cancel();
  try {
    controller.signal.throwIfAborted();
    return await work(controller.signal);
  } finally {
    clearTimeout(timer);
    parent?.removeEventListener("abort", cancel);
  }
}

type Waiter = { grant: () => void; signal: AbortSignal; abort: () => void };

// Shared by every MCP session. Expired waiters are removed immediately.
export class QueryPool {
  private active = 0;
  private queue: Waiter[] = [];
  constructor(private readonly limit: number, private readonly maxQueue: number) {
    if (!Number.isInteger(limit) || limit < 1 || !Number.isInteger(maxQueue) || maxQueue < 0) {
      throw new Error("Invalid query pool limits");
    }
  }
  get state() { return { active: this.active, queued: this.queue.length }; }

  async acquire(signal: AbortSignal): Promise<() => void> {
    signal.throwIfAborted();
    if (this.active < this.limit) {
      ++this.active;
    } else {
      if (this.queue.length >= this.maxQueue) throw new QueryError("QUERY_BUSY");
      await new Promise<void>((resolve, reject) => {
        const waiter: Waiter = {
          signal,
          grant: resolve,
          abort: () => {
            const index = this.queue.indexOf(waiter);
            if (index >= 0) this.queue.splice(index, 1);
            reject(signal.reason);
          },
        };
        this.queue.push(waiter);
        signal.addEventListener("abort", waiter.abort, { once: true });
      });
    }
    let released = false;
    return () => {
      if (released) return;
      released = true;
      const next = this.queue.shift();
      if (next) {
        next.signal.removeEventListener("abort", next.abort);
        next.grant(); // transfer the occupied slot
      } else {
        --this.active;
      }
    };
  }
}

type Connection = { url: string; token: string; org: string };
type QueryLog = {
  event: "influx_query"; duration_ms: number; outcome: string;
  queries_total: number; query_timeouts_total: number; active: number; queued: number;
};

export class QueryService {
  readonly pool: QueryPool;
  private queries = 0;
  private timeouts = 0;
  constructor(
    private readonly connection: Connection,
    limits: { concurrency: number; maxQueue: number },
    private readonly log: (event: QueryLog) => void = event => console.error(JSON.stringify(event)),
  ) {
    requirePairedCapacity(limits);
    this.pool = new QueryPool(limits.concurrency, limits.maxQueue);
  }

  async run(query: string, signal: AbortSignal): Promise<FluxRow[]> {
    const start = performance.now();
    const request = new AbortController();
    const abort = () => request.abort(signal.reason);
    signal.addEventListener("abort", abort, { once: true });
    if (signal.aborted) abort();
    let release: (() => void) | undefined;
    let requestStarted = false;
    let outcome = "ok";
    try {
      release = await this.pool.acquire(signal);
      signal.throwIfAborted();
      // A per-query signal reaches Node's http.request and destroys the socket
      // even before headers arrive. Observable unsubscribe alone does not.
      // Disable the SDK's inactivity timer; the shared wall-clock deadline owns
      // cancellation, including queued time and slowly streaming responses.
      const api = new InfluxDB({
        url: this.connection.url, token: this.connection.token,
        timeout: 0, transportOptions: { signal: request.signal },
      }).getQueryApi(this.connection.org);
      requestStarted = true;
      return await api.collectRows<FluxRow>(query);
    } catch (error) {
      let failure = signal.aborted ? signal.reason : error;
      outcome = classifyToolError(failure).code;
      if (outcome === "QUERY_TIMEOUT" && !requestStarted) {
        // Do not mutate the shared deadline reason: another source in the same
        // tool call may have started a database request and timed out there.
        failure = new QueryError("QUERY_QUEUE_TIMEOUT");
        outcome = failure.code;
      }
      if (outcome === "QUERY_TIMEOUT" || outcome === "QUERY_QUEUE_TIMEOUT") ++this.timeouts;
      throw failure;
    } finally {
      // Close any unfinished response on errors as well as on deadlines.
      request.abort();
      signal.removeEventListener("abort", abort);
      release?.();
      ++this.queries;
      this.log({ event: "influx_query", duration_ms: Math.round(performance.now() - start),
        outcome, queries_total: this.queries, query_timeouts_total: this.timeouts, ...this.pool.state });
    }
  }
}
