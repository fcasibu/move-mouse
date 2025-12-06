#include <ApplicationServices/ApplicationServices.h>
#include <Carbon/Carbon.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <math.h>
#include <time.h>

typedef float f32;
typedef int32_t i32;
typedef int64_t i64;

typedef struct {
    f32 x;
    f32 y;
} v2f_t;

typedef struct {
    f32 width;
    f32 height;
} screen_info_t;

typedef struct {
    f32 start_speed;
    f32 max_speed;
    f32 ramp_time;
    i32 scroll_lines_per_tick;
    screen_info_t screen;
} app_config_t;

typedef struct {
    bool up;
    bool down;
    bool left;
    bool right;
    bool is_dragging;
} input_state_t;

typedef struct {
    pthread_t movement_thread;

    v2f_t cursor_pos;
    bool is_intercepting;
    input_state_t input;
    f32 key_hold_time;

    app_config_t config;
} context_t;

v2f_t v2f_add(v2f_t v1, v2f_t v2);
v2f_t v2f_scale(v2f_t v, f32 scalar);
f32 v2f_length(v2f_t v);
v2f_t v2f_normalize(v2f_t v);
v2f_t v2f_clamp(v2f_t v, float min_x, float max_x, float min_y, float max_y);

void *movement_loop(void *arg);
CGEventRef event_tap_callback(CGEventTapProxy proxy, CGEventType type, CGEventRef event,
                              void *user_info);
void run_mouse_event(v2f_t pos, CGEventType type, CGMouseButton button);
void run_scroll_wheel_event(i32 lines_per_tick);
context_t init_context(void);

#define v2f_zero() ((v2f_t){ 0, 0 })

const f32 INPUT_DEADZONE = 0.05F;
const i64 TARGET_FRAME_NS = 16666667;

v2f_t v2f_add(v2f_t v1, v2f_t v2)
{
    return (v2f_t){ v1.x + v2.x, v1.y + v2.y };
}

v2f_t v2f_scale(v2f_t v, f32 scalar)
{
    return (v2f_t){ v.x * scalar, v.y * scalar };
}

f32 v2f_length(v2f_t v)
{
    return sqrtf(v.x * v.x + v.y * v.y);
}

v2f_t v2f_normalize(v2f_t v)
{
    f32 len = v2f_length(v);
    if (len < 1e-6F)
        return v2f_zero();

    return (v2f_t){ v.x / len, v.y / len };
}

v2f_t v2f_clamp(v2f_t v, float min_x, float max_x, float min_y, float max_y)
{
    return (v2f_t){
        fmaxf(min_x, fminf(max_x, v.x)),
        fmaxf(min_y, fminf(max_y, v.y)),
    };
}

void run_mouse_event(v2f_t pos, CGEventType type, CGMouseButton button)
{
    CGEventRef event = CGEventCreateMouseEvent(NULL, type, CGPointMake(pos.x, pos.y), button);
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
}

void run_scroll_wheel_event(i32 lines_per_tick)
{
    CGEventRef event =
        CGEventCreateScrollWheelEvent(NULL, kCGScrollEventUnitLine, 1, lines_per_tick);
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
}

