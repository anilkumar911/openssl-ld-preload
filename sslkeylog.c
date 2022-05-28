/*
 * Dumps master keys for OpenSSL clients to file. The format is documented at
 * https://developer.mozilla.org/en-US/docs/Mozilla/Projects/NSS/Key_Log_Format
 * Supports TLS 1.3 when used with OpenSSL 1.1.1.
 *
 * Copyright (C) 2014 Peter Wu <peter@lekensteyn.nl>
 * Licensed under the terms of GPLv3 (or any later version) at your choice.
 *
 * Usage:
 *  cc sslkeylog.c -shared -o libsslkeylog.so -fPIC -ldl
 *  SSLKEYLOGFILE=premaster.txt LD_PRELOAD=./libsslkeylog.so openssl ...
 *
 * Usage for macOS:
 *  cc sslkeylog.c -shared -o libsslkeylog.dylib -fPIC -ldl \
 *      -I/usr/local/opt/openssl@1.1/include \
 *      -L/usr/local/opt/openssl@1.1/lib -lssl
 *  DYLD_INSERT_LIBRARIES=./libsslkeylog.dylib DYLD_FORCE_FLAT_NAMESPACE=1 \
 *      SSLKEYLOGFILE=premaster.txt /usr/local/opt/openssl@1.1/bin/openssl ...
 */

/*
 * A single libsslkeylog.so supports multiple OpenSSL runtime versions. If you
 * would like to build this library without OpenSSL development headers and do
 * not require support for older OpenSSL versions, then disable it by defining
 * the NO_OPENSSL_102_SUPPORT or NO_OPENSSL_110_SUPPORT macros.
 */
/* Define to drop OpenSSL <= 1.0.2 support and require OpenSSL >= 1.1.0. */
//#define NO_OPENSSL_102_SUPPORT
/* Define to drop OpenSSL <= 1.1.0 support and require OpenSSL >= 1.1.1. */
//#define NO_OPENSSL_110_SUPPORT

/* No OpenSSL 1.1.0 support implies no OpenSSL 1.0.2 support. */
#ifdef NO_OPENSSL_110_SUPPORT
#   define NO_OPENSSL_102_SUPPORT
#endif

#define _GNU_SOURCE /* for RTLD_NEXT */
#include <dlfcn.h>
#ifndef NO_OPENSSL_102_SUPPORT
#   include <openssl/ssl.h>
#endif /* ! NO_OPENSSL_102_SUPPORT */
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "T4MkmsMetadata.h"
#include "T4SslSession.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#ifndef OPENSSL_SONAME
/* fallback library if OpenSSL is not already loaded. */
#  ifdef __APPLE__
/* libssl.dylib is a symlink, Homebrew installs:
 * OpenSSL 1.0.2 /usr/local/opt/openssl/lib/libssl.1.0.0.dylib
 * OpenSSL 1.1.1 /usr/local/opt/openssl@1.1/lib/libssl.1.1.dylib
 */
#   define OPENSSL_SONAME   "libssl.dylib"
#  else
/* Other values to try: libssl.so.0.9.8 libssl.so.1.0.0 libssl.so.1.1 */
#   define OPENSSL_SONAME   "libssl.so"
# endif
#endif

#define FIRSTLINE   "# SSL key logfile generated by sslkeylog.c\n"
#define FIRSTLINE_LEN (sizeof(FIRSTLINE) - 1)

/* When building for OpenSSL 1.1.0 or newer, no headers are required. */
#ifdef NO_OPENSSL_102_SUPPORT
typedef struct ssl_st SSL;
typedef struct ssl_ctx_st SSL_CTX;
/* Extra definitions for OpenSSL 1.1.0 support when headers are unavailable. */
#   ifndef NO_OPENSSL_110_SUPPORT
typedef struct ssl_session_st SSL_SESSION;
#       define SSL3_RANDOM_SIZE 32
#       define SSL_MAX_MASTER_KEY_LENGTH 48
#       define OPENSSL_VERSION_NUMBER 0x10100000L
#   endif /* ! NO_OPENSSL_110_SUPPORT */
#endif /* ! NO_OPENSSL_102_SUPPORT */

static int keylog_file_fd = -1;

