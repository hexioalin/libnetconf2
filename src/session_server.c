/**
 * \file session_server.c
 * \author Michal Vasko <mvasko@cesnet.cz>
 * \brief libnetconf2 server session manipulation functions
 *
 * Copyright (c) 2015 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>

#include "libnetconf.h"
#include "session_server.h"

struct nc_server_opts server_opts = {
    .endpt_lock = PTHREAD_RWLOCK_INITIALIZER,
    .ch_client_lock = PTHREAD_RWLOCK_INITIALIZER
};

static nc_rpc_clb global_rpc_clb = NULL;

struct nc_endpt *
nc_server_endpt_lock(const char *name, NC_TRANSPORT_IMPL ti, uint16_t *idx)
{
    uint16_t i;
    struct nc_endpt *endpt = NULL;

    /* READ LOCK */
    pthread_rwlock_rdlock(&server_opts.endpt_lock);

    for (i = 0; i < server_opts.endpt_count; ++i) {
        if (!strcmp(server_opts.endpts[i].name, name) && (!ti || (server_opts.endpts[i].ti == ti))) {
            endpt = &server_opts.endpts[i];
            break;
        }
    }

    if (!endpt) {
        ERR("Endpoint \"%s\" was not found.", name);
        /* READ UNLOCK */
        pthread_rwlock_unlock(&server_opts.endpt_lock);
        return NULL;
    }

    /* ENDPT LOCK */
    pthread_mutex_lock(&endpt->lock);

    if (idx) {
        *idx = i;
    }

    return endpt;
}

void
nc_server_endpt_unlock(struct nc_endpt *endpt)
{
    /* ENDPT UNLOCK */
    pthread_mutex_unlock(&endpt->lock);

    /* READ UNLOCK */
    pthread_rwlock_unlock(&server_opts.endpt_lock);
}

struct nc_ch_client *
nc_server_ch_client_lock(const char *name, NC_TRANSPORT_IMPL ti, uint16_t *idx)
{
    uint16_t i;
    struct nc_ch_client *client = NULL;

    /* READ LOCK */
    pthread_rwlock_rdlock(&server_opts.ch_client_lock);

    for (i = 0; i < server_opts.ch_client_count; ++i) {
        if (!strcmp(server_opts.ch_clients[i].name, name) && (!ti || (server_opts.ch_clients[i].ti == ti))) {
            client = &server_opts.ch_clients[i];
            break;
        }
    }

    if (!client) {
        ERR("Call Home client \"%s\" was not found.", name);
        /* READ UNLOCK */
        pthread_rwlock_unlock(&server_opts.ch_client_lock);
        return NULL;
    }

    /* CH CLIENT LOCK */
    pthread_mutex_lock(&client->lock);

    if (idx) {
        *idx = i;
    }

    return client;
}

void
nc_server_ch_client_unlock(struct nc_ch_client *client)
{
    /* CH CLIENT UNLOCK */
    pthread_mutex_unlock(&client->lock);

    /* READ UNLOCK */
    pthread_rwlock_unlock(&server_opts.ch_client_lock);
}

API void
nc_session_set_term_reason(struct nc_session *session, NC_SESSION_TERM_REASON reason)
{
    if (!session) {
        ERRARG("session");
        return;
    } else if (!reason) {
        ERRARG("reason");
        return;
    }

    session->term_reason = reason;
}

int
nc_sock_listen(const char *address, uint16_t port)
{
    const int optVal = 1;
    const socklen_t optLen = sizeof(optVal);
    int is_ipv4, sock;
    struct sockaddr_storage saddr;

    struct sockaddr_in *saddr4;
    struct sockaddr_in6 *saddr6;


    if (!strchr(address, ':')) {
        is_ipv4 = 1;
    } else {
        is_ipv4 = 0;
    }

    sock = socket((is_ipv4 ? AF_INET : AF_INET6), SOCK_STREAM, 0);
    if (sock == -1) {
        ERR("Failed to create socket (%s).", strerror(errno));
        goto fail;
    }

    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (void *)&optVal, optLen)) {
        ERR("Could not set socket SO_REUSEADDR socket option (%s).", strerror(errno));
        goto fail;
    }

    bzero(&saddr, sizeof(struct sockaddr_storage));
    if (is_ipv4) {
        saddr4 = (struct sockaddr_in *)&saddr;

        saddr4->sin_family = AF_INET;
        saddr4->sin_port = htons(port);

        if (inet_pton(AF_INET, address, &saddr4->sin_addr) != 1) {
            ERR("Failed to convert IPv4 address \"%s\".", address);
            goto fail;
        }

        if (bind(sock, (struct sockaddr *)saddr4, sizeof(struct sockaddr_in)) == -1) {
            ERR("Could not bind \"%s\" port %d (%s).", address, port, strerror(errno));
            goto fail;
        }

    } else {
        saddr6 = (struct sockaddr_in6 *)&saddr;

        saddr6->sin6_family = AF_INET6;
        saddr6->sin6_port = htons(port);

        if (inet_pton(AF_INET6, address, &saddr6->sin6_addr) != 1) {
            ERR("Failed to convert IPv6 address \"%s\".", address);
            goto fail;
        }

        if (bind(sock, (struct sockaddr *)saddr6, sizeof(struct sockaddr_in6)) == -1) {
            ERR("Could not bind \"%s\" port %d (%s).", address, port, strerror(errno));
            goto fail;
        }
    }

    if (listen(sock, NC_REVERSE_QUEUE) == -1) {
        ERR("Unable to start listening on \"%s\" port %d (%s).", address, port, strerror(errno));
        goto fail;
    }

    return sock;

fail:
    if (sock > -1) {
        close(sock);
    }

    return -1;
}

int
nc_sock_accept_binds(struct nc_bind *binds, uint16_t bind_count, int timeout, char **host, uint16_t *port, uint16_t *idx)
{
    uint16_t i;
    struct pollfd *pfd;
    struct sockaddr_storage saddr;
    socklen_t saddr_len = sizeof(saddr);
    int ret, sock = -1, flags;

    pfd = malloc(bind_count * sizeof *pfd);
    if (!pfd) {
        ERRMEM;
        return -1;
    }

    for (i = 0; i < bind_count; ++i) {
        if (binds[i].sock < 0) {
            /* invalid socket */
            --bind_count;
            continue;
        }
        if (binds[i].pollin) {
            binds[i].pollin = 0;
            /* leftover pollin */
            sock = binds[i].sock;
            break;
        }
        pfd[i].fd = binds[i].sock;
        pfd[i].events = POLLIN;
        pfd[i].revents = 0;
    }

    if (sock == -1) {
        /* poll for a new connection */
        ret = poll(pfd, bind_count, timeout);
        if (!ret) {
            /* we timeouted */
            free(pfd);
            return 0;
        } else if (ret == -1) {
            ERR("Poll failed (%s).", strerror(errno));
            free(pfd);
            return -1;
        }

        for (i = 0; i < bind_count; ++i) {
            if (pfd[i].revents & POLLIN) {
                --ret;

                if (!ret) {
                    /* the last socket with an event, use it */
                    sock = pfd[i].fd;
                    break;
                } else {
                    /* just remember the event for next time */
                    binds[i].pollin = 1;
                }
            }
        }
    }
    free(pfd);

    if (sock == -1) {
        ERRINT;
        return -1;
    }

    ret = accept(sock, (struct sockaddr *)&saddr, &saddr_len);
    if (ret < 0) {
        ERR("Accept failed (%s).", strerror(errno));
        return -1;
    }
    VRB("Accepted a connection on %s:%u.", binds[i].address, binds[i].port);

    /* make the socket non-blocking */
    if (((flags = fcntl(ret, F_GETFL)) == -1) || (fcntl(ret, F_SETFL, flags | O_NONBLOCK) == -1)) {
        ERR("Fcntl failed (%s).", strerror(errno));
        close(ret);
        return -1;
    }

    if (idx) {
        *idx = i;
    }

    /* host was requested */
    if (host) {
        if (saddr.ss_family == AF_INET) {
            *host = malloc(15);
            if (*host) {
                if (!inet_ntop(AF_INET, &((struct sockaddr_in *)&saddr)->sin_addr.s_addr, *host, 15)) {
                    ERR("inet_ntop failed (%s).", strerror(errno));
                    free(*host);
                    *host = NULL;
                }

                if (port) {
                    *port = ntohs(((struct sockaddr_in *)&saddr)->sin_port);
                }
            } else {
                ERRMEM;
            }
        } else if (saddr.ss_family == AF_INET6) {
            *host = malloc(40);
            if (*host) {
                if (!inet_ntop(AF_INET6, ((struct sockaddr_in6 *)&saddr)->sin6_addr.s6_addr, *host, 40)) {
                    ERR("inet_ntop failed (%s).", strerror(errno));
                    free(*host);
                    *host = NULL;
                }

                if (port) {
                    *port = ntohs(((struct sockaddr_in6 *)&saddr)->sin6_port);
                }
            } else {
                ERRMEM;
            }
        } else {
            ERR("Source host of an unknown protocol family.");
        }
    }

    return ret;
}

static struct nc_server_reply *
nc_clb_default_get_schema(struct lyd_node *rpc, struct nc_session *UNUSED(session))
{
    const char *identifier = NULL, *version = NULL, *format = NULL;
    char *model_data = NULL;
    const struct lys_module *module;
    struct nc_server_error *err;
    struct lyd_node *child, *data = NULL;
    const struct lys_node *sdata = NULL;

    LY_TREE_FOR(rpc->child, child) {
        if (!strcmp(child->schema->name, "identifier")) {
            identifier = ((struct lyd_node_leaf_list *)child)->value_str;
        } else if (!strcmp(child->schema->name, "version")) {
            version = ((struct lyd_node_leaf_list *)child)->value_str;
        } else if (!strcmp(child->schema->name, "format")) {
            format = ((struct lyd_node_leaf_list *)child)->value_str;
        }
    }

    /* check version */
    if (version && (strlen(version) != 10) && strcmp(version, "1.0")) {
        err = nc_err(NC_ERR_INVALID_VALUE, NC_ERR_TYPE_APP);
        nc_err_set_msg(err, "The requested version is not supported.", "en");
        return nc_server_reply_err(err);
    }

    /* check and get module with the name identifier */
    module = ly_ctx_get_module(server_opts.ctx, identifier, version);
    if (!module) {
        module = (const struct lys_module *)ly_ctx_get_submodule(server_opts.ctx, NULL, NULL, identifier, version);
    }
    if (!module) {
        err = nc_err(NC_ERR_INVALID_VALUE, NC_ERR_TYPE_APP);
        nc_err_set_msg(err, "The requested schema was not found.", "en");
        return nc_server_reply_err(err);
    }

    /* check format */
    if (!format || !strcmp(format, "ietf-netconf-monitoring:yang")) {
        lys_print_mem(&model_data, module, LYS_OUT_YANG, NULL);
    } else if (!strcmp(format, "ietf-netconf-monitoring:yin")) {
        lys_print_mem(&model_data, module, LYS_OUT_YIN, NULL);
    } else {
        err = nc_err(NC_ERR_INVALID_VALUE, NC_ERR_TYPE_APP);
        nc_err_set_msg(err, "The requested format is not supported.", "en");
        return nc_server_reply_err(err);
    }
    if (!model_data) {
        ERRINT;
        return NULL;
    }

    sdata = ly_ctx_get_node(server_opts.ctx, NULL, "/ietf-netconf-monitoring:get-schema/output/data");
    if (!sdata) {
        ERRINT;
        free(model_data);
        return NULL;
    }

    data = lyd_new_path(NULL, server_opts.ctx, "/ietf-netconf-monitoring:get-schema/data", model_data,
                        LYD_ANYDATA_STRING, LYD_PATH_OPT_OUTPUT);
    if (!data || lyd_validate(&data, LYD_OPT_RPCREPLY, NULL)) {
        ERRINT;
        free(model_data);
        return NULL;
    }

    return nc_server_reply_data(data, NC_WD_EXPLICIT, NC_PARAMTYPE_FREE);
}

