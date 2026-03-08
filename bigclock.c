#include <furi.h>
#include <furi_hal_rtc.h>

#include <gui/gui.h>
#include <gui/canvas.h>
#include <gui/view_port.h>

#include <input/input.h>

#include <notification/notification.h>
#include <notification/notification_messages.h>

#include <storage/storage.h>

// ----------------------------------------------------------------------------
// bigclock.c
// ----------------------------------------------------------------------------
//
// Render path:
//   ViewPort draw callback -> segdigit() -> style-specific renderer.
//
// Segment styles:
//   0: classic rectangle 7-segment
//   1: lozenge segment 7-seg
//   2: bitmap font digits (manual dot map)
//
// Persisted settings are loaded at startup and re-saved on user input only.

// ----------------------------------------------------------------------------
// App state
// ----------------------------------------------------------------------------
//
// Minimal Flipper app scaffold:
// - A ViewPort draws the UI and receives input callbacks.
// - A message queue moves input events from the callback into the main loop.
// - A periodic timer triggers redraws (once per second here).
// - NotificationApp is used only to force the backlight to stay on while running.
//
typedef struct {
    FuriMessageQueue* q;      // input events from ViewPort callback -> main loop
    ViewPort* vp;             // fullscreen drawing + input hook
    FuriTimer* timer;         // periodic "tick" that requests a redraw
    NotificationApp* notif;   // backlight control (keep screen on during app)
    bool mode_24h;            // false=12h with AM/PM, true=24h with "24"
    uint8_t segment_style;    // SegmentStyle persisted selection
} App;

// Files persisted under APP_DATA_PATH:
//   mode24.bin      => one byte: 0 (12h), 1 (24h)
//   segment_style.bin=> one byte: 0 (classic), 1 (lozenge), 2 (font)
//
// App state rule:
// - load both files on startup
// - only read RTC each frame (no cached time state)
// - write on explicit user action

// ----------------------------------------------------------------------------
// Persisted settings: 24-hour mode
// ----------------------------------------------------------------------------
//
// We keep one byte of state ("mode_24h") across launches.
// Flipper apps are expected to store small bits of configuration in the app data
// directory on the SD card, addressed via APP_DATA_PATH(...).
//
// We do NOT hand-manage file paths or directories here. Instead we rely on
// storage_common_resolve_path_and_ensure_app_directory(), which:
//   - resolves the APP_DATA_PATH alias into a real filesystem path, and
//   - creates the app's data directory if it doesn't already exist.
//
// File format:
//   MODE_FILE contains a single byte:
//     0 = 12-hour display
//     1 = 24-hour display
//
// Failure behavior:
//   - If the file doesn't exist or can't be read, we default to 12-hour mode.
//   - Writes are best-effort; if they fail, the app still runs normally.
//

#define MODE_FILE APP_DATA_PATH("mode24.bin")
#define SEGMENT_STYLE_FILE APP_DATA_PATH("segment_style.bin")

// Load persisted 24-hour mode setting from MODE_FILE.
//
// Returns:
//   true  => 24-hour mode enabled
//   false => 12-hour mode (default)
//
// Notes:
//   - Missing file is not an error; we treat it as "default settings".
//   - Uses the Storage service (RECORD_STORAGE) and a temporary File handle.
//   - Path resolution + directory creation is handled by the storage helper.
//
static bool load_mode_24h(void) {
    bool mode = false;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* f = storage_file_alloc(storage);

    FuriString* path = furi_string_alloc_set(MODE_FILE);
    storage_common_resolve_path_and_ensure_app_directory(storage, path);

    if(storage_file_open(f, furi_string_get_cstr(path), FSAM_READ, FSOM_OPEN_EXISTING)) {
        uint8_t b = 0;
        if(storage_file_read(f, &b, 1) == 1) mode = (b != 0);
        storage_file_close(f);
    }

    furi_string_free(path);
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);

    return mode;
}

// Save persisted 24-hour mode setting to MODE_FILE.
//
// Writes a single byte:
//   0 = 12-hour mode
//   1 = 24-hour mode
//
// Notes:
//   - Called when the user toggles mode (OK short press).
//   - Uses FSOM_CREATE_ALWAYS to overwrite atomically-at-our-scale.
//   - Best-effort: if the SD card isn't present or the write fails, we simply
//     won't remember the setting next launch.
//
static void save_mode_24h(bool mode) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* f = storage_file_alloc(storage);

    FuriString* path = furi_string_alloc_set(MODE_FILE);
    storage_common_resolve_path_and_ensure_app_directory(storage, path);

    if(storage_file_open(f, furi_string_get_cstr(path), FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        uint8_t b = mode ? 1 : 0;
        storage_file_write(f, &b, 1);
        storage_file_close(f);
    }

    furi_string_free(path);
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
}

// Load persisted segment style; defaults to classic block digits.
// Stored bytes are sanitized to only 0, 1, or 2.
static uint8_t load_segment_style(void) {
    uint8_t style = 0; // SegmentStyleClassic

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* f = storage_file_alloc(storage);

    FuriString* path = furi_string_alloc_set(SEGMENT_STYLE_FILE);
    storage_common_resolve_path_and_ensure_app_directory(storage, path);

    if(storage_file_open(f, furi_string_get_cstr(path), FSAM_READ, FSOM_OPEN_EXISTING)) {
        uint8_t b = 0;
        if(storage_file_read(f, &b, 1) == 1) {
            if(b == 0) {
                style = 0; // SegmentStyleClassic
            } else if(b == 1) {
                style = 1; // SegmentStyleLozenge
            } else if(b == 2) {
                style = 2; // SegmentStyleFont
            }
        }
        storage_file_close(f);
    }

    furi_string_free(path);
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);

    return style;
}

// Save persisted segment style.
//
// File format:
//   1 byte enum ordinal, same contract as SegmentStyle:
//   0=classic, 1=lozenge, 2=font.
//
// As with mode persistence, write failures are non-fatal and only affect
// whether the preference survives the next app launch.
static void save_segment_style(uint8_t style) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* f = storage_file_alloc(storage);

    FuriString* path = furi_string_alloc_set(SEGMENT_STYLE_FILE);
    storage_common_resolve_path_and_ensure_app_directory(storage, path);

    if(storage_file_open(f, furi_string_get_cstr(path), FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_write(f, &style, 1);
        storage_file_close(f);
    }

    furi_string_free(path);
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
}

