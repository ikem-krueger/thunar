/* vi:set et ai sw=2 sts=2 ts=2: */
/*-
 * Copyright (c) 2009-2011 Jannis Pohlmann <jannis@xfce.org>
 * Copyright (c) 2012      Nick Schermer <nick@xfce.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef HAVE_DBUS
#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>

#include <thunar/thunar-thumbnailer-proxy.h>
#endif

#include <thunar/thunar-marshal.h>
#include <thunar/thunar-private.h>
#include <thunar/thunar-thumbnailer.h>



/**
 * WARNING: The source code in this file may do harm to animals. Dead kittens
 * are to be expected.
 *
 *
 * ThunarThumbnailer is a class for requesting thumbnails from org.xfce.tumbler.*
 * D-Bus services. This header contains an in-depth description of its design to make
 * it easier to understand why things are the way they are.
 *
 * Please note that all D-Bus calls are performed asynchronously.
 *
 *
 * When a request call is sent out, an internal request ID is created and
 * associated with the corresponding DBusGProxyCall.
 *
 * The D-Bus reply handler then checks if there was an delivery error or
 * not. If the request method was sent successfully, the handle returned by the
 * D-Bus thumbnailer is associated bidirectionally with the internal request ID via
 * the request and handle values in the job structure. In both cases, the
 * association of the internal request ID with the DBusGProxyCall is set to NULL.
 *
 *
 * Ready / Error
 * =============
 *
 * The Ready and Error signal handlers work exactly like Started except that
 * the Ready idle function sets the thumb state of the corresponding
 * ThunarFile objects to _READY and the Error signal sets the state to _NONE.
 *
 *
 * Finished
 * ========
 *
 * The Finished signal handler looks up the internal request ID based on
 * the D-Bus thumbnailer handle. It then drops all corresponding information
 * from handle_request_mapping and request_handle_mapping.
 */



#ifdef HAVE_DBUS
typedef enum
{
  THUNAR_THUMBNAILER_IDLE_ERROR,
  THUNAR_THUMBNAILER_IDLE_READY,
} ThunarThumbnailerIdleType;



typedef struct _ThunarThumbnailerJob  ThunarThumbnailerJob;
typedef struct _ThunarThumbnailerIdle ThunarThumbnailerIdle;
#endif

/* Signal identifiers */
enum
{
  REQUEST_FINISHED,
  LAST_SIGNAL,
};



static void                   thunar_thumbnailer_finalize               (GObject                    *object);
#ifdef HAVE_DBUS
static void                   thunar_thumbnailer_init_thumbnailer_proxy (ThunarThumbnailer          *thumbnailer,
                                                                         DBusGConnection            *connection);
static gboolean               thunar_thumbnailer_file_is_supported      (ThunarThumbnailer          *thumbnailer,
                                                                         ThunarFile                 *file);
static void                   thunar_thumbnailer_thumbnailer_finished   (DBusGProxy                 *proxy,
                                                                         guint                       handle,
                                                                         ThunarThumbnailer          *thumbnailer);
static void                   thunar_thumbnailer_thumbnailer_error      (DBusGProxy                 *proxy,
                                                                         guint                       handle,
                                                                         const gchar               **uris,
                                                                         gint                        code,
                                                                         const gchar                *message,
                                                                         ThunarThumbnailer          *thumbnailer);
static void                   thunar_thumbnailer_thumbnailer_ready      (DBusGProxy                 *proxy,
                                                                         guint32                     handle,
                                                                         const gchar               **uris,
                                                                         ThunarThumbnailer          *thumbnailer);
static guint                  thunar_thumbnailer_queue_async            (ThunarThumbnailer          *thumbnailer,
                                                                         gchar                     **uris,
                                                                         const gchar               **mime_hints);
static void                   thunar_thumbnailer_idle                   (ThunarThumbnailer          *thumbnailer,
                                                                         guint                       handle,
                                                                         ThunarThumbnailerIdleType   type,
                                                                         const gchar               **uris);
static gboolean               thunar_thumbnailer_idle_func              (gpointer                    user_data);
static void                   thunar_thumbnailer_idle_free              (gpointer                    data);
#endif