static struct nc_server_reply *
nc_clb_default_close_session(struct lyd_node *UNUSED(rpc), struct nc_session *session)
{
    session->term_reason = NC_SESSION_TERM_CLOSED;
    return nc_server_reply_ok();
}

API int
nc_server_init(struct ly_ctx *ctx)
{
    const struct lys_node *rpc;

    if (!ctx) {
        ERRARG("ctx");
        return -1;
    }

    nc_init();

    /* set default <get-schema> callback if not specified */
    rpc = ly_ctx_get_node(ctx, NULL, "/ietf-netconf-monitoring:get-schema");
    if (rpc && !rpc->priv) {
        lys_set_private(rpc, nc_clb_default_get_schema);
    }

    /* set default <close-session> callback if not specififed */
    rpc = ly_ctx_get_node(ctx, NULL, "/ietf-netconf:close-session");
    if (rpc && !rpc->priv) {
        lys_set_private(rpc, nc_clb_default_close_session);
    }

    server_opts.ctx = ctx;

    server_opts.new_session_id = 1;
    pthread_spin_init(&server_opts.sid_lock, PTHREAD_PROCESS_PRIVATE);

    return 0;
}

API void
nc_server_destroy(void)
{
    unsigned int i;

    for (i = 0; i < server_opts.capabilities_count; i++) {
        lydict_remove(server_opts.ctx, server_opts.capabilities[i]);
    }
    free(server_opts.capabilities);
    pthread_spin_destroy(&server_opts.sid_lock);

#if defined(NC_ENABLED_SSH) || defined(NC_ENABLED_TLS)
    nc_server_del_endpt(NULL, 0);
#endif
#ifdef NC_ENABLED_SSH
    nc_server_ssh_del_authkey(NULL, NULL, 0, NULL);

    if (server_opts.hostkey_data && server_opts.hostkey_data_free) {
        server_opts.hostkey_data_free(server_opts.hostkey_data);
    }
#endif
#ifdef NC_ENABLED_TLS
    if (server_opts.server_cert_data && server_opts.server_cert_data_free) {
        server_opts.server_cert_data_free(server_opts.server_cert_data);
    }
    if (server_opts.trusted_cert_list_data && server_opts.trusted_cert_list_data_free) {
        server_opts.trusted_cert_list_data_free(server_opts.trusted_cert_list_data);
    }
#endif
    nc_destroy();
}

API int
nc_server_set_capab_withdefaults(NC_WD_MODE basic_mode, int also_supported)
{
    if (!basic_mode || (basic_mode == NC_WD_ALL_TAG)) {
        ERRARG("basic_mode");
        return -1;
    } else if (also_supported && !(also_supported & (NC_WD_ALL | NC_WD_ALL_TAG | NC_WD_TRIM | NC_WD_EXPLICIT))) {
        ERRARG("also_supported");
        return -1;
    }

    server_opts.wd_basic_mode = basic_mode;
    server_opts.wd_also_supported = also_supported;
    return 0;
}

API void
nc_server_get_capab_withdefaults(NC_WD_MODE *basic_mode, int *also_supported)
{
    if (!basic_mode && !also_supported) {
        ERRARG("basic_mode and also_supported");
        return;
    }

    if (basic_mode) {
        *basic_mode = server_opts.wd_basic_mode;
    }
    if (also_supported) {
        *also_supported = server_opts.wd_also_supported;
    }
}

API int
nc_server_set_capability(const char *value)
{
    const char **new;

    if (!value || !value[0]) {
        ERRARG("value must not be empty");
        return EXIT_FAILURE;
    }

    server_opts.capabilities_count++;
    new = realloc(server_opts.capabilities, server_opts.capabilities_count * sizeof *server_opts.capabilities);
    if (!new) {
        ERRMEM;
        return EXIT_FAILURE;
    }
    server_opts.capabilities = new;
    server_opts.capabilities[server_opts.capabilities_count - 1] = lydict_insert(server_opts.ctx, value, 0);

    return EXIT_SUCCESS;
}

API void
nc_server_set_hello_timeout(uint16_t hello_timeout)
{
    server_opts.hello_timeout = hello_timeout;
}

API uint16_t
nc_server_get_hello_timeout(void)
{
    return server_opts.hello_timeout;
}

API void
nc_server_set_idle_timeout(uint16_t idle_timeout)
{
    server_opts.idle_timeout = idle_timeout;
}

API uint16_t
nc_server_get_idle_timeout(void)
{
    return server_opts.idle_timeout;
}

API NC_MSG_TYPE
nc_accept_inout(int fdin, int fdout, const char *username, struct nc_session **session)
{
    NC_MSG_TYPE msgtype;

    if (!server_opts.ctx) {
        ERRINIT;
        return NC_MSG_ERROR;
    } else if (fdin < 0) {
        ERRARG("fdin");
        return NC_MSG_ERROR;
    } else if (fdout < 0) {
        ERRARG("fdout");
        return NC_MSG_ERROR;
    } else if (!username) {
        ERRARG("username");
        return NC_MSG_ERROR;
    } else if (!session) {
        ERRARG("session");
        return NC_MSG_ERROR;
    }

    /* prepare session structure */
    *session = calloc(1, sizeof **session);
    if (!(*session)) {
        ERRMEM;
        return NC_MSG_ERROR;
    }
    (*session)->status = NC_STATUS_STARTING;
    (*session)->side = NC_SERVER;

    /* transport specific data */
    (*session)->ti_type = NC_TI_FD;
    (*session)->ti.fd.in = fdin;
    (*session)->ti.fd.out = fdout;

    /* assign context (dicionary needed for handshake) */
    (*session)->flags = NC_SESSION_SHAREDCTX;
    (*session)->ctx = server_opts.ctx;

    /* assign new SID atomically */
    pthread_spin_lock(&server_opts.sid_lock);
    (*session)->id = server_opts.new_session_id++;
    pthread_spin_unlock(&server_opts.sid_lock);

    /* NETCONF handshake */
    msgtype = nc_handshake(*session);
    if (msgtype != NC_MSG_HELLO) {
        nc_session_free(*session, NULL);
        *session = NULL;
        return msgtype;
    }
    (*session)->opts.server.session_start = (*session)->opts.server.last_rpc = time(NULL);
    (*session)->status = NC_STATUS_RUNNING;

    return msgtype;
}

static void
nc_ps_queue_remove_id(struct nc_pollsession *ps, uint8_t id)
{
    uint8_t i, found = 0;

    for (i = 0; i < ps->queue_len; ++i) {
        /* idx round buffer adjust */
        if (ps->queue_begin + i == NC_PS_QUEUE_SIZE) {
            i = -ps->queue_begin;
        }

        if (found) {
            /* move the value back one place */
            if (ps->queue[ps->queue_begin + i] == id) {
                /* another equal value, simply cannot be */
                ERRINT;
            }

            if (ps->queue_begin + i == 0) {
                ps->queue[NC_PS_QUEUE_SIZE - 1] = ps->queue[ps->queue_begin + i];
            } else {
                ps->queue[ps->queue_begin + i - 1] = ps->queue[ps->queue_begin + i];
            }
        } else if (ps->queue[ps->queue_begin + i] == id) {
            /* found our id, there can be no more equal valid values */
            found = 1;
        }
    }

    if (!found) {
        ERRINT;
    }
    --ps->queue_len;
}

int
nc_ps_lock(struct nc_pollsession *ps, uint8_t *id, const char *func)
{
    int ret;
    uint8_t queue_last;
    struct timespec ts;

    nc_gettimespec(&ts);
    ts.tv_sec += NC_READ_TIMEOUT;

    /* LOCK */
    ret = pthread_mutex_timedlock(&ps->lock, &ts);
    if (ret) {
        ERR("%s: failed to lock a pollsession (%s).", func, strerror(ret));
        return -1;
    }

    /* get a unique queue value (by adding 1 to the last added value, if any) */
    if (ps->queue_len) {
        queue_last = ps->queue_begin + ps->queue_len - 1;
        if (queue_last > NC_PS_QUEUE_SIZE - 1) {
            queue_last -= NC_PS_QUEUE_SIZE;
        }
        *id = ps->queue[queue_last] + 1;
    } else {
        *id = 0;
    }

    /* add ourselves into the queue */
    if (ps->queue_len == NC_PS_QUEUE_SIZE) {
        ERR("%s: pollsession queue too small.", func);
        pthread_mutex_unlock(&ps->lock);
        return -1;
    }
    ++ps->queue_len;
    queue_last = ps->queue_begin + ps->queue_len - 1;
    if (queue_last > NC_PS_QUEUE_SIZE - 1) {
        queue_last -= NC_PS_QUEUE_SIZE;
    }
    ps->queue[queue_last] = *id;

    /* is it our turn? */
    while (ps->queue[ps->queue_begin] != *id) {
        nc_gettimespec(&ts);
        ts.tv_sec += NC_READ_TIMEOUT;

        ret = pthread_cond_timedwait(&ps->cond, &ps->lock, &ts);
        if (ret) {
            ERR("%s: failed to wait for a pollsession condition (%s).", func, strerror(ret));
            /* remove ourselves from the queue */
            nc_ps_queue_remove_id(ps, *id);
            pthread_mutex_unlock(&ps->lock);
            return -1;
        }
    }

    /* UNLOCK */
    pthread_mutex_unlock(&ps->lock);

    return 0;
}

int
nc_ps_unlock(struct nc_pollsession *ps, uint8_t id, const char *func)
{
    int ret;
    struct timespec ts;

    nc_gettimespec(&ts);
    ts.tv_sec += NC_READ_TIMEOUT;

    /* LOCK */
    ret = pthread_mutex_timedlock(&ps->lock, &ts);
    if (ret) {
        ERR("%s: failed to lock a pollsession (%s).", func, strerror(ret));
        ret = -1;
    }

    /* we must be the first, it was our turn after all, right? */
    if (ps->queue[ps->queue_begin] != id) {
        ERRINT;
        /* UNLOCK */
        if (!ret) {
            pthread_mutex_unlock(&ps->lock);
        }
        return -1;
    }

    /* remove ourselves from the queue */
    nc_ps_queue_remove_id(ps, id);

    /* broadcast to all other threads that the queue moved */
    pthread_cond_broadcast(&ps->cond);

    /* UNLOCK */
    if (!ret) {
        pthread_mutex_unlock(&ps->lock);
    }

    return ret;
}

