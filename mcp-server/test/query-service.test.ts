import assert from "node:assert/strict";
import { createServer, type ServerResponse } from "node:http";
import { once } from "node:events";
import { performance } from "node:perf_hooks";
import test, { type TestContext } from "node:test";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";
import { QueryPool, QueryService, querySettings, withQueryDeadline } from "../src/query-service.js";
import { classifyToolError, QueryError } from "../src/tool-utils.js";
import { createTelemetryTools } from "../src/telemetry-tools.js";

const csv = "#datatype,string,long,dateTime:RFC3339,double\n" +
  "#group,false,false,false,false\n#default,_result,,,\n" +
  ",result,table,_time,window_rms\n,,0,2026-10-28T12:00:00Z,10\n";

async function fixture(t: TestContext,
  respond: (query: string, res: ServerResponse) => void) {
  const server = createServer(async (req, res) => {
    let body = "";
    for await (const chunk of req) body += chunk;
    respond(JSON.parse(body).query, res);
  });
  server.listen(0, "127.0.0.1");
  await once(server, "listening");
  t.after(() => { server.closeAllConnections(); return new Promise<void>(resolve => server.close(() => resolve())); });
  const address = server.address();
  assert.ok(address && typeof address !== "string");
  const logs: Record<string, unknown>[] = [];
  const service = new QueryService({ url: `http://127.0.0.1:${address.port}`, token: "private-token", org: "industrial" },
    { concurrency: 2, maxQueue: 2 }, event => logs.push(event));
  return { service, logs, url: `http://127.0.0.1:${address.port}` };
}

function bounded<T>(promise: Promise<T>, milliseconds = 1000): Promise<T> {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error("test operation stalled")), milliseconds);
    promise.then(value => { clearTimeout(timer); resolve(value); }, error => { clearTimeout(timer); reject(error); });
  });
}

function body(result: Record<string, unknown>) {
  assert.ok(Array.isArray(result.content));
  assert.equal(result.content[0].type, "text");
  return JSON.parse(result.content[0].text);
}

test("query settings default conservatively and reject invalid values", () => {
  assert.deepEqual(querySettings({}), { deadlineMs: 4000, concurrency: 4, maxQueue: 16 });
  assert.equal(querySettings({ MCP_QUERY_MAX_QUEUE: "0" }).maxQueue, 0);
  for (const value of ["", "no", "0", "-1", "1.5", "Infinity", "2147483648"]) {
    assert.throws(() => querySettings({ MCP_QUERY_DEADLINE_MS: value }));
  }
});

test("deadline destroys an HTTP request stalled before headers and releases its slot", async t => {
  let closed!: () => void;
  const closure = new Promise<void>(resolve => { closed = resolve; });
  const { service, logs } = await fixture(t, (query, res) => {
    if (query === "stall") res.on("close", closed);
    else res.end(csv);
  });
  const start = performance.now();
  await assert.rejects(withQueryDeadline(150, signal => service.run("stall", signal)),
    error => classifyToolError(error).code === "QUERY_TIMEOUT");
  assert.ok(performance.now() - start < 650, "response must follow the wall-clock deadline");
  await bounded(closure);
  assert.deepEqual(service.pool.state, { active: 0, queued: 0 });
  assert.equal(logs[0].outcome, "QUERY_TIMEOUT");
  assert.equal(logs[0].timeouts_total, 1);
  assert.doesNotMatch(JSON.stringify(logs), /private-token|stall/);
  const rows = await withQueryDeadline(500, signal => service.run("recover", signal));
  assert.equal(rows[0].window_rms, 10);
});

