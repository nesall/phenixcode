import { expect, test } from "@playwright/test";
import type { Page } from "@playwright/test";

const fixtures = [
  { id: "first", project_id: "project", title: "First chat", updated_at: 200, created_at: 100,
    messages: [{ role: "user", content: "Original first question" }, { role: "assistant", content: "Original answer" }] },
  { id: "second", project_id: "project", title: "Second chat", updated_at: 150, created_at: 90,
    messages: [{ role: "user", content: "Second question" }] },
];
const row = (page: Page, id: string) => page.locator(`article[data-chat-id="${id}"]`);
const stored = (page: Page) => page.evaluate(() => JSON.parse(localStorage.getItem("PhenixCode.chats") || "[]"));
const history = (page: Page) => page.getByTitle("View chat history", { exact: true });
const search = (page: Page) => page.getByRole("searchbox", { name: "Search chats", exact: true });

test.beforeEach(async ({ page }) => {
  // Only test-origin localStorage and mocked service data; no user's database.
  await page.addInitScript(data => localStorage.setItem("PhenixCode.chats", JSON.stringify(data)), fixtures);
  await page.route("**/api/**", route => {
    const path = new URL(route.request().url()).pathname;
    const json = path === "/api/settings" ? { completionApis: [], currentApi: "" }
      : path === "/api/instances" ? { instances: [{ id: "instance", project_id: "project", name: "My project", host: "localhost", port: 8590 }], current_instance: "instance" }
      : { status: "success" };
    return route.fulfill({ json });
  });
  await page.goto("/");
  // Initial connection changes clear the editor. Let that finish before testing Open.
  await expect(page.getByText("Connected.", { exact: true })).toBeVisible();
  await history(page).click();
  await expect(row(page, "first")).toBeVisible();
});

test("rows expand singly; Close leaves editor alone; Open loads the original chat", async ({ page }, testInfo) => {
  await expect(page.getByRole("button", { name: "Delete", exact: true })).toHaveCount(0);
  await row(page, "first").getByRole("button").click();
  await expect(row(page, "first").getByRole("button").first()).toHaveAttribute("aria-expanded", "true");
  await expect(row(page, "first")).toContainText("My project");
  await expect(row(page, "first")).toContainText("Original first question");
  const preview = row(page, "first").locator("p").filter({ hasText: "Original first question" });
  await expect(preview).toBeVisible();
  await expect(preview.locator(":scope > span")).toHaveText(["You:Original first question", "Assistant:Original answer"]);
  await page.screenshot({ path: testInfo.outputPath("expanded-history.png") });
  await row(page, "second").getByRole("button").click();
  await expect(row(page, "first").getByRole("button", { name: "Delete", exact: true })).toHaveCount(0);
  await row(page, "second").getByRole("button", { name: "Close", exact: true }).click();
  await expect(row(page, "second").getByRole("button")).toHaveAttribute("aria-expanded", "false");
  await expect(page.getByRole("dialog")).toBeVisible();
  await row(page, "first").getByRole("button").click();
  await row(page, "first").getByRole("button", { name: "Open", exact: true }).click();
  await expect(page.getByRole("dialog")).toHaveCount(0);
  await expect(page.getByText("Original answer", { exact: true })).toBeVisible();
});

test("live search matches title, project, and both preview turns without reloading or reordering", async ({ page }) => {
  await page.evaluate(async () => {
    const api = await import("/src/lib/utils.ts");
    const rows = await api.listChats();
    (window as any).listCalls = 0;
    (window as any).cppApi = { listChats: async () => { (window as any).listCalls++; return rows; } };
  });
  await row(page, "first").getByRole("button").click();
  await search(page).fill("  FIRST CHAT  ");
  await expect(row(page, "first")).toBeVisible();
  await expect(row(page, "second")).toHaveCount(0);
  await expect(row(page, "first").getByRole("button").first()).toHaveAttribute("aria-expanded", "true");
  await search(page).fill("original ANSWER");
  await expect(row(page, "first")).toBeVisible();
  await search(page).fill("second question");
  await expect(row(page, "first")).toHaveCount(0);
  await expect(row(page, "second")).toBeVisible();
  await search(page).fill("my PROJECT");
  await expect(page.locator("article[data-chat-id]")).toHaveCount(2);
  await expect(page.locator("article[data-chat-id]").first()).toHaveAttribute("data-chat-id", "first");
  await expect(row(page, "first").getByRole("button")).toHaveAttribute("aria-expanded", "false");
  await search(page).fill("[");
  await expect(page.getByText("No matching chats", { exact: true })).toBeVisible();
  await search(page).fill("   ");
  await expect(page.locator("article[data-chat-id]")).toHaveCount(2);
  await search(page).fill("");
  await expect(row(page, "first")).toBeVisible();
  expect(await page.evaluate(() => (window as any).listCalls)).toBe(0);
});