API struct nc_pollsession *
nc_ps_new(void)
{
    struct nc_pollsession *ps;

    ps = calloc(1, sizeof(struct nc_pollsession));
    if (!ps) {
        ERRMEM;
        return NULL;
    }
    pthread_cond_init(&ps->cond, NULL);
    pthread_mutex_init(&ps->lock, NULL);

    return ps;
}

API void
nc_ps_free(struct nc_pollsession *ps)
{
    if (!ps) {
        return;
    }

    if (ps->queue_len) {
        ERR("FATAL: Freeing a pollsession structure that is currently being worked with!");
    }

    free(ps->sessions);
    pthread_mutex_destroy(&ps->lock);
    pthread_cond_destroy(&ps->cond);

    free(ps);
}

API int
nc_ps_add_session(struct nc_pollsession *ps, struct nc_session *session)
{
    uint8_t q_id;

    if (!ps) {
        ERRARG("ps");
        return -1;
    } else if (!session) {
        ERRARG("session");
        return -1;
    }

    /* LOCK */
    if (nc_ps_lock(ps, &q_id, __func__)) {
        return -1;
    }

    ++ps->session_count;
    ps->sessions = nc_realloc(ps->sessions, ps->session_count * sizeof *ps->sessions);
    if (!ps->sessions) {
        ERRMEM;
        /* UNLOCK */
        nc_ps_unlock(ps, q_id, __func__);
        return -1;
    }
    ps->sessions[ps->session_count - 1] = session;

    /* UNLOCK */
    return nc_ps_unlock(ps, q_id, __func__);
}

static int
_nc_ps_del_session(struct nc_pollsession *ps, struct nc_session *session, int index)
{
    uint16_t i;

    if (index >= 0) {
        i = (uint16_t)index;
        goto remove;
    }
    for (i = 0; i < ps->session_count; ++i) {
        if (ps->sessions[i] == session) {
remove:
            --ps->session_count;
            if (i < ps->session_count) {
                ps->sessions[i] = ps->sessions[ps->session_count];
                if (ps->last_event_session == i) {
                    ps->last_event_session = 0;
                }
            } else if (!ps->session_count) {
                free(ps->sessions);
                ps->sessions = NULL;
            }
            return 0;
        }
    }

    return -1;
}

API int
nc_ps_del_session(struct nc_pollsession *ps, struct nc_session *session)
{
    uint8_t q_id;
    int ret, ret2;

    if (!ps) {
        ERRARG("ps");
        return -1;
    } else if (!session) {
        ERRARG("session");
        return -1;
    }

    /* LOCK */
    if (nc_ps_lock(ps, &q_id, __func__)) {
        return -1;
    }

    ret = _nc_ps_del_session(ps, session, -1);

    /* UNLOCK */
    ret2 = nc_ps_unlock(ps, q_id, __func__);

    return (ret || ret2 ? -1 : 0);
}

API uint16_t
nc_ps_session_count(struct nc_pollsession *ps)
{
    uint8_t q_id;
    uint16_t count;

    if (!ps) {
        ERRARG("ps");
        return 0;
    }

    /* LOCK */
    if (nc_ps_lock(ps, &q_id, __func__)) {
        return -1;
    }

    count = ps->session_count;

    /* UNLOCK */
    nc_ps_unlock(ps, q_id, __func__);

    return count;
}

/* must be called holding the session lock!
 * returns: NC_PSPOLL_ERROR,
 *          NC_PSPOLL_BAD_RPC,
 *          NC_PSPOLL_BAD_RPC | NC_PSPOLL_REPLY_ERROR,
 *          NC_PSPOLL_RPC
 */
static int
nc_server_recv_rpc(struct nc_session *session, struct nc_server_rpc **rpc)
{
    struct lyxml_elem *xml = NULL;
    NC_MSG_TYPE msgtype;
    struct nc_server_reply *reply = NULL;
    int ret;

    if (!session) {
        ERRARG("session");
        return NC_PSPOLL_ERROR;
    } else if (!rpc) {
        ERRARG("rpc");
        return NC_PSPOLL_ERROR;
    } else if ((session->status != NC_STATUS_RUNNING) || (session->side != NC_SERVER)) {
        ERR("Session %u: invalid session to receive RPCs.", session->id);
        return NC_PSPOLL_ERROR;
    }

    msgtype = nc_read_msg(session, &xml);

    switch (msgtype) {
    case NC_MSG_RPC:
        *rpc = calloc(1, sizeof **rpc);
        if (!*rpc) {
            ERRMEM;
            goto error;
        }

        ly_errno = LY_SUCCESS;
        (*rpc)->tree = lyd_parse_xml(server_opts.ctx, &xml->child,
                                     LYD_OPT_RPC | LYD_OPT_DESTRUCT | LYD_OPT_NOEXTDEPS | LYD_OPT_STRICT, NULL);
        if (!(*rpc)->tree) {
            /* parsing RPC failed */
            reply = nc_server_reply_err(nc_err_libyang());
            ret = nc_write_msg(session, NC_MSG_REPLY, xml, reply);
            nc_server_reply_free(reply);
            if (ret == -1) {
                ERR("Session %u: failed to write reply.", session->id);
            }
            ret = NC_PSPOLL_REPLY_ERROR | NC_PSPOLL_BAD_RPC;
        } else {
            ret = NC_PSPOLL_RPC;
        }
        (*rpc)->root = xml;
        break;
    case NC_MSG_HELLO:
        ERR("Session %u: received another <hello> message.", session->id);
        ret = NC_PSPOLL_BAD_RPC;
        goto error;
    case NC_MSG_REPLY:
        ERR("Session %u: received <rpc-reply> from a NETCONF client.", session->id);
        ret = NC_PSPOLL_BAD_RPC;
        goto error;
    case NC_MSG_NOTIF:
        ERR("Session %u: received <notification> from a NETCONF client.", session->id);
        ret = NC_PSPOLL_BAD_RPC;
        goto error;
    default:
        /* NC_MSG_ERROR,
         * NC_MSG_WOULDBLOCK and NC_MSG_NONE is not returned by nc_read_msg()
         */
        ret = NC_PSPOLL_ERROR;
        break;
    }

    return ret;

error:
    /* cleanup */
    lyxml_free(server_opts.ctx, xml);

    return NC_PSPOLL_ERROR;
}

API void
nc_set_global_rpc_clb(nc_rpc_clb clb)
{
    global_rpc_clb = clb;
}

API NC_MSG_TYPE
nc_server_notif_send(struct nc_session *session, struct nc_server_notif *notif, int timeout)
{
    NC_MSG_TYPE result = NC_MSG_NOTIF;
    int ret;

    /* check parameters */
    if (!session) {
        ERRARG("session");
        return NC_MSG_ERROR;
    } else if (!notif || !notif->tree || !notif->eventtime) {
        ERRARG("notif");
        return NC_MSG_ERROR;
    }

    /* reading an RPC and sending a reply must be atomic (no other RPC should be read) */
    ret = nc_timedlock(session->ti_lock, timeout, __func__);
    if (ret < 0) {
        return NC_MSG_ERROR;
    } else if (!ret) {
        return NC_MSG_WOULDBLOCK;
    }

    ret = nc_write_msg(session, NC_MSG_NOTIF, notif);
    if (ret == -1) {
        ERR("Session %u: failed to write notification.", session->id);
        result = NC_MSG_ERROR;
    }
    pthread_mutex_unlock(session->ti_lock);

    return result;
}

/* must be called holding the session lock!
 * returns: NC_PSPOLL_ERROR,
 *          NC_PSPOLL_ERROR | NC_PSPOLL_REPLY_ERROR,
 *          NC_PSPOLL_REPLY_ERROR,
 *          0
 */
static int
nc_server_send_reply(struct nc_session *session, struct nc_server_rpc *rpc)
{
    nc_rpc_clb clb;
    struct nc_server_reply *reply;
    struct lys_node *rpc_act = NULL;
    struct lyd_node *next, *elem;
    int ret = 0, r;

    if (!rpc) {
        ERRINT;
        return NC_PSPOLL_ERROR;
    }

    if (rpc->tree->schema->nodetype == LYS_RPC) {
        /* RPC */
        rpc_act = rpc->tree->schema;
    } else {
        /* action */
        LY_TREE_DFS_BEGIN(rpc->tree, next, elem) {
            if (elem->schema->nodetype == LYS_ACTION) {
                rpc_act = elem->schema;
                break;
            }
            LY_TREE_DFS_END(rpc->tree, next, elem);
        }
        if (!rpc_act) {
            ERRINT;
            return NC_PSPOLL_ERROR;
        }
    }

    if (!rpc_act->priv) {
        /* no callback, reply with a not-implemented error */
        reply = nc_server_reply_err(nc_err(NC_ERR_OP_NOT_SUPPORTED, NC_ERR_TYPE_PROT));
    } else {
        clb = (nc_rpc_clb)rpc_act->priv;
        reply = clb(rpc->tree, session);
    }

    if (!reply) {
        reply = nc_server_reply_err(nc_err(NC_ERR_OP_FAILED, NC_ERR_TYPE_APP));
    }
    r = nc_write_msg(session, NC_MSG_REPLY, rpc->root, reply);
    if (reply->type == NC_RPL_ERROR) {
        ret |= NC_PSPOLL_REPLY_ERROR;
    }
    nc_server_reply_free(reply);

    if (r == -1) {
        ERR("Session %u: failed to write reply.", session->id);
        ret |= NC_PSPOLL_ERROR;
    }

    /* special case if term_reason was set in callback, last reply was sent (needed for <close-session> if nothing else) */
    if ((session->status == NC_STATUS_RUNNING) && (session->term_reason != NC_SESSION_TERM_NONE)) {
        session->status = NC_STATUS_INVALID;
    }

    return ret;
}

