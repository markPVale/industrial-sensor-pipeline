# Project engineering guidance

## Failure handling and review

When adding or changing failure handling:

- Trace the full operation lifecycle, including admission, queueing, request
  execution, persistence, and response interpretation. Check related failure
  paths when fixing a reported symptom.
- Preserve failure origin explicitly. Prefer typed, exhaustive classification
  that requires handling new error variants. Avoid inferring dependency failure
  from a rule that treats every unrecognized error as a database failure.
- Organize tests around lifecycle stages and response invariants. Cover immediate
  rejection, deadline expiry before execution, expiry during execution, partial
  success, and successful empty results.
- Exercise defaults and meaningful configuration boundaries, including serial
  operation, no waiting queue, and contention from concurrent calls. Use the real
  production admission and transport paths for integration regressions; injected
  errors alone do not verify those paths.
- Assert negative outcomes: requests rejected or expired before execution must
  not reach HTTP, dependency failures must not become sensor failures, and
  cancellation must remove queued work and release resources.
- For behavior bugs, add a regression and observe it fail before fixing the code
  when practical. Report what checks establish and their verification limits;
  mocked or loopback tests do not establish live InfluxDB performance or hardware
  behavior.
- Before declaring completion, review every outward status claim by asking:
  "What observation supports this field?" Check the response contract and docs
  as well as the implementation.

For MCP telemetry responses specifically:

- Local overload and deadline expiry before a database request starts do not
  establish InfluxDB degradation. Preserve admission versus execution origin.
- Measurement-quality judgments require an actual telemetry row. A failed query
  or an empty result leaves quality unassessed, rather than proving invalid
  sensor measurements.
- Preserve positive evidence such as recent reporting and observed flags when
  another source fails. Keep overall assessment completeness separate from
  those observations; missing evidence is unknown rather than false.
- Distinguish query success, data presence, measurement quality, observation
  freshness, and sensor health in response fields and documentation. Recent
  stored telemetry is a freshness heuristic, not proof of a current connection.
