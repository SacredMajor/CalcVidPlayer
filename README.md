# CalcVidPlayer

A multi-title USB video player for the **TI-84 Plus CE**, combining
[Cinema](https://github.com/wwierzbowski/cinema)'s rock-solid rendering pipeline
with a multi-title menu, SRT caption support, chapter/start-frame support,
and per-title resume — all over raw USB with no filesystem.

---

## Features

- **Multi-title menu** — navigate with UP/DOWN, select with ENTER
- **SRT captions** — synced to video, rendered at the bottom of the screen
- **Multiple caption tracks** — pick language before playback (e.g. `en_us`, `fr_fr`)
- **Chapter / start-frame support** — encode one long file, define multiple titles at different offsets
- **Resume** — saves your position per-title in AppVar `CPVRESME`; prompts Resume / Restart on re-entry
- **Double-buffered async USB reads** — Cinema's exact pipeline; glitch-free playback
- **Frame-accurate timer** — hardware timer at 32 768 Hz, not the 1-second RTC

---

## Requirements

### Calculator
- TI-84 Plus CE
- [Cesium](https://github.com/mateoconlechuga/cesium) or any shell that runs C programs
- USB drive (written raw with `dd` — **no filesystem**)

### Development machine (Windows + WSL Ubuntu)
- [CEdev](https://github.com/CE-Programming/toolchain) installed, in PATH (`cedev-config` works)
- [FBin](https://github.com/will-dabeast09/fbin) at `~/fbin/linux/bin/fbin`
- Python 3.10+ (for `encode_cpv.py`)
- Pillow (`pip install pillow`) — only needed if you regenerate `icon.png`

---

## Project structure

```
CalcVidPlayer/
├── Makefile            ← CE Toolchain build file
├── icon.png            ← 16×16 RGB icon shown in Cesium
├── encode_cpv.py       ← Python encoder: MP4 → CPVF binary for USB drive
├── README.md           ← this file
└── src/
    └── main.c          ← full C player source
```

After `make`:

```
CalcVidPlayer/
└── bin/
    └── CALCVID.8xp     ← send this to the calculator
```

---

## Build

```bash
cd CalcVidPlayer
make
```

Flash `bin/CALCVID.8xp` to your calculator via **TI Connect CE**.

---

## Encoding videos for the USB drive

```bash
python3 encode_cpv.py -o output.bin \
    --video ~/fbin/linux/jjk.mp4    "JJK Ep 42" 10 \
        --srt en_us captions_en.srt \
        --srt fr_fr captions_fr.srt \
    --video ~/fbin/linux/avatar.mp4 "Avatar"    10 \
        --start 00:05:00 \
        --srt en_us avatar_en.srt
```

| Argument | Meaning |
|---|---|
| `--video <file> "<title>" <fps>` | Add a title. FPS is typically 10–15 for the CE. |
| `--start HH:MM:SS` | Start playback from this time offset (chapter support). |
| `--srt <lang> <file.srt>` | Attach a caption track. `lang` is any 7-char tag, e.g. `en_us`. |

The encoder calls FBin internally — you don't need to run FBin yourself.

### Write to USB drive

```bash
sudo dd if=output.bin of=/dev/sdX bs=4M status=progress
```

Replace `/dev/sdX` with your actual drive (check with `lsblk`). **This wipes the drive.**

---

## Controls during playback

| Key | Action |
|---|---|
| `ENTER` | Toggle pause |
| `0` | Restart from the title's start frame |
| `2nd` or `MODE` | Save position and return to menu |
| `CLEAR` | Save position and quit to OS |
| Any other key | Resume if paused |

---

## CPVF binary format (v1)

The USB drive is written raw. Layout (little-endian, 512-byte sectors):

| Region | Size |
|---|---|
| Global header | 32 bytes (sector 0) |
| Title index entries | 68 bytes × num_titles |
| Caption track descriptors | 16 bytes × total tracks across all titles |
| Caption data | 88 bytes × total cues, padded to sector |
| **Video frame data** | 31 sectors × num_frames per title, concatenated |

Each video frame = 1 sector palette (256 × RGB1555) + 30 sectors pixels (160×96 indexed).

---

## Captions (.srt)

Standard SRT format. HTML tags (`<i>`, `<b>`) are stripped automatically.
Text longer than 79 characters is truncated. Timestamps are converted to
frame numbers based on each title's FPS at encode time.

---

## How it works (rendering pipeline)

Exactly Cinema's double-buffer pattern:

1. Bootstrap: load frame N synchronously from USB
2. Queue frame N+1 asynchronously (`msd_ReadAsync`)
3. Display frame N (`gfx_ScaledSprite_NoClip` 2× scaled, centred)
4. Overlay caption bar if a cue is active for this frame
5. `gfx_SwapDraw()` + `gfx_Wait()`
6. Wait for hardware timer tick (32 768 Hz ÷ FPS)
7. Wait for async read to finish
8. Swap cur/nxt, advance frame counter, go to 2

The `pal_cb` and `pix_cb` callbacks each increment `xfer->lba` by 31 so
the next queued read automatically targets the next frame.