test("search carries between Active and Trash, distinguishes empty views, and resets on reopening", async ({ page }) => {
  await page.evaluate(() => {
    const chats = JSON.parse(localStorage.getItem("PhenixCode.chats") || "[]");
    chats.push({ id: "deleted", project_id: "project", title: "First archived chat", updated_at: 100,
      deleted_at: 300, messages: [{ role: "user", content: "Archived question" }] });
    localStorage.setItem("PhenixCode.chats", JSON.stringify(chats));
  });
  await search(page).fill("first");
  await page.getByRole("button", { name: "Trash", exact: true }).click();
  await expect(search(page)).toHaveValue("first");
  await expect(row(page, "deleted")).toBeVisible();
  await search(page).fill("second");
  await expect(page.getByText("No matching chats", { exact: true })).toBeVisible();
  await page.getByRole("button", { name: "Active", exact: true }).click();
  await expect(row(page, "second")).toBeVisible();
  await expect(row(page, "first")).toHaveCount(0);
  await page.getByRole("dialog").getByRole("button", { name: "Close", exact: true }).click();
  await history(page).click();
  await expect(search(page)).toHaveValue("");
  await expect(page.locator("article[data-chat-id]")).toHaveCount(2);
  await page.evaluate(() => localStorage.setItem("PhenixCode.chats", "[]"));
  await page.getByRole("button", { name: "Trash", exact: true }).click();
  await expect(page.getByText("Trash is empty", { exact: true })).toBeVisible();
  await page.getByRole("button", { name: "Active", exact: true }).click();
  await expect(page.getByText("No chats yet", { exact: true })).toBeVisible();
});

test("filtered deletion retains the Undo placeholder through query changes and expiration", async ({ page }) => {
  await page.clock.install();
  await search(page).fill("first");
  await row(page, "first").getByRole("button").click();
  await row(page, "first").getByRole("button", { name: "Delete", exact: true }).click();
  await expect(row(page, "first")).toContainText("Chat moved to Trash");
  await search(page).fill("second");
  await expect(row(page, "first").getByRole("button", { name: "Undo", exact: true })).toBeVisible();
  await expect(row(page, "second")).toBeVisible();
  await row(page, "first").getByRole("button", { name: "Undo", exact: true }).click();
  await expect(row(page, "first")).toHaveCount(0);
  await search(page).fill("");
  await expect(page.locator("article[data-chat-id]").first()).toHaveAttribute("data-chat-id", "first");
  await row(page, "first").getByRole("button").click();
  await row(page, "first").getByRole("button", { name: "Delete", exact: true }).dblclick();
  await search(page).fill("second");
  await expect(row(page, "first")).toContainText("Chat moved to Trash");
  await page.clock.fastForward(5100);
  await expect(row(page, "first")).toHaveCount(0);
  await expect(row(page, "second")).toBeVisible();
  expect((await stored(page)).find((chat: any) => chat.id === "second").deleted_at).toBeUndefined();
});

for (const width of [320, 390]) {
  test(`history search and view controls fit a ${width}px screen`, async ({ page }) => {
    await page.setViewportSize({ width, height: 844 });
    await expect(search(page)).toBeVisible();
    const input = await search(page).boundingBox();
    const active = await page.getByRole("button", { name: "Active", exact: true }).boundingBox();
    const trash = await page.getByRole("button", { name: "Trash", exact: true }).boundingBox();
    for (const bounds of [input!, active!, trash!]) {
      expect(bounds.x).toBeGreaterThanOrEqual(0);
      expect(bounds.x + bounds.width).toBeLessThanOrEqual(width);
    }
    expect(input!.y).toBeGreaterThanOrEqual(Math.max(active!.y + active!.height, trash!.y + trash!.height));
    await search(page).fill("second");
    await expect(row(page, "second")).toBeVisible();
    await expect(row(page, "first")).toHaveCount(0);
  });
}

