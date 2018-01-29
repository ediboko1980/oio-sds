/*
OpenIO SDS metautils
Copyright (C) 2014 Worldline, as part of Redcurrant
Copyright (C) 2015-2017 OpenIO SAS, as part of OpenIO SDS

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 3.0 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library.
*/

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <sys/types.h>

#include "metautils.h"

#include <metautils/lib/common_variables.h>

#ifndef URL_MAXLEN
# define URL_MAXLEN STRLEN_ADDRINFO
#endif

#ifndef EVENT_BUFFER_SIZE
# define EVENT_BUFFER_SIZE 2048
#endif

enum client_step_e
{
	NONE = 0,
	CONNECTING,
	REQ_SENDING,
	REP_READING_SIZE,
	REP_READING_DATA,
	STATUS_OK,
	STATUS_FAILED
};

struct gridd_client_factory_s
{
	struct abstract_client_factory_s abstract;
};

struct gridd_client_s
{
	struct abstract_client_s abstract;
	GByteArray *request;
	GByteArray *reply;
	GError *error;

	gpointer ctx;
	client_on_reply on_reply;

	gint64 tv_connect; /* timestamp of the last connection */
	gint64 tv_start; /* timestamp of the last startup */

	gint64 delay_connect; /* max delay for a connection to be established */
	gint64 delay_single; /* max delay for a single request, without redirection */
	gint64 delay_overall; /* max delay with all possible redirections */

	guint32 size;

	guint nb_redirects;
	int fd;
	guint sent_bytes;

	enum client_step_e step : 4;
	guint8 keepalive : 1;
	guint8 forbid_redirect : 1;
	guint8 avoidance_onoff : 1;

	gchar orig_url[URL_MAXLEN];
	gchar url[URL_MAXLEN];
};

/* Cache of network errors ------------------------------------------------- */

static GMutex lock_errors;
static GTree *tree_errors = NULL;  /* <gchar*> -> <rrd*> */

static GRWLock lock_down;
static GTree *tree_down = NULL;  /* <gchar*> -> <constant> */

void  _oio_cache_of_errors_contructor (void);

void __attribute__ ((constructor))
_oio_cache_of_errors_contructor (void)
{
	static volatile guint lazy_init = 1;
	if (lazy_init) {
		if (g_atomic_int_compare_and_exchange(&lazy_init, 1, 0)) {
			g_mutex_init (&lock_errors);
			tree_errors = g_tree_new_full(metautils_strcmp3, NULL,
					g_free, (GDestroyNotify) grid_single_rrd_destroy);
			g_rw_lock_init(&lock_down);
			tree_down = g_tree_new_full(metautils_strcmp3, NULL, g_free, NULL);
		}
	}
}

static gboolean
_is_peer_down (const char *url)
{
	g_rw_lock_reader_lock(&lock_down);
	gpointer p = g_tree_lookup(tree_down, url);
	g_rw_lock_reader_unlock(&lock_down);

	return p != NULL;
}

static gboolean
_has_too_many_errors (const char *url)
{
	guint64 delta = 0;
	g_mutex_lock(&lock_errors);

	struct grid_single_rrd_s *rrd = g_tree_lookup(tree_errors, url);
	if (rrd) {
		const gint64 now = oio_ext_monotonic_seconds();
		delta = grid_single_rrd_get_delta(rrd, now, oio_client_cache_errors_period);
	}
	g_mutex_unlock(&lock_errors);

	const gboolean rc = (oio_client_cache_errors_max <= delta);
	if (rc) {
		GRID_DEBUG("[%s] seems DOWN (%"G_GUINT64_FORMAT" fails in %" G_GINT64_FORMAT "s)",
				url, delta, oio_client_cache_errors_period);
	}
	return rc;
}

