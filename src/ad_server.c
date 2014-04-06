#include <stdbool.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <errno.h>
#include <assert.h>
#include <event2/event.h>
#include <event2/bufferevent.h>
#include <event2/bufferevent_ssl.h>
#include <event2/thread.h>
#include <event2/listener.h>
#include <openssl/ssl.h>
#include <openssl/rand.h>
#include <openssl/conf.h>
#include <openssl/engine.h>
#include <openssl/err.h>
#include "macro.h"
#include "qlibc.h"
#include "ad_server.h"

#ifndef _DOXYGEN_SKIP
/*
 * User callback hook container.
 */
typedef struct ad_conn_s ad_conn_t;
struct ad_hook_s {
    char *method;
    ad_callback cb;
    void *userdata;
};

/*
 * Local functions.
 */
static void libevent_log_cb(int severity, const char *msg);
static int set_undefined_options(ad_server_t *server);
static SSL_CTX *init_ssl(const char *cert_path, const char *pkey_path);
static void listener_cb(struct evconnlistener *listener,
                        evutil_socket_t evsocket, struct sockaddr *sockaddr,
                        int socklen, void *userdata);
static ad_conn_t *conn_new(ad_server_t *server, struct bufferevent *buffer);
static void conn_reset(ad_conn_t *conn);
static void conn_free(ad_conn_t *conn);
static void conn_read_cb(struct bufferevent *buffer, void *userdata) ;
static void conn_write_cb(struct bufferevent *buffer, void *userdata);
static void conn_event_cb(struct bufferevent *buffer, short what, void *userdata);
static void conn_cb(ad_conn_t *conn, int event);
static int call_hooks(short event, ad_conn_t *conn);
static void *set_userdata(ad_conn_t *conn, int index, const void *userdata, ad_userdata_free_cb free_cb);
static void *get_userdata(ad_conn_t *conn, int index);

/*
 * Local variables.
 */
static bool initialized = false;
#endif

/*
 * Global variables.
 */
int _ad_log_level = AD_LOG_WARN;

/**
 * Set debug output level.
 *
 * @param debug_level debug output level. 0 to disable.
 *
 * @return previous debug level.
 *
 * @note
 *  debug_level:
 *    AD_LOG_DISABLE
 *    AD_LOG_ERROR
 *    AD_LOG_WARN (default)
 *    AD_LOG_INFO
 *    AD_LOG_DEBUG
 *    AD_LOG_DEBUG2
 */
enum ad_log_e ad_log_level(enum ad_log_e log_level) {
    int prev = _ad_log_level;
    _ad_log_level = log_level;
    return prev;
}

ad_server_t *ad_server_new(void) {
    if (initialized) {
        initialized = true;
        evthread_use_pthreads();
    }

    ad_server_t *server = NEW_STRUCT(ad_server_t);
    if (server == NULL) {
        return NULL;
    }

    // Initialize instance.
    server->options = qhashtbl(0, 0);
    server->stats = qhashtbl(100, QHASHTBL_THREADSAFE);
    server->hooks = qlist(0);
    if (server->options == NULL || server->stats == NULL || server->hooks == NULL) {
        ad_server_free(server);
        return NULL;
    }

    DEBUG("Created a server object.");
    return server;
}

/**
 * @return 0 if successful, otherwise -1.
 */