void *movement_loop(void *arg)
{
    context_t *ctx = (context_t *)arg;
    assert(ctx);

    struct timespec start_time;
    struct timespec end_time;
    struct timespec sleep_time;

    while (true) {
        clock_gettime(CLOCK_MONOTONIC, &start_time);

        f32 dt = (float)TARGET_FRAME_NS / 1e9F;
        v2f_t raw_dir = v2f_zero();

        if (ctx->input.up)
            raw_dir.y -= 1.0F;
        if (ctx->input.down)
            raw_dir.y += 1.0F;
        if (ctx->input.left)
            raw_dir.x -= 1.0F;
        if (ctx->input.right)
            raw_dir.x += 1.0F;
        v2f_t move_dir = v2f_normalize(raw_dir);

        bool moving = (move_dir.x != 0 || move_dir.y != 0);

        if (moving) {
            ctx->key_hold_time += dt;

            if (ctx->key_hold_time >= INPUT_DEADZONE) {
                f32 hold_time = ctx->key_hold_time - INPUT_DEADZONE;
                f32 current_speed =
                    ctx->config.start_speed + (ctx->config.max_speed - ctx->config.start_speed) *
                                                  fminf(1.0F, hold_time / ctx->config.ramp_time);

                v2f_t velocity = v2f_scale(move_dir, current_speed);
                ctx->cursor_pos = v2f_clamp(v2f_add(ctx->cursor_pos, v2f_scale(velocity, dt)), 0.0F,
                                            ctx->config.screen.width, 0.0F,
                                            ctx->config.screen.height);

                run_mouse_event(ctx->cursor_pos,
                                ctx->input.is_dragging ? kCGEventLeftMouseDragged :
                                                         kCGEventMouseMoved,
                                kCGMouseButtonLeft);
            }
        } else {
            ctx->key_hold_time = 0.0F;
        }

        clock_gettime(CLOCK_MONOTONIC, &end_time);
        i64 elapsed_ns = (end_time.tv_sec - start_time.tv_sec) * (i64)1e9 +
                         (end_time.tv_nsec - start_time.tv_nsec);

        i64 remaining_ns = TARGET_FRAME_NS - elapsed_ns;

        if (remaining_ns > 0) {
            sleep_time.tv_sec = 0;
            sleep_time.tv_nsec = remaining_ns;
            nanosleep(&sleep_time, NULL);
        }
    }

    return NULL;
}

CGEventRef event_tap_callback(CGEventTapProxy proxy, CGEventType type, CGEventRef event,
                              void *user_info)
{
    (void)proxy;

    context_t *ctx = (context_t *)user_info;
    assert(ctx);

    switch (type) {
    case kCGEventMouseMoved: {
        CGPoint point = CGEventGetLocation(event);

        ctx->cursor_pos = (v2f_t){ (f32)point.x, (f32)point.y };
        return event;
    }

    case kCGEventKeyDown:
    case kCGEventKeyUp: {
        CGKeyCode keycode = (CGKeyCode)CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);
        CGEventFlags flags = CGEventGetFlags(event);

        if (keycode == kVK_Space && (flags & kCGEventFlagMaskAlternate)) {
            if (type == kCGEventKeyDown) {
                ctx->is_intercepting = !ctx->is_intercepting;

                printf("Intercepting mode: %s\n", ctx->is_intercepting ? "ON" : "OFF");
                if (ctx->is_intercepting && ctx->movement_thread == 0) {
                    int ret = pthread_create(&ctx->movement_thread, NULL, movement_loop, ctx);
                    if (ret != 0) {
                        fprintf(stderr, "Failed to create thread: %s\n", strerror(ret));
                        ctx->is_intercepting = false;
                    }
                } else if (!ctx->is_intercepting && ctx->movement_thread != 0) {
                    int ret = pthread_cancel(ctx->movement_thread);
                    if (ret != 0)
                        fprintf(stderr, "Failed to cacel thread: %s\n", strerror(ret));

                    ret = pthread_join(ctx->movement_thread, NULL);
                    if (ret != 0) {
                        fprintf(stderr, "Failed to join thread:%s\n", strerror(ret));
                    }

                    ctx->movement_thread = 0;
                }
            }

            return NULL;
        }

        if (keycode == kVK_Escape && (flags & kCGEventFlagMaskAlternate)) {
            if (type == kCGEventKeyDown) {
                printf("Exiting...\n");
                CFRunLoopStop(CFRunLoopGetCurrent());
            }

            return NULL;
        }

        if (!ctx->is_intercepting)
            return event;

        bool is_down = (type == kCGEventKeyDown);
        switch (keycode) {
        // movement keys
        case kVK_ANSI_I: {
            ctx->input.up = is_down;
        } break;

        case kVK_ANSI_K: {
            ctx->input.down = is_down;
        } break;

        case kVK_ANSI_J: {
            ctx->input.left = is_down;
        } break;

        case kVK_ANSI_L: {
            ctx->input.right = is_down;
        } break;

        // mouse buttons
        case kVK_ANSI_Q: {
            run_mouse_event(ctx->cursor_pos, is_down ? kCGEventLeftMouseDown : kCGEventLeftMouseUp,
                            kCGMouseButtonLeft);

            ctx->input.is_dragging = is_down;

        } break;
        case kVK_ANSI_E: {
            run_mouse_event(ctx->cursor_pos,
                            is_down ? kCGEventRightMouseDown : kCGEventRightMouseUp,
                            kCGMouseButtonRight);

        } break;

        // scrolling keys
        case kVK_ANSI_W: {
            if (is_down)
                run_scroll_wheel_event(ctx->config.scroll_lines_per_tick);

        } break;

        case kVK_ANSI_S: {
            if (is_down)
                run_scroll_wheel_event(-ctx->config.scroll_lines_per_tick);

        } break;

        default:
            return event;
        }

        return NULL;
    }

    default:
        return event;
    }
}