API int
nc_ps_poll(struct nc_pollsession *ps, int timeout, struct nc_session **session)
{
    int ret, r, poll_ret;
    uint8_t q_id;
    uint16_t i, j;
    char msg[256];
    NC_SESSION_TERM_REASON term_reason;
    struct pollfd pfd;
    struct timespec begin_ts, cur_ts;
    struct nc_session *cur_session;
    struct nc_server_rpc *rpc = NULL;
#ifdef NC_ENABLED_SSH
    struct nc_session *new;
#endif

    if (!ps || !ps->session_count) {
        ERRARG("ps");
        return NC_PSPOLL_ERROR;
    }

    nc_gettimespec(&begin_ts);

    /* LOCK */
    if (nc_ps_lock(ps, &q_id, __func__)) {
        return NC_PSPOLL_ERROR;
    }

    /* check that all session are fine */
    for (i = 0; i < ps->session_count; ++i) {
        if (ps->sessions[i]->status != NC_STATUS_RUNNING) {
            ERR("Session %u: session not running.", ps->sessions[i]->id);
            ret = NC_PSPOLL_ERROR;
            if (session) {
                *session = ps->sessions[i];
            }
            goto finish;
        }

        /* TODO invalidate only sessions without subscription */
        if (!(ps->sessions[i]->flags & NC_SESSION_CALLHOME) && server_opts.idle_timeout
                && (begin_ts.tv_sec >= ps->sessions[i]->opts.server.last_rpc + server_opts.idle_timeout)) {
            ERR("Session %u: session idle timeout elapsed.", ps->sessions[i]->id);
            ps->sessions[i]->status = NC_STATUS_INVALID;
            ps->sessions[i]->term_reason = NC_SESSION_TERM_TIMEOUT;
            ret = NC_PSPOLL_SESSION_TERM | NC_PSPOLL_SESSION_ERROR;
            if (session) {
                *session = ps->sessions[i];
            }
            goto finish;
        }
    }

    /* poll on all the sessions one-by-one */
    do {
        /* loop from i to j */
        if (ps->last_event_session == ps->session_count - 1) {
            i = j = 0;
        } else {
            i = j = ps->last_event_session + 1;
        }
        do {
            cur_session = ps->sessions[i];

            switch (cur_session->ti_type) {
#ifdef NC_ENABLED_SSH
            case NC_TI_LIBSSH:
                r = ssh_channel_poll_timeout(cur_session->ti.libssh.channel, 0, 0);
                if (r < 1) {
                    if (r == SSH_EOF) {
                        sprintf(msg, "SSH channel unexpectedly closed");
                        term_reason = NC_SESSION_TERM_DROPPED;
                        poll_ret = -2;
                    } else if (r == SSH_ERROR) {
                        sprintf(msg, "SSH channel poll error (%s)", ssh_get_error(cur_session->ti.libssh.session));
                        term_reason = NC_SESSION_TERM_OTHER;
                        poll_ret = -2;
                    } else {
                        poll_ret = 0;
                    }
                    break;
                }
                /* we have some data, but it may be just an SSH message */

                r = nc_timedlock(cur_session->ti_lock, timeout, __func__);
                if (r < 0) {
                    if (session) {
                        *session = cur_session;
                    }
                    ret = NC_PSPOLL_ERROR;
                    goto finish;
                } else if (!r) {
                    if (session) {
                        *session = cur_session;
                    }
                    ret = NC_PSPOLL_TIMEOUT;
                    goto finish;
                }
                r = ssh_execute_message_callbacks(cur_session->ti.libssh.session);
                pthread_mutex_unlock(cur_session->ti_lock);

                if (r != SSH_OK) {
                    sprintf(msg, "failed to receive SSH messages (%s)", ssh_get_error(cur_session->ti.libssh.session));
                    term_reason = NC_SESSION_TERM_OTHER;
                    poll_ret = -2;
                } else if (cur_session->flags & NC_SESSION_SSH_NEW_MSG) {
                    /* new SSH message */
                    cur_session->flags &= ~NC_SESSION_SSH_NEW_MSG;
                    if (cur_session->ti.libssh.next) {
                        for (new = cur_session->ti.libssh.next; new != cur_session; new = new->ti.libssh.next) {
                            if ((new->status == NC_STATUS_STARTING) && new->ti.libssh.channel
                                    && (new->flags & NC_SESSION_SSH_SUBSYS_NETCONF)) {
                                /* new NETCONF SSH channel */
                                if (session) {
                                    *session = cur_session;
                                }
                                ret = NC_PSPOLL_SSH_CHANNEL;
                                goto finish;
                            }
                        }
                    }

                    /* just some SSH message */
                    if (session) {
                        *session = cur_session;
                    }
                    ret = NC_PSPOLL_SSH_MSG;
                    goto finish;
                } else {
                    /* we have some application data */
                    poll_ret = 1;
                }
                break;
#endif
#ifdef NC_ENABLED_TLS
            case NC_TI_OPENSSL:
                r = SSL_pending(cur_session->ti.tls);
                if (!r) {
                    /* no data pending in the SSL buffer, poll fd */
                    pfd.fd = SSL_get_rfd(cur_session->ti.tls);
                    if (pfd.fd < 0) {
                        ERRINT;
                        ret = NC_PSPOLL_ERROR;
                        goto finish;
                    }
                    pfd.events = POLLIN;
                    pfd.revents = 0;
                    r = poll(&pfd, 1, 0);

                    if (r < 0) {
                        sprintf(msg, "poll failed (%s)", strerror(errno));
                        poll_ret = -1;
                    } else if (r > 0) {
                        if (pfd.revents & (POLLHUP | POLLNVAL)) {
                            sprintf(msg, "communication socket unexpectedly closed");
                            term_reason = NC_SESSION_TERM_DROPPED;
                            poll_ret = -2;
                        } else if (pfd.revents & POLLERR) {
                            sprintf(msg, "communication socket error");
                            term_reason = NC_SESSION_TERM_OTHER;
                            poll_ret = -2;
                        } else {
                            poll_ret = 1;
                        }
                    } else {
                        poll_ret = 0;
                    }
                } else {
                    poll_ret = 1;
                }
                break;
#endif
            case NC_TI_FD:
                pfd.fd = cur_session->ti.fd.in;
                pfd.events = POLLIN;
                pfd.revents = 0;
                r = poll(&pfd, 1, 0);

                if (r < 0) {
                    sprintf(msg, "poll failed (%s)", strerror(errno));
                    poll_ret = -1;
                } else if (r > 0) {
                    if (pfd.revents & (POLLHUP | POLLNVAL)) {
                        sprintf(msg, "communication socket unexpectedly closed");
                        term_reason = NC_SESSION_TERM_DROPPED;
                        poll_ret = -2;
                    } else if (pfd.revents & POLLERR) {
                        sprintf(msg, "communication socket error");
                        term_reason = NC_SESSION_TERM_OTHER;
                        poll_ret = -2;
                    } else {
                        poll_ret = 1;
                    }
                } else {
                    poll_ret = 0;
                }
                break;
            case NC_TI_NONE:
                ERRINT;
                ret = NC_PSPOLL_ERROR;
                goto finish;
            }

             /* here: poll_ret == -2 - session error, session terminated,
              *       poll_ret == -1 - generic error,
              *       poll_ret == 0 - nothing to read,
              *       poll_ret > 0 - data available */
            if (poll_ret == -2) {
                ERR("Session %u: %s.", cur_session->id, msg);
                cur_session->status = NC_STATUS_INVALID;
                cur_session->term_reason = term_reason;
                if (session) {
                    *session = cur_session;
                }
                ret = NC_PSPOLL_SESSION_TERM | NC_PSPOLL_SESSION_ERROR;
                goto finish;
            } else if (poll_ret == -1) {
                ERR("Session %u: %s.", cur_session->id, msg);
                ret = NC_PSPOLL_ERROR;
                goto finish;
            } else if (poll_ret > 0) {
                break;
            }

            /* next iteration */
            if (i == ps->session_count - 1) {
                i = 0;
            } else {
                ++i;
            }
        } while (i != j);

        /* no event */
        if (!poll_ret && (timeout > -1)) {
            usleep(NC_TIMEOUT_STEP);

            nc_gettimespec(&cur_ts);
            /* final timeout */
            if (nc_difftimespec(&begin_ts, &cur_ts) >= (unsigned)timeout) {
                ret = NC_PSPOLL_TIMEOUT;
                goto finish;
            }
        }
    } while (!poll_ret);

    /* this is the session with some data available for reading */
    if (session) {
        *session = cur_session;
    }
    ps->last_event_session = i;

    /* reading an RPC and sending a reply must be atomic (no other RPC should be read) */
    r = nc_timedlock(cur_session->ti_lock, timeout, __func__);
    if (r < 0) {
        ret = NC_PSPOLL_ERROR;
        goto finish;
    } else if (!r) {
        ret = NC_PSPOLL_TIMEOUT;
        goto finish;
    }

    ret = nc_server_recv_rpc(cur_session, &rpc);
    if (ret & (NC_PSPOLL_ERROR | NC_PSPOLL_BAD_RPC)) {
        pthread_mutex_unlock(cur_session->ti_lock);
        if (cur_session->status != NC_STATUS_RUNNING) {
            ret |= NC_PSPOLL_SESSION_TERM | NC_PSPOLL_SESSION_ERROR;
        }
        goto finish;
    }

    cur_session->opts.server.last_rpc = time(NULL);

    /* process RPC */
    ret |= nc_server_send_reply(cur_session, rpc);

    pthread_mutex_unlock(cur_session->ti_lock);
    if (cur_session->status != NC_STATUS_RUNNING) {
        ret |= NC_PSPOLL_SESSION_TERM;
        if (!(cur_session->term_reason & (NC_SESSION_TERM_CLOSED | NC_SESSION_TERM_KILLED))) {
            ret |= NC_PSPOLL_SESSION_ERROR;
        }
    }

    nc_server_rpc_free(rpc, server_opts.ctx);

finish:
    /* UNLOCK */
    nc_ps_unlock(ps, q_id, __func__);
    return ret;
}

API void
nc_ps_clear(struct nc_pollsession *ps, int all, void (*data_free)(void *))
{
    uint8_t q_id;
    uint16_t i;
    struct nc_session *session;

    if (!ps) {
        ERRARG("ps");
        return;
    }

    /* LOCK */
    if (nc_ps_lock(ps, &q_id, __func__)) {
        return;
    }

    if (all) {
        for (i = 0; i < ps->session_count; i++) {
            nc_session_free(ps->sessions[i], data_free);
        }
        free(ps->sessions);
        ps->sessions = NULL;
        ps->session_count = 0;
        ps->last_event_session = 0;
    } else {
        for (i = 0; i < ps->session_count; ) {
            if (ps->sessions[i]->status != NC_STATUS_RUNNING) {
                session = ps->sessions[i];
                _nc_ps_del_session(ps, NULL, i);
                nc_session_free(session, data_free);
                continue;
            }

            ++i;
        }
    }

    /* UNLOCK */
    nc_ps_unlock(ps, q_id, __func__);
}

#if defined(NC_ENABLED_SSH) || defined(NC_ENABLED_TLS)

