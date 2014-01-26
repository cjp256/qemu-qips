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
#include <getopt.h>
#include <time.h>

#include "qemu-common.h"
#include "input-backend/input-backend.h"
#include "input-backend/evdev.h"
#include "console-backend/console-backend.h"
#include "console-backend/vt.h"
#include "console-backend/xback.h"
#include "console-frontend/console-frontend.h"
#include "console-frontend/xengt.h"
#include "console-frontend/xfront.h"
#include "ui/x_keymap.h"
#include "qips.h"
#include "qapi/qmp/json-streamer.h"
#include "qapi/qmp/json-parser.h"

int evdev_debug_mode = 0;
int input_backend_debug_mode = 0;
int qips_debug_mode = 0;

#define QIPS_SOCKETS_PATH "/var/run/qips"
#define QIPS_SOCKETS_FMT "/var/run/qips/slot-%d"
#define QIPS_SOCKETS_FMT_BASE "slot-"

const char * qips_sockets_path = QIPS_SOCKETS_PATH;
const char * qips_sockets_fmt = QIPS_SOCKETS_FMT;
const char * qips_sockets_fmt_base = QIPS_SOCKETS_FMT_BASE;

typedef struct QmpMessage {
    int64_t msg_id;
    char *msg;
    time_t t_queued;
    time_t t_sent;
    time_t t_response;
    time_t t_expire;
    QDict *response;
    pthread_cond_t response_cond;
    pthread_mutex_t response_mutex;
    QTAILQ_ENTRY(QmpMessage) next;
} QmpMessage;

typedef QTAILQ_HEAD(QmpMessageQueue, QmpMessage) QmpMessageQueue;

typedef struct QipsClient {
    bool active;                        /* threads break & join when 0 */
    char socket_path[PATH_MAX];
    pid_t process_id;
    int socket_fd;
    int domain_id;
    int slot_id;
    int led_state;
    int msg_recv_count;
    int msg_sent_count;
    bool mouse_mode_absolute;
    pthread_cond_t outgoing_messages_cond;
    pthread_mutex_t outgoing_messages_mutex;
    QmpMessageQueue outgoing_messages;

    pthread_t socket_listener;
    pthread_t regulator;
    JSONMessageParser inbound_parser;
    QTAILQ_ENTRY(QipsClient) next;
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

void error_report(const char *fmt, ...)
{
    va_list arg;
    va_start(arg, fmt);
    vfprintf(stderr, fmt, arg);
    va_end(arg);
    fflush(stderr);
}

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
        s->console_frontend->prep_switch(true);
        s->console_backend->lock();
    } else {
        s->console_frontend->prep_switch(false);
    }

    s->focused_client = new_focus;

    /* update console frontend */
    DPRINTF("domain_switch\n");
    s->console_frontend->domain_switch(new_focus->domain_id,
                                       new_focus->process_id,
                                       new_focus->slot_id);

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

static void client_list_add(QipsState * s, QipsClient * client)
{
    QipsClient *iter;

    DPRINTF("adding client slot id=%d...\n", client->slot_id);

    /* we want to add the client in order of slot id to simply switches */

    qemu_mutex_lock(&s->clients_mutex);
    QTAILQ_FOREACH(iter, &s->clients, next) {
        if (iter->slot_id > client->slot_id) {
            QTAILQ_INSERT_BEFORE(iter, client, next);
            qemu_mutex_unlock(&s->clients_mutex);
            return;
        }

        /* XXX: shouldn't happen - but if there is a race, allow add _after_
           existing entry if delete is racing (and losing) */
        if (iter->slot_id == client->slot_id) {
            DPRINTF("WARNING: re-adding slot id=%d...?\n", client->slot_id);
            QTAILQ_INSERT_AFTER(&s->clients, iter, client, next);
            qemu_mutex_unlock(&s->clients_mutex);
            return;
        }
    }

    /* no larger slot id was found - add to tail */
    QTAILQ_INSERT_TAIL(&s->clients, client, next);
    qemu_mutex_unlock(&s->clients_mutex);;
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

    qemu_mutex_lock(&s->clients_mutex);
    QTAILQ_REMOVE(&s->clients, client, next);
    qemu_mutex_unlock(&s->clients_mutex);
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
    s->console_frontend->domain_switch(0, 0, 0);

    /* release lock on dom0 */
    s->console_backend->release();

    s->console_frontend->cleanup();
    s->console_backend->cleanup();
    s->input_backend->cleanup();

    DPRINTF("complete...\n");
}

