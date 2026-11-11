// Run with the settings_reconciliation_tests executable as the first argument.
// The production TypeScript wrapper delegates to the real C++ reconciler.
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const { spawnSync } = require("node:child_process");
const ts = require("../ui/dashboard/spa-svelte/node_modules/typescript");

const executable = path.resolve(process.argv[2]);
const source = fs.readFileSync(path.join(__dirname, "../ui/dashboard/spa-svelte/src/lib/utils.ts"), "utf8");
const block = source.slice(source.indexOf("export interface Renames"), source.indexOf("export async function helper_createProject"));
const compiled = ts.transpileModule(block.replace(/export /g, ""), {
  compilerOptions: { target: ts.ScriptTarget.ES2022 },
}).outputText;
const window = {};
const reconcile = new Function("window", "jsonDeepCopy", compiled + "\nreturn helper_readjustProject;")(
  window, value => JSON.parse(JSON.stringify(value)),
);
const providers = { embedding_providers: [{ id: "embedding-new" }], generation_providers: [{ id: "model-new" }] };
const project = { settingsFilePath: "fixture.json", jsonData: {
  embedding: { current_api: "embedding-old" },
  generation: { current_api: "model-old", enabled_providers: ["model-old"],
    auto_router: { enabled: true, classifier: { type: "api", api_id: "model-old" },
      fallback: { default_model_id: "model-old" },
      routing_rules: { tier: { strategy: "direct", direct_model_id: "model-old" } } } },
} };
const clone = value => JSON.parse(JSON.stringify(value));

async function run() {
  let received;
  window.cppApi = { reconcileProject: async request => {
    received = request;
    const result = spawnSync(executable, ["--bridge"], { input: JSON.stringify(request), encoding: "utf8" });
    assert.equal(result.status, 0, result.stderr);
    return JSON.parse(result.stdout);
  } };
  const renamed = clone(project);
  const report = await reconcile(providers, renamed, {
    embedding: new Map([["embedding-old", "embedding-new"]]),
    generation: new Map([["model-old", "model-new"]]),
  });
  assert.deepEqual(received.renames.generation, { "model-old": "model-new" });
  assert.equal(renamed.jsonData.generation.auto_router.classifier.api_id, "model-new");
  assert.equal(renamed.jsonData.embedding.current_api, "embedding-new");
  assert.equal(report.proj, renamed);
  assert.equal(report.changed, true);
  assert.equal(report.needsReindex, false);
  assert.deepEqual(report.needsSelection, []);
  assert.equal(project.jsonData.generation.current_api, "model-old");

  const missing = clone(project);
  const missingReport = await reconcile(providers, missing);
  assert.deepEqual(missingReport.needsSelection, ["embedding.current_api", "generation.enabled_providers"]);
  assert.deepEqual(missing.jsonData.generation.enabled_providers, []);

  window.cppApi.reconcileProject = async () => ({ status: "error", message: "Fixture bridge error" });
  const failed = clone(project);
  await assert.rejects(reconcile(providers, failed), /Fixture bridge error/);
  assert.deepEqual(failed, project);

  let resolve;
  window.cppApi.reconcileProject = () => new Promise(value => { resolve = value; });
  const edited = clone(project);
  const pending = reconcile(providers, edited);
  edited.jsonData.generation.current_api = "concurrent-edit";
  resolve({ status: "success", jsonData: clone(project.jsonData) });
  await assert.rejects(pending, /changed during reconciliation/);
  assert.equal(edited.jsonData.generation.current_api, "concurrent-edit");
  console.log("TypeScript-to-C++ bridge, rename, selection, error, and concurrent-edit checks passed.");
}
run().catch(error => { console.error(error); process.exitCode = 1; });
