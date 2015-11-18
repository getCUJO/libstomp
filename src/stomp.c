/*
 * Copyright 2013 Evgeni Dobrev <evgeni_dobrev@developer.bg>
 * Copyright (c) 2015, CUJO LLC.
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

#include <sys/types.h>
#include <sys/socket.h>

#include <errno.h>
#include <limits.h>
#include <netdb.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libwebsockets.h>

#include "frame.h"
#include "stomp.h"

/* enough space for ULLONG_MAX as string */
#define ULL_STR_LEN 25

/* max number of broker heartbeat timeouts */
#define MAX_BROKER_TIMEOUTS 5

struct stomp_callbacks {
	void (*connected)(stomp_session_t *, void *, void *);
	void (*message)(stomp_session_t *, void *, void *);
	void (*error)(stomp_session_t *, void *, void *);
	void (*receipt)(stomp_session_t *, void *, void *);
	void (*user)(stomp_session_t *, void *, void *);
};

struct _stomp_session {
	struct stomp_callbacks	 callbacks;	/* event callbacks */
	void			*ctx;		/* ptr to session context */
	frame_t			*frame_out;	/* library -> broker */
	frame_t			*frame_in;	/* broker -> library */
	enum stomp_prot		 protocol;
	struct libwebsocket	*broker_fd;	/* pointer to a WS instance */
	int			 client_id;	/* unique ids for subscribe */
	unsigned long		 client_hb;	/* client heartbeat [ms] */
	unsigned long		 broker_hb;	/* broker heartbeat [ms] */
	struct timespec		 last_write;
	struct timespec		 last_read;
	int			 broker_timeouts;
	int			 run;
};

static int parse_version(const char *, enum stomp_prot *);
static int parse_heartbeat(const char *, unsigned long *, unsigned long *);
static void on_connected(stomp_session_t *);
static void on_receipt(stomp_session_t *);
static void on_error(stomp_session_t *);
static void on_message(stomp_session_t *);
static const char *hdr_get(size_t, const struct stomp_hdr *, const char *);

stomp_session_t *
stomp_session_new(void *session_ctx)
{
	stomp_session_t *s;

	if ((s = calloc(1, sizeof(*s))) == NULL)
		return (NULL);

	s->ctx = session_ctx;
	s->broker_fd = NULL;

	if ((s->frame_out = frame_new()) == NULL)
		free(s);

	if ((s->frame_in = frame_new()) == NULL) {
		free(s->frame_out);
		free(s);
	}

	return (s);
}

void
stomp_session_free(stomp_session_t *s)
{
	frame_free(s->frame_out);
	frame_free(s->frame_in);
	free(s);
}

void
stomp_callback_set(stomp_session_t *s, enum stomp_cb_type type, stomp_cb_t cb)
{
	if (s == NULL)
		return;

	switch (type) {
	case SCB_CONNECTED:
		s->callbacks.connected = cb;
		break;
	case SCB_ERROR:
		s->callbacks.error = cb;
		break;
	case SCB_MESSAGE:
		s->callbacks.message = cb;
		break;
	case SCB_RECEIPT:
		s->callbacks.receipt = cb;
		break;
	case SCB_USER:
		s->callbacks.user = cb;
		break;
	default:
		return;
	}
}

int
stomp_connect(stomp_session_t *s, struct libwebsocket* wsi, size_t hdrc,
    const struct stomp_hdr *hdrs)
{

	unsigned long	 x = 0, y = 0;
	const char	*hb;

	hb = hdr_get(hdrc, hdrs, "heart-beat");

	/*
	 * Heart-beat is optional, so hb may be NULL without problem.
	 */
	if (hb != NULL && parse_heartbeat(hb, &x, &y)) {
		errno = EINVAL;
		return (-1);
	}

	s->broker_fd = wsi;
	s->run = 1;

	frame_reset(s->frame_out);

	if (frame_cmd_set(s->frame_out, "CONNECT"))
		return (-1);

	s->client_hb = x;
	s->broker_hb = y;

	if (frame_hdrs_add(s->frame_out, hdrc, hdrs))
		return (-1);

	if (frame_write(wsi, s->frame_out) < 0) {
		s->run = 0;
		return (-1);
	}

	clock_gettime(CLOCK_MONOTONIC, &s->last_write);

	return (0);
}

