/**
 * CinemaPlus – Multi-video USB player for the TI-84 Plus CE
 * ==========================================================
 * Combines Cinema's glitch-free FBin rendering pipeline with a multi-title
 * selection menu inspired by TICEVid.
 *
 * Build with CE Toolchain (CEdev).  Add to your makefile:
 *   LIBS = msddrvce fatdrvce keypadc graphx
 *
 * USB drive must contain VIDEO.CPV in the FAT32 root directory.
 * See FORMAT.md for the binary format specification.
 */

#include <graphx.h>
#include <keypadc.h>
#include <msddrvce.h>
#include <fatdrvce.h>
#include <tice.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* ── Format constants (must match encoder) ─────────────────────────────────── */

#define CPV_MAGIC         "CPVF"
#define CPV_VERSION       1

#define FRAME_WIDTH       160
#define FRAME_HEIGHT      96
#define PALETTE_BYTES     (256 * 3)          /* 768 */
#define PIXEL_BYTES       (FRAME_WIDTH * FRAME_HEIGHT) /* 15 360 */
#define FRAME_RAW_BYTES   (PALETTE_BYTES + PIXEL_BYTES)/* 16 128 */
#define SECTOR_SIZE       512
#define FRAME_SECTORS     32                 /* ceil(16128/512) */
#define FRAME_PADDED      (FRAME_SECTORS * SECTOR_SIZE)/* 16 384 */

#define MAX_VIDEOS        32
#define TITLE_LEN         32

/* ── Structs matching the on-disk binary layout ────────────────────────────── */

/* All fields little-endian – ez80 is little-endian, so direct cast is safe. */

typedef struct __attribute__((packed)) {
    char     magic[4];          /* "CPVF"                          */
    uint16_t version;           /* 1                               */
    uint8_t  num_videos;
    uint8_t  flags;
    uint32_t index_offset;      /* always 32                       */
    uint32_t data_lba_start;    /* first video data LBA in file    */
    uint8_t  reserved[16];
} cpv_header_t;                 /* 32 bytes                        */

typedef struct __attribute__((packed)) {
    char     title[TITLE_LEN];  /* null-terminated                 */
    uint32_t lba_offset;        /* offset from data_lba_start      */
    uint32_t num_frames;
    uint16_t fps_numerator;
    uint16_t fps_denominator;
    uint16_t width;
    uint16_t height;
    uint32_t lba_count;
    uint8_t  reserved[12];
} cpv_index_entry_t;            /* 64 bytes                        */

/* ── Global driver state ────────────────────────────────────────────────────── */

static msd_t  g_msd;
static fat_t  g_fat;

/* We need two sector-aligned buffers:
 *   - One for reading frames from USB (FRAME_PADDED = 16 384 bytes = 32 sectors)
 *   - One for the header/index (1 sector = 512 bytes)
 *
 * The ez80 only has ~154 KB user RAM.  16 384 bytes is a big chunk but Cinema
 * uses this same size successfully, so we follow the same approach.
 *
 * fatdrvce requires buffers aligned to at least 4 bytes; the linker places
 * BSS at a fixed address so static arrays are fine.
 */
static uint8_t g_frame_buf[FRAME_PADDED];     /* 16 KB – frame read buffer   */
static uint8_t g_meta_buf[SECTOR_SIZE];       /* 512 B  – header / index     */

/* ── Parsed index ───────────────────────────────────────────────────────────── */

static cpv_header_t      g_header;
static cpv_index_entry_t g_index[MAX_VIDEOS];
static uint8_t           g_num_videos;

/* ── USB / FAT helpers ──────────────────────────────────────────────────────── */

/* Fat file handle for VIDEO.CPV */
static fat_file_t g_cpv_file;
static bool       g_file_open = false;

/* Read `count` sectors starting at `file_sector` into `buf`.
 * file_sector is relative to the start of VIDEO.CPV (sector 0 = first 512 bytes
 * of the file).  Returns true on success. */
static bool cpv_read_sectors(uint32_t file_sector, uint24_t count, void *buf) {
    fat_error_t err = fat_SetFilePos(&g_cpv_file, file_sector * SECTOR_SIZE);
    if (err != FAT_SUCCESS) return false;
    uint24_t got = fat_ReadFile(&g_cpv_file, count * SECTOR_SIZE, buf);
    return got == count * SECTOR_SIZE;
}

/* ── Initialise USB + FAT ───────────────────────────────────────────────────── */

