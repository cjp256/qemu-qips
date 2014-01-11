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

#ifndef QIPS_INPUT_BACKEND_H
#define QIPS_INPUT_BACKEND_H

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include "qips/qips.h"

typedef struct QipsMouseButtons {
    bool left;
    bool middle;
    bool right;
} QipsMouseButtons;

typedef struct {
    bool(*init) (void);
    bool(*cleanup) (void);
} QipsInputBackend;

void qips_input_backend_register(const QipsInputBackend * backend);

bool qips_input_backend_init(void);

bool qips_input_backend_cleanup(void);

void qips_input_backend_key_event(int64_t timestamp_usec,
                                  int scancode, int key_status);

void qips_input_backend_abs_mouse_event(int64_t timestamp_usec,
                                        int x, int y, int z,
                                        QipsMouseButtons * buttons);

void qips_input_backend_rel_mouse_event(int64_t timestamp_usec,
                                        int dx, int dy, int dz,
                                        QipsMouseButtons * buttons);

void qips_send_focused_client_message(char *msg, size_t sz, bool sync);

void qips_domain_switch_right(void);

void qips_domain_switch_left(void);

extern int input_backend_debug_mode;
extern int evdev_debug_mode;

#endif
