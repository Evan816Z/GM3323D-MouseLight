---
name: offbrand-mouse-driver
description: |-
  This skill should be used when a user wants to build, from scratch, a lightweight user-mode
  Windows driver/control tool for an off-brand or OEM-rebadged mouse (or similar USB HID
  gaming peripheral) whose original software is lost, bloatware, or has no public protocol
  docs. It covers the full methodology: enumerating the device's HID collections, empirically
  discovering the lighting / battery / DPI / polling-rate protocols via capture probes, and
  producing a single-file C++17 Win32 tray application (no .NET, no Qt, no firmware flashing)
  that replaces the OEM driver. Trigger phrases include "杂牌鼠标驱动", "鼠标驱动程序",
  "mouse lighting control", "reverse engineer mouse protocol", "DIY mouse driver",
  "lost mouse software", or any request to control RGB / battery / DPI on an unknown-brand mouse.
---

# Off-Brand Mouse Driver Builder

Build a small (≈300 KB, static, zero-dependency) Windows tray tool that controls an
off-brand / OEM-rebadged USB mouse entirely through the **native HID API** — no firmware
upload, no drivers.sys, no reverse-engineered binaries. The only thing that varies per mouse
is a handful of protocol bytes; this skill provides a reusable capture-probe and a known-good
application skeleton so the work is "find the bytes" not "write a HID stack".

## Scope & Non-Goals

- **In scope:** RGB lighting, brightness, lighting effects, on-screen battery %, polling-rate
  (return-rate) toggle, and DPI control — whatever the target mouse exposes and the user wants.
- **Out of scope / forbidden:** any firmware flashing, descriptor patching, kernel drivers, or
  impersonating the manufacturer's signed update channel. Only legal, reversible, user-space
  HID Feature/Input report traffic is used.

## Architecture Decisions (always apply)

- **C++17 + Win32 + native HID** (`setupapi.h`, `hidsdi.h`, `hidpi.h`). No .NET / MFC / Qt /
  Electron. `-mwindows` (no console), `-static`, `-s -ffunction-sections -fdata-sections
  -Wl,--gc-sections` for a ~300 KB binary.
- **Toolchain:** MinGW-w64 (WinLibs UCRT `g++.exe`). Build scripts MUST stay **ASCII** — a
  `.bat` with Chinese text breaks under the GBK console and the commands silently fail.
- **Single instance** via a named mutex; **single hidden top-level window** (`WS_EX_TOOLWINDOW`)
  to receive `WM_DEVICECHANGE` and cross-instance wake messages.
- **Config persistence:** `HKCU\Software\<AppName>` registry (mode/bright/color/autostart/on).
- **Autostart:** `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`.
- **Write strategy:** `ReadFeature → modify only known fields → SetFeature`, never overwrite
  unknown bytes. This survives firmware quirks and avoids bricking settings.
- **Idle = 0% CPU:** battery read on a blocking `ReadFile` thread; plug/unplug via
  `WM_DEVICECHANGE`; no timers, no polling loops in the steady state.

## Workflow

### Phase 0 — Identify the device
- Get the **VID/PID** from Device Manager (or `HidD_GetAttributes`). Tell the user to read it
  if you can't enumerate. Note the device name.
- Confirm it is a **HID** device (not a custom USB class). Everything below assumes HID.

### Phase 1 — Discover HID topology (do this first; it prevents hangs)
Enumerate **all** device interfaces and, for each, pull `HIDP_CAPS` (`UsagePage`,
`FeatureReportByteLength`, `InputReportByteLength`). Build a map like:

| Collection | UsagePage | Feature | Input | Role |
|-----------|-----------|---------|-------|------|
| col#0 | 0x000A | — | 5 B | battery / monitor |
| col#1 | 0x000B | 132 B | — | lighting / config |
| col#2 | 0x0001 | — | 2 B | standard mouse |
| col#3 | 0x000C | — | 3 B | consumer/media |

**Critical:** only the collection with `FeatureReportByteLength > 0` accepts `HidD_GetFeature`.
Calling `GetFeature` on a collection that has **no** feature reports **blocks forever**. Always
time-bound the call (see `references/hid-api-cookbook.md`).

### Phase 2 — Capture & reverse the protocol (the real work)
Use `scripts/hid_probe.cpp` (compile with the same toolchain). It:
1. Enumerates all collections of the target VID/PID,
2. For each feature-bearing collection, **scans report IDs 1..63** with a **200 ms
   timeout-bounded** `GetFeature` (a hung rid is skipped, never fatal),