static bool usb_init(void) {
    usb_error_t usberr;
    msd_error_t msderr;
    fat_error_t faterr;

    /* Initialise USB host */
    usberr = usb_Init(NULL, NULL, NULL, USB_DEFAULT_INIT_FLAGS);
    if (usberr != USB_SUCCESS) return false;

    /* Wait for drive to enumerate (max ~3 s) */
    uint8_t timeout = 100;
    while (timeout--) {
        usb_HandleEvents();
        if (msd_Open(&g_msd, usb_FindDevice(NULL, NULL, USB_SKIP_NONE)) == MSD_SUCCESS)
            break;
        delay(30);
    }
    if (!g_msd.device) return false;

    /* Mount FAT filesystem (partition 0) */
    faterr = fat_Init(&g_fat, &g_msd, g_meta_buf, 0);
    if (faterr != FAT_SUCCESS) return false;

    return true;
}

static void usb_cleanup(void) {
    if (g_file_open) {
        fat_CloseFile(&g_cpv_file);
        g_file_open = false;
    }
    fat_Deinit(&g_fat);
    msd_Close(&g_msd);
    usb_Cleanup();
}

/* ── Load header + index from VIDEO.CPV ─────────────────────────────────────── */

static bool cpv_open(void) {
    fat_error_t err = fat_OpenFile(&g_fat, "VIDEO.CPV", FAT_READ, &g_cpv_file);
    if (err != FAT_SUCCESS) return false;
    g_file_open = true;

    /* Read first sector (contains header and start of index) */
    if (!cpv_read_sectors(0, 1, g_meta_buf)) return false;

    /* Validate header */
    cpv_header_t *hdr = (cpv_header_t *)g_meta_buf;
    if (memcmp(hdr->magic, CPV_MAGIC, 4) != 0)   return false;
    if (hdr->version != CPV_VERSION)               return false;
    if (hdr->num_videos == 0)                      return false;

    memcpy(&g_header, hdr, sizeof(g_header));
    g_num_videos = hdr->num_videos;
    if (g_num_videos > MAX_VIDEOS) g_num_videos = MAX_VIDEOS;

    /* Read index entries.  Each entry is 64 bytes; 8 entries fit in one
     * sector (512 / 64 = 8).  Read as many sectors as needed. */
    uint32_t index_bytes   = (uint32_t)g_num_videos * sizeof(cpv_index_entry_t);
    uint32_t index_sectors = (index_bytes + SECTOR_SIZE - 1) / SECTOR_SIZE;
    /* index starts at byte 32, which is still in sector 0 – already read.
     * For >7 videos we need additional sectors. */
    uint8_t  *dst = (uint8_t *)g_index;
    const uint8_t *src = g_meta_buf + g_header.index_offset;

    /* Copy what's available in the already-read sector */
    uint24_t avail = SECTOR_SIZE - (uint24_t)g_header.index_offset;
    uint24_t need  = (uint24_t)index_bytes;
    uint24_t copy1 = avail < need ? avail : need;
    memcpy(dst, src, copy1);
    dst  += copy1;
    need -= copy1;

    /* Read remaining sectors if index spills into them */
    for (uint32_t s = 1; need > 0 && s < index_sectors + 1; s++) {
        if (!cpv_read_sectors(s, 1, g_meta_buf)) return false;
        uint24_t chunk = need < SECTOR_SIZE ? need : SECTOR_SIZE;
        memcpy(dst, g_meta_buf, chunk);
        dst  += chunk;
        need -= chunk;
    }

    return true;
}

/* ── Menu ────────────────────────────────────────────────────────────────────── */

