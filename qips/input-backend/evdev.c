/*
 * Copyright (c) 2013 Chris Patterson <cjp256@gmail.com>
 *
 * Many of these bits are adapted/referenced from evtest:
 *
 * Copyright (c) 1999-2000 Vojtech Pavlik
 * Copyright (c) 2009-2011 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <linux/input.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/time.h>
#include <termios.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>
#include <sys/inotify.h>
#include <glib.h>

#include "qapi/qmp/types.h"
#include "qemu/thread.h"
#include "qemu-common.h"
#include "input-backend.h"
#include "evdev.h"
#include "ui/x_keymap.h"

#define EVDEV_DPRINTF(msg, ...) do { \
    if (evdev_debug_mode) syslog(LOG_NOTICE, "%s():L%d: " msg, \
            __FUNCTION__, __LINE__, ## __VA_ARGS__); \
} while(0)

#define timestamp_usec(ts) (ts.tv_sec * 1000000 + ts.tv_usec)

// TODO: move key shortcut to input-backend
static pthread_t evdev_inotify_thread;

#define NAME_ELEMENT(element) [element] = #element

static const uint8_t evdev_keycode_to_pc_keycode[KEY_MAX] = {
    [0 ... KEY_MAX - 1] = 0,
    [KEY_RESERVED] = KEY_RESERVED,
    [KEY_ESC] = KEY_ESC,
    [KEY_1] = KEY_1,
    [KEY_2] = KEY_2,
    [KEY_3] = KEY_3,
    [KEY_4] = KEY_4,
    [KEY_5] = KEY_5,
    [KEY_6] = KEY_6,
    [KEY_7] = KEY_7,
    [KEY_8] = KEY_8,
    [KEY_9] = KEY_9,
    [KEY_0] = KEY_0,
    [KEY_MINUS] = KEY_MINUS,
    [KEY_EQUAL] = KEY_EQUAL,
    [KEY_BACKSPACE] = KEY_BACKSPACE,
    [KEY_TAB] = KEY_TAB,
    [KEY_Q] = KEY_Q,
    [KEY_W] = KEY_W,
    [KEY_E] = KEY_E,
    [KEY_R] = KEY_R,
    [KEY_T] = KEY_T,
    [KEY_Y] = KEY_Y,
    [KEY_U] = KEY_U,
    [KEY_I] = KEY_I,
    [KEY_O] = KEY_O,
    [KEY_P] = KEY_P,
    [KEY_LEFTBRACE] = KEY_LEFTBRACE,
    [KEY_RIGHTBRACE] = KEY_RIGHTBRACE,
    [KEY_ENTER] = KEY_ENTER,
    [KEY_LEFTCTRL] = KEY_LEFTCTRL,
    [KEY_A] = KEY_A,
    [KEY_S] = KEY_S,
    [KEY_D] = KEY_D,
    [KEY_F] = KEY_F,
    [KEY_G] = KEY_G,
    [KEY_H] = KEY_H,
    [KEY_J] = KEY_J,
    [KEY_K] = KEY_K,
    [KEY_L] = KEY_L,
    [KEY_SEMICOLON] = KEY_SEMICOLON,
    [KEY_APOSTROPHE] = KEY_APOSTROPHE,
    [KEY_GRAVE] = KEY_GRAVE,
    [KEY_LEFTSHIFT] = KEY_LEFTSHIFT,
    [KEY_BACKSLASH] = KEY_BACKSLASH,
    [KEY_Z] = KEY_Z,
    [KEY_X] = KEY_X,
    [KEY_C] = KEY_C,
    [KEY_V] = KEY_V,
    [KEY_B] = KEY_B,
    [KEY_N] = KEY_N,
    [KEY_M] = KEY_M,
    [KEY_COMMA] = KEY_COMMA,
    [KEY_DOT] = KEY_DOT,
    [KEY_SLASH] = KEY_SLASH,
    [KEY_RIGHTSHIFT] = KEY_RIGHTSHIFT,
    [KEY_KPASTERISK] = KEY_KPASTERISK,
    [KEY_LEFTALT] = KEY_LEFTALT,
    [KEY_SPACE] = KEY_SPACE,
    [KEY_CAPSLOCK] = KEY_CAPSLOCK,
    [KEY_F1] = KEY_F1,
    [KEY_F2] = KEY_F2,
    [KEY_F3] = KEY_F3,
    [KEY_F4] = KEY_F4,
    [KEY_F5] = KEY_F5,
    [KEY_F6] = KEY_F6,
    [KEY_F7] = KEY_F7,
    [KEY_F8] = KEY_F8,
    [KEY_F9] = KEY_F9,
    [KEY_F10] = KEY_F10,
    [KEY_NUMLOCK] = KEY_NUMLOCK,
    [KEY_SCROLLLOCK] = KEY_SCROLLLOCK,
    [KEY_KP7] = KEY_KP7,
    [KEY_KP8] = KEY_KP8,
    [KEY_KP9] = KEY_KP9,
    [KEY_KPMINUS] = KEY_KPMINUS,
    [KEY_KP4] = KEY_KP4,
    [KEY_KP5] = KEY_KP5,
    [KEY_KP6] = KEY_KP6,
    [KEY_KPPLUS] = KEY_KPPLUS,
    [KEY_KP1] = KEY_KP1,
    [KEY_KP2] = KEY_KP2,
    [KEY_KP3] = KEY_KP3,
    [KEY_KP0] = KEY_KP0,
    [KEY_KPDOT] = KEY_KPDOT,
    [KEY_ZENKAKUHANKAKU] = KEY_ZENKAKUHANKAKU,
    [KEY_102ND] = KEY_102ND,
    [KEY_F11] = KEY_F11,
    [KEY_F12] = KEY_F12,
    [KEY_RO] = 0,
    [KEY_KATAKANA] = 0,
    [KEY_HIRAGANA] = 0,
    [KEY_HENKAN] = 0x79,
    [KEY_KATAKANAHIRAGANA] = 0x70,
    [KEY_MUHENKAN] = 0x7b,
    [KEY_KPJPCOMMA] = 0,
    [KEY_KPENTER] = 0x9c,
    [KEY_RIGHTCTRL] = 0x9d,
    [KEY_KPSLASH] = 0xb5,
    [KEY_SYSRQ] = 0xb7,
    [KEY_RIGHTALT] = 0xb8,
    [KEY_LINEFEED] = 0,
    [KEY_HOME] = 0xc7,
    [KEY_UP] = 0xc8,
    [KEY_PAGEUP] = 0xc9,
    [KEY_LEFT] = 0xcb,
    [KEY_RIGHT] = 0xcd,
    [KEY_END] = 0xcf,
    [KEY_DOWN] = 0xd0,
    [KEY_PAGEDOWN] = 0xd1,
    [KEY_INSERT] = 0xd2,
    [KEY_DELETE] = 0xd3,
    [KEY_MACRO] = 0,
    [KEY_MUTE] = 0,
    [KEY_VOLUMEDOWN] = 0,
    [KEY_VOLUMEUP] = 0,
    [KEY_POWER] = 0,
    [KEY_KPEQUAL] = 0,
    [KEY_KPPLUSMINUS] = 0,
    [KEY_PAUSE] = 0,
    [KEY_SCALE] = 0,
    [KEY_KPCOMMA] = 0,
    [KEY_HANGEUL] = 0xf1,
    [KEY_HANJA] = 0xf2,
    [KEY_YEN] = 0x7b,
    [KEY_LEFTMETA] = 0xdb,
    [KEY_RIGHTMETA] = 0xdc,
    [KEY_COMPOSE] = 0xdd,
    [KEY_STOP] = 0,
    [KEY_AGAIN] = 0,
    [KEY_PROPS] = 0,
    [KEY_UNDO] = 0,
    [KEY_FRONT] = 0,
    [KEY_COPY] = 0,
    [KEY_OPEN] = 0,
    [KEY_PASTE] = 0,
    [KEY_FIND] = 0,
    [KEY_CUT] = 0,
    [KEY_HELP] = 0,
    [KEY_MENU] = 0,
    [KEY_CALC] = 0,
    [KEY_SETUP] = 0,
    [KEY_SLEEP] = 0,
    [KEY_WAKEUP] = 0,
    [KEY_FILE] = 0,
    [KEY_SENDFILE] = 0,
    [KEY_DELETEFILE] = 0,
    [KEY_XFER] = 0,
    [KEY_PROG1] = 0,
    [KEY_PROG2] = 0,
};

static const char *const events[EV_MAX + 1] = {
    [0 ... EV_MAX] = NULL,
    NAME_ELEMENT(EV_SYN), NAME_ELEMENT(EV_KEY),
    NAME_ELEMENT(EV_REL), NAME_ELEMENT(EV_ABS),
    NAME_ELEMENT(EV_MSC), NAME_ELEMENT(EV_LED),
    NAME_ELEMENT(EV_SND), NAME_ELEMENT(EV_REP),
    NAME_ELEMENT(EV_FF), NAME_ELEMENT(EV_PWR),
    NAME_ELEMENT(EV_FF_STATUS), NAME_ELEMENT(EV_SW),
};

#ifdef INPUT_PROP_SEMI_MT
static const char *const props[INPUT_PROP_MAX + 1] = {
    [0 ... INPUT_PROP_MAX] = NULL,
    NAME_ELEMENT(INPUT_PROP_POINTER),
    NAME_ELEMENT(INPUT_PROP_DIRECT),
    NAME_ELEMENT(INPUT_PROP_BUTTONPAD),
    NAME_ELEMENT(INPUT_PROP_SEMI_MT),
};
#endif

static const char *const keys[KEY_MAX + 1] = {
    [0 ... KEY_MAX] = NULL,
    NAME_ELEMENT(KEY_RESERVED), NAME_ELEMENT(KEY_ESC),
    NAME_ELEMENT(KEY_1), NAME_ELEMENT(KEY_2),
    NAME_ELEMENT(KEY_3), NAME_ELEMENT(KEY_4),
    NAME_ELEMENT(KEY_5), NAME_ELEMENT(KEY_6),
    NAME_ELEMENT(KEY_7), NAME_ELEMENT(KEY_8),
    NAME_ELEMENT(KEY_9), NAME_ELEMENT(KEY_0),
    NAME_ELEMENT(KEY_MINUS), NAME_ELEMENT(KEY_EQUAL),
    NAME_ELEMENT(KEY_BACKSPACE), NAME_ELEMENT(KEY_TAB),
    NAME_ELEMENT(KEY_Q), NAME_ELEMENT(KEY_W),
    NAME_ELEMENT(KEY_E), NAME_ELEMENT(KEY_R),
    NAME_ELEMENT(KEY_T), NAME_ELEMENT(KEY_Y),
    NAME_ELEMENT(KEY_U), NAME_ELEMENT(KEY_I),
    NAME_ELEMENT(KEY_O), NAME_ELEMENT(KEY_P),
    NAME_ELEMENT(KEY_LEFTBRACE), NAME_ELEMENT(KEY_RIGHTBRACE),
    NAME_ELEMENT(KEY_ENTER), NAME_ELEMENT(KEY_LEFTCTRL),
    NAME_ELEMENT(KEY_A), NAME_ELEMENT(KEY_S),
    NAME_ELEMENT(KEY_D), NAME_ELEMENT(KEY_F),
    NAME_ELEMENT(KEY_G), NAME_ELEMENT(KEY_H),
    NAME_ELEMENT(KEY_J), NAME_ELEMENT(KEY_K),
    NAME_ELEMENT(KEY_L), NAME_ELEMENT(KEY_SEMICOLON),
    NAME_ELEMENT(KEY_APOSTROPHE), NAME_ELEMENT(KEY_GRAVE),
    NAME_ELEMENT(KEY_LEFTSHIFT), NAME_ELEMENT(KEY_BACKSLASH),
    NAME_ELEMENT(KEY_Z), NAME_ELEMENT(KEY_X),
    NAME_ELEMENT(KEY_C), NAME_ELEMENT(KEY_V),
    NAME_ELEMENT(KEY_B), NAME_ELEMENT(KEY_N),
    NAME_ELEMENT(KEY_M), NAME_ELEMENT(KEY_COMMA),
    NAME_ELEMENT(KEY_DOT), NAME_ELEMENT(KEY_SLASH),
    NAME_ELEMENT(KEY_RIGHTSHIFT), NAME_ELEMENT(KEY_KPASTERISK),
    NAME_ELEMENT(KEY_LEFTALT), NAME_ELEMENT(KEY_SPACE),
    NAME_ELEMENT(KEY_CAPSLOCK), NAME_ELEMENT(KEY_F1),
    NAME_ELEMENT(KEY_F2), NAME_ELEMENT(KEY_F3),
    NAME_ELEMENT(KEY_F4), NAME_ELEMENT(KEY_F5),
    NAME_ELEMENT(KEY_F6), NAME_ELEMENT(KEY_F7),
    NAME_ELEMENT(KEY_F8), NAME_ELEMENT(KEY_F9),
    NAME_ELEMENT(KEY_F10), NAME_ELEMENT(KEY_NUMLOCK),
    NAME_ELEMENT(KEY_SCROLLLOCK), NAME_ELEMENT(KEY_KP7),
    NAME_ELEMENT(KEY_KP8), NAME_ELEMENT(KEY_KP9),
    NAME_ELEMENT(KEY_KPMINUS), NAME_ELEMENT(KEY_KP4),
    NAME_ELEMENT(KEY_KP5), NAME_ELEMENT(KEY_KP6),
    NAME_ELEMENT(KEY_KPPLUS), NAME_ELEMENT(KEY_KP1),
    NAME_ELEMENT(KEY_KP2), NAME_ELEMENT(KEY_KP3),
    NAME_ELEMENT(KEY_KP0), NAME_ELEMENT(KEY_KPDOT),
    NAME_ELEMENT(KEY_ZENKAKUHANKAKU), NAME_ELEMENT(KEY_102ND),
    NAME_ELEMENT(KEY_F11), NAME_ELEMENT(KEY_F12),
    NAME_ELEMENT(KEY_RO), NAME_ELEMENT(KEY_KATAKANA),
    NAME_ELEMENT(KEY_HIRAGANA), NAME_ELEMENT(KEY_HENKAN),
    NAME_ELEMENT(KEY_KATAKANAHIRAGANA), NAME_ELEMENT(KEY_MUHENKAN),
    NAME_ELEMENT(KEY_KPJPCOMMA), NAME_ELEMENT(KEY_KPENTER),
    NAME_ELEMENT(KEY_RIGHTCTRL), NAME_ELEMENT(KEY_KPSLASH),
    NAME_ELEMENT(KEY_SYSRQ), NAME_ELEMENT(KEY_RIGHTALT),
    NAME_ELEMENT(KEY_LINEFEED), NAME_ELEMENT(KEY_HOME),
    NAME_ELEMENT(KEY_UP), NAME_ELEMENT(KEY_PAGEUP),
    NAME_ELEMENT(KEY_LEFT), NAME_ELEMENT(KEY_RIGHT),
    NAME_ELEMENT(KEY_END), NAME_ELEMENT(KEY_DOWN),
    NAME_ELEMENT(KEY_PAGEDOWN), NAME_ELEMENT(KEY_INSERT),
    NAME_ELEMENT(KEY_DELETE), NAME_ELEMENT(KEY_MACRO),
    NAME_ELEMENT(KEY_MUTE), NAME_ELEMENT(KEY_VOLUMEDOWN),
    NAME_ELEMENT(KEY_VOLUMEUP), NAME_ELEMENT(KEY_POWER),
    NAME_ELEMENT(KEY_KPEQUAL), NAME_ELEMENT(KEY_KPPLUSMINUS),
    NAME_ELEMENT(KEY_PAUSE), NAME_ELEMENT(KEY_KPCOMMA),
    NAME_ELEMENT(KEY_HANGUEL), NAME_ELEMENT(KEY_HANJA),
    NAME_ELEMENT(KEY_YEN), NAME_ELEMENT(KEY_LEFTMETA),
    NAME_ELEMENT(KEY_RIGHTMETA), NAME_ELEMENT(KEY_COMPOSE),
    NAME_ELEMENT(KEY_STOP), NAME_ELEMENT(KEY_AGAIN),
    NAME_ELEMENT(KEY_PROPS), NAME_ELEMENT(KEY_UNDO),
    NAME_ELEMENT(KEY_FRONT), NAME_ELEMENT(KEY_COPY),
    NAME_ELEMENT(KEY_OPEN), NAME_ELEMENT(KEY_PASTE),
    NAME_ELEMENT(KEY_FIND), NAME_ELEMENT(KEY_CUT),
    NAME_ELEMENT(KEY_HELP), NAME_ELEMENT(KEY_MENU),
    NAME_ELEMENT(KEY_CALC), NAME_ELEMENT(KEY_SETUP),
    NAME_ELEMENT(KEY_SLEEP), NAME_ELEMENT(KEY_WAKEUP),
    NAME_ELEMENT(KEY_FILE), NAME_ELEMENT(KEY_SENDFILE),
    NAME_ELEMENT(KEY_DELETEFILE), NAME_ELEMENT(KEY_XFER),
    NAME_ELEMENT(KEY_PROG1), NAME_ELEMENT(KEY_PROG2),
    NAME_ELEMENT(KEY_WWW), NAME_ELEMENT(KEY_MSDOS),
    NAME_ELEMENT(KEY_COFFEE), NAME_ELEMENT(KEY_DIRECTION),
    NAME_ELEMENT(KEY_CYCLEWINDOWS), NAME_ELEMENT(KEY_MAIL),
    NAME_ELEMENT(KEY_BOOKMARKS), NAME_ELEMENT(KEY_COMPUTER),
    NAME_ELEMENT(KEY_BACK), NAME_ELEMENT(KEY_FORWARD),
    NAME_ELEMENT(KEY_CLOSECD), NAME_ELEMENT(KEY_EJECTCD),
    NAME_ELEMENT(KEY_EJECTCLOSECD), NAME_ELEMENT(KEY_NEXTSONG),
    NAME_ELEMENT(KEY_PLAYPAUSE), NAME_ELEMENT(KEY_PREVIOUSSONG),
    NAME_ELEMENT(KEY_STOPCD), NAME_ELEMENT(KEY_RECORD),
    NAME_ELEMENT(KEY_REWIND), NAME_ELEMENT(KEY_PHONE),
    NAME_ELEMENT(KEY_ISO), NAME_ELEMENT(KEY_CONFIG),
    NAME_ELEMENT(KEY_HOMEPAGE), NAME_ELEMENT(KEY_REFRESH),
    NAME_ELEMENT(KEY_EXIT), NAME_ELEMENT(KEY_MOVE),
    NAME_ELEMENT(KEY_EDIT), NAME_ELEMENT(KEY_SCROLLUP),
    NAME_ELEMENT(KEY_SCROLLDOWN), NAME_ELEMENT(KEY_KPLEFTPAREN),
    NAME_ELEMENT(KEY_KPRIGHTPAREN), NAME_ELEMENT(KEY_F13),
    NAME_ELEMENT(KEY_F14), NAME_ELEMENT(KEY_F15),
    NAME_ELEMENT(KEY_F16), NAME_ELEMENT(KEY_F17),
    NAME_ELEMENT(KEY_F18), NAME_ELEMENT(KEY_F19),
    NAME_ELEMENT(KEY_F20), NAME_ELEMENT(KEY_F21),
    NAME_ELEMENT(KEY_F22), NAME_ELEMENT(KEY_F23),
    NAME_ELEMENT(KEY_F24), NAME_ELEMENT(KEY_PLAYCD),
    NAME_ELEMENT(KEY_PAUSECD), NAME_ELEMENT(KEY_PROG3),
    NAME_ELEMENT(KEY_PROG4), NAME_ELEMENT(KEY_SUSPEND),
    NAME_ELEMENT(KEY_CLOSE), NAME_ELEMENT(KEY_PLAY),
    NAME_ELEMENT(KEY_FASTFORWARD), NAME_ELEMENT(KEY_BASSBOOST),
    NAME_ELEMENT(KEY_PRINT), NAME_ELEMENT(KEY_HP),
    NAME_ELEMENT(KEY_CAMERA), NAME_ELEMENT(KEY_SOUND),
    NAME_ELEMENT(KEY_QUESTION), NAME_ELEMENT(KEY_EMAIL),
    NAME_ELEMENT(KEY_CHAT), NAME_ELEMENT(KEY_SEARCH),
    NAME_ELEMENT(KEY_CONNECT), NAME_ELEMENT(KEY_FINANCE),
    NAME_ELEMENT(KEY_SPORT), NAME_ELEMENT(KEY_SHOP),
    NAME_ELEMENT(KEY_ALTERASE), NAME_ELEMENT(KEY_CANCEL),
    NAME_ELEMENT(KEY_BRIGHTNESSDOWN), NAME_ELEMENT(KEY_BRIGHTNESSUP),
    NAME_ELEMENT(KEY_MEDIA), NAME_ELEMENT(KEY_UNKNOWN),
    NAME_ELEMENT(KEY_OK),
    NAME_ELEMENT(KEY_SELECT), NAME_ELEMENT(KEY_GOTO),
    NAME_ELEMENT(KEY_CLEAR), NAME_ELEMENT(KEY_POWER2),
    NAME_ELEMENT(KEY_OPTION), NAME_ELEMENT(KEY_INFO),
    NAME_ELEMENT(KEY_TIME), NAME_ELEMENT(KEY_VENDOR),
    NAME_ELEMENT(KEY_ARCHIVE), NAME_ELEMENT(KEY_PROGRAM),
    NAME_ELEMENT(KEY_CHANNEL), NAME_ELEMENT(KEY_FAVORITES),
    NAME_ELEMENT(KEY_EPG), NAME_ELEMENT(KEY_PVR),
    NAME_ELEMENT(KEY_MHP), NAME_ELEMENT(KEY_LANGUAGE),
    NAME_ELEMENT(KEY_TITLE), NAME_ELEMENT(KEY_SUBTITLE),
    NAME_ELEMENT(KEY_ANGLE), NAME_ELEMENT(KEY_ZOOM),
    NAME_ELEMENT(KEY_MODE), NAME_ELEMENT(KEY_KEYBOARD),
    NAME_ELEMENT(KEY_SCREEN), NAME_ELEMENT(KEY_PC),
    NAME_ELEMENT(KEY_TV), NAME_ELEMENT(KEY_TV2),
    NAME_ELEMENT(KEY_VCR), NAME_ELEMENT(KEY_VCR2),
    NAME_ELEMENT(KEY_SAT), NAME_ELEMENT(KEY_SAT2),
    NAME_ELEMENT(KEY_CD), NAME_ELEMENT(KEY_TAPE),
    NAME_ELEMENT(KEY_RADIO), NAME_ELEMENT(KEY_TUNER),
    NAME_ELEMENT(KEY_PLAYER), NAME_ELEMENT(KEY_TEXT),
    NAME_ELEMENT(KEY_DVD), NAME_ELEMENT(KEY_AUX),
    NAME_ELEMENT(KEY_MP3), NAME_ELEMENT(KEY_AUDIO),
    NAME_ELEMENT(KEY_VIDEO), NAME_ELEMENT(KEY_DIRECTORY),
    NAME_ELEMENT(KEY_LIST), NAME_ELEMENT(KEY_MEMO),
    NAME_ELEMENT(KEY_CALENDAR), NAME_ELEMENT(KEY_RED),
    NAME_ELEMENT(KEY_GREEN), NAME_ELEMENT(KEY_YELLOW),
    NAME_ELEMENT(KEY_BLUE), NAME_ELEMENT(KEY_CHANNELUP),
    NAME_ELEMENT(KEY_CHANNELDOWN), NAME_ELEMENT(KEY_FIRST),
    NAME_ELEMENT(KEY_LAST), NAME_ELEMENT(KEY_AB),
    NAME_ELEMENT(KEY_NEXT), NAME_ELEMENT(KEY_RESTART),
    NAME_ELEMENT(KEY_SLOW), NAME_ELEMENT(KEY_SHUFFLE),
    NAME_ELEMENT(KEY_BREAK), NAME_ELEMENT(KEY_PREVIOUS),
    NAME_ELEMENT(KEY_DIGITS), NAME_ELEMENT(KEY_TEEN),
    NAME_ELEMENT(KEY_TWEN), NAME_ELEMENT(KEY_DEL_EOL),
    NAME_ELEMENT(KEY_DEL_EOS), NAME_ELEMENT(KEY_INS_LINE),
    NAME_ELEMENT(KEY_DEL_LINE),
    NAME_ELEMENT(KEY_VIDEOPHONE), NAME_ELEMENT(KEY_GAMES),
    NAME_ELEMENT(KEY_ZOOMIN), NAME_ELEMENT(KEY_ZOOMOUT),
    NAME_ELEMENT(KEY_ZOOMRESET), NAME_ELEMENT(KEY_WORDPROCESSOR),
    NAME_ELEMENT(KEY_EDITOR), NAME_ELEMENT(KEY_SPREADSHEET),
    NAME_ELEMENT(KEY_GRAPHICSEDITOR), NAME_ELEMENT(KEY_PRESENTATION),
    NAME_ELEMENT(KEY_DATABASE), NAME_ELEMENT(KEY_NEWS),
    NAME_ELEMENT(KEY_VOICEMAIL), NAME_ELEMENT(KEY_ADDRESSBOOK),
    NAME_ELEMENT(KEY_MESSENGER), NAME_ELEMENT(KEY_DISPLAYTOGGLE),
    NAME_ELEMENT(KEY_SPELLCHECK), NAME_ELEMENT(KEY_LOGOFF),
    NAME_ELEMENT(KEY_DOLLAR), NAME_ELEMENT(KEY_EURO),
    NAME_ELEMENT(KEY_FRAMEBACK), NAME_ELEMENT(KEY_FRAMEFORWARD),
    NAME_ELEMENT(KEY_CONTEXT_MENU), NAME_ELEMENT(KEY_MEDIA_REPEAT),
    NAME_ELEMENT(KEY_DEL_EOL), NAME_ELEMENT(KEY_DEL_EOS),
    NAME_ELEMENT(KEY_INS_LINE), NAME_ELEMENT(KEY_DEL_LINE),
    NAME_ELEMENT(KEY_FN), NAME_ELEMENT(KEY_FN_ESC),
    NAME_ELEMENT(KEY_FN_F1), NAME_ELEMENT(KEY_FN_F2),
    NAME_ELEMENT(KEY_FN_F3), NAME_ELEMENT(KEY_FN_F4),
    NAME_ELEMENT(KEY_FN_F5), NAME_ELEMENT(KEY_FN_F6),
    NAME_ELEMENT(KEY_FN_F7), NAME_ELEMENT(KEY_FN_F8),
    NAME_ELEMENT(KEY_FN_F9), NAME_ELEMENT(KEY_FN_F10),
    NAME_ELEMENT(KEY_FN_F11), NAME_ELEMENT(KEY_FN_F12),
    NAME_ELEMENT(KEY_FN_1), NAME_ELEMENT(KEY_FN_2),
    NAME_ELEMENT(KEY_FN_D), NAME_ELEMENT(KEY_FN_E),
    NAME_ELEMENT(KEY_FN_F), NAME_ELEMENT(KEY_FN_S),
    NAME_ELEMENT(KEY_FN_B),
    NAME_ELEMENT(KEY_BRL_DOT1), NAME_ELEMENT(KEY_BRL_DOT2),
    NAME_ELEMENT(KEY_BRL_DOT3), NAME_ELEMENT(KEY_BRL_DOT4),
    NAME_ELEMENT(KEY_BRL_DOT5), NAME_ELEMENT(KEY_BRL_DOT6),
    NAME_ELEMENT(KEY_BRL_DOT7), NAME_ELEMENT(KEY_BRL_DOT8),
    NAME_ELEMENT(KEY_BRL_DOT9), NAME_ELEMENT(KEY_BRL_DOT10),
    NAME_ELEMENT(KEY_NUMERIC_0), NAME_ELEMENT(KEY_NUMERIC_1),
    NAME_ELEMENT(KEY_NUMERIC_2), NAME_ELEMENT(KEY_NUMERIC_3),
    NAME_ELEMENT(KEY_NUMERIC_4), NAME_ELEMENT(KEY_NUMERIC_5),
    NAME_ELEMENT(KEY_NUMERIC_6), NAME_ELEMENT(KEY_NUMERIC_7),
    NAME_ELEMENT(KEY_NUMERIC_8), NAME_ELEMENT(KEY_NUMERIC_9),
    NAME_ELEMENT(KEY_NUMERIC_STAR), NAME_ELEMENT(KEY_NUMERIC_POUND),
    NAME_ELEMENT(KEY_BATTERY),
    NAME_ELEMENT(KEY_BLUETOOTH), NAME_ELEMENT(KEY_BRIGHTNESS_CYCLE),
    NAME_ELEMENT(KEY_BRIGHTNESS_ZERO), NAME_ELEMENT(KEY_DASHBOARD),
    NAME_ELEMENT(KEY_DISPLAY_OFF), NAME_ELEMENT(KEY_DOCUMENTS),
    NAME_ELEMENT(KEY_FORWARDMAIL), NAME_ELEMENT(KEY_NEW),
    NAME_ELEMENT(KEY_KBDILLUMDOWN), NAME_ELEMENT(KEY_KBDILLUMUP),
    NAME_ELEMENT(KEY_KBDILLUMTOGGLE), NAME_ELEMENT(KEY_REDO),
    NAME_ELEMENT(KEY_REPLY), NAME_ELEMENT(KEY_SAVE),
    NAME_ELEMENT(KEY_SCALE), NAME_ELEMENT(KEY_SEND),
    NAME_ELEMENT(KEY_SCREENLOCK), NAME_ELEMENT(KEY_SWITCHVIDEOMODE),
    NAME_ELEMENT(KEY_UWB), NAME_ELEMENT(KEY_VIDEO_NEXT),
    NAME_ELEMENT(KEY_VIDEO_PREV), NAME_ELEMENT(KEY_WIMAX),
    NAME_ELEMENT(KEY_WLAN),
#ifdef KEY_RFKILL
    NAME_ELEMENT(KEY_RFKILL),
#endif
#ifdef KEY_WPS_BUTTON
    NAME_ELEMENT(KEY_WPS_BUTTON),
#endif
#ifdef KEY_TOUCHPAD_TOGGLE
    NAME_ELEMENT(KEY_TOUCHPAD_TOGGLE),
    NAME_ELEMENT(KEY_TOUCHPAD_ON),
    NAME_ELEMENT(KEY_TOUCHPAD_OFF),
#endif

    NAME_ELEMENT(BTN_0), NAME_ELEMENT(BTN_1),
    NAME_ELEMENT(BTN_2), NAME_ELEMENT(BTN_3),
    NAME_ELEMENT(BTN_4), NAME_ELEMENT(BTN_5),
    NAME_ELEMENT(BTN_6), NAME_ELEMENT(BTN_7),
    NAME_ELEMENT(BTN_8), NAME_ELEMENT(BTN_9),
    NAME_ELEMENT(BTN_LEFT), NAME_ELEMENT(BTN_RIGHT),
    NAME_ELEMENT(BTN_MIDDLE), NAME_ELEMENT(BTN_SIDE),
    NAME_ELEMENT(BTN_EXTRA), NAME_ELEMENT(BTN_FORWARD),
    NAME_ELEMENT(BTN_BACK), NAME_ELEMENT(BTN_TASK),
    NAME_ELEMENT(BTN_TRIGGER), NAME_ELEMENT(BTN_THUMB),
    NAME_ELEMENT(BTN_THUMB2), NAME_ELEMENT(BTN_TOP),
    NAME_ELEMENT(BTN_TOP2), NAME_ELEMENT(BTN_PINKIE),
    NAME_ELEMENT(BTN_BASE), NAME_ELEMENT(BTN_BASE2),
    NAME_ELEMENT(BTN_BASE3), NAME_ELEMENT(BTN_BASE4),
    NAME_ELEMENT(BTN_BASE5), NAME_ELEMENT(BTN_BASE6),
    NAME_ELEMENT(BTN_DEAD), NAME_ELEMENT(BTN_A),
    NAME_ELEMENT(BTN_B), NAME_ELEMENT(BTN_C),
    NAME_ELEMENT(BTN_X), NAME_ELEMENT(BTN_Y),
    NAME_ELEMENT(BTN_Z), NAME_ELEMENT(BTN_TL),
    NAME_ELEMENT(BTN_TR), NAME_ELEMENT(BTN_TL2),
    NAME_ELEMENT(BTN_TR2), NAME_ELEMENT(BTN_SELECT),
    NAME_ELEMENT(BTN_START), NAME_ELEMENT(BTN_MODE),
    NAME_ELEMENT(BTN_THUMBL), NAME_ELEMENT(BTN_THUMBR),
    NAME_ELEMENT(BTN_TOOL_PEN), NAME_ELEMENT(BTN_TOOL_RUBBER),
    NAME_ELEMENT(BTN_TOOL_BRUSH), NAME_ELEMENT(BTN_TOOL_PENCIL),
    NAME_ELEMENT(BTN_TOOL_AIRBRUSH), NAME_ELEMENT(BTN_TOOL_FINGER),
    NAME_ELEMENT(BTN_TOOL_MOUSE), NAME_ELEMENT(BTN_TOOL_LENS),
    NAME_ELEMENT(BTN_TOUCH), NAME_ELEMENT(BTN_STYLUS),
    NAME_ELEMENT(BTN_STYLUS2), NAME_ELEMENT(BTN_TOOL_DOUBLETAP),
    NAME_ELEMENT(BTN_TOOL_TRIPLETAP), NAME_ELEMENT(BTN_TOOL_QUADTAP),
    NAME_ELEMENT(BTN_GEAR_DOWN),
    NAME_ELEMENT(BTN_GEAR_UP),

#ifdef BTN_TRIGGER_HAPPY
    NAME_ELEMENT(BTN_TRIGGER_HAPPY1), NAME_ELEMENT(BTN_TRIGGER_HAPPY11),
    NAME_ELEMENT(BTN_TRIGGER_HAPPY2), NAME_ELEMENT(BTN_TRIGGER_HAPPY12),
    NAME_ELEMENT(BTN_TRIGGER_HAPPY3), NAME_ELEMENT(BTN_TRIGGER_HAPPY13),
    NAME_ELEMENT(BTN_TRIGGER_HAPPY4), NAME_ELEMENT(BTN_TRIGGER_HAPPY14),
    NAME_ELEMENT(BTN_TRIGGER_HAPPY5), NAME_ELEMENT(BTN_TRIGGER_HAPPY15),
    NAME_ELEMENT(BTN_TRIGGER_HAPPY6), NAME_ELEMENT(BTN_TRIGGER_HAPPY16),
    NAME_ELEMENT(BTN_TRIGGER_HAPPY7), NAME_ELEMENT(BTN_TRIGGER_HAPPY17),
    NAME_ELEMENT(BTN_TRIGGER_HAPPY8), NAME_ELEMENT(BTN_TRIGGER_HAPPY18),
    NAME_ELEMENT(BTN_TRIGGER_HAPPY9), NAME_ELEMENT(BTN_TRIGGER_HAPPY19),
    NAME_ELEMENT(BTN_TRIGGER_HAPPY10), NAME_ELEMENT(BTN_TRIGGER_HAPPY20),

    NAME_ELEMENT(BTN_TRIGGER_HAPPY21), NAME_ELEMENT(BTN_TRIGGER_HAPPY31),
    NAME_ELEMENT(BTN_TRIGGER_HAPPY22), NAME_ELEMENT(BTN_TRIGGER_HAPPY32),
    NAME_ELEMENT(BTN_TRIGGER_HAPPY23), NAME_ELEMENT(BTN_TRIGGER_HAPPY33),
    NAME_ELEMENT(BTN_TRIGGER_HAPPY24), NAME_ELEMENT(BTN_TRIGGER_HAPPY34),
    NAME_ELEMENT(BTN_TRIGGER_HAPPY25), NAME_ELEMENT(BTN_TRIGGER_HAPPY35),
    NAME_ELEMENT(BTN_TRIGGER_HAPPY26), NAME_ELEMENT(BTN_TRIGGER_HAPPY36),
    NAME_ELEMENT(BTN_TRIGGER_HAPPY27), NAME_ELEMENT(BTN_TRIGGER_HAPPY37),
    NAME_ELEMENT(BTN_TRIGGER_HAPPY28), NAME_ELEMENT(BTN_TRIGGER_HAPPY38),
    NAME_ELEMENT(BTN_TRIGGER_HAPPY29), NAME_ELEMENT(BTN_TRIGGER_HAPPY39),
    NAME_ELEMENT(BTN_TRIGGER_HAPPY30), NAME_ELEMENT(BTN_TRIGGER_HAPPY40),
#endif
#ifdef BTN_TOOL_QUINTTAP
    NAME_ELEMENT(BTN_TOOL_QUINTTAP),
#endif
};

static const char *const absval[6] =
    { "Value", "Min  ", "Max  ", "Fuzz ", "Flat ", "Resolution " };

static const char *const relatives[REL_MAX + 1] = {
    [0 ... REL_MAX] = NULL,
    NAME_ELEMENT(REL_X), NAME_ELEMENT(REL_Y),
    NAME_ELEMENT(REL_Z), NAME_ELEMENT(REL_RX),
    NAME_ELEMENT(REL_RY), NAME_ELEMENT(REL_RZ),
    NAME_ELEMENT(REL_HWHEEL),
    NAME_ELEMENT(REL_DIAL), NAME_ELEMENT(REL_WHEEL),
    NAME_ELEMENT(REL_MISC),
};

static const char *const absolutes[ABS_MAX + 1] = {
    [0 ... ABS_MAX] = NULL,
    NAME_ELEMENT(ABS_X), NAME_ELEMENT(ABS_Y),
    NAME_ELEMENT(ABS_Z), NAME_ELEMENT(ABS_RX),
    NAME_ELEMENT(ABS_RY), NAME_ELEMENT(ABS_RZ),
    NAME_ELEMENT(ABS_THROTTLE), NAME_ELEMENT(ABS_RUDDER),
    NAME_ELEMENT(ABS_WHEEL), NAME_ELEMENT(ABS_GAS),
    NAME_ELEMENT(ABS_BRAKE), NAME_ELEMENT(ABS_HAT0X),
    NAME_ELEMENT(ABS_HAT0Y), NAME_ELEMENT(ABS_HAT1X),
    NAME_ELEMENT(ABS_HAT1Y), NAME_ELEMENT(ABS_HAT2X),
    NAME_ELEMENT(ABS_HAT2Y), NAME_ELEMENT(ABS_HAT3X),
    NAME_ELEMENT(ABS_HAT3Y), NAME_ELEMENT(ABS_PRESSURE),
    NAME_ELEMENT(ABS_DISTANCE), NAME_ELEMENT(ABS_TILT_X),
    NAME_ELEMENT(ABS_TILT_Y), NAME_ELEMENT(ABS_TOOL_WIDTH),
    NAME_ELEMENT(ABS_VOLUME), NAME_ELEMENT(ABS_MISC),
#ifdef ABS_MT_BLOB_ID
    NAME_ELEMENT(ABS_MT_TOUCH_MAJOR),
    NAME_ELEMENT(ABS_MT_TOUCH_MINOR),
    NAME_ELEMENT(ABS_MT_WIDTH_MAJOR),
    NAME_ELEMENT(ABS_MT_WIDTH_MINOR),
    NAME_ELEMENT(ABS_MT_ORIENTATION),
    NAME_ELEMENT(ABS_MT_POSITION_X),
    NAME_ELEMENT(ABS_MT_POSITION_Y),
    NAME_ELEMENT(ABS_MT_TOOL_TYPE),
    NAME_ELEMENT(ABS_MT_BLOB_ID),
#endif
#ifdef ABS_MT_TRACKING_ID
    NAME_ELEMENT(ABS_MT_TRACKING_ID),
#endif
#ifdef ABS_MT_PRESSURE
    NAME_ELEMENT(ABS_MT_PRESSURE),
#endif
#ifdef ABS_MT_SLOT
    NAME_ELEMENT(ABS_MT_SLOT),
#endif
#ifdef ABS_MT_TOOL_X
    NAME_ELEMENT(ABS_MT_TOOL_X),
    NAME_ELEMENT(ABS_MT_TOOL_Y),
    NAME_ELEMENT(ABS_MT_DISTANCE),
#endif

};

static const char *const misc[MSC_MAX + 1] = {
    [0 ... MSC_MAX] = NULL,
    NAME_ELEMENT(MSC_SERIAL), NAME_ELEMENT(MSC_PULSELED),
    NAME_ELEMENT(MSC_GESTURE), NAME_ELEMENT(MSC_RAW),
    NAME_ELEMENT(MSC_SCAN),
#ifdef MSC_TIMESTAMP
    NAME_ELEMENT(MSC_TIMESTAMP),
#endif
};

static const char *const leds[LED_MAX + 1] = {
    [0 ... LED_MAX] = NULL,
    NAME_ELEMENT(LED_NUML), NAME_ELEMENT(LED_CAPSL),
    NAME_ELEMENT(LED_SCROLLL), NAME_ELEMENT(LED_COMPOSE),
    NAME_ELEMENT(LED_KANA), NAME_ELEMENT(LED_SLEEP),
    NAME_ELEMENT(LED_SUSPEND), NAME_ELEMENT(LED_MUTE),
    NAME_ELEMENT(LED_MISC),
};

static const char *const repeats[REP_MAX + 1] = {
    [0 ... REP_MAX] = NULL,
    NAME_ELEMENT(REP_DELAY), NAME_ELEMENT(REP_PERIOD)
};

static const char *const sounds[SND_MAX + 1] = {
    [0 ... SND_MAX] = NULL,
    NAME_ELEMENT(SND_CLICK), NAME_ELEMENT(SND_BELL),
    NAME_ELEMENT(SND_TONE)
};

static const char *const syns[3] = {
    NAME_ELEMENT(SYN_REPORT),
    NAME_ELEMENT(SYN_CONFIG),
#ifdef SYN_MT_REPORT
    NAME_ELEMENT(SYN_MT_REPORT)
#endif
};

static const char *const switches[SW_MAX + 1] = {
    [0 ... SW_MAX] = NULL,
    NAME_ELEMENT(SW_LID),
    NAME_ELEMENT(SW_TABLET_MODE),
    NAME_ELEMENT(SW_HEADPHONE_INSERT),
    NAME_ELEMENT(SW_RFKILL_ALL),
    NAME_ELEMENT(SW_MICROPHONE_INSERT),
    NAME_ELEMENT(SW_DOCK),
    NAME_ELEMENT(SW_LINEOUT_INSERT),
    NAME_ELEMENT(SW_JACK_PHYSICAL_INSERT),
#ifdef SW_VIDEOOUT_INSERT
    NAME_ELEMENT(SW_VIDEOOUT_INSERT),
#endif
#ifdef SW_CAMERA_LENS_COVER
    NAME_ELEMENT(SW_CAMERA_LENS_COVER),
    NAME_ELEMENT(SW_KEYPAD_SLIDE),
    NAME_ELEMENT(SW_FRONT_PROXIMITY),
#endif
#ifdef SW_ROTATE_LOCK
    NAME_ELEMENT(SW_ROTATE_LOCK),
#endif
};

static const char *const force[FF_MAX + 1] = {
    [0 ... FF_MAX] = NULL,
    NAME_ELEMENT(FF_RUMBLE), NAME_ELEMENT(FF_PERIODIC),
    NAME_ELEMENT(FF_CONSTANT), NAME_ELEMENT(FF_SPRING),
    NAME_ELEMENT(FF_FRICTION), NAME_ELEMENT(FF_DAMPER),
    NAME_ELEMENT(FF_INERTIA), NAME_ELEMENT(FF_RAMP),
    NAME_ELEMENT(FF_SQUARE), NAME_ELEMENT(FF_TRIANGLE),
    NAME_ELEMENT(FF_SINE), NAME_ELEMENT(FF_SAW_UP),
    NAME_ELEMENT(FF_SAW_DOWN), NAME_ELEMENT(FF_CUSTOM),
    NAME_ELEMENT(FF_GAIN), NAME_ELEMENT(FF_AUTOCENTER),
};

static const char *const forcestatus[FF_STATUS_MAX + 1] = {
    [0 ... FF_STATUS_MAX] = NULL,
    NAME_ELEMENT(FF_STATUS_STOPPED), NAME_ELEMENT(FF_STATUS_PLAYING),
};

static const char *const *const names[EV_MAX + 1] = {
    [0 ... EV_MAX] = NULL,
    [EV_SYN] = events,
    [EV_KEY] = keys,
    [EV_REL] = relatives,
    [EV_ABS] = absolutes,
    [EV_MSC] = misc,
    [EV_LED] = leds,
    [EV_SND] = sounds,
    [EV_REP] = repeats,
    [EV_SW] = switches,
    [EV_FF] = force,
    [EV_FF_STATUS] = forcestatus,
};

static const char *ev_types[EV_MAX + 1] = {
    [0 ... EV_MAX] = NULL,
    NAME_ELEMENT(EV_SYN), NAME_ELEMENT(EV_KEY),
    NAME_ELEMENT(EV_REL), NAME_ELEMENT(EV_ABS),
    NAME_ELEMENT(EV_ABS), NAME_ELEMENT(EV_MSC),
    NAME_ELEMENT(EV_SW), NAME_ELEMENT(EV_LED),
    NAME_ELEMENT(EV_SND), NAME_ELEMENT(EV_REP),
    NAME_ELEMENT(EV_FF), NAME_ELEMENT(EV_PWR),
    NAME_ELEMENT(EV_FF_STATUS)
};

/**
 * Grab and immediately ungrab the device.
 *
 * @param fd The file descriptor to the device.
 * @return 0 if the grab was successful, or 1 otherwise.
 */