API int
nc_server_add_endpt(const char *name, NC_TRANSPORT_IMPL ti)
{
    uint16_t i;

    if (!name) {
        ERRARG("name");
        return -1;
    }

    /* WRITE LOCK */
    pthread_rwlock_wrlock(&server_opts.endpt_lock);

    /* check name uniqueness */
    for (i = 0; i < server_opts.endpt_count; ++i) {
        if (!strcmp(server_opts.endpts[i].name, name)) {
            ERR("Endpoint \"%s\" already exists.", name);
            /* WRITE UNLOCK */
            pthread_rwlock_unlock(&server_opts.endpt_lock);
            return -1;
        }
    }

    ++server_opts.endpt_count;
    server_opts.endpts = nc_realloc(server_opts.endpts, server_opts.endpt_count * sizeof *server_opts.endpts);
    if (!server_opts.endpts) {
        ERRMEM;
        /* WRITE UNLOCK */
        pthread_rwlock_unlock(&server_opts.endpt_lock);
        return -1;
    }
    server_opts.endpts[server_opts.endpt_count - 1].name = lydict_insert(server_opts.ctx, name, 0);
    server_opts.endpts[server_opts.endpt_count - 1].ti = ti;

    server_opts.binds = nc_realloc(server_opts.binds, server_opts.endpt_count * sizeof *server_opts.binds);
    if (!server_opts.binds) {
        ERRMEM;
        /* WRITE UNLOCK */
        pthread_rwlock_unlock(&server_opts.endpt_lock);
        return -1;
    }

    server_opts.binds[server_opts.endpt_count - 1].address = NULL;
    server_opts.binds[server_opts.endpt_count - 1].port = 0;
    server_opts.binds[server_opts.endpt_count - 1].sock = -1;
    server_opts.binds[server_opts.endpt_count - 1].pollin = 0;

    switch (ti) {
#ifdef NC_ENABLED_SSH
    case NC_TI_LIBSSH:
        server_opts.endpts[server_opts.endpt_count - 1].opts.ssh = calloc(1, sizeof(struct nc_server_ssh_opts));
        if (!server_opts.endpts[server_opts.endpt_count - 1].opts.ssh) {
            ERRMEM;
            /* WRITE UNLOCK */
            pthread_rwlock_unlock(&server_opts.endpt_lock);
            return -1;
        }
        server_opts.endpts[server_opts.endpt_count - 1].opts.ssh->auth_methods =
            NC_SSH_AUTH_PUBLICKEY | NC_SSH_AUTH_PASSWORD | NC_SSH_AUTH_INTERACTIVE;
        server_opts.endpts[server_opts.endpt_count - 1].opts.ssh->auth_attempts = 3;
        server_opts.endpts[server_opts.endpt_count - 1].opts.ssh->auth_timeout = 10;
        break;
#endif
#ifdef NC_ENABLED_TLS
    case NC_TI_OPENSSL:
        server_opts.endpts[server_opts.endpt_count - 1].opts.tls = calloc(1, sizeof(struct nc_server_tls_opts));
        if (!server_opts.endpts[server_opts.endpt_count - 1].opts.tls) {
            ERRMEM;
            /* WRITE UNLOCK */
            pthread_rwlock_unlock(&server_opts.endpt_lock);
            return -1;
        }
        break;
#endif
    default:
        ERRINT;
        /* WRITE UNLOCK */
        pthread_rwlock_unlock(&server_opts.endpt_lock);
        return -1;
    }

    pthread_mutex_init(&server_opts.endpts[server_opts.endpt_count - 1].lock, NULL);

    /* WRITE UNLOCK */
    pthread_rwlock_unlock(&server_opts.endpt_lock);

    return 0;
}

int
nc_server_endpt_set_address_port(const char *endpt_name, const char *address, uint16_t port)
{
    struct nc_endpt *endpt;
    struct nc_bind *bind = NULL;
    uint16_t i;
    int sock = -1, set_addr, ret = 0;

    if (!endpt_name) {
        ERRARG("endpt_name");
        return -1;
    } else if ((!address && !port) || (address && port)) {
        ERRARG("address and port");
        return -1;
    }

    if (address) {
        set_addr = 1;
    } else {
        set_addr = 0;
    }

    /* LOCK */
    endpt = nc_server_endpt_lock(endpt_name, 0, &i);
    if (!endpt) {
        return -1;
    }

    bind = &server_opts.binds[i];

    if (set_addr) {
        port = bind->port;
    } else {
        address = bind->address;
    }

    /* we have all the information we need to create a listening socket */
    if (address && port) {
        /* create new socket, close the old one */
        sock = nc_sock_listen(address, port);
        if (sock == -1) {
            ret = -1;
            goto cleanup;
        }

        if (bind->sock > -1) {
            close(bind->sock);
        }
        bind->sock = sock;
    } /* else we are just setting address or port */

    if (set_addr) {
        lydict_remove(server_opts.ctx, bind->address);
        bind->address = lydict_insert(server_opts.ctx, address, 0);
    } else {
        bind->port = port;
    }

    if (sock > -1) {
#if defined(NC_ENABLED_SSH) && defined(NC_ENABLED_TLS)
        VRB("Listening on %s:%u for %s connections.", address, port, (endpt->ti == NC_TI_LIBSSH ? "SSH" : "TLS"));
#elif defined(NC_ENABLED_SSH)
        VRB("Listening on %s:%u for SSH connections.", address, port);
#else
        VRB("Listening on %s:%u for TLS connections.", address, port);
#endif
    }

cleanup:
    /* UNLOCK */
    nc_server_endpt_unlock(endpt);

    return ret;
}

API int
nc_server_endpt_set_address(const char *endpt_name, const char *address)
{
    return nc_server_endpt_set_address_port(endpt_name, address, 0);
}

API int
nc_server_endpt_set_port(const char *endpt_name, uint16_t port)
{
    return nc_server_endpt_set_address_port(endpt_name, NULL, port);
}

API int
nc_server_del_endpt(const char *name, NC_TRANSPORT_IMPL ti)
{
    uint32_t i;
    int ret = -1;

    /* WRITE LOCK */
    pthread_rwlock_wrlock(&server_opts.endpt_lock);

    if (!name && !ti) {
        /* remove all endpoints */
        for (i = 0; i < server_opts.endpt_count; ++i) {
            lydict_remove(server_opts.ctx, server_opts.endpts[i].name);
            pthread_mutex_destroy(&server_opts.endpts[i].lock);
            switch (server_opts.endpts[i].ti) {
#ifdef NC_ENABLED_SSH
            case NC_TI_LIBSSH:
                nc_server_ssh_clear_opts(server_opts.endpts[i].opts.ssh);
                free(server_opts.endpts[i].opts.ssh);
                break;
#endif
#ifdef NC_ENABLED_TLS
            case NC_TI_OPENSSL:
                nc_server_tls_clear_opts(server_opts.endpts[i].opts.tls);
                free(server_opts.endpts[i].opts.tls);
                break;
#endif
            default:
                ERRINT;
                /* won't get here ...*/
                break;
            }
            ret = 0;
        }
        free(server_opts.endpts);
        server_opts.endpts = NULL;

        /* remove all binds */
        for (i = 0; i < server_opts.endpt_count; ++i) {
            lydict_remove(server_opts.ctx, server_opts.binds[i].address);
            if (server_opts.binds[i].sock > -1) {
                close(server_opts.binds[i].sock);
            }
        }
        free(server_opts.binds);
        server_opts.binds = NULL;

        server_opts.endpt_count = 0;

    } else {
        /* remove one endpoint with bind(s) or all endpoints using one transport protocol */
        for (i = 0; i < server_opts.endpt_count; ++i) {
            if ((name && !strcmp(server_opts.endpts[i].name, name)) || (!name && (server_opts.endpts[i].ti == ti))) {
                /* remove endpt */
                lydict_remove(server_opts.ctx, server_opts.endpts[i].name);
                pthread_mutex_destroy(&server_opts.endpts[i].lock);
                switch (server_opts.endpts[i].ti) {
#ifdef NC_ENABLED_SSH
                case NC_TI_LIBSSH:
                    nc_server_ssh_clear_opts(server_opts.endpts[i].opts.ssh);
                    free(server_opts.endpts[i].opts.ssh);
                    break;
#endif
#ifdef NC_ENABLED_TLS
                case NC_TI_OPENSSL:
                    nc_server_tls_clear_opts(server_opts.endpts[i].opts.tls);
                    free(server_opts.endpts[i].opts.tls);
                    break;
#endif
                default:
                    ERRINT;
                    break;
                }

                /* remove bind(s) */
                lydict_remove(server_opts.ctx, server_opts.binds[i].address);
                if (server_opts.binds[i].sock > -1) {
                    close(server_opts.binds[i].sock);
                }

                /* move last endpt and bind(s) to the empty space */
                --server_opts.endpt_count;
                if (!server_opts.endpt_count) {
                    free(server_opts.binds);
                    server_opts.binds = NULL;
                    free(server_opts.endpts);
                    server_opts.endpts = NULL;
                } else if (i < server_opts.endpt_count) {
                    memcpy(&server_opts.binds[i], &server_opts.binds[server_opts.endpt_count], sizeof *server_opts.binds);
                    memcpy(&server_opts.endpts[i], &server_opts.endpts[server_opts.endpt_count], sizeof *server_opts.endpts);
                }

                ret = 0;
                if (name) {
                    break;
                }
            }
        }
    }

    /* WRITE UNLOCK */
    pthread_rwlock_unlock(&server_opts.endpt_lock);

    return ret;
}