int ad_server_start(ad_server_t *server) {
    DEBUG("Starting a server.");

    // Set default options that were not set by user..
    set_undefined_options(server);

    // Hookup libevent's log message.
    if (_ad_log_level >= AD_LOG_DEBUG) {
        event_set_log_callback(libevent_log_cb);
        if (_ad_log_level >= AD_LOG_DEBUG2) {
            event_enable_debug_mode();
        }
    }

    // Parse addr
    int port = ad_server_get_option_int(server, "server.port");
    char *addr = ad_server_get_option(server, "server.addr");
    struct sockaddr *sockaddr = NULL;
    size_t sockaddr_len = 0;
    if (addr[0] == '/') {  // Unix socket.
        struct sockaddr_un unixaddr;
        bzero((void *) &unixaddr, sizeof(struct sockaddr_un));
        if (strlen(addr) >= sizeof(unixaddr.sun_path)) {
            errno = EINVAL;
            DEBUG("Too long unix socket name. '%s'", addr);
            return -1;
        }
        unixaddr.sun_family = AF_UNIX;
        strcpy(unixaddr.sun_path, addr);  // no need of strncpy()
        sockaddr = (struct sockaddr *) &unixaddr;
        sockaddr_len = sizeof(unixaddr);
    } else if (strstr(addr, ":")) {  // IPv6
        struct sockaddr_in6 ipv6addr;
        bzero((void *) &ipv6addr, sizeof(struct sockaddr_in6));
        ipv6addr.sin6_family = AF_INET6;
        ipv6addr.sin6_port = htons(port);
        evutil_inet_pton(AF_INET6, addr, &ipv6addr.sin6_addr);
        sockaddr = (struct sockaddr *) &ipv6addr;
        sockaddr_len = sizeof(ipv6addr);
    } else {  // IPv4
        struct sockaddr_in ipv4addr;
        bzero((void *) &ipv4addr, sizeof(struct sockaddr_in));
        ipv4addr.sin_family = AF_INET;
        ipv4addr.sin_port = htons(port);
        ipv4addr.sin_addr.s_addr =
                (IS_EMPTY_STR(addr)) ? INADDR_ANY : inet_addr(addr);
        sockaddr = (struct sockaddr *) &ipv4addr;
        sockaddr_len = sizeof(ipv4addr);
    }

    // SSL
    if (ad_server_get_option_int(server, "server.enable_ssl")) {
        char *cert_path = ad_server_get_option(server, "server.ssl_cert");
        char *pkey_path = ad_server_get_option(server, "server.ssl_pkey");
        server->sslctx = init_ssl(cert_path, pkey_path);
        if (server->sslctx == NULL) {
            ERROR("Couldn't load certificate file(%s) or private key file(%s).",
                  cert_path, pkey_path);
            return -1;
        }
        DEBUG("SSL Initialized.");
    }

    // Bind
    if (! server->evbase) {
        server->evbase = event_base_new();
        if (! server->evbase) {
            ERROR("Failed to create a new event base.");
            return -1;
        }
    }

    if (! server->listener) {
        server->listener = evconnlistener_new_bind(
                server->evbase, listener_cb, (void *)server,
                LEV_OPT_THREADSAFE | LEV_OPT_REUSEABLE | LEV_OPT_CLOSE_ON_FREE,
                ad_server_get_option_int(server, "server.backlog"),
                sockaddr, sockaddr_len);
        if (! server->listener) {
            ERROR("Failed to bind on %s:%d", addr, port);
            return -1;
        }
    }

    // Listen
    INFO("Listening on %s:%d%s", addr, port, ((server->sslctx) ? " (SSL)" : ""));
    event_base_loop(server->evbase, 0);
    int exitstatus = (event_base_got_break(server->evbase)) ? -1 : 0;

    if (ad_server_get_option_int(server, "server.free_on_stop")) {
        ad_server_free(server);
    }

    return exitstatus;
}

void ad_server_stop(ad_server_t *server) {
    if (server->listener) {
            evconnlistener_free(server->listener);
            server->listener = NULL;
    }

    event_base_loopbreak(server->evbase);
    INFO("Stopping server.");
}

void ad_server_free(ad_server_t *server) {
    if (server) {
        ad_server_stop(server);

        if (server->evbase) {
            event_base_free(server->evbase);
        }

        if (server->sslctx) {
            SSL_CTX_free(server->sslctx);
            ERR_remove_state(0);
            // We don't clean up some of global memory created by
            // OpenSSL library since that may affect on other servers.
            // But this is safe because it's not a memory leak but
            // more of memory in use.
        }

        if (server->options) {
            server->options->free(server->options);
        }
        if (server->stats) {
            server->stats->free(server->stats);
        }
        if (server->hooks) {
            qlist_t *tbl = server->hooks;
            ad_hook_t *hook;
            while ((hook = tbl->popfirst(tbl, NULL))) {
                if (hook->method) free(hook->method);
                free(hook);
            }
            server->hooks->free(server->hooks);
        }
        free(server);
    }
    DEBUG("Released all the resources successfully.");
}