/* Returns the selected video index, or -1 to quit. */
static int8_t run_menu(void) {
    uint8_t selected = 0;

    while (true) {
        /* ── Draw menu ── */
        gfx_FillScreen(0x00); /* black */

        /* Title bar */
        gfx_SetColor(0x12); /* dark blue */
        gfx_FillRectangle(0, 0, LCD_WIDTH, 18);
        gfx_SetTextFGColor(0xFF);    /* white */
        gfx_SetTextBGColor(0x12);
        gfx_SetTextXY(4, 4);
        gfx_PrintString("CinemaPlus");

        /* Subtitle */
        gfx_SetTextFGColor(0xC0);
        gfx_SetTextBGColor(0x00);
        gfx_SetTextXY(4, 22);
        gfx_PrintString("Select video: [ENTER] play  [CLEAR] quit");

        /* List */
        for (uint8_t i = 0; i < g_num_videos; i++) {
            uint8_t y = 40 + i * 16;

            if (i == selected) {
                gfx_SetColor(0x1F); /* bright blue highlight */
                gfx_FillRectangle(0, y - 2, LCD_WIDTH, 14);
                gfx_SetTextFGColor(0xFF);
                gfx_SetTextBGColor(0x1F);
            } else {
                gfx_SetTextFGColor(0xE0);
                gfx_SetTextBGColor(0x00);
            }
            gfx_SetTextXY(8, y);
            gfx_PrintString(g_index[i].title);

            /* fps info, right-aligned */
            char info[16];
            uint16_t fps = g_index[i].fps_numerator / g_index[i].fps_denominator;
            sprintf(info, "%u fps", fps);
            gfx_SetTextXY(LCD_WIDTH - 60, y);
            gfx_PrintString(info);
        }

        /* Footer */
        gfx_SetTextFGColor(0x80);
        gfx_SetTextBGColor(0x00);
        gfx_SetTextXY(4, LCD_HEIGHT - 12);
        gfx_PrintString("UP/DOWN to navigate");

        gfx_SwapDraw();

        /* ── Input ── */
        kb_Scan();
        while (kb_IsDown(kb_KeyUp) || kb_IsDown(kb_KeyDown) ||
               kb_IsDown(kb_KeyEnter) || kb_IsDown(kb_KeyClear)) {
            /* Debounce: wait for key release */
            kb_Scan();
            delay(10);
        }

        /* Wait for a fresh keypress */
        do { kb_Scan(); } while (!kb_AnyKey());

        if (kb_IsDown(kb_KeyUp)) {
            if (selected > 0) selected--;
        } else if (kb_IsDown(kb_KeyDown)) {
            if (selected < g_num_videos - 1) selected++;
        } else if (kb_IsDown(kb_KeyEnter)) {
            return (int8_t)selected;
        } else if (kb_IsDown(kb_KeyClear)) {
            return -1;
        }
    }
}

/* ── Playback ────────────────────────────────────────────────────────────────── */

/**
 * Play video at index `vid_idx`.
 * Returns true  → user wants to return to menu.
 * Returns false → user pressed CLEAR (quit entirely).
 */
