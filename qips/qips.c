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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <linux/input.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/time.h>
#include <termios.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>
#include <sys/inotify.h>

#include "../qemu-common.h"
#include "qlist.h"
#include "qint.h"
#include "qbool.h"
#include "qfloat.h"
#include "qdict.h"
#include "qemu-thread.h"
#include "input-backend/input-backend.h"
#include "input-backend/evdev.h"
#include "console-backend/console-backend.h"
#include "console-backend/vt.h"
#include "console-frontend/console-frontend.h"
#include "console-frontend/xengt.h"
#include "ui/x_keymap.h"
#include "json-streamer.h"
#include "json-parser.h"
#include "qips.h"

int evdev_debug_mode = 0;
int input_backend_debug_mode = 0;
int qips_debug_mode = 0;

#define QIPS_SOCKETS_PATH "/var/run/qips"
#define QIPS_SOCKETS_FMT "/var/run/qips/slot-%d"
#define QIPS_SOCKETS_FMT_BASE "slot-"

typedef struct QipsClient {
    char socket_path[PATH_MAX];
    int socket_fd;
    int domain_id;
    int slot_id;
    int led_state;
    int msg_recv_count;
    int msg_sent_count;
     QTAILQ_ENTRY(QipsClient) next;
    pthread_t socket_listener;
    JSONMessageParser inbound_parser;
} QipsClient;

typedef QTAILQ_HEAD(QipsClientList, QipsClient) QipsClientList;

typedef struct QipsState {
    bool do_quit;

    QemuMutex clients_mutex;
    QipsClientList clients;
    QipsClient *focused_client;

    const QipsInputBackend *input_backend;
    const QipsConsoleBackend *console_backend;
    const QipsConsoleFrontend *console_frontend;
} QipsState;

static QipsState state = {
    .do_quit = false,
    .clients = QTAILQ_HEAD_INITIALIZER(state.clients),
    .focused_client = NULL,
};

static void qips_request_kbd_reset(QipsState * s, QipsClient * client);

static void switch_focused_client(QipsState * s, QipsClient * new_focus,
                                  bool teardown)
{
    QipsClient *old_focus = s->focused_client;

    if (new_focus == NULL) {
        DPRINTF("warning new_focus is NULL!\n");
        return;
    }

    DPRINTF("new focus=%p slot=%d\n", new_focus, new_focus->slot_id);
    DPRINTF("old focus=%p slot=%d\n", old_focus, old_focus->slot_id);

    /* if tearing down - we don't care about state */
    if (!teardown) {
        /* send a keyboard reset to bring up any keys that may have been down */
        qips_request_kbd_reset(s, old_focus);
    }

    /* lock console backend if switching from domain-0 */
    if (s->focused_client->domain_id == 0) {
        s->console_backend->lock();
    }

    s->focused_client = new_focus;

    /* update console frontend */
    s->console_frontend->domain_switch(new_focus->domain_id);

    /* release lock on console backend if switching to domain-0 */
    if (new_focus->domain_id == 0) {
        s->console_backend->release();
    }

    /* update leds */
    DPRINTF("attempting to update led state to 0x%x\n", new_focus->led_state);
    s->console_backend->set_ledstate(new_focus->led_state);
}

void qips_domain_switch_right(void)
{
    QipsState *s = &state;

    QipsClient *new_focus = QTAILQ_NEXT(s->focused_client, next);

    if (new_focus == NULL) {
        /* end of list - go back to top */
        DPRINTF("end of list, moving back to top\n");
        new_focus = QTAILQ_FIRST(&s->clients);
    }

    DPRINTF("new focus=%p, old focus=%p\n", new_focus, s->focused_client);

    switch_focused_client(s, new_focus, false);
}

void qips_domain_switch_left(void)
{
    QipsState *s = &state;

    QipsClient *new_focus =
        QTAILQ_PREV(s->focused_client, QipsClientList, next);

    if (new_focus == NULL) {
        /* top of the list - loop back to tail */
        DPRINTF("top of list, moving back to end");
        new_focus = QTAILQ_LAST(&s->clients, QipsClientList);
    }

    DPRINTF("new focus=%p, old focus=%p\n", new_focus, s->focused_client);

    switch_focused_client(s, new_focus, false);
}

static void client_list_mutex_init(QipsState * s)
{
    qemu_mutex_init(&s->clients_mutex);
}

