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

#ifndef QIPS_CONSOLE_FRONTEND_H
#define QIPS_CONSOLE_FRONTEND_H

#include <stdbool.h>
#include <stdio.h>
#include "qips/qips.h"

typedef struct {
    bool(*init) (void);
    bool(*domain_switch) (int);
    bool(*cleanup) (void);
} QipsConsoleFrontend;

void qips_console_frontend_register(const QipsConsoleFrontend * frontend);

bool qips_console_frontend_init(void);

bool qips_console_frontend_domain_switch(int domain);

bool qips_console_frontend_cleanup(void);

#endif