static void
_count_network_error (const char *url, const GError *err)
{
	if (!url || !*url || !err)
		return;
	if (!CODE_IS_NETWORK_ERROR(err->code) || err->code == CODE_AVOIDED)
		return;

	/* all the network errors are considered, with the notable exception on
	 * CODE_AVOIDED that is precisely generated by the current feature.
	 * Using it to detect a failed service would be false (we don't know,
	 * in facts) and it would self-feed the feature. */

	g_mutex_lock(&lock_errors);
	struct grid_single_rrd_s *rrd = g_tree_lookup(tree_errors, url);
	if (!rrd) {
		rrd = grid_single_rrd_create(oio_ext_monotonic_seconds(),
				oio_client_cache_errors_period + 1);
		g_tree_replace(tree_errors, g_strdup(url), rrd);
	}
	const gint64 now = oio_ext_monotonic_seconds();
	grid_single_rrd_add(rrd, now, 1);
	g_mutex_unlock(&lock_errors);
}

/* ------------------------------------------------------------------------- */

static void _client_free(struct gridd_client_s *client);
static GError* _client_connect_url(struct gridd_client_s *client, const gchar *url);
static GError* _client_request(struct gridd_client_s *client, GByteArray *req,
		gpointer ctx, client_on_reply cb);
static gboolean _client_expired(struct gridd_client_s *client, gint64 now);
static gboolean _client_finished(struct gridd_client_s *c);
static const gchar* _client_url(struct gridd_client_s *client);
static int _client_get_fd(struct gridd_client_s *client);
static int _client_interest(struct gridd_client_s *client);
static GError* _client_error(struct gridd_client_s *client);
static gboolean _client_start(struct gridd_client_s *client);
static GError* _client_set_fd(struct gridd_client_s *client, int fd);
static void _client_set_timeout(struct gridd_client_s *client, gdouble seconds);
static void _client_set_timeout_cnx(struct gridd_client_s *client, gdouble sec);
static void _client_react(struct gridd_client_s *client);
static gboolean _client_expire(struct gridd_client_s *client, gint64 now);
static void _client_fail(struct gridd_client_s *client, GError *why);

static void _factory_clean(struct gridd_client_factory_s *self);
static struct gridd_client_s * _factory_create_client (
		struct gridd_client_factory_s *self);

struct gridd_client_factory_vtable_s VTABLE_FACTORY =
{
	_factory_clean,
	_factory_create_client
};

struct gridd_client_vtable_s VTABLE_CLIENT =
{
	_client_free,
	_client_connect_url,
	_client_request,
	_client_error,
	_client_interest,
	_client_url,
	_client_get_fd,
	_client_set_fd,
	_client_set_timeout,
	_client_set_timeout_cnx,
	_client_expired,
	_client_finished,
	_client_start,
	_client_react,
	_client_expire,
	_client_fail
};

static void
_client_reset_reply(struct gridd_client_s *client)
{
	client->size = 0;
	if (!client->reply)
		client->reply = g_byte_array_new();
	else if (client->reply->len > 0)
		g_byte_array_set_size(client->reply, 0);
}

/* Benefit from the maybe-present TCP_FASTOPEN, and try to send a few bytes
 * alongside with the initiation sequence.
 */
static GError*
_client_connect(struct gridd_client_s *client)
{
	GError *err = NULL;
	gsize sent = client->request ? client->request->len : 0;
	client->fd = sock_connect_and_send(client->url, &err,
			sent ? client->request->data : NULL, &sent);
	if (client->fd < 0) {
		EXTRA_ASSERT(err != NULL);
		g_prefix_error(&err, "Connect error: ");
		return err;
	}

	EXTRA_ASSERT(err == NULL);
	client->tv_connect = oio_ext_monotonic_time ();
	client->sent_bytes = sent;
	if (client->sent_bytes >= client->request->len) {
		_client_reset_reply(client);
		client->step = REP_READING_SIZE;
	} else {
		client->step = CONNECTING;
	}

	return NULL;
}

static void
_client_reset_request(struct gridd_client_s *client)
{
	if (client->request)
		g_byte_array_unref(client->request);
	client->request = NULL;
	client->ctx = NULL;
	client->on_reply = NULL;
	client->sent_bytes = 0;
	client->nb_redirects = 0;
}

static void
_client_reset_cnx(struct gridd_client_s *client)
{
	if (client->fd >= 0)
		metautils_pclose(&(client->fd));
	client->step = NONE;
}

