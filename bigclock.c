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

// Load persisted segment style; defaults to lozenge.
static uint8_t load_segment_style(void) {
    uint8_t style = 2; // SegmentStyleLozenge

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* f = storage_file_alloc(storage);

    FuriString* path = furi_string_alloc_set(SEGMENT_STYLE_FILE);
    storage_common_resolve_path_and_ensure_app_directory(storage, path);

    if(storage_file_open(f, furi_string_get_cstr(path), FSAM_READ, FSOM_OPEN_EXISTING)) {
        uint8_t b = 0;
        if(storage_file_read(f, &b, 1) == 1 && b <= 2) style = b;
        storage_file_close(f);
    }

    furi_string_free(path);
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);

    return style;
}

// Save persisted segment style.
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

typedef enum {
    SegmentStyleClassic = 0,
    SegmentStylePinched = 1,
    SegmentStyleLozenge = 2,
} SegmentStyle;

// Draw a horizontal segment with a subtle center "pinch":
// full-thickness shoulders and a slightly thinner middle section.
static void draw_hseg_pinched(Canvas* c, int x, int y, int w, int t) {
    if(w <= 0 || t <= 0) return;

    const int pinch = (t >= 4) ? ((t >= 8) ? 2 : 1) : 0;
    const int mid_h = t - (pinch * 2);
    if(mid_h <= 0 || w < 5) {
        canvas_draw_box(c, x, y, w, t);
        return;
    }

    int shoulder = w / 4;
    if(shoulder < 2) shoulder = 2;
    if(shoulder > 6) shoulder = 6;
    if(shoulder * 2 >= w) {
        canvas_draw_box(c, x, y, w, t);
        return;
    }

    const int mid_w = w - (shoulder * 2);
    canvas_draw_box(c, x, y, shoulder, t);
    canvas_draw_box(c, x + shoulder, y + pinch, mid_w, mid_h);
    canvas_draw_box(c, x + shoulder + mid_w, y, shoulder, t);
}

// Draw a vertical segment with a subtle center "pinch":
// full-width caps and a slightly thinner middle section.
static void draw_vseg_pinched(Canvas* c, int x, int y, int t, int h) {
    if(h <= 0 || t <= 0) return;

    const int pinch = (t >= 4) ? ((t >= 8) ? 2 : 1) : 0;
    const int mid_w = t - (pinch * 2);
    if(mid_w <= 0 || h < 5) {
        canvas_draw_box(c, x, y, t, h);
        return;
    }

    int cap = h / 4;
    if(cap < 2) cap = 2;
    if(cap > 6) cap = 6;
    if(cap * 2 >= h) {
        canvas_draw_box(c, x, y, t, h);
        return;
    }

    const int mid_h = h - (cap * 2);
    canvas_draw_box(c, x, y, t, cap);
    canvas_draw_box(c, x + pinch, y + cap, mid_w, mid_h);
    canvas_draw_box(c, x, y + cap + mid_h, t, cap);
}

// Draw a horizontal lozenge segment: center rectangle with tapered end caps.
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
//
static void segdigit(Canvas* c, int x, int y, int w, int h, int t, int d, SegmentStyle style) {
    // d is -1 to mean "blank" (used for leading zero in hours).
    if(d < 0 || d > 9) return;

    uint8_t m = segmap[d];
    int ym = y + (h / 2);
    int half = h / 2;

    if(style == SegmentStylePinched) {
        // Horizontal segments with a narrower center section.
        if(m & (1 << 0)) draw_hseg_pinched(c, x, y, w, t);            // a
        if(m & (1 << 6)) draw_hseg_pinched(c, x, ym - (t / 2), w, t); // g
        if(m & (1 << 3)) draw_hseg_pinched(c, x, y + h - t, w, t);    // d

        // Vertical segments with a narrower center section.
        if(m & (1 << 5)) draw_vseg_pinched(c, x, y, t, half);                    // f
        if(m & (1 << 1)) draw_vseg_pinched(c, x + w - t, y, t, half);            // b
        if(m & (1 << 4)) draw_vseg_pinched(c, x, y + h - half, t, half);         // e
        if(m & (1 << 2)) draw_vseg_pinched(c, x + w - t, y + h - half, t, half); // c
    } else if(style == SegmentStyleLozenge) {
        // Lozenge style: tapered segment ends with small air gaps between neighbors.
        const int g = 1;
        const int x_left = x + g;
        const int x_right = x + w - t - g;
        const int y_top = y + g;
        const int y_mid = ym - (t / 2);
        const int y_bot = y + h - t - g;

        // Centerlines of segment rails.
        const int cx_left = x_left + (t / 2);
        const int cx_right = x_right + (t / 2);
        const int cy_top = y_top + (t / 2);
        const int cy_mid = y_mid + (t / 2);
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
        if(m & (1 << 6)) draw_hseg_lozenge(c, h_x, y_mid, h_w, t); // g
        if(m & (1 << 3)) draw_hseg_lozenge(c, h_x, y_bot, h_w, t); // d

        if(m & (1 << 5)) draw_vseg_lozenge(c, x_left, upper_y, t, upper_h); // f
        if(m & (1 << 1)) draw_vseg_lozenge(c, x_right, upper_y, t, upper_h); // b
        if(m & (1 << 4)) draw_vseg_lozenge(c, x_left, lower_y, t, lower_h); // e
        if(m & (1 << 2)) draw_vseg_lozenge(c, x_right, lower_y, t, lower_h); // c
    } else {
        // Original look: plain full-thickness rectangles.
        if(m & (1 << 0)) canvas_draw_box(c, x, y, w, t);            // a
        if(m & (1 << 6)) canvas_draw_box(c, x, ym - (t / 2), w, t); // g
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
    const SegmentStyle style = app ? (SegmentStyle)app->segment_style : SegmentStyleLozenge;

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
    const int colon_w = 4;
    const int colon_gap = 2;
    const int ap_w = 10;
    const int gap_to_ap = 4;
    const int ap_x = 128 - ap_w;
    const int right_edge = ap_x - gap_to_ap;
    const int bar_area_w = ap_w;
    const int x0 = right_edge - ((w * 4) + (gap * 2) + colon_w + (colon_gap * 2));

    const int xH0 = x0;
    const int xH1 = xH0 + w + gap;

    const int cx  = xH1 + w + colon_gap;
    const int xM0 = cx + colon_w + colon_gap;
    const int xM1 = xM0 + w + gap;

    // Defensive guard: if constants ever change and overflow the screen, draw a marker.
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
    const int bar_w = 9;
    const int bar_h = 7;
    const int bar_gap = 1;

    const int bx = right_edge + ((bar_area_w - bar_w + 1) / 2) ;
    const int by = 0;

    for(int i = 0; i < count; i++) {
        int yy = by + i * (bar_h + bar_gap);
        canvas_draw_frame(canvas, bx, yy, bar_w, bar_h);
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
    furi_message_queue_put(app->q, event, FuriWaitForever);
}

//
// Timer callback: request a redraw of the ViewPort.
//
static void tick_cb(void* ctx) {
    ViewPort* vp = ctx;
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
        // Cycle segment style on long OK: Classic -> Pinched -> Lozenge.
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
