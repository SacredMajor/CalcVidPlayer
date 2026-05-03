/* CalcVidPlayer - TI-84 Plus CE USB video player
 * Combines Cinema's rendering pipeline with multi-title menu,
 * SRT caption support, chapter/start-frame support, and resume.
 *
 * Binary format: CPVF v1 (see encode_cpv.py)
 * Build: CE Toolchain (CEdev), libs: msddrvce keypadc graphx fileioc
 */

typedef struct global global_t;
#define usb_callback_data_t global_t

#include <fileioc.h>
#include <graphx.h>
#include <keypadc.h>
#include <msddrvce.h>
#include <tice.h>
#include <usbdrvce.h>

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* ── Constants ───────────────────────────────────────────────────── */
#define BLOCK_SIZE       512
#define PAL_SECTORS      1          /* 256 × uint16_t = 512 bytes      */
#define PIX_SECTORS      30         /* 160×96 = 15 360 bytes           */
#define FRAME_SECTORS    31         /* palette + pixels per frame       */
#define FRAME_W          160
#define FRAME_H          96
#define SCREEN_W         320
#define SCREEN_H         240
#define SPRITE_Y         24         /* top margin so 192-px sprite centres */

#define MAX_TITLES       32
#define MAX_CAP_TRACKS   8
#define MAX_CAPTIONS     256        /* per track                        */
#define CAP_TEXT_LEN     80

#define APPVAR_NAME      "CPVRESME"

/* ── CPVF binary structures (little-endian, packed) ─────────────── */
typedef struct __attribute__((packed)) {
    char     magic[4];         /* "CPVF"                              */
    uint16_t version;          /* 1                                   */
    uint8_t  num_titles;
    uint8_t  flags;
    uint32_t index_offset;     /* byte offset of first title entry    */
    uint32_t data_lba_start;   /* first sector of video data          */
    uint8_t  reserved[16];
} cpvf_header_t;               /* 32 bytes                            */

typedef struct __attribute__((packed)) {
    char     title[32];
    uint32_t lba_offset;       /* sectors from data_lba_start to frame0 */
    uint32_t start_frame;
    uint32_t num_frames;
    uint16_t fps_num;
    uint16_t fps_den;
    uint16_t width;
    uint16_t height;
    uint32_t lba_count;
    uint8_t  num_caption_tracks;
    uint8_t  reserved[11];
} cpvf_title_entry_t;          /* 68 bytes (32+4+4+4+2+2+2+2+4+1+11)   */

typedef struct __attribute__((packed)) {
    char     language[8];
    uint32_t data_offset;      /* byte offset into caption data region */
    uint32_t data_size;
} cpvf_caption_track_t;        /* 16 bytes                            */

typedef struct __attribute__((packed)) {
    uint32_t start_frame;
    uint32_t end_frame;
    char     text[CAP_TEXT_LEN];
} cpvf_caption_entry_t;        /* 88 bytes                            */

/* ── Runtime title info ──────────────────────────────────────────── */
typedef struct {
    char     title[32];
    uint32_t lba_offset;       /* absolute LBA of this title's frame 0 */
    uint32_t start_frame;
    uint32_t num_frames;
    uint16_t fps_num;
    uint16_t fps_den;
    uint8_t  num_caption_tracks;
    /* caption track info */
    char     cap_lang[MAX_CAP_TRACKS][8];
    /* caption entries loaded into RAM */
    cpvf_caption_entry_t *cap_entries[MAX_CAP_TRACKS];
    uint32_t cap_count[MAX_CAP_TRACKS];
} title_t;

/* ── USB / MSD global ────────────────────────────────────────────── */
struct global {
    usb_device_t usb;
    msd_t        msd;
};
enum { USB_RETRY_INIT = USB_USER_ERROR };

/* ── Transfer state shared with callbacks ────────────────────────── */
static volatile bool frame_ready = false;