struct _ThunarThumbnailerClass
{
  GObjectClass __parent__;
};

struct _ThunarThumbnailer
{
  GObject __parent__;

#ifdef HAVE_DBUS
  /* proxies to communicate with D-Bus services */
  DBusGProxy *thumbnailer_proxy;

  /* running jobs */
  GSList     *jobs;

  GMutex     *lock;

  /* cached arrays of URI schemes and MIME types for which thumbnails
   * can be generated */
  gchar     **supported_schemes;
  gchar     **supported_types;

  /* last ThunarThumbnailer request ID */
  guint       last_request;

  /* IDs of idle functions */
  GSList     *idles;
#endif
};

#ifdef HAVE_DBUS
struct _ThunarThumbnailerJob
{
  ThunarThumbnailer *thumbnailer;

  /* if this job is cancelled */
  guint              cancelled : 1;

  /* request number returned by ThunarThumbnailer */
  guint              request;

  /* handle returned by the tumbler dbus service */
  guint              handle;

  /* dbus call to get the handle */
  DBusGProxyCall    *handle_call;
};

struct _ThunarThumbnailerIdle
{
  ThunarThumbnailerIdleType  type;
  ThunarThumbnailer          *thumbnailer;
  guint                       id;
  gchar                     **uris;
};
#endif



static guint thumbnailer_signals[LAST_SIGNAL];



G_DEFINE_TYPE (ThunarThumbnailer, thunar_thumbnailer, G_TYPE_OBJECT);



static void
thunar_thumbnailer_class_init (ThunarThumbnailerClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize = thunar_thumbnailer_finalize;

  /**
   * ThunarThumbnailer:request-finished:
   * @thumbnailer : a #ThunarThumbnailer.
   * @request     : id of the request that is finished.
   *
   * Emitted by @thumbnailer, when a request is finished
   * by the thumbnail generator
   **/
  thumbnailer_signals[REQUEST_FINISHED] =
    g_signal_new (I_("request-finished"),
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL,
                  g_cclosure_marshal_VOID__UINT,
                  G_TYPE_NONE, 1, G_TYPE_UINT);
}



static void
thunar_thumbnailer_init (ThunarThumbnailer *thumbnailer)
{
#ifdef HAVE_DBUS
  DBusGConnection *connection;

  thumbnailer->lock = g_mutex_new ();

  /* try to connect to D-Bus */
  connection = dbus_g_bus_get (DBUS_BUS_SESSION, NULL);

  /* initialize the proxies */
  thunar_thumbnailer_init_thumbnailer_proxy (thumbnailer, connection);

  /* release the D-Bus connection if we have one */
  if (connection != NULL)
    dbus_g_connection_unref (connection);
#endif
}



static void
thunar_thumbnailer_finalize (GObject *object)
{
#ifdef HAVE_DBUS
  ThunarThumbnailer     *thumbnailer = THUNAR_THUMBNAILER (object);
  ThunarThumbnailerIdle *idle;
  ThunarThumbnailerJob  *job;
  GSList                *lp;

  /* acquire the thumbnailer lock */
  g_mutex_lock (thumbnailer->lock);

  if (thumbnailer->thumbnailer_proxy != NULL)
    {
      /* disconnect from the thumbnailer proxy */
      g_signal_handlers_disconnect_matched (thumbnailer->thumbnailer_proxy,
                                            G_SIGNAL_MATCH_DATA, 0, 0,
                                            NULL, NULL, thumbnailer);
    }

  /* abort all pending idle functions */
  for (lp = thumbnailer->idles; lp != NULL; lp = lp->next)
    {
      idle = lp->data;
      g_source_remove (idle->id);
    }
  g_slist_free (thumbnailer->idles);

  /* remove all jobs */
  for (lp = thumbnailer->jobs; lp != NULL; lp = lp->next)
    {
      job = lp->data;

      if (thumbnailer->thumbnailer_proxy != NULL)
        {
          if (job->handle_call != NULL)
            dbus_g_proxy_cancel_call (thumbnailer->thumbnailer_proxy, job->handle_call);

          if (job->handle != 0)
            thunar_thumbnailer_proxy_dequeue (thumbnailer->thumbnailer_proxy, job->handle, NULL);
        }

      g_slice_free (ThunarThumbnailerJob, job);
    }
  g_slist_free (thumbnailer->jobs);

  /* release the thumbnailer proxy */
  if (thumbnailer->thumbnailer_proxy != NULL)
    g_object_unref (thumbnailer->thumbnailer_proxy);

  /* free the cached URI schemes and MIME types array */
  g_strfreev (thumbnailer->supported_schemes);
  g_strfreev (thumbnailer->supported_types);

  /* release the thumbnailer lock */
  g_mutex_unlock (thumbnailer->lock);

  /* release the mutex */
  g_mutex_free (thumbnailer->lock);
#endif

  (*G_OBJECT_CLASS (thunar_thumbnailer_parent_class)->finalize) (object);
}