static void client_list_mutex_lock(QipsState * s)
{
    qemu_mutex_lock(&s->clients_mutex);
}

static void client_list_mutex_unlock(QipsState * s)
{
    qemu_mutex_unlock(&s->clients_mutex);
}

static void client_list_add(QipsState * s, QipsClient * client)
{
    QipsClient *iter;

    DPRINTF("adding client slot id=%d...\n", client->slot_id);

    /* we want to add the client in order of slot id to simply switches */

    client_list_mutex_lock(s);
    QTAILQ_FOREACH(iter, &s->clients, next) {
        if (iter->slot_id > client->slot_id) {
            QTAILQ_INSERT_BEFORE(iter, client, next);
            client_list_mutex_unlock(s);
            return;
        }

        /* XXX: shouldn't happen - but if there is a race, allow add _after_
           existing entry if delete is racing (and losing) */
        if (iter->slot_id == client->slot_id) {
            DPRINTF("WARNING: re-adding slot id=%d...?\n", client->slot_id);
            QTAILQ_INSERT_AFTER(&s->clients, iter, client, next);
            client_list_mutex_unlock(s);
            return;
        }
    }

    /* no larger slot id was found - add to tail */
    QTAILQ_INSERT_TAIL(&s->clients, client, next);
    client_list_mutex_unlock(s);
}

static void client_list_remove(QipsState * s, QipsClient * client)
{
    if (client->slot_id == 0 || client->domain_id == 0) {
        return;
    }

    DPRINTF("removing client slot id=%d...\n", client->slot_id);

    /* check if removing focused client - switch to head/dom0 if so */
    if (client == s->focused_client) {
        switch_focused_client(s, QTAILQ_FIRST(&s->clients), true);
    }

    client_list_mutex_lock(s);
    QTAILQ_REMOVE(&s->clients, client, next);
    client_list_mutex_unlock(s);
}

static void qips_cleanup(QipsState * s)
{
    QipsClient *client = NULL, *tmp = NULL;

    DPRINTF("starting cleanup...\n");

    QTAILQ_FOREACH_SAFE(client, &s->clients, next, tmp) {
        if (client->socket_fd >= 0) {
            close(client->socket_fd);
            DPRINTF("closed fd=%d...\n", client->socket_fd);
        }

        /* remove if not dom0 */
        if (client->domain_id != 0) {
            client_list_remove(s, client);
        }
    }

    /* switch back to dom0 */
    s->console_frontend->domain_switch(0);

    /* release lock on dom0 */
    s->console_backend->release();

    s->console_frontend->cleanup();
    s->console_backend->cleanup();
    s->input_backend->cleanup();

    DPRINTF("complete...\n");
}

static void qips_send_message(QipsState * s, QipsClient * client, char *msg,
                              size_t sz, bool sync)
{
    int sent_count, recv_count;

    if (!client) {
        DPRINTF(" noone is listening :(\n");
        return;
    }

    if (client->slot_id == 0) {
        /* do nothing with dom0 events */
        return;
    }

    DPRINTF("sending msg to client slot=%d domain=%d (fd=%d)\n",
            client->slot_id, client->domain_id, client->socket_fd);

    DPRINTF("msg = %s\n", msg);

    if (sz != strlen(msg)) {
        DPRINTF("WARNING msg sz %zd != strlen(msg) %zd \n", sz, strlen(msg));
    }

    if (client->socket_fd <= 0) {
        DPRINTF("warning invalid descriptor - ignoring packet!\n");
        return;
    }

    recv_count = client->msg_recv_count;

    if (send(client->socket_fd, msg, strlen(msg), 0) < 0) {
        DPRINTF
            ("send error - closing client domain=%d (fd=%d)\n",
             client->domain_id, client->socket_fd);
        client->socket_fd = -1;
        client_list_remove(s, client);
        return;
    }

    sent_count = ++client->msg_sent_count;

    DPRINTF("recv=%d sent=%d", recv_count, sent_count);

    /* XXX - perhaps use id? */
    if (sync) {
        DPRINTF("attempting to sync!\n");
        while (client->msg_recv_count == recv_count) {
            usleep(1);
        }
        DPRINTF("synced!\n");
    }
}

