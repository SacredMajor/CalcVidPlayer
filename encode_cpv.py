#!/usr/bin/env python3
"""
encode_cpv.py – CalcVidPlayer multi-video encoder
===================================================
Uses FBin (the same tool Cinema uses) to encode each video, then
concatenates them with a multi-video header/index so the calculator
knows where each one starts.

Requirements:
    - FBin binary at ~/fbin/linux/bin/fbin  (already set up)
    - ffmpeg  (already installed)

Usage:
    python3 encode_cpv.py -o output.bin \
        video1.mp4 "Title One" 10 \
        video2.mp4 "Title Two" 10

Each video takes three positional values: <file> <title> <fps>
"""

import math
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

# ── Format constants (must match main.c) ──────────────────────────────────────

MAGIC            = b"CPVF"
FORMAT_VERSION   = 1
SECTOR_SIZE      = 512
FRAME_WIDTH      = 160
FRAME_HEIGHT     = 96
# FBin writes: 1 sector palette (512 bytes, RGB1555 × 256) + 30 sectors pixels
# Total = 31 sectors per frame — matches Cinema exactly
PAL_SECTORS      = 1
PIX_SECTORS      = 30
FRAME_SECTORS    = PAL_SECTORS + PIX_SECTORS   # 31

HEADER_SIZE      = 32
INDEX_ENTRY_SIZE = 64
TITLE_MAX        = 31   # +1 null = 32 bytes on disk


# ── Find FBin ─────────────────────────────────────────────────────────────────

def find_fbin():
    candidates = [
        Path.home() / "fbin/linux/bin/fbin",
        Path("/usr/local/bin/fbin"),
        Path("/usr/bin/fbin"),
    ]
    for p in candidates:
        if p.exists():
            return str(p)
    sys.exit("ERROR: fbin not found. Expected at ~/fbin/linux/bin/fbin")


# ── Run FBin on one video ─────────────────────────────────────────────────────

def encode_one(fbin_path, video_path, fps, tmp_out):
    """
    Run FBin to encode a single video into tmp_out.
    FBin writes Cinema's native format: 1-sector RGB1555 palette + 30-sector pixels.
    """
    cmd = [
        fbin_path,
        "-i", str(video_path),
        "-o", str(tmp_out),
        "-s", f"{FRAME_WIDTH}:{FRAME_HEIGHT}",
        "-r", str(fps),
        "-p", "256",
    ]
    print(f"  Running FBin: {' '.join(cmd)}")
    result = subprocess.run(cmd, capture_output=False)
    if result.returncode != 0:
        sys.exit(f"ERROR: FBin failed with code {result.returncode}")


# ── Count frames in a FBin output file ───────────────────────────────────────

def count_frames(fbin_file):
    """Each frame is exactly FRAME_SECTORS * SECTOR_SIZE bytes."""
    size = os.path.getsize(fbin_file)
    frame_bytes = FRAME_SECTORS * SECTOR_SIZE
    if size % frame_bytes != 0:
        print(f"  WARNING: file size {size} not a multiple of {frame_bytes} — may be truncated")
    return size // frame_bytes


# ── Header / index packing ────────────────────────────────────────────────────

def pack_header(num_videos, data_lba_start):
    hdr = struct.pack(
        "<4sHBBII16s",
        MAGIC,
        FORMAT_VERSION,
        num_videos,
        0,               # flags
        HEADER_SIZE,     # index_offset (byte offset of first index entry)
        data_lba_start,  # absolute sector where video data begins
        b"\x00" * 16,    # reserved
    )
    assert len(hdr) == HEADER_SIZE
    return hdr