static int test_grab(int fd)
{
    int rc;

    rc = ioctl(fd, EVIOCGRAB, (void *)1);

    if (!rc)
        ioctl(fd, EVIOCGRAB, (void *)0);

    return rc;
}

/**
 * Filter for the AutoDevProbe scandir on /dev/input.
 *
 * @param dir The current directory entry provided by scandir.
 *
 * @return Non-zero if the given directory entry starts with "event", or zero
 * otherwise.
 */
static int is_event_device(const struct dirent *dir)
{
    return strncmp("event", dir->d_name, 5) == 0;
}

typedef struct QipsEventDevice {
    int fd;
    pthread_t thread;
    const char *name;
    const char *path;
     QTAILQ_ENTRY(QipsEventDevice) next;
} QipsEventDevice;

typedef QTAILQ_HEAD(QipsEventDeviceList, QipsEventDevice) QipsEventDeviceList;

static QipsEventDeviceList devices = QTAILQ_HEAD_INITIALIZER(devices);
static QemuMutex devices_mutex;

static void evdev_list_add(QipsEventDevice * device)
{
    DPRINTF("adding evdev name=%s path=%s...\n", device->name, device->path);
    qemu_mutex_lock(&devices_mutex);
    QTAILQ_INSERT_TAIL(&devices, device, next);
    qemu_mutex_unlock(&devices_mutex);
}