API NC_MSG_TYPE
nc_accept(int timeout, struct nc_session **session)
{
    NC_MSG_TYPE msgtype;
    int sock, ret;
    char *host = NULL;
    uint16_t port, bind_idx;

    if (!server_opts.ctx) {
        ERRINIT;
        return NC_MSG_ERROR;
    } else if (!session) {
        ERRARG("session");
        return NC_MSG_ERROR;
    }

    /* we have to hold WRITE for the whole time, since there is not
     * a way of downgrading the lock to READ */
    /* WRITE LOCK */
    pthread_rwlock_wrlock(&server_opts.endpt_lock);

    if (!server_opts.endpt_count) {
        ERR("No endpoints to accept sessions on.");
        /* WRITE UNLOCK */
        pthread_rwlock_unlock(&server_opts.endpt_lock);
        return NC_MSG_ERROR;
    }

    ret = nc_sock_accept_binds(server_opts.binds, server_opts.endpt_count, timeout, &host, &port, &bind_idx);
    if (ret < 1) {
        /* WRITE UNLOCK */
        pthread_rwlock_unlock(&server_opts.endpt_lock);
        free(host);
        if (!ret) {
            return NC_MSG_WOULDBLOCK;
        }
        return NC_MSG_ERROR;
    }
    sock = ret;

    *session = calloc(1, sizeof **session);
    if (!(*session)) {
        ERRMEM;
        close(sock);
        free(host);
        msgtype = NC_MSG_ERROR;
        goto cleanup;
    }
    (*session)->status = NC_STATUS_STARTING;
    (*session)->side = NC_SERVER;
    (*session)->ctx = server_opts.ctx;
    (*session)->flags = NC_SESSION_SHAREDCTX;
    (*session)->host = lydict_insert_zc(server_opts.ctx, host);
    (*session)->port = port;

    /* transport lock */
    (*session)->ti_lock = malloc(sizeof *(*session)->ti_lock);
    if (!(*session)->ti_lock) {
        ERRMEM;
        close(sock);
        msgtype = NC_MSG_ERROR;
        goto cleanup;
    }
    pthread_mutex_init((*session)->ti_lock, NULL);

    /* sock gets assigned to session or closed */
#ifdef NC_ENABLED_SSH
    if (server_opts.endpts[bind_idx].ti == NC_TI_LIBSSH) {
        (*session)->data = server_opts.endpts[bind_idx].opts.ssh;
        ret = nc_accept_ssh_session(*session, sock, timeout);
        if (ret < 0) {
            msgtype = NC_MSG_ERROR;
            goto cleanup;
        } else if (!ret) {
            msgtype = NC_MSG_WOULDBLOCK;
            goto cleanup;
        }
    } else
#endif
#ifdef NC_ENABLED_TLS
    if (server_opts.endpts[bind_idx].ti == NC_TI_OPENSSL) {
        (*session)->data = server_opts.endpts[bind_idx].opts.tls;
        ret = nc_accept_tls_session(*session, sock, timeout);
        if (ret < 0) {
            msgtype = NC_MSG_ERROR;
            goto cleanup;
        } else if (!ret) {
            msgtype = NC_MSG_WOULDBLOCK;
            goto cleanup;
        }
    } else
#endif
    {
        ERRINT;
        close(sock);
        msgtype = NC_MSG_ERROR;
        goto cleanup;
    }

    (*session)->data = NULL;

    /* WRITE UNLOCK */
    pthread_rwlock_unlock(&server_opts.endpt_lock);

    /* assign new SID atomically */
    /* LOCK */
    pthread_spin_lock(&server_opts.sid_lock);
    (*session)->id = server_opts.new_session_id++;
    /* UNLOCK */
    pthread_spin_unlock(&server_opts.sid_lock);

    /* NETCONF handshake */
    msgtype = nc_handshake(*session);
    if (msgtype != NC_MSG_HELLO) {
        nc_session_free(*session, NULL);
        *session = NULL;
        return msgtype;
    }
    (*session)->opts.server.session_start = (*session)->opts.server.last_rpc = time(NULL);
    (*session)->status = NC_STATUS_RUNNING;

    return msgtype;

cleanup:
    /* WRITE UNLOCK */
    pthread_rwlock_unlock(&server_opts.endpt_lock);

    nc_session_free(*session, NULL);
    *session = NULL;
    return msgtype;
}

API int
nc_server_ch_add_client(const char *name, NC_TRANSPORT_IMPL ti)
{
    uint16_t i;

    if (!name) {
        ERRARG("name");
        return -1;
    } else if (!ti) {
        ERRARG("ti");
        return -1;
    }

    /* WRITE LOCK */
    pthread_rwlock_wrlock(&server_opts.ch_client_lock);

    /* check name uniqueness */
    for (i = 0; i < server_opts.ch_client_count; ++i) {
        if (!strcmp(server_opts.ch_clients[i].name, name)) {
            ERR("Call Home client \"%s\" already exists.", name);
            /* WRITE UNLOCK */
            pthread_rwlock_unlock(&server_opts.ch_client_lock);
            return -1;
        }
    }

    ++server_opts.ch_client_count;
    server_opts.ch_clients = nc_realloc(server_opts.ch_clients, server_opts.ch_client_count * sizeof *server_opts.ch_clients);
    if (!server_opts.ch_clients) {
        ERRMEM;
        /* WRITE UNLOCK */
        pthread_rwlock_unlock(&server_opts.ch_client_lock);
        return -1;
    }
    server_opts.ch_clients[server_opts.ch_client_count - 1].name = lydict_insert(server_opts.ctx, name, 0);
    server_opts.ch_clients[server_opts.ch_client_count - 1].ti = ti;
    server_opts.ch_clients[server_opts.ch_client_count - 1].ch_endpts = NULL;
    server_opts.ch_clients[server_opts.ch_client_count - 1].ch_endpt_count = 0;

    switch (ti) {
#ifdef NC_ENABLED_SSH
    case NC_TI_LIBSSH:
        server_opts.ch_clients[server_opts.ch_client_count - 1].opts.ssh = calloc(1, sizeof(struct nc_server_ssh_opts));
        if (!server_opts.ch_clients[server_opts.ch_client_count - 1].opts.ssh) {
            ERRMEM;
            /* WRITE UNLOCK */
            pthread_rwlock_unlock(&server_opts.ch_client_lock);
            return -1;
        }
        server_opts.ch_clients[server_opts.ch_client_count - 1].opts.ssh->auth_methods =
            NC_SSH_AUTH_PUBLICKEY | NC_SSH_AUTH_PASSWORD | NC_SSH_AUTH_INTERACTIVE;
        server_opts.ch_clients[server_opts.ch_client_count - 1].opts.ssh->auth_attempts = 3;
        server_opts.ch_clients[server_opts.ch_client_count - 1].opts.ssh->auth_timeout = 10;
        break;
#endif
#ifdef NC_ENABLED_TLS
    case NC_TI_OPENSSL:
        server_opts.ch_clients[server_opts.ch_client_count - 1].opts.tls = calloc(1, sizeof(struct nc_server_tls_opts));
        if (!server_opts.ch_clients[server_opts.ch_client_count - 1].opts.tls) {
            ERRMEM;
            /* WRITE UNLOCK */
            pthread_rwlock_unlock(&server_opts.ch_client_lock);
            return -1;
        }
        break;
#endif
    default:
        ERRINT;
        /* WRITE UNLOCK */
        pthread_rwlock_unlock(&server_opts.ch_client_lock);
        return -1;
    }

    server_opts.ch_clients[server_opts.ch_client_count - 1].conn_type = 0;

    /* set CH default options */
    server_opts.ch_clients[server_opts.ch_client_count - 1].start_with = NC_CH_FIRST_LISTED;
    server_opts.ch_clients[server_opts.ch_client_count - 1].max_attempts = 3;

    pthread_mutex_init(&server_opts.ch_clients[server_opts.ch_client_count - 1].lock, NULL);

    /* WRITE UNLOCK */
    pthread_rwlock_unlock(&server_opts.ch_client_lock);

    return 0;
}

API int
nc_server_ch_del_client(const char *name, NC_TRANSPORT_IMPL ti)
{
    uint16_t i, j;
    int ret = -1;

    /* WRITE LOCK */
    pthread_rwlock_wrlock(&server_opts.ch_client_lock);

    if (!name && !ti) {
        /* remove all CH clients */
        for (i = 0; i < server_opts.ch_client_count; ++i) {
            lydict_remove(server_opts.ctx, server_opts.ch_clients[i].name);

            /* remove all endpoints */
            for (j = 0; j < server_opts.ch_clients[i].ch_endpt_count; ++j) {
                lydict_remove(server_opts.ctx, server_opts.ch_clients[i].ch_endpts[j].name);
                lydict_remove(server_opts.ctx, server_opts.ch_clients[i].ch_endpts[j].address);
            }
            free(server_opts.ch_clients[i].ch_endpts);

            switch (server_opts.ch_clients[i].ti) {
#ifdef NC_ENABLED_SSH
            case NC_TI_LIBSSH:
                nc_server_ssh_clear_opts(server_opts.ch_clients[i].opts.ssh);
                free(server_opts.ch_clients[i].opts.ssh);
                break;
#endif
#ifdef NC_ENABLED_TLS
            case NC_TI_OPENSSL:
                nc_server_tls_clear_opts(server_opts.ch_clients[i].opts.tls);
                free(server_opts.ch_clients[i].opts.tls);
                break;
#endif
            default:
                ERRINT;
                /* won't get here ...*/
                break;
            }

            pthread_mutex_destroy(&server_opts.ch_clients[i].lock);

            ret = 0;
        }
        free(server_opts.ch_clients);
        server_opts.ch_clients = NULL;

        server_opts.ch_client_count = 0;

    } else {
        /* remove one client with endpoint(s) or all clients using one protocol */
        for (i = 0; i < server_opts.ch_client_count; ++i) {
            if ((name && !strcmp(server_opts.ch_clients[i].name, name)) || (!name && (server_opts.ch_clients[i].ti == ti))) {
                /* remove endpt */
                lydict_remove(server_opts.ctx, server_opts.ch_clients[i].name);

                switch (server_opts.ch_clients[i].ti) {
#ifdef NC_ENABLED_SSH
                case NC_TI_LIBSSH:
                    nc_server_ssh_clear_opts(server_opts.ch_clients[i].opts.ssh);
                    free(server_opts.ch_clients[i].opts.ssh);
                    break;
#endif
#ifdef NC_ENABLED_TLS
                case NC_TI_OPENSSL:
                    nc_server_tls_clear_opts(server_opts.ch_clients[i].opts.tls);
                    free(server_opts.ch_clients[i].opts.tls);
                    break;
#endif
                default:
                    ERRINT;
                    break;
                }

                /* remove all endpoints */
                for (j = 0; j < server_opts.ch_clients[i].ch_endpt_count; ++j) {
                    lydict_remove(server_opts.ctx, server_opts.ch_clients[i].ch_endpts[j].name);
                    lydict_remove(server_opts.ctx, server_opts.ch_clients[i].ch_endpts[j].address);
                }
                free(server_opts.ch_clients[i].ch_endpts);

                pthread_mutex_destroy(&server_opts.ch_clients[i].lock);

                /* move last client and endpoint(s) to the empty space */
                --server_opts.ch_client_count;
                if (i < server_opts.ch_client_count) {
                    memcpy(&server_opts.ch_clients[i], &server_opts.ch_clients[server_opts.ch_client_count],
                           sizeof *server_opts.ch_clients);
                } else if (!server_opts.ch_client_count) {
                    free(server_opts.ch_clients);
                    server_opts.ch_clients = NULL;
                }

                ret = 0;
                if (name) {
                    break;
                }
            }
        }
    }

    /* WRITE UNLOCK */
    pthread_rwlock_unlock(&server_opts.ch_client_lock);

    return ret;
}

API int
nc_server_ch_client_add_endpt(const char *client_name, const char *endpt_name)
{
    uint16_t i;
    struct nc_ch_client *client;

    if (!client_name) {
        ERRARG("client_name");
        return -1;
    } else if (!endpt_name) {
        ERRARG("endpt_name");
        return -1;
    }

    /* LOCK */
    client = nc_server_ch_client_lock(client_name, 0, NULL);
    if (!client) {
        return -1;
    }

    for (i = 0; i < client->ch_endpt_count; ++i) {
        if (!strcmp(client->ch_endpts[i].name, endpt_name)) {
            ERR("Call Home client \"%s\" endpoint \"%s\" already exists.", client_name, endpt_name);
            /* UNLOCK */
            nc_server_ch_client_unlock(client);
            return -1;
        }
    }

    ++client->ch_endpt_count;
    client->ch_endpts = realloc(client->ch_endpts, client->ch_endpt_count * sizeof *client->ch_endpts);
    if (!client->ch_endpts) {
        ERRMEM;
        /* UNLOCK */
        nc_server_ch_client_unlock(client);
        return -1;
    }

    client->ch_endpts[client->ch_endpt_count - 1].name = lydict_insert(server_opts.ctx, endpt_name, 0);
    client->ch_endpts[client->ch_endpt_count - 1].address = NULL;
    client->ch_endpts[client->ch_endpt_count - 1].port = 0;

    /* UNLOCK */
    nc_server_ch_client_unlock(client);

    return 0;
}

