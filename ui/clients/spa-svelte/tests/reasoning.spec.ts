import { expect, test } from "@playwright/test";
import type { Page } from "@playwright/test";

const providers = [
  { id: "provider-a", name: "Provider A", model: "model-a", current: true, combinedPrice: 1,
    reasoning: { parameters: [
      { name: "reasoning_effort", value_type: "scalar", values: ["low", "medium", "high", "max"], default: "medium" },
      { name: "thinking", value_type: "object", value_field: "type", constant_fields: { budget: "1000" },
        values: ["disabled", "enabled", "adaptive"], default: "enabled" },
    ] } },
  { id: "provider-b", name: "Provider B", model: "model-b", combinedPrice: 2,
    reasoning: { parameters: [
      { name: "reasoning_effort", value_type: "scalar", values: ["minimal", "standard", "deep"], default: "standard" },
    ] } },
  { id: "provider-c", name: "Provider C", model: "model-c", combinedPrice: 3 },
  { id: "provider-empty", name: "Empty", model: "empty", combinedPrice: 4, reasoning: { parameters: [] } },
  { id: "provider-no-values", name: "No values", model: "no-values", combinedPrice: 5,
    reasoning: { parameters: [{ name: "thinking", value_type: "scalar", values: [], default: "" }] } },
  { id: "auto", name: "Auto-router", model: "auto", combinedPrice: 0 },
];

const picker = (page: Page) => page.getByRole("button", { name: "Reasoning", exact: true });
const popup = (page: Page) => page.getByRole("dialog", { name: "Reasoning", exact: true });
const parameter = (page: Page, name: string) => popup(page).getByRole("group", { name, exact: true });
async function openPicker(page: Page) {
  await picker(page).click();
  await expect(popup(page)).toBeVisible();
}
async function apply(page: Page) {
  await popup(page).getByRole("button", { name: "Apply", exact: true }).click();
  await expect(popup(page)).toBeHidden();
}
async function choose(page: Page, name: string, value: string) {
  await openPicker(page);
  await parameter(page, name).getByRole("radio", { name: value, exact: true }).check();
  await apply(page);
}

async function switchProvider(page: Page, id: string) {
  const selector = page.getByRole("combobox", { name: "Select an option", exact: true }).last();
  await selector.getByRole("button").click();
  await selector.locator(`[role="option"][data-value="${id}"]`).click();
  await expect(selector).toHaveAttribute("aria-expanded", "false");
}

test.beforeEach(async ({ page }) => {
  await page.route("**/api/**", route => {
    const path = new URL(route.request().url()).pathname;
    if (path === "/api/chat") return route.fulfill({ contentType: "text/event-stream",
      body: 'data: {"content":"Test reply"}\n\ndata: [DONE]\n\n' });
    const json = path === "/api/settings" ? { completionApis: providers, currentApi: "provider-a" }
      : path === "/api/instances" ? { instances: [{ id: "instance", project_id: "project", name: "Project", host: "localhost", port: 8590 }], current_instance: "instance" }
      : { status: "success" };
    return route.fulfill({ json });
  });
});

async function start(page: Page) {
  await page.goto("/");
  await expect(page.getByText("Connected.", { exact: true })).toBeVisible();
}

