const { readFileSync } = require("node:fs");
const { resolve } = require("node:path");
const vm = require("node:vm");
const assert = require("node:assert/strict");
const ts = require("typescript");

// Test the production pure helper without loading React, Next, or MQTT.
const source = readFileSync(resolve(__dirname, "../hooks/telemetryValues.ts"), "utf8");
const { outputText } = ts.transpileModule(source, {
  compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020 },
});
const exportsObject = {};
vm.runInNewContext(outputText, { exports: exportsObject });
const { displayedRms, incompleteReadings } = exportsObject;
const sample = { ax: 0, ay: 0, az: 9.81, gx: 0, gy: 0, gz: 0 };
assert.equal(displayedRms(sample, false), 9.81);
assert.equal(displayedRms({ ...sample, wrms: 10 }, false), 10);
assert.equal(displayedRms({ ...sample, wrms: null }, false), null);
assert.equal(displayedRms({ ...sample, ax: null }, false), null);
assert.equal(displayedRms({ ...sample, wrms: Infinity }, false), null);
assert.equal(displayedRms({ ...sample, wrms: 10 }, true), null);
assert.equal(incompleteReadings(sample), false);
assert.equal(incompleteReadings({ ...sample, gy: null }), true);
assert.equal(incompleteReadings({ ...sample, wrms: null }), true);
console.log("Telemetry value assertions passed (9).");