#ifdef HAVE_DBUS
static void
thunar_thumbnailer_init_thumbnailer_proxy (ThunarThumbnailer *thumbnailer,
                                           DBusGConnection   *connection)
{
  /* we can't have a proxy without a D-Bus connection */
  if (connection == NULL)
    {
      thumbnailer->thumbnailer_proxy = NULL;
      return;
    }

  /* create the thumbnailer proxy */
  thumbnailer->thumbnailer_proxy =
    dbus_g_proxy_new_for_name (connection,
                               "org.freedesktop.thumbnails.Thumbnailer1",
                               "/org/freedesktop/thumbnails/Thumbnailer1",
                               "org.freedesktop.thumbnails.Thumbnailer1");

  /* TODO this should actually be VOID:UINT,BOXED,INT,STRING */
  dbus_g_object_register_marshaller (_thunar_marshal_VOID__UINT_BOXED_UINT_STRING,
                                     G_TYPE_NONE,
                                     G_TYPE_UINT,
                                     G_TYPE_STRV,
                                     G_TYPE_UINT,
                                     G_TYPE_STRING,
                                     G_TYPE_INVALID);

  dbus_g_object_register_marshaller ((GClosureMarshal) _thunar_marshal_VOID__UINT_BOXED,
                                     G_TYPE_NONE,
                                     G_TYPE_UINT,
                                     G_TYPE_STRV,
                                     G_TYPE_INVALID);

  dbus_g_proxy_add_signal (thumbnailer->thumbnailer_proxy, "Error",
                           G_TYPE_UINT, G_TYPE_STRV, G_TYPE_UINT, G_TYPE_STRING,
                           G_TYPE_INVALID);
  dbus_g_proxy_add_signal (thumbnailer->thumbnailer_proxy, "Finished",
                           G_TYPE_UINT, G_TYPE_INVALID);
  dbus_g_proxy_add_signal (thumbnailer->thumbnailer_proxy, "Ready",
                           G_TYPE_UINT, G_TYPE_STRV, G_TYPE_INVALID);
  dbus_g_proxy_add_signal (thumbnailer->thumbnailer_proxy, "Started",
                           G_TYPE_UINT, G_TYPE_INVALID);

  dbus_g_proxy_connect_signal (thumbnailer->thumbnailer_proxy, "Error",
                               G_CALLBACK (thunar_thumbnailer_thumbnailer_error),
                               thumbnailer, NULL);
  dbus_g_proxy_connect_signal (thumbnailer->thumbnailer_proxy, "Finished",
                               G_CALLBACK (thunar_thumbnailer_thumbnailer_finished),
                               thumbnailer, NULL);
  dbus_g_proxy_connect_signal (thumbnailer->thumbnailer_proxy, "Ready",
                               G_CALLBACK (thunar_thumbnailer_thumbnailer_ready),
                               thumbnailer, NULL);
}



