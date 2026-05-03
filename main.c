/**
 * CalcVidPlayer – Multi-video USB player for the TI-84 Plus CE
 * ==========================================================
 * Uses Cinema's exact async double-buffer raw LBA pipeline for glitch-free
 * playback, plus a multi-title selection menu.
 *
 * Binary format on the USB drive (written raw with dd, no FAT):
 *
 *   LBA 0  : Header (512 bytes)
 *   LBA 1+ : Index entries (64 bytes each, packed into sectors)
 *   LBA N  : Video data back-to-back (each frame = 32 sectors)
 *
 * The Python encoder writes the whole thing; just dd it to the drive.
 *
 * Build with CE Toolchain (CEdev).  Libs needed: msddrvce fileioc graphx
 */

typedef struct global global_t;
#define usb_callback_data_t global_t

#include <fileioc.h>
#include <graphx.h>
#include <msddrvce.h>
#include <tice.h>
#include <usbdrvce.h>

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* ── Format constants (must match encoder) ───────────────────────────────── */

#define FRAME_WIDTH      160
#define FRAME_HEIGHT     96
#define FRAME_SECTORS    32      /* 32 sectors × 512 = 16 384 bytes per frame  */
                                 /* palette(768) + pixels(15360) + pad(256)    */
#define MAX_VIDEOS       16
#define TITLE_LEN        32

/* LBA where actual video data starts (after header + index sectors) */
/* Header = 1 sector, Index = ceil(MAX_VIDEOS * 64 / 512) = 2 sectors */
/* We reserve 8 sectors for the header/index to be safe               */
#define DATA_LBA_START   8

/* ── On-disk structs (512-byte header sector) ────────────────────────────── */

typedef struct __attribute__((packed)) {
    char     magic[4];        /* "CPVF"                              */
    uint8_t  version;         /* 1                                   */
    uint8_t  num_videos;      /* 1–16                                */
    uint16_t reserved0;
    uint32_t data_lba_start;  /* always DATA_LBA_START               */
    uint8_t  reserved[500];   /* pad to 512 bytes                    */
} cpv_header_t;               /* exactly 512 bytes = 1 sector        */

typedef struct __attribute__((packed)) {
    char     title[TITLE_LEN]; /* null-terminated                   */
    uint32_t lba_offset;       /* offset from data_lba_start        */
    uint32_t num_frames;
    uint16_t fps_num;
    uint16_t fps_den;
    uint8_t  reserved[20];    /* pad to 64 bytes                    */
} cpv_entry_t;                /* exactly 64 bytes                   */

/* ── USB global ──────────────────────────────────────────────────────────── */

struct global {
    usb_device_t usb;
    msd_t        msd;
};

static global_t g_global;

/* ── Index loaded from drive ─────────────────────────────────────────────── */

static cpv_header_t g_header;
static cpv_entry_t  g_index[MAX_VIDEOS];
static uint8_t      g_num_videos;

/* ── Sprite & palette double-buffers (same as Cinema) ────────────────────── */

static gfx_sprite_t *sprite_buf_1;
static gfx_sprite_t *sprite_buf_2;
static uint16_t      pal_buf_1[256];
static uint16_t      pal_buf_2[256];

/* ── Async transfer state ────────────────────────────────────────────────── */

static volatile bool g_render_ready = false;
static msd_transfer_t g_xfer_palette;
static msd_transfer_t g_xfer_image;

/* ── USB helpers ─────────────────────────────────────────────────────────── */

static void putstr(char *str) {
    os_PutStrFull(str);
    os_NewLine();
}

static usb_error_t handle_usb_event(usb_event_t event, void *event_data,
                                     usb_callback_data_t *g) {
    switch (event) {
        case USB_DEVICE_DISCONNECTED_EVENT:
            if (g->usb) msd_Close(&g->msd);
            g->usb = NULL;
            break;
        case USB_DEVICE_CONNECTED_EVENT:
            return usb_ResetDevice(event_data);
        case USB_DEVICE_ENABLED_EVENT:
            g->usb = event_data;
            break;
        case USB_DEVICE_DISABLED_EVENT:
            return USB_USER_ERROR;
        default:
            break;
    }
    return USB_SUCCESS;
}

/* ── Async callbacks (same pattern as Cinema) ────────────────────────────── */

void palette_callback(msd_error_t error, struct msd_transfer *xfer) {
    /* Advance palette LBA by one full frame (32 sectors) */
    xfer->lba += FRAME_SECTORS;
    (void)error;
}

void image_callback(msd_error_t error, struct msd_transfer *xfer) {
    /* Advance image LBA by one full frame (32 sectors) */
    xfer->lba += FRAME_SECTORS;
    g_render_ready = true;
    (void)error;
}