// ----------------------------------------------------------------------------
// 7-seg digit drawing helpers
// ----------------------------------------------------------------------------
//
// We draw big "7-segment" style digits using filled rectangles.
// segmap encodes which segments are on for digits 0..9.
//
// bit positions:
// 0=a (top)
// 1=b (upper-right)
// 2=c (lower-right)
// 3=d (bottom)
// 4=e (lower-left)
// 5=f (upper-left)
// 6=g (middle)
//
static const uint8_t segmap[10] = {
    /*0*/ 0b0111111,
    /*1*/ 0b0000110,
    /*2*/ 0b1011011,
    /*3*/ 0b1001111,
    /*4*/ 0b1100110,
    /*5*/ 0b1101101,
    /*6*/ 0b1111101,
    /*7*/ 0b0000111,
    /*8*/ 0b1111111,
    /*9*/ 0b1101111,
};

// Segment style is an explicit ordinal contract used by persisted storage:
// 0=classic, 1=lozenge, 2=font.
typedef enum {
    SegmentStyleClassic = 0,
    SegmentStyleLozenge = 1,
    SegmentStyleFont = 2,
} SegmentStyle;

// Font glyph geometry constants are canonical for all font-style edits:
// - exactly 10 digits
// - 64 rows per digit
// - 24 columns per row
#define FONT_DIGIT_COUNT 10u
#define FONT_DIGIT_HEIGHT 64u
#define FONT_DIGIT_WIDTH 24u
#define FONT_DIGIT_ROW_BYTES (FONT_DIGIT_WIDTH + 1u)