static void evdev_list_remove(QipsEventDevice * device)
{
    DPRINTF("removing evdev name=%s path=%s...\n", device->name, device->path);
    qemu_mutex_lock(&devices_mutex);
    QTAILQ_REMOVE(&devices, device, next);
    qemu_mutex_unlock(&devices_mutex);
}

/*
static void evdev_list_remove_by_fd(int fd)
{
    QipsEventDevice *device = NULL, *tmp = NULL;

    DPRINTF("removing evdev fd=%d...\n", fd);

    QTAILQ_FOREACH_SAFE(device, &devices, next, tmp) {
        if (device->fd == fd) {
            evdev_list_remove(device);
            return;
        }
    }
}
*/

static void process_event(struct input_event *ev)
{
    QipsMouseButtons mouse_buttons = { false, false, false };
    bool mouse_buttons_pkt = false;

    EVDEV_DPRINTF("ev->time: %ld.%06ld\n", ev->time.tv_sec, ev->time.tv_usec);
    EVDEV_DPRINTF("ev->type: %s (0x%x)\n", ev_types[ev->type], ev->type);
    EVDEV_DPRINTF("ev->code: %s (0x%x)\n", names[ev->type][ev->code], ev->code);

    if (ev->type == EV_KEY) {
        switch (ev->value) {
        case 0:
            EVDEV_DPRINTF("ev->value: KEY_RELEASED (%d)\n", ev->value);
            break;
        case 1:
            EVDEV_DPRINTF("ev->value: KEY_DEPRESSED (%d)\n", ev->value);
            break;
        case 2:
            EVDEV_DPRINTF("ev->value: KEY_REPEAT (%d)\n", ev->value);
            break;
        case 3:
            EVDEV_DPRINTF("ev->value: KEY_WTF (%d)\n", ev->value);
            break;
        }

        switch (ev->code) {
        case BTN_LEFT:
            mouse_buttons_pkt = true;
            if (ev->value == 0) {
                mouse_buttons.left = false;
            } else if (ev->value == 1) {
                mouse_buttons.left = true;
            }

            break;
        case BTN_MIDDLE:
            mouse_buttons_pkt = true;
            if (ev->value == 0) {
                mouse_buttons.middle = false;
            } else if (ev->value == 1) {
                mouse_buttons.middle = true;
            }

            break;
        case BTN_RIGHT:
            mouse_buttons_pkt = true;
            if (ev->value == 0) {
                mouse_buttons.right = false;
            } else if (ev->value == 1) {
                mouse_buttons.right = true;
            }

            break;
        }

        if (mouse_buttons_pkt) {
            // mouse button packet
            qips_input_backend_rel_mouse_event(timestamp_usec(ev->time), 0,
                                               0, 0, &mouse_buttons);
        } else {
            uint8_t scancode = 0;

            if (ev->code >= KEY_MAX) {
                EVDEV_DPRINTF("warning code=0x%x exceeds KEY_MAX!\n", ev->code);
                return;
            }

            scancode = evdev_keycode_to_pc_keycode[ev->code];

            EVDEV_DPRINTF("code=0x%x -> scancode=0x%x\n", ev->code, scancode);

            if (scancode) {
                qips_input_backend_key_event(timestamp_usec(ev->time),
                                             scancode, ev->value);
            }
        }
    } else if (ev->type == EV_MSC
               && (ev->code == MSC_RAW || ev->code == MSC_SCAN)) {
        EVDEV_DPRINTF("ev->value: 0x%x\n", ev->value);
    } else if (ev->type == EV_REL) {
        int dx = 0, dy = 0, dz = 0;

        if (ev->code == REL_X) {
            dx = ev->value;
        } else if (ev->code == REL_Y) {
            dy = ev->value;
        } else if (ev->code == REL_WHEEL) {
            dz = -ev->value;
        } else {
            //
        }

        qips_input_backend_rel_mouse_event(timestamp_usec(ev->time), dx, dy,
                                           dz, &mouse_buttons);

        EVDEV_DPRINTF("ev->value: %d\n", ev->value);
    }

    EVDEV_DPRINTF("\n");
}

