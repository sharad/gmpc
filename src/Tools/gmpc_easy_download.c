/* Gnome Music Player Client (GMPC)
 * Copyright (C) 2004-2012 Qball Cow <qball@gmpclient.org>
 * Project homepage: http://gmpclient.org/

 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <libsoup/soup.h>
#include "gmpc_easy_download.h"
#include "main.h"

#define LOG_DOMAIN "EasyDownload"

static SoupSession *soup_session = NULL;

/**
 * LIBSOUP BASED ASYNC DOWNLOADER
 */

static void gmpc_easy_async_free_handler_real(GEADAsyncHandler * handle);
typedef struct
{
    SoupMessage *msg;
    gchar *uri;
    GEADAsyncCallback callback;
    gpointer userdata;
  	GError *error;
  	GCancellable *cancellable;
  	GInputStream *istream;
    gchar *data;
    goffset length;
    gpointer extra_data;
} _GEADAsyncHandler;

// static void gmpc_easy_async_callback(SoupSession * session, SoupMessage * msg, gpointer data)
static void gmpc_easy_async_finished(_GEADAsyncHandler *d)
{
	  SoupStatus status_code = soup_message_get_status(d->msg);
	  if (SOUP_STATUS_IS_SUCCESSFUL(status_code) && !d->error)
    {
        d->callback((GEADAsyncHandler *) d, GEAD_DONE, d->userdata);
    } else if (g_error_matches(d->error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
        d->callback((GEADAsyncHandler *) d, GEAD_CANCELLED, d->userdata);
    } else
    {
        d->callback((GEADAsyncHandler *) d, GEAD_FAILED, d->userdata);
    }
    gmpc_easy_async_free_handler_real((GEADAsyncHandler *) d);
}

static void gmpc_easy_async_free_handler_real(GEADAsyncHandler * handle)
{
    _GEADAsyncHandler *d = (_GEADAsyncHandler *) handle;
    g_clear_error(&d->error);
	  g_clear_object(&d->cancellable);
	  g_clear_object(&d->istream);
    g_free(d->uri);
    g_free(d->data);
    g_free(d);
}

/**
 * Get the total size of the download, if available.
 */
goffset gmpc_easy_handler_get_content_size(const GEADAsyncHandler * handle)
{
    _GEADAsyncHandler *d = (_GEADAsyncHandler *) handle;
    SoupMessageHeaders *response_headers = soup_message_get_response_headers(d->msg);
    return soup_message_headers_get_content_length(response_headers);
}

const char *gmpc_easy_handler_get_uri(const GEADAsyncHandler * handle)
{
    _GEADAsyncHandler *d = (_GEADAsyncHandler *) handle;
    return d->uri;
}

const char *gmpc_easy_handler_get_data(const GEADAsyncHandler * handle, goffset * length)
{
    _GEADAsyncHandler *d = (_GEADAsyncHandler *) handle;
    if (length)
        *length = d->length;
    return d->data;
}

const guchar *gmpc_easy_handler_get_data_vala_wrap(const GEADAsyncHandler * handle, gint * length)
{
    _GEADAsyncHandler *d = (_GEADAsyncHandler *) handle;
    if (length)
        *length = (gint) d->length;
    return (guchar *) d->data;
}
const char *gmpc_easy_handler_get_data_as_string(const GEADAsyncHandler * handle)
{
    _GEADAsyncHandler *d = (_GEADAsyncHandler *) handle;
    g_assert(d->data[d->length] == '\0');
    return (gchar *) d->data;
}
void gmpc_easy_handler_set_user_data(const GEADAsyncHandler * handle, gpointer user_data)
{
    _GEADAsyncHandler *d = (_GEADAsyncHandler *) handle;
    d->extra_data = user_data;
}

gpointer gmpc_easy_handler_get_user_data(const GEADAsyncHandler * handle)
{
    _GEADAsyncHandler *d = (_GEADAsyncHandler *) handle;
    return d->extra_data;
}

void gmpc_easy_async_cancel(const GEADAsyncHandler * handle)
{
    _GEADAsyncHandler *d = (_GEADAsyncHandler *) handle;
    if (d->cancellable)
      g_cancellable_cancel(d->cancellable);
}

static void gmpc_easy_async_continue_reading(_GEADAsyncHandler *d);

static void gmpc_easy_async_read_cb(GObject *source_object, GAsyncResult *result, gpointer data)
{
	GInputStream *istream = G_INPUT_STREAM(source_object);
	_GEADAsyncHandler *d = data;
	gssize bytes_read;

	bytes_read = g_input_stream_read_finish(istream, result, &d->error);

	if (bytes_read <= 0)
	{
		/* either EOF or error */
		gmpc_easy_async_finished(d);
		return;
	}

	d->length += bytes_read;
	d->data[d->length] = '\0';

	d->callback((GEADAsyncHandler *) d, GEAD_PROGRESS, d->userdata);
	gmpc_easy_async_continue_reading(d);
}

#define CHUNK_SIZE 4096

static void gmpc_easy_async_continue_reading(_GEADAsyncHandler *d)
{
	d->data = g_realloc(d->data, d->length + CHUNK_SIZE + 1);
	g_input_stream_read_async(d->istream, d->data + d->length, CHUNK_SIZE, G_PRIORITY_DEFAULT, d->cancellable, gmpc_easy_async_read_cb, d);
}

static void gmpc_easy_async_send_cb(GObject *source_object, GAsyncResult *result, gpointer data)
{
	SoupSession *session = SOUP_SESSION(source_object);
	_GEADAsyncHandler *d = data;

	d->istream = soup_session_send_finish(session, result, &d->error);

	if (d->istream == NULL)
	{
		gmpc_easy_async_finished(d);
		return;
	}

	gmpc_easy_async_continue_reading(d);
}

GEADAsyncHandler *gmpc_easy_async_downloader(const gchar * uri, GEADAsyncCallback callback, gpointer user_data)
{
    if (uri == NULL)
    {
        g_log(LOG_DOMAIN,G_LOG_LEVEL_WARNING, "No download uri specified.");
        return NULL;
    }
    return gmpc_easy_async_downloader_with_headers(uri, callback, user_data, NULL);
}

GEADAsyncHandler *gmpc_easy_async_downloader2(const gchar *uri,
											  GEADMethod method,
											  const gchar *post_data,
											  const gchar *content_type,
											  GEADAsyncCallback callback,
											  gpointer user_data)
{
    if (uri == NULL)
    {
        g_log(LOG_DOMAIN,G_LOG_LEVEL_WARNING, "No download uri specified.");
        return NULL;
    }
    return gmpc_easy_async_downloader_with_headers2(uri, method, post_data,
													content_type, callback,
													user_data, NULL);
}

static GEADAsyncHandler *gmpc_easy_async_downloader_with_headers_common(const gchar * uri, GEADMethod method, const gchar *post_data, const gchar *content_type, GEADAsyncCallback callback, gpointer user_data, va_list ap)
{
    SoupMessage *msg;
    SoupMessageHeaders *request_headers;
    _GEADAsyncHandler *d;
    char *va_entry;
    if (soup_session == NULL)
    {
        soup_session = soup_session_new();
        g_object_set(soup_session, "timeout", 5, NULL);
        /* Set user agent, to get around wikipedia ban. */
        g_object_set(soup_session, "user-agent", "gmpc ",NULL);
    }

    msg = soup_message_new(method == GEAD_GET ? "GET" : "POST", uri);
    if (!msg)
        return NULL;

	  if (method == GEAD_POST)
	  {
		soup_message_set_request(msg, content_type, SOUP_MEMORY_COPY,
								 post_data, strlen(post_data));
	  }

	  request_headers = soup_message_get_request_headers(msg);
    va_entry = va_arg(ap, typeof(va_entry));
    while (va_entry)
    {
        char *value = va_arg(ap, typeof(value));
        soup_message_headers_append(request_headers, va_entry, value);
        va_entry = va_arg(ap, typeof(va_entry));
    }

    d = g_malloc0(sizeof(*d));
    d->error = NULL;
    d->cancellable = g_cancellable_new();
    d->istream = NULL;
    d->data = NULL;
    d->msg = msg;
    d->uri = g_strdup(uri);
    d->callback = callback;
    d->userdata = user_data;
    d->extra_data = NULL;
    soup_session_send_async(soup_session, msg, G_PRIORITY_DEFAULT, d->cancellable, gmpc_easy_async_send_cb, d);

    return (GEADAsyncHandler *) d;
}

GEADAsyncHandler *gmpc_easy_async_downloader_with_headers2(const gchar *uri,
														   GEADMethod method,
														   const gchar *post_data,
														   const gchar *content_type,
														   GEADAsyncCallback callback,
														   gpointer user_data,
														   ...)
{
    va_list ap, cp;
	GEADAsyncHandler *ret;

	va_start(ap, user_data);
	va_copy(cp, ap);
	ret = gmpc_easy_async_downloader_with_headers_common(uri, method, post_data,
														 content_type, callback,
														 user_data, cp);
	va_end(ap);
	va_end(cp);
	return ret;
}

GEADAsyncHandler *gmpc_easy_async_downloader_with_headers(const gchar *uri,
														  GEADAsyncCallback callback,
														  gpointer user_data,
														  ...)
{
    va_list ap, cp;
	GEADAsyncHandler *ret;

	va_start(ap, user_data);
	va_copy(cp, ap);
	ret = gmpc_easy_async_downloader_with_headers_common(uri, GEAD_GET, NULL,
														 NULL, callback,
														 user_data, cp);
	va_end(ap);
	va_end(cp);
	return ret;
}

void gmpc_easy_async_quit(void)
{
    if (soup_session)
    {
        soup_session_abort(soup_session);
        g_object_unref(soup_session);
        soup_session = NULL;
    }
}

char *gmpc_easy_download_uri_escape(const char *part)
{
    /* These are the HTTP 'sub-delims', except for & and + */
    static const char reserved_chars_allowed[] = "!$'()*,;=";
    GString *str;

    g_return_val_if_fail (part != NULL, NULL);

    str = g_string_new (NULL);
    /* Don't encode: -._~A-Za-z or reserved_chars_allowed */
    g_string_append_uri_escaped(str, part, reserved_chars_allowed, FALSE);
    return g_string_free (str, FALSE);
}

/**************************************************************/


typedef struct {
    void *a;
    void *b;
    GEADAsyncCallbackVala callback;
} valaf;

static void temp_callback(const GEADAsyncHandler *handle, GEADStatus status, gpointer user_data)
{
    valaf *f = (valaf*)user_data;
    f->callback(handle, status, f->b, f->a);
    if(status != GEAD_PROGRESS) g_free(f);
}
GEADAsyncHandler * gmpc_easy_async_downloader_vala(const char *path, gpointer user_data2, GEADAsyncCallbackVala callback,
                                                          gpointer user_data
                                                          )
{
    valaf *f = g_malloc0(sizeof(*f));
    f->a = user_data;
    f->b =user_data2;
    f->callback = callback;
    return gmpc_easy_async_downloader(path, temp_callback, f);
}

GEADAsyncHandler *gmpc_easy_async_downloader_vala2(const char *path,
												   GEADMethod method,
												   const gchar *post_data,
												   const gchar *content_type,
												   gpointer user_data2,
												   GEADAsyncCallbackVala callback,
												   gpointer user_data)
{
	valaf *f = g_malloc0(sizeof(*f));
	f->a = user_data;
	f->b =user_data2;
	f->callback = callback;
	return gmpc_easy_async_downloader2(path, method, post_data, content_type,
									   temp_callback, f);
}

/* vim: set noexpandtab ts=4 sw=4 sts=4 tw=120: */