test("one button summarizes applied values; independent radio groups apply together without sending", async ({ page }, testInfo) => {
  const requests: unknown[] = [];
  page.on("request", request => { if (new URL(request.url()).pathname === "/api/chat") requests.push(request.postDataJSON()); });
  await start(page);
  await expect(picker(page)).toHaveText("Reasoning");
  await expect(picker(page)).toHaveAttribute("title", "reasoning_effort: medium; thinking: enabled");
  await picker(page).hover();
  await expect(popup(page)).toBeHidden();
  const input = page.getByRole("textbox");
  await input.fill("Keep this draft");
  await openPicker(page);
  await expect(parameter(page, "reasoning_effort").getByRole("radio")).toHaveCount(4);
  await expect(parameter(page, "thinking").getByRole("radio")).toHaveCount(3);
  await parameter(page, "thinking").getByRole("radio", { name: "disabled", exact: true }).check();
  await expect(parameter(page, "reasoning_effort").getByRole("radio", { name: "high", exact: true })).toBeEnabled();
  await parameter(page, "reasoning_effort").getByRole("radio", { name: "high", exact: true }).check();
  await expect(picker(page)).toHaveAttribute("title", "reasoning_effort: medium; thinking: enabled");
  expect(await page.evaluate(() => localStorage.getItem("ReasoningSelections"))).toBeNull();
  await page.screenshot({ path: testInfo.outputPath("reasoning-picker.png") });
  await apply(page);
  await expect(picker(page)).toHaveAttribute("title", "reasoning_effort: high; thinking: disabled");
  expect(requests).toHaveLength(0);
  await expect(input).toHaveValue("Keep this draft");
  const send = await page.getByRole("button", { name: "Send message", exact: true }).boundingBox();
  const reasoning = await picker(page).boundingBox();
  const frame = await page.getByTestId("message-input-frame").boundingBox();
  const bottom = frame!.y + frame!.height;
  expect(reasoning!.y).toBeLessThan(bottom);
  expect(reasoning!.y + reasoning!.height).toBeGreaterThan(bottom);
  expect(send!.y + send!.height).toBeLessThanOrEqual(reasoning!.y);
  const request = page.waitForRequest("**/api/chat");
  await page.getByRole("button", { name: "Send message", exact: true }).click();
  expect((await request).postDataJSON()).toMatchObject({ targetapi: "provider-a", temperature: 0.1,
    reasoning: { reasoning_effort: "high", thinking: "disabled" } });
  await expect(page.getByText("Test reply", { exact: true })).toBeVisible();
});

test("Escape and outside click discard drafts and reopening uses applied choices", async ({ page }) => {
  await start(page);
  await choose(page, "reasoning_effort", "high");
  await openPicker(page);
  await parameter(page, "reasoning_effort").getByRole("radio", { name: "max", exact: true }).check();
  await page.keyboard.press("Escape");
  await expect(popup(page)).toBeHidden();
  await expect(picker(page)).toBeFocused();
  await expect(picker(page)).toHaveAttribute("title", "reasoning_effort: high; thinking: enabled");
  await openPicker(page);
  await expect(parameter(page, "reasoning_effort").getByRole("radio", { name: "high", exact: true })).toBeChecked();
  await parameter(page, "thinking").getByRole("radio", { name: "adaptive", exact: true }).check();
  await page.getByRole("textbox").click();
  await expect(popup(page)).toBeHidden();
  await openPicker(page);
  await expect(parameter(page, "thinking").getByRole("radio", { name: "enabled", exact: true })).toBeChecked();
});

test("provider selections remain independent and survive reload", async ({ page }) => {
  await start(page);
  await choose(page, "reasoning_effort", "high");
  await choose(page, "thinking", "disabled");
  await switchProvider(page, "provider-b");
  await expect(picker(page)).toHaveAttribute("title", "reasoning_effort: standard");
  await openPicker(page);
  await expect(parameter(page, "thinking")).toHaveCount(0);
  await page.keyboard.press("Escape");
  await choose(page, "reasoning_effort", "deep");
  await switchProvider(page, "provider-a");
  await expect(picker(page)).toHaveAttribute("title", "reasoning_effort: high; thinking: disabled");
  await page.reload();
  await expect(page.getByText("Connected.", { exact: true })).toBeVisible();
  await expect(picker(page)).toHaveAttribute("title", "reasoning_effort: high; thinking: disabled");
  await switchProvider(page, "provider-b");
  await expect(picker(page)).toHaveAttribute("title", "reasoning_effort: deep");
});

test("unsupported, empty, and automatic providers hide the picker and omit reasoning from requests", async ({ page }) => {
  await start(page);
  for (const id of ["provider-c", "provider-empty", "provider-no-values", "auto"]) {
    await switchProvider(page, id);
    await expect(picker(page)).toHaveCount(0);
    await page.getByRole("textbox").fill("Question");
    const request = page.waitForRequest("**/api/chat");
    await page.getByRole("button", { name: "Send message", exact: true }).click();
    const body = (await request).postDataJSON();
    expect(body.targetapi).toBe(id);
    expect(body).not.toHaveProperty("reasoning");
    await expect(page.getByRole("textbox")).toBeEnabled();
  }
});

