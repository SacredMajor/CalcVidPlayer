/**
 * CalcVidPlayer – Multi-video USB player for the TI-84 Plus CE
 * =============================================================
 * Uses Cinema's exact rendering pipeline (FBin format, async double-buffer)
 * with a multi-title menu so you can pick which video to play.
 *
 * Build: CEdev toolchain
 *   NAME = CALCVID
 *   LIBS = msddrvce fatdrvce keypadc graphx
 */

typedef struct global_t global_t;
#define usb_callback_data_t global_t

#include <fileioc.h>
#include <graphx.h>
#include <keypadc.h>
#include <msddrvce.h>
#include <fatdrvce.h>
#include <tice.h>
#include <usbdrvce.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

/* ── Format (must match encoder) ────────────────────────────────────────────── */

#define CPV_MAGIC        "CPVF"
#define CPV_VERSION      1
#define SECTOR_SIZE      512
#define FRAME_WIDTH      160
#define FRAME_HEIGHT     96

/* FBin / Cinema frame layout on disk:
 *   Sector  0   : 256 × uint16_t RGB1555 palette = 512 bytes  (1 sector)
 *   Sectors 1-30: 160×96 pixel index data = 15 360 bytes      (30 sectors)
 *   Total: 31 sectors per frame                                            */
#define PAL_SECTORS      1
#define PIX_SECTORS      30
#define FRAME_SECTORS    31

#define MAX_VIDEOS       16
#define TITLE_LEN        32
#define APPVAR_RESUME    "CPVRESME"

/* ── On-disk structs ─────────────────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    char     magic[4];
    uint16_t version;
    uint8_t  num_videos;
    uint8_t  flags;
    uint32_t index_offset;      /* byte offset of first index entry = 32 */
    uint32_t data_lba_start;    /* sector where video 0 frame 0 lives    */
    uint8_t  reserved[16];
} cpv_header_t;                 /* 32 bytes */

typedef struct __attribute__((packed)) {
    char     title[TITLE_LEN];
    uint32_t lba_offset;        /* sectors from data_lba_start           */
    uint32_t num_frames;
    uint16_t fps_numerator;
    uint16_t fps_denominator;
    uint16_t width;
    uint16_t height;
    uint32_t lba_count;
    uint8_t  reserved[12];
} cpv_index_entry_t;            /* 64 bytes */

typedef struct { uint8_t vid_idx; uint32_t frame_idx; } resume_t;

/* ── USB state ───────────────────────────────────────────────────────────────── */

struct global_t { usb_device_t usb; msd_t msd; };
static global_t g;

/* ── Double buffers ──────────────────────────────────────────────────────────── */

static gfx_sprite_t *spr[2];   /* pixel buffers – allocated at runtime    */
static uint16_t      pal[2][256]; /* palette buffers – RGB1555             */

/* ── Scratch / state ─────────────────────────────────────────────────────────── */

static uint8_t          meta_buf[SECTOR_SIZE]; /* header/index reads only  */
static msd_transfer_t   xfer_pal, xfer_pix;
static volatile bool    frame_ready;

static cpv_header_t      hdr;
static cpv_index_entry_t idx[MAX_VIDEOS];
static uint8_t           num_vids;

static fat_t      fat;
static fat_file_t cpv_file;
static bool       fat_open = false;

/* ── USB callbacks ───────────────────────────────────────────────────────────── */

static usb_error_t usb_cb(usb_event_t ev, void *evdata, usb_callback_data_t *cb) {
    switch (ev) {
        case USB_DEVICE_DISCONNECTED_EVENT:
            if (cb->usb) msd_Close(&cb->msd);
            cb->usb = NULL; break;
        case USB_DEVICE_CONNECTED_EVENT:
            return usb_ResetDevice(evdata);
        case USB_DEVICE_ENABLED_EVENT:
            cb->usb = evdata; break;
        case USB_DEVICE_DISABLED_EVENT:
            return USB_USER_ERROR;
        default: break;
    }
    return USB_SUCCESS;
}

static void pal_cb(msd_error_t e, msd_transfer_t *x) { (void)e; x->lba += FRAME_SECTORS; }
static void pix_cb(msd_error_t e, msd_transfer_t *x) { (void)e; x->lba += FRAME_SECTORS; frame_ready = true; }

/* ── USB init ────────────────────────────────────────────────────────────────── */