/* ── Read header + index from raw LBA ────────────────────────────────────── */

static bool load_header_index(void) {
    /* Read LBA 0 → header */
    if (!msd_Read(&g_global.msd, 0, 1, &g_header)) return false;

    /* Validate magic */
    if (memcmp(g_header.magic, "CPVF", 4) != 0) return false;
    if (g_header.version != 1)                   return false;
    if (g_header.num_videos == 0)                return false;

    g_num_videos = g_header.num_videos;
    if (g_num_videos > MAX_VIDEOS) g_num_videos = MAX_VIDEOS;

    /* Read LBA 1–2 → index entries */
    static uint8_t idx_buf[512 * 2];
    if (!msd_Read(&g_global.msd, 1, 2, idx_buf)) return false;

    memcpy(g_index, idx_buf, g_num_videos * sizeof(cpv_entry_t));
    return true;
}

/* ── Menu ────────────────────────────────────────────────────────────────── */

/* Returns selected video index, or -1 to quit. */
static int8_t run_menu(void) {
    uint8_t sel = 0;

    /* Debounce: wait for all keys up before showing menu */
    while (os_GetCSC());

    while (true) {
        gfx_FillScreen(0x00);

        /* Title bar */
        gfx_SetColor(0x03);
        gfx_FillRectangle(0, 0, 320, 16);
        gfx_SetTextFGColor(0xFF);
        gfx_SetTextBGColor(0x03);
        gfx_SetTextXY(4, 4);
        gfx_PrintString("CalcVidPlayer");

        gfx_SetTextFGColor(0xB5);
        gfx_SetTextBGColor(0x00);
        gfx_SetTextXY(4, 20);
        gfx_PrintString("[ENTER] play  [2ND] back to menu  [CLEAR] quit");

        /* Video list */
        for (uint8_t i = 0; i < g_num_videos; i++) {
            uint8_t y = 38 + i * 16;
            if (i == sel) {
                gfx_SetColor(0x07);
                gfx_FillRectangle(0, y - 1, 320, 14);
                gfx_SetTextFGColor(0xFF);
                gfx_SetTextBGColor(0x07);
            } else {
                gfx_SetTextFGColor(0xE0);
                gfx_SetTextBGColor(0x00);
            }
            gfx_SetTextXY(8, y);
            gfx_PrintString(g_index[i].title);

            /* fps label */
            char fps_str[12];
            uint16_t fps = g_index[i].fps_num / g_index[i].fps_den;
            sprintf(fps_str, "%u fps", (unsigned)fps);
            gfx_SetTextXY(260, y);
            gfx_PrintString(fps_str);
        }

        gfx_SetTextFGColor(0x49);
        gfx_SetTextBGColor(0x00);
        gfx_SetTextXY(4, 228);
        gfx_PrintString("UP/DOWN to navigate");

        gfx_SwapDraw();

        /* Wait for keypress */
        uint8_t key;
        while (!(key = os_GetCSC()));

        if (key == sk_Up   && sel > 0)               sel--;
        else if (key == sk_Down && sel < g_num_videos - 1) sel++;
        else if (key == sk_Enter)                     return (int8_t)sel;
        else if (key == sk_Clear)                     return -1;
    }
}

/* ── Playback ────────────────────────────────────────────────────────────── */

/*
 * Returns true  → back to menu
 * Returns false → quit entirely
 */