static void
_client_reset_target(struct gridd_client_s *client)
{
	memset(client->url, 0, sizeof(client->url));
	memset(client->orig_url, 0, sizeof(client->orig_url));
}

static void
_client_replace_error(struct gridd_client_s *client, GError *e)
{
	if (client->error)
		g_clear_error(&(client->error));
	client->error = e;
}

static GError *
_client_manage_reply(struct gridd_client_s *client, MESSAGE reply)
{
	GError *err = NULL;
	guint status = 0;
	gchar *message = NULL;

	if (NULL != (err = metaXClient_reply_simple(reply, &status, &message))) {
		g_prefix_error (&err, "reply: ");
		return err;
	}
	STRING_STACKIFY(message);

	if (CODE_IS_NETWORK_ERROR(status)) {
		err = NEWERROR(status, "net error: %s", message);
		metautils_pclose(&(client->fd));
		client->step = STATUS_FAILED;
		return err;
	}

	if (status == CODE_TEMPORARY) {
		_client_reset_reply(client);
		client->step = REP_READING_SIZE;
		return NULL;
	}

	if (CODE_IS_OK(status)) {
		client->step = (status==CODE_FINAL_OK) ? STATUS_OK : REP_READING_SIZE;
		if (client->step == STATUS_OK) {
			if (!client->keepalive)
				metautils_pclose(&(client->fd));
		} else {
			_client_reset_reply(client);
		}
		if (client->on_reply) {
			if (!client->on_reply(client->ctx, reply))
				return SYSERR("Handler error");
		}
		return NULL;
	}

	if (status == CODE_REDIRECT && !client->forbid_redirect) {
		/* Reset the context */
		_client_reset_reply(client);
		_client_reset_cnx(client);
		client->sent_bytes = 0;

		++ client->nb_redirects;

		/* Allow 4 redirections (i.e. 5 attemps) */
		if (client->nb_redirects > 4)
			return NEWERROR(CODE_TOOMANY_REDIRECT, "Too many redirections");

		/* The first redirection is legit, but subsequent tell us there is
		 * something happening with the election. Let the services get a
		 * final status and wait for it a little bit.
		 * We will wait 200, 400, 800 ms. */
		if (client->nb_redirects > 1) {
			guint backoff = client->nb_redirects - 1;
			gulong delay = (1<<backoff) * 100 * G_TIME_SPAN_MILLISECOND;
			g_usleep(delay);
		}

		/* Replace the URL */
		g_strlcpy(client->url, message, URL_MAXLEN);
		if (NULL != (err = _client_connect(client)))
			g_prefix_error(&err, "Redirection error: Connect error: ");
		return err;
	}

	if (!client->keepalive)
		_client_reset_cnx(client);
	_client_reset_reply(client);

	return NEWERROR(status, "%s", message);
}

static GError *
_client_manage_reply_data(struct gridd_client_s *c)
{
	GError *err = NULL;
	MESSAGE r = message_unmarshall(c->reply->data, c->reply->len, &err);
	if (!r)
		g_prefix_error(&err, "Decoding: ");
	else
		err = _client_manage_reply(c, r);
	metautils_message_destroy(r);
	return err;
}

