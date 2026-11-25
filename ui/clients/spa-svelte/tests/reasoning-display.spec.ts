import { expect, test } from "@playwright/test";
import type { Locator, Page } from "@playwright/test";

test.beforeEach(async ({ page }) => {
  await page.route("**/api/**", route => {
    const path = new URL(route.request().url()).pathname;
    const json = path === "/api/settings" ? {
      completionApis: [{ id: "provider", name: "Provider", model: "model", current: true, combinedPrice: 0 }],
      currentApi: "provider",
    } : path === "/api/instances" ? {
      instances: [{ id: "instance", project_id: "project", name: "Project", host: "localhost", port: 8590 }],
      current_instance: "instance",
    } : { status: "success" };
    return route.fulfill({ json });
  });
  await page.addInitScript(() => {
    const originalFetch = window.fetch.bind(window);
    const requests: unknown[] = [];
    let controller: ReadableStreamDefaultController<Uint8Array>;
    (window as any).reasoningRequests = requests;
    (window as any).emitReasoningEvents = (events: unknown[], finish: boolean) => {
      const frames = events.map(event => "data: " + JSON.stringify(event) + "\n\n").join("") +
        (finish ? "data: [DONE]\n\n" : "");
      const bytes = new TextEncoder().encode(frames);
      // Exercise fragmented JSON, delimiters, and multi-byte UTF-8 text.
      for (let offset = 0; offset < bytes.length; offset += 7)
        controller.enqueue(bytes.slice(offset, offset + 7));
      if (finish) controller.close();
    };
    window.fetch = async (input, init) => {
      const url = input instanceof Request ? input.url : String(input);
      if (new URL(url, location.href).pathname !== "/api/chat") return originalFetch(input, init);
      requests.push(JSON.parse(String(init?.body)));
      return new Response(new ReadableStream<Uint8Array>({ start(value) { controller = value; } }), {
        headers: { "Content-Type": "text/event-stream" },
      });
    };
  });
  await page.goto("/");
  await expect(page.getByText("Connected.", { exact: true })).toBeVisible();
});

const assistants = (page: Page) => page.locator('[data-role="assistant"]');
async function send(page: Page, question: string) {
  await page.getByRole("textbox").fill(question);
  await page.getByRole("button", { name: "Send message", exact: true }).click();
}
async function emit(page: Page, events: unknown[], finish = false) {
  await page.evaluate(({ events, finish }) => (window as any).emitReasoningEvents(events, finish), { events, finish });
}

async function showReasoning(reply: Locator) {
  const summary = reply.locator("summary").filter({ hasText: /^Thought process$/ });
  const region = reply.getByRole("region", { name: "Reasoning content", exact: true });
  if (!await region.isVisible()) await summary.click();
  return region;
}

test("thought process streams independently and stays out of history and follow-up requests", async ({ page }) => {
  await send(page, "Question");
  await emit(page, [{ type: "reasoning_delta", content: "**First thought** 🔍\n\n" }]);
  const reply = assistants(page).first();
  const region = reply.getByRole("region", { name: "Reasoning content" });
  await expect(region).toBeHidden();
  await expect(reply.locator(".message-content")).toHaveCount(0);
  await expect(reply.getByTitle("Retry the answer")).toHaveCount(0);
  await showReasoning(reply);
  await expect(region.locator("strong")).toHaveText("First thought");
  await emit(page, [{ type: "reasoning_delta", content: "Second thought." }]);
  await expect(region).toContainText("Second thought.", { timeout: 1000 });
  await emit(page, [{ content: "Final answer" }]);
  await expect(reply.locator(".message-content")).toHaveText("Final answer");
  await expect(region).toBeVisible();
  await emit(page, [{ type: "reasoning_delta", content: " Final detail." },
    { type: "meta", content: "Approx. cost incurred: $0.001" }], true);
  await expect(region).toContainText("Second thought. Final detail.");
  await expect(reply.locator("summary").filter({ hasText: /tool calls?/ })).toHaveCount(0);
  await reply.locator("summary").filter({ hasText: "Response details" }).click();
  await expect(reply.getByText("Approx. cost incurred: $0.001", { exact: true })).toBeVisible();
  await reply.locator("summary").filter({ hasText: "Thought process" }).click();
  await expect(region).toBeHidden();
  await send(page, "Next question");
  const requests = await page.evaluate(() => (window as any).reasoningRequests);
  expect(requests[1].messages[1]).toEqual({ role: "assistant", content: "Final answer" });
  await emit(page, [{ content: "Second answer" }], true);
  await expect(assistants(page).last().locator(".message-content")).toHaveText("Second answer");
  const saved = await page.evaluate(() => localStorage.getItem("PhenixCode.chats"));
  expect(saved).toContain("Final answer");
  expect(saved).not.toContain("First thought");
});

