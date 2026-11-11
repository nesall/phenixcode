const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const ts = require("../ui/dashboard/spa-svelte/node_modules/typescript");
const source = fs.readFileSync(path.join(__dirname, "../ui/dashboard/spa-svelte/src/lib/widgets/CentralProjects.svelte"), "utf8");
const block = source.slice(source.indexOf("  let createDialog:"), source.indexOf("  async function onDeleteProject"));
const compiled = ts.transpileModule(block, { compilerOptions: { target: ts.ScriptTarget.ES2022 } }).outputText;
const calls = [];
let resolveCreation;
const dialog = { showModal() { calls.push("open"); }, close() { calls.push("close"); } };
const input = { focus() { calls.push("focus"); }, select() { calls.push("select"); } };
const store = {};
const factory = new Function("$state", "tick", "helper_createProject", "projectStore", "fetchProjects", "toaster", compiled + `
return {
  initialize(dialog, input) { createDialog = dialog; filenameInput = input; },
  open: onAddProject,
  submit: createProject,
  setFilename(value) { filename = value; },
  state() { return { filename, creating, createError }; }
};`);
const ui = factory(value => value, () => Promise.resolve(), filename => {
  calls.push(filename);
  return new Promise(resolve => { resolveCreation = resolve; });
}, store, () => calls.push("refresh"), { success() { calls.push("success"); } });
ui.initialize(dialog, input);
const submit = () => ui.submit({ preventDefault() {} });
(async () => {
  await ui.open();
  assert.match(ui.state().filename, /^settings_[a-f0-9]{12}$/);
  assert.deepEqual(calls, ["open", "focus", "select"]);
  ui.setFilename("../outside");
  await submit();
  assert.match(ui.state().createError, /valid filename/);
  assert.equal(calls.length, 3);
  ui.setFilename("my-project.JSON");
  const pending = submit();
  assert.equal(ui.state().creating, true);
  await submit();
  assert.equal(calls.filter(value => value === "my-project.json").length, 1);
  resolveCreation({ status: "error", message: "Already exists" });
  await pending;
  assert.equal(ui.state().createError, "Already exists");
  assert.equal(ui.state().creating, false);
  assert.equal(calls.includes("close"), false);
  const retry = submit();
  const project = { status: "success", settingsFilePath: "fixture/my-project.json" };
  resolveCreation(project);
  await retry;
  assert.equal(store.selected, project);
  assert.deepEqual(calls.slice(-3), ["close", "refresh", "success"]);
  console.log("Project popup defaults, validation, duplicate submission, errors, and success checks passed.");
})().catch(error => { console.error(error); process.exitCode = 1; });