// font_digits[d][y] maps directly to digit d (0..9), row y (0..63),
// with a human-friendly "."/"X" visual layout.
// Edit blocks are intentionally index-labeled (// 0 ... // 9) for quick manual tuning.
static const char font_digits[FONT_DIGIT_COUNT][FONT_DIGIT_HEIGHT][FONT_DIGIT_ROW_BYTES] = {
    // 0
    {
        "........XXXXXXXX........",
        ".......XXXXXXXXXX.......",
        "......XXXXXXXXXXXX......",
        ".....XXXXXXXXXXXXXX.....",
        "....XXXXXXXXXXXXXXX.....",
        "....XXXXXXXXXXXXXXXX....",
        "....XXXXXXX..XXXXXXX....",
        "...XXXXXX......XXXXXX...",
        "...XXXXXX.......XXXXX...",
        "..XXXXX..........XXXX...",
        "..XXXXX..........XXXXX..",
        "..XXXXX..........XXXXX..",
        "..XXXXX..........XXXXX..",
        "..XXXX............XXXX..",
        ".XXXXX............XXXX..",
        ".XXXXX............XXXXX.",
        ".XXXXX............XXXXX.",
        ".XXXXX............XXXXX.",
        ".XXXXX............XXXXX.",
        ".XXXX.............XXXXX.",
        ".XXXX..............XXXX.",
        ".XXXX..............XXXX.",
        ".XXXX..............XXXX.",
        "XXXXX..............XXXX.",
        "XXXXX..............XXXX.",
        "XXXXX..............XXXX.",
        "XXXXX..............XXXX.",
        "XXXXX..............XXXX.",
        "XXXXX..............XXXX.",
        "XXXXX..............XXXX.",
        "XXXXX..............XXXX.",
        "XXXXX..............XXXX.",
        "XXXXX..............XXXX.",
        "XXXXX..............XXXX.",
        "XXXXX..............XXXX.",
        "XXXXX..............XXXX.",
        "XXXXX..............XXXX.",
        "XXXXX..............XXXX.",
        "XXXXX..............XXXX.",
        "XXXXX..............XXXX.",
        "XXXXX..............XXXX.",
        "XXXXX..............XXXX.",
        "XXXXX.............XXXXX.",
        "XXXXX.............XXXXX.",
        "XXXXX.............XXXXX.",
        "XXXXX.............XXXXX.",
        ".XXXXX............XXXXX.",
        ".XXXXX............XXXXX.",
        ".XXXXX............XXXXX.",
        "..XXXX...........XXXXX..",
        "..XXXX...........XXXXX..",
        "..XXXXX..........XXXXX..",
        "..XXXXX..........XXXX...",
        "...XXXXX........XXXXX...",
        "...XXXXX.......XXXXXX...",
        "...XXXXXX.....XXXXXX....",
        "....XXXXXXX..XXXXXXX....",
        "....XXXXXXXXXXXXXXXX....",
        "....XXXXXXXXXXXXXXX.....",
        ".....XXXXXXXXXXXXXX.....",
        ".....XXXXXXXXXXXXX......",
        "......XXXXXXXXXXX.......",
        ".......XXXXXXXXX........",
        "........XXXXXXX.........",
    },
    // 1   
    {
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        ".............XXXXXX.....",
        "............XXXXXXX.....",
        ".........XXXXXXXXXX.....",
        ".....XXXXXXXXXXXXXX.....",
        ".....XXXXXXXXXXXXXX.....",
        ".....XXXXXXXXXXXXXX.....",
        ".....XXXXXXXXXXXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
    },
    // 2
    {
        "..........XXXX..........",
        "........XXXXXXXXX.......",
        ".......XXXXXXXXXXX......",
        "......XXXXXXXXXXXXX.....",
        ".....XXXXXXXXXXXXXXX....",
        "....XXXXXXXXXXXXXXXX....",
        "....XXXXXXXXXXXXXXXXX...",
        "....XXXXXXXXXXXXXXXXX...",
        "...XXXXXXX....XXXXXXXX..",
        "...XXXXXX......XXXXXXX..",
        "...XXXXX........XXXXXX..",
        "..XXXXXX.........XXXXX..",
        "..XXXXX..........XXXXXX.",
        "..XXXXX...........XXXXX.",
        "..XXXX............XXXXX.",
        "..XXXX............XXXXX.",
        ".XXXXX.............XXXXX",
        ".XXXXX.............XXXXX",
        ".XXXXX.............XXXXX",
        ".XXXXX.............XXXXX",
        ".XXXXX.............XXXXX",
        ".XXXXX.............XXXXX",
        ".XXXXX.............XXXXX",
        "..................XXXXX.",
        "..................XXXXX.",
        "..................XXXXX.",
        "..................XXXXX.",
        ".................XXXXXX.",
        ".................XXXXX..",
        "................XXXXXX..",
        "................XXXXXX..",
        "...............XXXXXX...",
        "..............XXXXXXX...",
        ".............XXXXXXX....",
        "............XXXXXXX.....",
        "...........XXXXXXXX.....",
        "..........XXXXXXXX......",
        ".........XXXXXXXX.......",
        "........XXXXXXXX........",
        "........XXXXXXX.........",
        ".......XXXXXXXX.........",
        "......XXXXXXXX..........",
        ".....XXXXXXXX...........",
        ".....XXXXXXX............",
        "....XXXXXXX.............",
        "....XXXXXX..............",
        "...XXXXXX...............",
        "...XXXXXX...............",
        "..XXXXXX................",
        "..XXXXX.................",
        "..XXXXX.................",
        "..XXXX..................",
        "..XXXX..................",
        ".XXXXX..................",
        ".XXXX...................",
        ".XXXX...................",
        ".XXXX...................",
        ".XXXXXXXXXXXXXXXXXXXXXX.",
        ".XXXXXXXXXXXXXXXXXXXXXX.",
        ".XXXXXXXXXXXXXXXXXXXXXX.",
        ".XXXXXXXXXXXXXXXXXXXXXX.",
        "XXXXXXXXXXXXXXXXXXXXXXX.",
        "XXXXXXXXXXXXXXXXXXXXXXX.",
        "XXXXXXXXXXXXXXXXXXXXXXX.",
    },

    // 3
    {
        "..........XXXX..........",
        "........XXXXXXXX........",
        ".......XXXXXXXXXX.......",
        "......XXXXXXXXXXXX......",
        ".....XXXXXXXXXXXXXX.....",
        "....XXXXXXXXXXXXXXXX....",
        "....XXXXXXXXXXXXXXXX....",
        "...XXXXXXXX  XXXXXXX....",
        "...XXXXXXX... XXXXXXX...",
        "...XXXXXX......XXXXXX...",
        "..XXXXXX........XXXXX...",
        "..XXXXXX.........XXXXX..",
        "..XXXXXX.........XXXXX..",
        "..XXXXXX.........XXXXX..",
        ".................XXXXX..",
        "..................XXXX..",
        "..................XXXX..",
        "..................XXXX..",
        "..................XXXX..",
        "..................XXXX..",
        ".................XXXXX..",
        ".................XXXXX..",
        "................XXXXX...",
        "................XXXXX...",
        "...............XXXXXX...",
        "...............XXXXXX...",
        "............XXXXXXXX....",
        ".....XXXXXXXXXXXXXXX....",
        ".....XXXXXXXXXXXXXX.....",
        ".....XXXXXXXXXXXXX......",
        ".....XXXXXXXXXXXXXX.....",
        ".....XXXXXXXXXXXXXX.....",
        "............XXXXXXXXX...",
        "..............XXXXXXX...",
        "...............XXXXXXX..",
        "................XXXXXX..",
        ".................XXXXX..",
        ".................XXXXXX.",
        "..................XXXXX.",
        "..................XXXXX.",
        "..................XXXXX.",
        "..................XXXXX.",
        "XXXXX..............XXXX.",
        "XXXXX..............XXXX.",
        "XXXXX..............XXXX.",
        "XXXXX..............XXXX.",
        "XXXXX..............XXXX.",
        "XXXXX..............XXXX.",
        "XXXXX..............XXXX.",
        ".XXXX..............XXXX.",
        ".XXXXX............XXXXX.",
        ".XXXXX............XXXXX.",
        ".XXXXX............XXXXX.",
        ".XXXXXX..........XXXXX..",
        ".XXXXXXX........XXXXXX..",
        "..XXXXXX.......XXXXXXX..",
        "..XXXXXXXX....XXXXXXX...",
        "...XXXXXXXXXXXXXXXXXX...",
        "...XXXXXXXXXXXXXXXXX....",
        "....XXXXXXXXXXXXXXXX....",
        ".....XXXXXXXXXXXXXX.....",
        ".....XXXXXXXXXXXXX......",
        "......XXXXXXXXXXX.......",
        ".........XXXXX..........",
    },
    // 4
    {
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        "..............XXXXX.....",
        ".............XXXXXX.....",
        ".............XXXXXX.....",
        ".............XXXXXX.....",
        "............XXXXXXX.....",
        "............XXXXXXX.....",
        "............XXXXXXX.....",
        "............XXXXXXX.....",
        "...........XXXXXXXX.....",
        "...........XXXXXXXX.....",
        "...........XXXXXXXX.....",
        "..........XXXX.XXXX.....",
        "..........XXX..XXXX.....",
        ".........XXXX..XXXX.....",
        ".........XXXX..XXXX.....",
        ".........XXX...XXXX.....",
        "........XXXX...XXXX.....",
        "........XXXX...XXXX.....",
        ".......XXXXX...XXXX.....",
        ".......XXXX....XXXX.....",
        ".......XXXX....XXXX.....",
        "......XXXXX....XXXX.....",
        "......XXXX.....XXXX.....",
        "......XXXX.....XXXX.....",
        ".....XXXXX.....XXXX.....",
        ".....XXXX......XXXX.....",
        ".....XXXX......XXXX.....",
        "....XXXX.......XXXX.....",
        "...XXXXX.......XXXX.....",
        "...XXXX........XXXX.....",
        "...XXXX........XXXX.....",
        "..XXXXX........XXXX.....",
        "..XXXX.........XXXX.....",
        ".XXXXX.........XXXX.....",
        "XXXXXX.........XXXX.....",
        "XXXXX..........XXXX.....",
        "XXXXX..........XXXX.....",
        "XXXXX..........XXXX.....",
        "XXXXXXXXXXXXXXXXXXXXXXX.",
        "XXXXXXXXXXXXXXXXXXXXXXX.",
        "XXXXXXXXXXXXXXXXXXXXXXX.",
        "XXXXXXXXXXXXXXXXXXXXXXX.",
        "XXXXXXXXXXXXXXXXXXXXXXX.",
        "XXXXXXXXXXXXXXXXXXXXXXX.",
        "...............XXXX.....",
        "...............XXXX.....",
        "...............XXXX.....",
        "...............XXXX.....",
        "...............XXXX.....",
        "...............XXXX.....",
        "...............XXXX.....",
        "...............XXXX.....",
        "...............XXXX.....",
        "...............XXXX.....",
        "...............XXXX.....",
        "...............XXXX.....",
        "...............XXXX.....",
        "...............XXXX.....",
        "...............XXXX.....",
        "...............XXXX.....",
    },
    // 5
    {
        "....XXXXXXXXXXXXXXXXX...",
        "....XXXXXXXXXXXXXXXXX...",
        "....XXXXXXXXXXXXXXXXX...",
        "....XXXXXXXXXXXXXXXXX...",
        "....XXXXXXXXXXXXXXXXX...",
        "....XXXXXXXXXXXXXXXXX...",
        "....XXXXXXXXXXXXXXXXX...",
        "....XXXXXXXXXXXXXXXXX...",
        "...XXXX.................",
        "...XXXX.................",
        "...XXXX.................",
        "...XXXX.................",
        "...XXXX.................",
        "...XXXX.................",
        "...XXXX.................",
        "...XXXX.................",
        "...XXX..................",
        "...XXX..................",
        "...XXX..................",
        "...XXX..................",
        "...XXX..................",
        "..XXXX.....XX...........",
        "..XXXX...XXXXXXX........",
        "..XXXX.XXXXXXXXXX.......",
        "..XXXXXXXXXXXXXXXX......",
        "..XXXXXXXXXXXXXXXXX.....",
        "..XXXXXXXXXXXXXXXXXX....",
        "..XXXXXXXXXXXXXXXXXX....",
        "..XXXXXXXXXXXXXXXXXXX...",
        "..XXXXXXX.....XXXXXXX...",
        "..XXXXXX.......XXXXXXX..",
        "..XXXXX.........XXXXXX..",
        "..XXXX...........XXXXXX.",
        "..XXXX...........XXXXXX.",
        "..................XXXXX.",
        "..................XXXXX.",
        "..................XXXXX.",
        "..................XXXXX.",
        "...................XXXXX",
        "...................XXXXX",
        "...................XXXXX",
        "...................XXXXX",
        "...................XXXXX",
        "...................XXXXX",
        "...................XXXX.",
        "...................XXXX.",
        "...................XXXX.",
        "XXXXX.............XXXXX.",
        "XXXXX.............XXXXX.",
        "XXXXX.............XXXXX.",
        ".XXXX.............XXXX..",
        ".XXXXX...........XXXXX..",
        ".XXXXX...........XXXXX..",
        ".XXXXXX.........XXXXXX..",
        "..XXXXX.........XXXXX...",
        "..XXXXXX.......XXXXXX...",
        "..XXXXXXXX....XXXXXXX...",
        "...XXXXXXXXXXXXXXXXX....",
        "...XXXXXXXXXXXXXXXXX....",
        "....XXXXXXXXXXXXXXX.....",
        "....XXXXXXXXXXXXXXX.....",
        ".....XXXXXXXXXXXXX......",
        "......XXXXXXXXXXX.......",
        ".........XXXXX..........",
    },
    // 6
    {
        "............XX..........",
        ".........XXXXXXXX.......",
        "........XXXXXXXXXX......",
        ".......XXXXXXXXXXXX.....",
        "......XXXXXXXXXXXXXX....",
        "......XXXXXXXXXXXXXX....",
        ".....XXXXXXXXXXXXXXXX...",
        ".....XXXXXXXXXXXXXXXX...",
        "....XXXXXXX....XXXXXX...",
        "....XXXXXX......XXXXXX..",
        "....XXXXX........XXXXX..",
        "...XXXXX..........XXXX..",
        "...XXXXX..........XXXX..",
        "...XXXX...........XXXXX.",
        "..XXXXX...........XXXXX.",
        "..XXXXX...........XXXXX.",
        "..XXXX.............XXXX.",
        "..XXXX..................",
        "..XXXX..................",
        "..XXXX..................",
        ".XXXXX..................",
        ".XXXX...................",
        ".XXXX...................",
        ".XXXX.....XXXXXX........",
        ".XXXX....XXXXXXXXX......",
        ".XXXX...XXXXXXXXXXX.....",
        ".XXXX..XXXXXXXXXXXX.....",
        ".XXXX.XXXXXXXXXXXXXX....",
        "XXXXX.XXXXXXXXXXXXXXX...",
        "XXXXXXXXXXXXXXXXXXXXX...",
        "XXXXXXXXXXXX.XXXXXXXXX..",
        "XXXXXXXXXX.....XXXXXXX..",
        "XXXXXXXX.........XXXXX..",
        "XXXXXXX..........XXXXXX.",
        "XXXXXXX...........XXXXX.",
        "XXXXXXX...........XXXXX.",
        "XXXXXX............XXXXX.",
        "XXXXXX............XXXXX.",
        "XXXXXX.............XXXXX",
        "XXXXXX.............XXXXX",
        "XXXXXX.............XXXXX",
        "XXXXXX.............XXXXX",
        "XXXXXX.............XXXXX",
        ".XXXXX.............XXXXX",
        ".XXXXX.............XXXXX",
        ".XXXXX.............XXXXX",
        ".XXXXX.............XXXXX",
        ".XXXXX.............XXXX.",
        ".XXXXX.............XXXX.",
        "..XXXX............XXXXX.",
        "..XXXX............XXXXX.",
        "..XXXXX...........XXXXX.",
        "..XXXXX...........XXXX..",
        "...XXXXX.........XXXXX..",
        "...XXXXX........XXXXXX..",
        "...XXXXXX.......XXXXX...",
        "....XXXXXXX...XXXXXXX...",
        "....XXXXXXXXXXXXXXXXX...",
        ".....XXXXXXXXXXXXXXX....",
        ".....XXXXXXXXXXXXXXX....",
        "......XXXXXXXXXXXXX.....",
        ".......XXXXXXXXXXX......",
        "........XXXXXXXXX.......",
        "........................",
    },
    // 7
    {
        ".XXXXXXXXXXXXXXXXXXXXXX.",
        ".XXXXXXXXXXXXXXXXXXXXXX.",
        ".XXXXXXXXXXXXXXXXXXXXXX.",
        ".XXXXXXXXXXXXXXXXXXXXXX.",
        ".XXXXXXXXXXXXXXXXXXXXXX.",
        ".XXXXXXXXXXXXXXXXXXXXXX.",
        ".XXXXXXXXXXXXXXXXXXXXXX.",
        ".XXXXXXXXXXXXXXXXXXXXXX.",
        "...................XXXX.",
        "...................XXXX.",
        "..................XXXX..",
        "..................XXXX..",
        ".................XXXXX..",
        ".................XXXX...",
        ".................XXXX...",
        "................XXXX....",
        "................XXXX....",
        "...............XXXXX....",
        "...............XXXX.....",
        "...............XXXX.....",
        "..............XXXXX.....",
        "..............XXXX......",
        "..............XXXX......",
        "..............XXXX......",
        ".............XXXXX......",
        ".............XXXX.......",
        ".............XXXX.......",
        "............XXXXX.......",
        "............XXXX........",
        "............XXXX........",
        "...........XXXXX........",
        "...........XXXXX........",
        "...........XXXX.........",
        "..........XXXXX.........",
        "..........XXXXX.........",
        "..........XXXX..........",
        "..........XXXX..........",
        ".........XXXXX..........",
        ".........XXXXX..........",
        ".........XXXXX..........",
        ".........XXXX...........",
        ".........XXXX...........",
        "........XXXXX...........",
        "........XXXXX...........",
        "........XXXXX...........",
        "........XXXX............",
        "........XXXX............",
        "........XXXX............",
        ".......XXXXX............",
        ".......XXXXX............",
        ".......XXXXX............",
        ".......XXXX.............",
        ".......XXXX.............",
        ".......XXXX.............",
        "......XXXXX.............",
        "......XXXXX.............",
        "......XXXXX.............",
        "......XXXXX.............",
        "......XXXXX.............",
        "......XXXXX.............",
        "......XXXX..............",
        "......XXXX..............",
        "......XXXX..............",
        ".....XXXXX..............",
    },
    // 8
    {
        "...........XX...........",
        "........XXXXXXXX........",
        ".......XXXXXXXXXX.......",
        "......XXXXXXXXXXXX......",
        ".....XXXXXXXXXXXXXX.....",
        "....XXXXXXXXXXXXXXXX....",
        "....XXXXXXXXXXXXXXXX....",
        "...XXXXXXXXXXXXXXXXX....",
        "...XXXXXXX...XXXXXXXX...",
        "...XXXXXX......XXXXXX...",
        "..XXXXXX........XXXXX...",
        "..XXXXX..........XXXXX..",
        "..XXXXX..........XXXXX..",
        "..XXXX...........XXXXX..",
        "..XXXX...........XXXXX..",
        "..XXXX............XXXX..",
        "..XXXX............XXXX..",
        "..XXXX............XXXX..",
        "..XXXX............XXXX..",
        "..XXXX............XXXX..",
        "..XXXX...........XXXXX..",
        "..XXXXX..........XXXXX..",
        "..XXXXX.........XXXXX...",
        "..XXXXX.........XXXXX...",
        "...XXXXX.......XXXXXX...",
        "...XXXXXX......XXXXXX...",
        "...XXXXXXXX.XXXXXXXX....",
        "....XXXXXXXXXXXXXXXX....",
        ".....XXXXXXXXXXXXXX.....",
        "......XXXXXXXXXXXX......",
        ".....XXXXXXXXXXXXXX.....",
        ".....XXXXXXXXXXXXXX.....",
        "...XXXXXXXXXXXXXXXXXX...",
        "...XXXXXXX....XXXXXXX...",
        "..XXXXXX.......XXXXXXX..",
        "..XXXXXX........XXXXXX..",
        "..XXXXX..........XXXXX..",
        ".XXXXX...........XXXXXX.",
        ".XXXXX............XXXXX.",
        ".XXXXX............XXXXX.",
        ".XXXX.............XXXXX.",
        ".XXXX.............XXXXX.",
        "XXXXX..............XXXX.",
        "XXXXX..............XXXX.",
        "XXXXX..............XXXX.",
        "XXXXX..............XXXX.",
        "XXXXX..............XXXX.",
        "XXXXX..............XXXX.",
        "XXXXX..............XXXX.",
        ".XXXX..............XXXX.",
        ".XXXXX............XXXXX.",
        ".XXXXX............XXXXX.",
        ".XXXXX............XXXXX.",
        ".XXXXXX..........XXXXX..",
        "..XXXXXX........XXXXXX..",
        "..XXXXXX.......XXXXXXX..",
        "..XXXXXXXX....XXXXXXX...",
        "...XXXXXXXXXXXXXXXXXX...",
        "...XXXXXXXXXXXXXXXXX....",
        "....XXXXXXXXXXXXXXXX....",
        ".....XXXXXXXXXXXXXX.....",
        ".....XXXXXXXXXXXXX......",
        "......XXXXXXXXXXX.......",
        ".........XXXXX..........",
    },
    // 9
        {
        "..........XXXX..........",
        "........XXXXXXXX........",
        ".......XXXXXXXXXX.......",
        "......XXXXXXXXXXXX......",
        ".....XXXXXXXXXXXXXX.....",
        ".....XXXXXXXXXXXXXX.....",
        "....XXXXXXXXXXXXXXXX....",
        "....XXXXXXXXXXXXXXXX....",
        "...XXXXXXXX...XXXXXXX...",
        "...XXXXXX......XXXXXX...",
        "...XXXXX........XXXXX...",
        "..XXXXX..........XXXXX..",
        "..XXXXX..........XXXXX..",
        "..XXXX............XXXX..",
        "..XXXX............XXXX..",
        ".XXXXX............XXXXX.",
        ".XXXXX............XXXXX.",
        ".XXXXX............XXXXX.",
        ".XXXXX.............XXXX.",
        "XXXXX..............XXXX.",
        "XXXXX..............XXXX.",
        "XXXXX..............XXXX.",
        "XXXXX..............XXXX.",
        "XXXXX..............XXXX.",
        "XXXXXX.............XXXXX",
        "XXXXXX............XXXXXX",
        ".XXXXX............XXXXXX",
        ".XXXXX............XXXXXX",
        ".XXXXX............XXXXXX",
        "..XXXX............XXXXXX",
        "..XXXXX..........XXXXXXX",
        "..XXXXX..........XXXXXXX",
        "..XXXXXX........XXXXXXXX",
        "...XXXXXX......XXXXXXXXX",
        "...XXXXXXXX..XXXXXXXXXXX",
        "....XXXXXXXXXXXXXXXXXXX.",
        "....XXXXXXXXXXXXXX.XXXX.",
        ".....XXXXXXXXXXXXX.XXXX.",
        ".....XXXXXXXXXXXX..XXXX.",
        "......XXXXXXXXXXX..XXXX.",
        ".......XXXXXXXXX...XXXX.",
        ".........XXXXX.....XXXX.",
        "...................XXXX.",
        "..................XXXXX.",
        "..................XXXXX.",
        "..................XXXXX.",
        "..................XXXX..",
        "..................XXXX..",
        "XXXXXX............XXXX..",
        "XXXXXX............XXXX..",
        ".XXXXX...........XXXXX..",
        ".XXXXX...........XXXX...",
        ".XXXXX...........XXXX...",
        ".XXXXXX.........XXXXX...",
        "..XXXXX........XXXXX....",
        "..XXXXXX.......XXXXX....",
        "...XXXXXX.....XXXXXX....",
        "...XXXXXXXX.XXXXXXX.....",
        "...XXXXXXXXXXXXXXXX.....",
        "....XXXXXXXXXXXXXX......",
        ".....XXXXXXXXXXXXX......",
        ".....XXXXXXXXXXXX.......",
        "......XXXXXXXXXX........",
        ".........XXXX...........",
    },
};