/* ── USB event handler ───────────────────────────────────────────── */
static usb_error_t handle_usb_event(usb_event_t event, void *event_data,
                                    usb_callback_data_t *global)
{
    switch (event) {
        case USB_DEVICE_DISCONNECTED_EVENT:
            if (global->usb) msd_Close(&global->msd);
            global->usb = NULL;
            break;
        case USB_DEVICE_CONNECTED_EVENT:
            return usb_ResetDevice(event_data);
        case USB_DEVICE_ENABLED_EVENT:
            global->usb = event_data;
            break;
        case USB_DEVICE_DISABLED_EVENT:
            return USB_RETRY_INIT;
        default:
            break;
    }
    return USB_SUCCESS;
}

/* ── Async callbacks (Cinema pattern) ───────────────────────────── */
static void pal_cb(msd_error_t error, struct msd_transfer *xfer)
{
    (void)error;
    xfer->lba += FRAME_SECTORS;
}

static void pix_cb(msd_error_t error, struct msd_transfer *xfer)
{
    (void)error;
    xfer->lba += FRAME_SECTORS;
    frame_ready = true;
}

/* ── AppVar resume helpers ───────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t  vid_idx;
    uint32_t frame_idx;
} resume_rec_t;

static bool resume_load(uint8_t *vid_idx_out, uint32_t *frame_idx_out)
{
    uint8_t var = ti_Open(APPVAR_NAME, "r");
    if (!var) return false;
    resume_rec_t rec;
    bool ok = (ti_Read(&rec, sizeof(rec), 1, var) == 1);
    ti_Close(var);
    if (ok) { *vid_idx_out = rec.vid_idx; *frame_idx_out = rec.frame_idx; }
    return ok;
}

static void resume_save(uint8_t vid_idx, uint32_t frame_idx)
{
    uint8_t var = ti_Open(APPVAR_NAME, "w");
    if (!var) return;
    ti_SetGCBehavior(NULL, NULL);
    ti_SetArchiveStatus(0, var);
    resume_rec_t rec = { vid_idx, frame_idx };
    ti_Write(&rec, sizeof(rec), 1, var);
    ti_SetArchiveStatus(1, var);
    ti_Close(var);
}

static void resume_clear(void)
{
    ti_Delete(APPVAR_NAME);
}

/* ── Simple text helpers (OS home screen) ────────────────────────── */
static void put_str(const char *s) { os_PutStrFull(s); os_NewLine(); }

/* ── Graphics helpers ────────────────────────────────────────────── */
#define COLOR_TITLEBAR   0x13   /* dark blue palette index            */
#define COLOR_HIGHLIGHT  0xE0   /* orange-ish highlight               */
#define COLOR_WHITE      0xFF
#define COLOR_BLACK      0x00
#define COLOR_DARKGRAY   0x49

static void gfx_draw_filled_rect(int x, int y, int w, int h, uint8_t col)
{
    gfx_SetColor(col);
    gfx_FillRectangle(x, y, w, h);
}

/* Draw the title bar at top of screen */
static void draw_title_bar(void)
{
    gfx_draw_filled_rect(0, 0, SCREEN_W, 16, COLOR_TITLEBAR);
    gfx_SetColor(COLOR_WHITE);
    gfx_PrintStringXY("CalcVidPlayer", 4, 4);
}

/* Draw a single menu row */
static void draw_menu_row(int row, const char *text, bool selected)
{
    int y = 20 + row * 14;
    if (selected) {
        gfx_draw_filled_rect(0, y - 1, SCREEN_W, 13, COLOR_HIGHLIGHT);
        gfx_SetTextFGColor(COLOR_BLACK);
    } else {
        gfx_SetTextFGColor(COLOR_WHITE);
    }
    gfx_PrintStringXY(text, 4, y);
    gfx_SetTextFGColor(COLOR_WHITE);
}