/* Legacy routines for dumping TLS <= 1.2 secrets on older OpenSSL versions. */
#ifndef NO_OPENSSL_110_SUPPORT
#define PREFIX      "CLIENT_RANDOM "
#define PREFIX_LEN  (sizeof(PREFIX) - 1)

 void apps_ssl_info_callback(const SSL *s, int where, int ret)
{
        const char *str;
        int w;

        w=where& ~SSL_ST_MASK;

        if (w & SSL_ST_CONNECT) str="SSL_connect";
        else if (w & SSL_ST_ACCEPT) str="SSL_accept";
        else str="undefined";
        if( SSL_get_state(s) == TLS_ST_CW_CLNT_HELLO) {
          printf("##### %s: Terminating handshake\n",__func__);
          //s->statem.state = MSG_FLOW_ERROR;
          //s->statem.in_init = 1;
        }

        if (where & SSL_CB_LOOP)
                {
                printf("##### %s:%s\n",str,SSL_state_string_long(s));
                }
        else if (where & SSL_CB_ALERT)
                {
                str=(where & SSL_CB_READ)?"read":"write";
                printf("##### SSL3 alert %s:%s:%s\n",
                        str,
                        SSL_alert_type_string_long(ret),
                        SSL_alert_desc_string_long(ret));
                }
        else if (where & SSL_CB_EXIT)
                {
                if (ret == 0)
                        printf("##### %s:failed in %s\n",
                                str,SSL_state_string_long(s));
                else if (ret < 0)
                        {
                        printf("##### %s:error in %s\n",
                                str,SSL_state_string_long(s));
                        }
                }
}

static inline void put_hex(char *buffer, int pos, char c)
{
    unsigned char c1 = ((unsigned char) c) >> 4;
    unsigned char c2 = c & 0xF;
    buffer[pos] = c1 < 10 ? '0' + c1 : 'A' + c1 - 10;
    buffer[pos+1] = c2 < 10 ? '0' + c2 : 'A' + c2 - 10;
}

static void dump_to_fd(int fd, unsigned char *client_random,
        unsigned char *master_key, int master_key_length)
{
    int pos, i;
    char line[PREFIX_LEN + 2 * SSL3_RANDOM_SIZE + 1 +
              2 * SSL_MAX_MASTER_KEY_LENGTH + 1];

    memcpy(line, PREFIX, PREFIX_LEN);
    pos = PREFIX_LEN;
    /* Client Random for SSLv3/TLS */
    for (i = 0; i < SSL3_RANDOM_SIZE; i++) {
        put_hex(line, pos, client_random[i]);
        pos += 2;
    }
    line[pos++] = ' ';
    /* Master Secret (size is at most SSL_MAX_MASTER_KEY_LENGTH) */
    for (i = 0; i < master_key_length; i++) {
        put_hex(line, pos, master_key[i]);
        pos += 2;
    }
    line[pos++] = '\n';
    /* Write at once rather than using buffered I/O. Perhaps there is concurrent
     * write access so do not write hex values one by one. */
    write(fd, line, pos);
}
#endif /* ! NO_OPENSSL_110_SUPPORT */

static void init_keylog_file(void)
{
    if (keylog_file_fd >= 0)
        return;

    const char *filename = getenv("SSLKEYLOGFILE");
    if (filename) {
        keylog_file_fd = open(filename, O_WRONLY | O_APPEND | O_CREAT, 0644);
        if (keylog_file_fd >= 0 && lseek(keylog_file_fd, 0, SEEK_END) == 0) {
            /* file is opened successfully and there is no data (pos == 0) */
            write(keylog_file_fd, FIRSTLINE, FIRSTLINE_LEN);
        }
    }
}

static inline void *try_lookup_symbol(const char *sym, int optional)
{
    void *func = dlsym(RTLD_NEXT, sym);
    if (!func && optional && dlsym(RTLD_NEXT, "SSL_new")) {
        /* Symbol not found, but an old OpenSSL version was actually loaded. */
        return NULL;
    }
    /* Symbol not found, OpenSSL is not loaded (linked) so try to load it
     * manually. This is error-prone as it depends on a fixed library name.
     * Perhaps it should be an env name? */
    if (!func) {
        void *handle = dlopen(OPENSSL_SONAME, RTLD_LAZY);
        if (!handle) {
            fprintf(stderr, "Lookup error for %s: %s\n", sym, dlerror());
            abort();
        }
        func = dlsym(handle, sym);
        if (!func && !optional) {
            fprintf(stderr, "Cannot lookup %s\n", sym);
            abort();
        }
        dlclose(handle);
    }
    return func;
}

static inline void *lookup_symbol(const char *sym)
{
    return try_lookup_symbol(sym, 0);
}

#ifndef NO_OPENSSL_110_SUPPORT
typedef struct ssl_tap_state {
    int master_key_length;
    unsigned char master_key[SSL_MAX_MASTER_KEY_LENGTH];

} ssl_tap_state_t;