static void qips_send_message(QipsState * s, QipsClient * client, char *msg,
                              size_t sz)
{
    QmpMessage *message;
    size_t msg_len = strlen(msg) + 256; /* TODO: can do better */
    static int64_t msg_id = 0;

    if (!client) {
        DPRINTF(" noone is listening :(\n");
        return;
    }

    if (client->slot_id == 0) {
        /* do nothing with dom0 events */
        return;
    }

    message = g_malloc0(sizeof(*message));
    pthread_mutex_init(&message->response_mutex, NULL);
    pthread_cond_init(&message->response_cond, NULL);
    message->t_queued = time(NULL);
    message->msg = g_malloc0(msg_len);
    message->msg_id = ++msg_id;

    snprintf(message->msg, msg_len, "{ \"id\": %" PRId64 ", %s }\r\n",
            msg_id, msg);

    DPRINTF("queuing msg id=%" PRId64 " to client slot=%d domain=%d (fd=%d)\n",
            msg_id, client->slot_id, client->domain_id, client->socket_fd);

    DPRINTF("msg = %s\n", msg);

    if (sz != strlen(msg)) {
        DPRINTF("WARNING msg sz %zd != strlen(msg) %zd \n", sz, strlen(msg));
    }

    pthread_mutex_lock(&client->outgoing_messages_mutex);
    QTAILQ_INSERT_TAIL(&client->outgoing_messages, message, next);
    pthread_mutex_unlock(&client->outgoing_messages_mutex);
    pthread_cond_signal(&client->outgoing_messages_cond);
}

static void qips_send_hello(QipsState * s, QipsClient * client)
{
    char hello[] = " \"execute\": \"qmp_capabilities\" ";

    DPRINTF("sending hello to client slot=%d domain=%d (fd=%d)\n",
            client->slot_id, client->domain_id, client->socket_fd);

    qips_send_message(s, client, hello, strlen(hello));
}

static void qips_send_xen_query(QipsState * s, QipsClient * client)
{
    char query[] = " \"execute\": \"query-xen-status\" ";

    DPRINTF("sending xen query to client slot=%d domain=%d (fd=%d)\n",
            client->slot_id, client->domain_id, client->socket_fd);

    qips_send_message(s, client, query, strlen(query));
}

static void qips_send_process_info_query(QipsState * s, QipsClient * client)
{
    char query[] = " \"execute\": \"query-process-info\" ";

    DPRINTF("sending process info query to client slot=%d domain=%d (fd=%d)\n",
            client->slot_id, client->domain_id, client->socket_fd);

    qips_send_message(s, client, query, strlen(query));
}

static void qips_request_kbd_leds(QipsState * s, QipsClient * client)
{
    char query[] = " \"execute\": \"query-kbd-leds\" ";

    DPRINTF("sending kbd leds query to client slot=%d domain=%d (fd=%d)\n",
            client->slot_id, client->domain_id, client->socket_fd);

    qips_send_message(s, client, query, strlen(query));
}

static void qips_request_kbd_reset(QipsState * s, QipsClient * client)
{
    char query[] = " \"execute\": \"send-kbd-reset\" ";

    DPRINTF("sending kbd reset to client slot=%d domain=%d (fd=%d)\n",
            client->slot_id, client->domain_id, client->socket_fd);

    qips_send_message(s, client, query, strlen(query));
}

void qips_send_focused_client_message(char *msg, size_t sz)
{
    qips_send_message(&state, state.focused_client, msg, sz);
}

static void terminate(int signum)
{
    DPRINTF("SIGTERM!\n");

    qips_cleanup(&state);

    exit(5);
}

static void setup_signals(bool allow_sigint)
{
    struct sigaction sa;

    /* ignore most signals... */
    sigemptyset(&(sa.sa_mask));
    sa.sa_flags = SA_RESTART;
    sa.sa_handler = SIG_IGN;
    sigaction(SIGHUP, &sa, 0);

    if (!allow_sigint) {
        sigaction(SIGINT, &sa, 0);
    }

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
    return strncmp(qips_sockets_fmt_base, dir->d_name,
                   strlen(qips_sockets_fmt_base)) == 0;
}

static void process_return_message(QipsClient * client, QDict * dict);