def pack_index_entry(title, lba_offset, num_frames, fps, lba_count):
    title_bytes = title.encode("utf-8")[:TITLE_MAX]
    title_field = title_bytes + b"\x00" * (32 - len(title_bytes))
    fps_num = int(fps * 1000)
    fps_den = 1000
    body = struct.pack(
        "<IIHHHI",
        lba_offset,   # 4  offset from data_lba_start to this video
        num_frames,   # 4
        fps_num,      # 2
        fps_den,      # 2
        FRAME_WIDTH,  # 2
        FRAME_HEIGHT, # 2
        lba_count,    # 4  total sectors for this video
    )
    # title(32) + body(20) = 52, pad to 64
    entry = title_field + body + b"\x00" * 12
    assert len(entry) == INDEX_ENTRY_SIZE
    return entry


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    args = sys.argv[1:]

    # Parse: -o output.bin  video1.mp4 "Title" fps  video2.mp4 "Title" fps ...
    if len(args) < 2 or args[0] != "-o":
        print(__doc__)
        sys.exit("Usage: python3 encode_cpv.py -o output.bin file1.mp4 Title1 fps1 ...")

    output_path = args[1]
    rest = args[2:]

    if len(rest) % 3 != 0 or len(rest) == 0:
        sys.exit("Each video needs exactly 3 args: file title fps")

    videos = []
    for i in range(0, len(rest), 3):
        path  = rest[i]
        title = rest[i+1]
        fps   = float(rest[i+2])
        if not Path(path).exists():
            sys.exit(f"File not found: {path}")
        videos.append({"path": path, "title": title, "fps": fps})

    fbin_path = find_fbin()
    num_videos = len(videos)

    print(f"CalcVidPlayer Encoder")
    print(f"Output: {output_path}")
    print(f"Videos: {num_videos}")
    print(f"FBin:   {fbin_path}")
    print()

    # How many sectors does the header+index occupy?
    meta_bytes   = HEADER_SIZE + num_videos * INDEX_ENTRY_SIZE
    meta_sectors = math.ceil(meta_bytes / SECTOR_SIZE)
    # data_lba_start is where video data begins (in sectors from file start)
    data_lba_start = meta_sectors

    # Encode each video with FBin into a temp file
    tmp_files = []
    frame_counts = []
    for i, vid in enumerate(videos):
        print(f"[{i+1}/{num_videos}] Encoding '{vid['title']}' ...")
        tmp = tempfile.NamedTemporaryFile(delete=False, suffix=".fbin")
        tmp.close()
        # FBin needs to write to a folder called 'frames' in its working dir
        # Run it from the fbin directory so it can find/create that folder
        fbin_dir = str(Path(fbin_path).parent.parent)
        encode_one(fbin_path, Path(vid["path"]).resolve(), vid["fps"], tmp.name)
        n = count_frames(tmp.name)
        print(f"  {n} frames ({n * FRAME_SECTORS} sectors)")
        tmp_files.append(tmp.name)
        frame_counts.append(n)

    # Write the final output file
    print(f"\nAssembling {output_path} ...")
    with open(output_path, "wb") as fout:
        # Write placeholder header (will patch data_lba_start)
        fout.write(pack_header(num_videos, data_lba_start))

        # Write index entries
        lba_offset = 0
        for i, vid in enumerate(videos):
            n   = frame_counts[i]
            lba = n * FRAME_SECTORS
            entry = pack_index_entry(
                vid["title"], lba_offset, n, vid["fps"], lba
            )
            fout.write(entry)
            lba_offset += lba

        # Pad meta region to sector boundary
        written = HEADER_SIZE + num_videos * INDEX_ENTRY_SIZE
        fout.write(b"\x00" * (meta_sectors * SECTOR_SIZE - written))

        # Append each encoded video
        for i, tmp_path in enumerate(tmp_files):
            print(f"  Appending video {i+1}/{num_videos} ...")
            with open(tmp_path, "rb") as fin:
                while True:
                    chunk = fin.read(1024 * 1024)
                    if not chunk:
                        break
                    fout.write(chunk)
            os.unlink(tmp_path)

    size_mb = os.path.getsize(output_path) / (1024 * 1024)
    print(f"\nDone! {output_path} ({size_mb:.1f} MB)")
    print(f"Write to USB with:")
    print(f"  sudo dd if={output_path} of=/dev/sdX bs=4M status=progress")

if __name__ == "__main__":
    main()
