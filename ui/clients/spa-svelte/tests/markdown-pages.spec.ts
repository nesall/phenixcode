import { expect, test } from "@playwright/test";
import { readdirSync } from "node:fs";

const filenames = readdirSync(new URL("./pages/", import.meta.url)).filter(name => name.endsWith(".md")).sort();

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
});

test("Markdown pages are an explicit development preview rendered through the chat renderer", async ({ page }) => {
  await page.goto("/");
  await expect(page.getByText("Connected.", { exact: true })).toBeVisible();
  await expect(page.locator('[data-role="assistant"]')).toHaveCount(0);

  await page.goto("/?testMessages");
  await expect(page.locator('[data-role="assistant"]')).toHaveCount(filenames.length);
  for (const filename of filenames) {
    const words = filename.replace(/\.md$/i, "").replace(/[-_]+/g, " ");
    const title = words.charAt(0).toUpperCase() + words.slice(1);
    await expect(page.getByText(`Preview: ${title}`, { exact: true })).toBeVisible();
  }
  await expect(page.locator('[data-role="assistant"] pre code').first()).toContainText("supports_tools?");
  await expect(page.locator('[data-role="assistant"] table').first()).toContainText("looksCodeLike");
});