_Static_assert(FONT_DIGIT_COUNT == 10u, "font_digits count must remain 10");
_Static_assert(FONT_DIGIT_WIDTH == 24u, "font_digits row width must remain 24");
_Static_assert(sizeof(font_digits) == (FONT_DIGIT_COUNT * FONT_DIGIT_HEIGHT * FONT_DIGIT_ROW_BYTES),
               "font_digits requires exactly 10x64 rows of 24-char patterns");
_Static_assert(sizeof(font_digits[0]) == (FONT_DIGIT_HEIGHT * FONT_DIGIT_ROW_BYTES),
               "font_digits digit entries must contain exactly 64 rows");
_Static_assert(sizeof(font_digits[0][0]) == FONT_DIGIT_ROW_BYTES,
               "font_digits rows must be 24 chars plus terminator");

// Compile-time checks guarantee table shape; debug-time checks validate data quality.
#ifndef NDEBUG
static bool g_font_digits_valid = true;

// Validate a row has only supported glyph chars and exactly one terminating nul.
static bool validate_font_digit_row(const char* row) {
    if(!row) return false;
    for(int x = 0; x < (int)FONT_DIGIT_WIDTH; x++) {
        const char c = row[x];
        if(c != '.' && c != 'X') return false;
    }
    return row[FONT_DIGIT_WIDTH] == '\0';
}