static void qips_send_hello(QipsState * s, QipsClient * client)
{
    char hello[] = "{ \"execute\": \"qmp_capabilities\" }";

    DPRINTF("sending hello to client slot=%d domain=%d (fd=%d)\n",
            client->slot_id, client->domain_id, client->socket_fd);

    qips_send_message(s, client, hello, strlen(hello), false);
}

static void qips_send_xen_query(QipsState * s, QipsClient * client)
{
    char query[] = "{ \"execute\": \"query-xen-status\" }";

    DPRINTF("sending xen query to client slot=%d domain=%d (fd=%d)\n",
            client->slot_id, client->domain_id, client->socket_fd);

    qips_send_message(s, client, query, strlen(query), false);
}

static void qips_request_kbd_leds(QipsState * s, QipsClient * client)
{
    char query[] = "{ \"execute\": \"query-kbd-leds\" }\r\n";

    DPRINTF("sending kbd leds query to client slot=%d domain=%d (fd=%d)\n",
            client->slot_id, client->domain_id, client->socket_fd);

    qips_send_message(s, client, query, strlen(query), false);
}

static void qips_request_kbd_reset(QipsState * s, QipsClient * client)
{
    char query[] = "{ \"execute\": \"send-kbd-reset\" }\r\n";

    DPRINTF("sending kbd reset to client slot=%d domain=%d (fd=%d)\n",
            client->slot_id, client->domain_id, client->socket_fd);

    qips_send_message(s, client, query, strlen(query), false);
}

void qips_send_focused_client_message(char *msg, size_t sz, bool sync)
{
    qips_send_message(&state, state.focused_client, msg, sz, sync);
}

static void terminate(int signum)
{
    DPRINTF("SIGTERM!\n");

    qips_cleanup(&state);

    exit(5);
}

static void setup_signals(void)
{
    struct sigaction sa;

    /* ignore most signals... */
    sigemptyset(&(sa.sa_mask));
    sa.sa_flags = SA_RESTART;
    sa.sa_handler = SIG_IGN;
    sigaction(SIGHUP, &sa, 0);
    sigaction(SIGINT, &sa, 0);
    sigaction(SIGQUIT, &sa, 0);
    sigaction(SIGPIPE, &sa, 0);
    sigaction(SIGALRM, &sa, 0);
    sigaction(SIGTSTP, &sa, 0);
    sigaction(SIGTTIN, &sa, 0);
    sigaction(SIGTTOU, &sa, 0);
    sigaction(SIGURG, &sa, 0);
    sigaction(SIGVTALRM, &sa, 0);
    sigaction(SIGIO, &sa, 0);
    sigaction(SIGPWR, &sa, 0);

    /* catch sigterm to exit cleanly... */
    sa.sa_flags = SA_RESETHAND;
    sa.sa_handler = terminate;
    sigaction(SIGTERM, &sa, NULL);
}

static int is_domain_socket(const struct dirent *dir)
{
    return strncmp(QIPS_SOCKETS_FMT_BASE, dir->d_name,
                   strlen(QIPS_SOCKETS_FMT_BASE)) == 0;
}

static void process_json_message(JSONMessageParser * parser, QList * tokens);
static void client_consumer(QipsState * s, QipsClient * client)
{
    char buf[4096];

    while (1) {
        ssize_t sz = read(client->socket_fd, buf, sizeof(buf));

        if (sz < 0) {
            DPRINTF("failed to read: %s!\n", strerror(errno));
            break;
        }

        if (sz > 0) {
            json_message_parser_feed(&client->inbound_parser,
                                     (const char *)buf, sz);
        }

        if (sz == 0) {
            DPRINTF("client disconnected: %s\n", strerror(errno));
            break;
        }
    }

    /* client is done for - cleanup */
    DPRINTF("closing client slot=%d\n", client->slot_id);
    close(client->socket_fd);
    client->socket_fd = -1;
    client_list_remove(s, client);
}

static const char *qtype_names[] = {
    [QTYPE_NONE] = "QTYPE_NONE",
    [QTYPE_QSTRING] = "QTYPE_QSTRING",
    [QTYPE_QDICT] = "QTYPE_QDICT",
    [QTYPE_QLIST] = "QTYPE_QLIST",
    [QTYPE_QFLOAT] = "QTYPE_QFLOAT",
    [QTYPE_QBOOL] = "QTYPE_QBOOL",
    [QTYPE_QINT] = "QTYPE_QINT",
    [QTYPE_QERROR] = "QTYPE_QERROR",
};

