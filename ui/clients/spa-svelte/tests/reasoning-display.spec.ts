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

async function showCompletedReasoning(reply: Locator) {
  const ready = reply.getByRole("button", { name: "Ready", exact: true });
  if (await ready.getAttribute("aria-expanded") !== "true") await ready.click();
  const reasoning = reply.getByRole("button", { name: "Reasoning", exact: true });
  if (await reasoning.getAttribute("aria-expanded") !== "true") await reasoning.click();
  return reply.getByRole("region", { name: "Reasoning content", exact: true });
}

test("reasoning appears live without an empty answer bubble and returns under Ready after completion", async ({ page }, testInfo) => {
  await expect(assistants(page)).toHaveCount(0);
  await send(page, "Question");
  await emit(page, [
    { type: "meta", content: "Working" },
    { type: "reasoning_delta", content: "**First thought** ✓\n\n" },
  ]);
  await expect(assistants(page)).toHaveCount(1);
  const reply = assistants(page).first();
  const region = reply.getByRole("region", { name: "Reasoning content", exact: true });
  await expect(region).toBeVisible();
  await expect(region.locator("strong")).toHaveText("First thought");
  await expect(reply.locator(".message-content")).toHaveCount(0);
  await expect(reply.getByTitle("Copy to clipboard", { exact: true })).toHaveCount(0);
  await expect(reply.getByTitle("Retry the answer", { exact: true })).toHaveCount(0);
  await expect(reply.getByRole("button", { name: "Ready", exact: true })).toHaveCount(0);
  await expect(reply.getByRole("button", { name: "Working…", exact: true })).toBeVisible();
  await expect(reply.getByRole("button", { name: "Reasoning…", exact: true }).last()).toHaveAttribute("aria-expanded", "true");
  await page.screenshot({ path: testInfo.outputPath("live-reasoning.png") });
  // A sparse second delta must render without waiting for a third network batch.
  await emit(page, [{ type: "reasoning_delta", content: "Second thought." }]);
  await expect(region).toContainText("Second thought.", { timeout: 1000 });
  await emit(page, [{ type: "meta", content: "Step 1/10: grep(needle)" }]);
  await expect(region).toBeVisible();
  await expect(reply.getByRole("status")).toHaveText("Step 1/10: grep(needle)");
  await expect(reply.getByRole("button", { name: "Ready", exact: true })).toHaveCount(0);
  await emit(page, [{ content: "Final answer" }]);
  await expect(reply.locator(".message-content")).toHaveText("Final answer");
  await expect(region).toHaveCount(0);
  // Completion must flush the last reasoning delta even before its render timer fires.
  await emit(page, [
    { type: "reasoning_delta", content: " Final detail." },
    { type: "meta", content: "Approx. cost incurred: $0.001" },
  ], true);
  const ready = reply.getByRole("button", { name: "Ready", exact: true });
  await expect(ready).toHaveAttribute("aria-expanded", "false");
  const completed = await showCompletedReasoning(reply);
  await expect(completed).toHaveText("First thought ✓ Second thought. Final detail.");
  await expect(reply.getByText("Approx. cost incurred: $0.001", { exact: true })).toBeVisible();
  await ready.click();
  await expect(completed).toHaveCount(0);
  await ready.click();
  await expect(completed).toBeVisible();
  await send(page, "Next question");
  const requests = await page.evaluate(() => (window as any).reasoningRequests);
  expect(requests[1].messages[1]).toEqual({ role: "assistant", content: "Final answer" });
  expect(JSON.stringify(requests)).not.toContain("First thought");
  await emit(page, [{ type: "reasoning_delta", content: "A new thought" }]);
  await expect(assistants(page).last().getByRole("region", { name: "Reasoning content" })).toHaveText("A new thought");
  await expect(reply.getByRole("button", { name: "Ready", exact: true })).toBeVisible();
  await expect(completed).toHaveText("First thought ✓ Second thought. Final detail.");
  await emit(page, [{ content: "Second answer" }], true);
  await expect(assistants(page).last().locator(".message-content")).toHaveText("Second answer");
  const saved = await page.evaluate(() => localStorage.getItem("PhenixCode.chats"));
  expect(saved).toContain("Final answer");
  expect(saved).not.toContain("First thought");
});