/**
 * Clean up all the global objects.
 *
 * There are globally shared resources in libevent and openssl and
 * it's usually not a problem since they don't grow but having these
 * can confuse some debugging tools into thinking as memory leak.
 * If you need to make sure that libasyncd has released all internal
 * library-global data structures, call this.
 */
void ad_server_global_free(void) {
    // Libevent related.
    //libevent_global_shutdown(); // From libevent v2.1

    // OpenSSL related.
    ENGINE_cleanup();
    CONF_modules_free();
    ERR_free_strings();
    EVP_cleanup();
    CRYPTO_cleanup_all_ex_data();
    sk_SSL_COMP_free(SSL_COMP_get_compression_methods());
}

void ad_server_set_option(ad_server_t *server, const char *key, const char *value) {
    server->options->putstr(server->options, key, value);
}

char *ad_server_get_option(ad_server_t *server, const char *key) {
    return server->options->getstr(server->options, key, false);
}

int ad_server_get_option_int(ad_server_t *server, const char *key) {
    char *value = ad_server_get_option(server, key);
    return (value) ? atoi(value) : 0;
}

/**
 * Get OpenSSL SSL_CTX object.
 *
 * @return SSL_CTX object, NULL if not enabled.
 *
 * @note
 * Libasyncd initializes SSL_CTX with minimum default with
 * "SSLv23_server_method" which will make the server understand
 * SSLv2, SSLv3, and TLSv1 protocol. Use returned SSL_CTX object
 * to set any custom options before starting the server.
 *
 * @code
 *  SSL_CTX *sslctx = ad_server_get_ssl_ctx(server);
 *  SSL_CTX_set_options(sslctx, SSL_OP_NO_SSLv2);
 *  SSL_CTX_set_session_cache_mode(sslctx, SSL_SESS_CACHE_SERVER);
 *  ad_server_start(server);
 */
SSL_CTX *ad_server_get_ssl_ctx(ad_server_t *server) {
    return server->sslctx;
}

qhashtbl_t *ad_server_get_stats(ad_server_t *server, const char *key) {
    return server->stats;
}

void ad_server_register_hook(ad_server_t *server, ad_callback cb, void *userdata) {
    ad_server_register_hook_on_method(server, NULL, cb, userdata);
}

void ad_server_register_hook_on_method(ad_server_t *server, const char *method, ad_callback cb, void *userdata) {
    ad_hook_t hook;
    bzero((void *)&hook, sizeof(ad_hook_t));
    hook.method = (method) ? strdup(method) : NULL;
    hook.cb = cb;
    hook.userdata = userdata;

    server->hooks->addlast(server->hooks, (void *)&hook, sizeof(ad_hook_t));
}

/**
 * Attach userdata into this connection.
 *
 * @return previous userdata;
 */
void *ad_conn_set_userdata(ad_conn_t *conn, const void *userdata, ad_userdata_free_cb free_cb) {
    return set_userdata(conn, 0, userdata, free_cb);
}

void *ad_conn_get_userdata(ad_conn_t *conn) {
    return get_userdata(conn, 0);
}

void *ad_conn_set_extra(ad_conn_t *conn, const void *extra, ad_userdata_free_cb free_cb) {
    return set_userdata(conn, 1, extra, free_cb);
}

void *ad_conn_get_extra(ad_conn_t *conn) {
    return get_userdata(conn, 1);
}

char *ad_conn_set_method(ad_conn_t *conn, char *method) {
    char *prev = conn->method;
    if (conn->method) {
        free(conn->method);
    }
    conn->method = strdup(method);
    return prev;
}

/******************************************************************************
 * Private internal functions.
 *****************************************************************************/
static void libevent_log_cb(int severity, const char *msg) {
    switch(severity) {
        case _EVENT_LOG_MSG : {
            INFO("%s", msg);
            break;
        }
        case _EVENT_LOG_WARN : {
            WARN("%s", msg);
            break;
        }
        case _EVENT_LOG_ERR : {
            ERROR("%s", msg);
            break;
        }
        default : {
            DEBUG("%s", msg);
            break;
        }
    }
}

