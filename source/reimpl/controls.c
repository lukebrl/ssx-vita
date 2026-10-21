/*
 * Copyright (C) 2025 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/controls.h"
#include "ssx/overlay.h"

#include <math.h>
#include <psp2/ctrl.h>
#include <psp2/motion.h>
#include <psp2/touch.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>

#define LEFT_ANALOG_DEADZONE  0.16f
#define RIGHT_ANALOG_DEADZONE 0.16f


void coord_normalize(float * x, float * y, float deadzone) {
    float magnitude = sqrtf((*x * *x) + (*y * *y));
    if (magnitude < deadzone) {
        *x = 0;
        *y = 0;
        return;
    }

    // normalize
    *x = *x / magnitude;
    *y = *y / magnitude;

    float multiplier = ((magnitude - deadzone) / (1 - deadzone));
    *x = *x * multiplier;
    *y = *y * multiplier;
}

static int is_pstv;

void controls_init() {
    // Enable analog sticks and touchscreen
    sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG_WIDE);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, 1);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, 1);
    is_pstv = sceKernelIsPSVitaTV();

    // Enable accelerometer
    sceMotionStartSampling();
}

void poll_touch();
void poll_pad();
void poll_accel();

void poll_stick(ControlsStickId which, float raw_x, float raw_y, float * readings_x, float * readings_y, float deadzone);

void controls_poll() {
    poll_touch();
    poll_pad();
    //poll_accel();
}

SceTouchData touch;
SceTouchData touch_old;

void poll_touch() {
    sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);

    for (int i = 0; i < touch.reportNum; i++) {
        float x = (float) touch.report[i].x * 960.f / 1920.0f;
        float y = (float) touch.report[i].y * 544.f / 1088.0f;

        // Check if the finger was down before to distinguish between the Move and Down events
        int finger_down = 0;

        if (touch_old.reportNum > 0) {
            for (int j = 0; j < touch_old.reportNum; j++) {
                if (touch.report[i].id == touch_old.report[j].id) {
                    finger_down = 1;
                    break;
                }
            }
        }

        if (!finger_down) {
            controls_handler_touch(touch.report[i].id, x, y, CONTROLS_ACTION_DOWN);
        } else {
            controls_handler_touch(touch.report[i].id, x, y, CONTROLS_ACTION_MOVE);
        }
    }

    for (int i = 0; i < touch_old.reportNum; i++) {
        int finger_up = 1;

        for (int j = 0; j < touch.reportNum; j++) {
            if (touch.report[j].id == touch_old.report[i].id ) {
                finger_up = 0;
                break;
            }
        }

        if (finger_up == 1) {
            float x = (float) touch_old.report[i].x * 960.f / 1920.0f;
            float y = (float) touch_old.report[i].y * 544.f / 1088.0f;

            controls_handler_touch(touch_old.report[i].id, x, y, CONTROLS_ACTION_UP);
        }
    }

    sceClibMemcpy(&touch_old, &touch, sizeof(touch));
}

static ButtonMapping mapping[] = {
        { SCE_CTRL_UP,        AKEYCODE_DPAD_UP },
        { SCE_CTRL_DOWN,      AKEYCODE_DPAD_DOWN },
        { SCE_CTRL_LEFT,      AKEYCODE_DPAD_LEFT },
        { SCE_CTRL_RIGHT,     AKEYCODE_DPAD_RIGHT },
        // TouchController::NIControllerKeyToSSXKey turns the face buttons into
        // SSX keys by position (left, top, right, bottom, like its D-pad
        // keys): BUTTON_A is the left one, B the top, Y the right, X the bottom
        { SCE_CTRL_CROSS,     AKEYCODE_BUTTON_X },
        { SCE_CTRL_CIRCLE,    AKEYCODE_BUTTON_Y },
        { SCE_CTRL_SQUARE,    AKEYCODE_BUTTON_A },
        { SCE_CTRL_TRIANGLE,  AKEYCODE_BUTTON_B },
        { SCE_CTRL_L1,        AKEYCODE_BUTTON_L1 },
        { SCE_CTRL_R1,        AKEYCODE_BUTTON_R1 },
        { SCE_CTRL_LTRIGGER,  AKEYCODE_BUTTON_L2 },
        { SCE_CTRL_RTRIGGER,  AKEYCODE_BUTTON_R2 },
        { SCE_CTRL_L3,        AKEYCODE_BUTTON_THUMBL },
        { SCE_CTRL_R3,        AKEYCODE_BUTTON_THUMBR },
        { SCE_CTRL_START,     AKEYCODE_BUTTON_START },
        { SCE_CTRL_SELECT,    AKEYCODE_BUTTON_SELECT },
};

/*
 * Shoulder buttons. The game uses all four (L1/R1/L2/R2: grabs in the air,
 * rewind and boost on the ground). A DualShock on a PS TV reports them as
 * L1/R1 and LTRIGGER/RTRIGGER (= L2/R2), which the table above maps as is.
 * The handheld only has L and R, which it reports as LTRIGGER/RTRIGGER: those
 * become L1/R1 here, and L2/R2 come from the rear touchpad (left half / right
 * half), set as LTRIGGER/RTRIGGER below.
 */

