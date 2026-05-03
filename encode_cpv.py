#!/usr/bin/env python3
"""
encode_cpv.py – CalcVidPlayer multi-video encoder
===============================================
Converts one or more MP4/MKV files into a single binary file that you
dd directly onto your USB drive (same as Cinema).

Requirements:
    pip install Pillow numpy

FFmpeg must be installed and on your PATH.

Usage:
    # Single video (just like Cinema, but with a menu):
    python encode_cpv.py -o output.bin video.mp4 "My Video" 10

    # Multiple videos:
    python encode_cpv.py -o output.bin \\
        jjk.mp4 "JJK Ep 42" 10 \\
        avatar.mp4 "Avatar" 10

Each video takes 3 arguments: <file> <title> <fps>
Title is shown in the on-calculator menu (max 31 chars).
fps of 10 is recommended (same as Cinema).

Then write to USB:
    sudo dd if=output.bin of=/dev/sdX bs=4M status=progress
"""

import math
import os
import struct
import subprocess
import sys
from pathlib import Path

# ── Constants (must match main.c) ──────────────────────────────────────────

FRAME_WIDTH    = 160
FRAME_HEIGHT   = 96
PALETTE_BYTES  = 256 * 3           # 768  bytes
PIXEL_BYTES    = FRAME_WIDTH * FRAME_HEIGHT  # 15360 bytes
FRAME_RAW      = PALETTE_BYTES + PIXEL_BYTES # 16128 bytes
SECTOR         = 512
FRAME_SECTORS  = 32                # ceil(16128/512) = 32
FRAME_PADDED   = FRAME_SECTORS * SECTOR      # 16384 bytes

HEADER_SECTORS = 8                 # sectors 0-7 reserved for header+index
HEADER_SIZE    = SECTOR            # header fits in 1 sector (512 bytes)
ENTRY_SIZE     = 64                # bytes per index entry
MAX_VIDEOS     = 16
TITLE_MAX      = 31


# ── Palette quantisation (same quality as FBin) ────────────────────────────

def quantise_frame(rgb_array):
    """
    rgb_array: H×W×3 numpy uint8 array
    Returns (palette_bytes [768], index_bytes [H*W])
    """
    from PIL import Image
    img = Image.fromarray(rgb_array, "RGB")
    img_q = img.quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=0)
    pal = bytes(img_q.getpalette()[:768])
    idx = bytes(img_q.tobytes())
    return pal, idx


# ── FFmpeg frame iterator ──────────────────────────────────────────────────

def iter_frames(path, fps):
    """Yields raw (H,W,3) numpy uint8 arrays via ffmpeg pipe."""
    import numpy as np
    cmd = [
        "ffmpeg", "-i", path,
        "-vf", f"fps={fps},scale={FRAME_WIDTH}:{FRAME_HEIGHT}:flags=lanczos",
        "-f", "rawvideo", "-pix_fmt", "rgb24", "-",
    ]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    nbytes = FRAME_WIDTH * FRAME_HEIGHT * 3
    try:
        while True:
            raw = proc.stdout.read(nbytes)
            if len(raw) < nbytes:
                break
            yield np.frombuffer(raw, dtype=np.uint8).reshape((FRAME_HEIGHT, FRAME_WIDTH, 3))
    finally:
        proc.stdout.close()
        proc.wait()


def count_frames(path, fps):
    """Estimate frame count via ffprobe (used for progress display only)."""
    try:
        out = subprocess.check_output([
            "ffprobe", "-v", "error", "-count_frames",
            "-select_streams", "v:0",
            "-show_entries", "stream=nb_read_frames",
            "-of", "default=nokey=1:noprint_wrappers=1",
            "-vf", f"fps={fps},scale={FRAME_WIDTH}:{FRAME_HEIGHT}",
            path,
        ], stderr=subprocess.DEVNULL).decode().strip()
        return int(out)
    except Exception:
        return 0


# ── Frame writer ───────────────────────────────────────────────────────────

def write_frame(fout, pal, idx):
    """Write one 16384-byte padded frame."""
    raw = pal + idx
    fout.write(raw)
    fout.write(b"\x00" * (FRAME_PADDED - len(raw)))


# ── Pack header (512 bytes = 1 sector) ────────────────────────────────────

def pack_header(num_videos):
    # magic(4) version(1) num_videos(1) reserved0(2) data_lba_start(4) reserved(500)
    hdr = struct.pack("<4sBBHI500s",
        b"CPVF",
        1,              # version
        num_videos,
        0,              # reserved0
        HEADER_SECTORS, # data_lba_start
        b"\x00" * 500,
    )
    assert len(hdr) == SECTOR
    return hdr