3. Runs a blocking `ReadFile` thread per Input collection,
4. During a capture window, records **all distinct coherent states** per report ID with
   timestamps, and flags which **byte offsets vary** when you drive the setting.

**Run protocol (must involve the user + factory software):**
1. User exits any prior instance of your app (it holds the device handle exclusively).
2. User opens the **original OEM software** and parks on the relevant page (e.g. lighting, DPI).
3. User runs `hid_probe.exe`; the probe prints `NOW change <setting>` and starts a window.
4. **While the window is open**, the user performs the change — e.g. drags DPI min→max→min 2–3
   times, or cycles lighting colors. ESC stops early; output goes to `%TEMP%\hid_probe_*.txt`.
5. You read the log: locate the report (and bytes) that change **only** with the setting.

Three hard-won insights — do not rediscover them the hard way:
- **Config state is transient.** The coherent config block (e.g. `05 0d …` for lighting)
  surfaces **only at the instant the factory app writes a change**. A passive poll loop from an
  independent handle catches ~0 hits even at 10 ms. You MUST capture *during* the change; broad
  + fast scanning across many report IDs during the action is what catches it.
- **Most feature reads are "flash noise"** (high-entropy random memory). Filter for fixed
  signatures (`05 0d`, `04 32 01 08`, …) and structured low-entropy blocks; ignore everything
  else.
- **Battery is almost always an Input report** (`UsagePage 0x000A`), not a feature. Read it with
  a blocking `ReadFile` thread, not `GetFeature`.

See `references/protocol-discovery.md` for the worked GM3323D case study (lighting,
battery, and the DPI investigation) and how to turn a capture into byte offsets + encodings.

### Phase 3 — Build the application
Copy `assets/main.cpp` as the starting point — it is a **complete, compiling** tray tool for the
GM3323D that already implements: tray icon with a **live drawn battery %**, lighting on/off,
RGB color picker, 8 effect modes, 8 brightness steps, settings window, registry save/restore,
autostart toggle, plug/unplug auto-reconnect, single-instance, and a harmless **"write format"
debug toggle** for when the device is ambiguous about whether the first byte is report-ID or
data. Generalize it:
- Change `kVID/kPID`, `kReportID`, `kFeatLen`, signatures, and `kModes` to the new device.
- Replace the `kDefaultTemplate[132]` with a real capture from the new device.
- Map the discovered offsets into the `OFF_*` enum and the `ApplyConfig()` write path.
- For DPI / polling-rate / multi-profile features, add the UI + `SetFeature` plumbing using the
  offsets you found in Phase 2.

`assets/build.bat` auto-detects MinGW / clang / MSVC and embeds `app.rc` (icon is optional; the
tray icon is drawn at runtime). `assets/clean.bat` removes build output.

### Phase 4 — Verify & ship
- Run the built `.exe`; confirm the mouse reacts to each control and the tray battery updates.
- If a control is silent, flip the **write-format** toggle (some firmware wants the report-id
  byte, others the raw data byte as the first byte).
- Recommend a `.gitignore` that excludes `dist/`, `build/`, and `*.exe`; ship source + docs to
  GitHub; attach the `.exe` as a Release artifact.

## Resources

- `scripts/hid_probe.cpp` — reusable capture/scan probe (compile-ready; pass VID/PID on the
  command line). The single most valuable artifact for any new mouse.
- `references/protocol-discovery.md` — deep-dive methodology + the GM3323D case study (lighting
  offsets, battery single-point calibration, and the controlled-experiment technique that
  cracked DPI).
- `references/hid-api-cookbook.md` — copy-paste Win32 HID snippets: SetupAPI enum with
  timeout-bounded `GetFeature`, blocking `ReadFile` battery thread, dynamic `BITMAPV5HEADER`
  battery tray icon, single-instance mutex, registry config, `WM_DEVICECHANGE`.
- `assets/main.cpp` — complete lighting+battery tray app skeleton (the GM3323D reference build).
- `assets/app.rc`, `assets/build.bat`, `assets/clean.bat` — build scaffolding.

## When NOT to use this skill
- If the user just wants to *remap buttons / macros*, that is usually HID consumer/buttons usage
  and a different (simpler) capture target — still applicable but lighter.
- If the device is not HID (custom USB vendor class), this skill's HID API approach won't bind;
  flag that early.