static GError *
_client_manage_event_in_buffer(struct gridd_client_s *client, guint8 *d, gsize ds)
{
	guint32 s32;

	switch (client->step) {

		case CONNECTING:
			EXTRA_ASSERT(client->fd >= 0);
			_client_reset_reply(client);
			/* Do not reset client->sent_bytes, as we may have sent some bytes
			 * along with the connection packets (thanks to TCP_FASTOPEN). */
			client->step = REQ_SENDING;
			return NULL;

		case REQ_SENDING:

			EXTRA_ASSERT(client->step == REQ_SENDING);
			_client_reset_reply(client);
			if (!client->request)
				return NULL;
			else {
				/* Continue to send the request */
				ssize_t rc = metautils_syscall_send(client->fd,
						client->request->data + client->sent_bytes,
						client->request->len - client->sent_bytes,
						MSG_NOSIGNAL);

				if (rc < 0)
					return (errno == EINTR || errno == EAGAIN) ? NULL :
						NEWERROR(CODE_NETWORK_ERROR,
								"ERROR while requesting %s: (%d) %s",
								_client_url(client), errno, strerror(errno));
				if (rc > 0)
					client->sent_bytes += rc;

				if (client->sent_bytes < client->request->len)
					return NULL;
			}

			client->step = REP_READING_SIZE;
			// FALLTHROUGH

		case REP_READING_SIZE:

			EXTRA_ASSERT(client->step == REP_READING_SIZE);

			if (!client->reply)
				client->reply = g_byte_array_sized_new(256);

			if (client->reply->len < 4) {
				/* Continue reading the size */
				ssize_t rc = metautils_syscall_read(
						client->fd, d, (4 - client->reply->len));
				if (rc == 0)
					return NEWERROR(CODE_NETWORK_ERROR,
							"EOF while reading response size from %s",
							_client_url(client));
				if (rc < 0)
					return (errno == EINTR || errno == EAGAIN) ? NULL :
						NEWERROR(CODE_NETWORK_ERROR,
								"ERROR while reading response size from %s:"
								" (%d) %s", _client_url(client),
								errno, strerror(errno));

				EXTRA_ASSERT(rc > 0);
				g_byte_array_append(client->reply, d, rc);

				if (client->reply->len < 4)  /* size still incomplete */
					return NULL;
			}

			EXTRA_ASSERT (client->reply->len >= 4);
			s32 = *((guint32*)(client->reply->data));
			client->size = g_ntohl(s32);
			client->step = REP_READING_DATA;
			// FALLTHROUGH
		case REP_READING_DATA:

			EXTRA_ASSERT(client->step == REP_READING_DATA);
			EXTRA_ASSERT (client->reply->len <= client->size + 4);

			/* If the reply is not complete, try to consume some bytes */
			if (client->reply->len < client->size + 4) {
				gsize remaining = client->size + 4 - client->reply->len;
				gsize dmax = ds;
				if (dmax > remaining)
					dmax = remaining;
				ssize_t rc = metautils_syscall_read(client->fd, d, dmax);
				if (rc == 0)
					return NEWERROR(CODE_NETWORK_ERROR,
							"EOF while reading response from %s "
							"(got %u bytes, expected %"G_GUINT32_FORMAT")",
							_client_url(client), client->reply->len,
							client->size + 4);
				if (rc < 0)
					return (errno == EINTR || errno == EAGAIN) ? NULL :
						NEWERROR(CODE_NETWORK_ERROR,
								"ERROR while reading response from %s: (%d) %s",
								_client_url(client), errno, strerror(errno));

				EXTRA_ASSERT(rc > 0);
				g_byte_array_append(client->reply, d, rc);
			}

			EXTRA_ASSERT (client->reply->len <= client->size + 4);

			if (client->reply->len < client->size + 4) {
				/* the reply is not complete yet */
				return NULL;
			} else {
				/* Ok, the reply is complete, ready to be parsed */
				GError *err = _client_manage_reply_data(client);
				if (err) {
					client->step = STATUS_FAILED;
					return err;
				} else {
					/* `step` has been changed by _client_manage_reply_data() */
					return NULL;
				}
			}

		default:
			g_assert_not_reached();
			return NEWERROR(0, "Invalid state");
	}

	g_assert_not_reached();
	return NEWERROR(0, "BUG unreachable code");
}

static GError *
_client_manage_event(struct gridd_client_s *client)
{
	guint8 d[EVENT_BUFFER_SIZE];
	return _client_manage_event_in_buffer(client, d, EVENT_BUFFER_SIZE);
}

/* ------------------------------------------------------------------------- */

static void
_client_react(struct gridd_client_s *client)
{
	EXTRA_ASSERT(client != NULL);
	EXTRA_ASSERT(client->abstract.vtable == &VTABLE_CLIENT);

	if (!client)
		return;
	GError *err = NULL;

retry:
	if (!(err = _client_manage_event(client))) {
		if (client->step == REP_READING_SIZE && client->reply
				&& client->reply->len >= 4)
				goto retry;
	} else {
		_client_reset_request(client);
		_client_reset_reply(client);
		_client_reset_cnx(client);
		_client_replace_error(client, err);
		client->step = STATUS_FAILED;
	}
}