test("invalid persisted values fall back to defaults then the first available value", async ({ page }) => {
  await page.addInitScript(() => localStorage.setItem("ReasoningSelections", JSON.stringify({
    "provider-a": { reasoning_effort: "removed-value", thinking: "adaptive", obsolete: "ignored" },
    "provider-b": { reasoning_effort: "wrong" },
  })));
  await page.route("**/api/settings", route => route.fulfill({ json: {
    completionApis: providers.map(provider => provider.id === "provider-b" ? { ...provider,
      reasoning: { parameters: [{ name: "reasoning_effort", values: ["minimal", "standard", "deep"], default: "removed-default" }] } } : provider),
    currentApi: "provider-a",
  } }));
  await start(page);
  await expect(picker(page)).toHaveAttribute("title", "reasoning_effort: medium; thinking: adaptive");
  await switchProvider(page, "provider-b");
  await expect(picker(page)).toHaveAttribute("title", "reasoning_effort: minimal");
});

test("malformed saved preferences do not block sending or replace defaults", async ({ page }) => {
  await page.addInitScript(() => localStorage.setItem("ReasoningSelections", "{broken json"));
  await start(page);
  await expect(picker(page)).toHaveAttribute("title", "reasoning_effort: medium; thinking: enabled");
  await page.getByRole("textbox").fill("Question");
  const request = page.waitForRequest("**/api/chat");
  await page.getByRole("textbox").press("Enter");
  expect((await request).postDataJSON().reasoning).toEqual({ reasoning_effort: "medium", thinking: "enabled" });
});

test("picker is disabled while generating and Retry uses the applied selections", async ({ page }) => {
  let release!: () => void;
  const pending = new Promise<void>(resolve => { release = resolve; });
  let first = true;
  await page.route("**/api/chat", async route => {
    if (first) { first = false; await pending; }
    await route.fulfill({ contentType: "text/event-stream", body: 'data: {"content":"Test reply"}\n\ndata: [DONE]\n\n' });
  });
  await start(page);
  await page.getByRole("textbox").fill("Question");
  const sent = page.waitForRequest("**/api/chat");
  await page.getByRole("button", { name: "Send message", exact: true }).click();
  await sent;
  await expect(picker(page)).toBeDisabled();
  release();
  await expect(page.getByText("Test reply", { exact: true })).toBeVisible();
  await expect(picker(page)).toBeEnabled();
  await choose(page, "reasoning_effort", "max");
  const retry = page.waitForRequest("**/api/chat");
  await page.getByTitle("Retry the answer", { exact: true }).click();
  expect((await retry).postDataJSON().reasoning).toEqual({ reasoning_effort: "max", thinking: "enabled" });
});

for (const width of [390, 320]) {
test(`edge picker and popover fit a ${width}px screen without overlapping the checkbox or Send`, async ({ page }, testInfo) => {
  await page.setViewportSize({ width, height: 844 });
  await start(page);
  const send = await page.getByRole("button", { name: "Send message", exact: true }).boundingBox();
  const reasoning = await picker(page).boundingBox();
  const checkbox = await page.locator('label[for="checkbox-attached-files-only"]').boundingBox();
  const frame = await page.getByTestId("message-input-frame").boundingBox();
  const bottom = frame!.y + frame!.height;
  expect(reasoning!.y).toBeLessThan(bottom);
  expect(reasoning!.y + reasoning!.height).toBeGreaterThan(bottom);
  expect(checkbox!.x + checkbox!.width).toBeLessThanOrEqual(reasoning!.x);
  expect(send!.y + send!.height).toBeLessThanOrEqual(reasoning!.y);
  expect(send!.x + send!.width).toBeLessThanOrEqual(width);
  await page.screenshot({ path: testInfo.outputPath("reasoning-edge.png") });
  await openPicker(page);
  const bounds = await popup(page).boundingBox();
  expect(bounds!.x).toBeGreaterThanOrEqual(0);
  expect(bounds!.x + bounds!.width).toBeLessThanOrEqual(width);
  expect(bounds!.y).toBeGreaterThanOrEqual(0);
  expect(bounds!.y + bounds!.height).toBeLessThanOrEqual(844);
  await page.screenshot({ path: testInfo.outputPath("reasoning-mobile.png") });
});
}