API int
nc_server_ch_client_del_endpt(const char *client_name, const char *endpt_name)
{
    uint16_t i;
    int ret = -1;
    struct nc_ch_client *client;

    if (!client_name) {
        ERRARG("client_name");
        return -1;
    }

    /* LOCK */
    client = nc_server_ch_client_lock(client_name, 0, NULL);
    if (!client) {
        return -1;
    }

    if (!endpt_name) {
        /* remove all endpoints */
        for (i = 0; i < client->ch_endpt_count; ++i) {
            lydict_remove(server_opts.ctx, client->ch_endpts[i].name);
            lydict_remove(server_opts.ctx, client->ch_endpts[i].address);
        }
        free(client->ch_endpts);
        client->ch_endpts = NULL;
        client->ch_endpt_count = 0;

        ret = 0;
    } else {
        for (i = 0; i < client->ch_endpt_count; ++i) {
            if (!strcmp(client->ch_endpts[i].name, endpt_name)) {
                lydict_remove(server_opts.ctx, client->ch_endpts[i].name);
                lydict_remove(server_opts.ctx, client->ch_endpts[i].address);

                /* move last endpoint to the empty space */
                --client->ch_endpt_count;
                if (i < client->ch_endpt_count) {
                    memcpy(&client->ch_endpts[i], &client->ch_endpts[client->ch_endpt_count], sizeof *client->ch_endpts);
                } else if (!server_opts.ch_client_count) {
                    free(server_opts.ch_clients);
                    server_opts.ch_clients = NULL;
                }

                ret = 0;
                break;
            }
        }
    }

    /* UNLOCK */
    nc_server_ch_client_unlock(client);

    return ret;
}

API int
nc_server_ch_client_endpt_set_address(const char *client_name, const char *endpt_name, const char *address)
{
    uint16_t i;
    int ret = -1;
    struct nc_ch_client *client;

    if (!client_name) {
        ERRARG("client_name");
        return -1;
    } else if (!endpt_name) {
        ERRARG("endpt_name");
        return -1;
    } else if (!address) {
        ERRARG("address");
        return -1;
    }

    /* LOCK */
    client = nc_server_ch_client_lock(client_name, 0, NULL);
    if (!client) {
        return -1;
    }

    for (i = 0; i < client->ch_endpt_count; ++i) {
        if (!strcmp(client->ch_endpts[i].name, endpt_name)) {
            lydict_remove(server_opts.ctx, client->ch_endpts[i].address);
            client->ch_endpts[i].address = lydict_insert(server_opts.ctx, address, 0);

            ret = 0;
            break;
        }
    }

    /* UNLOCK */
    nc_server_ch_client_unlock(client);

    if (ret == -1) {
        ERR("Call Home client \"%s\" endpoint \"%s\" not found.", client_name, endpt_name);
    }

    return ret;
}

API int
nc_server_ch_client_endpt_set_port(const char *client_name, const char *endpt_name, uint16_t port)
{
    uint16_t i;
    int ret = -1;
    struct nc_ch_client *client;

    if (!client_name) {
        ERRARG("client_name");
        return -1;
    } else if (!endpt_name) {
        ERRARG("endpt_name");
        return -1;
    } else if (!port) {
        ERRARG("port");
        return -1;
    }

    /* LOCK */
    client = nc_server_ch_client_lock(client_name, 0, NULL);
    if (!client) {
        return -1;
    }

    for (i = 0; i < client->ch_endpt_count; ++i) {
        if (!strcmp(client->ch_endpts[i].name, endpt_name)) {
            client->ch_endpts[i].port = port;

            ret = 0;
            break;
        }
    }

    /* UNLOCK */
    nc_server_ch_client_unlock(client);

    if (ret == -1) {
        ERR("Call Home client \"%s\" endpoint \"%s\" not found.", client_name, endpt_name);
    }

    return ret;
}

API int
nc_server_ch_client_set_conn_type(const char *client_name, NC_CH_CONN_TYPE conn_type)
{
    struct nc_ch_client *client;

    if (!client_name) {
        ERRARG("client_name");
        return -1;
    } else if (!conn_type) {
        ERRARG("conn_type");
        return -1;
    }

    /* LOCK */
    client = nc_server_ch_client_lock(client_name, 0, NULL);
    if (!client) {
        return -1;
    }

    if (client->conn_type != conn_type) {
        client->conn_type = conn_type;

        /* set default options */
        switch (conn_type) {
        case NC_CH_PERSIST:
            client->conn.persist.idle_timeout = 86400;
            client->conn.persist.ka_max_wait = 30;
            client->conn.persist.ka_max_attempts = 3;
            break;
        case NC_CH_PERIOD:
            client->conn.period.idle_timeout = 300;
            client->conn.period.reconnect_timeout = 60;
            break;
        default:
            ERRINT;
            break;
        }
    }

    /* UNLOCK */
    nc_server_ch_client_unlock(client);

    return 0;
}

API int
nc_server_ch_client_persist_set_idle_timeout(const char *client_name, uint32_t idle_timeout)
{
    struct nc_ch_client *client;

    if (!client_name) {
        ERRARG("client_name");
        return -1;
    }

    /* LOCK */
    client = nc_server_ch_client_lock(client_name, 0, NULL);
    if (!client) {
        return -1;
    }

    if (client->conn_type != NC_CH_PERSIST) {
        ERR("Call Home client \"%s\" is not of persistent connection type.");
        /* UNLOCK */
        nc_server_ch_client_unlock(client);
        return -1;
    }

    client->conn.persist.idle_timeout = idle_timeout;

    /* UNLOCK */
    nc_server_ch_client_unlock(client);

    return 0;
}

API int
nc_server_ch_client_persist_set_keep_alive_max_wait(const char *client_name, uint16_t max_wait)
{
    struct nc_ch_client *client;

    if (!client_name) {
        ERRARG("client_name");
        return -1;
    } else if (!max_wait) {
        ERRARG("max_wait");
        return -1;
    }

    /* LOCK */
    client = nc_server_ch_client_lock(client_name, 0, NULL);
    if (!client) {
        return -1;
    }

    if (client->conn_type != NC_CH_PERSIST) {
        ERR("Call Home client \"%s\" is not of persistent connection type.");
        /* UNLOCK */
        nc_server_ch_client_unlock(client);
        return -1;
    }

    client->conn.persist.ka_max_wait = max_wait;

    /* UNLOCK */
    nc_server_ch_client_unlock(client);

    return 0;
}

API int
nc_server_ch_client_persist_set_keep_alive_max_attempts(const char *client_name, uint8_t max_attempts)
{
    struct nc_ch_client *client;

    if (!client_name) {
        ERRARG("client_name");
        return -1;
    }

    /* LOCK */
    client = nc_server_ch_client_lock(client_name, 0, NULL);
    if (!client) {
        return -1;
    }

    if (client->conn_type != NC_CH_PERSIST) {
        ERR("Call Home client \"%s\" is not of persistent connection type.");
        /* UNLOCK */
        nc_server_ch_client_unlock(client);
        return -1;
    }

    client->conn.persist.ka_max_attempts = max_attempts;

    /* UNLOCK */
    nc_server_ch_client_unlock(client);

    return 0;
}

API int
nc_server_ch_client_period_set_idle_timeout(const char *client_name, uint16_t idle_timeout)
{
    struct nc_ch_client *client;

    if (!client_name) {
        ERRARG("client_name");
        return -1;
    }

    /* LOCK */
    client = nc_server_ch_client_lock(client_name, 0, NULL);
    if (!client) {
        return -1;
    }

    if (client->conn_type != NC_CH_PERIOD) {
        ERR("Call Home client \"%s\" is not of periodic connection type.");
        /* UNLOCK */
        nc_server_ch_client_unlock(client);
        return -1;
    }

    client->conn.period.idle_timeout = idle_timeout;

    /* UNLOCK */
    nc_server_ch_client_unlock(client);

    return 0;
}

API int
nc_server_ch_client_period_set_reconnect_timeout(const char *client_name, uint16_t reconnect_timeout)
{
    struct nc_ch_client *client;

    if (!client_name) {
        ERRARG("client_name");
        return -1;
    } else if (!reconnect_timeout) {
        ERRARG("reconnect_timeout");
        return -1;
    }

    /* LOCK */
    client = nc_server_ch_client_lock(client_name, 0, NULL);
    if (!client) {
        return -1;
    }

    if (client->conn_type != NC_CH_PERIOD) {
        ERR("Call Home client \"%s\" is not of periodic connection type.");
        /* UNLOCK */
        nc_server_ch_client_unlock(client);
        return -1;
    }

    client->conn.period.reconnect_timeout = reconnect_timeout;

    /* UNLOCK */
    nc_server_ch_client_unlock(client);

    return 0;
}

API int
nc_server_ch_client_set_start_with(const char *client_name, NC_CH_START_WITH start_with)
{
    struct nc_ch_client *client;

    if (!client_name) {
        ERRARG("client_name");
        return -1;
    }

    /* LOCK */
    client = nc_server_ch_client_lock(client_name, 0, NULL);
    if (!client) {
        return -1;
    }

    client->start_with = start_with;

    /* UNLOCK */
    nc_server_ch_client_unlock(client);

    return 0;
}

API int
nc_server_ch_client_set_max_attempts(const char *client_name, uint8_t max_attempts)
{
    struct nc_ch_client *client;

    if (!client_name) {
        ERRARG("client_name");
        return -1;
    } else if (!max_attempts) {
        ERRARG("max_attempts");
        return -1;
    }

    /* LOCK */
    client = nc_server_ch_client_lock(client_name, 0, NULL);
    if (!client) {
        return -1;
    }

    client->max_attempts = max_attempts;

    /* UNLOCK */
    nc_server_ch_client_unlock(client);

    return 0;
}