static bool play_video(uint8_t vid_idx) {
    cpv_index_entry_t *vid = &g_index[vid_idx];

    /* Calculate frame timing.  The ez80 timer runs at 32 768 Hz.
     * frame_ticks = 32768 * fps_denominator / fps_numerator               */
    uint32_t frame_ticks = (uint32_t)32768 * vid->fps_denominator
                           / vid->fps_numerator;

    /* Base file sector for this video's data:
     *   data_lba_start  (sectors from file start to first video data)
     * + lba_offset      (sectors from data_lba_start to this video)       */
    uint32_t base_sector = g_header.data_lba_start + vid->lba_offset;

    uint32_t frame_idx = 0;
    bool     paused    = false;
    bool     return_to_menu = false;

    /* ── Resume / restart dialog ── */
    /* (Shown only if a hypothetical saved position were present; for
     *  simplicity we always start from frame 0 unless the user pressed 0
     *  during a previous playback – that is handled inside the loop below.) */

    gfx_SetDrawBuffer();

    while (frame_idx < vid->num_frames) {
        uint32_t t_start = rtc_Time(); /* 32 768 Hz ticks */

        /* ── Read frame ── */
        uint32_t sector = base_sector + (uint32_t)frame_idx * FRAME_SECTORS;
        if (!cpv_read_sectors(sector, FRAME_SECTORS, g_frame_buf)) {
            /* Read error – show brief message and return to menu */
            gfx_SetTextFGColor(0xE0);
            gfx_SetTextBGColor(0x00);
            gfx_SetTextXY(4, 120);
            gfx_PrintString("Read error – returning to menu");
            gfx_SwapDraw();
            delay(1500);
            return true;
        }

        /* ── Decode palette ── */
        /* Palette is first 768 bytes: interleaved R,G,B per entry.
         * gfx_SetPalette expects a uint16_t* array of 1555 or 565 colours.
         * The CE LCD uses 1555 format: 0RRRRRGGGGGBBBBB
         * R,G,B in the file are 8-bit; we downshift to 5-bit.             */
        uint8_t  *pal_src  = g_frame_buf;
        uint16_t  pal16[256];
        for (uint16_t i = 0; i < 256; i++) {
            uint8_t r = pal_src[i * 3 + 0] >> 3;
            uint8_t g = pal_src[i * 3 + 1] >> 3;
            uint8_t b = pal_src[i * 3 + 2] >> 3;
            pal16[i]  = ((uint16_t)r << 10) | ((uint16_t)g << 5) | b;
        }
        gfx_SetPalette(pal16, sizeof(pal16), 0);

        /* ── Blit pixels ── */
        /* Pixel data starts at byte 768.
         * gfx_GetBuffer() returns a pointer to the off-screen buffer.
         * The CE display buffer is 320×240.  We render the 160×96 video
         * centred: x_off = (320-160)/2 = 80, y_off = (240-96)/2 = 72.    */
        #define X_OFF 80
        #define Y_OFF 72

        uint8_t *pixels = g_frame_buf + PALETTE_BYTES;
        uint8_t *screen = gfx_GetBuffer();

        for (uint8_t row = 0; row < FRAME_HEIGHT; row++) {
            uint8_t *src_row = pixels + (uint32_t)row * FRAME_WIDTH;
            uint8_t *dst_row = screen + (uint24_t)(Y_OFF + row) * 320 + X_OFF;
            memcpy(dst_row, src_row, FRAME_WIDTH);
        }
        gfx_SwapDraw();

        frame_idx++;

        /* ── Input handling ── */
        kb_Scan();
        if (kb_IsDown(kb_KeyClear)) {
            /* Quit entirely */
            return false;
        }
        if (kb_IsDown(kb_Key2nd) || kb_IsDown(kb_KeyMode)) {
            /* Return to menu */
            return true;
        }
        if (kb_IsDown(kb_Key0)) {
            /* Restart from beginning (Cinema's resume feature) */
            frame_idx = 0;
        }
        if (kb_IsDown(kb_KeyEnter)) {
            /* Toggle pause */
            paused = !paused;
            while (kb_IsDown(kb_KeyEnter)) { kb_Scan(); delay(10); }
        }

        /* If paused, wait until enter again */
        while (paused) {
            kb_Scan();
            if (kb_IsDown(kb_KeyEnter)) {
                paused = false;
                while (kb_IsDown(kb_KeyEnter)) { kb_Scan(); delay(10); }
            }
            if (kb_IsDown(kb_KeyClear)) return false;
            if (kb_IsDown(kb_Key2nd) || kb_IsDown(kb_KeyMode)) return true;
        }

        /* ── Frame rate throttle ── */
        /* Busy-wait for the remainder of the frame period.
         * rtc_Time() wraps at 32768 per second; we handle simple wrap.    */
        uint32_t elapsed = rtc_Time() - t_start;
        if (elapsed < frame_ticks) {
            while ((rtc_Time() - t_start) < frame_ticks) {
                /* Poll USB to keep enumeration alive */
                usb_HandleEvents();
            }
        }
    }

    /* Video finished – return to menu */
    return true;
}

/* ── Entry point ─────────────────────────────────────────────────────────────── */

int main(void) {
    /* Initialise graphics */
    gfx_Begin();
    gfx_SetDrawBuffer();
    gfx_FillScreen(0x00);
    gfx_SetTextFGColor(0xFF);
    gfx_SetTextBGColor(0x00);
    gfx_SetTextXY(4, 100);
    gfx_PrintString("CinemaPlus  –  initialising USB ...");
    gfx_SwapDraw();

    /* Initialise USB + FAT */
    if (!usb_init()) {
        gfx_SetTextXY(4, 120);
        gfx_PrintString("USB init failed. Insert drive & retry.");
        gfx_SwapDraw();
        delay(3000);
        goto cleanup;
    }

    /* Open VIDEO.CPV and load index */
    if (!cpv_open()) {
        gfx_SetTextXY(4, 120);
        gfx_PrintString("VIDEO.CPV not found or invalid.");
        gfx_SwapDraw();
        delay(3000);
        goto cleanup;
    }

    /* Main menu loop */
    while (true) {
        int8_t choice = run_menu();
        if (choice < 0) break;           /* CLEAR pressed → quit */

        bool back_to_menu = play_video((uint8_t)choice);
        if (!back_to_menu) break;        /* CLEAR during playback → quit */
    }

cleanup:
    usb_cleanup();
    gfx_End();
    return 0;
}