static void *device_thread(void *d)
{
    QipsEventDevice *device = (QipsEventDevice *) d;

    struct input_event last_packet;
    int hacked_pkt_count = 0;

    while (1) {
        struct input_event ev;

        if (read(device->fd, &ev, sizeof(ev)) < sizeof(ev)) {
            DPRINTF("failed to read from device!\n");
            evdev_list_remove(device);
            return NULL;
        }

        /* giant stupid hack until switch to udev - basically read()
           will keep spitting out the same darn packet endlessly */
        if (memcmp(&last_packet, &ev, sizeof(ev)) == 0) {
            if (++hacked_pkt_count > 100) {
                DPRINTF("100 repeated packets - dropping device!\n");
                evdev_list_remove(device);
                return NULL;
            }
        } else {
            memcpy(&last_packet, &ev, sizeof(ev));
            hacked_pkt_count = 0;
            process_event(&ev);
        }
    }

    return NULL;
}

static bool add_event_device(int fd, const char *name, const char *path)
{
    size_t len;
    char *device_name, *device_path;
    QipsEventDevice *device, *tmp;

    DPRINTF("adding evdev fd=%d...\n", fd);

    /* quick sanity check to make sure device doesn't already exist */
    QTAILQ_FOREACH_SAFE(device, &devices, next, tmp) {
        if (strcmp(path, device->path) == 0 && strcmp(name, device->name) == 0) {

            /* XXX: do a further check and make sure the old one is stale */
            DPRINTF("possible duplicate evdev name=%s path=%s fd=%d v fd=%d?\n",
                    name, path, fd, device->fd);
        }
    }

    device = g_malloc0(sizeof(QipsEventDevice));

    len = strlen(name) + 1;
    device_name = g_malloc0(len);
    pstrcpy(device_name, len, name);

    len = strlen(path) + 1;
    device_path = g_malloc0(len);
    pstrcpy(device_path, len, path);

    device->fd = fd;
    device->name = device_name;
    device->path = device_path;

    pthread_create(&device->thread, NULL, device_thread, device);

    evdev_list_add(device);

    return -1;
}

