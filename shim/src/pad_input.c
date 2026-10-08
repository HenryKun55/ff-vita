/* Pad controls (see pad_input.h).
 *
 * First version: every control presses a fixed key of the game's default PC
 * bindings. The menus take the arrows, Enter and Esc. */
#include "pad_input.h"
#include "platform.h"
#include "runtime.h"

#define DIK_ESCAPE 0x01
#define DIK_E 0x12
#define DIK_R 0x13
#define DIK_RETURN 0x1C
#define DIK_A 0x1E
#define DIK_S 0x1F
#define DIK_D 0x20
#define DIK_W 0x11
#define DIK_C 0x2E
#define DIK_SPACE 0x39
#define DIK_TAB 0x0F
#define DIK_UP 0xC8
#define DIK_DOWN 0xD0
#define DIK_LEFT 0xCB
#define DIK_RIGHT 0xCD

static const uint8_t control_key[PAD_CONTROL_COUNT] = {
    [PAD_CROSS] = DIK_RETURN,
    [PAD_CIRCLE] = DIK_ESCAPE,
    [PAD_SQUARE] = DIK_E,
    [PAD_TRIANGLE] = DIK_R,
    [PAD_L] = DIK_C,
    [PAD_R] = DIK_SPACE,
    [PAD_SELECT] = DIK_TAB,
    [PAD_START] = DIK_ESCAPE,
    [PAD_DPAD_UP] = DIK_UP,
    [PAD_DPAD_DOWN] = DIK_DOWN,
    [PAD_DPAD_LEFT] = DIK_LEFT,
    [PAD_DPAD_RIGHT] = DIK_RIGHT,
    [PAD_LSTICK_UP] = DIK_W,
    [PAD_LSTICK_DOWN] = DIK_S,
    [PAD_LSTICK_LEFT] = DIK_A,
    [PAD_LSTICK_RIGHT] = DIK_D,
};

/* ---- output: keys and mouse buttons, reference counted -------------------- */

static uint8_t key_refs[256];
static uint8_t mouse_refs[4];

void pad_key(uint8_t dik, int down)
{
    if (!dik)
        return;
    if (down) {
        if (key_refs[dik]++ == 0u && !g_key_state[dik])
            platform_key(dik, 1);
    } else if (key_refs[dik] && --key_refs[dik] == 0u && g_key_state[dik]) {
        platform_key(dik, 0);
    }
}

void pad_mouse_button(int b, int down)
{
    if (down ? mouse_refs[b]++ != 0u : (!mouse_refs[b] || --mouse_refs[b] != 0u))
        return;
    g_mouse.buttons[b] = down ? 0x80 : 0;
    if (b < 3) {
        static const uint32_t msgs[3][2] = { { 0x0202, 0x0201 }, { 0x0205, 0x0204 }, { 0x0208, 0x0207 } };
        user32_post_message(msgs[b][down ? 1 : 0], 0, ((uint32_t)g_mouse.y << 16) | ((uint32_t)g_mouse.x & 0xFFFFu));
    }
}

int pad_in_gameplay(void)
{
    return 0;
}

int pad_capturing_key(void)
{
    return 0;
}

void pad_control(PadControl control, int down)
{
    if ((unsigned)control < PAD_CONTROL_COUNT)
        pad_key(control_key[control], down);
}

void pad_tick(void)
{
}