static bool play_video(uint8_t vid_idx) {
    cpv_entry_t *vid = &g_index[vid_idx];

    /* Absolute LBA of frame 0 of this video */
    uint32_t base_lba = g_header.data_lba_start + vid->lba_offset;

    /* ── Allocate sprite buffers (same as Cinema) ── */
    sprite_buf_1 = gfx_MallocSprite(FRAME_WIDTH, FRAME_HEIGHT);
    sprite_buf_2 = gfx_MallocSprite(FRAME_WIDTH, FRAME_HEIGHT);
    if (!sprite_buf_1 || !sprite_buf_2) {
        free(sprite_buf_1);
        free(sprite_buf_2);
        return true;
    }

    /* ── Setup transfers (same structure as Cinema) ── */
    g_xfer_palette.msd      = &g_global.msd;
    g_xfer_palette.lba      = base_lba;
    g_xfer_palette.count    = 1;   /* palette = 1 sector (512 bytes, first 768 padded) */
    g_xfer_palette.callback = palette_callback;

    g_xfer_image.msd      = &g_global.msd;
    g_xfer_image.lba      = base_lba + 1;
    g_xfer_image.count    = 30;   /* pixel data = 30 sectors */
    g_xfer_image.callback = image_callback;
    g_xfer_image.userptr  = NULL;

    gfx_SetDrawBuffer();
    gfx_ZeroScreen();
    gfx_SwapDraw();
    gfx_SetDrawBuffer();
    gfx_ZeroScreen();

    /* ── Prime the first frame ── */
    g_render_ready = false;
    g_xfer_palette.buffer = pal_buf_1;
    g_xfer_image.buffer   = &sprite_buf_1->data;

    msd_ReadAsync(&g_xfer_palette);
    msd_ReadAsync(&g_xfer_image);
    while (!g_render_ready) usb_HandleEvents();
    g_render_ready = false;

    /* ── Main playback loop (identical double-buffer pattern to Cinema) ── */
    uint32_t frame = 0;
    bool back_to_menu = true;

    while (frame < vid->num_frames) {
        uint8_t key = os_GetCSC();
        if (key == sk_Clear) { back_to_menu = false; break; }
        if (key == sk_2nd || key == sk_Mode) { back_to_menu = true; break; }
        if (key == sk_0) {
            /* Restart: reset LBAs to base and re-prime */
            g_xfer_palette.lba = base_lba;
            g_xfer_image.lba   = base_lba + 1;
            frame = 0;
        }

        /* Queue next frame into buffer 2 while we display buffer 1 */
        gfx_SetDrawBuffer();
        g_xfer_palette.buffer = pal_buf_2;
        g_xfer_image.buffer   = &sprite_buf_2->data;

        msd_ReadAsync(&g_xfer_palette);
        msd_ReadAsync(&g_xfer_image);

        /* Display frame 1 while USB reads frame 2 */
        gfx_ScaledSprite_NoClip(sprite_buf_1, 0, 24, 2, 2);
        gfx_SwapDraw();
        gfx_Wait();
        gfx_SetPalette(pal_buf_1, sizeof(pal_buf_1), 0);

        while (!g_render_ready) usb_HandleEvents();
        g_render_ready = false;
        frame++;

        if (frame >= vid->num_frames) break;
        key = os_GetCSC();
        if (key == sk_Clear) { back_to_menu = false; break; }
        if (key == sk_2nd || key == sk_Mode) { back_to_menu = true; break; }
        if (key == sk_0) {
            g_xfer_palette.lba = base_lba;
            g_xfer_image.lba   = base_lba + 1;
            frame = 0;
        }

        /* Queue next frame into buffer 1 while we display buffer 2 */
        gfx_SetDrawBuffer();
        g_xfer_palette.buffer = pal_buf_1;
        g_xfer_image.buffer   = &sprite_buf_1->data;

        msd_ReadAsync(&g_xfer_palette);
        msd_ReadAsync(&g_xfer_image);

        /* Display frame 2 while USB reads frame 1 */
        gfx_ScaledSprite_NoClip(sprite_buf_2, 0, 24, 2, 2);
        gfx_SwapDraw();
        gfx_Wait();
        gfx_SetPalette(pal_buf_2, sizeof(pal_buf_2), 0);

        while (!g_render_ready) usb_HandleEvents();
        g_render_ready = false;
        frame++;
    }

    free(sprite_buf_1);
    free(sprite_buf_2);
    return back_to_menu;
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

int main(void) {
    usb_error_t usberr;
    msd_error_t msderr;

    memset(&g_global, 0, sizeof(g_global));
    os_SetCursorPos(1, 0);

    /* USB init loop */
    do {
        g_global.usb = NULL;
        usberr = usb_Init(handle_usb_event, &g_global, NULL, USB_DEFAULT_INIT_FLAGS);
        if (usberr != USB_SUCCESS) { putstr("usb init error"); goto usb_err; }

        while (usberr == USB_SUCCESS) {
            if (g_global.usb) break;
            if (os_GetCSC())  goto usb_err;
            usberr = usb_WaitForInterrupt();
        }
    } while (usberr == USB_USER_ERROR);

    if (usberr != USB_SUCCESS) { putstr("usb enable error"); goto usb_err; }

    msderr = msd_Open(&g_global.msd, g_global.usb);
    if (msderr != MSD_SUCCESS) { putstr("msd open error"); goto msd_err; }

    /* Load header and index */
    if (!load_header_index()) {
        putstr("No valid CalcVidPlayer data found.");
        putstr("Did you dd the .cpv file to the drive?");
        while (!os_GetCSC());
        goto msd_err;
    }

    /* Graphics */
    gfx_Begin();
    gfx_SetDrawBuffer();

    /* Main menu loop */
    while (true) {
        int8_t choice = run_menu();
        if (choice < 0) break;
        bool back = play_video((uint8_t)choice);
        if (!back) break;
    }

    gfx_End();

msd_err:
    msd_Close(&g_global.msd);
usb_err:
    usb_Cleanup();
    while (!os_GetCSC());
    return 0;
}
