#!/usr/bin/env python3
"""
encode_cpv.py  –  CalcVidPlayer CPVF v1 encoder
================================================
Encodes one or more MP4 files into a raw CPVF binary suitable for writing
directly to a USB drive with dd.  No filesystem is used.

Usage
-----
python3 encode_cpv.py -o output.bin \
    --video jjk.mp4    "JJK Ep 42" 10 \
        --srt en_us captions_en.srt \
        --srt fr_fr captions_fr.srt \
    --video avatar.mp4 "Avatar"    10 \
        --start 00:05:00 \
        --srt en_us avatar_en.srt

Arguments that belong to a --video entry must follow it before the next
--video (or -o / end of args).

Binary layout (little-endian, no filesystem)
--------------------------------------------
Sector 0        : CPVF global header (32 bytes)
Sectors 0-N     : Title index entries (64 bytes each)
After index     : Caption track descriptors (16 bytes per track, per title)
After descs     : Caption data (88 bytes per entry), padded to sector
Video data      : Raw FBin frames concatenated (31 sectors per frame)

Environment assumptions
-----------------------
  fbin  lives at  ~/fbin/linux/bin/fbin
  Videos live anywhere you like; paths are supplied on the CLI.
"""

import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

# ── Constants ────────────────────────────────────────────────────────────────

SECTOR_SIZE   = 512
PAL_SECTORS   = 1          # 256 × uint16 = 512 bytes
PIX_SECTORS   = 30         # 160 × 96 = 15 360 bytes
FRAME_SECTORS = 31         # per frame on disk

FBIN_PATH = Path.home() / "fbin" / "linux" / "bin" / "fbin"
FBIN_W    = 160
FBIN_H    = 96
FBIN_PAL  = 256

HEADER_SIZE     = 32       # global header
TITLE_ENTRY_SZ  = 68       # per title (32+4+4+4+2+2+2+2+4+1+11 = 68)
CAP_TRACK_SZ    = 16       # per caption track descriptor
# Note: spec says 64 but actual field count (32+4+4+4+2+2+2+2+4+1+11) = 68
CAP_ENTRY_SZ    = 88       # per caption cue (4+4+80)
CAP_TEXT_LEN    = 80

# ── SRT parser ────────────────────────────────────────────────────────────────

_TS_RE = re.compile(
    r'(\d{1,2}):(\d{2}):(\d{2})[,.](\d{3})'
)

def _parse_ts(ts_str: str) -> float:
    """Return timestamp in seconds."""
    m = _TS_RE.match(ts_str.strip())
    if not m:
        raise ValueError(f"Bad SRT timestamp: {ts_str!r}")
    h, mi, s, ms = int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4))
    return h * 3600 + mi * 60 + s + ms / 1000.0


def _strip_html(text: str) -> str:
    """Remove <i>, <b> etc. from SRT text."""
    return re.sub(r'<[^>]+>', '', text)


def parse_srt(path: str, fps: float) -> list[tuple[int, int, str]]:
    """
    Parse an SRT file.  Returns list of (start_frame, end_frame, text).
    text is truncated to CAP_TEXT_LEN-1 characters.
    """
    entries = []
    with open(path, 'r', encoding='utf-8-sig', errors='replace') as f:
        content = f.read()

    # Split on blank lines; each block is one cue
    blocks = re.split(r'\n\s*\n', content.strip())
    for block in blocks:
        lines = block.strip().splitlines()
        if len(lines) < 3:
            continue
        # line 0: sequence number (skip)
        # line 1: timestamps
        # lines 2+: text
        try:
            ts_line = lines[1]
            parts = ts_line.split('-->')
            if len(parts) != 2:
                continue
            start_s = _parse_ts(parts[0])
            end_s   = _parse_ts(parts[1])
        except (ValueError, IndexError):
            continue

        raw_text = ' '.join(lines[2:])
        text     = _strip_html(raw_text).strip()
        text     = text[:CAP_TEXT_LEN - 1]

        start_frame = int(start_s * fps)
        end_frame   = int(end_s   * fps)
        if end_frame <= start_frame:
            end_frame = start_frame + 1

        entries.append((start_frame, end_frame, text))

    return entries


# ── Helpers ───────────────────────────────────────────────────────────────────

