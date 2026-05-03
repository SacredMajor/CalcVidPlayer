# CalcVidPlayer

Multi-video USB player for the TI-84 Plus CE.  
Combines Cinema's glitch-free async double-buffer playback with a multi-title selection menu.

## How it works

- Uses Cinema's **exact** async double-buffer pipeline (`msd_ReadAsync`) — same smooth, glitch-free output
- Reads raw from the USB drive (no FAT, same as Cinema — just `dd` the binary)
- Shows a menu on the calculator to pick which video to watch
- Supports up to 16 videos in one binary

## Controls

| Key | Action |
|-----|--------|
| UP / DOWN | Navigate menu |
| ENTER | Play selected video |
| 2ND / MODE | Return to menu during playback |
| 0 | Restart current video from beginning |
| CLEAR | Quit |

## Setup

### 1. Install Python dependencies

```bash
pip install Pillow numpy
```

FFmpeg must be installed too (`sudo apt install ffmpeg`).

### 2. Encode your videos

```bash
# Single video
python encode_cpv.py -o output.bin myvideo.mp4 "My Video" 10

# Multiple videos
python encode_cpv.py -o output.bin \
    jjk.mp4 "JJK Ep 42" 10 \
    avatar.mp4 "Avatar" 10
```

Arguments per video: `<file> <title> <fps>`  
**10 fps is recommended** (same as Cinema).

### 3. Write to USB drive

```bash
sudo dd if=output.bin of=/dev/sdX bs=4M status=progress
```

Replace `/dev/sdX` with your USB drive path (`sudo fdisk -l` to find it).

### 4. Build the calculator program

Requires [CEdev (CE Toolchain)](https://github.com/CE-Programming/toolchain).

```bash
make
```

Flash `bin/CALCVID.8xp` to your TI-84 Plus CE via TI Connect CE.

### 5. Play

1. Plug USB drive into calculator
2. Run CALCVID
3. Select your video with UP/DOWN + ENTER

## Technical notes

- Frame format: 160×96, 256-color palette per frame (identical to Cinema/FBin)
- Each frame = 32 sectors (16 384 bytes) on disk
- Binary layout: 8-sector header/index, then video data back-to-back
- The `encode_cpv.py` script replaces FBin — no need for FBin at all