test("Apply persists every parameter together through the native bridge", async ({ page }) => {
  await page.addInitScript(() => {
    const saved: Record<string, string> = { ReasoningSelections: JSON.stringify({
      "provider-a": { reasoning_effort: "high", thinking: "disabled" },
    }) };
    (window as any).nativeReasoningWrites = [];
    (window as any).cppApi = {
      getPersistentKey: async (key: string) => saved[key] ?? null,
      setPersistentKey: async (key: string, value: string) => {
        saved[key] = value;
        if (key === "ReasoningSelections") (window as any).nativeReasoningWrites.push(JSON.parse(value));
      },
      getServerUrl: async () => undefined,
    };
  });
  await start(page);
  await expect(picker(page)).toHaveAttribute("title", "reasoning_effort: high; thinking: disabled");
  await openPicker(page);
  await parameter(page, "reasoning_effort").getByRole("radio", { name: "max", exact: true }).check();
  await parameter(page, "thinking").getByRole("radio", { name: "adaptive", exact: true }).check();
  expect(await page.evaluate(() => (window as any).nativeReasoningWrites)).toHaveLength(0);
  await apply(page);
  await expect.poll(() => page.evaluate(() => (window as any).nativeReasoningWrites)).toEqual([
    { "provider-a": { reasoning_effort: "max", thinking: "adaptive" } },
  ]);
});

test("attachment loading keeps the provider and applied values chosen at Send", async ({ page }) => {
  await start(page);
  await choose(page, "reasoning_effort", "high");
  await choose(page, "thinking", "adaptive");
  await page.evaluate(() => {
    const OriginalFileReader = window.FileReader;
    window.FileReader = class extends OriginalFileReader {
      readAsText(file: Blob) {
        (window as any).resumeAttachmentRead = () => super.readAsText(file);
      }
    };
  });
  await page.locator("#file-input").setInputFiles({ name: "context.txt", mimeType: "text/plain", buffer: Buffer.from("Attached context") });
  await page.getByRole("textbox").fill("Use this attachment");
  await page.getByRole("button", { name: "Send message", exact: true }).click();
  await switchProvider(page, "provider-b");
  await choose(page, "reasoning_effort", "deep");
  const request = page.waitForRequest("**/api/chat");
  await page.evaluate(() => (window as any).resumeAttachmentRead());
  expect((await request).postDataJSON()).toMatchObject({ targetapi: "provider-a",
    reasoning: { reasoning_effort: "high", thinking: "adaptive" },
    attachments: [{ filename: "context.txt", content: "Attached context" }],
  });
});

test("arbitrary parameter names and values remain independent without synthesized modes", async ({ page }) => {
  await page.route("**/api/settings", route => route.fulfill({ json: {
    completionApis: [{ ...providers[0], reasoning: { parameters: [
      { name: "scheduler_mode", values: ["off", "custom"], default: "off", value_type: "object", value_field: "mode" },
      { name: "budget_class", values: ["red", "blue"], default: "red", value_type: "scalar" },
    ] } }], currentApi: "provider-a",
  } }));
  await start(page);
  await expect(picker(page)).toHaveAttribute("title", "scheduler_mode: off; budget_class: red");
  await openPicker(page);
  await expect(parameter(page, "scheduler_mode").getByRole("radio")).toHaveCount(2);
  await expect(parameter(page, "budget_class").getByRole("radio", { name: "blue", exact: true })).toBeEnabled();
  await parameter(page, "budget_class").getByRole("radio", { name: "blue", exact: true }).check();
  await apply(page);
  await expect(picker(page)).toHaveText("Reasoning");
  await expect(picker(page)).toHaveAttribute("title", "scheduler_mode: off; budget_class: blue");
  await page.getByRole("textbox").fill("Question");
  const request = page.waitForRequest("**/api/chat");
  await page.getByRole("button", { name: "Send message", exact: true }).click();
  expect((await request).postDataJSON().reasoning).toEqual({ scheduler_mode: "off", budget_class: "blue" });
});

test("changing provider discards the open draft", async ({ page }) => {
  await start(page);
  await openPicker(page);
  await parameter(page, "reasoning_effort").getByRole("radio", { name: "max", exact: true }).check();
  await switchProvider(page, "provider-b");
  await expect(popup(page)).toBeHidden();
  await expect(picker(page)).toHaveAttribute("title", "reasoning_effort: standard");
  await switchProvider(page, "provider-a");
  await openPicker(page);
  await expect(parameter(page, "reasoning_effort").getByRole("radio", { name: "medium", exact: true })).toBeChecked();
});