/* ── Caption rendering ───────────────────────────────────────────── */
static void render_caption(title_t *title, int8_t track_idx, uint32_t frame)
{
    /* Clear caption bar */
    gfx_draw_filled_rect(0, 218, SCREEN_W, 22, COLOR_BLACK);

    if (track_idx < 0 || track_idx >= title->num_caption_tracks) return;
    if (!title->cap_entries[track_idx]) return;

    uint32_t count = title->cap_count[track_idx];
    cpvf_caption_entry_t *entries = title->cap_entries[track_idx];

    /* Linear scan — small arrays on embedded; acceptable */
    for (uint32_t i = 0; i < count; i++) {
        if (frame >= entries[i].start_frame && frame < entries[i].end_frame) {
            /* Semi-transparent black bar already drawn above */
            gfx_SetTextFGColor(COLOR_WHITE);
            /* Centre text approximately */
            int len = (int)strlen(entries[i].text);
            int px = (SCREEN_W - len * 8) / 2;
            if (px < 2) px = 2;
            gfx_PrintStringXY(entries[i].text, px, 221);
            break;
        }
    }
}

/* ── Sector-aligned synchronous read helpers ─────────────────────── */
/* Read `count` sectors starting at `lba` into `buf`.
   buf must be sector-aligned (512-byte aligned). */
static msd_error_t sync_read(msd_t *msd, uint32_t lba, uint24_t count, void *buf)
{
    return msd_Read(msd, lba, count, buf);
}

/* ── Load caption data for a title from USB ──────────────────────── */
/* Caption data sits after the title/track index headers.
   We need the absolute byte offset. Because we read everything raw
   we work sector-by-sector.

   Layout:
     Sector 0              : global header (32 bytes)
     Sectors 0+            : title index entries (64 bytes each)
     After index entries   : caption track descriptors (16 bytes per track,
                             per title, in order)
     After all descriptors : caption data (88 bytes per entry)
     Padded to sector boundary, then video data starts at data_lba_start.

   The encoder writes data_offset as a byte offset from the start of
   the caption data region, which itself starts right after all track
   descriptors.  We derive the track descriptor region start and
   caption data region start from the header.
*/
static void load_captions(msd_t *msd, title_t *titles, uint8_t num_titles,
                           uint32_t cap_data_lba, uint32_t cap_desc_sector,
                           cpvf_caption_track_t all_tracks[][MAX_CAP_TRACKS])
{
    /* Scratch sector buffer — 512 bytes, static to avoid stack overflow */
    static uint8_t sector_buf[BLOCK_SIZE];

    for (uint8_t t = 0; t < num_titles; t++) {
        title_t *ti = &titles[t];
        for (uint8_t k = 0; k < ti->num_caption_tracks; k++) {
            cpvf_caption_track_t *trk = &all_tracks[t][k];

            /* Copy language tag */
            memcpy(ti->cap_lang[k], trk->language, 8);
            ti->cap_lang[k][7] = '\0';

            uint32_t size = trk->data_size;
            if (size == 0) { ti->cap_entries[k] = NULL; ti->cap_count[k] = 0; continue; }

            uint32_t num_entries = size / sizeof(cpvf_caption_entry_t);
            cpvf_caption_entry_t *entries = malloc(sizeof(cpvf_caption_entry_t) * num_entries);
            if (!entries) { ti->cap_entries[k] = NULL; ti->cap_count[k] = 0; continue; }

            /* Byte offset of this track's data from start of disk */
            uint32_t byte_off = (cap_data_lba * BLOCK_SIZE) + trk->data_offset;
            uint32_t start_lba = byte_off / BLOCK_SIZE;
            uint32_t offset_in_sector = byte_off % BLOCK_SIZE;

            /* Read sector by sector into entries */
            uint8_t *dst = (uint8_t *)entries;
            uint32_t remaining = size;
            uint32_t cur_lba = start_lba;

            /* Handle first partial sector */
            if (offset_in_sector != 0) {
                sync_read(msd, cur_lba, 1, sector_buf);
                uint32_t chunk = BLOCK_SIZE - offset_in_sector;
                if (chunk > remaining) chunk = remaining;
                memcpy(dst, sector_buf + offset_in_sector, chunk);
                dst += chunk; remaining -= chunk; cur_lba++;
            }

            /* Full sectors */
            while (remaining >= BLOCK_SIZE) {
                sync_read(msd, cur_lba, 1, sector_buf);
                memcpy(dst, sector_buf, BLOCK_SIZE);
                dst += BLOCK_SIZE; remaining -= BLOCK_SIZE; cur_lba++;
            }

            /* Trailing partial sector */
            if (remaining > 0) {
                sync_read(msd, cur_lba, 1, sector_buf);
                memcpy(dst, sector_buf, remaining);
            }

            ti->cap_entries[k] = entries;
            ti->cap_count[k] = num_entries;
        }
    }
}