static bool validate_font_digits(void) {
    // Full table scan: exactly 10 glyphs * 64 rows each.
    // We intentionally stop at first error because any malformed row means
    // the font cannot be trusted for rendering.
    for(int d = 0; d < (int)FONT_DIGIT_COUNT; d++) {
        for(int y = 0; y < (int)FONT_DIGIT_HEIGHT; y++) {
            if(!validate_font_digit_row(font_digits[d][y])) return false;
        }
    }
    return true;
}
#endif

// Font renderer is intentionally simple: draw dots where glyph has 'X', up to h rows.
static void draw_scaled_font_digit(Canvas* c, int x, int y, int w, int h, int d) {
    if(d < 0 || d > 9 || w <= 0 || h <= 0) return;

    const int rows_to_draw = h < (int)FONT_DIGIT_HEIGHT ? h : (int)FONT_DIGIT_HEIGHT;
    for(int yy = 0; yy < rows_to_draw; yy++) {
        const char* row = font_digits[d][yy];
        for(int x_off = 0; x_off < (int)FONT_DIGIT_WIDTH && x_off < w; x_off++) {
            if(row[x_off] == 'X') {
                canvas_draw_dot(c, x + x_off, y + yy);
            }
        }
    }
}

#ifndef NDEBUG
// Debug marker for invalid font data; visible only when font mode is selected.
static void draw_font_digits_invalid_marker(Canvas* c) {
    for(int i = 0; i < 10; i++) {
        canvas_draw_dot(c, 1 + i, 1 + i);
        canvas_draw_dot(c, 10 - i, 1 + i);
        canvas_draw_dot(c, 1 + i, 10 + i);
    }
    for(int x = 0; x < 12; x++) {
        canvas_draw_dot(c, 1 + x, 10);
        canvas_draw_dot(c, 10 + x, 1);
    }
}
#endif

