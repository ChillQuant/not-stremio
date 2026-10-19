# Not Stremio

**A tuned fork of [Sp9nky's unofficial Stremio PS5 port](https://github.com/Sp9nky/unofficial-stremio-ps5-port) — Thai subtitles fixed, playback hardened, and everything sped up.**

> [!IMPORTANT]
> **Credit where it's due.** This project is a fork. The original port — the app itself, its design, its torrent engine, its UI — was built by **Sp9nky**. Everything here stands on that work. If you like this app, support the upstream author:
>
> [![Support Sp9nky on Ko-fi](https://img.shields.io/badge/Ko--fi-Support_the_original_author-FF5E5B?style=for-the-badge&logo=ko-fi&logoColor=white)](https://ko-fi.com/sp9nky)
>
> Bug fixes from this fork are contributed back upstream where appropriate (e.g. the Thai subtitle fix, [upstream PR #10](https://github.com/Sp9nky/unofficial-stremio-ps5-port/pull/10)).

A Stremio client for jailbroken PlayStation 5 consoles, running as a native app on the home screen: browse your catalogs and library, pick a stream, and watch it, all with the DualSense.

> **Unofficial.** This project is not affiliated with, endorsed by or supported by Stremio, or by Sony Interactive Entertainment. It has no connection with either. "Stremio" and its logo are trademarks of their owner; "PlayStation", "PS5" and "DualSense" are trademarks of Sony Interactive Entertainment Inc. No Sony code, SDK, keys or firmware files are included.

## What this fork changes

Upstream is the app. This fork is a flaw-fix and performance program on top of it:

| Area | What changed |
| --- | --- |
| **Thai subtitles** | Fixed mojibake (TIS-620 decoding), bundled Noto Sans Thai into the font chain, and shaped combining marks with the font's GPOS anchors. Thai subs now render correctly. |
| **Startup** | Sub-second cold start; the Board boots from a snapshot of your last session instead of a loading logo. Silent startup (no chime). |
| **Stream start** | Parallel chunk downloading with progressive streaming — playback begins on the first bytes instead of waiting for full buffers. Pre-resolved stream URLs and warm TLS connections make repeat opens near-instant. |
| **Play Next** | Seamless in-player episode transitions: no black flash, no loading modal, next episode pre-resolved and pre-warmed while you watch. |
| **Reliability** | Removed the multi-GB on-disk stream cache (source of a segfault on large files) in favor of a pure in-RAM ring buffer; fixed player-exit deadlocks, seek freezes, and buffer races. |
| **Search** | Instant search: pre-built index, no debounce lag, IME race handling. |
| **Polish** | Autoplay/Up-Next flow, Continue Watching one-press resume, per-setting live descriptions, and a sweep of small UI fixes. |

## Screenshots

See upstream's [screenshots](https://github.com/Sp9nky/unofficial-stremio-ps5-port#screenshots) — the app looks the same; this fork changes how it behaves.

## Requirements

A jailbroken PS5 (see upstream [requirements](https://github.com/Sp9nky/unofficial-stremio-ps5-port#requirements)).

## Install

1. Download `PPSA77711.ffpfsc` from the [Releases](../../releases) page.
2. Copy it to `/data/homebrew/` on the console (FTP, PS5 Upload, ...).
3. ShadowMountPlus registers it: start **Not Stremio** from the **Games** section of the home screen.
4. In **Settings**, sign in to your Stremio account.

To update, close the app first, delete the old `PPSA77711.ffpfsc`, wait a few seconds, then copy the new one. (If the image is replaced while the app is running, the console keeps the old icon and backgrounds.)

The app keeps its settings, artwork cache, read-ahead caches and log in its own storage (`/download0/stremio`, up to 16 GB), the same place as before, so your sign-in should carry over; if it doesn't, sign in again.

> [!NOTE]
> This fork keeps the title ID `PPSA77711`, so it installs into the same slot as upstream 1.0 (game-type title, **Games** section). Your settings and login carry over.

## Controls

| Where | Button | Action |
| --- | --- | --- |
| Everywhere | ✕ / ○ | select / back (○ from any page goes to the Board) |
| | D-pad, left stick | move |
| | △ or Options | search |
| Board | □ | See all (not on Continue Watching) |
| | ✕ on a Continue Watching card | play now / choose another episode / choose another stream |
| | Options, on a Continue Watching card | remove it (asks first) |
| Addons | □ | reload the addons |
| | ✕ on an addon you added by URL | remove it |
| Title page, episodes | ✕ | choose the episode |
| | L1 / R1 | previous / next season |
| | △ | mark the episode watched / not watched |
| Title page, streams | ✕ | play |
| | □ | play with the streaming server |
| Player | ✕ or touchpad | pause / play |
| | D-pad left / right | seek 10 s |
| | L1 / R1 | seek 1 min |
| | L3 | back to the start |
| | □ / △ | next subtitle / next audio track |
| | L2 / R2 | subtitle delay -/+ 0.25 s |
| | D-pad up or Options | track menu |
| | ○ | stop |

## Building

Same toolchain as upstream (PS5 SDK + clang-18, see upstream [Building](https://github.com/Sp9nky/unofficial-stremio-ps5-port#building)). From the repo root:

```bash
bash build.sh
```

## Status and known limitations

Same as upstream, plus: this fork is tested on-device by its maintainer, but every release is a work in progress.

## Credits and licenses

- **Sp9nky** — author of the [unofficial Stremio PS5 port](https://github.com/Sp9nky/unofficial-stremio-ps5-port) this fork is built on. All core app credit goes upstream.
- This fork's changes (Thai subtitle support, performance and reliability work) by ChillQuant, contributed back upstream where suitable.
- Licensed under **GNU GPL v3** (see [LICENSE](LICENSE)), same as upstream. Forking and modifying is exactly what the license is for.