static inline SSL_SESSION *ssl_get_session(const SSL *ssl)
{
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
    static SSL_SESSION *(*func)();
    if (!func) {
        func = lookup_symbol("SSL_get_session");
    }
    return func(ssl);
#else
    return ssl->session;
#endif
}

static void copy_master_secret(const SSL_SESSION *session,
        unsigned char *master_key_out, int *keylen_out)
{
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
    static size_t (*func)();
    if (!func) {
        func = lookup_symbol("SSL_SESSION_get_master_key");
    }
    *keylen_out = func(session, master_key_out, SSL_MAX_MASTER_KEY_LENGTH);
#else
    if (session->master_key_length > 0) {
        *keylen_out = session->master_key_length;
        memcpy(master_key_out, session->master_key,
                session->master_key_length);
    }
#endif
}

static void copy_client_random(const SSL *ssl, unsigned char *client_random)
{
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
    static size_t (*func)();
    if (!func) {
        func = lookup_symbol("SSL_get_client_random");
    }
    /* ssl->s3 is not checked in openssl 1.1.0-pre6, but let's assume that
     * we have a valid SSL context if we have a non-NULL session. */
    func(ssl, client_random, SSL3_RANDOM_SIZE);
#else
    if (ssl->s3) {
        memcpy(client_random, ssl->s3->client_random, SSL3_RANDOM_SIZE);
    }
#endif
}

/* non-NULL if the new OpenSSL 1.1.1 keylog API is supported. */
static int supports_keylog_api(void)
{
/*
    static int supported = -1;
    if (supported == -1) {
        supported = try_lookup_symbol("SSL_CTX_set_keylog_callback", 1) != NULL;
    }
    return supported;
*/
	return 0;
}

/* Copies SSL state for later comparison in tap_ssl_key. */
static void ssl_tap_state_init(ssl_tap_state_t *state, const SSL *ssl)
{
    if (supports_keylog_api()) {
        /* Favor using the callbacks API to extract secrets. */
        return;
    }

    const SSL_SESSION *session = ssl_get_session(ssl);

    memset(state, 0, sizeof(ssl_tap_state_t));
    if (session) {
        copy_master_secret(session, state->master_key, &state->master_key_length);
    }
}

#define SSL_TAP_STATE(state, ssl) \
    ssl_tap_state_t state; \
    ssl_tap_state_init(&state, ssl)

static void tap_ssl_key(const SSL *ssl, ssl_tap_state_t *state)
{
    if (supports_keylog_api()) {
        /* Favor using the callbacks API to extract secrets. */
        return;
    }

    const SSL_SESSION *session = ssl_get_session(ssl);
    unsigned char client_random[SSL3_RANDOM_SIZE];
    unsigned char master_key[SSL_MAX_MASTER_KEY_LENGTH];
    int master_key_length = 0;

    if (session) {
        copy_master_secret(session, master_key, &master_key_length);
        /* Assume we have a client random if the master key is set. */
        if (master_key_length > 0) {
            copy_client_random(ssl, client_random);
        }
    }

    /* Write the logfile when the master key is available for SSLv3/TLSv1. */
    if (master_key_length > 0) {
        /* Skip writing keys if it did not change. */
        if (state->master_key_length == master_key_length &&
            memcmp(state->master_key, master_key, master_key_length) == 0) {
            return;
        }

        init_keylog_file();
        if (keylog_file_fd >= 0) {
            dump_to_fd(keylog_file_fd, client_random, master_key,
                    master_key_length);
        }
    }
}

int SSL_connect(SSL *ssl)
{
		printf("---> %s called\n", __func__);
    static int (*func)();
    if (!func) {
        func = lookup_symbol(__func__);
    }
    SSL_TAP_STATE(state, ssl);
    int ret = func(ssl);
    tap_ssl_key(ssl, &state);
    return ret;
}

int SSL_do_handshake(SSL *ssl)
{
		printf("---> %s called\n", __func__);
    static int (*func)();
    if (!func) {
        func = lookup_symbol(__func__);
    }
    SSL_TAP_STATE(state, ssl);
    int ret = func(ssl);
    tap_ssl_key(ssl, &state);
    return ret;
}

int SSL_accept(SSL *ssl)
{
		printf("---> %s called\n", __func__);
    static int (*func)();
    if (!func) {
        func = lookup_symbol(__func__);
    }
    SSL_TAP_STATE(state, ssl);
    int ret = func(ssl);
    tap_ssl_key(ssl, &state);
    return ret;
}

int SSL_read(SSL *ssl, void *buf, int num)
{
		printf("---> %s called\n", __func__);
    static int (*func)();
    if (!func) {
        func = lookup_symbol(__func__);
    }
    SSL_TAP_STATE(state, ssl);
    int ret = func(ssl, buf, num);
    tap_ssl_key(ssl, &state);
    return ret;
}

