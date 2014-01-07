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
#ifndef QIPS_H
#define QIPS_H

#include <syslog.h>
#include "qapi/qmp/types.h"

extern int qips_debug_mode;

#define DO_LOG_SYSLOG
//#define DO_LOG_STDERR

#ifdef DO_LOG_SYSLOG
#define DPRINTF_SYSLOG(msg, ...) do syslog(LOG_NOTICE, "%s():L%d: " msg, \
            __FUNCTION__, __LINE__, ## __VA_ARGS__); while(0)
#else
#define DPRINTF_SYSLOG(msg, ...) do { } while(0)
#endif

#ifdef DO_LOG_STDERR
#define DPRINTF_STDERR(msg, ...) do fprintf(stderr, "%s():L%d: " msg, \
            __FUNCTION__, __LINE__, ## __VA_ARGS__); while(0)
#else
#define DPRINTF_STDERR(msg, ...) do { } while(0)
#endif

#define DPRINTF(msg, ...) if (qips_debug_mode) do { \
    DPRINTF_SYSLOG(msg, ## __VA_ARGS__); \
    DPRINTF_STDERR(msg, ## __VA_ARGS__); \
} while(0)

#endif                          /* QIPS_H */