static void dump_qobj(int indent_level, QObject * obj)
{
    int type;
    const char *type_name;

    type = qobject_type(obj);
    type_name = qtype_names[type];

    DPRINTF("%*s{", indent_level, "-");
    switch (type) {
    case QTYPE_QSTRING:
        {
            const char *str;
            str = qstring_get_str(qobject_to_qstring(obj));
            DPRINTF("%*s(%s) %s", indent_level, "-", type_name, str);
            break;
        }
    case QTYPE_QDICT:
        {
            const QDictEntry *ent;
            QDict *qdict = qobject_to_qdict(obj);
            for (ent = qdict_first(qdict); ent != NULL;
                 ent = qdict_next(qdict, ent)) {
                QObject *val = qdict_entry_value(ent);
                const char *key = qdict_entry_key(ent);
                DPRINTF("%*s(%s) %s=>", indent_level, "-", type_name, key);
                dump_qobj(indent_level + 4, val);
            }
            break;
        }
    case QTYPE_QLIST:
        {
            const QListEntry *lent;
            QList *list = qobject_to_qlist(obj);

            for (lent = qlist_first(list); lent != NULL;
                 lent = qlist_next(lent)) {
                dump_qobj(indent_level + 4, lent->value);
            }
            break;
        }
    case QTYPE_QFLOAT:
        {
            double fl = qfloat_get_double(qobject_to_qfloat(obj));
            DPRINTF("%*s(%s) %e", indent_level, "-", type_name, fl);
            break;
        }
    case QTYPE_QBOOL:
        {
            int b = qbool_get_int(qobject_to_qbool(obj));
            DPRINTF("%*s(%s) %s", indent_level, "-", type_name,
                    b ? "true" : "false");
            break;
        }
    case QTYPE_QINT:
        {
            int i = qint_get_int(qobject_to_qint(obj));
            DPRINTF("%*s(%s) %d", indent_level, "-", type_name, i);
            break;
        }
    case QTYPE_NONE:
    default:
        {
            DPRINTF("?????");
            break;
        }
    }
    DPRINTF("%*s}", indent_level, "-");
}

/* handle incoming json message */
static void process_json_message(JSONMessageParser * parser, QList * tokens)
{
    QipsClient *client = container_of(parser, QipsClient, inbound_parser);
    QObject *obj;
    QDict *qdict;
    QDict *rdict;
    Error *err = NULL;

    DPRINTF("processing message...\n");

    client->msg_recv_count++;

    g_assert(client && parser);

    obj = json_parser_parse_err(tokens, NULL, &err);

    dump_qobj(4, obj);

    qdict = qobject_to_qdict(obj);

    if (!qdict) {
        DPRINTF("no qdict\n");
    } else {
        DPRINTF("qdict\n");
        if (qdict_haskey(qdict, "return")) {
            rdict = qdict_get_qdict(qdict, "return");
            if (!rdict) {
                DPRINTF("no rdict\n");
            } else {
                DPRINTF("rdict\n");
                /* HACK XXX: pick out the things we care about */
                // domain id
                if (qdict_haskey(rdict, "domain")) {
                    QObject *obj;
                    DPRINTF("rdict has domain\n");

                    obj = qdict_get(rdict, "domain");

                    DPRINTF("rdict domain obj = %p (type=%d)\n", obj,
                            qobject_type(obj));
                    if (qobject_type(obj) == QTYPE_QINT) {
                        int domain;
                        DPRINTF("rdict domain type match\n");
                        domain = qint_get_int(qobject_to_qint(obj));
                        client->domain_id = domain;
                        DPRINTF("set client slot=%d to domain=%d",
                                client->slot_id, client->domain_id);
                    } else {
                        DPRINTF("rdict has domain type mismatch\n");
                    }
                }
                // caps
                if (qdict_haskey(rdict, "caps")) {
                    QObject *obj;
                    obj = qdict_get(rdict, "caps");

                    DPRINTF("rdict has caps\n");

                    if (obj && qobject_type(obj) == QTYPE_QBOOL) {
                        int caps = qbool_get_int(qobject_to_qbool(obj));
                        if (caps) {
                            client->led_state |= QEMU_CAPS_LOCK_LED;
                        } else {
                            client->led_state &= ~QEMU_CAPS_LOCK_LED;
                        }
                        DPRINTF("set client slot=%d to caps=%d (0x%x)",
                                client->slot_id, caps, client->led_state);
                    } else {
                        DPRINTF("rdict has caps type mismatch\n");
                    }
                }
                // scroll
                if (qdict_haskey(rdict, "scroll")) {
                    QObject *obj;
                    obj = qdict_get(rdict, "scroll");

                    DPRINTF("rdict has scroll\n");

                    if (obj && qobject_type(obj) == QTYPE_QBOOL) {
                        int scroll = qbool_get_int(qobject_to_qbool(obj));
                        if (scroll) {
                            client->led_state |= QEMU_SCROLL_LOCK_LED;
                        } else {
                            client->led_state &= ~QEMU_SCROLL_LOCK_LED;
                        }
                        DPRINTF("set client slot=%d to scroll=%d (0x%x)",
                                client->slot_id, scroll, client->led_state);
                    } else {
                        DPRINTF("rdict has scroll type mismatch\n");
                    }
                }
                // num
                if (qdict_haskey(rdict, "num")) {
                    QObject *obj;
                    obj = qdict_get(rdict, "num");

                    DPRINTF("rdict has num\n");

                    if (obj && qobject_type(obj) == QTYPE_QBOOL) {
                        int num = qbool_get_int(qobject_to_qbool(obj));
                        if (num) {
                            client->led_state |= QEMU_NUM_LOCK_LED;
                        } else {
                            client->led_state &= ~QEMU_NUM_LOCK_LED;
                        }
                        DPRINTF("set client slot=%d to num=%d (0x%x)",
                                client->slot_id, num, client->led_state);
                    } else {
                        DPRINTF("rdict has num type mismatch\n");
                    }
                }
            }
        }
    }

    /* TODO: responsible for DECREF? */
    DPRINTF("processed message.\n");
}