static const gchar*
_client_url(struct gridd_client_s *client)
{
	EXTRA_ASSERT(client != NULL);
	EXTRA_ASSERT(client->abstract.vtable == &VTABLE_CLIENT);

	if (!client)
		return NULL;
	else {
		client->url[ sizeof(client->url)-1 ] = '\0';
		return client->url;
	}
}

static int
_client_get_fd(struct gridd_client_s *client)
{
	EXTRA_ASSERT(client != NULL);
	EXTRA_ASSERT(client->abstract.vtable == &VTABLE_CLIENT);

	return client ? client->fd : -1;
}

static int
_client_interest(struct gridd_client_s *client)
{
	EXTRA_ASSERT(client != NULL);
	EXTRA_ASSERT(client->abstract.vtable == &VTABLE_CLIENT);

	if (!client)
		return 0;

	switch (client->step) {
		case NONE:
			return 0;
		case CONNECTING:
			return CLIENT_WR;
		case REQ_SENDING:
			return client->request != NULL ?  CLIENT_WR : 0;
		case REP_READING_SIZE:
			return CLIENT_RD;
		case REP_READING_DATA:
			return CLIENT_RD;
		case STATUS_OK:
		case STATUS_FAILED:
			return 0;
		default:
			g_assert_not_reached();
			return 0;
	}
}

static GError *
_client_error(struct gridd_client_s *client)
{
	EXTRA_ASSERT(client != NULL);
	EXTRA_ASSERT(client->abstract.vtable == &VTABLE_CLIENT);

	if (!client || !client->error)
		return NULL;
	return NEWERROR(client->error->code, "%s", client->error->message);
}

static void
_client_free(struct gridd_client_s *client)
{
	EXTRA_ASSERT(client != NULL);
	EXTRA_ASSERT(client->abstract.vtable == &VTABLE_CLIENT);

	if (oio_client_cache_errors)
		_count_network_error(client->url, client->error);

	_client_reset_reply(client);
	_client_reset_request(client);
	_client_reset_cnx(client);
	_client_reset_target(client);
	_client_replace_error(client, NULL);
	if (client->reply)
		g_byte_array_free(client->reply, TRUE);
	client->fd = -1;
	SLICE_FREE (struct gridd_client_s, client);
}

static void
_client_set_timeout(struct gridd_client_s *client, gdouble seconds)
{
	EXTRA_ASSERT(client != NULL);
	EXTRA_ASSERT(client->abstract.vtable == &VTABLE_CLIENT);

	client->delay_single = seconds * (gdouble) G_TIME_SPAN_SECOND;
	client->delay_overall = seconds * (gdouble) G_TIME_SPAN_SECOND;
}

static void
_client_set_timeout_cnx(struct gridd_client_s *client, gdouble seconds)
{
	EXTRA_ASSERT(client != NULL);
	EXTRA_ASSERT(client->abstract.vtable == &VTABLE_CLIENT);

	client->delay_connect = seconds * (gdouble) G_TIME_SPAN_SECOND;
}

static GError*
_client_set_fd(struct gridd_client_s *client, int fd)
{
	EXTRA_ASSERT(client != NULL);
	EXTRA_ASSERT(client->abstract.vtable == &VTABLE_CLIENT);

	if (fd >= 0) {
		switch (client->step) {
			case NONE: /* ok */
				break;
			case CONNECTING:
				if (client->request != NULL)
					return NEWERROR(CODE_INTERNAL_ERROR, "Request pending");
				break;
			case REQ_SENDING:
			case REP_READING_SIZE:
			case REP_READING_DATA:
				return NEWERROR(CODE_INTERNAL_ERROR, "Request pending");
			case STATUS_OK:
			case STATUS_FAILED:
				/* ok */
				break;
		}
	}

	/* reset any connection and request */
	_client_reset_reply(client);
	_client_reset_request(client);
	_client_reset_target(client);

	/* XXX do not call _client_reset_cnx(), or close the connexion.
	 * It is the responsibility of the caller to manage this, because it
	 * explicitely breaks the pending socket management. */
	client->fd = fd;

	client->step = (client->fd >= 0) ? CONNECTING : NONE;

	return NULL;
}

