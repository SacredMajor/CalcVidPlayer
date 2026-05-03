#!/usr/bin/env python3
"""
encode_cpv.py – CinemaPlus multi-video encoder
================================================
Encodes one or more video files into a single VIDEO.CPV file for use with the
CinemaPlus TI-84 Plus CE player.

Requirements:
    pip install ffmpeg-python Pillow numpy

Usage:
    python encode_cpv.py -o VIDEO.CPV \
        --video "Bad Apple.mp4" "Bad Apple" 24 \
        --video "Rick Roll.mp4" "Never Gonna" 24

Each --video argument takes three values: <file> <title> <fps>.
The title is truncated to 31 characters.

The output file can be placed on any FAT32 USB drive.  No special partition
layout is required; the encoder writes a self-contained file and the player
reads it via fatdrvce's file API.
"""

import argparse
import math
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

# ── Constants ─────────────────────────────────────────────────────────────────

MAGIC            = b"CPVF"
FORMAT_VERSION   = 1
FRAME_WIDTH      = 160
FRAME_HEIGHT     = 96
PALETTE_BYTES    = 256 * 3          # 768
PIXEL_BYTES      = FRAME_WIDTH * FRAME_HEIGHT  # 15 360
FRAME_RAW_BYTES  = PALETTE_BYTES + PIXEL_BYTES  # 16 128
SECTOR_SIZE      = 512
FRAME_SECTORS    = math.ceil(FRAME_RAW_BYTES / SECTOR_SIZE)  # 32
FRAME_PADDED     = FRAME_SECTORS * SECTOR_SIZE                # 16 384

HEADER_SIZE      = 32
INDEX_ENTRY_SIZE = 64
TITLE_MAX        = 31   # +1 for null terminator = 32 bytes


# ── Palette quantisation ───────────────────────────────────────────────────────

def quantise_frame(rgb_array):
    """
    Given a (H, W, 3) uint8 numpy array, return:
        palette  – bytes of length 768  (256 × R,G,B)
        indices  – bytes of length H*W  (one byte per pixel)

    Uses PIL's median-cut quantiser which produces a 256-colour palette.
    """
    from PIL import Image
    img = Image.fromarray(rgb_array, "RGB")
    img_q = img.quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=0)
    palette_raw = img_q.getpalette()           # list of 768 ints (R,G,B × 256)
    palette_bytes = bytes(palette_raw[:768])
    index_bytes   = bytes(img_q.tobytes())     # one byte per pixel, row-major
    return palette_bytes, index_bytes


# ── FFmpeg frame extraction ────────────────────────────────────────────────────

def iter_frames_ffmpeg(input_path: str, target_fps: float, width=160, height=96):
    """
    Yields raw RGB numpy arrays (H, W, 3) for each output frame.
    Uses ffmpeg via subprocess to avoid loading the whole video into RAM.
    """
    import numpy as np

    cmd = [
        "ffmpeg",
        "-i", input_path,
        "-vf", f"fps={target_fps},scale={width}:{height}:flags=lanczos",
        "-f", "rawvideo",
        "-pix_fmt", "rgb24",
        "-",
    ]
    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    frame_bytes = width * height * 3
    try:
        while True:
            raw = proc.stdout.read(frame_bytes)
            if len(raw) < frame_bytes:
                break
            yield np.frombuffer(raw, dtype=np.uint8).reshape((height, width, 3))
    finally:
        proc.stdout.close()
        proc.wait()


def count_frames_ffmpeg(input_path: str, target_fps: float, width=160, height=96) -> int:
    """Quick pass to count total output frames (used for progress display)."""
    cmd = [
        "ffprobe",
        "-v", "error",
        "-count_frames",
        "-select_streams", "v:0",
        "-show_entries", "stream=nb_read_frames",
        "-of", "default=nokey=1:noprint_wrappers=1",
        "-vf", f"fps={target_fps},scale={width}:{height}",
        input_path,
    ]
    try:
        out = subprocess.check_output(cmd, stderr=subprocess.DEVNULL).decode().strip()
        return int(out)
    except Exception:
        return 0  # unknown – progress bar will just not show totals