static gboolean
thunar_thumbnailer_file_is_supported (ThunarThumbnailer *thumbnailer,
                                      ThunarFile        *file)
{
  const gchar *content_type;
  gboolean     supported = FALSE;
  guint        n;

  _thunar_return_val_if_fail (THUNAR_IS_THUMBNAILER (thumbnailer), FALSE);
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), FALSE);

  /* acquire the thumbnailer lock */
  g_mutex_lock (thumbnailer->lock);

  /* no types are supported if we don't have a thumbnailer */
  if (thumbnailer->thumbnailer_proxy == NULL)
    {
      /* release the thumbnailer lock */
      g_mutex_unlock (thumbnailer->lock);
      return FALSE;
    }

  /* determine the content type of the passed file */
  content_type = thunar_file_get_content_type (file);

  /* abort if the content type is unknown */
  if (content_type == NULL)
    {
      /* release the thumbnailer lock */
      g_mutex_unlock (thumbnailer->lock);
      return FALSE;
    }

  /* request the supported types on demand */
  if (thumbnailer->supported_schemes == NULL
      || thumbnailer->supported_types == NULL)
    {
      /* request the supported types from the thumbnailer D-Bus service. We only do
       * this once, so using a non-async call should be ok */
      thunar_thumbnailer_proxy_get_supported (thumbnailer->thumbnailer_proxy,
                                              &thumbnailer->supported_schemes,
                                              &thumbnailer->supported_types,
                                              NULL);
    }

  /* check if we have supported URI schemes and MIME types now */
  if (thumbnailer->supported_schemes != NULL
      && thumbnailer->supported_types != NULL)
    {
      /* go through all the URI schemes we support */
      for (n = 0; !supported && thumbnailer->supported_schemes[n] != NULL; ++n)
        {
          /* check if the file has the current URI scheme */
          if (thunar_file_has_uri_scheme (file, thumbnailer->supported_schemes[n]))
            {
              /* check if the type of the file is a subtype of the supported type */
              if (g_content_type_is_a (content_type, thumbnailer->supported_types[n]))
                supported = TRUE;
            }
        }
    }

  /* release the thumbnailer lock */
  g_mutex_unlock (thumbnailer->lock);

  return supported;
}



static void
thunar_thumbnailer_thumbnailer_error (DBusGProxy        *proxy,
                                      guint              handle,
                                      const gchar      **uris,
                                      gint               code,
                                      const gchar       *message,
                                      ThunarThumbnailer *thumbnailer)
{
  _thunar_return_if_fail (DBUS_IS_G_PROXY (proxy));
  _thunar_return_if_fail (THUNAR_IS_THUMBNAILER (thumbnailer));

  /* check if we have any ready URIs */
  thunar_thumbnailer_idle (thumbnailer,
                           handle,
                           THUNAR_THUMBNAILER_IDLE_ERROR,
                           uris);
}



static void
thunar_thumbnailer_thumbnailer_ready (DBusGProxy        *proxy,
                                      guint32            handle,
                                      const gchar      **uris,
                                      ThunarThumbnailer *thumbnailer)
{
  _thunar_return_if_fail (DBUS_IS_G_PROXY (proxy));
  _thunar_return_if_fail (THUNAR_IS_THUMBNAILER (thumbnailer));

  thunar_thumbnailer_idle (thumbnailer,
                           handle,
                           THUNAR_THUMBNAILER_IDLE_READY,
                           uris);
}



static void
thunar_thumbnailer_thumbnailer_finished (DBusGProxy        *proxy,
                                         guint              handle,
                                         ThunarThumbnailer *thumbnailer)
{
  ThunarThumbnailerJob *job;
  GSList               *lp;

  _thunar_return_if_fail (DBUS_IS_G_PROXY (proxy));
  _thunar_return_if_fail (THUNAR_IS_THUMBNAILER (thumbnailer));

  for (lp = thumbnailer->jobs; lp != NULL; lp = lp->next)
    {
      job = lp->data;

      if (job->handle == handle)
        {
          /* this job is finished, forget about the handle */
          job->handle = 0;

          /* tell everybody we're done here */
          g_signal_emit (G_OBJECT (thumbnailer), thumbnailer_signals[REQUEST_FINISHED], 0, job->request);

          /* remove job from the list */
          g_mutex_lock (thumbnailer->lock);
          thumbnailer->jobs = g_slist_delete_link (thumbnailer->jobs, lp);
          g_mutex_unlock (thumbnailer->lock);

          g_slice_free (ThunarThumbnailerJob, job);
          break;
        }
    }
}