int
stomp_disconnect(stomp_session_t *s, size_t hdrc, const struct stomp_hdr *hdrs)
{
	frame_reset(s->frame_out);

	if (frame_cmd_set(s->frame_out, "DISCONNECT") ||
	    frame_hdrs_add(s->frame_out, hdrc, hdrs))
		return (-1);

	if (frame_write(s->broker_fd, s->frame_out) < 0) {
		s->run = 0;
		return (-1);
	}

	clock_gettime(CLOCK_MONOTONIC, &s->last_write);

	return (0);
}

/* TODO enforce different client-ids in case they are provided with hdrs */
int
stomp_subscribe(stomp_session_t *s, size_t hdrc, const struct stomp_hdr *hdrs)
{
	int		 client_id = 0;
	char		 buf[ULL_STR_LEN];
	const char	*ack;

	if (hdr_get(hdrc, hdrs, "destination") == NULL) {
		errno = EINVAL;
		return (-1);
	}

	ack = hdr_get(hdrc, hdrs, "ack");
	if (ack != NULL && strcmp(ack, "auto") != 0 &&
	    strcmp(ack, "client") != 0 &&
	    strcmp(ack, "client-individual") != 0) {
		errno = EINVAL;
		return (-1);
	}

	frame_reset(s->frame_out);

	if (frame_cmd_set(s->frame_out, "SUBSCRIBE"))
		return (-1);

	if (hdr_get(hdrc, hdrs, "id") == NULL) {
		client_id = s->client_id;
		if (client_id == INT_MAX)
			client_id = 0;
		client_id++;
		snprintf(buf, ULL_STR_LEN, "%d", client_id);
		if (frame_hdr_add(s->frame_out, "id", buf))
			return (-1);
	}

	if (ack == NULL && frame_hdr_add(s->frame_out, "ack", "auto") ||
	    frame_hdrs_add(s->frame_out, hdrc, hdrs))
		return (-1);

	if (frame_write(s->broker_fd, s->frame_out) < 0) {
		s->run = 0;
		return (-1);
	}

	clock_gettime(CLOCK_MONOTONIC, &s->last_write);
	s->client_id = client_id;

	return (client_id);
}

int
stomp_unsubscribe(stomp_session_t *s, int client_id, size_t hdrc,
    const struct stomp_hdr *hdrs)
{
	char		 buf[ULL_STR_LEN];
	const char	*id, *destination;

	id = hdr_get(hdrc, hdrs, "id");
	destination = hdr_get(hdrc, hdrs, "destination");

	if (s->protocol == SPL_10) {
		if (destination == NULL && id == NULL && client_id == 0) {
			errno = EINVAL;
			return (-1);
		}
	} else if (id == NULL && client_id == 0) {
			errno = EINVAL;
			return (-1);
	}

	frame_reset(s->frame_out);

	if (frame_cmd_set(s->frame_out, "UNSUBSCRIBE"))
		return (-1);

	/* user provided client id. overrride all other supplied headers */
	if (client_id) {
		snprintf(buf, ULL_STR_LEN, "%lu", (unsigned long)client_id);
		if (frame_hdr_add(s->frame_out, "id", buf))
			return (-1);
	}

	if (frame_hdrs_add(s->frame_out, hdrc, hdrs))
		return (-1);

	if (frame_write(s->broker_fd, s->frame_out) < 0) {
		s->run = 0;
		return (-1);
	}

	clock_gettime(CLOCK_MONOTONIC, &s->last_write);

	return (0);
}

