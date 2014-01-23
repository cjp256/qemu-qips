/*
 * Copyright (c) 2013 Chris Patterson <cjp256@gmail.com>
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

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <X11/keysym.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <sys/vt.h>
#include <sys/kd.h>
#include <sys/ioctl.h>
#include "xfront.h"

static Display *xfront_dpy = NULL;
static Window saved_window = 0;

static void xfront_raise_window(Window win)
{
#if 0
    /* Option #1: use XRaiseWindow with XSetInputFocus */

    DPRINTF("setting focus for 0x%lx\n", (long) win);

    XSetInputFocus(xfront_dpy, win, RevertToPointerRoot, CurrentTime);

    DPRINTF("XSetInputFocus complete for 0x%lx\n", (long) win);

    if (!XRaiseWindow(xfront_dpy, win)) {
        DPRINTF("XRaiseWindow error?\n");
    }

    DPRINTF("XRaiseWindow complete for 0x%lx\n", (long) win);

#else
    /* Option #2: Do it the "better" way with an event message */
    XEvent event;

    DPRINTF("setting focus for 0x%lx\n", (long) win);

    event.xclient.type = ClientMessage;
    event.xclient.serial = 0;
    event.xclient.send_event = True;
    event.xclient.message_type = XInternAtom(xfront_dpy, "_NET_ACTIVE_WINDOW", False);
    event.xclient.window = win;
    event.xclient.format = 32;
    event.xclient.data.l[0] = 0;
    event.xclient.data.l[1] = 0;
    event.xclient.data.l[2] = 0;
    event.xclient.data.l[3] = 0;
    event.xclient.data.l[4] = 0;

    if (!XSendEvent(xfront_dpy, DefaultRootWindow(xfront_dpy),
        False, SubstructureRedirectMask | SubstructureNotifyMask, &event)) {
        DPRINTF("XSendEvent failed\n");
        return;
    }

    XMapRaised(xfront_dpy, win);
#endif
    XSync(xfront_dpy, False);
}

static void xfront_find_window_by_name(Window win, const char *name,
                                            void (*cb)(Window))
{
    Window unused;
    Window *win_list;
    unsigned int nwin_list;
    int i;
    char *window_name;

    //DPRINTF("traversing for name=%s (win=0x%lx)\n", name, (long) win);

    XFetchName(xfront_dpy, win, &window_name);

    //DPRINTF("current window name: %s\n", window_name);

    if (window_name && strncmp(window_name, name, strlen(name)) == 0) {
        DPRINTF("match: 0x%lx\n", (long) win);
        cb(win);
    }

    /* gather up children */
    if (!XQueryTree(xfront_dpy, win, &unused, &unused, &win_list, &nwin_list)) {
        return;
    }

    /* recurse for all children */
    for (i = 0; i < nwin_list; i++)
    {
        xfront_find_window_by_name(win_list[i], name, cb);
    }

    if (win_list) {
        //DPRINTF("freeing win_list=%p\n", win_list);
        XFree((char *)win_list);
    }
}

static void xfront_raise_windows_by_slot(int slot)
{
    char name[256];

    DPRINTF("raising for slot=%d\n", slot);

    /* find windows with form: QEMU (slot-%d) */
    snprintf(name, sizeof(name), "QEMU (slot-%d)", slot);
    xfront_find_window_by_name(DefaultRootWindow(xfront_dpy), name,
                               xfront_raise_window);
}

static void xfront_window_focus_save(void)
{
#if 0
    /* Option 1: save window under mouse (focus follows mouse) */

    Window win, unused1;
    int unused2;
    unsigned int unused3;


    do {
        XQueryPointer(xfront_dpy, DefaultRootWindow(xfront_dpy), &unused1,
                      &win, &unused2, &unused2, &unused2, &unused2,
                      &unused3);
    } while (win <= 0);

    DPRINTF("saving: 0x%lx\n", (long) win);
    saved_window = win;

#else
    /* Option 2: use _NET_ACTIVE_WINDOW */

    Atom netactivewindow, real;
    Window win;
    int format;
    unsigned long extra, n;
    unsigned char *data;

    netactivewindow = XInternAtom(xfront_dpy, "_NET_ACTIVE_WINDOW", False);

    if (XGetWindowProperty(xfront_dpy, XDefaultRootWindow(xfront_dpy),
        netactivewindow, 0, ~0, False,
        AnyPropertyType, &real, &format, &n, &extra,
        &data) != Success) {

        DPRINTF("XGetWindowProperty failed :(...\n");
        return;
    }

    win = *(unsigned long *) data;

    XFree (data);

    DPRINTF("saving: 0x%lx\n", (long) win);
    saved_window = win;
#endif
}

static void xfront_window_focus_restore(void)
{
    DPRINTF("entry\n");

    if (saved_window <= 0) {
        DPRINTF("skipping restore for 0x%lx\n", (long) saved_window);
    }

    xfront_raise_window(saved_window);
}

static bool xfront_init(void)
{
    DPRINTF("entry\n");

    xfront_dpy = XOpenDisplay(0);

    if (!xfront_dpy) {
        fprintf(stderr, "cannot open display\n");
        return false;
    }

    return true;
}

static bool xfront_prep_switch(bool leaving_control)
{
    DPRINTF("entry\n");

    if (leaving_control) {
        xfront_window_focus_save();
    }

    return true;
}

static bool xfront_switch(int domain, pid_t pid, int slot)
{
    DPRINTF("switch to domain=%d pid=%ld slot=%d!\n", domain, (long)pid, slot);

    if (slot == 0) {
        xfront_window_focus_restore();
    } else {
        xfront_raise_windows_by_slot(slot);
    }

    return true;
}

static bool xfront_cleanup(void)
{
    DPRINTF("entry\n");
    XCloseDisplay(xfront_dpy);
    xfront_dpy = NULL;
    return true;
}

static const QipsConsoleFrontend xfront = {
    .init = xfront_init,
    .prep_switch = xfront_prep_switch,
    .domain_switch = xfront_switch,
    .cleanup = xfront_cleanup,
};

const QipsConsoleFrontend *xfront_console_frontend_register(void)
{
    qips_console_frontend_register(&xfront);

    return &xfront;
}
