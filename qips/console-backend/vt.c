/*
 * Copyright (c) 2013 Chris Patterson <cjp256@gmail.com>
 *
 * With bits adapted/referenced from kbd/src/vlock/vt.c:
 * Copyright (C) 1994-1998  Michael K. Johnson <johnsonm@redhat.com>
 * Copyright (C) 2002, 2004, 2005  Dmitry V. Levin <ldv@altlinux.org>
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
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <signal.h>
#include <sys/vt.h>
#include <sys/kd.h>
#include <termios.h>

#include "console-backend.h"
#include "vt.h"

#define VT_DEFAULT_TARGET_TTY "/dev/tty9"

static const char *vt_target_tty = VT_DEFAULT_TARGET_TTY;

/* data saved for lock & unlock/restore */
static struct termios vt_term_saved;
static struct vt_mode vt_mode_saved;
static struct sigaction vt_sigusr1_handler_saved;
static struct sigaction vt_sigusr2_handler_saved;
static int tty_index = 9;
static int tty_index_save = 1;

/* Is console switching currently disabled? */
static bool console_locked = false;

/* save current settings & lock terminal */
static void termios_lock(void)
{
    struct termios term_locked;

    /* save current termios settings */
    tcgetattr(STDIN_FILENO, &vt_term_saved);
    memcpy(&term_locked, &vt_term_saved, sizeof(term_locked));

    /* disable echoing and character command signals */
    term_locked.c_lflag &= ~(ECHO | ISIG);
    tcsetattr(STDIN_FILENO, TCSANOW, &term_locked);
}

/* restore terminal */
static void termios_restore(void)
{
    tcsetattr(STDIN_FILENO, TCSANOW, &vt_term_saved);
}

/* acknowledge signal & deny vt switch */
static void vt_console_switch_away_deny(int __attribute__ ((__unused__)) n)
{
    ioctl(STDIN_FILENO, VT_RELDISP, 0);
}

/* acknowledge signal & allow vt switch */
static void vt_console_switch_to_allow(int __attribute__ ((__unused__)) n)
{
    ioctl(STDIN_FILENO, VT_RELDISP, VT_ACKACQ);
}

/* update vt to prevent vt switching */
static bool vt_console_lock(void)
{
    struct vt_stat vt_stat;
    struct vt_mode vt_mode_locked;
    struct sigaction sa;
    sigset_t sig;

    DPRINTF("entry\n");

    /* backup current console mode */
    if (ioctl(STDIN_FILENO, VT_GETMODE, &vt_mode_saved) < 0) {
        perror("VT_GETMODE failed");
        return false;
    }

    /* remember user vt index */
    if (ioctl(STDIN_FILENO, VT_GETSTATE, &vt_stat) < 0) {
        perror("VT_GETSTATE failed");
        return false;
    }

    DPRINTF("v_active=%d\n", (int)vt_stat.v_active);
    DPRINTF("v_state=0x%x\n", (int)vt_stat.v_state);
    DPRINTF("v_signal=0x%x\n", (int)vt_stat.v_signal);

    tty_index_save = vt_stat.v_active;

    DPRINTF("remembering term=%d\n", tty_index_save);

    /* switch to qemu-iss console */
    if (ioctl(STDIN_FILENO, VT_ACTIVATE, tty_index) < 0) {
        perror("VT_ACTIVATE failed");
        return false;
    }

    if (ioctl(STDIN_FILENO, VT_WAITACTIVE, tty_index) < 0) {
        perror("VT_WAITACTIVE failed");
        return false;
    }

    /* copy current console mode for modification */
    memcpy(&vt_mode_locked, &vt_mode_saved, sizeof(vt_mode_locked));

    sigemptyset(&(sa.sa_mask));
    sa.sa_flags = SA_RESTART;

    /* set SIGUSR1 to handle vt release events which need to be denied */
    sa.sa_handler = vt_console_switch_away_deny;
    sigaction(SIGUSR1, &sa, &vt_sigusr1_handler_saved);
    vt_mode_locked.relsig = SIGUSR1;

    /* set SIGUSR2 to handle vt acquire events which should be allowed */
    sa.sa_handler = vt_console_switch_to_allow;
    sigaction(SIGUSR2, &sa, &vt_sigusr2_handler_saved);
    vt_mode_locked.acqsig = SIGUSR2;

    /* allow SIGUSR1 & SIGUSR2 signals */
    sigemptyset(&sig);
    sigaddset(&sig, SIGUSR1);
    sigaddset(&sig, SIGUSR2);
    sigprocmask(SIG_UNBLOCK, &sig, 0);

    /* we be in charge of vt handling */
    vt_mode_locked.mode = VT_PROCESS;

    /* just do it. */
    if (ioctl(STDIN_FILENO, VT_SETMODE, &vt_mode_locked) < 0) {
        perror("VT_SETMODE failed");
        return false;
    }

    return true;
}