test("a user can collapse live reasoning and explicitly keep it open when the answer arrives", async ({ page }) => {
  await send(page, "Question");
  await emit(page, [{ type: "reasoning_delta", content: "First thought." }]);
  const reply = assistants(page).first();
  const region = reply.getByRole("region", { name: "Reasoning content", exact: true });
  const toggle = reply.getByRole("button", { name: "Reasoning…", exact: true }).last();
  await expect(region).toBeVisible();
  await toggle.click();
  await expect(region).toHaveCount(0);
  await emit(page, [{ type: "reasoning_delta", content: " Second thought." }]);
  await expect(toggle).toHaveAttribute("aria-expanded", "false");
  await expect(region).toHaveCount(0);
  await toggle.click();
  await expect(region).toContainText("Second thought.");
  await emit(page, [{ content: "Final answer" }]);
  await expect(reply.locator(".message-content")).toHaveText("Final answer");
  await expect(region).toBeVisible();
  await emit(page, [], true);
  await expect(reply.getByRole("button", { name: "Ready", exact: true })).toHaveAttribute("aria-expanded", "true");
  await expect(region).toBeVisible();
  const completed = await showCompletedReasoning(reply);
  await expect(completed).toHaveText("First thought. Second thought.");
});

test("reasoning-only completion preserves the question and retry replaces the response", async ({ page }) => {
  await send(page, "Original question");
  await emit(page, [{ type: "reasoning_delta", content: "Reasoning without an answer." }], true);
  const reply = assistants(page).first();
  await expect(reply.getByRole("button", { name: "Ready", exact: true })).toBeVisible();
  await showCompletedReasoning(reply);
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
  await showCompletedReasoning(reply);
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
  await verifyWrapping(reply.getByRole("region", { name: "Reasoning content", exact: true }));
  await emit(page, [{ content: "Answer" }], true);
  const region = await showCompletedReasoning(reply);
  await verifyWrapping(region);
  await page.screenshot({ path: testInfo.outputPath("wrapped-reasoning.png") });
  expect(await page.evaluate(() => (window as any).reasoningInjected)).toBeUndefined();
});

test("activity is height bounded and keeps the reader's position when new entries arrive", async ({ page }) => {
  await page.setViewportSize({ width: 390, height: 600 });
  await send(page, "Inspect many files");
  await emit(page, [{ type: "reasoning_delta", content: "Investigating." }, { type: "meta", content: "Starting" }]);
  const reply = assistants(page).first();
  await reply.getByRole("button", { name: "Working…", exact: true }).click();
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
  await expect(reply.getByRole("button", { name: "Ready", exact: true })).toBeVisible();
  await expect(reply.getByRole("button", { name: "Reasoning", exact: true })).toBeVisible();
  expect(await activity.evaluate(node => node.contains(node.parentElement!.querySelector("button")))).toBe(false);
  await page.setViewportSize({ width: 1280, height: 900 });
  await expect.poll(() => activity.evaluate(node => node.clientHeight)).toBe(288);
});