/* ── Read the CPVF header + title index ──────────────────────────── */
/* Returns false on error.  Fills titles[], num_titles_out,
   data_lba_start_out.  Also fills all_tracks for later caption load. */
static bool read_disk_header(msd_t *msd,
                              title_t titles[], uint8_t *num_titles_out,
                              uint32_t *data_lba_start_out,
                              uint32_t *cap_desc_sector_out,
                              uint32_t *cap_data_lba_out,
                              cpvf_caption_track_t all_tracks[][MAX_CAP_TRACKS])
{
    /* Sector 0 contains global header (32 bytes) followed immediately by
       title index entries (64 bytes each).  Both fit in a handful of sectors. */
    static uint8_t raw[BLOCK_SIZE * 4];

    /* Read first 4 sectors — enough for header + up to 31 title entries */
    if (sync_read(msd, 0, 4, raw) != MSD_SUCCESS) return false;

    cpvf_header_t *hdr = (cpvf_header_t *)raw;
    if (memcmp(hdr->magic, "CPVF", 4) != 0) return false;
    if (hdr->version != 1) return false;

    uint8_t n = hdr->num_titles;
    if (n == 0 || n > MAX_TITLES) return false;

    *num_titles_out   = n;
    *data_lba_start_out = hdr->data_lba_start;

    /* Title entries start at hdr->index_offset (= 32) */
    uint8_t *entry_ptr = raw + hdr->index_offset;

    for (uint8_t i = 0; i < n; i++) {
        cpvf_title_entry_t *e = (cpvf_title_entry_t *)(entry_ptr + i * sizeof(cpvf_title_entry_t));
        title_t *t = &titles[i];

        memcpy(t->title, e->title, 32);
        t->title[31] = '\0';
        t->lba_offset     = hdr->data_lba_start + e->lba_offset;
        t->start_frame    = e->start_frame;
        t->num_frames     = e->num_frames;
        t->fps_num        = e->fps_num;
        t->fps_den        = e->fps_den;
        t->num_caption_tracks = e->num_caption_tracks;
        for (uint8_t k = 0; k < MAX_CAP_TRACKS; k++) {
            t->cap_entries[k] = NULL;
            t->cap_count[k]   = 0;
        }
    }

    /* Caption track descriptors follow immediately after all title entries */
    uint32_t desc_byte_off = hdr->index_offset + (uint32_t)n * sizeof(cpvf_title_entry_t);

    /* We need to read these — may span sector boundary, so read a block */
    uint32_t desc_sector = desc_byte_off / BLOCK_SIZE;
    uint32_t desc_in_sec = desc_byte_off % BLOCK_SIZE;

    /* Read up to 8 more sectors for the descriptor region */
    static uint8_t desc_buf[BLOCK_SIZE * 8];
    /* Compute total descriptor bytes */
    uint32_t total_desc_bytes = 0;
    for (uint8_t i = 0; i < n; i++)
        total_desc_bytes += titles[i].num_caption_tracks * 16;

    uint32_t desc_sectors_needed = (desc_in_sec + total_desc_bytes + BLOCK_SIZE - 1) / BLOCK_SIZE;
    if (desc_sectors_needed > 8) desc_sectors_needed = 8;
    if (desc_sectors_needed > 0) {
        sync_read(msd, desc_sector, (uint24_t)desc_sectors_needed, desc_buf);
    }

    uint8_t *dp = desc_buf + desc_in_sec;
    for (uint8_t i = 0; i < n; i++) {
        for (uint8_t k = 0; k < titles[i].num_caption_tracks && k < MAX_CAP_TRACKS; k++) {
            memcpy(&all_tracks[i][k], dp, 16);
            dp += 16;
        }
    }

    /* Caption data region starts right after descriptors, rounded up to sector */
    uint32_t cap_data_byte = desc_byte_off + total_desc_bytes;
    uint32_t cap_data_lba  = (cap_data_byte + BLOCK_SIZE - 1) / BLOCK_SIZE;
    *cap_data_lba_out    = cap_data_lba;
    *cap_desc_sector_out = desc_sector;

    return true;
}