test("reasoning-only completion preserves the question and retry replaces the response", async ({ page }) => {
  await send(page, "Original question");
  await emit(page, [{ type: "reasoning_delta", content: "Reasoning without an answer." }], true);
  const reply = assistants(page).first();
  await showReasoning(reply);
  await expect(reply.getByRole("region", { name: "Reasoning content", exact: true })).toHaveText("Reasoning without an answer.");
  await expect(page.locator('[data-role="user"] .message-content')).toHaveText("Original question");
  await reply.getByTitle("Retry the answer", { exact: true }).click();
  const requests = await page.evaluate(() => (window as any).reasoningRequests);
  expect(requests[1].messages).toEqual([{ role: "user", content: "Original question" }]);
  await emit(page, [
    { type: "reasoning_delta", content: "Replacement reasoning." },
    { error: "Provider failed" },
  ], true);
  await expect(assistants(page)).toHaveCount(1);
  await expect(reply.locator(".message-content")).toContainText("Provider failed");
  await showReasoning(reply);
  await expect(reply.getByRole("region", { name: "Reasoning content", exact: true })).toHaveText("Replacement reasoning.");
});

test("live and completed reasoning wrap long prose, code, and tables without horizontal scrolling", async ({ page }, testInfo) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await send(page, "Question");
  await emit(page, [{ type: "reasoning_delta", content:
    '**Thought** <img src="x" onerror="window.reasoningInjected = true">\n\n' + "A long paragraph of reasoning.\n\n".repeat(80) +
    "https://example.test/" + "long_path_".repeat(100) + "\n\n```cpp\n" + 'const auto text = "' + "unbroken".repeat(100) + '";\n```\n\n' +
    "| Key | Value |\n| --- | --- |\n| Path | " + "identifier".repeat(100) + " |" }]);
  const reply = assistants(page).first();
  const verifyWrapping = async (region: Locator) => {
    await expect(region.locator("strong")).toHaveText("Thought");
    await expect(region.locator("table")).toBeVisible();
    await expect(region.locator("[onerror]")).toHaveCount(0);
    await expect(region).toHaveCSS("overflow-y", "auto");
    const bounds = await region.evaluate(element => ({
      height: element.clientHeight, scroll: element.scrollHeight, right: element.getBoundingClientRect().right,
      maxHeight: Number.parseFloat(getComputedStyle(element).maxHeight),
      overflowing: [element, ...element.querySelectorAll("*")].filter(node => node.scrollWidth > node.clientWidth + 1).map(node => node.tagName),
    }));
    expect(bounds.scroll).toBeGreaterThan(bounds.height);
    expect(bounds.maxHeight).toBeGreaterThan(0);
    expect(bounds.height).toBeLessThanOrEqual(bounds.maxHeight);
    expect(bounds.right).toBeLessThanOrEqual(390);
    expect(bounds.overflowing).toEqual([]);
  };
  await verifyWrapping(await showReasoning(reply));
  await emit(page, [{ content: "Answer" }], true);
  const region = await showReasoning(reply);
  await verifyWrapping(region);
  await page.screenshot({ path: testInfo.outputPath("wrapped-reasoning.png") });
  expect(await page.evaluate(() => (window as any).reasoningInjected)).toBeUndefined();
});

