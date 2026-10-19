# Not Stremio

**A fork of [Sp9nky's unofficial Stremio PS5 port](https://github.com/Sp9nky/unofficial-stremio-ps5-port), focused on Thai subtitle support, playback performance, and a glass interface.**

> [!IMPORTANT]
> **Credit where it's due.** The original port — the app itself, its design, its torrent engine, its UI — was built by **Sp9nky**. Everything here stands on that work. If you like this app, support the upstream author:
>
> [![Support Sp9nky on Ko-fi](https://img.shields.io/badge/Ko--fi-Support_the_original_author-FF5E5B?style=for-the-badge&logo=ko-fi&logoColor=white)](https://ko-fi.com/sp9nky)
>
> Bug fixes from this fork are contributed back upstream where appropriate (e.g. the Thai subtitle fix, [upstream PR #10](https://github.com/Sp9nky/unofficial-stremio-ps5-port/pull/10)).
>
> The glass visual language is inspired by [Fxy6969's "liquid-glass" Stremio theme](https://github.com/REVENGE977/stremio-enhanced) (via Stremio Enhanced) — re-implemented natively for the PS5 UI toolkit, not copied.

A Stremio client for jailbroken PlayStation 5 consoles, running as a native app on the home screen: browse your catalogs and library, pick a stream, and watch it, all with the DualSense.

> **Unofficial.** This project is not affiliated with, endorsed by or supported by Stremio, or by Sony Interactive Entertainment. It has no connection with either. "Stremio" and its logo are trademarks of their owner; "PlayStation", "PS5" and "DualSense" are trademarks of Sony Interactive Entertainment Inc. No Sony code, SDK, keys or firmware files are included.

## What this fork focuses on

- **Thai language support.** TIS-620 subtitle decoding, Noto Sans Thai bundled into the font chain, and combining-mark shaping via the font's GPOS anchors.
- **Playback performance.** Parallel chunk downloading, progressive streaming (playback starts on the first bytes), pre-warmed stream URLs and TLS connections, sub-0.5s seeks.
- **Continuity.** Seamless in-player episode transitions, next-episode pre-resolve with a stream matcher, one-press Continue Watching resume, Up-Next countdown.
- **Reliability.** In-RAM stream ring buffer, non-blocking player teardown, fixes for buffer and seek freezes.
- **Picture.** Upscaler options (Cinema HQ bicubic + CAS, FSR1 + sharpening, Anime4K shaders), SDR tone-mapping polish, HDR10 diagnostic probe.
- **Interface.** Glass visual language (frosted surfaces, rim light, cinematic hero), top pill navigation, instant search, live setting descriptions.

## Differences from upstream

| Area | Upstream | Not Stremio |
| --- | --- | --- |
| Thai subtitles | mojibake / missing glyphs | TIS-620 decoding, Noto Sans Thai, GPOS mark shaping |
| Startup | loading logo | snapshot boot of last session, no startup chime |
| Navigation | — | top horizontal pill navigation; L1/R1 switch pages; L2/R2 jump settings sections |
| Search | debounced input | pre-built index; Triangle opens search from anywhere; IME race handling |
| Stream start | buffers before playback | progressive — plays on first bytes; 6 parallel chunk downloads |
| Stream buffering | on-disk cache files | 128 MB in-RAM ring buffer |
| Connection reuse | new handshake per stream | pre-warmed URLs and TLS connections (20-min cache, re-warmed on Up Next) |
| Seeking | — | 0.25s start gate; 15s keyframe snap |
| Episode transitions | loading screen between episodes | seamless in-player transitions; next episode pre-resolved at 80% |
| Continue Watching | — | one-press resume; 4-tier stream matcher for autoplay; Up-Next countdown |
| Upscaling | — | Cinema HQ (bicubic + CAS), FSR1 + sharpening, Anime4K shaders |
| Tone mapping | — | SDR tone-mapping polish; HDR10 diagnostic probe |
| Player options | — | playback speed, skip-intro (TheIntroDB), watched-sync |
| Settings | static labels | live per-choice descriptions; artwork cache capped at 512 MB |
| Player exit | — | reaper-thread teardown, returns instantly |
| Accounts | single account | multiple account profiles |
| Design language | upstream default | glass theme, inspired by Fxy6969's liquid-glass (re-implemented natively) |

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
- **Fxy6969** — author of the "liquid-glass" Stremio theme (via [REVENGE977/stremio-enhanced](https://github.com/REVENGE977/stremio-enhanced)) that inspired this fork's glass visual language. Re-implemented natively; no theme code was copied.
- This fork's changes (Thai subtitle support, performance and reliability work) by ChillQuant, contributed back upstream where suitable.
- Licensed under **GNU GPL v3** (see [LICENSE](LICENSE)), same as upstream. Forking and modifying is exactly what the license is for.