/* TODO enforce different tx_ids */
int
stomp_begin(stomp_session_t *s, size_t hdrc, const struct stomp_hdr *hdrs)
{
	if (hdr_get(hdrc, hdrs, "transaction") == NULL) {
		errno = EINVAL;
		return (-1);
	}

	frame_reset(s->frame_out);

	if (frame_cmd_set(s->frame_out, "BEGIN") ||
	    frame_hdrs_add(s->frame_out, hdrc, hdrs))
		return (-1);

	if (frame_write(s->broker_fd, s->frame_out) < 0) {
		s->run = 0;
		return (-1);
	}

	clock_gettime(CLOCK_MONOTONIC, &s->last_write);

	return (0);
}

int
stomp_abort(stomp_session_t *s, size_t hdrc, const struct stomp_hdr *hdrs)
{
	if (hdr_get(hdrc, hdrs, "transaction") == NULL) {
		errno = EINVAL;
		return (-1);
	}

	frame_reset(s->frame_out);

	if (frame_cmd_set(s->frame_out, "ABORT") ||
	    frame_hdrs_add(s->frame_out, hdrc, hdrs))
		return (-1);

	if (frame_write(s->broker_fd, s->frame_out) < 0) {
		s->run = 0;
		return (-1);
	}

	clock_gettime(CLOCK_MONOTONIC, &s->last_write);

	return (0);
}

int
stomp_ack(stomp_session_t *s, size_t hdrc, const struct stomp_hdr *hdrs)
{
	switch(s->protocol) {
	case SPL_12:
		if (hdr_get(hdrc, hdrs, "id") == NULL) {
			errno = EINVAL;
			return (-1);
		}
		break;
	case SPL_11:
		if (hdr_get(hdrc, hdrs, "message-id") == NULL ||
		    hdr_get(hdrc, hdrs, "subscription") == NULL) {
			errno = EINVAL;
			return (-1);
		}
		if (hdr_get(hdrc, hdrs, "subscription") == NULL) {
			errno = EINVAL;
			return (-1);
		}
		break;
	default: /* SPL_10 */
		if (hdr_get(hdrc, hdrs, "message-id") == NULL) {
			errno = EINVAL;
			return (-1);
		}
	}

	frame_reset(s->frame_out);

	if (frame_cmd_set(s->frame_out, "ACK") ||
	    frame_hdrs_add(s->frame_out, hdrc, hdrs))
		return (-1);

	if (frame_write(s->broker_fd, s->frame_out) < 0) {
		s->run = 0;
		return (-1);
	}

	clock_gettime(CLOCK_MONOTONIC, &s->last_write);

	return (0);
}

int
stomp_nack(stomp_session_t *s, size_t hdrc, const struct stomp_hdr *hdrs)
{
	switch(s->protocol) {
	case SPL_12:
		if (hdr_get(hdrc, hdrs, "id") == NULL) {
			errno = EINVAL;
			return (-1);
		}
		break;
	case SPL_11:
		if (hdr_get(hdrc, hdrs, "message-id") == NULL ||
		    hdr_get(hdrc, hdrs, "subscription") == NULL) {
			errno = EINVAL;
			return (-1);
		}
		break;
	default: /* SPL_10 */
		errno = EINVAL;
		return (-1);
	}

	frame_reset(s->frame_out);

	if (frame_cmd_set(s->frame_out, "NACK") ||
	    frame_hdrs_add(s->frame_out, hdrc, hdrs))
		return (-1);

	if (frame_write(s->broker_fd, s->frame_out) < 0) {
		s->run = 0;
		return (-1);
	}

	clock_gettime(CLOCK_MONOTONIC, &s->last_write);

	return (0);
}

int
stomp_commit(stomp_session_t *s, size_t hdrc, const struct stomp_hdr *hdrs)
{
	if (hdr_get(hdrc, hdrs, "transaction") == NULL) {
		errno = EINVAL;
		return (-1);
	}

	frame_reset(s->frame_out);

	if (frame_cmd_set(s->frame_out, "COMMIT") ||
	    frame_hdrs_add(s->frame_out, hdrc, hdrs))
		return (-1);

	if (frame_write(s->broker_fd, s->frame_out) < 0) {
		s->run = 0;
		return (-1);
	}

	clock_gettime(CLOCK_MONOTONIC, &s->last_write);

	return (0);
}