static bool check_event_device(const char *path)
{
    int fd;
    const char name[PATH_MAX];

    DPRINTF("checking event device: %s\n", path);

    if (strncmp("/dev/input/event", path, sizeof("/dev/input/event") - 1)) {
        DPRINTF("not an input device, skipping...");
        return false;
    }

    fd = open(path, O_RDONLY);

    if (fd < 0) {
        DPRINTF("unable to open %s:	%s\n", path, name);
        return false;
    }

    ioctl(fd, EVIOCGNAME(sizeof(name)), name);

    DPRINTF("adding %s:	%s\n", path, name);

    if (test_grab(fd) == 0) {
        DPRINTF("adding %s:	%s\n", path, name);
        if (add_event_device(fd, name, path) == true) {
            return true;
        }
    } else {
        DPRINTF("unable to grab %s:	%s\n", path, name);
    }

    close(fd);

    return false;
}

/**
 * Scans all /dev/input/event*, tries to grab them and adds them to the list.
 * XXX: a poor way of doing this - need to switch to udev monitoring
 *
 * @return The number of devices added.
 */
static int scan_devices(void)
{
    struct dirent **namelist;
    int i, ndev;
    int ndev_added = 0;

    ndev = scandir("/dev/input", &namelist, is_event_device, alphasort);

    if (ndev <= 0) {
        return 0;
    }

    DPRINTF("checking devices:\n");

    for (i = 0; i < ndev; i++) {
        char fname[PATH_MAX];

        snprintf(fname, sizeof(fname),
                 "%s/%s", "/dev/input", namelist[i]->d_name);

        check_event_device(fname);

        free(namelist[i]);
    }

    return ndev_added;
}

