# CinemaPlus

A multi-video USB player for the **TI-84 Plus CE** that combines Cinema's
glitch-free FBin rendering pipeline with a DVD-style title selection menu.

---

## Features

| Feature | Source |
|---|---|
| Glitch-free FBin rendering (256-colour palette per frame, 160×96) | Cinema |
| Per-frame palette quantisation (no global palette compression) | Cinema |
| Press **0** to restart; any key to resume after pause | Cinema |
| Multi-title menu – scroll and pick any video | TICEVid (redesigned) |
| Single `.cpv` container file holds all videos | New |
| Direct LBA seeks – no QOI, no block-index bugs | New |

---

## Repository Structure

```
CinemaPlus/
├── FORMAT.md          Binary format specification
├── Makefile           CEdev build file
├── encode_cpv.py      PC-side encoder (Python 3)
└── src/
    └── main.c         Calculator player (C, CEdev)
```

---

## Quick Start

### 1 – Build the calculator program

```bash
# Install CEdev, then:
make
# Produces bin/CINPLUS.8xp  →  send to calculator via TI Connect CE
```

### 2 – Encode your videos

```bash
pip install ffmpeg-python Pillow numpy
# FFmpeg must also be installed and on your PATH

python encode_cpv.py \
    --video "bad_apple.mp4"   "Bad Apple"     24 \
    --video "rick_roll.mp4"   "Never Gonna"   24 \
    --video "short_clip.mp4"  "Short Clip"    15 \
    -o VIDEO.CPV
```

Each `--video` argument takes:  `<input file>  <menu title>  <fps>`

- Title is truncated to 31 characters.
- FPS can be a decimal (e.g. `23.976`).
- Any format FFmpeg understands is accepted (mp4, mkv, avi, gif, …).

### 3 – Copy to USB drive

Format a USB drive as **FAT32**.  Copy `VIDEO.CPV` to the **root directory**.

```
USB:\
└── VIDEO.CPV
```

### 4 – Play

1. Connect the USB drive to the calculator via a mini-USB OTG adapter.
2. Run **CINPLUS** from the calculator's program list.
3. Navigate with **↑ / ↓**, press **[ENTER]** to play.

---

## Controls

| Key | Action |
|---|---|
| **↑ / ↓** | Navigate menu |
| **[ENTER]** | Play selected video / toggle pause |
| **0** | Restart current video from beginning |
| **[2nd]** or **[MODE]** | Return to menu during playback |
| **[CLEAR]** | Quit (menu or during playback) |

---

## Binary Format (.cpv) Summary

See `FORMAT.md` for the full specification.

```
[ File Header   ]   32 bytes      magic "CPVF", version, num_videos, data_lba_start
[ Index Entry 0 ]   64 bytes  \
[ Index Entry 1 ]   64 bytes   >  one entry per video: title, lba_offset,
[ ...           ]   64 bytes  /   num_frames, fps, dimensions
[ padding       ]   to 512-byte sector boundary
[ Video 0 data  ]   frames back-to-back, each 16 384 bytes (32 sectors)
[ Video 1 data  ]
[ ...           ]
```

Each frame:
```
[ Palette ]   768 bytes   (256 × R,G,B, 8-bit each)
[ Pixels  ]   15 360 bytes (160×96, 1 byte = palette index)
[ Padding ]   256 bytes    (zero-fill to 16 384 / 32-sector boundary)
```

---

## Memory Budget (ez80)

| Buffer | Size |
|---|---|
| `g_frame_buf` (frame read buffer) | 16 384 B |
| `g_meta_buf`  (header/index sector) | 512 B |
| `pal16`       (stack, converted palette) | 512 B |
| Index array   (32 entries × 64 B) | 2 048 B |
| **Total** | **≈ 19.5 KB** |

The ez80 has 154 KB user-accessible RAM so this is comfortable.

---

## Video Size Estimates

| Duration | FPS | Frames | Sectors | Size |
|---|---|---|---|---|
| 1 min | 24 | 1 440 | 46 080 | 22.5 MB |
| 5 min | 24 | 7 200 | 230 400 | 112 MB |
| 10 min | 15 | 9 000 | 288 000 | 140 MB |

A 64 GB FAT32 USB drive can hold roughly **570 minutes** at 24 fps.

---

## Troubleshooting

**"VIDEO.CPV not found or invalid"**
- Make sure the file is in the root of a FAT32 drive.
- Re-run the encoder; if it errored mid-way the file may be truncated.

**"USB init failed"**
- Try a different drive (SanDisk or Kingston recommended by CEdev).
- Make sure you're using a proper OTG adapter.

**Playback is slower than expected FPS**
- The ez80 at 48 MHz can sustain ~24 fps on a fast USB drive.  Use a
  Class 10 or UHS-I drive for best results.
- Lower the FPS argument in the encoder (e.g. 15 fps).

**Build fails**
- Make sure `CEDEV` is set: `export CEDEV=/path/to/CEdev`
- Ensure `msddrvce`, `fatdrvce`, `keypadc`, `graphx` libraries are
  present in your CEdev installation (available since toolchain v11.0).

---

## Licence

MIT.  Builds on concepts from Cinema (wwierzbowski) and TICEVid
(the-pink-hacker), both MIT-licenced.