#ifndef NDEBUG
static void invalidate_font_digits_if_needed(void) {
    // Startup-time cache of validation status.
    // Draw path reads this boolean to either render glyphs or show debug marker.
    g_font_digits_valid = validate_font_digits();
}
#endif



// Draw a horizontal lozenge segment: center rectangle with tapered end caps.
// Used by segdigit() for styles that emulate LCD-like angled segment ends.
static void draw_hseg_lozenge(Canvas* c, int x, int y, int w, int t) {
    if(w <= 0 || t <= 0) return;
    int tip = (t / 2) + 1;
    if((tip * 2) >= w) {
        canvas_draw_box(c, x, y, w, t);
        return;
    }

    canvas_draw_box(c, x + tip, y, w - (tip * 2), t);

    const int den = (t + 1) / 2;
    for(int dy = 0; dy < t; dy++) {
        int k = dy;
        if(k > (t - 1 - dy)) k = (t - 1 - dy);
        int reach = (tip * (k + 1)) / den;
        if(reach < 1) reach = 1;

        canvas_draw_line(c, x + tip - reach, y + dy, x + tip - 1, y + dy);
        canvas_draw_line(c, x + w - tip, y + dy, x + w - tip + reach - 1, y + dy);
    }
}

// Draw a vertical lozenge segment: center rectangle with tapered end caps.
// Symmetric companion to draw_hseg_lozenge(); same geometry strategy on Y axis.
static void draw_vseg_lozenge(Canvas* c, int x, int y, int t, int h) {
    if(h <= 0 || t <= 0) return;
    int tip = (t / 2) + 1;
    if((tip * 2) >= h) {
        canvas_draw_box(c, x, y, t, h);
        return;
    }

    canvas_draw_box(c, x, y + tip, t, h - (tip * 2));

    const int den = (t + 1) / 2;
    for(int dx = 0; dx < t; dx++) {
        int k = dx;
        if(k > (t - 1 - dx)) k = (t - 1 - dx);
        int reach = (tip * (k + 1)) / den;
        if(reach < 1) reach = 1;

        canvas_draw_line(c, x + dx, y + tip - reach, x + dx, y + tip - 1);
        canvas_draw_line(c, x + dx, y + h - tip, x + dx, y + h - tip + reach - 1);
    }
}