/* restore vt to pre-locked conditions */
static bool vt_console_restore(void)
{
    DPRINTF("entry\n");

    /* restore saved vt mode */
    if (ioctl(STDIN_FILENO, VT_SETMODE, &vt_mode_saved) < 0) {
        perror("VT_SETMODE failed");
        return false;
    }

    /* restore sigusr1 signal handler */
    sigaction(SIGUSR1, &vt_sigusr1_handler_saved, NULL);

    /* restore user vt if different than qemu-iss */
    if (tty_index_save != tty_index) {
        DPRINTF("switching back to term=%d\n", tty_index_save);

        /* do the switch */
        if (ioctl(STDIN_FILENO, VT_ACTIVATE, tty_index_save) < 0) {
            perror("VT_ACTIVATE");
        }

        /* wait until switch is complete */
        if (ioctl(STDIN_FILENO, VT_WAITACTIVE, tty_index_save) < 0) {
            perror("VT_WAITACTIVE");
        }
    }

    return true;
}

static bool vt_init(void)
{
    pid_t pid, ppid, pgid, ppgid, ttypgid, sid, psid;
    int fd;

    DPRINTF("entry\n");

    if ((fd = open(vt_target_tty, O_RDWR | O_NOCTTY)) < 0) {
        DPRINTF("unable to open tty=%s\n", vt_target_tty);
        return false;
    }

    if (!isatty(fd)) {
        DPRINTF("errr %s not a tty!!??\n", vt_target_tty);
        close(fd);
        return false;
    }

    DPRINTF("valid tty fd=%d\n", fd);

    /* Get current process id */
    pid = getpid();
    DPRINTF("pid=%jd\n", (intmax_t) pid);

    /* Get parent process id */
    ppid = getppid();
    DPRINTF("ppid=%jd\n", (intmax_t) ppid);

    /* Get current process group id */
    pgid = getpgid(pid);
    DPRINTF("pgid=%jd\n", (intmax_t) pgid);

    /* Get parent process group id */
    ppgid = getpgid(ppid);
    DPRINTF("ppgid=%jd\n", (intmax_t) ppgid);

    /* Get process group id for tty */
    ttypgid = tcgetpgrp(fd);
    DPRINTF("ttypgid=%jd\n", (intmax_t) ttypgid);

    /* Get current session id */
    sid = getsid(pid);
    DPRINTF("sid=%jd\n", (intmax_t) sid);

    /* Get parent session id */
    psid = getsid(ppid);
    DPRINTF("psid=%jd\n", (intmax_t) psid);

    /*
     * If self and parent group id matches tty's group id,
     * we are good to go.
     */
    if (ppid == ttypgid || ppgid == ttypgid) {
        DPRINTF("process id matches tty\n");
        return true;
    }

    /* if parent is not init - daemonize */
    if (ppid != 1) {
        pid = fork();

        if (pid < 0) {
            perror("fork failed");
            exit(-1);
        }

        /* exit if parent */
        if (pid > 0) {
            exit(0);
        }
    }

    /*
     * create a new session if not a process group leader
     */
    if (pid != sid) {
        DPRINTF("creating new session...\n");

        /* set new session */
        setsid();
        sid = getsid(pid);
        DPRINTF("new sid=%jd\n", (intmax_t) sid);
    }

    /* get the controlling terminal */
    ioctl(fd, TIOCSCTTY, (char *)1);

    /* we need to reopen all stdio descriptions (in, out, err) under new tty */
    dup2(fd, STDIN_FILENO);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);

    /* close old tty handle if it is not stdio */
    if (fd > 2) {
        DPRINTF("closing fd=%d\n", fd);
        close(fd);
    }

    return true;
}

static bool vt_lock(void)
{
    DPRINTF("entry\n");

    if (console_locked) {
        return true;
    }

    DPRINTF("locking console...\n");
    vt_console_lock();
    termios_lock();
    console_locked = true;

    return true;
}

static bool vt_release(void)
{
    DPRINTF("entry\n");

    if (console_locked) {
        DPRINTF("restoring console...\n");
        vt_console_restore();
        termios_restore();
        console_locked = false;
    }

    return true;
}

static int vt_get_ledstate(void)
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

static bool vt_set_ledstate(int qips_led_state)
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

static bool vt_cleanup(void)
{
    DPRINTF("entry\n");

    return true;
}

static const QipsConsoleBackend vt = {
    .init = vt_init,
    .lock = vt_lock,
    .release = vt_release,
    .get_ledstate = vt_get_ledstate,
    .set_ledstate = vt_set_ledstate,
    .cleanup = vt_cleanup,
};

const QipsConsoleBackend *vt_console_backend_register(void)
{
    qips_console_backend_register(&vt);

    return &vt;
}
