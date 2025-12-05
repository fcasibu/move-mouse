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

#define v2f_zero() ((v2f_t){ 0, 0 })

const f32 START_SPEED = 20.0F;
const f32 MAX_SPEED = 1200.0F;
const f32 RAMP_TIME = 0.7F;
const f32 INPUT_DEADZONE = 0.05F;
const i32 SCROLL_LINES_PER_TICK = 3;
const i64 TARGET_FRAME_NS = 16666667;

v2f_t pos = v2f_zero();
bool input_up = false, input_down = false, input_left = false, input_right = false;
f32 key_hold_time = 0.0F;
bool is_intercepting = false;
pthread_t movement_thread = 0;
CGFloat screen_width, screen_height;

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

void *movement_loop(void *arg)
{
    (void)arg;

    struct timespec start_time;
    struct timespec end_time;
    struct timespec sleep_time;

    while (true) {
        clock_gettime(CLOCK_MONOTONIC, &start_time);

        f32 dt = (float)TARGET_FRAME_NS / 1e9F;
        v2f_t raw_dir = v2f_zero();

        if (input_up)
            raw_dir.y -= 1.0F;
        if (input_down)
            raw_dir.y += 1.0F;
        if (input_left)
            raw_dir.x -= 1.0F;
        if (input_right)
            raw_dir.x += 1.0F;
        v2f_t move_dir = v2f_normalize(raw_dir);

        bool moving = (move_dir.x != 0 || move_dir.y != 0);

        // TODO(fcasibu): do we want deceleration
        if (moving) {
            key_hold_time += dt;

            if (key_hold_time >= INPUT_DEADZONE) {
                f32 hold_time = key_hold_time - INPUT_DEADZONE;
                f32 current_speed =
                    START_SPEED + (MAX_SPEED - START_SPEED) * fminf(1.0F, hold_time / RAMP_TIME);
                v2f_t velocity = v2f_scale(move_dir, current_speed);
                pos = v2f_clamp(v2f_add(pos, v2f_scale(velocity, dt)), 0.0F, (f32)screen_width,
                                0.0F, (f32)screen_height);

                CGEventRef move = CGEventCreateMouseEvent(
                    NULL, kCGEventMouseMoved, CGPointMake(pos.x, pos.y), kCGMouseButtonLeft);
                CGEventPost(kCGHIDEventTap, move);
                CFRelease(move);
            }
        } else {
            key_hold_time = 0.0F;
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
    (void)user_info;

    if (type == kCGEventKeyDown || type == kCGEventKeyUp) {
        CGKeyCode keycode = (CGKeyCode)CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);
        CGEventFlags flags = CGEventGetFlags(event);

        if (keycode == kVK_Space && (flags & kCGEventFlagMaskAlternate)) {
            if (type == kCGEventKeyDown) {
                is_intercepting = !is_intercepting;
                printf("Intercepting mode: %s\n", is_intercepting ? "ON" : "OFF");
                if (is_intercepting && movement_thread == 0) {
                    int ret = pthread_create(&movement_thread, NULL, movement_loop, NULL);
                    if (ret != 0) {
                        fprintf(stderr, "Failed to create thread: %s\n", strerror(ret));
                        is_intercepting = false;
                    }
                } else if (!is_intercepting && movement_thread != 0) {
                    int ret = pthread_cancel(movement_thread);
                    if (ret != 0)
                        fprintf(stderr, "Failed to cacel thread: %s\n", strerror(ret));

                    ret = pthread_join(movement_thread, NULL);
                    if (ret != 0) {
                        fprintf(stderr, "Failed to join thread:%s\n", strerror(ret));
                    }

                    movement_thread = 0;
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

        if (is_intercepting) {
            bool is_down = (type == kCGEventKeyDown);
            switch (keycode) {
            // movement keys
            case kVK_ANSI_I: {
                input_up = is_down;
                return NULL;
            }
            case kVK_ANSI_K: {
                input_down = is_down;
                return NULL;
            }
            case kVK_ANSI_J: {
                input_left = is_down;
                return NULL;
            }
            case kVK_ANSI_L: {
                input_right = is_down;
                return NULL;
            }

            // mouse buttons
            case kVK_ANSI_Q: {
                CGEventRef click = CGEventCreateMouseEvent(
                    NULL, is_down ? kCGEventLeftMouseDown : kCGEventLeftMouseUp,
                    CGPointMake(pos.x, pos.y), kCGMouseButtonLeft);
                CGEventPost(kCGHIDEventTap, click);
                CFRelease(click);
                return NULL;
            }
            case kVK_ANSI_E: {
                CGEventRef click = CGEventCreateMouseEvent(
                    NULL, is_down ? kCGEventRightMouseDown : kCGEventRightMouseUp,
                    CGPointMake(pos.x, pos.y), kCGMouseButtonRight);
                CGEventPost(kCGHIDEventTap, click);
                CFRelease(click);
                return NULL;
            }

            // scrolling keys
            case kVK_ANSI_W: {
                if (is_down) {
                    CGEventRef scroll = CGEventCreateScrollWheelEvent(NULL, kCGScrollEventUnitLine,
                                                                      1, SCROLL_LINES_PER_TICK);
                    CGEventPost(kCGHIDEventTap, scroll);
                    CFRelease(scroll);
                }
                return NULL;
            }
            case kVK_ANSI_S: {
                if (is_down) {
                    CGEventRef scroll = CGEventCreateScrollWheelEvent(NULL, kCGScrollEventUnitLine,
                                                                      1, -SCROLL_LINES_PER_TICK);
                    CGEventPost(kCGHIDEventTap, scroll);
                    CFRelease(scroll);
                }
                return NULL;
            }

            default:
                return NULL;
            }
        }
    }

    return event;
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

    CGDirectDisplayID display = CGMainDisplayID();
    CGRect bounds = CGDisplayBounds(display);
    screen_width = CGRectGetWidth(bounds);
    screen_height = CGRectGetHeight(bounds);
    pos = (v2f_t){ (f32)(screen_width / 2), (f32)(screen_height / 2) };

    CGEventMask event_mask = (1 << kCGEventKeyDown) | (1 << kCGEventKeyUp);
    CFMachPortRef event_tap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap, 0,
                                               event_mask, event_tap_callback, NULL);

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

    if (movement_thread != 0) {
        int ret = pthread_cancel(movement_thread);
        if (ret != 0)
            fprintf(stderr, "Failed to cacel thread: %s\n", strerror(ret));

        ret = pthread_join(movement_thread, NULL);
        if (ret != 0)
            fprintf(stderr, "Failed to join thread: %s\n", strerror(ret));

        return ret;
    }

    return 0;
}