/* XXX: a terrible way of doing this - need to switch to udev monitoring */
static void *evdev_notify(void *unused)
{
    int fd, wd;

    /* initalize inotify */
    fd = inotify_init();

    if (fd < 0) {
        DPRINTF("inotify_init() error: %s\n", strerror(errno));
        return NULL;
    }

    /* add watch for /var/run/qemu-iss */
    wd = inotify_add_watch(fd, "/dev/input", IN_CREATE | IN_DELETE);

    while (1) {
        char event_buffer[sizeof(struct inotify_event) + NAME_MAX + 1];
        char full_path[PATH_MAX + 1];
        struct inotify_event *event;
        int length;

        length = read(fd, event_buffer, sizeof(event_buffer));

        if (length < 0) {
            DPRINTF("inotify read() error: %s\n", strerror(errno));
            return NULL;
        }

        event = (struct inotify_event *)event_buffer;

        if (!event->len) {
            DPRINTF("warning: name is zero bytes?\n");
            continue;
        }

        /* determine full path */
        snprintf(full_path, sizeof(full_path), "/dev/input/%s", event->name);

        if (event->mask & IN_CREATE) {
            if (event->mask & IN_ISDIR) {
                DPRINTF("detected new directory: %s\n", full_path);
            } else {
                DPRINTF("detected new file: %s\n", full_path);
                check_event_device(full_path);
            }
        } else if (event->mask & IN_DELETE) {
            if (event->mask & IN_ISDIR) {
                DPRINTF("detected deleted directory: %s\n", full_path);
            } else {
                DPRINTF("detected deleted file: %s\n", full_path);
            }
        }
    }

    /* cleanup */
    inotify_rm_watch(fd, wd);
    close(fd);
    return NULL;
}

static bool evdev_init(void)
{
    DPRINTF("evdev_init: called!\n");
    qemu_mutex_init(&devices_mutex);
    scan_devices();
    pthread_create(&evdev_inotify_thread, NULL, evdev_notify, NULL);
    return true;
}

static bool evdev_cleanup(void)
{
    DPRINTF("evdev_cleanup: called!\n");
    return true;
}

static const QipsInputBackend evdev = {
    .init = evdev_init,
    .cleanup = evdev_cleanup,
};

const QipsInputBackend *evdev_input_backend_register(void)
{
    qips_input_backend_register(&evdev);

    return &evdev;
}