static void
thunar_thumbnailer_queue_async_reply (DBusGProxy *proxy,
                                      guint       handle,
                                      GError     *error,
                                      gpointer    user_data)
{
  ThunarThumbnailerJob *job = user_data;
  ThunarThumbnailer    *thumbnailer = THUNAR_THUMBNAILER (job->thumbnailer);

  _thunar_return_if_fail (DBUS_IS_G_PROXY (proxy));
  _thunar_return_if_fail (job != NULL);
  _thunar_return_if_fail (THUNAR_IS_THUMBNAILER (thumbnailer));

  g_mutex_lock (thumbnailer->lock);

  /* the queue call is finished, we can forget about its proxy call */
  job->handle_call = NULL;

  if (job->cancelled)
    {
      /* job is cancelled while there was no handle jet, so dequeue it now */
      thunar_thumbnailer_proxy_dequeue (proxy, handle, NULL);

      /* cleanup */
      thumbnailer->jobs = g_slist_remove (thumbnailer->jobs, job);
      g_slice_free (ThunarThumbnailerJob, job);
    }
  else if (error == NULL)
    {
      /* store the handle returned by tumbler */
      job->handle = handle;
    }

  g_mutex_unlock (thumbnailer->lock);
}



static guint
thunar_thumbnailer_queue_async (ThunarThumbnailer *thumbnailer,
                                gchar            **uris,
                                const gchar      **mime_hints)
{
  ThunarThumbnailerJob *job;
  guint                 request_no;

  _thunar_return_val_if_fail (THUNAR_IS_THUMBNAILER (thumbnailer), 0);
  _thunar_return_val_if_fail (uris != NULL, 0);
  _thunar_return_val_if_fail (mime_hints != NULL, 0);
  _thunar_return_val_if_fail (DBUS_IS_G_PROXY (thumbnailer->thumbnailer_proxy), 0);
  _thunar_return_val_if_fail (!g_mutex_trylock (thumbnailer->lock), 0);

  /* compute the next request ID, making sure it's never 0 */
  request_no = thumbnailer->last_request + 1;
  request_no = MAX (request_no, 1);

  /* remember the ID for the next request */
  thumbnailer->last_request = request_no;

  /* allocate a new struct to follow this job until finished */
  job = g_slice_new0 (ThunarThumbnailerJob);
  job->thumbnailer = thumbnailer;
  job->request = request_no;

  /* store the job */
  thumbnailer->jobs = g_slist_prepend (thumbnailer->jobs, job);

  /* queue thumbnails for the given URIs asynchronously */
  job->handle_call = thunar_thumbnailer_proxy_queue_async (thumbnailer->thumbnailer_proxy,
                                                           (const gchar **)uris, mime_hints,
                                                           "normal", "foreground", 0,
                                                           thunar_thumbnailer_queue_async_reply,
                                                           job);

  /* return the request ID used for this request */
  return request_no;
}



static void
thunar_thumbnailer_idle (ThunarThumbnailer          *thumbnailer,
                         guint                       handle,
                         ThunarThumbnailerIdleType   type,
                         const gchar               **uris)
{
  GSList                *lp;
  ThunarThumbnailerIdle *idle;
  ThunarThumbnailerJob  *job;

  /* leave if there are no uris */
  if (G_UNLIKELY (uris == NULL))
    return;

  /* look for the job so we don't emit unknown handles, the reason
   * we do this is when you have multiple windows opened, you don't
   * want each window (because they all have a connection to the
   * same proxy) emit the file change, only the window that requested
   * the data */
  for (lp = thumbnailer->jobs; lp != NULL; lp = lp->next)
    {
      job = lp->data;

      if (job->handle == handle)
        {
          /* allocate a new idle struct */
          idle = g_slice_new0 (ThunarThumbnailerIdle);
          idle->type = type;
          idle->thumbnailer = thumbnailer;

          /* copy the URI array because we need it in the idle function */
          idle->uris = g_strdupv ((gchar **)uris);

          /* remember the idle struct because we might have to remove it in finalize() */
          g_mutex_lock (thumbnailer->lock);
          thumbnailer->idles = g_slist_prepend (thumbnailer->idles, idle);
          g_mutex_unlock (thumbnailer->lock);

          /* call the idle function when we have the time */
          idle->id = g_idle_add_full (G_PRIORITY_LOW,
                                      thunar_thumbnailer_idle_func, idle,
                                      thunar_thumbnailer_idle_free);

          break;
        }
    }
}



