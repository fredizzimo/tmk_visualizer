/*
The MIT License (MIT)

Copyright (c) 2016 Fred Sundvik

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "visualizer.h"
#include "ch.h"
#include <string.h>
#include "chschd.h"

#ifdef LCD_ENABLE
#include "gfx.h"
#endif

#ifdef LCD_BACKLIGHT_ENABLE
#include "lcd_backlight.h"
#endif

#define DEBUG_VISUALIZER

#ifdef DEBUG_VISUALIZER
#include "debug.h"
#else
#include "nodebug.h"
#endif


static visualizer_keyboard_status_t current_status = {
    .layer = 0xFFFFFFFF,
    .default_layer = 0xFFFFFFFF,
    .leds = 0xFFFFFFFF,
};

static bool same_status(visualizer_keyboard_status_t* status1, visualizer_keyboard_status_t* status2) {
    return memcmp(status1, status2, sizeof(visualizer_keyboard_status_t)) == 0;
}

static event_source_t layer_changed_event;
static bool visualizer_enabled = false;

#define MAX_SIMULTANEOUS_ANIMATIONS 4
static keyframe_animation_t* animations[MAX_SIMULTANEOUS_ANIMATIONS] = {};

void start_keyframe_animation(keyframe_animation_t* animation) {
    animation->current_frame = -1;
    animation->time_left_in_frame = 0;
    animation->need_update = true;
    int free_index = -1;
    for (int i=0;i<MAX_SIMULTANEOUS_ANIMATIONS;i++) {
        if (animations[i] == animation) {
            return;
        }
        if (free_index == -1 && animations[i] == NULL) {
           free_index=i;
        }
    }
    if (free_index!=-1) {
        animations[free_index] = animation;
    }
}

void stop_keyframe_animation(keyframe_animation_t* animation) {
    animation->current_frame = animation->num_frames;
    animation->time_left_in_frame = 0;
    animation->need_update = true;
    for (int i=0;i<MAX_SIMULTANEOUS_ANIMATIONS;i++) {
        if (animations[i] == animation) {
            animations[i] = NULL;
            return;
        }
    }
}

static bool update_keyframe_animation(keyframe_animation_t* animation, visualizer_state_t* state, systime_t delta, systime_t* sleep_time) {
    dprintf("Animation frame%d, left %d, delta %d\n", animation->current_frame,
            animation->time_left_in_frame, delta);
    if (animation->current_frame == animation->num_frames) {
        animation->need_update = false;
        return false;
    }
    if (animation->current_frame == -1) {
       animation->current_frame = 0;
       animation->time_left_in_frame = animation->frame_lengths[0];
       animation->need_update = true;
    } else {
        animation->time_left_in_frame -= delta;
        while (animation->time_left_in_frame <= 0) {
            int left = animation->time_left_in_frame;
            if (animation->need_update) {
                animation->time_left_in_frame = 0;
                (*animation->frame_functions[animation->current_frame])(animation, state);
            }
            animation->current_frame++;
            animation->need_update = true;
            if (animation->current_frame == animation->num_frames) {
                if (animation->loop) {
                    animation->current_frame = 0;
                }
                else {
                    stop_keyframe_animation(animation);
                    return false;
                }
            }
            delta = -left;
            animation->time_left_in_frame = animation->frame_lengths[animation->current_frame];
            animation->time_left_in_frame -= delta;
        }
    }
    if (animation->need_update) {
        animation->need_update = (*animation->frame_functions[animation->current_frame])(animation, state);
    }

    systime_t wanted_sleep = animation->need_update ? MS2ST(10) : (systime_t)animation->time_left_in_frame;
    if (wanted_sleep < *sleep_time) {
        *sleep_time = wanted_sleep;
    }

    return true;
}

bool keyframe_no_operation(keyframe_animation_t* animation, visualizer_state_t* state) {
    (void)animation;
    (void)state;
    return false;
}

#ifdef LCD_BACKLIGHT_ENABLE
bool keyframe_animate_backlight_color(keyframe_animation_t* animation, visualizer_state_t* state) {
    int frame_length = animation->frame_lengths[1];
    int current_pos = frame_length - animation->time_left_in_frame;
    uint8_t t_h = LCD_HUE(state->target_lcd_color);
    uint8_t t_s = LCD_SAT(state->target_lcd_color);
    uint8_t t_i = LCD_INT(state->target_lcd_color);
    uint8_t p_h = LCD_HUE(state->prev_lcd_color);
    uint8_t p_s = LCD_SAT(state->prev_lcd_color);
    uint8_t p_i = LCD_INT(state->prev_lcd_color);

    uint8_t d_h1 = t_h - p_h; //Modulo arithmetic since we want to wrap around
    int d_h2 = t_h - p_h;
    // Chose the shortest way around
    int d_h = abs(d_h2) < d_h1 ? d_h2 : d_h1;
    int d_s = t_s - p_s;
    int d_i = t_i - p_i;

    int hue = (d_h * current_pos) / frame_length;
    int sat = (d_s * current_pos) / frame_length;
    int intensity = (d_i * current_pos) / frame_length;
    //dprintf("%X -> %X = %X\n", p_h, t_h, hue);
    hue += p_h;
    sat += p_s;
    intensity += p_i;
    state->current_lcd_color = LCD_COLOR(hue, sat, intensity);
    lcd_backlight_color(
            LCD_HUE(state->current_lcd_color),
            LCD_SAT(state->current_lcd_color),
            LCD_INT(state->current_lcd_color));

    return true;
}

bool keyframe_set_backlight_color(keyframe_animation_t* animation, visualizer_state_t* state) {
    (void)animation;
    state->prev_lcd_color = state->target_lcd_color;
    state->current_lcd_color = state->target_lcd_color;
    lcd_backlight_color(
            LCD_HUE(state->current_lcd_color),
            LCD_SAT(state->current_lcd_color),
            LCD_INT(state->current_lcd_color));
    return false;
}
#endif // LCD_BACKLIGHT_ENABLE

#ifdef LCD_ENABLE
bool keyframe_display_layer_text(keyframe_animation_t* animation, visualizer_state_t* state) {
    (void)animation;
    //ucnt_t ctxtBefore = ch.kernel_stats.n_ctxswc;
	systime_t beforeClear = chVTGetSystemTimeX();
	systime_t beforeClearOwn = chThdGetTicksX(chThdGetSelfX());
    gdispClear(White);
	systime_t beforeDrawString = chVTGetSystemTimeX();
	systime_t beforeDrawStringOwn = chThdGetTicksX(chThdGetSelfX());
    gdispDrawString(0, 10, state->layer_text, state->font_dejavusansbold12, Black);
	systime_t beforeFlush = chVTGetSystemTimeX();
	systime_t beforeFlushOwn = chThdGetTicksX(chThdGetSelfX());
    gdispFlush();
	systime_t afterFlush = chVTGetSystemTimeX();
	systime_t afterFlushOwn = chThdGetTicksX(chThdGetSelfX());
    //ucnt_t ctxtAfter = ch.kernel_stats.n_ctxswc;
	xprintf("display_layer_text clear %d, drawString %d, flush %d\n", beforeDrawString - beforeClear, beforeFlush - beforeDrawString, afterFlush - beforeFlush);
	xprintf("own times clear %d, drawString %d, flush %d\n", beforeDrawStringOwn - beforeClearOwn, beforeFlushOwn - beforeDrawStringOwn, afterFlushOwn - beforeFlushOwn);
	//xprintf("Context switches %d\n", ctxtAfter - ctxtBefore);
    return false;
}

static void format_layer_bitmap_string(uint16_t default_layer, uint16_t layer, char* buffer) {
    for (int i=0; i<16;i++)
    {
        uint32_t mask = (1u << i);
        if (default_layer & mask) {
            if (layer & mask) {
                *buffer = 'B';
            } else {
                *buffer = 'D';
            }
        } else if (layer & mask) {
            *buffer = '1';
        } else {
            *buffer = '0';
        }
        ++buffer;

        if (i==3 || i==7 || i==11) {
            *buffer = ' ';
            ++buffer;
        }
    }
    *buffer = 0;
}

bool keyframe_display_layer_bitmap(keyframe_animation_t* animation, visualizer_state_t* state) {
    (void)animation;
    const char* layer_help = "1=On D=Default B=Both";
    char layer_buffer[16 + 4]; // 3 spaces and one null terminator
    gdispClear(White);
    gdispDrawString(0, 0, layer_help, state->font_fixed5x8, Black);
    format_layer_bitmap_string(state->status.default_layer, state->status.layer, layer_buffer);
    gdispDrawString(0, 10, layer_buffer, state->font_fixed5x8, Black);
    format_layer_bitmap_string(state->status.default_layer >> 16, state->status.layer >> 16, layer_buffer);
    gdispDrawString(0, 20, layer_buffer, state->font_fixed5x8, Black);
    gdispFlush();
    return false;
}
#endif // LCD_ENABLE

bool user_visualizer_inited(keyframe_animation_t* animation, visualizer_state_t* state) {
    (void)animation;
    (void)state;
    dprint("User visualizer inited\n");
    visualizer_enabled = true;
    return false;
}

// TODO: Optimize the stack size, this is probably way too big
static THD_WORKING_AREA(visualizerThreadStack, 1024);
static THD_FUNCTION(visualizerThread, arg) {
    (void)arg;

    event_listener_t event_listener;
    chEvtRegister(&layer_changed_event, &event_listener, 0);

    visualizer_state_t state = {
        .status = {
            .default_layer = 0xFFFFFFFF,
            .layer = 0xFFFFFFFF,
            .leds = 0xFFFFFFFF,
        },

        .current_lcd_color = 0,
#ifdef LCD_ENABLE
        .font_fixed5x8 = gdispOpenFont("fixed_5x8"),
        .font_dejavusansbold12 = gdispOpenFont("DejaVuSansBold12")
#endif
    };
    initialize_user_visualizer(&state);
    state.prev_lcd_color = state.current_lcd_color;

#ifdef LCD_BACKLIGHT_ENABLE
    lcd_backlight_color(
            LCD_HUE(state.current_lcd_color),
            LCD_SAT(state.current_lcd_color),
            LCD_INT(state.current_lcd_color));
#endif

    systime_t sleep_time = TIME_INFINITE;
    systime_t current_time = chVTGetSystemTimeX();

    while(true) {
        systime_t new_time = chVTGetSystemTimeX();
        systime_t delta = new_time - current_time;
        current_time = new_time;
        bool enabled = visualizer_enabled;
        if (!same_status(&state.status, &current_status)) {
            if (visualizer_enabled) {
                state.status = current_status;
                update_user_visualizer_state(&state);
                state.prev_lcd_color = state.current_lcd_color;
            }
        }
        sleep_time = TIME_INFINITE;
        for (int i=0;i<MAX_SIMULTANEOUS_ANIMATIONS;i++) {
            if (animations[i]) {
                update_keyframe_animation(animations[i], &state, delta, &sleep_time);
            }
        }
        if (enabled != visualizer_enabled) {
            sleep_time = 0;
        }

        systime_t after_update = chVTGetSystemTimeX();
        unsigned update_delta = after_update - current_time;
        if (sleep_time != TIME_INFINITE) {
            if (sleep_time > update_delta) {
                sleep_time -= update_delta;
            }
            else {
                sleep_time = 0;
            }
        }
        dprintf("Update took %d, last delta %d, sleep_time %d\n", update_delta, delta, sleep_time);
        chEvtWaitOneTimeout(EVENT_MASK(0), sleep_time);
    }
#ifdef LCD_ENABLE
    gdispCloseFont(state.font_fixed5x8);
    gdispCloseFont(state.font_dejavusansbold12);
#endif
}

void visualizer_init(void) {
#ifdef LCD_ENABLE
    gfxInit();
#endif

#ifdef LCD_BACKLIGHT_ENABLE
    lcd_backlight_init();
#endif
    // We are using a low priority thread, the idea is to have it run only
    // when the main thread is sleeping during the matrix scanning
    chEvtObjectInit(&layer_changed_event);
    (void)chThdCreateStatic(visualizerThreadStack, sizeof(visualizerThreadStack),
                              LOWPRIO, visualizerThread, NULL);
}

void visualizer_set_state(uint32_t default_state, uint32_t state, uint32_t leds) {
    // Note that there's a small race condition here, the thread could read
    // a state where one of these are set but not the other. But this should
    // not really matter as it will be fixed during the next loop step.
    // Alternatively a mutex could be used instead of the volatile variables
    bool changed = false;
    visualizer_keyboard_status_t new_status = {
        .layer = state,
        .default_layer = default_state,
        .leds = leds,
    };
    if (!same_status(&current_status, &new_status)) {
        changed = true;
    }
    current_status = new_status;
    if (changed) {
        chEvtBroadcast(&layer_changed_event);
    }
}
