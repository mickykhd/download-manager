# Changelog

All notable changes to cdm are documented here. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

## [0.2.0] - 2026-10-10
### Added
- Named download queues with per-queue concurrency limits and time-window
  schedules; queue picker in the Add dialog; queue manager.
- Batch import: add many URLs at once, with `{start:end}` range expansion.
- Edit-download dialog for queued/paused jobs.
- Connection settings: proxy modes (direct/system/manual), per-host
  overrides (connections, speed, user agent), per-download headers,
  cookies, user agent and referer.
- Browser integration: local REST server (`POST /add`,
  `POST /start-headless-download`, `GET /queues`, `GET /ping`, API-key
  auth) plus Native Messaging host manifest + docs for extensions.
- Desktop citizenship on Windows: system tray icon, completion
  notifications, auto-start on boot, in-app update checker.
- Branding: app icon, Windows version metadata, LICENSE (MIT).

### Fixed
- GUI startup crash from uninitialized Nuklear backend state.
- Windows build: pthreads ported to Win32 abstraction, portable string
  helpers, CMake GUI/CURL handling, CPack NSIS escaping.
- Installer now creates Start Menu + Desktop shortcuts and offers PATH
  setup; packages ship required DLLs and are app-only (no curl dev files).

## [0.1.1] - 2026-10-10
### Added
- Windows installer (NSIS) and portable ZIP via CPack, published to
  GitHub Releases.
- Installer Start Menu + Desktop shortcuts and optional PATH setup.

## [0.1.0] - 2026-10-04
### Added
- Segmented multi-connection engine (HTTP Range, resume, retry/backoff,
  speed limiting, SHA-256 gate, adaptive concurrency).
- CLI (`cdm`) and Nuklear/GLFW GUI (`cdm-gui`) with categories, queue,
  scheduler, persisted options.