int
stomp_send(stomp_session_t *s, size_t hdrc, const struct stomp_hdr *hdrs,
    void *body, size_t body_len)
{
	char		 buf[ULL_STR_LEN];
	const char	*len;

	if (hdr_get(hdrc, hdrs, "destination") == NULL) {
		errno = EINVAL;
		return (-1);
	}

	frame_reset(s->frame_out);

	if (frame_cmd_set(s->frame_out, "SEND"))
		return (-1);

	/* frames SHOULD include a content-length */
	len = hdr_get(hdrc, hdrs, "content-length");
	if (len == 0) {
		snprintf(buf, ULL_STR_LEN, "%lu", (unsigned long)body_len);
		if (frame_hdr_add(s->frame_out, "content-length", buf))
			return (-1);
	}

	if (frame_hdrs_add(s->frame_out, hdrc, hdrs) ||
	    frame_body_set(s->frame_out, body, body_len))
		return (-1);

	if (frame_write(s->broker_fd, s->frame_out) < 0) {
		s->run = 0;
		return (-1);
	}

	clock_gettime(CLOCK_MONOTONIC, &s->last_write);

	return (0);
}

int
stomp_recv_cmd(stomp_session_t *s, const unsigned char* buf, size_t len)
{
	frame_t		*f = s->frame_in;
	size_t		 cmd_len;
	int		 err;
	const char	*cmd;

	frame_reset(f);

	if (err = frame_read(buf, len, f))
		return (-1);

	clock_gettime(CLOCK_MONOTONIC, &s->last_read);
	s->broker_timeouts = 0;

	/* heart-beat */
	if ((cmd_len = frame_cmd_get(f, &cmd)) == 0)
		return (0);

	if (strncmp(cmd, "CONNECTED", cmd_len) == 0)
		on_connected(s);
	else if (strncmp(cmd, "ERROR", cmd_len) == 0)
		on_error(s);
	else if (strncmp(cmd, "RECEIPT", cmd_len) == 0)
		on_receipt(s);
	else if (strncmp(cmd, "MESSAGE", cmd_len) == 0)
		on_message(s);
	else
		return (-1);

	return (0);
}

int
stomp_handle_heartbeat(stomp_session_t *s)
{
	struct timespec	 now;
	unsigned long	 elapsed;
	unsigned char	*buf;

	if (s->callbacks.user != NULL)
		s->callbacks.user(s, NULL, s->ctx);

	if (s->client_hb != 0 || s->broker_hb != 0)
		clock_gettime(CLOCK_MONOTONIC, &now);

	if (s->broker_hb != 0) {
		elapsed = (now.tv_sec - s->last_read.tv_sec) * 1000 +
		    (now.tv_nsec - s->last_read.tv_nsec) / 1000000;
		if (elapsed > s->broker_hb) {
			memcpy(&s->last_read, &now, sizeof(s->last_read));
			s->broker_timeouts++;
		}
		/* XXX assert inside a library? */
		assert(s->broker_timeouts <= MAX_BROKER_TIMEOUTS);
	}

	if (s->client_hb != 0) {
		elapsed = (now.tv_sec - s->last_write.tv_sec) * 1000 + \
		    (now.tv_nsec - s->last_write.tv_nsec) / 1000000;
		if (elapsed > s->client_hb) {
			memcpy(&s->last_write, &now, sizeof(s->last_write));
			if ((buf = calloc(1, LWS_SEND_BUFFER_PRE_PADDING + 1 +
			    LWS_SEND_BUFFER_POST_PADDING + 1)) == NULL)
				return (-1);
			buf[LWS_SEND_BUFFER_PRE_PADDING] = '\n';
			/* XXX assert inside a library? */
			assert(libwebsocket_write(s->broker_fd,
			    &buf[LWS_SEND_BUFFER_PRE_PADDING], 1,
			    LWS_WRITE_TEXT) != -1);
			free(buf);
		}
	}

	return (0);
}