// ----------------------------------------------------------------------------
// segdigit()
// ----------------------------------------------------------------------------
//
// Draw one large 7-segment-style digit using filled rectangles.
//
// This is a deliberately "brute force" renderer: it does no caching and it
// does not attempt to be a general font system. It simply takes a digit value
// (0-9) and draws the corresponding segments into a fixed bounding box.
//
// Parameters:
//   c  - Canvas to draw into.
//   x,y- Top-left corner of the digit bounding box.
//   w  - Total digit width in pixels (including segment thickness).
//   h  - Total digit height in pixels.
//   t  - Segment thickness in pixels.
//   d  - Digit to draw:
//          0..9 draws that digit
//          -1 means "blank" (used for suppressing a leading 0 in 12h mode)
//
// Segment layout (classic 7-seg):
//
//        a
//     +-----+
//   f |     | b
//     |- g -|
//   e |     | c
//     +-----+
//        d
//
// Implementation notes:
//   - segmap[] holds the segment enable bitmask for each digit.
//   - Horizontal segments (a,g,d) are drawn full-width so overlaps look solid.
//   - Vertical segments (b,c,e,f) each span half height, meeting the middle bar.
//   - The middle segment 'g' is centered at y + h/2 with a half-thickness offset.
//   - This expects sane values (t <= w and t <= h/2). If you make t huge,
//     you'll get interesting... abstract art.
//   - style dispatch is fixed: classic -> rectangles, lozenge -> tapered, font -> bitmap rows.
//
static void segdigit(Canvas* c, int x, int y, int w, int h, int t, int d, SegmentStyle style) {
    // d is -1 to mean "blank" (used for leading zero in hours).
    if(d < 0 || d > 9) return;

    uint8_t m = segmap[d];
    int y_mid = y + ((h - t) / 2);
    int half = h / 2;

    if(style == SegmentStyleLozenge) {
        // Lozenge style: tapered segment ends with small air gaps between neighbors.
        const int g = 0;
        const int x_left = x + g;
        const int x_right = x + w - t - g;
        const int y_top = y + g;
        const int y_mid_seg = y_mid;
        const int y_bot = y + h - t - g;

        // Centerlines of segment rails.
        const int cx_left = x_left + (t / 2);
        const int cx_right = x_right + (t / 2);
        const int cy_top = y_top + (t / 2);
        const int cy_mid = y_mid_seg + (t / 2);
        const int cy_bot = y_bot + (t / 2);

        // Horizontal bars terminate at vertical segment centerlines.
        const int h_x = cx_left;
        int h_w = (cx_right - cx_left + 1);
        if(h_w < 1) h_w = 1;

        // Vertical bars span centerline-to-centerline.
        const int upper_y = cy_top;
        int upper_h = (cy_mid - cy_top + 1);
        const int lower_y = cy_mid;
        int lower_h = (cy_bot - cy_mid + 1);
        if(upper_h < 1) upper_h = 1;
        if(lower_h < 1) lower_h = 1;

        if(m & (1 << 0)) draw_hseg_lozenge(c, h_x, y_top, h_w, t); // a
        if(m & (1 << 6)) draw_hseg_lozenge(c, h_x, y_mid_seg, h_w, t); // g
        if(m & (1 << 3)) draw_hseg_lozenge(c, h_x, y_bot, h_w, t); // d

        if(m & (1 << 5)) draw_vseg_lozenge(c, x_left, upper_y, t, upper_h); // f
        if(m & (1 << 1)) draw_vseg_lozenge(c, x_right, upper_y, t, upper_h); // b
        if(m & (1 << 4)) draw_vseg_lozenge(c, x_left, lower_y, t, lower_h); // e
        if(m & (1 << 2)) draw_vseg_lozenge(c, x_right, lower_y, t, lower_h); // c
    } else if(style == SegmentStyleFont) {
        draw_scaled_font_digit(c, x, y, w, h, d);
    } else {
        // Original look: plain full-thickness rectangles.
        if(m & (1 << 0)) canvas_draw_box(c, x, y, w, t);            // a
        if(m & (1 << 6)) canvas_draw_box(c, x, y_mid, w, t); // g
        if(m & (1 << 3)) canvas_draw_box(c, x, y + h - t, w, t);    // d

        if(m & (1 << 5)) canvas_draw_box(c, x, y, t, half);                    // f
        if(m & (1 << 1)) canvas_draw_box(c, x + w - t, y, t, half);            // b
        if(m & (1 << 4)) canvas_draw_box(c, x, y + h - half, t, half);         // e
        if(m & (1 << 2)) canvas_draw_box(c, x + w - t, y + h - half, t, half); // c
    }
}

// ----------------------------------------------------------------------------
// draw_colon()
// ----------------------------------------------------------------------------
//
// Draw the ":" between HH and MM as two filled square dots.
//
// Parameters:
//   c  - Canvas to draw into.
//   x,y- Origin; we treat y as the top of the digit area.
//   t  - Dot size in pixels (we reuse the "segment thickness" so it matches
//        the visual weight of the 7-seg digits).
//
// Notes:
//   - The Y offsets (currently +16 and +40) are tuned for the 64px-tall layout.
//     If you change digit height or vertical positioning, these may need retuning.
//   - This is intentionally dumb and fast: no blink, no animation, just pixels.
//
static void draw_colon(Canvas* c, int x, int y, int t) {
    // Two square dots between HH and MM.
    canvas_draw_box(c, x, y + 16, t, t);
    canvas_draw_box(c, x, y + 40, t, t);
}