/* this guy regulates (serializes) all sent messages to make QMP happy */
static void * client_regulator(void *c)
{
    QipsClient *client = (QipsClient *) c;

    while (client->active) {
        QmpMessage *next_message;

        pthread_mutex_lock(&client->outgoing_messages_mutex);
        while ((next_message = QTAILQ_FIRST(&client->outgoing_messages)) == NULL) {
            pthread_cond_wait(&client->outgoing_messages_cond,
                            &client->outgoing_messages_mutex);
        }
        pthread_mutex_unlock(&client->outgoing_messages_mutex);

        if (!next_message) {
            DPRINTF("next_message is NULL??\n");
            exit(1);
        }

        DPRINTF("sending msg_id=%" PRId64 "\n", next_message->msg_id);
        if (send(client->socket_fd, next_message->msg,
                 strlen(next_message->msg), 0) < 0) {
            DPRINTF("send error - closing client domain=%d (fd=%d)\n",
                    client->domain_id, client->socket_fd);
            client->active = false;
            return NULL;
        }

        DPRINTF("awaiting response msg_id=%" PRId64 "\n", next_message->msg_id);
        pthread_mutex_lock(&next_message->response_mutex);
        while (!next_message->response) {
            pthread_cond_wait(&next_message->response_cond,
                              &next_message->response_mutex);
        }
        pthread_mutex_unlock(&next_message->response_mutex);
        DPRINTF("got response msg_id=%" PRId64 "\n", next_message->msg_id);

        client->msg_sent_count++;

        /* drop it from the queue */
        pthread_mutex_lock(&client->outgoing_messages_mutex);
        QTAILQ_REMOVE(&client->outgoing_messages, next_message, next);
        pthread_mutex_unlock(&client->outgoing_messages_mutex);

        /* TODO: callback handler */
        process_return_message(client, next_message->response);

        /* cleanup message */
        pthread_cond_destroy(&next_message->response_cond);
        pthread_mutex_destroy(&next_message->response_mutex);
        if (next_message->response) {
            g_free(next_message->response);
        }
        g_free(next_message->msg);
        g_free(next_message);
    }

    return NULL;
}

static void client_consumer(QipsState * s, QipsClient * client)
{
    char buf[4096];

    while (client->active) {
        ssize_t sz;

        sz = read(client->socket_fd, buf, sizeof(buf));

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

        DPRINTF("received msg: recv=%d sent=%d", client->msg_recv_count, client->msg_sent_count);
    }

    /* client is done for - cleanup */
    DPRINTF("closing client slot=%d\n", client->slot_id);
    client->active = 0;
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

/* process mouse mode status */
static void process_mouse_mode_message(QipsClient * client, QDict * dict)
{
    DPRINTF("mouse mode status msg client=%p dict=%p\n", client, dict);

    /* mouse mode absolute */
    if (qdict_haskey(dict, "absolute")) {
        QObject *obj;
        obj = qdict_get(dict, "absolute");

        if (qobject_type(obj) == QTYPE_QBOOL) {
            int abs = qbool_get_int(qobject_to_qbool(obj));
            client->mouse_mode_absolute = abs;
            DPRINTF("set client slot=%d to mouse_mode_absolute=%d",
                    client->slot_id, client->mouse_mode_absolute);
        } else {
            DPRINTF("return msg has absolute type mismatch\n");
        }
    }

    /* ignore x/y coords */
}

/* process keyboard leds status */
static void process_kbd_leds_status_message(QipsClient * client, QDict * dict)
{
    DPRINTF("kbd leds status msg client=%p dict=%p\n", client, dict);

    /* caps lock status */
    if (qdict_haskey(dict, "caps")) {
        QObject *obj;
        obj = qdict_get(dict, "caps");

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
            DPRINTF("kbd led status msg has caps type mismatch\n");
        }
    }

    /* scroll lock status */
    if (qdict_haskey(dict, "scroll")) {
        QObject *obj;
        obj = qdict_get(dict, "scroll");

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
            DPRINTF("kbd led status msg has scroll type mismatch\n");
        }
    }

    /* num lock status */
    if (qdict_haskey(dict, "num")) {
        QObject *obj;
        obj = qdict_get(dict, "num");

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
            DPRINTF("kbd led status msg has num type mismatch\n");
        }
    }

    /* update leds if client is current focus */
    if (client == state.focused_client) {
        qips_console_backend_set_ledstate(client->led_state);
    }
}