def pad_to_sector(data: bytearray) -> bytearray:
    """Pad bytearray to next sector boundary."""
    rem = len(data) % SECTOR_SIZE
    if rem:
        data += b'\x00' * (SECTOR_SIZE - rem)
    return data


def sector_count(n_bytes: int) -> int:
    """Return number of 512-byte sectors needed for n_bytes."""
    return (n_bytes + SECTOR_SIZE - 1) // SECTOR_SIZE


def parse_start_time(s: str) -> float:
    """Parse HH:MM:SS or MM:SS or seconds string to seconds."""
    parts = s.split(':')
    try:
        if len(parts) == 3:
            return int(parts[0]) * 3600 + int(parts[1]) * 60 + float(parts[2])
        elif len(parts) == 2:
            return int(parts[0]) * 60 + float(parts[1])
        else:
            return float(s)
    except ValueError:
        raise ValueError(f"Cannot parse start time: {s!r}")


# ── FBin encoding ─────────────────────────────────────────────────────────────

def encode_fbin(input_mp4: str, fps: int, tmp_dir: str) -> Path:
    """
    Call fbin to encode input_mp4 at given fps into a temp .fbin file.
    Returns the path to the output file.
    Raises subprocess.CalledProcessError on failure.
    """
    out_path = Path(tmp_dir) / (Path(input_mp4).stem + ".fbin")
    cmd = [
        str(FBIN_PATH),
        "-i", input_mp4,
        "-o", str(out_path),
        "-s", f"{FBIN_W}:{FBIN_H}",
        "-r", str(fps),
        "-p", str(FBIN_PAL),
    ]
    print(f"  [fbin] {' '.join(cmd)}")
    subprocess.run(cmd, check=True)
    return out_path


def count_frames_in_fbin(fbin_path: Path) -> int:
    """Count frames from file size: each frame is FRAME_SECTORS * SECTOR_SIZE bytes."""
    size = fbin_path.stat().st_size
    frame_bytes = FRAME_SECTORS * SECTOR_SIZE
    return size // frame_bytes


# ── Argument parsing ──────────────────────────────────────────────────────────

def parse_args() -> tuple[str, list[dict]]:
    """
    Custom parser because argparse doesn't handle the per---video sub-args cleanly.
    Returns (output_path, list_of_video_dicts).

    Each video_dict has:
      path      : str
      title     : str
      fps       : int
      start_s   : float  (start time in seconds, 0.0 if not given)
      srts      : list of (lang: str, path: str)
    """
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        sys.exit(0)

    output = None
    videos = []
    current = None
    i = 0

    def flush():
        if current is not None:
            videos.append(current)

    while i < len(args):
        a = args[i]
        if a == '-o':
            output = args[i + 1]; i += 2
        elif a == '--video':
            flush()
            current = {
                'path':    args[i + 1],
                'title':   args[i + 2],
                'fps':     int(args[i + 3]),
                'start_s': 0.0,
                'srts':    [],
            }
            i += 4
        elif a == '--start':
            if current is None:
                sys.exit("--start must follow --video")
            current['start_s'] = parse_start_time(args[i + 1])
            i += 2
        elif a == '--srt':
            if current is None:
                sys.exit("--srt must follow --video")
            lang = args[i + 1]
            srt_path = args[i + 2]
            current['srts'].append((lang, srt_path))
            i += 3
        else:
            sys.exit(f"Unknown argument: {a!r}")

    flush()

    if output is None:
        sys.exit("Missing -o output.bin")
    if not videos:
        sys.exit("No --video entries specified")

    return output, videos


# ── Main encoder ──────────────────────────────────────────────────────────────