# ── Frame writing ──────────────────────────────────────────────────────────────

def write_frame(fout, palette_bytes: bytes, index_bytes: bytes):
    """Write one padded frame (16 384 bytes) to the output file."""
    raw = palette_bytes + index_bytes          # 16 128 bytes
    padding = FRAME_PADDED - len(raw)          # 256 bytes of zero padding
    fout.write(raw)
    fout.write(b"\x00" * padding)


# ── Header / index packing ─────────────────────────────────────────────────────

def pack_header(num_videos: int) -> bytes:
    """Pack the 32-byte file header."""
    # data begins after header + index entries, rounded up to sector boundary
    header_and_index = HEADER_SIZE + num_videos * INDEX_ENTRY_SIZE
    sectors_for_meta = math.ceil(header_and_index / SECTOR_SIZE)
    data_lba_start   = sectors_for_meta   # relative to file start (sector 0 = byte 0)

    hdr = struct.pack(
        "<4sHBBII16s",
        MAGIC,
        FORMAT_VERSION,
        num_videos,
        0,                  # flags
        HEADER_SIZE,        # index_offset (always 32)
        data_lba_start,
        b"\x00" * 16,       # reserved
    )
    assert len(hdr) == HEADER_SIZE
    return hdr


def pack_index_entry(title: str, lba_offset: int, num_frames: int,
                     fps_num: int, fps_den: int, lba_count: int) -> bytes:
    title_bytes = title.encode("utf-8")[:TITLE_MAX] + b"\x00"
    title_bytes = title_bytes.ljust(32, b"\x00")

    entry = struct.pack(
        "<32sIIHHHH12s",
        title_bytes,
        lba_offset,
        num_frames,
        fps_num,
        fps_den,
        FRAME_WIDTH,
        FRAME_HEIGHT,
        lba_count,
        b"\x00" * 12,
    )
    # struct gives 32+4+4+2+2+2+2+4+12 = 64 bytes – but the pack format above
    # has lba_count as H (2 bytes) and reserved as 12s; let's recount:
    # 32+4+4+2+2+2+2 = 48, then lba_count needs 4 bytes; fix:
    assert len(entry) == INDEX_ENTRY_SIZE, f"Entry size {len(entry)} != {INDEX_ENTRY_SIZE}"
    return entry


def _pack_index_entry_correct(title: str, lba_offset: int, num_frames: int,
                               fps_num: int, fps_den: int, lba_count: int) -> bytes:
    """Correctly sized 64-byte index entry."""
    title_bytes = title.encode("utf-8")[:TITLE_MAX]
    title_field = title_bytes + b"\x00" * (32 - len(title_bytes))

    body = struct.pack(
        "<IIHHHI",
        lba_offset,     # 4
        num_frames,     # 4
        fps_num,        # 2
        fps_den,        # 2
        FRAME_WIDTH,    # 2
        FRAME_HEIGHT,   # 2  → subtotal body so far: 16
        lba_count,      # 4  → 20
    )
    # title(32) + body(20) = 52; need 12 bytes reserved
    entry = title_field + body + b"\x00" * 12
    assert len(entry) == INDEX_ENTRY_SIZE, f"Bug: entry is {len(entry)} bytes"
    return entry


# ── Main encode routine ────────────────────────────────────────────────────────