/* add a client via its own thread because connect() may require some time */
static void *client_add_thread(void *p)
{
    bool connected = false;
    int retry_count = 5;
    char *path = (char *)p;
    QipsState *s = &state;
    int slot_id = 0;
    struct sockaddr_un serv_addr;
    QipsClient *new_client = NULL;

    sscanf(path, QIPS_SOCKETS_FMT, &slot_id);

    DPRINTF("path=%s slot=%d\n", path, slot_id);

    if (slot_id <= 0) {
        DPRINTF("invalid client with path: %s\n", path);
        free(path);
        return NULL;
    }

    /* register new client */
    new_client = g_malloc0(sizeof(QipsClient));

    new_client->slot_id = slot_id;
    pstrcpy(new_client->socket_path, sizeof(new_client->socket_path), path);

    serv_addr.sun_family = AF_UNIX;
    pstrcpy(serv_addr.sun_path, sizeof(serv_addr.sun_path),
            new_client->socket_path);

    new_client->socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);

    while (--retry_count) {
        /* if at first you don't succeed... a race condition here
           where we try to connect before socket is accepting */

        if (connect(new_client->socket_fd, (const struct sockaddr *)&serv_addr,
                    sizeof(serv_addr)) == 0) {
            /* good to go */
            connected = true;
            break;
        }

        DPRINTF("failed to connect to slot_id: %d (%s)\n",
                new_client->slot_id, strerror(errno));

        sleep(1);
    }

    if (!connected) {
        close(new_client->socket_fd);
        free(new_client);
        free(path);
        return NULL;
    }

    DPRINTF("connected new client at %s with slot=%d\n",
            new_client->socket_path, new_client->slot_id);

    new_client->socket_listener = pthread_self();

    json_message_parser_init(&new_client->inbound_parser, process_json_message);

    client_list_add(s, new_client);

    qips_send_hello(s, new_client);

    /* XXX: ugly hack because we can't sync on read thread... */
    sleep(1);

    qips_send_xen_query(s, new_client);

    qips_request_kbd_leds(s, new_client);

    client_consumer(s, new_client);

    return NULL;
}