/* ── Key debounce helpers ────────────────────────────────────────── */
static uint8_t last_key = 0;

static uint8_t get_key_once(void)
{
    kb_Scan();
    /* Map keypad scan to single key code the way os_GetCSC does for common keys */
    uint8_t k = 0;
    if      (kb_IsDown(kb_KeyUp))    k = sk_Up;
    else if (kb_IsDown(kb_KeyDown))  k = sk_Down;
    else if (kb_IsDown(kb_KeyEnter)) k = sk_Enter;
    else if (kb_IsDown(kb_KeyClear)) k = sk_Clear;
    else if (kb_IsDown(kb_Key2nd))   k = sk_2nd;
    else if (kb_IsDown(kb_KeyMode))  k = sk_Mode;
    else if (kb_IsDown(kb_Key0))     k = sk_0;
    else if (kb_AnyKey())            k = 1; /* generic "any" */

    if (k && k != last_key) { last_key = k; return k; }
    if (!k) last_key = 0;
    return 0;
}

/* ── Menu — title selection ──────────────────────────────────────── */
/* Returns selected title index, or 255 on CLEAR (quit). */
static uint8_t menu_select_title(title_t titles[], uint8_t n)
{
    uint8_t sel = 0;
    uint8_t scroll = 0;          /* first visible row index         */
    const uint8_t visible = 14;  /* rows that fit on screen          */

    gfx_Begin();
    gfx_SetDrawBuffer();

    while (true) {
        gfx_ZeroScreen();
        draw_title_bar();

        /* Instruction hint */
        gfx_SetTextFGColor(0xAA);
        gfx_PrintStringXY("UP/DOWN=navigate  ENTER=select  CLEAR=quit", 2, SCREEN_H - 10);
        gfx_SetTextFGColor(COLOR_WHITE);

        for (uint8_t row = 0; row < visible && (scroll + row) < n; row++) {
            uint8_t idx = scroll + row;
            char buf[48];
            /* Show fps as decimal approximation */
            uint16_t fps = (titles[idx].fps_den > 0)
                         ? titles[idx].fps_num / titles[idx].fps_den
                         : titles[idx].fps_num;
            snprintf(buf, sizeof(buf), "%-28s %uFPS", titles[idx].title, (unsigned)fps);
            draw_menu_row(row, buf, idx == sel);
        }

        gfx_SwapDraw();

        /* Input */
        uint8_t k;
        do {
            usb_HandleEvents();
            k = os_GetCSC();
        } while (!k);

        if (k == sk_Up) {
            if (sel > 0) {
                sel--;
                if (sel < scroll) scroll = sel;
            }
        } else if (k == sk_Down) {
            if (sel + 1 < n) {
                sel++;
                if (sel >= scroll + visible) scroll = sel - visible + 1;
            }
        } else if (k == sk_Enter) {
            gfx_End();
            return sel;
        } else if (k == sk_Clear) {
            gfx_End();
            return 255;
        }
    }
}