def encode(output_path: str, videos: list[dict]):
    """
    videos: list of dicts with keys: path, title, fps
    """
    num_videos   = len(videos)
    header_bytes = pack_header(num_videos)

    # We'll write the header placeholder, then collect index entries as we go,
    # then seek back to patch them in.  But since index entries need lba_offset
    # (which depends on prior video sizes) we process sequentially.

    # Calculate starting LBA for data (relative to byte 0 of file = LBA 0)
    header_and_index_bytes = HEADER_SIZE + num_videos * INDEX_ENTRY_SIZE
    meta_sectors = math.ceil(header_and_index_bytes / SECTOR_SIZE)
    meta_bytes   = meta_sectors * SECTOR_SIZE

    print(f"CinemaPlus Encoder v1.0")
    print(f"Output     : {output_path}")
    print(f"Videos     : {num_videos}")
    print(f"Meta bytes : {meta_bytes} ({meta_sectors} sectors)")
    print()

    index_entries  = []
    current_lba    = 0   # LBA offset from data_lba_start

    # First pass: encode each video to a temp file and record metadata
    temp_files = []
    for i, vid in enumerate(videos):
        title   = vid["title"]
        fps     = vid["fps"]
        fps_num = int(fps * 1000)
        fps_den = 1000

        print(f"[{i+1}/{num_videos}] Encoding '{title}' from {vid['path']} @ {fps} fps")
        total = count_frames_ffmpeg(vid["path"], fps)
        if total:
            print(f"  Estimated frames: {total}")

        tmp = tempfile.NamedTemporaryFile(delete=False, suffix=".fbin")
        temp_files.append(tmp.name)
        frame_count = 0

        for frame_idx, rgb in enumerate(iter_frames_ffmpeg(vid["path"], fps)):
            palette, indices = quantise_frame(rgb)
            write_frame(tmp, palette, indices)
            frame_count += 1
            if frame_count % 50 == 0:
                if total:
                    pct = frame_count * 100 // total
                    print(f"  {frame_count}/{total} frames ({pct}%)", end="\r")
                else:
                    print(f"  {frame_count} frames", end="\r")

        tmp.flush()
        tmp.close()
        lba_count = frame_count * FRAME_SECTORS

        print(f"  {frame_count} frames encoded, {lba_count} sectors")

        entry = _pack_index_entry_correct(
            title, current_lba, frame_count, fps_num, fps_den, lba_count
        )
        index_entries.append(entry)
        current_lba += lba_count

    # Second pass: write final output file
    print(f"\nWriting {output_path} ...")
    with open(output_path, "wb") as fout:
        # Header
        fout.write(header_bytes)
        # Index entries
        for entry in index_entries:
            fout.write(entry)
        # Pad to sector boundary
        written = HEADER_SIZE + num_videos * INDEX_ENTRY_SIZE
        pad = meta_bytes - written
        fout.write(b"\x00" * pad)
        # Video data
        for i, tmp_path in enumerate(temp_files):
            print(f"  Appending video {i+1}/{num_videos} ...")
            with open(tmp_path, "rb") as fin:
                while True:
                    chunk = fin.read(65536)
                    if not chunk:
                        break
                    fout.write(chunk)
            os.unlink(tmp_path)

    size_mb = os.path.getsize(output_path) / (1024 * 1024)
    print(f"\nDone!  {output_path}  ({size_mb:.1f} MB)")


# ── CLI ────────────────────────────────────────────────────────────────────────

class VideoAction(argparse.Action):
    """Collect --video FILE TITLE FPS triples."""
    def __call__(self, parser, namespace, values, option_string=None):
        if len(values) != 3:
            parser.error("--video requires exactly 3 arguments: FILE TITLE FPS")
        lst = getattr(namespace, self.dest, None) or []
        try:
            fps = float(values[2])
        except ValueError:
            parser.error(f"FPS must be a number, got: {values[2]}")
        lst.append({"path": values[0], "title": values[1], "fps": fps})
        setattr(namespace, self.dest, lst)


def main():
    parser = argparse.ArgumentParser(
        description="Encode videos into a CinemaPlus multi-video .cpv file",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("-o", "--output", default="VIDEO.CPV",
                        help="Output file (default: VIDEO.CPV)")
    parser.add_argument("--video", dest="videos", nargs=3,
                        metavar=("FILE", "TITLE", "FPS"),
                        action=VideoAction,
                        help="Add a video. Can be repeated.",
                        required=True)

    args = parser.parse_args()

    if not args.videos:
        parser.error("At least one --video is required")

    for v in args.videos:
        if not Path(v["path"]).exists():
            sys.exit(f"File not found: {v['path']}")
        if not (1 <= v["fps"] <= 60):
            sys.exit(f"FPS out of range (1–60): {v['fps']}")
        if len(v["title"]) > TITLE_MAX:
            print(f"Warning: title '{v['title']}' truncated to {TITLE_MAX} chars")

    encode(args.output, args.videos)


if __name__ == "__main__":
    main()