test("tool metadata updates one row per call and preserves distinct outcomes within a round", async ({ page }, testInfo) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await send(page, "Inspect tools");
  const first = { type: "meta", event_id: "tool:1:first", round: 1, call_id: "first", call_index: 1, call_count: 2,
    status: "running", content: 'Round 1/10 · Call 1/2: grep(pattern="onInfo", path_filter="missing/")',
    summary: 'Round 1 · 1/2 · Search "onInfo" in missing/' };
  const generating = { type: "meta", event_id: "round:1", status: "running", content: "Round 1/10: generating response", summary: "Generating response..." };
  await emit(page, [{ type: "reasoning_delta", content: "Inspecting the source." }, generating]);
  const reply = assistants(page).first();
  await expect(reply.getByRole("status")).toHaveText("Generating response...");
  await reply.getByRole("button", { name: "Working…", exact: true }).click();
  await expect(reply.getByRole("status")).toHaveCount(0);
  await expect(reply.locator('[data-meta-id="round:1"]')).toHaveText("Generating response...");
  await emit(page, [{ ...generating, status: "info", content: "Round 1/10: requested 2 tool calls", summary: "Round 1 - 2 tools" }, first]);
  const firstRow = reply.locator('[data-meta-id="tool:1:first"]');
  await expect(firstRow).toHaveCount(1);
  await expect(firstRow).toHaveAttribute("data-meta-status", "running");
  await expect(firstRow.locator('[aria-label="Running"]')).toBeVisible();
  await expect(firstRow.locator('[aria-label="Completed"]')).toHaveCount(0);
  await expect(firstRow).not.toContainText("path_filter=");
  await expect(firstRow).toHaveAttribute("title", /path_filter="missing\/"/);
  await expect(firstRow).toHaveAttribute("title", /Call ID: first/);
  await reply.getByRole("button", { name: "Working…", exact: true }).click();
  await expect(reply.getByRole("status")).toHaveText(first.summary);
  await reply.getByRole("button", { name: "Working…", exact: true }).click();
  await expect(reply.getByRole("status")).toHaveCount(0);
  const second = { ...first, event_id: "tool:1:second", call_id: "second", call_index: 2,
    content: 'Round 1/10 · Call 2/2: grep(pattern="onInfo(", path_filter="src/")', summary: 'Round 1 · 2/2 · Search "onInfo(" in src/' };
  await emit(page, [{ ...first, status: "info", content: first.content + " — no files matched the filter", summary: first.summary + " — no files matched" }, second]);
  await expect(firstRow).toHaveCount(1);
  await expect(firstRow).toHaveAttribute("data-meta-status", "info");
  await expect(firstRow).toContainText('in missing/');
  await expect(firstRow.locator("svg")).toHaveCount(0);
  const secondRow = reply.locator('[data-meta-id="tool:1:second"]');
  await expect(secondRow).toHaveAttribute("data-meta-status", "running");
  await emit(page, [{ ...second, status: "error", content: second.content + " — failed: test failure", summary: second.summary + " — failed: test failure" },
    { type: "meta", event_id: "tool:1:third", status: "success", content: "Call 3 completed" }]);
  await expect(secondRow).toHaveCount(1);
  await expect(secondRow).toHaveAttribute("data-meta-status", "error");
  await expect(secondRow.locator('[aria-label="Failed"]')).toBeVisible();
  await expect(secondRow.locator('[aria-label="Completed"]')).toHaveCount(0);
  await expect(reply.locator('[data-meta-id="tool:1:third"] [aria-label="Completed"]')).toBeVisible();
  expect((await firstRow.boundingBox())!.x + (await firstRow.boundingBox())!.width).toBeLessThanOrEqual(390);
  await page.screenshot({ path: testInfo.outputPath("compact-tool-metadata.png") });
  await emit(page, [{ content: "Final answer" }], true);
  await expect(reply.getByRole("button", { name: "Ready", exact: true })).toBeVisible();
  await expect(firstRow).toHaveAttribute("data-meta-status", "info");
  await expect(secondRow).toHaveAttribute("data-meta-status", "error");
  await expect(reply.locator('[data-meta-status="running"]')).toHaveCount(0);
  // IDs are scoped to this reply; later requests may reuse them without altering history.
  await send(page, "Another turn");
  await emit(page, [{ type: "reasoning_delta", content: "Next thought" }, { ...first, content: "A later call" }]);
  await expect(secondRow).toHaveAttribute("data-meta-status", "error");
  await emit(page, [{ error: "Connection ended" }], true);
  const interrupted = assistants(page).last();
  await interrupted.getByRole("button", { name: "Ready", exact: true }).click();
  await expect(interrupted.locator('[data-meta-id="tool:1:first"]')).toHaveAttribute("data-meta-status", "warning");
  await expect(interrupted).toContainText("interrupted");
});

test("saved metadata supports legacy strings and structured failures without adding success marks", async ({ page }) => {
  await page.evaluate(async () => {
    const { messages } = await import("/src/lib/store.ts");
    messages.set([{ role: "assistant", content: "Saved response", _html: "Saved response", _metaInfoArray: [
      "Legacy metadata", { content: "Saved failed call", status: "error", event_id: "saved" },
    ] }]);
  });
  const reply = assistants(page).last();
  await reply.getByRole("button", { name: "Ready", exact: true }).click();
  await expect(reply.getByText("Legacy metadata", { exact: true })).toBeVisible();
  await expect(reply.locator('[data-meta-status="info"] svg')).toHaveCount(0);
  await expect(reply.locator('[data-meta-id="saved"] [aria-label="Failed"]')).toBeVisible();
  await expect(reply.locator('[aria-label="Completed"]')).toHaveCount(0);
});