def main():
    output_path, video_specs = parse_args()

    print(f"\nCalcVidPlayer encoder — {len(video_specs)} title(s)")
    print(f"Output: {output_path}\n")

    # Check fbin exists
    if not FBIN_PATH.exists():
        sys.exit(f"fbin not found at {FBIN_PATH}\n"
                 "Install it or update FBIN_PATH in this script.")

    with tempfile.TemporaryDirectory() as tmp_dir:
        # ── Step 1: encode each video with fbin ──────────────────────
        encoded = []   # list of { spec, fbin_path, num_frames, start_frame }
        for spec in video_specs:
            print(f"Encoding: {spec['title']} ({spec['path']}) @ {spec['fps']} FPS")
            fbin_path = encode_fbin(spec['path'], spec['fps'], tmp_dir)
            num_frames = count_frames_in_fbin(fbin_path)
            start_frame = int(spec['start_s'] * spec['fps'])
            if start_frame >= num_frames:
                print(f"  WARNING: start_frame {start_frame} >= num_frames {num_frames}, clamping to 0")
                start_frame = 0
            print(f"  → {num_frames} frames, start_frame={start_frame}")
            encoded.append({
                'spec':        spec,
                'fbin_path':   fbin_path,
                'num_frames':  num_frames,
                'start_frame': start_frame,
            })

        # ── Step 2: parse SRT files ───────────────────────────────────
        for enc in encoded:
            spec = enc['spec']
            fps = float(spec['fps'])
            enc['cap_entries'] = []   # list of (lang, [(sf,ef,text), ...])
            for lang, srt_path in spec['srts']:
                print(f"  Parsing SRT: {srt_path} [{lang}]")
                cues = parse_srt(srt_path, fps)
                print(f"    {len(cues)} cues")
                enc['cap_entries'].append((lang, cues))

        n = len(encoded)

        # ── Step 3: lay out the binary ────────────────────────────────
        #
        # Byte offsets:
        #   0                              : global header (32 bytes)
        #   32                             : title index entries (n × 64)
        #   32 + n*64                      : caption track descriptors
        #                                    (sum of num_tracks × 16)
        #   After all descriptors, pad     : caption data region
        #   Pad to sector, then            : video data (data_lba_start)

        # Build caption data blobs first so we know sizes
        # cap_data_blobs[t][k] = bytes for track k of title t
        cap_data_blobs = []
        for enc in encoded:
            blobs = []
            for lang, cues in enc['cap_entries']:
                blob = bytearray()
                for (sf, ef, text) in cues:
                    txt_bytes = text.encode('ascii', errors='replace')[:CAP_TEXT_LEN - 1]
                    txt_bytes = txt_bytes.ljust(CAP_TEXT_LEN, b'\x00')
                    blob += struct.pack('<II', sf, ef) + txt_bytes
                blobs.append(blob)
            cap_data_blobs.append(blobs)

        # Descriptor region
        desc_region = bytearray()
        # We'll fill data_offset relative to the start of the caption data region,
        # which we compute after we know descriptor region size.
        # Keep track of per-track (blob index, data_offset placeholder)
        desc_entries = []   # list of (lang, size, placeholder_offset_in_desc_region)

        for t, enc in enumerate(encoded):
            for k, (lang, _) in enumerate(enc['cap_entries']):
                blob = cap_data_blobs[t][k]
                lang_bytes = lang.encode('ascii', errors='replace')[:8].ljust(8, b'\x00')
                # data_offset will be filled in later; write 0 as placeholder
                ph_off = len(desc_region) + 8   # offset of data_offset field
                desc_region += lang_bytes + struct.pack('<II', 0, len(blob))
                desc_entries.append((t, k, ph_off))

        # Now compute caption data region start (byte offset from disk start)
        header_and_index_size = HEADER_SIZE + n * TITLE_ENTRY_SZ  # 32 + n*68
        desc_region_start     = header_and_index_size
        cap_data_byte_start   = desc_region_start + len(desc_region)
        # Pad to sector boundary
        pad_to = sector_count(cap_data_byte_start) * SECTOR_SIZE
        cap_data_padding = pad_to - cap_data_byte_start
        cap_data_sector_start = pad_to // SECTOR_SIZE   # = data_lba after metadata

        # Fill in data_offset fields in desc_region
        cap_data_offset = 0
        for t, enc in enumerate(encoded):
            for k in range(len(enc['cap_entries'])):
                _, _, ph_off = desc_entries.pop(0)
                struct.pack_into('<I', desc_region, ph_off, cap_data_offset)
                cap_data_offset += len(cap_data_blobs[t][k])

        # Concatenate all caption data
        cap_data_region = bytearray()
        for blobs in cap_data_blobs:
            for blob in blobs:
                cap_data_region += blob
        cap_data_region = pad_to_sector(cap_data_region)

        # The video data starts right after the cap_data_region
        # metadata sector count = sectors occupied by [header+index+desc+cap_data]
        metadata_bytes = (
            HEADER_SIZE
            + n * TITLE_ENTRY_SZ
            + len(desc_region)
            + cap_data_padding
            + len(cap_data_region)
        )
        data_lba_start = sector_count(metadata_bytes - len(cap_data_region))
        # Simpler: first sector of video = sector after cap_data ends
        # header+index+desc, padded → cap_data → video
        pre_video_bytes = (
            HEADER_SIZE + n * TITLE_ENTRY_SZ + len(desc_region) + cap_data_padding
            + len(cap_data_region)
        )
        data_lba_start = pre_video_bytes // SECTOR_SIZE

        # ── Step 4: build title index entries ────────────────────────
        lba_cursor = 0   # relative to data_lba_start
        title_entries_bytes = bytearray()
        for t, enc in enumerate(encoded):
            spec = enc['spec']
            title_str = spec['title'].encode('ascii', errors='replace')[:31].ljust(32, b'\x00')
            lba_offset    = lba_cursor
            lba_count     = enc['num_frames'] * FRAME_SECTORS
            num_cap_tracks= len(enc['cap_entries'])

            entry = struct.pack('<32sIIIHHHHIB11s',
                title_str,
                lba_offset,
                enc['start_frame'],
                enc['num_frames'],
                spec['fps'],    # fps_num
                1,              # fps_den
                FBIN_W,
                FBIN_H,
                lba_count,
                num_cap_tracks,
                b'\x00' * 11,
            )
            assert len(entry) == TITLE_ENTRY_SZ, f"entry size {len(entry)} != {TITLE_ENTRY_SZ}"
            title_entries_bytes += entry
            lba_cursor += lba_count

        # ── Step 5: build global header ───────────────────────────────
        header = struct.pack('<4sHBBIII16s',
            b'CPVF',
            1,                  # version
            n,                  # num_titles
            0,                  # flags
            HEADER_SIZE,        # index_offset
            data_lba_start,
            0,                  # unused field (was duplicated by mistake)
            b'\x00' * 16,
        )
        # Trim to 32 bytes — struct above packs 4+2+1+1+4+4+4+16 = 36; fix format:
        # Correct: magic(4) version(2) num_titles(1) flags(1) index_offset(4)
        #          data_lba_start(4) reserved(16) = 32
        header = struct.pack('<4sHBBII16s',
            b'CPVF',
            1,
            n,
            0,
            HEADER_SIZE,
            data_lba_start,
            b'\x00' * 16,
        )
        assert len(header) == HEADER_SIZE, len(header)

        # ── Step 6: write output file ─────────────────────────────────
        print(f"\nAssembling output binary...")

        with open(output_path, 'wb') as out:
            # Global header
            out.write(header)

            # Title index entries
            out.write(title_entries_bytes)

            # Caption track descriptors
            out.write(desc_region)

            # Padding to sector boundary (before cap data)
            out.write(b'\x00' * cap_data_padding)

            # Caption data region
            out.write(cap_data_region)

            # Video data — stream fbin files
            for t, enc in enumerate(encoded):
                print(f"  Writing video: {enc['spec']['title']} "
                      f"({enc['num_frames']} frames, "
                      f"{enc['fbin_path'].stat().st_size // 1024} KB)...")
                with open(enc['fbin_path'], 'rb') as f:
                    while chunk := f.read(1024 * 1024):
                        out.write(chunk)
                # Pad to frame boundary just in case
                pos = out.tell()
                frame_bytes = FRAME_SECTORS * SECTOR_SIZE
                rem = pos % frame_bytes
                if rem:
                    out.write(b'\x00' * (frame_bytes - rem))

        # ── Summary ───────────────────────────────────────────────────
        total_size = Path(output_path).stat().st_size
        print(f"\n{'─'*60}")
        print(f"Done!  Output: {output_path}")
        print(f"Total size : {total_size:,} bytes "
              f"({total_size / 1024 / 1024:.2f} MB)")
        print(f"data_lba_start : {data_lba_start}")
        print()
        for t, enc in enumerate(encoded):
            spec = enc['spec']
            print(f"  [{t}] {spec['title']}")
            print(f"       {enc['num_frames']} frames @ {spec['fps']} FPS  "
                  f"start_frame={enc['start_frame']}")
            for lang, cues in enc['cap_entries']:
                print(f"       caption [{lang}]: {len(cues)} cues")
        print()
        print("Write to USB drive:")
        print(f"  sudo dd if={output_path} of=/dev/sdX bs=4M status=progress")
        print()


if __name__ == '__main__':
    main()
