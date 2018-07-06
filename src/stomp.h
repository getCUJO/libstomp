/*
 * Copyright 2013 Evgeni Dobrev <evgeni_dobrev@developer.bg>
 * Copyright (c) 2015 - 2018, CUJO LLC.
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef STOMP_H
#define STOMP_H

#include <sys/types.h>
#include <libwebsockets.h>

typedef struct _stomp_session stomp_session_t;

struct stomp_hdr {
	const char *key;
	const char *val;
};

struct stomp_ctx_connected {
	size_t hdrc;
	const struct stomp_hdr *hdrs;
};

struct stomp_ctx_receipt {
	size_t hdrc;
	const struct stomp_hdr *hdrs;
};

struct stomp_ctx_error {
	size_t hdrc;
	const struct stomp_hdr *hdrs;
	const void *body;
	size_t body_len;
};
struct stomp_ctx_message {
	size_t hdrc;
	const struct stomp_hdr *hdrs;
	const void *body;
	size_t body_len;
};

enum stomp_prot {
	SPL_10,
	SPL_11,
	SPL_12,
};

enum stomp_cb_type {
	SCB_CONNECTED,
	SCB_ERROR,
	SCB_MESSAGE,
	SCB_RECEIPT,
};

typedef void(*stomp_cb_t)(stomp_session_t *, void *, void *);

void stomp_callback_set(stomp_session_t *, enum stomp_cb_type, stomp_cb_t);

stomp_session_t *stomp_session_new(void *);

void stomp_session_free(stomp_session_t *);

int stomp_disconnect(stomp_session_t *, size_t, const struct stomp_hdr *);

int stomp_subscribe(stomp_session_t *, size_t, const struct stomp_hdr *);

int stomp_unsubscribe(stomp_session_t *, int, size_t, const struct stomp_hdr *);

int stomp_begin(stomp_session_t *, size_t, const struct stomp_hdr *);

int stomp_abort(stomp_session_t *, size_t, const struct stomp_hdr *);

int stomp_ack(stomp_session_t *, size_t, const struct stomp_hdr *);

int stomp_nack(stomp_session_t *, size_t, const struct stomp_hdr *);

int stomp_commit(stomp_session_t *, size_t, const struct stomp_hdr *);

int stomp_send(stomp_session_t *, size_t, const struct stomp_hdr *, const void *,
    size_t);

int stomp_recv_cmd(stomp_session_t *, const unsigned char*, size_t);

int stomp_connect(stomp_session_t *, struct lws *,size_t,
    const struct stomp_hdr *);

int stomp_send_heartbeat(stomp_session_t *);
int stomp_get_broker_hb(stomp_session_t *);
int stomp_get_client_hb(stomp_session_t *);

#endif /* STOMP_H */