/* ── Menu — caption track selection ──────────────────────────────── */
/* Returns 0..num_tracks-1, or -1 for "No captions", or -2 for back. */
static int8_t menu_select_caption(title_t *title)
{
    if (title->num_caption_tracks == 0) return -1;

    uint8_t sel = 0;
    uint8_t rows = title->num_caption_tracks + 1; /* +1 for "No captions" */

    gfx_Begin();
    gfx_SetDrawBuffer();

    while (true) {
        gfx_ZeroScreen();
        draw_title_bar();
        gfx_SetTextFGColor(COLOR_WHITE);
        gfx_PrintStringXY("Select caption track:", 4, 20);

        for (uint8_t i = 0; i < title->num_caption_tracks; i++) {
            draw_menu_row(i + 1, title->cap_lang[i], sel == i);
        }
        draw_menu_row(title->num_caption_tracks + 1, "No captions",
                      sel == title->num_caption_tracks);

        gfx_SwapDraw();

        uint8_t k;
        do { usb_HandleEvents(); k = os_GetCSC(); } while (!k);

        if (k == sk_Up && sel > 0) sel--;
        else if (k == sk_Down && sel + 1 < rows) sel++;
        else if (k == sk_Enter) {
            gfx_End();
            return (sel == title->num_caption_tracks) ? -1 : (int8_t)sel;
        } else if (k == sk_2nd || k == sk_Mode) {
            gfx_End();
            return -2; /* back to title menu */
        }
    }
}

/* ── Resume / Restart prompt ─────────────────────────────────────── */
/* Returns true = resume, false = restart */
static bool menu_resume_prompt(void)
{
    gfx_Begin();
    gfx_SetDrawBuffer();
    gfx_ZeroScreen();
    draw_title_bar();
    gfx_SetTextFGColor(COLOR_WHITE);
    gfx_PrintStringXY("Resume playback?", 4, 40);
    gfx_PrintStringXY("ENTER = Resume", 4, 60);
    gfx_PrintStringXY("0     = Restart", 4, 75);
    gfx_SwapDraw();

    while (true) {
        usb_HandleEvents();
        uint8_t k = os_GetCSC();
        if (k == sk_Enter) { gfx_End(); return true; }
        if (k == sk_0)     { gfx_End(); return false; }
    }
}

