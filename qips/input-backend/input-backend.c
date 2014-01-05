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

#include "input-backend.h"
#include "ui/keymaps.h"
#include "console.h"

#define INPUT_DPRINTF(msg, ...) do { \
    if (input_backend_debug_mode) fprintf(stderr, "%s():L%d: " msg "\n", \
            __FUNCTION__, __LINE__, ## __VA_ARGS__); \
} while(0)

static uint8_t key_down_map[256] = { 0, };

static const QipsInputBackend *input_backend = NULL;

void qips_input_backend_register(const QipsInputBackend * backend)
{
    input_backend = backend;
}

bool qips_input_backend_init(void)
{
    return input_backend->init();
}

bool qips_input_backend_cleanup(void)
{
    return input_backend->cleanup();
}

#if 0
static void qips_input_backend_dump_key_map(void)
{
    int i;

    fprintf(stderr, "key map down:\n");

    for (i = 0; i < 256; i++) {
        if (key_down_map[i]) {
            fprintf(stderr, "( %d -- 0x%x) ", i, i);
        }
    }

    fprintf(stderr, "\nmodifiers:\n");

}
#endif

static void qips_input_backend_key_map(int scancode, int key_status)
{
    switch (key_status) {
    case 0:
        INPUT_DPRINTF("KEY_RELEASED (%d)\n", scancode);
        key_down_map[scancode & SCANCODE_KEYMASK] = 0;
        break;
    case 1:
        INPUT_DPRINTF("KEY_DEPRESSED (%d)\n", scancode);
        key_down_map[scancode & SCANCODE_KEYMASK] = 1;
        break;
    case 2:
        INPUT_DPRINTF("KEY_REPEAT (%d)\n", scancode);
        key_down_map[scancode & SCANCODE_KEYMASK] = 1;
        break;
    case 3:
        DPRINTF("KEY_WTF (%d)\n", scancode);
        return;
    }

    /* Check magic keys (left ctrl + left alt + left/right) */
    if (key_down_map[0x1d] && key_down_map[0x38] && key_down_map[0xcb]) {
        DPRINTF("switch left detected\n");
        qips_domain_switch_left();
        return;
    }

    if (key_down_map[0x1d] && key_down_map[0x38] && key_down_map[0xcd]) {
        DPRINTF("switch right detected\n");
        qips_domain_switch_right();
        return;
    }
}

void qips_input_backend_key_event(int64_t timestamp_usec,
                                  int scancode, int key_status)
{
    char buf[1024];

    snprintf(buf, sizeof(buf),
             "{ \"execute\": \"send-keycode\", \"arguments\": { \"keycode\": %d, \"released\": %s } }\r\n",
             scancode, (key_status == 0) ? "true" : "false");

    qips_send_focused_client_message(buf, strlen(buf), false);

    qips_input_backend_key_map(scancode, key_status);
}

void qips_input_backend_abs_mouse_event(int64_t timestamp_usec,
                                        int x, int y, int z,
                                        QipsMouseButtons * buttons)
{
    char buf[1024];

    snprintf(buf, sizeof(buf),
             "{ \"execute\": \"send-mouse-abs\","
             " \"arguments\": { \"x\": %d, \"y\": %d, \"z\": %d,"
             " \"buttons\": { \"left\": %s, \"middle\": %s,"
             " \"right\": %s } } }\r\n",
             x, y, z,
             buttons->left ? "true" : "false",
             buttons->middle ? "true" : "false",
             buttons->right ? "true" : "false");

    qips_send_focused_client_message(buf, strlen(buf), false);
}

void qips_input_backend_rel_mouse_event(int64_t timestamp_usec,
                                        int dx, int dy, int dz,
                                        QipsMouseButtons * buttons)
{
    char buf[1024];

    snprintf(buf, sizeof(buf),
             "{ \"execute\": \"send-mouse-rel\","
             " \"arguments\": { \"dx\": %d, \"dy\": %d, \"dz\": %d,"
             " \"buttons\": { \"left\": %s, \"middle\": %s,"
             " \"right\": %s } } }\r\n",
             dx, dy, dz,
             buttons->left ? "true" : "false",
             buttons->middle ? "true" : "false",
             buttons->right ? "true" : "false");

    qips_send_focused_client_message(buf, strlen(buf), false);
}