static GError*
_client_connect_url(struct gridd_client_s *client, const gchar *url)
{
	EXTRA_ASSERT(client != NULL);
	EXTRA_ASSERT(client->abstract.vtable == &VTABLE_CLIENT);

	if (NULL == url || !url[0])
		return NEWERROR(CODE_INTERNAL_ERROR, "Bad address");

	if (*url != '/' && !metautils_url_valid_for_connect(url))
		return NEWERROR(CODE_BAD_REQUEST, "Bad address [%s]", url);

	EXTRA_ASSERT(client != NULL);

	_client_reset_cnx(client);
	_client_reset_target(client);
	_client_reset_reply(client);

	g_strlcpy(client->orig_url, url, URL_MAXLEN);
	g_strlcpy(client->url, url, URL_MAXLEN);
	client->step = NONE;
	return NULL;
}

static GError*
_client_request(struct gridd_client_s *client, GByteArray *req,
		gpointer ctx, client_on_reply cb)
{
	EXTRA_ASSERT(client != NULL);
	EXTRA_ASSERT(client->abstract.vtable == &VTABLE_CLIENT);

	if ( NULL == req)
		return NEWERROR(CODE_INTERNAL_ERROR, "Invalid parameter");

	switch (client->step) {
		case NONE:
		case CONNECTING:
			if (client->request != NULL)
				return NEWERROR(CODE_INTERNAL_ERROR, "Request already pending");
			/* ok */
			break;
		case REQ_SENDING:
		case REP_READING_SIZE:
		case REP_READING_DATA:
			return NEWERROR(CODE_INTERNAL_ERROR, "Request not terminated");
		case STATUS_OK:
		case STATUS_FAILED:
			/* ok */
			if (client->fd >= 0)
				client->step = REQ_SENDING;
			else
				client->step = CONNECTING;
			break;
	}

	/* if any, reset the last reply */
	_client_reset_reply(client);
	_client_reset_request(client);
	_client_replace_error(client, NULL);

	/* Now set the new request components */
	client->ctx = ctx;
	client->on_reply = cb;
	client->request = g_byte_array_ref(req);
	return NULL;
}

static gboolean
_client_expired(struct gridd_client_s *client, gint64 now)
{
	EXTRA_ASSERT(client != NULL);
	EXTRA_ASSERT(client->abstract.vtable == &VTABLE_CLIENT);
	switch (client->step) {
		case NONE:
			return FALSE;
		case CONNECTING:
			if (client->delay_connect > 0) {
				if ((now - client->tv_connect) > client->delay_connect)
					return TRUE;
			}
			if (client->delay_overall > 0) {
				if ((now - client->tv_start) > client->delay_overall)
					return TRUE;
			}
			return FALSE;
		case REQ_SENDING:
		case REP_READING_SIZE:
		case REP_READING_DATA:
			if (client->delay_single > 0) {
				if ((now - client->tv_connect) > client->delay_single)
					return TRUE;
			}
			if (client->delay_overall > 0) {
				if ((now - client->tv_start) > client->delay_overall)
					return TRUE;
			}
			return FALSE;
		case STATUS_OK:
		case STATUS_FAILED:
			return FALSE;
	}

	g_assert_not_reached();
	return FALSE;
}

static void
_client_react_timeout(struct gridd_client_s *client)
{
	_client_reset_cnx(client);
	_client_replace_error(client, NEWERROR(client->step == CONNECTING
			? ERRCODE_CONN_TIMEOUT : ERRCODE_READ_TIMEOUT, "Timeout"));
	client->step = STATUS_FAILED;
}

static gboolean
_client_expire(struct gridd_client_s *client, gint64 now)
{
	EXTRA_ASSERT(client != NULL);
	EXTRA_ASSERT(client->abstract.vtable == &VTABLE_CLIENT);

	if (_client_finished(client))
		return FALSE;

	if (!_client_expired(client, now)) {
#ifdef HAVE_ENBUG
		if (oio_client_fake_timeout_threshold >= oio_ext_rand_int_range(1,100)) {
			_client_react_timeout(client);
			return TRUE;
		}
#endif
		return FALSE;
	}

	_client_react_timeout(client);
	return TRUE;
}

