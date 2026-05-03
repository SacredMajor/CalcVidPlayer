# CinemaPlus Multi-Video Binary Format (.cpv)

## Overview

A single `.cpv` file on the USB drive holds the global file header, an index
of video entries, and all video data concatenated back-to-back.  The
calculator reads the header/index once at startup, shows the menu, then seeks
directly to the chosen video's LBA offset using `msd_Read`.

All multi-byte integers are **little-endian** (matches the ez80 native byte
order so the C code can cast pointers directly without byte-swapping).

---

## File Layout

```
[ File Header    ]  32 bytes
[ Index Entry 0  ]  64 bytes  \
[ Index Entry 1  ]  64 bytes   >  num_videos entries
[ ...            ]            /
[ --- padding to 512-byte sector boundary --- ]
[ Video 0 data   ]  variable
[ Video 1 data   ]  variable
[ ...            ]
```

---

## File Header (32 bytes)

| Offset | Size | Type     | Field            | Description                                  |
|--------|------|----------|------------------|----------------------------------------------|
| 0x00   | 4    | char[4]  | magic            | `"CPVF"` (CinemaPlus Video File)             |
| 0x04   | 2    | uint16_t | version          | Format version, currently `0x0001`           |
| 0x06   | 1    | uint8_t  | num_videos       | Number of videos (1–255)                     |
| 0x07   | 1    | uint8_t  | flags            | Reserved, set to 0                           |
| 0x08   | 4    | uint32_t | index_offset     | Byte offset from file start to first index entry (always 32) |
| 0x0C   | 4    | uint32_t | data_lba_start   | LBA of first video data block on the drive   |
| 0x10   | 16   | uint8_t  | reserved         | Zero-filled                                  |

`data_lba_start` is the absolute LBA on the USB drive where video payload
data begins.  The encoder computes this by:

```
data_lba_start = partition_start_lba
               + ceil((32 + num_videos * 64) / 512)
```

---

## Index Entry (64 bytes each)

| Offset | Size | Type     | Field          | Description                                          |
|--------|------|----------|----------------|------------------------------------------------------|
| 0x00   | 32   | char[32] | title          | Null-terminated UTF-8 title string (truncated at 31) |
| 0x20   | 4    | uint32_t | lba_offset     | LBA offset from `data_lba_start` to this video's first frame block |
| 0x24   | 4    | uint32_t | num_frames     | Total number of frames                               |
| 0x28   | 2    | uint16_t | fps_numerator  | Frame rate numerator (e.g. 24)                       |
| 0x2A   | 2    | uint16_t | fps_denominator| Frame rate denominator (e.g. 1), giving fps = num/denom |
| 0x2C   | 2    | uint16_t | width          | Frame width in pixels (always 160 for FBin)          |
| 0x2E   | 2    | uint16_t | height         | Frame height in pixels (always 96 for FBin)          |
| 0x30   | 4    | uint32_t | lba_count      | Number of 512-byte LBA blocks this video occupies    |
| 0x34   | 12   | uint8_t  | reserved       | Zero-filled                                          |

---

## Frame Format (FBin, unchanged from Cinema)

Each frame is a flat binary blob immediately following the previous one,
sector-aligned (padded with 0x00 to the next 512-byte boundary):

```
[ Palette  ]  256 × 3 bytes = 768 bytes  (R, G, B per entry)
[ Pixels   ]  160 × 96 = 15 360 bytes    (one byte per pixel = palette index)
              Total raw = 16 128 bytes
              Padded to 512-byte boundary = 16 384 bytes (32 sectors)
```

Sector count per frame: `ceil(16128 / 512) = 32 sectors` (exactly, since
16384 / 512 = 32).

---

## Worked Example

Three videos at 24 fps, each 1200 frames:

```
File header:       32 bytes  (sector 0, bytes 0–31)
Index (3 × 64):   192 bytes  (bytes 32–223)
Padding:           288 bytes  (bytes 224–511)
= 1 sector (512 bytes) for header+index

Video 0: 1200 frames × 32 sectors/frame = 38 400 sectors
Video 1: same
Video 2: same

data_lba_start = partition_lba + 1
Video 0 lba_offset = 0
Video 1 lba_offset = 38 400
Video 2 lba_offset = 76 800
```

---

## File Naming

The file must be named `VIDEO.CPV` in the FAT32 root of the USB drive (8.3
filename, uppercase).  The player opens it by that name via `fatdrvce`.