test("keyboard radio navigation and Apply do not submit a chat", async ({ page }) => {
  const requests: string[] = [];
  page.on("request", request => { if (new URL(request.url()).pathname === "/api/chat") requests.push(request.url()); });
  await start(page);
  await page.getByRole("textbox").fill("Keep draft");
  await picker(page).focus();
  await page.keyboard.press("Enter");
  await expect(popup(page)).toBeVisible();
  await parameter(page, "reasoning_effort").getByRole("radio", { name: "medium", exact: true }).focus();
  await page.keyboard.press("ArrowRight");
  await expect(parameter(page, "reasoning_effort").getByRole("radio", { name: "high", exact: true })).toBeChecked();
  await popup(page).getByRole("button", { name: "Apply", exact: true }).focus();
  await page.keyboard.press("Enter");
  await expect(popup(page)).toBeHidden();
  await expect(picker(page)).toHaveAttribute("title", "reasoning_effort: high; thinking: enabled");
  await expect(page.getByRole("textbox")).toHaveValue("Keep draft");
  expect(requests).toHaveLength(0);
});

test("replay references follow completed responses and edits invalidate the following turns", async ({ page }) => {
  const requests: any[] = [];
  await page.route("**/api/chat", route => {
    const request = route.request().postDataJSON();
    requests.push(request);
    return route.fulfill({ contentType: "text/event-stream", body:
      `data: ${JSON.stringify({ content: `Reply ${requests.length}` })}\n\n` +
      `data: ${JSON.stringify({ type: "context_sources", sources: null, replay_id: request.response_id })}\n\n` +
      "data: [DONE]\n\n" });
  });
  await start(page);
  async function send(text: string, reply: number) {
    await page.getByRole("textbox").fill(text);
    await page.getByRole("button", { name: "Send message", exact: true }).click();
    await expect(page.getByText(`Reply ${reply}`, { exact: true })).toBeVisible();
    await expect(page.getByTitle("Edit your original question", { exact: true }).first()).toBeEnabled();
  }
  await send("Original question", 1);
  await send("Follow up", 2);
  expect(requests[1].messages[1].replay_id).toBe(requests[0].response_id);
  expect(requests[1].messages[1].content).toBe("Reply 1");
  await expect.poll(() => page.evaluate(() => {
    const chats = JSON.parse(localStorage.getItem("PhenixCode.chats") || "[]");
    return chats[0]?.messages[1]?.replay;
  })).toEqual({ id: requests[0].response_id, session: requests[0].session_id });
  await page.getByTitle("Edit your original question", { exact: true }).first().click();
  await page.locator("#user-message-0").fill("Edited question");
  await page.getByRole("textbox").click();
  await send("After edit", 3);
  expect(requests[2].messages[0].content).toBe("Edited question");
  expect(requests[2].messages.filter((message: any) => message.replay_id)).toHaveLength(0);
  expect(requests[2].session_id).toBe(requests[0].session_id);
  expect(requests[2].response_id).not.toBe(requests[1].response_id);
  expect(JSON.stringify(requests)).not.toContain("reasoning_content");
});

test("reopening a saved chat restores its opaque replay references and backend session", async ({ page }) => {
  await page.addInitScript(() => localStorage.setItem("PhenixCode.chats", JSON.stringify([{
    id: "retained-chat", project_id: "project", title: "Retained chat", updated_at: 1,
    messages: [{ role: "user", content: "Earlier question" },
      { role: "assistant", content: "Earlier answer", replay: { id: "retained-reply", session: "retained-session" } }],
  }])));
  await start(page);
  await page.getByTitle("View chat history", { exact: true }).click();
  const row = page.locator('article[data-chat-id="retained-chat"]');
  await row.getByRole("button").click();
  await row.getByRole("button", { name: "Open", exact: true }).click();
  await expect(page.getByText("Earlier answer", { exact: true })).toBeVisible();
  await page.getByRole("textbox").fill("Continue");
  const outgoing = page.waitForRequest("**/api/chat");
  await page.getByRole("button", { name: "Send message", exact: true }).click();
  expect((await outgoing).postDataJSON()).toMatchObject({ session_id: "retained-session",
    messages: [{ role: "user", content: "Earlier question" },
      { role: "assistant", content: "Earlier answer", replay_id: "retained-reply" },
      { role: "user", content: "Continue" }] });
});