test("a slowly streaming response cannot extend the wall-clock deadline", async t => {
  let closed!: () => void;
  const closure = new Promise<void>(resolve => { closed = resolve; });
  const { service } = await fixture(t, (_query, res) => {
    res.writeHead(200, { "Content-Type": "text/csv" });
    res.write(csv);
    const timer = setInterval(() => res.write(",,0,2026-10-28T12:00:00Z,10\n"), 15);
    res.on("close", () => { clearInterval(timer); closed(); });
  });
  await assert.rejects(withQueryDeadline(120, signal => service.run("stream", signal)),
    error => classifyToolError(error).code === "QUERY_TIMEOUT");
  await bounded(closure);
  assert.equal(service.pool.state.active, 0);
});

test("a slow query does not block an independent query", async t => {
  const { service } = await fixture(t, (query, res) => { if (query !== "stall") res.end(csv); });
  const stalled = withQueryDeadline(150, signal => service.run("stall", signal));
  const rejection = assert.rejects(stalled, QueryError);
  const rows = await withQueryDeadline(100, signal => service.run("fast", signal));
  assert.equal(rows.length, 1);
  await rejection;
});

test("MCP stdio responds during a slow call, returns timeout, then recovers", async t => {
  let stall = true;
  let received!: () => void;
  const started = new Promise<void>(resolve => { received = resolve; });
  const { url } = await fixture(t, (_query, res) => { received(); if (!stall) res.end(csv); });
  const transport = new StdioClientTransport({
    command: process.execPath,
    args: ["--import", "tsx", "src/index.ts"],
    cwd: process.cwd(),
    env: { INFLUX_URL: url, INFLUX_TOKEN: "private-token", TRANSPORT: "stdio", MCP_QUERY_DEADLINE_MS: "150" },
    stderr: "pipe",
  });
  // Drain diagnostics without mixing them into MCP stdout.
  transport.stderr?.on("data", () => {});
  const client = new Client({ name: "deadline-test", version: "1" });
  t.after(() => client.close());
  await client.connect(transport);
  const pending = client.callTool({ name: "get_latest_telemetry", arguments: {} });
  await bounded(started);
  assert.equal((await client.listTools()).tools.length, 3);
  const response = await bounded(pending);
  assert.equal(response.isError, true);
  assert.equal(body(response).error.code, "QUERY_TIMEOUT");
  stall = false;
  const recovered = await client.callTool({ name: "get_latest_telemetry", arguments: {} });
  assert.notEqual(recovered.isError, true);
  assert.equal(body(recovered).window_rms_mps2, 10);
});

test("a pre-cancelled call never starts HTTP work", async t => {
  let calls = 0;
  const { service } = await fixture(t, (_query, res) => { ++calls; res.end(csv); });
  const parent = new AbortController();
  parent.abort();
  await assert.rejects(withQueryDeadline(100, signal => service.run("unused", signal), parent.signal),
    error => classifyToolError(error).code === "QUERY_CANCELLED");
  assert.equal(calls, 0);
  assert.deepEqual(service.pool.state, { active: 0, queued: 0 });
});

test("queued work expires within its original budget and never takes a slot", async () => {
  const pool = new QueryPool(1, 1);
  const release = await pool.acquire(new AbortController().signal);
  await assert.rejects(withQueryDeadline(30, signal => pool.acquire(signal)),
    error => classifyToolError(error).code === "QUERY_TIMEOUT");
  assert.deepEqual(pool.state, { active: 1, queued: 0 });
  release();
  assert.deepEqual(pool.state, { active: 0, queued: 0 });
});

test("pool bounds its queue, transfers slots FIFO, and releases idempotently", async () => {
  const pool = new QueryPool(1, 2);
  const signal = new AbortController().signal;
  const release = await pool.acquire(signal);
  const queued = pool.acquire(signal);
  let lastGranted = false;
  const last = pool.acquire(signal).then(release => { lastGranted = true; return release; });
  await assert.rejects(pool.acquire(signal), error => classifyToolError(error).code === "QUERY_BUSY");
  release(); release();
  const nextRelease = await queued;
  assert.equal(lastGranted, false);
  assert.deepEqual(pool.state, { active: 1, queued: 1 });
  nextRelease();
  (await last)();
  assert.equal(pool.state.active, 0);
});