// ----------------------------------------------------------------------------
// Draw callback
// ----------------------------------------------------------------------------
//
// This is called by the GUI when the ViewPort needs repainting.
// We do not store time in app state. We read RTC each draw and render from scratch.
//
static void draw_cb(Canvas* canvas, void* ctx) {
    App* app = ctx;
    // Resolve style from persisted user preference; no stateful mode is stored in draw.
    const SegmentStyle style = app ? (SegmentStyle)app->segment_style : SegmentStyleClassic;

#ifndef NDEBUG
    if(style == SegmentStyleFont && !g_font_digits_valid) {
        draw_font_digits_invalid_marker(canvas);
        return;
    }
#endif

    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);

    const int H24 = (int)dt.hour;
    const int M   = (int)dt.minute;

    int H = H24; // display hour
    bool show_24 = app && app->mode_24h;

    if(!show_24) {
        // 12-hour clock: 0 -> 12, 13 -> 1, etc.
        H = H24 % 12;
        if(H == 0) H = 12;
    }

    // Hours tens digit is blank for 1..9 in 12h mode; in 24h mode show 0 for 00..09.
    int ht_raw = H / 10;
    int ht = ht_raw;
    if(!show_24 && ht_raw == 0) ht = -1;

    const int ho = H % 10;

    const int mt = M / 10;
    const int mo = M % 10;

    // Layout constants tuned for 128x64.
    // Right side reserves a narrow gutter for a small "alive" indicator.
    const int y = 0;
    const int h = 64;
    const int t = 7;

    const int w = 24;
    const int gap = 4;
    const int colon_w = 6;
    const int colon_gap = 2;
    const int ap_w = 10;
    const int gap_to_ap = 4;
    const int ap_x = 127 - ap_w;
    const int right_edge = ap_x - gap_to_ap;
    const int bar_area_w = ap_w;
    const int x0 = right_edge - ((w * 4) + (gap * 2) + colon_w + (colon_gap * 2));

    const int xH0 = x0;
    const int xH1 = xH0 + w + gap;

    const int cx  = xH1 + w + colon_gap;
    const int xM0 = cx + colon_w + colon_gap;
    const int xM1 = xM0 + w + gap;

    // Defensive guard: if constants ever change and overflow the screen, draw marker.
    if(xM1 + w <= right_edge) {
        segdigit(canvas, xH0, y, w, h, t, ht, style);
        segdigit(canvas, xH1, y, w, h, t, ho, style);
        draw_colon(canvas, cx, y, colon_w);
        segdigit(canvas, xM0, y, w, h, t, mt, style);
        segdigit(canvas, xM1, y, w, h, t, mo, style);
    } else {
        canvas_draw_box(canvas, 0, 0, 3, 3);
    }

    // 10-second progress indicator: draw N outlined boxes (no fill), where:
    // 0s => 0 boxes, 10s => 1 box, ... 50s => 5 boxes.
    const int steps = 5;
    int count = (int)dt.second / 10; // 0..5
    if(count > steps) count = steps;

    // Make the column shorter to leave room for AM/PM at the bottom.
    const int bar_w = 11;
    const int bar_h = 7;
    const int bar_gap = 1;

    const int bx = ap_x + ((bar_area_w - bar_w + 1) / 2);
    const int by = 0;

    for(int i = 0; i < count; i++) {
        int yy = by + i * (bar_h + bar_gap);
        for(int dx = 0; dx < bar_w; dx++) {
            canvas_draw_dot(canvas, bx + dx, yy);
            canvas_draw_dot(canvas, bx + dx, yy + bar_h - 1);
        }
        for(int dy = 0; dy < bar_h; dy++) {
            canvas_draw_dot(canvas, bx, yy + dy);
            canvas_draw_dot(canvas, bx + bar_w - 1, yy + dy);
        }
    }

    // AM/PM indicator (LCD-style): two fixed labels, only one is "lit".
    // They must not occupy the same location.
    const bool is_pm = (H24 >= 12);
    const bool is_am = !is_pm;

    const int ap_y0 = 48;

    canvas_set_font(canvas, FontKeyboard);
    if(show_24) {
        canvas_draw_str(canvas, ap_x, ap_y0, "24");
    } else {
        if(is_am) canvas_draw_str(canvas, ap_x, ap_y0 + 8, "AM");
        if(is_pm) canvas_draw_str(canvas, ap_x, ap_y0 + 16, "PM");
    }
    canvas_set_font(canvas, FontPrimary);
}

// ----------------------------------------------------------------------------
// Input + tick
// ----------------------------------------------------------------------------
//
// ViewPort input callback runs in GUI context.
// We do the standard pattern: enqueue the event and let the main loop handle it.
//
static void input_cb(InputEvent* event, void* ctx) {
    App* app = ctx;
    // Non-blocking producer in GUI/input context; main loop consumes from app.q.
    furi_message_queue_put(app->q, event, FuriWaitForever);
}

//
// Timer callback: request a redraw of the ViewPort.
//
static void tick_cb(void* ctx) {
    ViewPort* vp = ctx;
    // Timer only requests repaint; all clock state is derived in draw_cb().
    view_port_update(vp);
}

// ----------------------------------------------------------------------------
// Entry point
// ----------------------------------------------------------------------------
//
// bigclock_app is the Flipper entry point.
// Baseline behavior:
// - Force backlight on while running.
// - Redraw once per second.
// - Exit on BACK (short press).
//
int32_t bigclock_app(void* p) {
    UNUSED(p);

    App app = {0};
    // Preferences are loaded once up front; time itself is always pulled from RTC.
    app.mode_24h = load_mode_24h();
    app.segment_style = load_segment_style();

    // Input events sent from ViewPort callback to this thread.
    app.q = furi_message_queue_alloc(8, sizeof(InputEvent));

    // Create fullscreen ViewPort and attach draw + input callbacks.
    app.vp = view_port_alloc();
    view_port_draw_callback_set(app.vp, draw_cb, &app);
    view_port_input_callback_set(app.vp, input_cb, &app);

    // Register the ViewPort with the system GUI.
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, app.vp, GuiLayerFullscreen);

    // Notification service controls system features like backlight.
    app.notif = furi_record_open(RECORD_NOTIFICATION);

    // Keep backlight on so the clock stays visible (no auto-timeout).
    notification_message(app.notif, &sequence_display_backlight_enforce_on);

    // Debug-only one-time font-data pass; this keeps malformed glyphs visible
    // when font mode is selected.
    #ifndef NDEBUG
    invalidate_font_digits_if_needed();
    #endif

    // Once-per-second redraw so time and alive indicator update.
    app.timer = furi_timer_alloc(tick_cb, FuriTimerTypePeriodic, app.vp);
    furi_timer_start(app.timer, furi_ms_to_ticks(1000));

    // Main event loop: wait for input events and handle only BACK-to-exit.
    InputEvent event;
    while(true) {
        furi_message_queue_get(app.q, &event, FuriWaitForever);

        // Exit on BACK short press.
        if(event.type == InputTypeShort && event.key == InputKeyBack) {
            break;
        }
        // Toggle 12/24 hour on OK
        if(event.type == InputTypeShort && event.key == InputKeyOk) {
            app.mode_24h = !app.mode_24h;
            save_mode_24h(app.mode_24h);
            view_port_update(app.vp);
        }
        // Cycle segment style on long OK: Classic -> Lozenge -> Font.
        if(event.type == InputTypeLong && event.key == InputKeyOk) {
            app.segment_style = (uint8_t)((app.segment_style + 1U) % 3U);
            save_segment_style(app.segment_style);
            view_port_update(app.vp);
        }
    }

    // Stop periodic redraws.
    furi_timer_stop(app.timer);
    furi_timer_free(app.timer);

    // Remove ViewPort and release GUI record.
    gui_remove_view_port(gui, app.vp);
    view_port_free(app.vp);
    furi_record_close(RECORD_GUI);

    // Free input queue.
    furi_message_queue_free(app.q);

    // Restore normal backlight behavior and clear any display overrides.
    notification_message(app.notif, &sequence_display_backlight_enforce_auto);
    furi_record_close(RECORD_NOTIFICATION);
    return 0;
}