static int
parse_version(const char *s, enum stomp_prot *v)
{
	enum stomp_prot tmp_v;

	if (s == NULL) {
		errno = EINVAL;
		return (-1);
	}

	if (strncmp(s, "1.2", 3) == 0)
		tmp_v = SPL_12;
	else if (strncmp(s, "1.1", 3) == 0)
		tmp_v = SPL_11;
	else if (strncmp(s, "1.0", 3) == 0)
		tmp_v = SPL_10;
	else
		tmp_v = SPL_10;

	*v = tmp_v;

	return (0);
}

static int
parse_heartbeat(const char *s, unsigned long *x, unsigned long *y)
{
	unsigned long	 tmp_x, tmp_y;
	char		*endptr;
	const char	*nptr = s;

	if (s == NULL)
		goto error;

	errno = 0;
	tmp_x = strtoul(nptr, &endptr, 10);
	if (errno != 0)
		goto error;
	if (tmp_x == ULONG_MAX)
		goto error;
	if (endptr == nptr)
		goto error;
	if (*endptr != ',')
		goto error;

	nptr = endptr;
	nptr++;

	errno = 0;
	tmp_y = strtoul(nptr, &endptr, 10);
	if (errno != 0)
		goto error;
	if (tmp_y == ULONG_MAX)
		goto error;
	if (endptr == nptr)
		goto error;

	*x = tmp_x;
	*y = tmp_y;

	return (0);

error:
	errno = EINVAL;
	return (-1);
}

static void
on_connected(stomp_session_t *s)
{
	const struct stomp_hdr		*hdrs;
	struct stomp_ctx_connected	 e;
	frame_t				*f = s->frame_in;
	unsigned long			 x, y;
	size_t				 hdrc;
	enum stomp_prot			 v;
	const char			*h;

	hdrc = frame_hdrs_get(f, &hdrs);
	h = hdr_get(hdrc, hdrs, "version");
	if (h != NULL && !parse_version(h, &v))
		s->protocol = v;

	h = hdr_get(hdrc, hdrs, "heart-beat");
	if (h != NULL && !parse_heartbeat(h, &x, &y)) {
		if (s->client_hb == 0 || y == 0)
			s->client_hb = 0;
		else
			s->client_hb = s->client_hb > y ? s->client_hb : y;

		if (s->broker_hb == 0 || x == 0)
			s->broker_hb = 0;
		else
			s->broker_hb = s->broker_hb > x ? s->broker_hb : x;
	} else {
		s->client_hb = 0;
		s->broker_hb = 0;
	}

	if (s->callbacks.connected == NULL)
		return;

	e.hdrc = hdrc;
	e.hdrs = hdrs;

	s->callbacks.connected(s, &e, s->ctx);
}

static void
on_receipt(stomp_session_t *s)
{
	struct stomp_ctx_receipt	 e;
	frame_t				*f = s->frame_in;

	if (s->callbacks.receipt == NULL)
		return;

	e.hdrc = frame_hdrs_get(f, &e.hdrs);

	s->callbacks.receipt(s, &e, s->ctx);
}

static void
on_error(stomp_session_t *s)
{
	struct stomp_ctx_error	 e;
	frame_t			*f = s->frame_in;

	if (s->callbacks.error == NULL)
		return;

	e.hdrc = frame_hdrs_get(f, &e.hdrs);
	e.body_len = frame_body_get(f, &e.body);

	s->callbacks.error(s, &e, s->ctx);
}

static void
on_message(stomp_session_t *s)
{
	struct stomp_ctx_message	e;
	frame_t				*f = s->frame_in;

	if (s->callbacks.message == NULL)
		return;

	e.hdrc = frame_hdrs_get(f, &e.hdrs);
	e.body_len = frame_body_get(f, &e.body);

	s->callbacks.message(s, &e, s->ctx);
}

static const char *
hdr_get(size_t count, const struct stomp_hdr *hdrs, const char *key)
{
	const struct stomp_hdr	*h;
	size_t			 i;

	for (i = 0; i < count; i++) {
		h = &hdrs[i];
		if (strcmp(key, h->key) == 0)
			return (h->val);
	}

	return (NULL);
}