/* process xen status */
static void process_xen_status_message(QipsClient * client, QDict * dict)
{
    DPRINTF("xen status msg client=%p dict=%p\n", client, dict);

    /* xen domain id */
    if (qdict_haskey(dict, "domain")) {
        QObject *obj;
        obj = qdict_get(dict, "domain");

        if (qobject_type(obj) == QTYPE_QINT) {
            int domain;
            domain = qint_get_int(qobject_to_qint(obj));
            client->domain_id = domain;
            DPRINTF("set client slot=%d to domain=%d",
                    client->slot_id, client->domain_id);
        } else {
            DPRINTF("xen status msg has domain type mismatch\n");
        }
    }
}

/* process process info */
static void process_proces_info_message(QipsClient * client, QDict * dict)
{
    DPRINTF("process info msg client=%p dict=%p\n", client, dict);

    /* process id */
    if (qdict_haskey(dict, "pid")) {
        QObject *obj;
        obj = qdict_get(dict, "pid");

        if (qobject_type(obj) == QTYPE_QINT) {
            pid_t pid;
            pid = qint_get_int(qobject_to_qint(obj));
            client->process_id = pid;
            DPRINTF("set client slot=%d pid=%ld",
                    client->slot_id, (long)client->process_id);
        } else {
            DPRINTF("process info msg has pid type mismatch\n");
        }
    }
}

/* process return messages */
static void process_return_message(QipsClient * client, QDict * dict)
{
    DPRINTF("return msg client=%p dict=%p\n", client, dict);

    /* this is a little tricky since you don't have context available here
     * unless we fully synchronize send & recv and/or id & track them.
     * For now, we process all possible return fields as
     * they are uniquely named for QIP(S) related messages.
     */
    process_xen_status_message(client, dict);
    process_mouse_mode_message(client, dict);
    process_kbd_leds_status_message(client, dict);
    process_proces_info_message(client, dict);
}

/* process event message given event name and data dictionary */
static void process_event_message(QipsClient * client, const char *event,
                                  QDict * data)
{
    if (strcmp(event, "QEVENT_QIP_MOUSE_MODE_UPDATE") == 0) {
        process_mouse_mode_message(client, data);
    } else if (strcmp(event, "QEVENT_QIP_DISPLAY_MODE_UPDATE") == 0) {
        /* TODO: not yet implemented on QIP side */
    } else if (strcmp(event, "QEVENT_QIP_KBD_LEDS_UPDATE") == 0) {
        process_kbd_leds_status_message(client, data);
    }
}