context_t init_context(void)
{
    context_t ctx = { 0 };

    CGDirectDisplayID display = CGMainDisplayID();
    CGRect bounds = CGDisplayBounds(display);

    ctx.config.screen.width = (float)CGRectGetWidth(bounds);
    ctx.config.screen.height = (float)CGRectGetHeight(bounds);
    ctx.config.start_speed = 20.0F;
    ctx.config.max_speed = 1000.0F;
    ctx.config.ramp_time = 0.7F;
    ctx.config.scroll_lines_per_tick = 3;

    CGEventRef event = CGEventCreate(NULL);
    CGPoint cursor_loc = CGEventGetLocation(event);
    CFRelease(event);

    ctx.cursor_pos = (v2f_t){ (f32)cursor_loc.x, (f32)cursor_loc.y };

    return ctx;
}

int main(void)
{
    CFDictionaryRef options = CFDictionaryCreate(NULL,
                                                 (const void *[]){ kAXTrustedCheckOptionPrompt },
                                                 (const void *[]){ kCFBooleanTrue }, 1, NULL, NULL);

    if (!AXIsProcessTrustedWithOptions(options)) {
        fprintf(stderr, "ERROR: Accessibility permission required.\n");
        return 1;
    }

    context_t ctx = init_context();

    CGEventMask event_mask = EventCodeMask(kCGEventKeyDown) | EventCodeMask(kCGEventKeyUp) |
                             EventCodeMask(kCGEventMouseMoved);
    CFMachPortRef event_tap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap, 0,
                                               event_mask, event_tap_callback, &ctx);

    assert(event_tap && "No accessiblity permission");

    CFRunLoopSourceRef run_loop_source = CFMachPortCreateRunLoopSource(NULL, event_tap, 0);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), run_loop_source, kCFRunLoopCommonModes);
    CGEventTapEnable(event_tap, true);

    printf("Controls (while intercepted):\n");
    printf("  Move  :   I, J, K, L\n");
    printf("  Scroll:   W (up), S (down)\n");
    printf("  Click :   Q (left), E (right)\n");
    printf("\nPress Option+Space to toggle. Press Option+Esc to quit.\n");
    CFRunLoopRun();

    if (event_tap)
        CFRelease(event_tap);
    if (run_loop_source)
        CFRelease(run_loop_source);

    if (ctx.movement_thread != 0) {
        int ret = pthread_cancel(ctx.movement_thread);
        if (ret != 0)
            fprintf(stderr, "Failed to cacel thread: %s\n", strerror(ret));

        ret = pthread_join(ctx.movement_thread, NULL);
        if (ret != 0)
            fprintf(stderr, "Failed to join thread: %s\n", strerror(ret));

        return ret;
    }

    return 0;
}
