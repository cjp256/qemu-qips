/*
 * Copyright (c) 2013 Chris Patterson <cjp256@gmail.com>
 *
 * With bits adapted/referenced from suckless-tools/slock/slock.c:
 * Copyright (c) 2006-2012 Anselm R Garbe <anselm@garbe.us>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
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

#include "console-backend.h"
#include "xback.h"

#define COLOR1 "red"
#define COLOR2 "blue"

typedef struct {
    int screen;
    Window root, win;
    Pixmap pmap;
    unsigned long colors[2];
} Lock;

static Lock **locks;
static int nscreens;
static Bool running = True;
static Display *dpy;

static void xback_unlockscreen(Display * dpy, Lock * lock)
{
    if (dpy == NULL || lock == NULL) {
        return;
    }

    XUngrabPointer(dpy, CurrentTime);
    XFreeColors(dpy, DefaultColormap(dpy, lock->screen), lock->colors, 2, 0);
    XFreePixmap(dpy, lock->pmap);
    XDestroyWindow(dpy, lock->win);

    g_free(lock);
}

static Lock *xback_lockscreen(Display * dpy, int screen)
{
    char curs[] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    unsigned int len;
    Lock *lock;
    XColor color, dummy;
    XSetWindowAttributes wa;
    Cursor invisible;

    if (dpy == NULL || screen < 0) {
        return NULL;
    }

    lock = g_malloc0(sizeof(Lock));

    lock->screen = screen;

    lock->root = RootWindow(dpy, lock->screen);

    /* init */
    wa.override_redirect = 1;
    wa.background_pixel = BlackPixel(dpy, lock->screen);
#if 0
    lock->win =
        XCreateWindow(dpy, lock->root, 0, 0, DisplayWidth(dpy, lock->screen),
                      DisplayHeight(dpy, lock->screen), 0, DefaultDepth(dpy,
                                                                        lock->
                                                                        screen),
                      CopyFromParent, DefaultVisual(dpy, lock->screen),
                      CWOverrideRedirect | CWBackPixel, &wa);
#else
    lock->win =
        XCreateWindow(dpy, lock->root, 0, 0, 1,
                    1, 0, DefaultDepth(dpy, lock-> screen),
                    CopyFromParent, DefaultVisual(dpy, lock->screen),
                    CWOverrideRedirect | CWBackPixel, &wa);
#endif
    XAllocNamedColor(dpy, DefaultColormap(dpy, lock->screen), COLOR2, &color,
                     &dummy);
    lock->colors[1] = color.pixel;
    XAllocNamedColor(dpy, DefaultColormap(dpy, lock->screen), COLOR1, &color,
                     &dummy);
    lock->colors[0] = color.pixel;
    lock->pmap = XCreateBitmapFromData(dpy, lock->win, curs, 8, 8);
    invisible =
        XCreatePixmapCursor(dpy, lock->pmap, lock->pmap, &color, &color, 0, 0);
    XDefineCursor(dpy, lock->win, invisible);
    //XMapRaised(dpy, lock->win);

    for (len = 1000; len; len--) {
        if (XGrabPointer
            (dpy, lock->root, False,
             ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
             GrabModeAsync, GrabModeAsync, None, invisible,
             CurrentTime) == GrabSuccess)
            break;
        usleep(1000);
    }
    if (running && (len > 0)) {
        for (len = 1000; len; len--) {
            if (XGrabKeyboard
                (dpy, lock->root, True, GrabModeAsync, GrabModeAsync,
                 CurrentTime)
                == GrabSuccess)
                break;
            usleep(1000);
        }
    }

    running &= (len > 0);
    if (!running) {
        xback_unlockscreen(dpy, lock);
        lock = NULL;
    } else {
        XSelectInput(dpy, lock->root, SubstructureNotifyMask);
    }
    return lock;
}

