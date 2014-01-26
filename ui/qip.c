/*
 * Copyright (c) 2013 Chris Patterson <cjp256@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 or
 * (at your option) version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */
#include <stdint.h>
#include <syslog.h>
#include "qemu/thread.h"
#include "qemu-common.h"
#include "hw/xen/xen.h"
#include "ui/keymaps.h"
#include "ui/console.h"
#include "ui/qip.h"
#include "qapi/qmp/types.h"
#include "qmp-commands.h"
#include "qemu/config-file.h"

#define DO_LOG_SYSLOG
//#define DO_LOG_STDERR

#ifdef DO_LOG_SYSLOG
#define DPRINTF_SYSLOG(msg, ...) do { char ts[256]; time_t now = time(NULL); \
            strftime(ts, 20, "%Y-%m-%d %H:%M:%S", localtime(&now)); \
            syslog(LOG_NOTICE, "[%s] %s():L%d: " msg, \
            ts, __FUNCTION__, __LINE__, ## __VA_ARGS__); } while(0)
#else
#define DPRINTF_SYSLOG(msg, ...) do { } while(0)
#endif

#ifdef DO_LOG_STDERR
#define DPRINTF_STDERR(msg, ...) do { char ts[256]; time_t now = time(NULL); \
            strftime(ts, 20, "%Y-%m-%d %H:%M:%S", localtime(&now)); \
            fprintf(stderr, "[%s] %s():L%d: " msg, \
            ts, __FUNCTION__, __LINE__, ## __VA_ARGS__); } while(0)
#else
#define DPRINTF_STDERR(msg, ...) do { } while(0)
#endif

#define DPRINTF(msg, ...) if (qip_debug_mode) do { \
    DPRINTF_SYSLOG(msg, ## __VA_ARGS__); \
    DPRINTF_STDERR(msg, ## __VA_ARGS__); \
} while(0)

#define KEY_MAP_SIZE 256

static int qip_debug_mode = 0;
int using_qip = 0;

typedef struct QipState {
    /* display required for absolute devices & scaling */
    int display_size_x;
    int display_size_y;

    /* scaling options for mouse inputs */
    double mouse_scale_x;
    double mouse_scale_y;

    /* maintain mouse abs positioning coords */
    int absolute_mouse_x;
    int absolute_mouse_y;

    /* listen for mouse mode changes */
    Notifier mouse_mode_notifier;

    /* track keyboard led state */
    int kbd_led_state;

    /* track key downs */
    uint8_t key_down_map[KEY_MAP_SIZE];

    /* display change listener */
    DisplayChangeListener dcl;
} QipState;

static QipState qip_state = {
    .display_size_x = 1920,
    .display_size_y = 1200,
    .mouse_scale_x = 1.0,
    .mouse_scale_y = 1.0,
    .absolute_mouse_x = 0,
    .absolute_mouse_y = 0,
    .mouse_mode_notifier = {},
    .kbd_led_state = 0,
    .key_down_map = {0,},
};

/* *INDENT-OFF* */
static QemuOptsList qemu_qip_opts = {
    .name = "qip",
    .head = QTAILQ_HEAD_INITIALIZER(qemu_qip_opts.head),
    .desc = {
                {
                .name = "debug",
                .type = QEMU_OPT_NUMBER,
                },
                {
                /* end of list */
                }
            },
};
/* *INDENT-ON* */

/* send mouse mode event over qmp  */
static void qip_qmp_mouse_mode_event(QipState * qss, int abs)
{
    QObject *mouse_status;

    DPRINTF("entry\n");

    if (!qss) {
        return;
    }

    qmp_marshal_input_query_mouse_status(NULL, NULL, &mouse_status);

    monitor_protocol_event(QEVENT_QIP_MOUSE_MODE_UPDATE, mouse_status);
}

/* listener to detect changes between absolute/relative mouse */
static void mouse_mode_notifier(Notifier * notifier, void *opaque)
{
    QipState *qss;

    DPRINTF("entry\n");

    qss = container_of(opaque, QipState, mouse_mode_notifier);

    DPRINTF("mouse is_absolute: %d\n", (int)kbd_mouse_is_absolute());

    qip_qmp_mouse_mode_event(qss, kbd_mouse_is_absolute());
}

/* send mouse mode event over qmp  */
static void qip_qmp_kbd_leds_event(QipState * qss)
{
    QObject *kbd_leds;

    DPRINTF("entry\n");

    if (!qss) {
        return;
    }

    qmp_marshal_input_query_kbd_leds(NULL, NULL, &kbd_leds);

    monitor_protocol_event(QEVENT_QIP_KBD_LEDS_UPDATE, kbd_leds);
}

/* listener for led updates - send updates to server */
static void kbd_leds(void *opaque, int ledstate)
{
    QipState *qss = (QipState *) opaque;

    DPRINTF("kbd_leds(): ledstate=0x%x\n", ledstate);

    qss->kbd_led_state = ledstate;

    qip_qmp_kbd_leds_event(qss);
}

/* listener for gfx_switch to detect display changes */
static void gfx_switch(DisplayChangeListener *dcl,
                           DisplaySurface *ds)
{
    //QipState *qss = container_of(dcl, QipState, dcl);

    if (ds) {
        DPRINTF("old width=%d, height=%d\n",
                qip_state.display_size_x, qip_state.display_size_y);
        qip_state.display_size_x = surface_width(ds);
        qip_state.display_size_y = surface_height(ds);
        DPRINTF("new width=%d, height=%d\n",
                qip_state.display_size_x, qip_state.display_size_y);
    } else {
        DPRINTF("iss_gfx_switch: nothing\n");
    }
}

static const DisplayChangeListenerOps dcl_ops = {
    .dpy_name        = "qip",
    .dpy_gfx_switch  = gfx_switch,
};

/* process incoming keycode */
void qmp_send_keycode(int64_t keycode, bool released, Error ** errp)
{
    QipState *qss = &qip_state;

    DPRINTF("keycode=%d released=%d\n", (int)keycode, (int)released);

    if (keycode < 0 || keycode >= sizeof(qss->key_down_map)) {
        DPRINTF("ignoring invalid keycode=0x%" PRIx64 "\n", keycode);
        return;
    }

    if (released) {
        if (qss->key_down_map[keycode] == 0) {
            DPRINTF("ignoring invalid keyup event for keycode=0x%" PRIx64 "\n",
                    keycode);
        } else {
            qss->key_down_map[keycode] = 0;

            /* send keycode with extended code if grey */
            if (keycode & SCANCODE_GREY) {
                kbd_put_keycode(SCANCODE_EMUL0);
            }
            kbd_put_keycode(keycode | SCANCODE_UP);
        }
    } else {
        if (keycode & SCANCODE_GREY) {
            kbd_put_keycode(SCANCODE_EMUL0);
        }
        qss->key_down_map[keycode] = 1;
        kbd_put_keycode(keycode & SCANCODE_KEYCODEMASK);
    }
}

/* process incoming absolute mouse input */
void qmp_send_mouse_abs(int64_t x, int64_t y, int64_t z, MouseButtons * buttons,
                        Error ** errp)
{
    //QipState *qss = &qip_state;
    int mb = 0;

    if (buttons->left) {
        mb |= MOUSE_EVENT_LBUTTON;
    }
    if (buttons->middle) {
        mb |= MOUSE_EVENT_MBUTTON;
    }
    if (buttons->right) {
        mb |= MOUSE_EVENT_RBUTTON;
    }

    DPRINTF("x=%" PRId64 "d, y=%" PRId64 "d, z=%" PRId64 "d buttons=0x%x\n",
            x, y, z, mb);

    /* handle the 'relatively' hard case - har har */
    if (!kbd_mouse_is_absolute()) {
        /* TODO: Convert abs to relative... */
        DPRINTF("ignoring abs event as mouse is currently relative...\n");
        return;
    }

    kbd_mouse_event(x, y, z, mb);
}

/* process incoming relative mouse input */
void qmp_send_mouse_rel(int64_t dx, int64_t dy, int64_t dz,
                        MouseButtons * buttons, Error ** errp)
{
    QipState *qss = &qip_state;
    int mb = 0;

    if (buttons->left) {
        mb |= MOUSE_EVENT_LBUTTON;
    }
    if (buttons->middle) {
        mb |= MOUSE_EVENT_MBUTTON;
    }
    if (buttons->right) {
        mb |= MOUSE_EVENT_RBUTTON;
    }

    DPRINTF("dx=%" PRId64 "d, dy=%" PRId64 "d, dz=%" PRId64 "d buttons=0x%x\n",
            dx, dy, dz, mb);

    /* handle the 'relatively' simple case - har har */
    if (!kbd_mouse_is_absolute()) {
        kbd_mouse_event(dx, dy, dz, mb);
        return;
    }

    /* convert relative to absolute */
    DPRINTF("abs mouse: pre-correction x=%d\n", qss->absolute_mouse_x);
    qss->absolute_mouse_x += (int)
        (qss->mouse_scale_x * dx * 0x7FFF) / (qss->display_size_x - 1);

    if (qss->absolute_mouse_x < 0) {
        qss->absolute_mouse_x = 0;
    } else if (qss->absolute_mouse_x > 0x7FFF) {
        qss->absolute_mouse_x = 0x7FFF;
    }
    DPRINTF("abs mouse: post-correction x=%d\n", qss->absolute_mouse_x);

    DPRINTF("abs mouse: pre-correction y=%d\n", qss->absolute_mouse_y);
    qss->absolute_mouse_y += (int)
        (qss->mouse_scale_y * dy * 0x7FFF) / (qss->display_size_y - 1);

    if (qss->absolute_mouse_y < 0) {
        qss->absolute_mouse_y = 0;
    } else if (qss->absolute_mouse_y > 0x7FFF) {
        qss->absolute_mouse_y = 0x7FFF;
    }
    DPRINTF("abs mouse: post-correction y=%d\n", qss->absolute_mouse_y);

    //DPRINTF("abs mouse: pre-correction z=%d\n", qss->absolute_mouse_z);
    //qss->absolute_mouse_z += dz;
    //DPRINTF("abs mouse: post-correction z=%d\n", qss->absolute_mouse_z);

    kbd_mouse_event(qss->absolute_mouse_x, qss->absolute_mouse_y, dz, mb);
}

/* process incoming keyboard reset request */
void qmp_send_kbd_reset(Error ** errp)
{
    QipState *qss = &qip_state;
    int i;

    DPRINTF("keyboard reset\n");

    for (i = 0; i < sizeof(qss->key_down_map); i++) {
        if (qss->key_down_map[i]) {
            DPRINTF("reset key=%d (0x%x) up\n", i, i);

            qss->key_down_map[i] = 0;

            if (i & SCANCODE_GREY) {
                kbd_put_keycode(SCANCODE_EMUL0);
            }

            kbd_put_keycode(i | SCANCODE_UP);
        }
    }
}

/* process incoming display size update */
void qmp_send_display_size(int64_t x, int64_t y, Error ** errp)
{
    QipState *qss = &qip_state;

    qss->display_size_x = x;
    qss->display_size_y = y;
}

/* process incoming mouse scaler update */
void qmp_send_mouse_scale(double x, double y, Error ** errp)
{
    QipState *qss = &qip_state;

    qss->mouse_scale_x = x;
    qss->mouse_scale_y = y;
}

/* process incoming query for mouse status */
MouseStatus *qmp_query_mouse_status(Error ** errp)
{
    QipState *qss = &qip_state;
    MouseStatus *mouse_status;

    DPRINTF("entry\n");

    mouse_status = g_malloc0(sizeof(*mouse_status));

    mouse_status->absolute = kbd_mouse_is_absolute();
    mouse_status->x = qss->absolute_mouse_x;
    mouse_status->y = qss->absolute_mouse_y;

    return mouse_status;
}

/* process incoming query for xen status */
XenStatus *qmp_query_xen_status(Error ** errp)
{
    //QipState *qss = &qip_state;
    XenStatus *xen_status;

    DPRINTF("entry\n");

    xen_status = g_malloc0(sizeof(*xen_status));

    if (xen_domid > 0) {
        xen_status->xen = true;
        xen_status->domain = (int64_t) xen_domid;
    } else {
        xen_status->xen = false;
        xen_status->domain = -1;
    }

    return xen_status;
}

/* process incoming query for process info */
ProcessInfo *qmp_query_process_info(Error ** errp)
{
    ProcessInfo *p_info;

    DPRINTF("entry\n");

    p_info = g_malloc0(sizeof(*p_info));

    p_info->pid = (int64_t) getpid();

    return p_info;
}

/* process incoming query for keyboard led status */
KbdLedStatus *qmp_query_kbd_leds(Error ** errp)
{
    QipState *qss = &qip_state;
    KbdLedStatus *led_status;

    DPRINTF("entry\n");

    led_status = g_malloc0(sizeof(*led_status));

    if (qss->kbd_led_state & QEMU_SCROLL_LOCK_LED) {
        led_status->scroll = true;
    }
    if (qss->kbd_led_state & QEMU_CAPS_LOCK_LED) {
        led_status->caps = true;
    }
    if (qss->kbd_led_state & QEMU_NUM_LOCK_LED) {
        led_status->num = true;
    }

    return led_status;
}

/* qip initialization function */
void qip_init(DisplayState * ds)
{
    QipState *qss = &qip_state;

    QemuOpts *opts;

#ifdef DO_LOG_SYSLOG
    /* prep syslog if logging there... */
    openlog("qemu", LOG_CONS | LOG_PID, LOG_USER);
#endif

    DPRINTF("entry\n");

    opts = QTAILQ_FIRST(&qemu_qip_opts.head);

    if (!opts) {
        fprintf(stderr, "qip_init(): invalid opts\n");
        exit(1);
    }

    qip_debug_mode = (int)qemu_opt_get_number(opts, "debug", 0);

    qss->dcl.ops = &dcl_ops;
    register_displaychangelistener(&qss->dcl);

    qemu_add_led_event_handler(kbd_leds, qss);

    qss->mouse_mode_notifier.notify = mouse_mode_notifier;
    qemu_add_mouse_mode_change_notifier(&qss->mouse_mode_notifier);

    DPRINTF("end\n");
}

static void qip_register_config(void)
{
    qemu_add_opts(&qemu_qip_opts);
}

machine_init(qip_register_config);