/* ── Playback ────────────────────────────────────────────────────── */
/* Returns: 0=quit to OS, 1=back to menu */
static int play_title(global_t *global, title_t *title,
                      uint8_t title_idx, int8_t cap_track,
                      uint32_t start_frame)
{
    /* ── Sprite / palette buffers (Cinema pattern) ───────────────── */
    gfx_sprite_t *spr[2];
    spr[0] = gfx_MallocSprite(FRAME_W, FRAME_H);
    spr[1] = gfx_MallocSprite(FRAME_W, FRAME_H);
    if (!spr[0] || !spr[1]) return 1;

    uint16_t pal[2][256];
    uint8_t cur = 0, nxt = 1;

    /* Absolute LBA of the first frame to show */
    uint32_t first_frame_lba = title->lba_offset + start_frame * FRAME_SECTORS;

    /* ── Transfer structs ────────────────────────────────────────── */
    msd_transfer_t xfer_pal, xfer_pix;

    xfer_pal.msd      = &global->msd;
    xfer_pal.lba      = first_frame_lba;
    xfer_pal.count    = PAL_SECTORS;
    xfer_pal.callback = pal_cb;
    xfer_pal.userptr  = NULL;

    xfer_pix.msd      = &global->msd;
    xfer_pix.lba      = first_frame_lba + PAL_SECTORS;
    xfer_pix.count    = PIX_SECTORS;
    xfer_pix.callback = pix_cb;
    xfer_pix.userptr  = &frame_ready;

    /* ── Frame timer setup ───────────────────────────────────────── */
    /* Timer counts down at 32 768 Hz */
    uint32_t fps = (title->fps_den > 0)
                 ? title->fps_num / title->fps_den
                 : title->fps_num;
    if (fps == 0) fps = 10;
    uint32_t frame_ticks = 32768 / fps;

    /* ── Graphics init ───────────────────────────────────────────── */
    gfx_Begin();
    gfx_SetDrawBuffer();
    gfx_ZeroScreen();
    gfx_SwapDraw();
    gfx_SetDrawBuffer();
    gfx_ZeroScreen();

    /* ── Bootstrap: load frame `start_frame` synchronously ───────── */
    xfer_pal.buffer = pal[cur];
    if (msd_Read(&global->msd, xfer_pal.lba, PAL_SECTORS, pal[cur]) != MSD_SUCCESS) goto done;
    xfer_pal.lba += FRAME_SECTORS;

    if (msd_Read(&global->msd, xfer_pix.lba, PIX_SECTORS, spr[cur]->data) != MSD_SUCCESS) goto done;
    xfer_pix.lba += FRAME_SECTORS;

    gfx_SetPalette(pal[cur], sizeof(pal[0]), 0);

    /* ── Enable timer ────────────────────────────────────────────── */
    timer_Set(1, (unsigned int)frame_ticks);
    timer_Enable(1, TIMER_32K, TIMER_0INT, TIMER_DOWN);

    uint32_t frame_num = start_frame;
    bool paused = false;
    int  result = 1;   /* default: back to menu */

    /* ── Main playback loop ───────────────────────────────────────── */
    while (true) {
        /* Check end of video */
        if (frame_num >= title->num_frames) {
            resume_clear();
            result = 1;
            break;
        }

        /* ── Key handling ────────────────────────────────────────── */
        uint8_t key = os_GetCSC();
        if (key) {
            if (key == sk_Clear) {
                resume_save(title_idx, frame_num);
                result = 0;
                break;
            } else if (key == sk_2nd || key == sk_Mode) {
                resume_save(title_idx, frame_num);
                result = 1;
                break;
            } else if (key == sk_0) {
                /* Restart from title's start_frame */
                frame_num = title->start_frame;
                uint32_t restart_lba = title->lba_offset + frame_num * FRAME_SECTORS;
                xfer_pal.lba = restart_lba;
                xfer_pix.lba = restart_lba + PAL_SECTORS;
                /* Re-bootstrap */
                msd_Read(&global->msd, xfer_pal.lba, PAL_SECTORS, pal[cur]);
                xfer_pal.lba += FRAME_SECTORS;
                msd_Read(&global->msd, xfer_pix.lba, PIX_SECTORS, spr[cur]->data);
                xfer_pix.lba += FRAME_SECTORS;
                gfx_SetPalette(pal[cur], sizeof(pal[0]), 0);
                continue;
            } else if (key == sk_Enter) {
                paused = !paused;
            } else if (paused) {
                paused = false;
            }
        }

        if (paused) {
            /* Drain USB events, don't advance */
            usb_HandleEvents();
            continue;
        }

        /* ── Queue nxt async reads ───────────────────────────────── */
        xfer_pal.buffer = pal[nxt];
        xfer_pix.buffer = spr[nxt]->data;
        frame_ready = false;

        if (msd_ReadAsync(&xfer_pal) != MSD_SUCCESS) { result = 1; break; }
        if (msd_ReadAsync(&xfer_pix) != MSD_SUCCESS) { result = 1; break; }

        /* ── Display cur frame ───────────────────────────────────── */
        gfx_SetDrawBuffer();
        gfx_ScaledSprite_NoClip(spr[cur], 0, SPRITE_Y, 2, 2);

        /* Caption overlay */
        render_caption(title, cap_track, frame_num);

        gfx_SwapDraw();
        gfx_Wait();
        gfx_SetPalette(pal[cur], sizeof(pal[0]), 0);

        /* ── Wait for frame timer ────────────────────────────────── */
        while (!timer_ChkInterrupt(1, TIMER_RELOADED)) {
            usb_HandleEvents();
        }

        /* ── Wait for async to finish ────────────────────────────── */
        while (!frame_ready) {
            usb_HandleEvents();
        }

        /* ── Swap buffers ────────────────────────────────────────── */
        cur ^= 1; nxt ^= 1;
        frame_num++;
    }

done:
    timer_Disable(1);
    gfx_End();
    free(spr[0]);
    free(spr[1]);
    return result;
}