test("activity is height bounded and keeps the reader's position when new entries arrive", async ({ page }) => {
  await page.setViewportSize({ width: 390, height: 600 });
  await send(page, "Inspect many files");
  await emit(page, [{ type: "reasoning_delta", content: "Investigating." }, { type: "meta", content: "Starting" }]);
  const reply = assistants(page).first();
  await reply.locator("summary").filter({ hasText: "Response details" }).click();
  const activity = reply.getByRole("region", { name: "Response activity", exact: true });
  await expect.poll(() => activity.evaluate(node => node.scrollHeight <= node.clientHeight)).toBe(true);
  const entry = (index: number) => ({ type: "meta", event_id: `entry:${index}`, status: "success", content: `Read src/component_${index}.cpp:100-200 — evidence gathered` });
  await emit(page, Array.from({ length: 45 }, (_, index) => entry(index)));
  await expect(activity.locator("[data-meta-id]")).toHaveCount(45);
  await expect.poll(() => activity.evaluate(node => node.clientHeight)).toBe(240);
  await activity.focus();
  await page.keyboard.press("End");
  await expect.poll(() => activity.evaluate(node => Math.abs(node.scrollTop + node.clientHeight - node.scrollHeight))).toBeLessThan(1);
  const anchor = await activity.evaluate(node => {
    node.scrollTop = node.scrollHeight / 2;
    const top = node.getBoundingClientRect().top;
    const row = [...node.querySelectorAll<HTMLElement>("[data-meta-id]")].find(row => row.getBoundingClientRect().top > top)!;
    return { id: row.dataset.metaId!, offset: row.getBoundingClientRect().top - top };
  });
  await emit(page, [entry(45), entry(46)]);
  await expect(activity.locator("[data-meta-id]")).toHaveCount(47);
  await expect.poll(() => activity.locator(`[data-meta-id="${anchor.id}"]`).evaluate(node =>
    node.getBoundingClientRect().top - node.parentElement!.getBoundingClientRect().top)).toBeCloseTo(anchor.offset, 0);
  expect(await activity.evaluate(node => node.scrollWidth <= node.clientWidth)).toBe(true);
  await emit(page, [{ content: "Answer" }], true);
  await expect(reply.locator("summary").filter({ hasText: "Thought process" })).toBeVisible();
  expect(await activity.evaluate(node => node.contains(node.parentElement!.querySelector("button")))).toBe(false);
  await page.setViewportSize({ width: 1280, height: 900 });
  await expect.poll(() => activity.evaluate(node => node.clientHeight)).toBe(288);
});


test("tool metadata updates one row per call and preserves distinct outcomes within a round", async ({ page }, testInfo) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await send(page, "Inspect tools");
  const first = { type: "meta", event_id: "tool:1:first", tool: "grep", round: 1, call_id: "first",
    args: { pattern: "onInfo" }, action: "Search onInfo", status: "running", content: "Starting grep" };
  const second = { ...first, event_id: "tool:1:second", call_id: "second", args: {}, action: "Second search" };
  await emit(page, [first]);
  const reply = assistants(page).first();
  await expect(reply.locator(".message-content")).toHaveCount(0);
  const header = reply.locator("summary").filter({ hasText: /tool calls?/ }).first();
  await expect(header).toContainText("Working");
  await header.click();
  const rows = reply.locator("[data-meta-id^='tool:']");
  await rows.first().locator("summary").focus();
  await page.keyboard.press("Enter");
  await expect(rows.first().getByLabel("Running")).toBeVisible();
  await expect(rows.first().locator("pre")).toContainText('"pattern": "onInfo"');
  await expect(rows.first().locator("pre .hljs-attr")).toHaveText('"pattern"');
  await expect(rows.first().locator("pre .hljs-string")).toHaveText('"onInfo"');
  await emit(page, [{ ...first, status: "info", duration_ms: 0, outcome_summary: "No matches",
    result_preview: '{"matches": []}', result_preview_truncated: false }, second]);
  await expect(rows).toHaveCount(2);
  await expect(rows.first()).toHaveAttribute("open", "");
  await expect(rows.first()).toContainText("0 ms");
  await expect(rows.first().getByText('{"matches": []}', { exact: true })).toBeVisible();
  await expect(rows.first().locator("pre").last().locator(".hljs-attr")).toHaveText('"matches"');
  await expect(rows.nth(1)).not.toHaveAttribute("open", "");
  const longPreview = '{"count":1,"truncated":true,"text":"<img src=x onerror=alert(1)> ' + "match".repeat(750);
  await emit(page, [{ ...second, status: "error", duration_ms: 5, outcome_summary: "Failed",
    result_preview: longPreview, result_preview_truncated: true }, { content: "Final answer" }], true);
  await expect(header).toContainText("2 tool calls · 5 ms in tools");
  await expect(header).toContainText("1 failed");
  await rows.nth(1).locator("summary").click();
  await expect(rows.nth(1).getByLabel("Failed", { exact: true })).toBeVisible();
  await expect(rows.nth(1).locator("img")).toHaveCount(0);
  const clippedResult = rows.nth(1).locator("pre").last();
  await expect(clippedResult.locator(".hljs-number")).toHaveText("1");
  await expect(clippedResult.locator(".hljs-literal")).toHaveText("true");
  await expect(clippedResult.locator(".hljs-string")).toContainText("<img src=x");
  const toolList = reply.getByRole("region", { name: "Tool calls", exact: true });
  const shortDetails = rows.first().getByRole("region", { name: "grep details" });
  const longDetails = rows.nth(1).getByRole("region", { name: "grep details" });
  await expect(toolList).toHaveCSS("max-height", "none");
  await expect(toolList).toHaveCSS("overflow-y", "visible");
  expect(await shortDetails.evaluate(node => node.scrollHeight - node.clientHeight)).toBeLessThanOrEqual(1);
  expect(await longDetails.evaluate(node => node.scrollHeight - node.clientHeight)).toBeGreaterThan(0);
  expect(await longDetails.evaluate(node => node.scrollWidth - node.clientWidth)).toBeLessThanOrEqual(1);
  await expect(longDetails.locator("pre").first()).toHaveCSS("overflow-y", "visible");
  await expect(longDetails.locator("pre").last()).toHaveCSS("overflow-y", "visible");
  await page.evaluate(() => Object.defineProperty(navigator, "clipboard", { configurable: true, value: {
    writeText: async (text: string) => { (window as any).copiedPreview = text; },
  } }));
  await rows.nth(1).getByRole("button", { name: "Copy preview" }).click();
  await expect.poll(() => page.evaluate(() => (window as any).copiedPreview)).toBe(longPreview);
  expect((await rows.first().boundingBox())!.x + (await rows.first().boundingBox())!.width).toBeLessThanOrEqual(390);
  await page.screenshot({ path: testInfo.outputPath("expanded-tool-trace.png") });
  await reply.getByTitle("Retry the answer").click();
  await emit(page, [first, { error: "Connection ended" }], true);
  await expect(header).toContainText("Interrupted");
  await expect(header).not.toContainText("ms in tools");
  await expect(rows.first()).toBeHidden();
  await header.click();
  await expect(rows.first()).not.toHaveAttribute("open", "");
  await expect(reply.locator(".message-content")).toContainText("Connection ended");
  const saved = await page.evaluate(() => localStorage.getItem("PhenixCode.chats"));
  expect(saved).toContain("Connection ended");
  expect(saved).not.toContain("result_preview");
});