static bool usb_init(void) {
    usb_error_t err;
    memset(&g, 0, sizeof(g));
    do {
        g.usb = NULL;
        err = usb_Init(usb_cb, &g, NULL, USB_DEFAULT_INIT_FLAGS);
        if (err != USB_SUCCESS) return false;
        while (err == USB_SUCCESS) { if (g.usb) break; err = usb_WaitForInterrupt(); }
    } while (err == (usb_error_t)USB_USER_ERROR);
    if (err != USB_SUCCESS) return false;
    return msd_Open(&g.msd, g.usb) == MSD_SUCCESS;
}

static void usb_cleanup(void) {
    if (fat_open) { fat_CloseFile(&cpv_file); fat_open = false; }
    fat_Deinit(&fat);
    msd_Close(&g.msd);
    usb_Cleanup();
}

/* ── Read header + index via FAT (startup only) ──────────────────────────────── */

static bool fat_read_sec(uint32_t sec, uint24_t n, void *buf) {
    if (fat_SetFilePos(&cpv_file, sec * SECTOR_SIZE) != FAT_SUCCESS) return false;
    return fat_ReadFile(&cpv_file, n * SECTOR_SIZE, buf) == n * SECTOR_SIZE;
}

static bool cpv_open(void) {
    if (fat_Init(&fat, &g.msd, meta_buf, 0) != FAT_SUCCESS) return false;
    if (fat_OpenFile(&fat, "VIDEO.CPV", FAT_READ, &cpv_file) != FAT_SUCCESS) return false;
    fat_open = true;

    if (!fat_read_sec(0, 1, meta_buf)) return false;
    cpv_header_t *h = (cpv_header_t *)meta_buf;
    if (memcmp(h->magic, CPV_MAGIC, 4) != 0) return false;
    if (h->version != CPV_VERSION || h->num_videos == 0) return false;
    memcpy(&hdr, h, sizeof(hdr));
    num_vids = h->num_videos;
    if (num_vids > MAX_VIDEOS) num_vids = MAX_VIDEOS;

    /* Copy index entries — start at byte 32 inside sector 0 */
    uint32_t need = (uint32_t)num_vids * sizeof(cpv_index_entry_t);
    uint8_t *dst  = (uint8_t *)idx;
    uint24_t avail = SECTOR_SIZE - (uint24_t)hdr.index_offset;
    uint24_t c1    = avail < (uint24_t)need ? avail : (uint24_t)need;
    memcpy(dst, meta_buf + hdr.index_offset, c1);
    dst += c1; need -= c1;
    for (uint32_t s = 1; need > 0; s++) {
        if (!fat_read_sec(s, 1, meta_buf)) return false;
        uint24_t chunk = need < SECTOR_SIZE ? (uint24_t)need : SECTOR_SIZE;
        memcpy(dst, meta_buf, chunk);
        dst += chunk; need -= chunk;
    }

    fat_CloseFile(&cpv_file); fat_open = false;
    return true;
}

/* ── Resume helpers ──────────────────────────────────────────────────────────── */

static void resume_save(uint8_t vi, uint32_t fi) {
    uint8_t v = ti_Open(APPVAR_RESUME, "w");
    if (!v) return;
    ti_SetGCBehavior(NULL, NULL);
    ti_SetArchiveStatus(0, v);
    resume_t r = {vi, fi};
    ti_Write(&r, sizeof(r), 1, v);
    ti_SetArchiveStatus(1, v);
    ti_Close(v);
}

static bool resume_load(uint8_t vi, uint32_t *fi) {
    uint8_t v = ti_Open(APPVAR_RESUME, "r+");
    if (!v) return false;
    resume_t r;
    bool ok = (ti_Read(&r, sizeof(r), 1, v) == 1) && r.vid_idx == vi;
    ti_Close(v);
    if (ok) *fi = r.frame_idx;
    return ok;
}

/* ── Menu ────────────────────────────────────────────────────────────────────── */