/* ── main ────────────────────────────────────────────────────────── */
int main(void)
{
    static global_t global;
    memset(&global, 0, sizeof(global));

    os_ClrHome();
    os_SetCursorPos(1, 0);
    put_str("CalcVidPlayer");
    put_str("Waiting for USB drive...");

    /* ── USB init (Cinema pattern) ───────────────────────────────── */
    usb_error_t usberr;
    do {
        global.usb = NULL;
        usberr = usb_Init(handle_usb_event, &global, NULL, USB_DEFAULT_INIT_FLAGS);
        if (usberr != USB_SUCCESS) {
            put_str("USB init error");
            goto usb_error;
        }
        while (usberr == USB_SUCCESS) {
            if (global.usb) break;
            if (os_GetCSC()) { put_str("Cancelled."); goto usb_error; }
            usberr = usb_WaitForInterrupt();
        }
    } while (usberr == USB_RETRY_INIT);

    if (usberr != USB_SUCCESS) { put_str("USB enable error"); goto usb_error; }

    /* ── MSD open ────────────────────────────────────────────────── */
    if (msd_Open(&global.msd, global.usb) != MSD_SUCCESS) {
        put_str("Failed to open MSD");
        goto usb_error;
    }
    put_str("Drive ready, reading header...");

    /* ── Read CPVF header ────────────────────────────────────────── */
    static title_t titles[MAX_TITLES];
    static cpvf_caption_track_t all_tracks[MAX_TITLES][MAX_CAP_TRACKS];
    uint8_t  num_titles = 0;
    uint32_t data_lba_start = 0;
    uint32_t cap_desc_sector = 0;
    uint32_t cap_data_lba = 0;

    memset(titles, 0, sizeof(titles));
    memset(all_tracks, 0, sizeof(all_tracks));

    if (!read_disk_header(&global.msd, titles, &num_titles, &data_lba_start,
                          &cap_desc_sector, &cap_data_lba, all_tracks)) {
        put_str("Invalid or missing CPVF header!");
        put_str("(Is the drive written with encode_cpv.py?)");
        while (!os_GetCSC()) usb_HandleEvents();
        goto msd_error;
    }

    /* ── Load captions ───────────────────────────────────────────── */
    load_captions(&global.msd, titles, num_titles, cap_data_lba,
                  cap_desc_sector, all_tracks);

    /* ── Check for saved resume position ─────────────────────────── */
    uint8_t  resume_vid = 0;
    uint32_t resume_frame = 0;
    bool     has_resume = resume_load(&resume_vid, &resume_frame);

    /* ── Main app loop ───────────────────────────────────────────── */
    while (true) {
        uint8_t sel = menu_select_title(titles, num_titles);
        if (sel == 255) break; /* CLEAR = quit */

        /* Caption track selection */
        int8_t cap_track;
        if (titles[sel].num_caption_tracks > 0) {
            cap_track = menu_select_caption(&titles[sel]);
            if (cap_track == -2) continue; /* back to title list */
        } else {
            cap_track = -1;
        }

        /* Resume or start-frame */
        uint32_t play_from = titles[sel].start_frame;
        if (has_resume && resume_vid == sel && resume_frame > titles[sel].start_frame) {
            if (menu_resume_prompt())
                play_from = resume_frame;
            has_resume = false; /* clear offer after one use */
        }

        int res = play_title(&global, &titles[sel], sel, cap_track, play_from);
        if (res == 0) break; /* quit to OS */
        /* else loop back to menu */
    }

    /* ── Cleanup ─────────────────────────────────────────────────── */
    for (uint8_t i = 0; i < num_titles; i++)
        for (uint8_t k = 0; k < MAX_CAP_TRACKS; k++)
            free(titles[i].cap_entries[k]);

    msd_Close(&global.msd);
    usb_Cleanup();
    return 0;

msd_error:
    msd_Close(&global.msd);
usb_error:
    usb_Cleanup();
    while (!os_GetCSC());
    return 0;
}
