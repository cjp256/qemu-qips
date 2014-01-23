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

#include <stdlib.h>
#include "xengt.h"

static bool xengt_init(void)
{
    DPRINTF("entry");
    return true;
}

static bool xengt_prep_switch(bool leaving_control)
{
    return true;
}

static bool xengt_switch(int domain, pid_t pid, int slot)
{
    char cmd[4096];

    DPRINTF("switch to domain=%d pid=%ld!\n", domain, (long)pid);

    /* TODO: I'm sure we can do better than this... */
    snprintf(cmd, sizeof(cmd),
             "echo %d > /sys/kernel/vgt/control/foreground_vm", domain);

    if (system(cmd)) {
        DPRINTF("system() failed...");
    }

    return true;
}

static bool xengt_cleanup(void)
{
    DPRINTF("entry");
    return true;
}

static const QipsConsoleFrontend xengt = {
    .init = xengt_init,
    .prep_switch = xengt_prep_switch,
    .domain_switch = xengt_switch,
    .cleanup = xengt_cleanup,
};

const QipsConsoleFrontend *xengt_console_frontend_register(void)
{
    qips_console_frontend_register(&xengt);

    return &xengt;
}