static int8_t run_menu(void) {
    uint8_t sel = 0;
    do { kb_Scan(); } while (kb_AnyKey());

    while (true) {
        gfx_FillScreen(0x00);

        gfx_SetColor(0x12);
        gfx_FillRectangle(0, 0, LCD_WIDTH, 18);
        gfx_SetTextFGColor(0xFF); gfx_SetTextBGColor(0x12);
        gfx_SetTextXY(4, 4); gfx_PrintString("CalcVidPlayer");

        gfx_SetTextFGColor(0xC0); gfx_SetTextBGColor(0x00);
        gfx_SetTextXY(4, 22); gfx_PrintString("[ENTER] play  [CLEAR] quit");

        for (uint8_t i = 0; i < num_vids; i++) {
            uint8_t y = 40 + i * 16;
            if (i == sel) {
                gfx_SetColor(0x1F);
                gfx_FillRectangle(0, y - 2, LCD_WIDTH, 14);
                gfx_SetTextFGColor(0xFF); gfx_SetTextBGColor(0x1F);
            } else {
                gfx_SetTextFGColor(0xE0); gfx_SetTextBGColor(0x00);
            }
            gfx_SetTextXY(8, y);
            gfx_PrintString(idx[i].title);

            char info[12];
            sprintf(info, "%u fps", idx[i].fps_numerator / idx[i].fps_denominator);
            gfx_SetTextXY(LCD_WIDTH - 56, y);
            gfx_PrintString(info);
        }

        gfx_SetTextFGColor(0x80); gfx_SetTextBGColor(0x00);
        gfx_SetTextXY(4, LCD_HEIGHT - 12);
        gfx_PrintString("UP / DOWN to navigate");
        gfx_SwapDraw();

        do { kb_Scan(); } while (kb_AnyKey());
        do { kb_Scan(); } while (!kb_AnyKey());

        if      (kb_IsDown(kb_KeyUp)    && sel > 0)              sel--;
        else if (kb_IsDown(kb_KeyDown)  && sel < num_vids - 1)   sel++;
        else if (kb_IsDown(kb_KeyEnter))                          return (int8_t)sel;
        else if (kb_IsDown(kb_KeyClear))                          return -1;
    }
}

/* ── Playback (Cinema async double-buffer pipeline) ─────────────────────────── */