// Set default options that were not set by user..
static int set_undefined_options(ad_server_t *server) {
    int newentries = 0;
    char *default_options[][2] = AD_SERVER_OPTIONS;
    for (int i = 0; ! IS_EMPTY_STR(default_options[i][0]); i++) {
        if (! ad_server_get_option(server, default_options[i][0])) {
            ad_server_set_option(server, default_options[i][0], default_options[i][1]);
            newentries++;
        }
        DEBUG("%s=%s", default_options[i][0], ad_server_get_option(server, default_options[i][0]));
    }
    return newentries;
}

static SSL_CTX *init_ssl(const char *cert_path, const char *pkey_path) {
    // Initialize OpenSSL library.
    SSL_load_error_strings();
    SSL_library_init();
    RAND_poll();

    SSL_CTX *sslctx = SSL_CTX_new(SSLv23_server_method());
    if (! SSL_CTX_use_certificate_file(sslctx, cert_path, SSL_FILETYPE_PEM) ||
        ! SSL_CTX_use_PrivateKey_file(sslctx, pkey_path, SSL_FILETYPE_PEM)) {
        return NULL;
    }

    return sslctx;
}

static void listener_cb(struct evconnlistener *listener, evutil_socket_t socket,
                        struct sockaddr *sockaddr, int socklen, void *userdata) {
    DEBUG("New connection.");
    ad_server_t *server = (ad_server_t *)userdata;

    // Create a new buffer.
    struct bufferevent *buffer = NULL;
    if (server->sslctx) {
        buffer = bufferevent_openssl_socket_new(server->evbase, socket,
                                                SSL_new(server->sslctx),
                                                BUFFEREVENT_SSL_ACCEPTING,
                                                BEV_OPT_CLOSE_ON_FREE);
    } else {
        buffer = bufferevent_socket_new(server->evbase, socket, BEV_OPT_CLOSE_ON_FREE);
    }
    if (buffer == NULL) goto error;

    // Set read timeout.
    int timeout = ad_server_get_option_int(server, "server.timeout");
    if (timeout > 0) {
        struct timeval tm;
        bzero((void *)&tm, sizeof(struct timeval));
        tm.tv_sec = timeout;
        bufferevent_set_timeouts(buffer, &tm, NULL);
    }

    // Create a connection.
    void *conn = conn_new(server, buffer);
    if (! conn) goto error;

    return;

  error:
    if (buffer) bufferevent_free(buffer);
    ERROR("Failed to create a connection handler.");
    event_base_loopbreak(server->evbase);
    server->errcode = ENOMEM;
}

static ad_conn_t *conn_new(ad_server_t *server, struct bufferevent *buffer) {
    if (server == NULL || buffer == NULL) {
        return NULL;
    }

    // Create a new connection container.
    ad_conn_t *conn = NEW_STRUCT(ad_conn_t);
    if (conn == NULL) return NULL;

    // Initialize with default values.
    conn->server = server;
    conn->buffer = buffer;
    conn->in = bufferevent_get_input(buffer);
    conn->out = bufferevent_get_output(buffer);
    conn_reset(conn);

    // Bind callback
    bufferevent_setcb(buffer, conn_read_cb, conn_write_cb, conn_event_cb, (void *)conn);
    bufferevent_setwatermark(buffer, EV_WRITE, 0, 0);
    bufferevent_enable(buffer, EV_WRITE);
    bufferevent_enable(buffer, EV_READ);

    // Run callbacks with AD_EVENT_INIT event.
    conn->status = call_hooks(AD_EVENT_INIT, conn);

    return conn;
}

static void conn_reset(ad_conn_t *conn) {
    conn->status = AD_OK;

    for(int i = 0; i < AD_NUM_USERDATA; i++) {
        if (conn->userdata[i]) {
            if (conn->userdata_free_cb[i] != NULL) {
                conn->userdata_free_cb[i](conn, conn->userdata[i]);
            } else {
                WARN("Found unreleased userdata.");
            }
            conn->userdata[i] = NULL;
        }
    }

    if (conn->method) {
        free(conn->method);
        conn->method = NULL;
    }
}

