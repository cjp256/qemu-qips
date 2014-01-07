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

#include "console-backend.h"

static const QipsConsoleBackend *console_backend = NULL;

void qips_console_backend_register(const QipsConsoleBackend * backend)
{
    console_backend = backend;
}

bool qips_console_backend_init(void)
{
    return console_backend->init();
}

bool qips_console_backend_lock(void)
{
    return console_backend->lock();
}

int qips_console_backend_get_ledstate(void)
{
    int state = console_backend->get_ledstate();

    if (state & QEMU_SCROLL_LOCK_LED) {
        DPRINTF("scroll lock led is set...\n");
    }

    if (state & QEMU_NUM_LOCK_LED) {
        DPRINTF("num lock led is set...\n");
    }

    if (state & QEMU_CAPS_LOCK_LED) {
        DPRINTF("caps lock led is set...\n");
    }

    return state;
}

bool qips_console_backend_set_ledstate(int state)
{
    if (state & QEMU_SCROLL_LOCK_LED) {
        DPRINTF("setting scroll lock led...\n");
    }

    if (state & QEMU_NUM_LOCK_LED) {
        DPRINTF("setting num lock led...\n");
    }

    if (state & QEMU_CAPS_LOCK_LED) {
        DPRINTF("setting caps lock led...\n");
    }

    return console_backend->set_ledstate(state);
}

bool qips_console_backend_release(void)
{
    return console_backend->release();
}

bool qips_console_backend_cleanup(void)
{
    return console_backend->cleanup();
}