test("two physical clicks at Delete coordinates cannot delete or open the next chat", async ({ page }) => {
  await row(page, "first").getByRole("button").click();
  const firstBounds = await row(page, "first").boundingBox();
  const nextBounds = await row(page, "second").boundingBox();
  const button = await row(page, "first").getByRole("button", { name: "Delete", exact: true }).boundingBox();
  if (!button || !firstBounds || !nextBounds) throw new Error("Missing row geometry");
  // Include a completed render between clicks to reproduce the original failure.
  await page.mouse.click(button.x + button.width / 2, button.y + button.height / 2);
  await expect(row(page, "first")).toContainText("Chat moved to Trash");
  await page.mouse.click(button.x + button.width / 2, button.y + button.height / 2, { clickCount: 2 });
  const after = await row(page, "second").boundingBox();
  expect(after!.y).toBeCloseTo(nextBounds.y, 0);
  await expect(row(page, "second").getByRole("button")).toHaveAttribute("aria-expanded", "false");
  await expect(row(page, "first").getByRole("button", { name: "Delete", exact: true })).toHaveCount(0);
  const data = await stored(page);
  expect(data).toHaveLength(2);
  expect(data[0].deleted_at).toBeGreaterThan(0);
  expect(data[0].updated_at).toBe(200);
  expect(data[1].deleted_at).toBeUndefined();
  await expect(row(page, "first")).toHaveCount(0, { timeout: 7000 });
  await page.getByRole("button", { name: "Trash", exact: true }).click();
  await row(page, "first").getByRole("button").click();
  await expect(row(page, "first")).toContainText("Deleted");
  await expect(page.getByRole("button", { name: "Delete", exact: true })).toHaveCount(0);
  await row(page, "first").getByRole("button", { name: "Restore", exact: true }).click();
  await expect(row(page, "first")).toHaveCount(0);
  await page.getByRole("button", { name: "Active", exact: true }).click();
  await expect(page.locator("article[data-chat-id]").first()).toHaveAttribute("data-chat-id", "first");
  expect((await stored(page))[0]).toEqual({ ...fixtures[0], deleted_at: null });
});

test("Undo recovers content, retains ordering, and cancels placeholder expiration", async ({ page }) => {
  await page.clock.install();
  await row(page, "first").getByRole("button").click();
  await row(page, "first").getByRole("button", { name: "Delete", exact: true }).dblclick();
  await expect(row(page, "first")).toContainText("Chat moved to Trash");
  await page.getByRole("button", { name: "Trash", exact: true }).click();
  await expect(row(page, "first")).toBeVisible();
  await page.getByRole("button", { name: "Active", exact: true }).click();
  await row(page, "first").getByRole("button", { name: "Undo", exact: true }).click();
  await expect(row(page, "first")).toContainText("First chat");
  expect((await stored(page))[0]).toEqual({ ...fixtures[0], deleted_at: null });
  await page.clock.fastForward(6000);
  await expect(row(page, "first")).toBeVisible();
  await row(page, "first").getByRole("button").click();
  await row(page, "first").getByRole("button", { name: "Open", exact: true }).click();
  await expect(page.getByText("Original answer", { exact: true })).toBeVisible();
});

test("browser persistence supports new chats, updates, project filters, and rejects stale saves to Trash", async ({ page }) => {
  const result = await page.evaluate(async () => {
    const api = await import("/src/lib/utils.ts");
    const created = await api.saveChat({ project_id: "new-project", title: "Created", messages: [{ role: "user", content: "New question" }] });
    const id = created.id;
    await api.saveChat({ id, project_id: "new-project", title: "Updated", messages: [{ role: "user", content: "Changed question" }] });
    const loaded = await api.getChat(id);
    const active = await api.listChats("new-project");
    await api.deleteChat(id);
    const hidden = await api.getChat(id);
    const stale = await api.saveChat(loaded.chat);
    const trash = await api.listChats("new-project", true);
    await api.restoreChat(id);
    return { loaded, active, hidden, stale, trash, restored: await api.getChat(id) };
  });
  expect(result.loaded.chat.title).toBe("Updated");
  expect(result.active.chats).toHaveLength(1);
  expect(result.trash.chats).toHaveLength(1);
  expect(result.hidden.status).toBe("error");
  expect(result.stale.status).toBe("error");
  expect(result.restored.chat).toEqual(result.loaded.chat);
});

