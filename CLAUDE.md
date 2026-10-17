# TDM — handoff notes for Claude

TDM is a download manager: **Windows only**, native **C++17 + Win32**, no frameworks. It handles HTTP(S), Tor (SOCKS5, `.onion`) and BitTorrent (`.torrent` and magnet links). It builds to a single static, portable `dist\TDM.exe` of about 1 MB. User-facing docs are in `README.md`; this file covers what you need to change the code safely.

The user's brief:
- fast and responsive;
- a native Windows app;
- all the usual download-manager features, for both regular and Tor downloads ("Tor" meaning **both** the Tor network and BitTorrent);
- the same style as the sibling projects BareLog (`F:\Prathap\Others\BareLogClone-source-and-app`), BareGrep and NoteUltra: custom-painted chrome, INI settings rather than the registry, and `build.ps1` with tests.

## Build & test

```
powershell -ExecutionPolicy Bypass -File build.ps1            # icon, resources, exe, engine tests, UI tests
powershell -ExecutionPolicy Bypass -File build.ps1 -SkipTests # exe only
```

- **Compiler:** TDM-GCC 9.2 at `F:\Software\TDM-GCC-64\bin\g++.exe`, with windres from the same folder.
- **Close running copies first.** Stop any running `TDM.exe` before rebuilding, or the link step fails.
- **Always finish with a full `build.ps1`.** It ends with `All N checks passed`, then `UI integration: N checks passed`, then `Built:`.
- **Engine tests:** `tests.cpp` → `dist\engine-tests.exe`.
  - Everything runs against local servers in the test process: an HTTP server with ranges, and a SOCKS5 server that stands in for Tor.
  - The BitTorrent test re-launches the exe as `--seed <torrent> <dir> <port>`, because `BtServices` is a per-process singleton. The main process then leeches over a magnet link with `x.pe`.
  - The corporate network blocks BitTorrent and Tor, so never write tests that need the internet.
- **UI tests:** `ui-tests.cpp` → `dist\ui-tests.exe <TDM.exe>`.
  - Launches the app hidden, with `TDM_SETTINGS` pointing at a temp INI file.
  - Forwards links through a second instance, drives the app with `WM_COMMAND`, and reads state back through `WM_TESTQUERY`. The query cases are in `MainProc`.
  - The clipboard checks skip themselves when the session has no clipboard, which is the case in the agent shell.
- **Visual check:** `tools\Capture-Window.ps1 -Out x.png [-Send cmdId | -Post cmdId] [-Class TDM.Dialog]`.
  - Set `$env:TDM_SETTINGS` to a scratch path first, so the user's real `%APPDATA%\TDM` is never touched.
  - Theme commands are 3100–3103. Add URL is 100 (use `-Post`), and Settings is 109.

## Files

| File | Role |
| --- | --- |
| `common.hpp` | String, path and file helpers (namespace `tdm`). |
| `net.hpp` | URL parsing, TCP, SOCKS5 and HTTP CONNECT proxies, TLS via SChannel. |
| `http.hpp` | HTTP/1.1 client: redirects, ranges, chunked bodies, `Content-Disposition`. |
| `engine.hpp` | Download engine (namespace `engine`): items, queue, segmented HTTP workers, rate limiters, Tor service, checksums, `View` snapshots for the UI. |
| `torrent.hpp` | Bencode, metainfo, magnets, `createTorrent` (namespace `bt`). |
| `torrent_job.hpp` | BitTorrent: peer wire, `ut_metadata` and PEX, trackers, DHT, piece picker, storage, seeding. `BtServices` is a singleton that owns the listen socket and the DHT. |
| `settings.hpp` | `settings.ini` and `downloads.ini`, plus DPAPI for secrets (namespace `settings`). |
| `main.cpp` | The whole UI in one translation unit, organised by `// ----` banners: palette and darkmode, chrome, list, details, dialogs, Settings and Create torrent, actions and menus, window procedures, `wWinMain`. |

## Things that bite

- **Threading.** Engine threads never touch windows. `E.notify` posts `WM_ENGINE`, coalesced by the `enginePosted` flag, and the UI then pulls `E.snapshot()`. Keep it that way.
- **Item fields.** These are guarded by `Item::m`. The segment list changes under `claim()` while workers write. `flush()` clamps to the segment's current end because a split can land inside an in-flight write. A negative length there once caused `ERROR_INVALID_USER_BUFFER`.
- **The queue.** `Engine::add` does not force-start; it respects `maxActive`. `resume()` forces by default, because a user clicking Resume means "now".
- **`View::tor`** is the per-item setting (0 auto, 1 Tor, 2 direct), not whether Tor was used. `.onion` items have `tor == 0`. The UI's `viaTor()` derives the effective route.
- **Toolchain quirks.**
  - `far` and `near` are macros in the Windows headers, so don't use them as names.
  - Call `SetProcessDpiAwarenessContext` and `GetDpiForWindow` through `GetProcAddress`.
  - Use `std::max<LONG>` when mixing `LONG` and `int`.
- **Icon glyphs.** Write them as `\uE…` escapes, never as raw private-use characters. The face is Segoe Fluent Icons, with Segoe MDL2 Assets as the fallback.
- **Shell editing.** Python on this machine is Python 2. Heredocs in the agent shell mangle escapes, so edit C++ with the Edit tool rather than sed or python heredocs where `\n` is involved.