int SSL_write(SSL *ssl, const void *buf, int num)
{
		printf("---> %s called\n", __func__);
    static int (*func)();
    if (!func) {
        func = lookup_symbol(__func__);
    }
    SSL_TAP_STATE(state, ssl);
    int ret = func(ssl, buf, num);
    tap_ssl_key(ssl, &state);
    return ret;
}
#endif /* ! NO_OPENSSL_110_SUPPORT */

/* Key extraction via the new OpenSSL 1.1.1 API. */
static void keylog_callback(const SSL *ssl, const char *line)
{
    init_keylog_file();
    if (keylog_file_fd >= 0) {
        write(keylog_file_fd, line, strlen(line));
        write(keylog_file_fd, "\n", 1);
    }
}

struct MkmsMetaData* getMkmsMetaDataUsingSSL(const SSL *ssl, char **key, char **tls_connection_type);
int get_t4_server_sd(const SSL *s);
SSL_SESSION *create_a_psk(SSL *ssl, const char *psk_id, const char *psk_key);

int getPskIdAndValueFrmMkms( char **pskId,
                             char **pskValue,
                             char *serveIp,
                             int serverPort,
                             struct MkmsMetaData *mkms_metadata);


static int psk_use_session_cb(SSL *s, const EVP_MD *md, 
                              const unsigned char **id, size_t *idlen,
                              SSL_SESSION **sess)
{
  /*
  */
  printf("@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n");

    struct sockaddr_in addr1;
    socklen_t serv_len = sizeof(addr1);
    printf("RAJNISH: use_session_cb: value of server_sd is: %d\n", get_t4_server_sd(s));
    getpeername(get_t4_server_sd(s), (struct sockaddr *)&addr1, &serv_len);
    char *serverIp = inet_ntoa(addr1.sin_addr);
    int serverPort = ntohs(addr1.sin_port);
    if (strcmp(serverIp, "0.0.0.0") == 0)
        serverIp = "127.0.0.1";
    printf("RAJNISH: use_session_cb: peer(server) ip: %s and port: %d\n", serverIp, serverPort);

      char *pskId = "";
    char *pskValue = "";

	char *key = calloc(64, sizeof(char));
  char *tls_connection_type = malloc(16);
  struct MkmsMetaData* mkms_metadata =
  	getMkmsMetaDataUsingSSL(s, &key, &tls_connection_type);

  if (mkms_metadata == NULL || mkms_metadata->clntCert == NULL || !strlen(mkms_metadata->clntCert))
	{
    printf("%s:Terminating Handshake !!!!!!\n", __func__);
		return 0;
	}

	// et PSK.
  if (getPskIdAndValueFrmMkms(&pskId, &pskValue, serverIp, serverPort, mkms_metadata)){
    printf("%s:failed to get pskid and psk value from mkms ip: %s and port: %d\n", __func__, serverIp, serverPort);
  }

  *sess = create_a_psk(s, (const char *)pskId, (const char *)pskValue);
  *id = (const unsigned char *)pskId;
  *idlen = strlen(pskId);

	return 1;
}

SSL *SSL_new(SSL_CTX *ctx)
{
		printf("---> %s called\n", __func__);
    static SSL *(*func)();
    static void (*set_keylog_cb)();
    if (!func) {
        func = lookup_symbol(__func__);
#ifdef NO_OPENSSL_110_SUPPORT
        /* The new API MUST be available since OpenSSL 1.1.1. */
        set_keylog_cb = lookup_symbol("SSL_CTX_set_keylog_callback");
#else  /* ! NO_OPENSSL_110_SUPPORT */
        /* May be NULL if used with an older OpenSSL runtime library. */
        set_keylog_cb = try_lookup_symbol("SSL_CTX_set_keylog_callback", 1);
#endif /* ! NO_OPENSSL_110_SUPPORT */
    }
    if (set_keylog_cb) {
        /* Override any previous key log callback. */
        set_keylog_cb(ctx, keylog_callback);
    }
    SSL *s = func(ctx);

    SSL_set_min_proto_version(s, TLS1_3_VERSION);
    SSL_set_max_proto_version(s, TLS1_3_VERSION);

    SSL_set_info_callback(s, apps_ssl_info_callback);
    SSL_set_ciphersuites(s, "TLS_AES_256_GCM_SHA384");
		SSL_set_psk_find_session_callback(s, &t4_find_session_cb);
		SSL_set_psk_use_session_callback(s, psk_use_session_cb);
    return s;
}