test("saved metadata supports legacy strings and structured failures without adding success marks", async ({ page }) => {
  await page.evaluate(async () => {
    const { messages } = await import("/src/lib/store.ts");
    messages.set([{ role: "assistant", content: "Saved response", _html: "Saved response", _metaInfoArray: [
      "Legacy metadata", { content: "Saved failed call", status: "error", event_id: "saved" },
      { tool: "grep", status: "info", event_id: "old-tool", content: "grep(pattern=needle) returned no matches", summary: "Search needle" },
    ] }]);
  });
  const reply = assistants(page).last();
  await reply.locator("summary").filter({ hasText: "Response details" }).click();
  await expect(reply.getByText("Legacy metadata", { exact: true })).toBeVisible();
  await expect(reply.locator('[data-meta-status="info"] svg')).toHaveCount(2);
  await expect(reply.locator('[data-meta-id="saved"] [aria-label="Failed"]')).toBeVisible();
  await expect(reply.locator('[aria-label="Completed"]')).toHaveCount(0);
  await reply.locator("summary").filter({ hasText: "1 tool call" }).click();
  const legacyTool = reply.locator('[data-meta-id="old-tool"]');
  await legacyTool.locator("summary").click();
  await expect(legacyTool.locator("pre")).toHaveText("grep(pattern=needle) returned no matches");
});

test("the transcript scrolls independently of the composer", async ({ page }) => {
  await page.setViewportSize({ width: 890, height: 540 });
  await send(page, "Inspect the code");
  await emit(page, [
    { type: "meta", tool: "search_context", event_id: "tool:1:search", status: "success", content: "Search complete",
      args: { query: "tool definitions" }, result_preview: "Result line\n".repeat(120) },
    { content: "Paragraph of evidence.\n\n".repeat(100) },
  ], true);

  const transcript = page.getByRole("region", { name: "Chat messages", exact: true });
  const composer = page.getByTestId("message-input-frame");
  const bottom = page.getByRole("button", { name: "Scroll to bottom" });
  await expect(page.getByRole("textbox")).toBeEnabled();
  const composerY = (await composer.boundingBox())!.y;
  await transcript.evaluate(node => { node.scrollTop = 0; });
  await transcript.locator("summary").filter({ hasText: "1 tool call" }).click();
  await transcript.locator('[data-meta-id="tool:1:search"] summary').click();
  expect((await composer.boundingBox())!.y).toBeCloseTo(composerY, 0);
  expect(await transcript.evaluate(node => node.getBoundingClientRect().bottom)).toBeLessThan(composerY);
  await bottom.click();
  await expect.poll(() => transcript.evaluate(node => node.scrollHeight - node.scrollTop - node.clientHeight))
    .toBeLessThanOrEqual(1);
  await expect(bottom).toBeHidden();
  expect((await composer.boundingBox())!.y).toBeCloseTo(composerY, 0);
  await transcript.evaluate(node => { node.scrollTop = 0; });
  await expect(bottom).toBeVisible();
  expect((await composer.boundingBox())!.y).toBeCloseTo(composerY, 0);
});