test("MCP cancellation propagates to HTTP and is distinct from timeout", async t => {
  let received!: () => void;
  const started = new Promise<void>(resolve => { received = resolve; });
  let closed!: () => void;
  const closure = new Promise<void>(resolve => { closed = resolve; });
  const { service } = await fixture(t, (_query, res) => { res.on("close", closed); received(); });
  const parent = new AbortController();
  const result = withQueryDeadline(1000, signal => service.run("cancel", signal), parent.signal);
  const rejection = assert.rejects(result, error => classifyToolError(error).code === "QUERY_CANCELLED");
  await bounded(started);
  parent.abort();
  await rejection;
  await bounded(closure);
  assert.equal(service.pool.state.active, 0);
});

test("repeated timeouts close every request and leave no occupied slots", async t => {
  const closures: Promise<void>[] = [];
  const { service } = await fixture(t, (_query, res) => {
    closures.push(new Promise<void>(resolve => res.on("close", resolve)));
  });
  for (let i = 0; i < 4; ++i) {
    await assert.rejects(withQueryDeadline(150, signal => service.run("stall", signal)), QueryError);
  }
  await bounded(Promise.all(closures));
  assert.equal(closures.length, 4);
  assert.deepEqual(service.pool.state, { active: 0, queued: 0 });
});

test("a shared deadline preserves completed telemetry when the fault query stalls", async t => {
  const { service } = await fixture(t, (query, res) => {
    if (!query.includes('r._measurement == "sensor_faults"')) res.end(csv);
  });
  const result = await withQueryDeadline(120, signal =>
    createTelemetryTools(query => service.run(query, signal), "sensors").getSensorHealth("node01"));
  assert.equal(result.status, "partial");
  assert.equal(result.assessment_complete, false);
  assert.equal(result.is_online, null);
  assert.match(result.health_summary, /^UNKNOWN/);
  assert.ok("window_rms_mps2" in result);
  assert.equal(result.window_rms_mps2, 10);
  assert.equal(result.sources.sensor_faults.error?.code, "QUERY_TIMEOUT");
  assert.deepEqual(service.pool.state, { active: 0, queued: 0 });
});

test("time spent queueing is not reset when HTTP work begins", async t => {
  let closed!: () => void;
  const closure = new Promise<void>(resolve => { closed = resolve; });
  const { url } = await fixture(t, (_query, res) => { res.on("close", closed); });
  const service = new QueryService({ url, token: "test", org: "industrial" },
    { concurrency: 1, maxQueue: 1 }, () => {});
  const release = await service.pool.acquire(new AbortController().signal);
  const timer = setTimeout(release, 120);
  t.after(() => { clearTimeout(timer); release(); });
  const start = performance.now();
  await assert.rejects(withQueryDeadline(240, signal => service.run("stall", signal)),
    error => classifyToolError(error).code === "QUERY_TIMEOUT");
  assert.ok(performance.now() - start < 330, "queueing must consume the same tool budget");
  await bounded(closure);
  assert.deepEqual(service.pool.state, { active: 0, queued: 0 });
});

test("HTTP failures retain classification and never turn into sensor offline", async t => {
  const { service } = await fixture(t, (_query, res) => {
    res.writeHead(401, { "Content-Type": "application/json" });
    res.end('{"message":"private database detail"}');
  });
  await assert.rejects(withQueryDeadline(500, signal =>
    createTelemetryTools(query => service.run(query, signal), "sensors").getSensorHealth("node01")),
    error => {
      assert.equal(classifyToolError(error).code, "DEPENDENCY_MISCONFIGURED");
      assert.doesNotMatch(JSON.stringify(classifyToolError(error)), /private database detail/);
      return true;
    });
  assert.equal(service.pool.state.active, 0);
});
