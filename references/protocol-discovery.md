# Protocol Discovery — Methodology & Case Study

This document is the heart of the skill. Off-brand mice ship no protocol spec; you recover it
empirically by capturing HID traffic while the factory software drives the device. The GM3323D
(GeVeda / DELL rebadge, VID 0x1D57 PID 0xFA60) is used as a fully-worked example.

## 0. What you are really looking for

Every configurable mouse exposes its state in **Feature reports** (host → device → host echo,
used to read/write config) and/or **Input reports** (device → host, used for broadcast state like
battery). Your job: find which report ID, on which HID collection, carries each setting, and the
byte offset + encoding inside that report.

A typical 132-byte lighting config report looks like:
```
05 0d 01 01 04 08 00 00 ff 06 01 00 00 00 00 06 ... 8 × (R G B) ...
│  │  │  │  │  │  │        │
│  │  │  │  │  │  └─ RGB (R=00 G=00 B=ff)
│  │  │  │  │  └─ brightness (1–8)
│  │  │  │  └─ mode (0x00 off … 0x08 marquee)
│  │  │  └─ (mode duplicated region / profile stage)
│  │  └─ length = 0x0d (13?) or similar framing
└─ signature 0x05 0x0d
```

The trailing 8 × 3 bytes are the per-profile / per-DPI color slots.

## 1. The three insights that save you days

### 1a. Config state is TRANSIENT — capture *during* the change
The coherent config block does NOT sit in the device waiting to be read. It surfaces **only at
the instant the factory app writes a change**, then disappears back into flash noise. Evidence
from the GM3323D:
- A passive `config_watch` probe polling the known `05 0d` / report-ID `0x04` at ~10 ms for 23
  seconds caught **0** coherent config states.
- A *broad* probe scanning report IDs 1..63 at high rate, run **while the user dragged the DPI
  slider**, caught coherent states immediately (including the `04 32 01 08` block whose byte [10]
  tracked the change).

**Rule:** never try to "read the current config" by polling an idle device. Instead, run the
probe and have the user *perform the action* in the same window.

### 1b. Most Feature reads are flash noise
The device returns garbage high-entropy memory on the majority of reads. Only a few "personalities"
are coherent. Filter:
- Fixed signature prefixes (`05 0d`, `04 32 01 08`, `23 43 1e de` …).
- Low-entropy / structured blocks (repeating RGB triples, profile tables).
Discard everything else; it is not config.

### 1c. Battery is an Input report, not a Feature
On the GM3323D the battery arrives as a 5-byte **Input** report on the `UsagePage 0x000A`
collection:
```
03 00 40 01 07     ← 21 identical samples at 70%
```
Mapping: `byte[4]` = level 0..10; percent = `byte[4] * 10` (0x07 → 70%, confirmed against the
OEM UI). `byte[2]=0x40` ≈ voltage/ADC, `byte[3]=0x01` ≈ charging flag. Read it with a blocking
`ReadFile` thread (reports arrive ~every 2.9 s), **not** `GetFeature`.

## 2. Topology first — or you will hang

Enumerate collections and read `HIDP_CAPS`. Typical GM3323D layout:

| # | UsagePage | Feature | Input | Role |
|---|-----------|---------|-------|------|
| 0 | 0x000A | — | 5 B | battery / monitor |
| 1 | 0x000B | 132 B | — | lighting / config |
| 2 | 0x0001 | — | 2 B | standard mouse |
| 3 | 0x000C | — | 3 B | consumer/media |

**Only collection 1 accepts `HidD_GetFeature`.** Calling `GetFeature` on col 0/2/3 (which have
`FeatureReportByteLength == 0`) **blocks forever** — the driver waits for a feature that will
never come. Always:
- Gate `GetFeature` on `caps.FeatureReportByteLength > 0`.
- Run each `GetFeature` in a **200 ms timeout-bounded worker thread**; on timeout mark that
  report-ID as "skip" so one bad rid can never stall the whole capture.

## 3. Capture procedure (the run protocol)

1. User quits any prior instance of your app (it holds the device handle exclusively; two
   handles competing cause silent failures or hangs).
2. User opens the **original OEM software**, parks on the page for the setting under study
   (lighting / DPI / polling-rate).
3. User launches `hid_probe.exe`. It prints the topology, calibrates live report IDs + buffer
   sizes, then prints `NOW change <setting>` and starts the timer.
4. **During the window** the user performs the change:
   - Lighting: cycle through colors/effects.
   - DPI/polling: see §5 (discrete values, not continuous drag).
5. Probe writes `%TEMP%\hid_probe_<timestamp>.txt` (ESC to stop early).

In the log you locate the report whose **distinct-state count is low** and whose bytes **move only
when the setting moves**. That is the setting's home.

## 4. Worked example — GM3323D lighting

Captured coherent `05 0d` block (132 B), decoded:
- `[0]=0x05 [1]=0x0d` signature
- `[3]` = mode (0x00 off … 0x08 marquee; 0x09/0x0a exist in firmware but have no OEM name →
  omit from UI rather than invent labels)
- `[5]` = brightness 1–8
- `[6,7,8]` = RGB
- `[25..48]` = 8 × (R G B) profile color slots

Write path: `ReadFeature(0x04)` → patch `[3][5][6,7,8]` → `SetFeature`. Because the device echoes
data with first byte `0x05` (not the report-id `0x04`), the app keeps a **"write format" toggle**:
either send the buffer as-is (first byte = data) or shift everything +1 with byte[0]=report-id.
Ship both; flip at runtime if a control is silent.

## 5. Worked example — DPI is a SLIDER, crack it with a controlled experiment

The DPI control on the GM3323D is a **200–10000 continuous slider, step 100** — NOT 6 presets.
Two mistakes were made and corrected:
- **Mistake 1 — assumed fixed presets at offsets [16..21].** A capture taken after the user set
  slots `200/800/1600/2400/3200/5100` showed `[16..21]` **unchanged** (`04 12 24 37 49 39`). The
  hypothesis was false; DPI lives elsewhere.
- **Mistake 2 — continuous drag yields too few samples.** A 30 s drag produced only 4 random
  samples (`04 / 22 / 61 / 73` in byte [10]) — not enough to deduce the encoding (04≈400 but
  22/61/73 → 3400/9700/11500, not a clean ÷100).

**Correct method — controlled values with pauses:** have the user set *exact* discrete DPI values
(e.g. 400, 800, 1600, 3200, 5100), pausing ~3 s after each so the probe latches one coherent
state per value. Then fit: if `[10] ∝ DPI/100` (linear), wire a 200–10000 step-100 slider; if
non-linear, build a lookup table. The probe prints `[10]` live during the run so you get immediate
feedback. **Polling-rate** (return-rate) is a separate byte (`[24]` on GM3323D — observed 03→02
when the user changed return rate); confirm its Hz encoding with two exports at different rates.

## 6. From capture to code

For each discovered setting, record:
- Collection index + `UsagePage`
- Report ID
- Report length
- Byte offset(s) and encoding (linear? table? bitfield?)
- Whether it is a Feature (read/write) or Input (read-only broadcast)
- Known signature prefix

Drop these into `assets/main.cpp`'s `kVID/kPID`, `kReportID`, `kFeatLen`, the `OFF_*` enum,
`kDefaultTemplate[]`, and the `ApplyConfig()` write path. Keep the `ReadFeature → patch →
SetFeature` discipline so unknown bytes are always preserved.