static bool play_video(uint8_t vi) {
    cpv_index_entry_t *v = &idx[vi];

    spr[0] = gfx_MallocSprite(FRAME_WIDTH, FRAME_HEIGHT);
    spr[1] = gfx_MallocSprite(FRAME_WIDTH, FRAME_HEIGHT);
    if (!spr[0] || !spr[1]) { free(spr[0]); free(spr[1]); return true; }

    uint32_t frame_ticks = (uint32_t)32768u * v->fps_denominator / v->fps_numerator;
    uint32_t base_lba    = hdr.data_lba_start + v->lba_offset;

    /* Resume dialog */
    uint32_t fi = 0;
    { uint32_t saved = 0;
      if (resume_load(vi, &saved) && saved > 0 && saved < v->num_frames) {
          gfx_FillScreen(0x00);
          gfx_SetTextFGColor(0xFF); gfx_SetTextBGColor(0x00);
          gfx_SetTextXY(4, 100); gfx_PrintString("Resume where you left off?");
          gfx_SetTextXY(4, 116); gfx_PrintString("Any key = YES     0 = Restart");
          gfx_SwapDraw();
          do { kb_Scan(); } while (!kb_AnyKey());
          if (!kb_IsDown(kb_Key0)) fi = saved;
          do { kb_Scan(); } while (kb_AnyKey());
      }
    }

    uint8_t cur = 0, nxt = 1;

    /* Transfer structs — palette reads 1 sector directly into pal[],
     * pixel reads 30 sectors into spr[]->data.
     * Callbacks advance lba by FRAME_SECTORS automatically.             */
    xfer_pal.msd = &g.msd; xfer_pal.count = PAL_SECTORS; xfer_pal.callback = pal_cb;
    xfer_pix.msd = &g.msd; xfer_pix.count = PIX_SECTORS; xfer_pix.callback = pix_cb;

    /* Bootstrap: load frame fi into slot cur synchronously */
    frame_ready = false;
    xfer_pal.lba = base_lba + (uint32_t)fi * FRAME_SECTORS;
    xfer_pix.lba = xfer_pal.lba + PAL_SECTORS;
    xfer_pal.buffer = pal[cur];
    xfer_pix.buffer = spr[cur]->data;
    if (msd_ReadAsync(&xfer_pal) != MSD_SUCCESS) goto end_menu;
    if (msd_ReadAsync(&xfer_pix) != MSD_SUCCESS) goto end_menu;
    while (!frame_ready) usb_HandleEvents();
    frame_ready = false;
    fi++;

    gfx_SetDrawBuffer();

    while (fi <= v->num_frames) {
        bool last = (fi >= v->num_frames);

        /* Queue next frame async */
        if (!last) {
            xfer_pal.buffer = pal[nxt];
            xfer_pix.buffer = spr[nxt]->data;
            if (msd_ReadAsync(&xfer_pal) != MSD_SUCCESS) goto end_menu;
            if (msd_ReadAsync(&xfer_pix) != MSD_SUCCESS) goto end_menu;
        }

        /* Display current frame — 160×96 scaled 2× → 320×192, centred (y=24) */
        gfx_SetPalette(pal[cur], sizeof(pal[cur]), 0);
        gfx_ScaledSprite_NoClip(spr[cur], 0, 24, 2, 2);
        gfx_SwapDraw();
        gfx_Wait();

        /* Frame timer */
        timer_Set(1, frame_ticks);
        timer_Enable(1, TIMER_32K, TIMER_0INT, TIMER_DOWN);

        /* Input */
        kb_Scan();
        if (kb_IsDown(kb_KeyClear)) {
            resume_save(vi, fi); timer_Disable(1); goto end_quit;
        }
        if (kb_IsDown(kb_Key2nd) || kb_IsDown(kb_KeyMode)) {
            resume_save(vi, fi); timer_Disable(1); goto end_menu;
        }
        if (kb_IsDown(kb_Key0)) {
            /* Restart */
            if (!last) { while (!frame_ready) usb_HandleEvents(); frame_ready = false; }
            timer_Disable(1);
            fi = 0; cur = 0; nxt = 1;
            xfer_pal.lba = base_lba; xfer_pix.lba = base_lba + PAL_SECTORS;
            xfer_pal.buffer = pal[cur]; xfer_pix.buffer = spr[cur]->data;
            if (msd_ReadAsync(&xfer_pal) != MSD_SUCCESS) goto end_menu;
            if (msd_ReadAsync(&xfer_pix) != MSD_SUCCESS) goto end_menu;
            while (!frame_ready) usb_HandleEvents();
            frame_ready = false; fi = 1;
            continue;
        }
        if (kb_IsDown(kb_KeyEnter)) {
            /* Pause */
            if (!last) { while (!frame_ready) usb_HandleEvents(); frame_ready = false; }
            timer_Disable(1);
            do { kb_Scan(); } while (kb_IsDown(kb_KeyEnter));
            while (true) {
                kb_Scan();
                if (kb_IsDown(kb_KeyEnter)) { do{kb_Scan();}while(kb_IsDown(kb_KeyEnter)); break; }
                if (kb_IsDown(kb_KeyClear)) { resume_save(vi, fi); goto end_quit; }
                if (kb_IsDown(kb_Key2nd) || kb_IsDown(kb_KeyMode)) { resume_save(vi, fi); goto end_menu; }
            }
            if (!last) {
                uint8_t tmp = cur; cur = nxt; nxt = tmp; fi++;
            }
            continue;
        }

        /* Wait for async read */
        if (!last) {
            while (!frame_ready) usb_HandleEvents();
            frame_ready = false;
        }

        /* Wait for frame period */
        while (!timer_ChkInterrupt(1, TIMER_RELOADED)) usb_HandleEvents();
        timer_Disable(1);

        uint8_t tmp = cur; cur = nxt; nxt = tmp;
        fi++;
    }

    resume_save(vi, 0); /* clear resume on natural finish */
    free(spr[0]); free(spr[1]);
    return true;

end_menu: free(spr[0]); free(spr[1]); return true;
end_quit: free(spr[0]); free(spr[1]); return false;
}

/* ── Entry point ─────────────────────────────────────────────────────────────── */

int main(void) {
    gfx_Begin();
    gfx_SetDrawBuffer();
    gfx_FillScreen(0x00);
    gfx_SetTextFGColor(0xFF); gfx_SetTextBGColor(0x00);
    gfx_SetTextXY(4, 108);
    gfx_PrintString("CalcVidPlayer  -  initialising USB ...");
    gfx_SwapDraw();

    if (!usb_init()) {
        gfx_SetDrawBuffer();
        gfx_SetTextXY(4, 124); gfx_PrintString("USB init failed.");
        gfx_SwapDraw(); while (!os_GetCSC()); goto done;
    }
    if (!cpv_open()) {
        gfx_SetDrawBuffer();
        gfx_SetTextXY(4, 124); gfx_PrintString("VIDEO.CPV not found or invalid.");
        gfx_SwapDraw(); while (!os_GetCSC()); goto done;
    }

    while (true) {
        int8_t choice = run_menu();
        if (choice < 0) break;
        if (!play_video((uint8_t)choice)) break;
    }

done:
    usb_cleanup();
    gfx_End();
    return 0;
}