static bool xback_lock(void)
{
    int screen;
    int nlocks = 0;

    if (!(dpy = XOpenDisplay(0))) {
        fprintf(stderr, "cannot open display");
        return false;
    }

    /* Get the number of screens in display "dpy" and blank them all. */
    nscreens = ScreenCount(dpy);
    locks = g_malloc0(sizeof(Lock *) * nscreens);

    for (screen = 0; screen < nscreens; screen++) {
        if ((locks[screen] = xback_lockscreen(dpy, screen)) != NULL) {
            nlocks++;
        }
    }
    XSync(dpy, False);

    /* Did we actually manage to lock something? */
    if (nlocks == 0) {
        g_free(locks);
        locks = NULL;
        return false;
    }

    return true;
}

/* restore X to pre-locked conditions */
static bool xback_release(void)
{
    int screen;

    if (locks) {
        /* unlock everything and quit. */
        for (screen = 0; screen < nscreens; screen++) {
            xback_unlockscreen(dpy, locks[screen]);
        }

        g_free(locks);
    }

    XCloseDisplay(dpy);
    dpy = NULL;

    return true;
}

static int xback_get_ledstate(void)
{
    int qips_led_state = 0;
    char kbd_leds = 0;

    DPRINTF("entry\n");

    if (ioctl(0, KDGETLED, &kbd_leds)) {
        DPRINTF("KDGETLED failure: %s\n", strerror(errno));
        return 0;
    }

    DPRINTF("KDGETLED = 0x%x\n", kbd_leds);

    if (kbd_leds & LED_SCR) {
        qips_led_state |= QEMU_SCROLL_LOCK_LED;
    }

    if (kbd_leds & LED_NUM) {
        qips_led_state |= QEMU_NUM_LOCK_LED;
    }

    if (kbd_leds & LED_CAP) {
        qips_led_state |= QEMU_CAPS_LOCK_LED;
    }

    DPRINTF("QEMULED = 0x%x\n", qips_led_state);

    return true;
}

static bool xback_set_ledstate(int qips_led_state)
{
    char kbd_leds = 0;

    DPRINTF("entry\n");

    DPRINTF("QEMULED = 0x%x\n", qips_led_state);

    if (ioctl(0, KDGETLED, &kbd_leds)) {
        DPRINTF("KDGETLED failure: %s\n", strerror(errno));
        return 0;
    }

    DPRINTF("KDGETLED = 0x%x\n", kbd_leds);

    kbd_leds = 0;

    if (qips_led_state & QEMU_SCROLL_LOCK_LED) {
        kbd_leds |= LED_SCR;
    }

    if (qips_led_state & QEMU_NUM_LOCK_LED) {
        kbd_leds |= LED_NUM;
    }

    if (qips_led_state & QEMU_CAPS_LOCK_LED) {
        kbd_leds |= LED_CAP;
    }

    DPRINTF("KDSETLED = 0x%x\n", kbd_leds);

    if (ioctl(0, KDSETLED, kbd_leds)) {
        DPRINTF("KDSETLED failure: %s\n", strerror(errno));
        return false;
    }

    if (ioctl(0, KDGETLED, &kbd_leds)) {
        DPRINTF("KDGETLED failure: %s\n", strerror(errno));
        return 0;
    }

    DPRINTF("KDGETLED after KDSETLED = 0x%x\n", kbd_leds);

    /* make the dom0 tty match up if we're looking at it... */
    if (ioctl(0, KDSKBLED, kbd_leds)) {
        DPRINTF("KDSKBLED failure: %s\n", strerror(errno));
        return false;
    }

    if (ioctl(0, KDGKBLED, &kbd_leds)) {
        DPRINTF("KDGKBLED failure: %s\n", strerror(errno));
        return false;
    }

    DPRINTF("KDGKBLED after KDSKBLED = 0x%x\n", kbd_leds);

    return true;
}

static bool xback_init(void)
{
    DPRINTF("entry\n");

    return true;
}

static bool xback_cleanup(void)
{
    DPRINTF("entry\n");

    return true;
}

static const QipsConsoleBackend xback_qcb = {
    .init = xback_init,
    .lock = xback_lock,
    .release = xback_release,
    .get_ledstate = xback_get_ledstate,
    .set_ledstate = xback_set_ledstate,
    .cleanup = xback_cleanup,
};

const QipsConsoleBackend *xback_console_backend_register(void)
{
    qips_console_backend_register(&xback_qcb);

    return &xback_qcb;
}