/* handle incoming json message */
static void process_json_message(JSONMessageParser * parser, QList * tokens)
{
    QipsClient *client = container_of(parser, QipsClient, inbound_parser);
    QObject *obj = NULL;
    QDict *qdict = NULL;
    int64_t msg_id = -1;

    Error *err = NULL;

    DPRINTF("processing message...\n");

    client->msg_recv_count++;

    g_assert(client && parser);

    obj = json_parser_parse_err(tokens, NULL, &err);

    dump_qobj(4, obj);

    qdict = qobject_to_qdict(obj);

    if (!qdict) {
        DPRINTF("json message is not qdict?? - qdict = %p\n", qdict);
        return;
    }

    /* check for id */
    if (qdict_haskey(qdict, "id")) {
        DPRINTF("has key id - qdict = %p\n", qdict);
        msg_id = qdict_get_try_int(qdict, "id", -1);
    }

    /* check if return message */
    if (qdict_haskey(qdict, "return")) {
        QObject *obj = qdict_get(qdict, "return");
        DPRINTF("has key return - qdict = %p\n", qdict);

        if (obj) {
            if (qobject_type(obj) == QTYPE_QDICT) {
                /* any return message belongs to currently pending message */
                QmpMessage *pending_message;

                pending_message = QTAILQ_FIRST(&client->outgoing_messages);

                if (!pending_message) {
                    DPRINTF("error: no pending message??\n");
                    exit(1);
                }

                DPRINTF("handling pending response msg_id=%" PRId64 "\n",
                        msg_id);

                pthread_mutex_lock(&pending_message->response_mutex);
                pending_message->response = qobject_to_qdict(obj);
                pthread_mutex_unlock(&pending_message->response_mutex);
                pthread_cond_signal(&pending_message->response_cond);
                return;
            } else {
                DPRINTF("return type mismatch - type=%d\n", qobject_type(obj));
            }
        }
    }

    /* check if event message */
    if (qdict_haskey(qdict, "event")) {
        const char *name = qdict_get_try_str(qdict, "event");
        DPRINTF("has key event - qdict = %p\n", qdict);

        if (name) {
            QObject *obj = qdict_get(qdict, "data");
            DPRINTF("event name = %s - data = %p\n", name, obj);

            if (obj) {
                if (qobject_type(obj) == QTYPE_QDICT) {
                    process_event_message(client, name, qobject_to_qdict(obj));
                    return;
                } else {
                    DPRINTF("event type mismatch - type=%d\n", qobject_type(obj));
                }
            }
        }
    }
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

    sscanf(path, qips_sockets_fmt, &slot_id);

    DPRINTF("path=%s slot=%d\n", path, slot_id);

    if (slot_id <= 0) {
        DPRINTF("invalid client with path: %s\n", path);
        g_free(path);
        return NULL;
    }

    /* register new client */
    new_client = g_malloc0(sizeof(QipsClient));

    QTAILQ_INIT(&new_client->outgoing_messages);
    pthread_mutex_init(&new_client->outgoing_messages_mutex, NULL);
    pthread_cond_init(&new_client->outgoing_messages_cond, NULL);

    new_client->active = true;
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
        g_free(new_client);
        g_free(path);
        return NULL;
    }

    DPRINTF("connected new client at %s with slot=%d\n",
            new_client->socket_path, new_client->slot_id);

    new_client->socket_listener = pthread_self();

    pthread_create(&new_client->regulator, NULL, client_regulator, new_client);

    json_message_parser_init(&new_client->inbound_parser, process_json_message);

    client_list_add(s, new_client);

    qips_send_hello(s, new_client);

    /* XXX: ugly hack because we can't sync on read thread... */
    sleep(1);

    qips_send_xen_query(s, new_client);

    qips_send_process_info_query(s, new_client);

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
    wd = inotify_add_watch(fd, qips_sockets_path, IN_CREATE | IN_DELETE);

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
            snprintf(full_path, sizeof(full_path), "%s/%s",
                     qips_sockets_path, event->name);

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

    ndev = scandir(qips_sockets_path, &namelist, is_domain_socket, alphasort);

    if (ndev <= 0) {
        return;
    }

    DPRINTF("checking client qemu sockets...\n");

    for (i = 0; i < ndev; i++) {
        pthread_t thread_id;
        char *path = g_malloc0(PATH_MAX);
        snprintf(path, PATH_MAX, "%s/%s", qips_sockets_path,
                 namelist[i]->d_name);

        pthread_create(&thread_id, NULL, client_add_thread, path);

        free(namelist[i]);
    }

    free(namelist);
}

static void usage(const char *prog)
{
    fprintf(stderr, "[USAGE]\n %s [-dEID] " \
                    "--console-backend [vt|xback] " \
                    "--console-frontend [xengt|xfront] " \
                    "--input-backend [evdev|xinput] " \
                    "[--qmp-dir path]\n\n", prog);

    fprintf(stderr, "[OPTIONS]\n");
    fprintf(stderr, "  [-h|--help]          -- help\n");
    fprintf(stderr, "  [-d|--daemonize]     -- dameonize\n");
    fprintf(stderr, "  [-E|--debug-evdev]   -- dump evdev debug info\n");
    fprintf(stderr, "  [-I|--debug-input]   -- dump input debug info\n");
    fprintf(stderr, "  [-D|--debug]         -- dump basic debug info\n");
    fprintf(stderr, "  [-b|--console-backend]  -- specify console backend\n");
    fprintf(stderr, "  [-f|--console-frontend] -- specify console frontend\n");
    fprintf(stderr, "  [-i|--input-backend] -- specify input backend\n");
    fprintf(stderr, "  [-q|--qmp-dir]       -- specify qmp socket directory\n");
}

static void daemonize(void)
{
    /* TODO: move from vt.c backend? */
}

