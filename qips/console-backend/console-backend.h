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

#ifndef QIPS_CONSOLE_BACKEND_H
#define QIPS_CONSOLE_BACKEND_H

#include <stdbool.h>
#include <stdio.h>

#include "qips/qips.h"
#include "console.h"

typedef struct {
    bool(*init) (void);
    bool(*lock) (void);
    bool(*release) (void);
    int (*get_ledstate) (void);
     bool(*set_ledstate) (int);
     bool(*cleanup) (void);
} QipsConsoleBackend;

void qips_console_backend_register(const QipsConsoleBackend * backend);

bool qips_console_backend_init(void);

bool qips_console_backend_lock(void);

bool qips_console_backend_release(void);

int qips_console_backend_get_ledstate(void);

bool qips_console_backend_set_ledstate(int);

bool qips_console_backend_cleanup(void);

#endif
