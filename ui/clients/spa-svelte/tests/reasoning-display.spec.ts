import { expect, test } from "@playwright/test";
import type { Page } from "@playwright/test";

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

test("nested reasoning streams independently, preserves metadata, and stays out of saved and outgoing answers", async ({ page }) => {
  await expect(assistants(page)).toHaveCount(0);
  await send(page, "Question");
  await emit(page, [
    { type: "reasoning_delta", content: "**First thought** ✓\n\n" },
    { content: "[meta]Working" },
  ]);
  await expect(assistants(page)).toHaveCount(1);
  const reply = assistants(page).first();
  await expect(reply.locator(".message-content")).toHaveText("");
  const ready = reply.getByRole("button", { name: "Ready", exact: true });
  await expect(ready).toHaveAttribute("aria-expanded", "false");
  await expect(reply.getByRole("button", { name: "Reasoning", exact: true })).toHaveCount(0);
  await ready.click();
  await expect(reply.getByText("Working ✓", { exact: true })).toBeVisible();
  const reasoning = reply.getByRole("button", { name: "Reasoning", exact: true });
  await expect(reasoning).toHaveAttribute("aria-expanded", "false");
  await reasoning.click();
  const region = reply.getByRole("region", { name: "Reasoning content" });
  await expect(region.locator("strong")).toHaveText("First thought");
  await emit(page, [
    { content: "Final answer" },
    { type: "reasoning_delta", content: "Second thought." },
    { content: "[meta]Approx. cost incurred: $0.001" },
  ], true);
  await expect(reply.locator(".message-content")).toHaveText("Final answer");
  await expect(region).toHaveText("First thought ✓ Second thought.");
  await expect(reply.getByText("Approx. cost incurred: $0.001 ✓", { exact: true })).toBeVisible();
  await ready.click();
  await expect(region).toHaveCount(0);
  await ready.click();
  await expect(region).toBeVisible();
  await expect(reasoning).toHaveAttribute("aria-expanded", "true");
  await send(page, "Next question");
  const requests = await page.evaluate(() => (window as any).reasoningRequests);
  expect(requests[1].messages[1]).toEqual({ role: "assistant", content: "Final answer" });
  expect(JSON.stringify(requests)).not.toContain("First thought");
  await emit(page, [{ content: "Second answer" }], true);
  await expect(assistants(page).last().locator(".message-content")).toHaveText("Second answer");
  await expect(assistants(page).last().getByRole("button", { name: "Reasoning", exact: true })).toHaveCount(0);
  const saved = await page.evaluate(() => localStorage.getItem("PhenixCode.chats"));
  expect(saved).toContain("Final answer");
  expect(saved).not.toContain("First thought");
});

test("reasoning-only completion preserves the question and retry replaces the response", async ({ page }) => {
  await send(page, "Original question");
  await emit(page, [{ type: "reasoning_delta", content: "Reasoning without an answer." }], true);
  const reply = assistants(page).first();
  await reply.getByRole("button", { name: "Ready", exact: true }).click();
  await reply.getByRole("button", { name: "Reasoning", exact: true }).click();
  await expect(reply.getByRole("region")).toHaveText("Reasoning without an answer.");
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
  await reply.getByRole("button", { name: "Ready", exact: true }).click();
  await reply.getByRole("button", { name: "Reasoning", exact: true }).click();
  await expect(reply.getByRole("region")).toHaveText("Replacement reasoning.");
});

test("long reasoning scrolls within a narrow viewport and sanitized Markdown renders", async ({ page }) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await send(page, "Question");
  await emit(page, [{ type: "reasoning_delta", content:
    '**Thought** <img src="x" onerror="window.reasoningInjected = true">\n\n' + "A long paragraph of reasoning.\n\n".repeat(80) },
    { content: "Answer" }], true);
  const reply = assistants(page).first();
  await reply.getByRole("button", { name: "Ready", exact: true }).click();
  await reply.getByRole("button", { name: "Reasoning", exact: true }).click();
  const region = reply.getByRole("region", { name: "Reasoning content" });
  await expect(region.locator("strong")).toHaveText("Thought");
  await expect(region.locator("[onerror]")).toHaveCount(0);
  await expect(region).toHaveCSS("overflow-y", "auto");
  const bounds = await region.evaluate(element => ({
    height: element.clientHeight, scroll: element.scrollHeight, right: element.getBoundingClientRect().right,
  }));
  expect(bounds.scroll).toBeGreaterThan(bounds.height);
  expect(bounds.height).toBeLessThanOrEqual(128);
  expect(bounds.right).toBeLessThanOrEqual(390);
  expect(await page.evaluate(() => (window as any).reasoningInjected)).toBeUndefined();
});