static gboolean
thunar_thumbnailer_idle_func (gpointer user_data)
{
  ThunarThumbnailerIdle *idle = user_data;
  ThunarFile            *file;
  GFile                 *gfile;
  guint                  n;

  _thunar_return_val_if_fail (idle != NULL, FALSE);
  _thunar_return_val_if_fail (THUNAR_IS_THUMBNAILER (idle->thumbnailer), FALSE);

  /* iterate over all failed URIs */
  for (n = 0; idle->uris != NULL && idle->uris[n] != NULL; ++n)
    {
      /* look up the corresponding ThunarFile from the cache */
      gfile = g_file_new_for_uri (idle->uris[n]);
      file = thunar_file_cache_lookup (gfile);
      g_object_unref (gfile);

      /* check if we have a file for this URI in the cache */
      if (file != NULL)
        {
          if (idle->type == THUNAR_THUMBNAILER_IDLE_ERROR)
            {
              /* set thumbnail state to none unless the thumbnail has already been created.
               * This is to prevent race conditions with the other idle functions */
              if (thunar_file_get_thumb_state (file) != THUNAR_FILE_THUMB_STATE_READY)
                thunar_file_set_thumb_state (file, THUNAR_FILE_THUMB_STATE_NONE);
            }
          else if (idle->type == THUNAR_THUMBNAILER_IDLE_READY)
            {
              /* set thumbnail state to ready - we now have a thumbnail */
              thunar_file_set_thumb_state (file, THUNAR_FILE_THUMB_STATE_READY);
            }
          else
            {
              _thunar_assert_not_reached ();
            }
        }
    }

  /* remove the idle struct */
  g_mutex_lock (idle->thumbnailer->lock);
  idle->thumbnailer->idles = g_slist_remove (idle->thumbnailer->idles, idle);
  g_mutex_unlock (idle->thumbnailer->lock);

  /* remove the idle source, which also destroys the idle struct */
  return FALSE;
}



static void
thunar_thumbnailer_idle_free (gpointer data)
{
  ThunarThumbnailerIdle *idle = data;

  _thunar_return_if_fail (idle != NULL);

  /* free the URI array if necessary */
  if (idle->type == THUNAR_THUMBNAILER_IDLE_READY
      || idle->type == THUNAR_THUMBNAILER_IDLE_ERROR)
    {
      g_strfreev (idle->uris);
    }

  /* free the struct */
  g_slice_free (ThunarThumbnailerIdle, idle);
}
#endif /* HAVE_DBUS */



/**
 * thunar_thumbnailer_new:
 *
 * Allocates a new #ThunarThumbnailer object, which can be used to
 * generate and store thumbnails for files.
 *
 * The caller is responsible to free the returned
 * object using g_object_unref() when no longer needed.
 *
 * Return value: a newly allocated #ThunarThumbnailer.
 **/
ThunarThumbnailer*
thunar_thumbnailer_new (void)
{
  return g_object_new (THUNAR_TYPE_THUMBNAILER, NULL);
}



gboolean
thunar_thumbnailer_queue_file (ThunarThumbnailer *thumbnailer,
                               ThunarFile        *file,
                               guint             *request)
{
  GList files;

  _thunar_return_val_if_fail (THUNAR_IS_THUMBNAILER (thumbnailer), FALSE);
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), FALSE);

  /* fake a file list */
  files.data = file;
  files.next = NULL;
  files.prev = NULL;

  /* queue a thumbnail request for the file */
  return thunar_thumbnailer_queue_files (thumbnailer, &files, request);
}