static uint32_t handheld_buttons(uint32_t buttons) {
    uint32_t out = buttons & ~(SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER);
    if (buttons & SCE_CTRL_LTRIGGER) out |= SCE_CTRL_L1;
    if (buttons & SCE_CTRL_RTRIGGER) out |= SCE_CTRL_R1;

    SceTouchData rear;
    if (sceTouchPeek(SCE_TOUCH_PORT_BACK, &rear, 1) >= 0) {
        for (int i = 0; i < rear.reportNum; i++)
            out |= rear.report[i].x < 960 ? SCE_CTRL_LTRIGGER : SCE_CTRL_RTRIGGER;
    }
    return out;
}

uint32_t old_buttons = 0, current_buttons = 0, pressed_buttons = 0, released_buttons = 0;

float analog_lx[3] = { 0 };
float analog_ly[3] = { 0 };
float analog_rx[3] = { 0 };
float analog_ry[3] = { 0 };

void poll_pad() {
    SceCtrlData pad;
    sceCtrlPeekBufferPositiveExt2(0, &pad, 1);

    // Gamepad buttons
    uint32_t raw = is_pstv ? pad.buttons : handheld_buttons(pad.buttons);

    // Holding Select, pressing Start opens or closes the port's settings menu
    // (ssx/overlay.c). While it is open the game sees no button and centred
    // sticks; the buttons held when it closes (the Start of the shortcut,
    // Circle) stay the menu's until they are let go, so the game doesn't
    // take them (Start would open its pause menu).
    static uint32_t raw_old, blocked;
    static int was_open;
    uint32_t raw_pressed = raw & ~raw_old;
    raw_old = raw;
    if ((raw & SCE_CTRL_SELECT) && (raw_pressed & SCE_CTRL_START))
        overlay_menu_toggle();
    else if (overlay_menu_is_open())
        overlay_menu_input(raw_pressed);
    int menu = overlay_menu_is_open();
    if (was_open && !menu)
        blocked |= raw;
    was_open = menu;
    blocked &= raw;

    old_buttons = current_buttons;
    current_buttons = menu ? 0 : raw & ~blocked;
    pressed_buttons = current_buttons & ~old_buttons;
    released_buttons = ~current_buttons & old_buttons;

    for (int i = 0; i < sizeof(mapping) / sizeof(ButtonMapping); i++) {
        if (pressed_buttons & mapping[i].sce_button) {
            controls_handler_key(mapping[i].android_button, CONTROLS_ACTION_DOWN);
        }
        if (released_buttons & mapping[i].sce_button) {
            controls_handler_key(mapping[i].android_button, CONTROLS_ACTION_UP);
        }
    }

    // Analog sticks (centred for the game while the menu is open)
    if (menu)
        pad.lx = pad.ly = pad.rx = pad.ry = 128;
    poll_stick(CONTROLS_STICK_LEFT, (float)pad.lx, (float)pad.ly, analog_lx, analog_ly, LEFT_ANALOG_DEADZONE);
    poll_stick(CONTROLS_STICK_RIGHT, (float)pad.rx, (float)pad.ry, analog_rx, analog_ry, RIGHT_ANALOG_DEADZONE);
}

void poll_stick(ControlsStickId which, float raw_x, float raw_y, float * readings_x, float * readings_y, float deadzone) {
    readings_x[0] = (raw_x - 128.0f) / 128.0f;
    readings_y[0] = (raw_y - 128.0f) / 128.0f;

    coord_normalize(&readings_x[0], &readings_y[0], deadzone);

    // Last two readings are 0, the one before that isn't ==> MOTION_ACTION_UP
    if (
        (readings_x[0] == 0.f && readings_y[0] == 0.f) &&
        (readings_x[1] == 0.f && readings_y[1] == 0.f) &&
        (readings_x[2] != 0.f || readings_y[2] != 0.f)
    ) {
        controls_handler_analog(which, readings_x[0], readings_y[0], CONTROLS_ACTION_UP);
    }
    // Current reading isn't 0, the two before are ==> MOTION_ACTION_DOWN
    else if (
        (readings_x[0] != 0.f || readings_y[0] != 0.f) &&
        (readings_x[1] == 0.f && readings_y[1] == 0.f) &&
        (readings_x[2] == 0.f && readings_y[2] == 0.f)
    ) {
        controls_handler_analog(which, readings_x[0], readings_y[0], CONTROLS_ACTION_DOWN);
    }
    // Other cases ==> MOTION_ACTION_MOVE
    else {
        controls_handler_analog(which, readings_x[0], readings_y[0], CONTROLS_ACTION_MOVE);
    }

    readings_x[2] = readings_x[1];
    readings_y[2] = readings_y[1];
    readings_x[1] = readings_x[0];
    readings_y[1] = readings_y[0];
}