static gboolean
_client_finished(struct gridd_client_s *client)
{
	EXTRA_ASSERT(client != NULL);
	EXTRA_ASSERT(client->abstract.vtable == &VTABLE_CLIENT);

	if (client->error != NULL)
		return TRUE;

	switch (client->step) {
		case NONE:
			return TRUE;
		case CONNECTING:
			return FALSE;
		case REQ_SENDING:
		case REP_READING_SIZE:
		case REP_READING_DATA:
			/* The only case where fd<0 is when an error occured,
			 * and 'error' MUST have been set */
			EXTRA_ASSERT(client->fd >= 0);
			return FALSE;
		case STATUS_OK:
		case STATUS_FAILED:
			return TRUE;
	}

	g_assert_not_reached();
	return FALSE;
}

static gboolean
_client_start(struct gridd_client_s *client)
{
	EXTRA_ASSERT(client != NULL);
	EXTRA_ASSERT(client->abstract.vtable == &VTABLE_CLIENT);

	client->tv_start = client->tv_connect = oio_ext_monotonic_time ();

	if (client->step != NONE) {
		_client_replace_error(client, SYSERR("bug: invalid client state"));
		return FALSE;
	}

	if (!client->url[0]) {
		_client_replace_error(client, SYSERR("bug: no client target"));
		return FALSE;
	}

	if (client->avoidance_onoff) {
		if (oio_client_cache_errors && _has_too_many_errors(client->url)) {
			_client_replace_error(client, NEWERROR(CODE_AVOIDED,
						"Request avoided, service probably down"));
			return FALSE;
		}

		if (oio_client_down_avoid || oio_client_down_shorten) {
			const gboolean down = _is_peer_down(client->url);
			if (down) {
				if (oio_client_down_avoid) {
					_client_replace_error(client, NEWERROR(CODE_AVOIDED,
								"Request avoided, service marked down"));
					return FALSE;
				}
				if (oio_client_down_shorten) {
					client->delay_connect = 25 * G_TIME_SPAN_MILLISECOND;
				}
			}
		}
	}

	GError *err = _client_connect(client);
	if (NULL == err)
		return TRUE;

	client->step = STATUS_FAILED;
	_client_replace_error(client, err);
	return FALSE;
}

static void
_client_fail(struct gridd_client_s *client, GError *why)
{
	EXTRA_ASSERT(client != NULL);
	EXTRA_ASSERT(client->abstract.vtable == &VTABLE_CLIENT);
	_client_replace_error(client, why);
}

static void
_factory_clean(struct gridd_client_factory_s *self)
{
	EXTRA_ASSERT(self != NULL);
	EXTRA_ASSERT(self->abstract.vtable == &VTABLE_FACTORY);
	SLICE_FREE(struct gridd_client_factory_s, self);
}

static struct gridd_client_s *
_factory_create_client (struct gridd_client_factory_s *factory)
{
	EXTRA_ASSERT(factory != NULL);
	EXTRA_ASSERT(factory->abstract.vtable == &VTABLE_FACTORY);
	(void) factory;
	return gridd_client_create_empty();
}

/* ------------------------------------------------------------------------- */

void
gridd_client_learn_peers_down(const char * const * peers)
{
	GTree *new_down = NULL, *old_down = NULL;

	new_down = g_tree_new_full(metautils_strcmp3, NULL, g_free, NULL);
	for (const char * const * ppeer = peers; peers && *ppeer ;++ppeer)
		g_tree_replace(new_down, g_strdup(*ppeer), (void*)0xDEADBEAF);

	g_rw_lock_writer_lock(&lock_down);
	old_down = tree_down;
	tree_down = new_down;
	new_down = NULL;
	g_rw_lock_writer_unlock(&lock_down);

	g_tree_destroy(old_down);
}

