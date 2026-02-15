#include <furi.h>
#include <furi_hal_rtc.h>

#include <gui/gui.h>
#include <gui/canvas.h>
#include <gui/view_port.h>

#include <input/input.h>

#include <notification/notification.h>
#include <notification/notification_messages.h>

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
} App;

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

static void segdigit(Canvas* c, int x, int y, int w, int h, int t, int d) {
    // d is -1 to mean "blank" (used for leading zero in hours).
    if(d < 0 || d > 9) return;

    uint8_t m = segmap[d];
    int ym = y + (h / 2);
    int half = h / 2;

    // Horizontal segments. Full width so overlaps look solid (especially digit 8).
    if(m & (1 << 0)) canvas_draw_box(c, x, y, w, t);               // a
    if(m & (1 << 6)) canvas_draw_box(c, x, ym - (t / 2), w, t);    // g
    if(m & (1 << 3)) canvas_draw_box(c, x, y + h - t, w, t);       // d

    // Vertical segments. Each spans half height so they meet the middle bar cleanly.
    if(m & (1 << 5)) canvas_draw_box(c, x, y, t, half);                       // f
    if(m & (1 << 1)) canvas_draw_box(c, x + w - t, y, t, half);               // b
    if(m & (1 << 4)) canvas_draw_box(c, x, y + h - half, t, half);            // e
    if(m & (1 << 2)) canvas_draw_box(c, x + w - t, y + h - half, t, half);    // c
}

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
    UNUSED(ctx);

    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);

    const int H24 = (int)dt.hour;
    const int M   = (int)dt.minute;

    // 12-hour clock: 0 -> 12, 13 -> 1, etc.
    int H12 = H24 % 12;
    if(H12 == 0) H12 = 12;

    // Hours tens digit is blank for 1..9, then 1 for 10..12.
    const int ht_raw = H12 / 10;               // 0..1
    const int ht = (ht_raw == 0) ? -1 : ht_raw;
    const int ho = H12 % 10;

    const int mt = M / 10;
    const int mo = M % 10;

    // Layout constants tuned for 128x64.
    // Right side reserves a narrow gutter for a small "alive" indicator.
    const int y = 2;
    const int h = 60;
    const int t = 7;

    const int bar_area_w = 12;
    const int right_edge = 128 - 2 - bar_area_w;

    const int w = 23;
    const int gap = 3;
    const int colon_w = 6;
    const int colon_gap = 2;

    const int x0 = 2;

    const int xH0 = x0;
    const int xH1 = xH0 + w + gap;

    const int cx  = xH1 + w + colon_gap;
    const int xM0 = cx + colon_w + colon_gap;
    const int xM1 = xM0 + w + gap;

    // Defensive guard: if constants ever change and overflow the screen, draw a marker.
    if(xM1 + w <= right_edge) {
        segdigit(canvas, xH0, y, w, h, t, ht);
        segdigit(canvas, xH1, y, w, h, t, ho);
        draw_colon(canvas, cx, y, colon_w);
        segdigit(canvas, xM0, y, w, h, t, mt);
        segdigit(canvas, xM1, y, w, h, t, mo);
    } else {
        canvas_draw_box(canvas, 0, 0, 3, 3);
    }

    // 10-second progress indicator: draw N outlined boxes (no fill), where:
    // 0s => 0 boxes, 10s => 1 box, ... 50s => 5 boxes.
    const int steps = 5;
    int count = (int)dt.second / 10; // 0..5
    if(count > steps) count = steps;

    // Make the column shorter to leave room for AM/PM at the bottom.
    const int bar_w = 6;
    const int bar_h = 8;
    const int bar_gap = 1;

    const int bx = right_edge + ((bar_area_w - bar_w) / 2);
    const int by = 2;

    for(int i = 0; i < count; i++) {
        int yy = by + i * (bar_h + bar_gap);
        canvas_draw_frame(canvas, bx, yy, bar_w, bar_h);
    }

    // AM/PM indicator (LCD-style): two fixed labels, only one is "lit".
    // They must not occupy the same location.
    const bool is_pm = (((int)dt.hour) >= 12);
    const bool is_am = !is_pm;

    const int col_h = (steps * bar_h) + ((steps - 1) * bar_gap);
    const int ap_x = right_edge + 1;
    const int ap_y0 = by + col_h + 2;

    canvas_set_font(canvas, FontKeyboard);
    if(is_am) canvas_draw_str(canvas, ap_x, ap_y0 + 7, "AM");
    if(is_pm) canvas_draw_str(canvas, ap_x, ap_y0 + 15, "PM");
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

    // Input events sent from ViewPort callback to this thread.
    app.q = furi_message_queue_alloc(8, sizeof(InputEvent));

    // Create fullscreen ViewPort and attach draw + input callbacks.
    app.vp = view_port_alloc();
    view_port_draw_callback_set(app.vp, draw_cb, NULL);
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

        // Baseline: ignore all other inputs.
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
    notification_message(app.notif, &sequence_reset_display);
    furi_record_close(RECORD_NOTIFICATION);

    return 0;
}