test("a slow failing bridge deletion is locked immediately and leaves both chats recoverable", async ({ page }) => {
  await row(page, "first").getByRole("button").click();
  await page.evaluate(() => {
    (window as any).deleteCalls = [];
    (window as any).cppApi = {
      deleteChat: async (id: string) => {
        (window as any).deleteCalls.push(id);
        await new Promise(resolve => setTimeout(resolve, 400));
        return { status: "error", message: "Test bridge failure" };
      },
    };
  });
  await row(page, "first").getByRole("button", { name: "Delete", exact: true }).dblclick();
  await expect(page.getByText("Test bridge failure", { exact: false })).toBeVisible();
  expect(await page.evaluate(() => (window as any).deleteCalls)).toEqual(["first"]);
  await expect(row(page, "first").getByRole("button", { name: "Delete", exact: true })).toBeEnabled();
  await expect(row(page, "second").getByRole("button")).toHaveAttribute("aria-expanded", "false");
  expect(await stored(page)).toEqual(fixtures);
});

test("previews normalize and cap each initial turn, preserve Unicode, and match in Active and Trash", async ({ page }) => {
  const cases = [
    { messages: [], expected: "" },
    { messages: [{ role: "system", content: "Instructions" }, { role: "assistant", content: "No question yet" }], expected: "" },
    { messages: [{ role: "user", content: "Question\r\nline\rnext\nlast" }], expected: "Question line next last" },
    { messages: [
      { role: "system", content: "Instructions" }, { role: "assistant", content: "Earlier response" },
      { role: "user", content: "Question\r\nline\rnext\nlast" }, { role: "system", content: "Context" },
      { role: "assistant", content: "Answer\r\nline\rnext\nlast" },
      { role: "user", content: "Later question" }, { role: "assistant", content: "Later answer" },
    ], expected: "Question line next last\nAnswer line next last" },
    { messages: [{ role: "user", content: "a".repeat(99) + "\r\nz" }, { role: "assistant", content: "b".repeat(101) }],
      expected: "a".repeat(99) + " z\n" + "b".repeat(101) },
    { messages: [{ role: "user", content: "😀".repeat(241) }, { role: "assistant", content: "😀".repeat(241) }],
      expected: "😀".repeat(100) + "...\n" + "😀".repeat(100) + "..." },
    ...[99, 100, 101, 102, 103, 104].map(length => ({
      messages: [{ role: "user", content: "a".repeat(length) }, { role: "assistant", content: "😀".repeat(length) }],
      expected: length <= 103 ? "a".repeat(length) + "\n" + "😀".repeat(length)
        : "a".repeat(100) + "...\n" + "😀".repeat(100) + "...",
    })),
    { messages: [{ role: "user", content: "a".repeat(100) + "\r\nxyz" }], expected: "a".repeat(100) + "..." },
    { messages: [{ role: "user", content: "a".repeat(100) + "\r\nxy" }], expected: "a".repeat(100) + " xy" },
    { messages: [{ role: "user", content: "Unanswered" }, { role: "user", content: "Later question" },
      { role: "assistant", content: "Later answer" }], expected: "Unanswered" },
    { messages: [{ role: "user", content: "" }, { role: "assistant", content: "Answer" }], expected: "\nAnswer" },
  ];
  const results = await page.evaluate(async cases => {
    const api = await import("/src/lib/utils.ts");
    const results = [];
    for (const { messages } of cases) {
      const { id } = await api.saveChat({ project_id: "previews", title: "Preview", messages });
      const active = await api.listChats("previews");
      const original = await api.getChat(id);
      await api.deleteChat(id);
      const trash = await api.listChats("previews", true);
      results.push({ active: active.chats.find(c => c.id === id).preview,
        trash: trash.chats.find(c => c.id === id).preview, messages: original.chat.messages });
    }
    return results;
  }, cases);
  expect(results).toEqual(cases.map(c => ({ active: c.expected, trash: c.expected, messages: c.messages })));
});