static void client_notify(QipsState * s)
{
    int fd, wd;

    /* initalize inotify */
    fd = inotify_init();

    if (fd < 0) {
        DPRINTF("inotify_init() error: %s\n", strerror(errno));
        return;
    }

    /* add watch for /var/run/qemu-iss */
    wd = inotify_add_watch(fd, QIPS_SOCKETS_PATH, IN_CREATE | IN_DELETE);

    while (1) {
        /* need to get multiple events at a time or risk losing them */
        char event_buffer[16 * (sizeof(struct inotify_event) + NAME_MAX + 1)];
        char full_path[PATH_MAX + 1];
        struct inotify_event *event;
        int length;
        int b = 0;

        length = read(fd, event_buffer, sizeof(event_buffer));

        if (length < 0) {
            DPRINTF("inotify read() error: %s\n", strerror(errno));
            return;
        }

        event = (struct inotify_event *)event_buffer;

        for (b = 0; b < length; b += sizeof(struct inotify_event) + event->len) {
            event = (struct inotify_event *)&event_buffer[b];

            if (!event->len) {
                DPRINTF("warning: name is zero bytes?\n");
                continue;
            }

            if ((b + sizeof(struct inotify_event) + event->len) > length) {
                DPRINTF("warning: partial event?\n");
                break;
            }

            /* determine full path */
            snprintf(full_path, sizeof(full_path), QIPS_SOCKETS_PATH "/%s",
                     event->name);

            DPRINTF("event name=%s mask=0x%x\n", event->name, event->mask);

            if (event->mask & IN_CREATE) {
                if (event->mask & IN_ISDIR) {
                    DPRINTF("detected new directory: %s\n", full_path);
                } else {
                    pthread_t thread_id;
                    char *path = g_malloc0(PATH_MAX);

                    DPRINTF("detected new file: %s\n", full_path);

                    pstrcpy(path, PATH_MAX, full_path);

                    pthread_create(&thread_id, NULL, client_add_thread, path);
                }
            } else if (event->mask & IN_DELETE) {
                if (event->mask & IN_ISDIR) {
                    DPRINTF("detected deleted directory: %s\n", full_path);
                } else {
                    /* allow socket code to handle client removal */
                    DPRINTF("detected deleted file: %s\n", full_path);
                }
            }
        }
    }

    /* cleanup */
    inotify_rm_watch(fd, wd);
    close(fd);
}

static void client_scan(QipsState * s)
{
    struct dirent **namelist;
    int i, ndev;

    ndev = scandir(QIPS_SOCKETS_PATH, &namelist, is_domain_socket, alphasort);

    if (ndev <= 0) {
        return;
    }

    DPRINTF("checking client qemu sockets...\n");

    for (i = 0; i < ndev; i++) {
        pthread_t thread_id;
        char *path = g_malloc0(PATH_MAX);
        snprintf(path, PATH_MAX, "%s/%s", QIPS_SOCKETS_PATH,
                 namelist[i]->d_name);

        pthread_create(&thread_id, NULL, client_add_thread, path);

        free(namelist[i]);
    }
}

int main(int argc, char *argv[])
{
    int i;
    QipsClient *dom0;

#ifdef DO_LOG_SYSLOG
    openlog("qips", LOG_CONS | LOG_PID, LOG_USER);
#endif

    DPRINTF("main entry...\n");

    for (i = 1; i < argc && argv[i]; i++) {
        if (strcmp(argv[i], "debug-evdev") == 0) {
            DPRINTF("evdev_debug_mode = 1\n");
            evdev_debug_mode = 1;
        }
        if (strcmp(argv[i], "debug-input") == 0) {
            DPRINTF("input_backend_debug_mode = 1\n");
            input_backend_debug_mode = 1;
        }
        if (strcmp(argv[i], "debug-qips") == 0) {
            DPRINTF("qips_debug_mode = 1\n");
            qips_debug_mode = 1;
        }
    }

    state.console_frontend = xengt_console_frontend_register();
    state.console_backend = vt_console_backend_register();
    state.input_backend = evdev_input_backend_register();

    setup_signals();

    client_list_mutex_init(&state);

    /* add dom0 to client list */
    dom0 = g_malloc0(sizeof(QipsClient));
    dom0->socket_fd = -1;
    dom0->domain_id = 0;
    dom0->slot_id = 0;
    strcpy(dom0->socket_path, "dom0");

    QTAILQ_INIT(&state.clients);

    client_list_add(&state, dom0);

    state.focused_client = QTAILQ_FIRST(&state.clients);

    /* modules init after state is initialized */
    state.console_frontend->init();
    state.console_backend->init();
    state.input_backend->init();

    client_scan(&state);

    while (1) {
        client_notify(&state);
    }

    DPRINTF("exiting...\n");

    qips_cleanup(&state);

    return 0;
}
