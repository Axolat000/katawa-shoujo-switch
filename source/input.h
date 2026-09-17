#pragma once
#include <string>

// Per-frame input, in virtual 800x600 coordinates.
struct Input {
    bool accept = false;   // A / Enter / Space / left click on background
    bool cancel = false;   // B / Escape
    bool menu = false;     // + / Escape in game / right click
    bool hide = false;     // X / H / middle click
    bool history = false;  // Y / T
    bool rollback = false; // L / PageUp / wheel up
    bool rollforward = false;
    bool skipToggle = false; // R / Tab
    bool skipHeld = false;   // ZR / Ctrl held
    bool autoToggle = false; // ZL / A key
    bool up = false, down = false, left = false, right = false; // navigation presses (repeat)
    bool pointerPressed = false, pointerReleased = false, pointerDown = false, pointerMoved = false;
    float px = -1, py = -1;
    float scroll = 0; // >0 down
    bool quit = false;
    bool any() const {
        return accept || cancel || menu || hide || history || rollback || rollforward || skipToggle || autoToggle ||
               up || down || left || right || pointerPressed || pointerReleased;
    }
};

extern Input g_input;
