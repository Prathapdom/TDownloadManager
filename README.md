# TDM Download Manager

A fast native Windows download manager for regular downloads, **Tor** (`.onion` and routing any link through Tor) and **BitTorrent** (`.torrent` files and magnet links). It is plain C++17 and Win32 and builds to one portable `dist\TDM.exe` of about 1 MB, with no installer and no runtime dependencies.

## Features

**Regular downloads (HTTP and HTTPS)**
- Segmented downloads with up to 32 connections. Busy segments are split again as connections free up.
- Pause and resume, which survive restarts. Progress is kept in `<file>.tdm` until the download finishes.
- The server's file is checked with `If-Range` (ETag or Last-Modified). A changed file restarts cleanly instead of being corrupted.
- Retries with backoff.
- Handles redirects and `Content-Disposition` file names, and never overwrites an existing file (it adds ` (1)`, ` (2)` …).
- Optional checksum check after download: SHA-256, SHA-1, MD5 or SHA-512.
- Per-download options: user name and password, connection count, speed limit and network (Automatic, Tor or Direct).
- Queue control: at most *N* downloads at once, plus move to top, up, down or bottom.
- Global and per-download speed limits, which can be changed while downloading.
- Proxy support: SOCKS5 or HTTP CONNECT, with optional authentication.
- Categories (Video, Music, Documents, Compressed, Programs, Images, Torrents), with optional per-category sub-folders.

**Tor**
- `.onion` links go through Tor automatically. Any download can be sent through Tor, and so can all downloads ("Route everything through Tor").
- Uses a running Tor (`9050`) or Tor Browser (`9150`), or starts `tor.exe` itself (Settings › Tor).
- Host names are resolved by Tor, never locally.
- Each connection gets its own circuit (SOCKS isolation).
- If Tor is not reachable, the download fails with a clear message. It never falls back to a direct connection.

**BitTorrent**
- Adds `.torrent` files and magnet links. Magnet metadata is fetched from peers (`ut_metadata`).
- Peer discovery:
  - HTTP and UDP trackers;
  - the DHT;
  - peer exchange (PEX), received from peers;
  - `x.pe` direct peers in magnet links.
- Choose which files to download, before or during the download.
- Rarest-first piece selection with endgame mode.
- Seeding up to a share ratio, an upload limit and a piece re-check.
- Create torrents from a file or folder, with an option to start seeding at once.
- Optional: run trackers and peers through Tor. This uses TCP only, so the DHT is off.

**App**
- Light, dark and AMOLED-black themes, or follow Windows. Custom-drawn chrome; per-monitor DPI aware.
- Sidebar filters (status, torrents, Tor, categories), search, sortable and resizable columns.
- Details pane with three tabs:
  - General: a segment or piece map;
  - Files: per-file selection;
  - Peers.
- Offers to download links you copy to the clipboard.
- Drag and drop links and `.torrent` files.
- Single instance: links opened elsewhere go to the running window.
- Notification-area icon, completion notifications, and progress on the taskbar button. Windows does not sleep while downloads are active.
- Can register itself for `magnet:` links and `.torrent` files (More › "Open magnet links and .torrent files with TDM"). This writes to HKCU only.

Not included: browser extensions, FTP, video-site grabbing, UPnP port mapping, and BitTorrent v2-only torrents.

## Keyboard

| Key | Action |
| --- | --- |
| Ctrl+N | Add URL (pre-filled with links from the clipboard) |
| Ctrl+V | Download links from the clipboard |
| Ctrl+O | Add torrent file |
| Ctrl+T | Create torrent |
| Ctrl+F | Search |
| Ctrl+A | Select all |
| Ctrl+, | Settings |
| Ctrl+P / F2 | Properties of the selected download |
| Ctrl+Up / Ctrl+Down | Move in the queue |
| Space | Pause or resume the selection |
| Enter / double-click | Open the file (or its folder while downloading) |
| Del / Shift+Del | Remove / remove and delete files |
| Ctrl+Q | Exit (even with "close to tray") |

## Command line

```
TDM.exe [--silent] [--minimized] <url | magnet | file.torrent> ...
```

Links open the Add dialog pre-filled; `--silent` adds them straight to the queue. A running TDM receives the arguments.

## Settings

Everything is stored in text files, not the registry:

- `%APPDATA%\TDM\settings.ini`: options and window layout. The environment variable `TDM_SETTINGS` can point elsewhere, which allows portable or test setups.
- `downloads.ini` next to it: the download list.
- `torrents\` next to it: copies of `.torrent` metadata and resume data.

Proxy passwords, download passwords and cookies are encrypted for the current Windows user with DPAPI.

## Build

Requires TDM-GCC (MinGW-w64) 9.2 or newer.

```
powershell -ExecutionPolicy Bypass -File build.ps1             # icon, resources, exe, engine tests, UI tests
powershell -ExecutionPolicy Bypass -File build.ps1 -SkipTests  # exe only
```

or double-click `Build.cmd`. The tests use only local servers: an HTTP server with ranges, a SOCKS5 server standing in for Tor, and a second test process acting as a BitTorrent seeder. They need no internet connection.