gboolean
thunar_thumbnailer_queue_files (ThunarThumbnailer *thumbnailer,
                                GList             *files,
                                guint             *request)
{
  gboolean      success = FALSE;
#ifdef HAVE_DBUS
  const gchar **mime_hints;
  gchar       **uris;
  GList        *lp;
  GList        *supported_files = NULL;
  guint         n;
  guint         n_items;
#endif

  _thunar_return_val_if_fail (THUNAR_IS_THUMBNAILER (thumbnailer), FALSE);
  _thunar_return_val_if_fail (files != NULL, FALSE);

#ifdef HAVE_DBUS
  /* acquire the thumbnailer lock */
  g_mutex_lock (thumbnailer->lock);

  if (thumbnailer->thumbnailer_proxy == NULL)
    {
      /* release the lock and abort */
      g_mutex_unlock (thumbnailer->lock);
      return FALSE;
    }

  /* release the thumbnailer lock */
  g_mutex_unlock (thumbnailer->lock);

  /* collect all supported files from the list that are neither in the
   * about to be queued (wait queue), nor already queued, nor already
   * processed (and awaiting to be refreshed) */
  for (lp = g_list_last (files); lp != NULL; lp = lp->prev)
    {
      if (thunar_thumbnailer_file_is_supported (thumbnailer, lp->data))
        supported_files = g_list_prepend (supported_files, lp->data);
    }

  /* determine how many URIs are in the wait queue */
  n_items = g_list_length (supported_files);

  /* check if we have any supported files */
  if (n_items > 0)
    {
      /* allocate arrays for URIs and mime hints */
      uris = g_new0 (gchar *, n_items + 1);
      mime_hints = g_new0 (const gchar *, n_items + 1);

      /* fill URI and MIME hint arrays with items from the wait queue */
      for (lp = g_list_last (supported_files), n = 0; lp != NULL; lp = lp->prev, ++n)
        {
          /* set the thumbnail state to loading */
          thunar_file_set_thumb_state (lp->data, THUNAR_FILE_THUMB_STATE_LOADING);

          /* save URI and MIME hint in the arrays */
          uris[n] = thunar_file_dup_uri (lp->data);
          mime_hints[n] = thunar_file_get_content_type (lp->data);
        }

      /* NULL-terminate both arrays */
      uris[n] = NULL;
      mime_hints[n] = NULL;

      g_mutex_lock (thumbnailer->lock);

      /* queue a thumbnail request for the URIs from the wait queue */
      if (request != NULL)
        *request = thunar_thumbnailer_queue_async (thumbnailer, uris, mime_hints);
      else
        thunar_thumbnailer_queue_async (thumbnailer, uris, mime_hints);

      g_mutex_unlock (thumbnailer->lock);

      /* free mime hints array */
      g_free (mime_hints);
      g_strfreev (uris);

      /* free the list of supported files */
      g_list_free (supported_files);

      /* we assume success if we've come so far */
      success = TRUE;
    }
#endif /* HAVE_DBUS */

  return success;
}


void
thunar_thumbnailer_dequeue (ThunarThumbnailer *thumbnailer,
                            guint              request)
{
#ifdef HAVE_DBUS
  ThunarThumbnailerJob *job;
  GSList               *lp;
#endif

  _thunar_return_if_fail (THUNAR_IS_THUMBNAILER (thumbnailer));

#ifdef HAVE_DBUS
  /* acquire the thumbnailer lock */
  g_mutex_lock (thumbnailer->lock);

  for (lp = thumbnailer->jobs; lp != NULL; lp = lp->next)
    {
      job = lp->data;

      /* find the request in the list */
      if (job->request == request)
        {
          /* this job is cancelled */
          job->cancelled = TRUE;

          if (job->handle != 0)
            {
              /* dequeue the tumbler request */
              if (thumbnailer->thumbnailer_proxy != NULL)
                thunar_thumbnailer_proxy_dequeue (thumbnailer->thumbnailer_proxy, job->handle, NULL);

              /* remove job */
              thumbnailer->jobs = g_slist_delete_link (thumbnailer->jobs, lp);
              g_slice_free (ThunarThumbnailerJob, job);
            }

          break;
        }
    }

  /* release the thumbnailer lock */
  g_mutex_unlock (thumbnailer->lock);
#endif
}