# ── Pack index entry (64 bytes) ────────────────────────────────────────────

def pack_entry(title, lba_offset, num_frames, fps_num, fps_den):
    t = title.encode()[:TITLE_MAX] + b"\x00"
    t = t.ljust(TITLE_MAX + 1, b"\x00")
    # title(32) + lba_offset(4) + num_frames(4) + fps_num(2) + fps_den(2) + reserved(20)
    entry = struct.pack("<32sIIHH20s",
        t,
        lba_offset,
        num_frames,
        fps_num,
        fps_den,
        b"\x00" * 20,
    )
    assert len(entry) == ENTRY_SIZE, f"entry is {len(entry)} bytes"
    return entry


# ── Main encode ────────────────────────────────────────────────────────────

def encode(output_path, videos):
    import tempfile

    n = len(videos)
    print(f"CalcVidPlayer Encoder")
    print(f"Output : {output_path}")
    print(f"Videos : {n}")
    print()

    temp_files = []
    entries = []
    lba_cursor = 0  # lba_offset from data_lba_start

    for i, (path, title, fps) in enumerate(videos):
        fps_num = int(fps * 1000)
        fps_den = 1000
        total = count_frames(path, fps)
        print(f"[{i+1}/{n}] '{title}'  ({path})  @ {fps} fps")
        if total:
            print(f"  ~{total} frames")

        tmp = tempfile.NamedTemporaryFile(delete=False, suffix=".bin")
        temp_files.append(tmp.name)
        frame_count = 0

        for rgb in iter_frames(path, fps):
            pal, idx = quantise_frame(rgb)
            write_frame(tmp, pal, idx)
            frame_count += 1
            if frame_count % 100 == 0:
                if total:
                    print(f"  {frame_count}/{total} ({frame_count*100//total}%)", end="\r")
                else:
                    print(f"  {frame_count} frames", end="\r")

        tmp.flush()
        tmp.close()

        lba_count = frame_count * FRAME_SECTORS
        print(f"  {frame_count} frames done ({lba_count} sectors)")

        entries.append(pack_entry(title, lba_cursor, frame_count, fps_num, fps_den))
        lba_cursor += lba_count

    # Write output file
    print(f"\nWriting {output_path} ...")
    with open(output_path, "wb") as f:
        # Sector 0: header
        f.write(pack_header(n))

        # Sectors 1-7: index entries + padding
        idx_data = b"".join(entries)
        idx_padded = idx_data + b"\x00" * ((HEADER_SECTORS - 1) * SECTOR - len(idx_data))
        f.write(idx_padded)

        # Video data
        for i, tmp_path in enumerate(temp_files):
            print(f"  Appending video {i+1}/{n} ...")
            with open(tmp_path, "rb") as fin:
                while True:
                    chunk = fin.read(1 << 16)
                    if not chunk:
                        break
                    f.write(chunk)
            os.unlink(tmp_path)

    mb = os.path.getsize(output_path) / (1024 * 1024)
    print(f"\nDone!  {output_path}  ({mb:.1f} MB)")
    print()
    print("Write to USB:")
    print(f"  sudo dd if={output_path} of=/dev/sdX bs=4M status=progress")


# ── CLI ────────────────────────────────────────────────────────────────────

def main():
    args = sys.argv[1:]

    if "-h" in args or "--help" in args or not args:
        print(__doc__)
        sys.exit(0)

    output = "output.bin"
    if args[0] in ("-o", "--output"):
        output = args[1]
        args = args[2:]

    if len(args) % 3 != 0:
        sys.exit("Error: provide videos as triples: <file> <title> <fps>")

    videos = []
    for i in range(0, len(args), 3):
        path  = args[i]
        title = args[i+1]
        try:
            fps = float(args[i+2])
        except ValueError:
            sys.exit(f"FPS must be a number, got: {args[i+2]}")

        if not Path(path).exists():
            sys.exit(f"File not found: {path}")
        if len(videos) >= MAX_VIDEOS:
            sys.exit(f"Maximum {MAX_VIDEOS} videos supported")

        if len(title) > TITLE_MAX:
            print(f"Warning: title truncated to {TITLE_MAX} chars")
            title = title[:TITLE_MAX]

        videos.append((path, title, fps))

    if not videos:
        sys.exit("No videos specified.")

    # Check dependencies
    try:
        import numpy
        from PIL import Image
    except ImportError:
        sys.exit("Missing dependencies. Run: pip install Pillow numpy")

    encode(output, videos)


if __name__ == "__main__":
    main()