static void conn_free(ad_conn_t *conn) {
    if (conn) {
        if (conn->status != AD_CLOSE) {
            call_hooks(AD_EVENT_CLOSE | AD_EVENT_SHUTDOWN , conn);
        }
        conn_reset(conn);
        if (conn->buffer) {
            if (conn->server->sslctx) {
                int sslerr = bufferevent_get_openssl_error(conn->buffer);
                if (sslerr) {
                    char errmsg[256];
                    ERR_error_string_n(sslerr, errmsg, sizeof(errmsg));
                    ERROR("SSL %s (err:%d)", errmsg, sslerr);
                }
            }
            bufferevent_free(conn->buffer);
        }
        free(conn);
    }
}

#define DRAIN_EVBUFFER(b) evbuffer_drain(b, evbuffer_get_length(b))
static void conn_read_cb(struct bufferevent *buffer, void *userdata) {
    DEBUG("read_cb");
    ad_conn_t *conn = userdata;
    conn_cb(conn, AD_EVENT_READ);
}

static void conn_write_cb(struct bufferevent *buffer, void *userdata) {
    DEBUG("write_cb");
    ad_conn_t *conn = userdata;
    conn_cb(conn, AD_EVENT_WRITE);
}

static void conn_event_cb(struct bufferevent *buffer, short what, void *userdata) {
    DEBUG("event_cb 0x%x", what);
    ad_conn_t *conn = userdata;

    if (what & BEV_EVENT_EOF || what & BEV_EVENT_ERROR || what & BEV_EVENT_TIMEOUT) {
        conn->status = AD_CLOSE;
        conn_cb(conn, AD_EVENT_CLOSE | ((what & BEV_EVENT_TIMEOUT) ? AD_EVENT_TIMEOUT : 0));
    }
}

static void conn_cb(ad_conn_t *conn, int event) {
    DEBUG("conn: status:0x%x, event:0x%x", conn->status, event)
    if(conn->status == AD_OK || conn->status == AD_TAKEOVER) {
        conn->status = call_hooks(event, conn);
    }

    if(conn->status == AD_DONE) {
        if (ad_server_get_option_int(conn->server, "server.request_pipelining")) {
            call_hooks(AD_EVENT_CLOSE , conn);
            conn_reset(conn);
            call_hooks(AD_EVENT_INIT , conn);
        } else {
            // Do nothing but drain input buffer.
            if (event == AD_EVENT_READ) {
                DEBUG("Draining in-buffer. %d", conn->status);
                DRAIN_EVBUFFER(conn->in);
            }
        }
        return;
    } else if(conn->status == AD_CLOSE) {
        if (evbuffer_get_length(conn->out) <= 0) {
            int newevent = (event & AD_EVENT_CLOSE) ? event : AD_EVENT_CLOSE;
            call_hooks(newevent, conn);
            conn_free(conn);
            DEBUG("Connection closed.");
            return;
        }
    }
}

static int call_hooks(short event, ad_conn_t *conn) {
    qlist_t *hooks = conn->server->hooks;

    qdlobj_t obj;
    bzero((void *)&obj, sizeof(qdlobj_t));
    while (hooks->getnext(hooks, &obj, false) == true) {
        ad_hook_t *hook = (ad_hook_t *)obj.data;
        if (hook->cb) {
            if (hook->method && conn->method && strcmp(hook->method, conn->method)) {
                continue;
            }

            int status = hook->cb(event, conn, hook->userdata);
            if (status != AD_OK) {
                return status;
            }
        }
    }
    return AD_OK;
}

static void *set_userdata(ad_conn_t *conn, int index, const void *userdata, ad_userdata_free_cb free_cb) {
    void *prev = conn->userdata;
    conn->userdata[index] = (void *)userdata;
    conn->userdata_free_cb[index] = free_cb;
    return prev;
}

static void *get_userdata(ad_conn_t *conn, int index) {
    return conn->userdata[index];
}