/* client lock is expected to be held */
static NC_MSG_TYPE
nc_connect_ch_client_endpt(struct nc_ch_client *client, struct nc_ch_endpt *endpt, struct nc_session **session)
{
    NC_MSG_TYPE msgtype;
    int sock, ret;

    sock = nc_sock_connect(endpt->address, endpt->port);
    if (sock < 0) {
        return NC_MSG_ERROR;
    }

    *session = calloc(1, sizeof **session);
    if (!(*session)) {
        ERRMEM;
        close(sock);
        return NC_MSG_ERROR;
    }
    (*session)->status = NC_STATUS_STARTING;
    (*session)->side = NC_SERVER;
    (*session)->ctx = server_opts.ctx;
    (*session)->flags = NC_SESSION_SHAREDCTX;
    (*session)->host = lydict_insert(server_opts.ctx, endpt->address, 0);
    (*session)->port = endpt->port;

    /* transport lock */
    (*session)->ti_lock = malloc(sizeof *(*session)->ti_lock);
    if (!(*session)->ti_lock) {
        ERRMEM;
        close(sock);
        msgtype = NC_MSG_ERROR;
        goto fail;
    }
    pthread_mutex_init((*session)->ti_lock, NULL);

    /* sock gets assigned to session or closed */
#ifdef NC_ENABLED_SSH
    if (client->ti == NC_TI_LIBSSH) {
        (*session)->data = client->opts.ssh;
        ret = nc_accept_ssh_session(*session, sock, NC_TRANSPORT_TIMEOUT);
        (*session)->data = NULL;

        if (ret < 0) {
            msgtype = NC_MSG_ERROR;
            goto fail;
        } else if (!ret) {
            msgtype = NC_MSG_WOULDBLOCK;
            goto fail;
        }
    } else
#endif
#ifdef NC_ENABLED_TLS
    if (client->ti == NC_TI_OPENSSL) {
        (*session)->data = client->opts.tls;
        ret = nc_accept_tls_session(*session, sock, NC_TRANSPORT_TIMEOUT);
        (*session)->data = NULL;

        if (ret < 0) {
            msgtype = NC_MSG_ERROR;
            goto fail;
        } else if (!ret) {
            msgtype = NC_MSG_WOULDBLOCK;
            goto fail;
        }
    } else
#endif
    {
        ERRINT;
        close(sock);
        msgtype = NC_MSG_ERROR;
        goto fail;
    }

    /* assign new SID atomically */
    /* LOCK */
    pthread_spin_lock(&server_opts.sid_lock);
    (*session)->id = server_opts.new_session_id++;
    /* UNLOCK */
    pthread_spin_unlock(&server_opts.sid_lock);

    /* NETCONF handshake */
    msgtype = nc_handshake(*session);
    if (msgtype != NC_MSG_HELLO) {
        goto fail;
    }
    (*session)->opts.server.session_start = (*session)->opts.server.last_rpc = time(NULL);
    (*session)->status = NC_STATUS_RUNNING;

    return msgtype;

fail:
    nc_session_free(*session, NULL);
    *session = NULL;
    return msgtype;
}

/* ms */
#define NC_CH_NO_ENDPT_WAIT 1000
#define NC_CH_ENDPT_FAIL_WAIT 1000

struct nc_ch_client_thread_arg {
    char *client_name;
    void (*session_clb)(const char *client_name, struct nc_session *new_session);
};

static struct nc_ch_client *
nc_server_ch_client_with_endpt_lock(const char *name)
{
    struct nc_ch_client *client;

    while (1) {
        /* LOCK */
        client = nc_server_ch_client_lock(name, 0, NULL);
        if (!client) {
            return NULL;
        }
        if (client->ch_endpt_count) {
            return client;
        }
        /* no endpoints defined yet */

        /* UNLOCK */
        nc_server_ch_client_unlock(client);

        usleep(NC_CH_NO_ENDPT_WAIT * 1000);
    }

    return NULL;
}

static int
nc_server_ch_client_thread_session_cond_wait(struct nc_session *session, struct nc_ch_client_thread_arg *data)
{
    int ret;
    uint32_t idle_timeout;
    struct timespec ts;
    struct nc_ch_client *client;

    /* session created, initialize condition */
    session->opts.server.ch_lock = malloc(sizeof *session->opts.server.ch_lock);
    session->opts.server.ch_cond = malloc(sizeof *session->opts.server.ch_cond);
    if (!session->opts.server.ch_lock || !session->opts.server.ch_cond) {
        ERRMEM;
        nc_session_free(session, NULL);
        return -1;
    }
    pthread_mutex_init(session->opts.server.ch_lock, NULL);
    pthread_cond_init(session->opts.server.ch_cond, NULL);

    session->flags |= NC_SESSION_CALLHOME;

    /* CH LOCK */
    pthread_mutex_lock(session->opts.server.ch_lock);

    /* give the session to the user */
    data->session_clb(data->client_name, session);

    do {
        nc_gettimespec(&ts);
        ts.tv_nsec += NC_CH_NO_ENDPT_WAIT * 1000000L;
        if (ts.tv_nsec > 1000000000L) {
            ts.tv_sec += ts.tv_nsec / 1000000000L;
            ts.tv_nsec %= 1000000000L;
        }

        ret = pthread_cond_timedwait(session->opts.server.ch_cond, session->opts.server.ch_lock, &ts);
        if (ret && (ret != ETIMEDOUT)) {
            ERR("Pthread condition timedwait failed (%s).", strerror(ret));
            goto ch_client_remove;
        }

        /* check whether the client was not removed */
        /* LOCK */
        client = nc_server_ch_client_lock(data->client_name, 0, NULL);
        if (!client) {
            /* client was removed, finish thread */
            VRB("Call Home client \"%s\" removed, but an established session will not be terminated.",
                data->client_name);
            goto ch_client_remove;
        }

        if (client->conn_type == NC_CH_PERSIST) {
            /* TODO keep-alives */
            idle_timeout = client->conn.persist.idle_timeout;
        } else {
            idle_timeout = client->conn.period.idle_timeout;
        }

        /* TODO only for sessions without subscriptions */
        if (idle_timeout && (ts.tv_sec >= session->opts.server.last_rpc + idle_timeout)) {
            VRB("Call Home client \"%s\" session %u: session idle timeout elapsed.", client->name, session->id);
            session->status = NC_STATUS_INVALID;
            session->term_reason = NC_SESSION_TERM_TIMEOUT;
        }

        /* UNLOCK */
        nc_server_ch_client_unlock(client);

    } while (session->status == NC_STATUS_RUNNING);

    /* CH UNLOCK */
    pthread_mutex_unlock(session->opts.server.ch_lock);

    return 0;

ch_client_remove:
    /* make the session a standard one */
    pthread_cond_destroy(session->opts.server.ch_cond);
    free(session->opts.server.ch_cond);
    session->opts.server.ch_cond = NULL;

    session->flags &= ~NC_SESSION_CALLHOME;

    /* CH UNLOCK */
    pthread_mutex_unlock(session->opts.server.ch_lock);

    pthread_mutex_destroy(session->opts.server.ch_lock);
    free(session->opts.server.ch_lock);
    session->opts.server.ch_lock = NULL;

    return 1;
}

static void *
nc_ch_client_thread(void *arg)
{
    struct nc_ch_client_thread_arg *data = (struct nc_ch_client_thread_arg *)arg;
    NC_MSG_TYPE msgtype;
    uint8_t cur_attempts = 0;
    uint16_t i;
    char *cur_endpt_name;
    struct nc_ch_endpt *cur_endpt;
    struct nc_session *session;
    struct nc_ch_client *client;

    /* LOCK */
    client = nc_server_ch_client_with_endpt_lock(data->client_name);
    if (!client) {
        goto cleanup;
    }

    cur_endpt = &client->ch_endpts[0];
    cur_endpt_name = strdup(cur_endpt->name);

    VRB("Call Home client \"%s\" connecting...", data->client_name);
    while (1) {
        msgtype = nc_connect_ch_client_endpt(client, cur_endpt, &session);

        if (msgtype == NC_MSG_HELLO) {
            /* UNLOCK */
            nc_server_ch_client_unlock(client);

            VRB("Call Home client \"%s\" session %u established.", data->client_name, session->id);
            if (nc_server_ch_client_thread_session_cond_wait(session, data)) {
                goto cleanup;
            }
            VRB("Call Home client \"%s\" session terminated, reconnecting...", client->name);

            /* LOCK */
            client = nc_server_ch_client_with_endpt_lock(data->client_name);
            if (!client) {
                goto cleanup;
            }

            /* session changed status -> it was disconnected for whatever reason,
             * persistent connection immediately tries to reconnect, periodic waits some first */
            if (client->conn_type == NC_CH_PERIOD) {
                /* UNLOCK */
                nc_server_ch_client_unlock(client);

                /* TODO wake up sometimes to check for new notifications */
                usleep(client->conn.period.reconnect_timeout * 60 * 1000000);

                /* LOCK */
                client = nc_server_ch_client_with_endpt_lock(data->client_name);
                if (!client) {
                    goto cleanup;
                }
            }

            /* set next endpoint to try */
            if (client->start_with == NC_CH_FIRST_LISTED) {
                cur_endpt = &client->ch_endpts[0];
                free(cur_endpt_name);
                cur_endpt_name = strdup(cur_endpt->name);
            } /* else we keep the current one */
        } else {
            /* UNLOCK */
            nc_server_ch_client_unlock(client);

            /* session was not created */
            usleep(NC_CH_ENDPT_FAIL_WAIT * 1000);

            /* LOCK */
            client = nc_server_ch_client_with_endpt_lock(data->client_name);
            if (!client) {
                goto cleanup;
            }

            ++cur_attempts;
            if (cur_attempts == client->max_attempts) {
                for (i = 0; i < client->ch_endpt_count; ++i) {
                    if (!strcmp(client->ch_endpts[i].name, cur_endpt_name)) {
                        break;
                    }
                }
                if (i < client->ch_endpt_count - 1) {
                    /* just go to the next endpoint */
                    cur_endpt = &client->ch_endpts[i + 1];
                    free(cur_endpt_name);
                    cur_endpt_name = strdup(cur_endpt->name);
                } else {
                    /* cur_endpoint was removed or is the last, either way start with the first one */
                    cur_endpt = &client->ch_endpts[0];
                    free(cur_endpt_name);
                    cur_endpt_name = strdup(cur_endpt->name);
                }

                cur_attempts = 0;
            } /* else we keep the current one */
        }
    }

cleanup:
    VRB("Call Home client \"%s\" thread exit.", data->client_name);

    free(data->client_name);
    free(data);
    return NULL;
}

API int
nc_connect_ch_client_dispatch(const char *client_name,
                              void (*session_clb)(const char *client_name, struct nc_session *new_session)) {
    int ret;
    pthread_t tid;
    struct nc_ch_client_thread_arg *arg;

    if (!client_name) {
        ERRARG("client_name");
        return -1;
    } else if (!session_clb) {
        ERRARG("session_clb");
        return -1;
    }

    arg = malloc(sizeof *arg);
    if (!arg) {
        ERRMEM;
        return -1;
    }
    arg->client_name = strdup(client_name);
    if (!arg->client_name) {
        ERRMEM;
        free(arg);
        return -1;
    }
    arg->session_clb = session_clb;

    ret = pthread_create(&tid, NULL, nc_ch_client_thread, arg);
    if (ret) {
        ERR("Creating a new thread failed (%s).", strerror(ret));
        free(arg->client_name);
        free(arg);
        return -1;
    }
    /* the thread now manages arg */

    pthread_detach(tid);

    return 0;
}

#endif /* NC_ENABLED_SSH || NC_ENABLED_TLS */

API time_t
nc_session_get_start_time(const struct nc_session *session)
{
    if (!session || (session->side != NC_SERVER)) {
        ERRARG("session");
        return 0;
    }

    return session->opts.server.session_start;
}