struct gridd_client_s *
gridd_client_create_empty(void)
{
	struct gridd_client_s *client = SLICE_NEW0(struct gridd_client_s);
	if (unlikely(NULL == client))
		return NULL;

	client->abstract.vtable = &VTABLE_CLIENT;
	client->fd = -1;
	client->step = NONE;
	client->delay_overall = oio_client_timeout_whole * (gdouble)G_TIME_SPAN_SECOND;
	client->delay_single = oio_client_timeout_single * (gdouble)G_TIME_SPAN_SECOND;
	client->delay_connect = oio_client_timeout_connect * (gdouble)G_TIME_SPAN_SECOND;
	client->tv_start = client->tv_connect = oio_ext_monotonic_time ();

	client->keepalive = 0;
	client->forbid_redirect = 0;
	client->avoidance_onoff = 1;

	return client;
}

void
gridd_client_no_redirect (struct gridd_client_s *c)
{
	if (!c) return;
	EXTRA_ASSERT(c->abstract.vtable == &VTABLE_CLIENT);
	c->forbid_redirect = 1;
}

void
gridd_client_set_keepalive(struct gridd_client_s *c, gboolean onoff)
{
	if (!c) return;
	EXTRA_ASSERT(c->abstract.vtable == &VTABLE_CLIENT);
	c->keepalive = BOOL(onoff);
}

void
gridd_client_set_avoidance (struct gridd_client_s *c, gboolean onoff)
{
	if (!c) return;
	EXTRA_ASSERT(c->abstract.vtable == &VTABLE_CLIENT);
	c->avoidance_onoff = BOOL(onoff);
}

struct gridd_client_factory_s *
gridd_client_factory_create(void)
{
	struct gridd_client_factory_s *factory = SLICE_NEW0(struct gridd_client_factory_s);
	factory->abstract.vtable = &VTABLE_FACTORY;
	return factory;
}

#define GRIDD_CALL(self,F) VTABLE_CALL(self,struct abstract_client_s*,F)

void
gridd_client_free (struct gridd_client_s *self)
{
	if (!self) return;
	GRIDD_CALL(self,clean)(self);
}

GError *
gridd_client_connect_url (struct gridd_client_s *self, const gchar *u)
{
	GRIDD_CALL(self,connect_url)(self,u);
}

GError *
gridd_client_request (struct gridd_client_s *self, GByteArray *req,
		gpointer ctx, client_on_reply cb)
{
	GRIDD_CALL(self,request)(self,req,ctx,cb);
}

GError *
gridd_client_error (struct gridd_client_s *self)
{
	GRIDD_CALL(self,error)(self);
}

int
gridd_client_interest (struct gridd_client_s *self)
{
	GRIDD_CALL(self,interest)(self);
}

const gchar *
gridd_client_url (struct gridd_client_s *self)
{
	GRIDD_CALL(self,get_url)(self);
}

int
gridd_client_fd (struct gridd_client_s *self)
{
	GRIDD_CALL(self,get_fd)(self);
}

GError *
gridd_client_set_fd(struct gridd_client_s *self, int fd)
{
	GRIDD_CALL(self,set_fd)(self,fd);
}

void
gridd_client_set_timeout (struct gridd_client_s *self, gdouble seconds)
{
	GRIDD_CALL(self,set_timeout)(self,seconds);
}

void
gridd_client_set_timeout_cnx (struct gridd_client_s *self, gdouble seconds)
{
	GRIDD_CALL(self,set_timeout_cnx)(self,seconds);
}

gboolean
gridd_client_expired(struct gridd_client_s *self, gint64 now)
{
	GRIDD_CALL(self,expired)(self,now);
}

gboolean
gridd_client_finished (struct gridd_client_s *self)
{
	GRIDD_CALL(self,finished)(self);
}

gboolean
gridd_client_start (struct gridd_client_s *self)
{
	GRIDD_CALL(self,start)(self);
}

gboolean
gridd_client_expire (struct gridd_client_s *self, gint64 now)
{
	GRIDD_CALL(self,expire)(self,now);
}

void
gridd_client_react (struct gridd_client_s *self)
{
	GRIDD_CALL(self,react)(self);
}

void
gridd_client_fail (struct gridd_client_s *self, GError *why)
{
	GRIDD_CALL(self,fail)(self,why);
}