static void set_qmp_dir(const char *qmp_dir)
{
    /* qmp_dir/fmt_base%d + null */
    ssize_t fmt_len = strlen(qmp_dir) + 1 + strlen(QIPS_SOCKETS_FMT_BASE) + 3;
    char *fmt = g_malloc0(fmt_len);

    qips_sockets_path = qmp_dir;

    DPRINTF("set qips_sockets_path=%s\n", qips_sockets_path);

    qips_sockets_fmt_base = QIPS_SOCKETS_FMT_BASE;

    DPRINTF("set qips_sockets_fmt_base=%s\n", qips_sockets_fmt_base);

    snprintf(fmt, fmt_len, "%s/%s%%d",
             qmp_dir, qips_sockets_fmt_base);

    qips_sockets_fmt = fmt;

    DPRINTF("set qips_sockets_fmt=%s\n", qips_sockets_fmt);
}

int main(int argc, char *argv[])
{
    const char *sopt = "hcdEIDb:f:i:q:";
    const char *console_backend = NULL;
    const char *console_frontend = NULL;
    const char *input_backend = NULL;
    const char *qmp_dir = NULL;
    const struct option lopt[] = {
        { "help", 0, NULL, 'h' },
        { "daemonize", 0, NULL, 'd' },
        { "debug-evdev", 0, NULL, 'E' },
        { "debug-input", 0, NULL, 'I' },
        { "debug", 0, NULL, 'D' },
        { "console-backend", 1, NULL, 'b' },
        { "console-frontend", 1, NULL, 'f' },
        { "input-backend", 1, NULL, 'i' },
        { "qmp-dir", 1, NULL, 'q' },
        { NULL, 0, NULL, 0 }
    };

    int opt_ind = 0;
    char ch;
    bool allow_sigint = true;
    bool do_daemonize = false;

    QipsClient *dom0;

#ifdef DO_LOG_SYSLOG
    openlog("qips", LOG_CONS | LOG_PID, LOG_USER);
#endif

    while ((ch = getopt_long(argc, argv, sopt, lopt, &opt_ind)) != -1) {
        switch (ch) {
            case 'E':
                evdev_debug_mode = 1;
                break;
            case 'I':
                input_backend_debug_mode = 1;
                break;
            case 'D':
                qips_debug_mode = 1;
                break;
            case 'b':
                console_backend = optarg;
                break;
            case 'c':
                allow_sigint = true;
                break;
            case 'f':
                console_frontend = optarg;
                break;
            case 'i':
                input_backend = optarg;
                break;
            case 'd':
                do_daemonize = 1;
                break;
            case 'q':
                qmp_dir = optarg;
                break;
            case 'h':
                usage(argv[0]);
                return 0;
            case '?':
                g_print("Unknown option, try '%s --help' for more information.\n",
                        argv[0]);
                return EXIT_FAILURE;
        }
    }

    /* no console frontend chosen - error out */
    if (console_frontend == NULL) {
        fprintf(stderr, "error: must specify valid console-frontend!\n");
        return 1;
    }

    /* figure out which console frontend to use */
    if (strcmp(console_frontend, "xengt") == 0) {
        state.console_frontend = xengt_console_frontend_register();
    } else if (strcmp(console_frontend, "xfront") == 0) {
        state.console_frontend = xfront_console_frontend_register();
    }

    /* no console frontend chosen - error out */
    if (state.console_frontend == NULL) {
        fprintf(stderr, "error: must specify valid console-frontend!\n");
        return 1;
    }

    /* no console backend chosen - error out */
    if (console_backend == NULL) {
        fprintf(stderr, "error: must specify valid console-backend!\n");
        return 1;
    }

    /* figure out which console backend to use */
    if (strcmp(console_backend, "vt") == 0) {
        state.console_backend = vt_console_backend_register();
    } else if (strcmp(console_backend, "xback") == 0) {
        state.console_backend = xback_console_backend_register();
    }

    /* no console backend chosen - error out */
    if (state.console_backend == NULL) {
        fprintf(stderr, "error: must specify valid console-backend!\n");
        return 1;
    }

    /* no input backend chosen - error out */
    if (input_backend == NULL) {
        fprintf(stderr, "error: must specify valid console-backend!\n");
        return 1;
    }

    /* figure out which input backend to use */
    if (strcmp(input_backend, "evdev") == 0) {
        state.input_backend = evdev_input_backend_register();
    }

    /* no input backend chosen - error out */
    if (state.input_backend == NULL) {
        fprintf(stderr, "error: must specify valid console-backend!\n");
        return 1;
    }

    if (qmp_dir) {
        set_qmp_dir(qmp_dir);
    }

    if (do_daemonize) {
        daemonize();
    }

    setup_signals(allow_sigint);

    qemu_mutex_init(&state.clients_mutex);

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
