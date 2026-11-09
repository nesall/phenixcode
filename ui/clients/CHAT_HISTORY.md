# Chat history and Trash

The client in this checkout is `spa-svelte`; its native host is `webview`.
`Toolbar.svelte` retains chat loading and editor reset behavior, while
`ChatHistory.svelte` owns the existing history dialog's Active/Trash lists,
single expanded row, and temporary Undo placeholders. Project names come from
the current instance list, with the stored project ID as a fallback.

The existing `window.cppApi` bridge now supports:

- `listChats(projectId = "", deleted = false)` — up to 500 summaries, including
  a preview of the initial user message and its assistant response, when present.
  Each snippet replaces CRLF, CR, and LF with spaces. Snippets up to 103 Unicode
  code points remain complete; longer snippets show the first 100 followed by
  `...`. The 100-character limit is a named constant in each implementation.
  One LF (`\n`) separates the snippets; a missing response adds no
  separator. Active sorts by modification time; Trash sorts
  by deletion time.
- `getChat(id)` and `saveChat(chat)` — active chats only.
- `deleteChat(id)` — mark an active chat deleted.
- `restoreChat(id)` — restore a deleted chat, also used by Undo.

`ChatDb` adds nullable `deleted_at` and `free_chat` (a boolean stored as an
integer with a default of false) to older SQLite databases on initialization.
Deletion and restoration only change `deleted_at`. IDs, project, title, creation
and modification timestamps, and messages survive recovery. Stale saves cannot
overwrite a chat while it is in Trash. Browser mode applies the same behavior
to the existing localStorage store. There is no permanent deletion or retention
policy.

Deleted rows retain their measured height and keyed identity for five seconds.
During that interval they have no expandable content or delete action. Mutation
handlers lock before awaiting the bridge; row expansion ignores repeated clicks
and briefly ignores clicks after deletion or placeholder expiry. No list or row
transition moves another action under the pointer.

## Validation

From `spa-svelte`:

```sh
npm install
npm run check
npm run build
npx playwright install chromium
npm test
```

Alternatively set `PLAYWRIGHT_CHANNEL=msedge` to use installed Microsoft Edge.
The tests use isolated browser storage and mock the application service; they
cover Open/Close, exclusive expansion, physical Delete click coordinates,
placeholder expiry, Undo, Trash/Restore, slow deletion failure, and persistence.

The standalone C++ persistence tests require SQLite development libraries and
the project's existing utils-log headers. From the repository root:

```sh
cmake -S ui/clients/webview/tests -B build_dbg/chat-history-tests -DLOGGER_INCLUDE_DIR=/path/to/utils-log
cmake --build build_dbg/chat-history-tests
ctest --test-dir build_dbg/chat-history-tests --output-on-failure
```

Logger headers are also detected in the root build directories' dependency
caches. C++ tests use a uniquely named temporary database and `:memory:`; they
never open or reset the user's chat database. Only the unrelated random-ID
generator is substituted in the test executable.
