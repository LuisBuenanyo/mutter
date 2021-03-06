/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/*
 * Copyright 2013 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Giovanni Campagna <gcampagn@redhat.com>
 */

/**
 * SECTION:cursor-tracker
 * @title: MetaCursorTracker
 * @short_description: Mutter cursor tracking helper. Originally only
 *                     tracking the cursor image, now more of a "core
 *                     pointer abstraction"
 */

#include "config.h"
#include "meta-cursor-tracker-private.h"

#include <string.h>
#include <meta/main.h>
#include <meta/util.h>
#include <meta/errors.h>

#include <cogl/cogl.h>
#include <clutter/clutter.h>

#include <gdk/gdk.h>
#include <gdk/gdkx.h>
#include <X11/extensions/Xfixes.h>

#include "meta-backend-private.h"

G_DEFINE_TYPE (MetaCursorTracker, meta_cursor_tracker, G_TYPE_OBJECT);

enum {
  CURSOR_CHANGED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

static MetaCursorSprite *
get_displayed_cursor (MetaCursorTracker *tracker)
{
  MetaDisplay *display = meta_get_display ();

  if (!tracker->is_showing)
    return NULL;

  if (meta_display_windows_are_interactable (display))
    {
      if (tracker->has_window_cursor)
        return tracker->window_cursor;
    }

  return tracker->root_cursor;
}

static void
update_displayed_cursor (MetaCursorTracker *tracker)
{
  meta_cursor_renderer_set_cursor (tracker->renderer, tracker->displayed_cursor);
}

static void
sync_cursor (MetaCursorTracker *tracker)
{
  MetaCursorSprite *displayed_cursor = get_displayed_cursor (tracker);

  if (tracker->displayed_cursor == displayed_cursor)
    return;

  g_clear_object (&tracker->displayed_cursor);
  if (displayed_cursor)
    tracker->displayed_cursor = g_object_ref (displayed_cursor);

  update_displayed_cursor (tracker);
  g_signal_emit (tracker, signals[CURSOR_CHANGED], 0);
}

static void
meta_cursor_tracker_init (MetaCursorTracker *self)
{
  MetaBackend *backend = meta_get_backend ();

  self->renderer = meta_backend_get_cursor_renderer (backend);
  self->is_showing = TRUE;
}

static void
meta_cursor_tracker_finalize (GObject *object)
{
  MetaCursorTracker *self = META_CURSOR_TRACKER (object);

  if (self->displayed_cursor)
    g_object_unref (self->displayed_cursor);
  if (self->root_cursor)
    g_object_unref (self->root_cursor);

  G_OBJECT_CLASS (meta_cursor_tracker_parent_class)->finalize (object);
}

static void
meta_cursor_tracker_class_init (MetaCursorTrackerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = meta_cursor_tracker_finalize;

  signals[CURSOR_CHANGED] = g_signal_new ("cursor-changed",
                                          G_TYPE_FROM_CLASS (klass),
                                          G_SIGNAL_RUN_LAST,
                                          0,
                                          NULL, NULL, NULL,
                                          G_TYPE_NONE, 0);
}

static MetaCursorTracker *
meta_cursor_tracker_new (void)
{
  return g_object_new (META_TYPE_CURSOR_TRACKER, NULL);
}

static MetaCursorTracker *_cursor_tracker;

/**
 * meta_cursor_tracker_get_for_screen:
 * @screen: the #MetaScreen
 *
 * Retrieves the cursor tracker object for @screen.
 *
 * Returns: (transfer none):
 */
MetaCursorTracker *
meta_cursor_tracker_get_for_screen (MetaScreen *screen)
{
  if (!_cursor_tracker)
    _cursor_tracker = meta_cursor_tracker_new ();

  return _cursor_tracker;
}

static void
set_window_cursor (MetaCursorTracker *tracker,
                   gboolean           has_cursor,
                   MetaCursorSprite  *cursor_sprite)
{
  g_clear_object (&tracker->window_cursor);
  if (cursor_sprite)
    tracker->window_cursor = g_object_ref (cursor_sprite);
  tracker->has_window_cursor = has_cursor;
  sync_cursor (tracker);
}

gboolean
meta_cursor_tracker_handle_xevent (MetaCursorTracker *tracker,
                                   XEvent            *xevent)
{
  MetaDisplay *display = meta_get_display ();
  XFixesCursorNotifyEvent *notify_event;

  if (meta_is_wayland_compositor ())
    return FALSE;

  if (xevent->xany.type != display->xfixes_event_base + XFixesCursorNotify)
    return FALSE;

  notify_event = (XFixesCursorNotifyEvent *)xevent;
  if (notify_event->subtype != XFixesDisplayCursorNotify)
    return FALSE;

  g_clear_object (&tracker->xfixes_cursor);
  g_signal_emit (tracker, signals[CURSOR_CHANGED], 0);

  return TRUE;
}

static void
ensure_xfixes_cursor (MetaCursorTracker *tracker)
{
  MetaDisplay *display = meta_get_display ();
  XFixesCursorImage *cursor_image;
  CoglTexture2D *sprite;
  guint8 *cursor_data;
  gboolean free_cursor_data;
  CoglContext *ctx;
  CoglError *error = NULL;

  if (tracker->xfixes_cursor)
    return;

  cursor_image = XFixesGetCursorImage (display->xdisplay);
  if (!cursor_image)
    return;

  /* Like all X APIs, XFixesGetCursorImage() returns arrays of 32-bit
   * quantities as arrays of long; we need to convert on 64 bit */
  if (sizeof(long) == 4)
    {
      cursor_data = (guint8 *)cursor_image->pixels;
      free_cursor_data = FALSE;
    }
  else
    {
      int i, j;
      guint32 *cursor_words;
      gulong *p;
      guint32 *q;

      cursor_words = g_new (guint32, cursor_image->width * cursor_image->height);
      cursor_data = (guint8 *)cursor_words;

      p = cursor_image->pixels;
      q = cursor_words;
      for (j = 0; j < cursor_image->height; j++)
        for (i = 0; i < cursor_image->width; i++)
          *(q++) = *(p++);

      free_cursor_data = TRUE;
    }

  ctx = clutter_backend_get_cogl_context (clutter_get_default_backend ());
  sprite = cogl_texture_2d_new_from_data (ctx,
                                          cursor_image->width,
                                          cursor_image->height,
                                          CLUTTER_CAIRO_FORMAT_ARGB32,
                                          cursor_image->width * 4, /* stride */
                                          cursor_data,
                                          &error);

  if (free_cursor_data)
    g_free (cursor_data);

  if (error != NULL)
    {
      meta_warning ("Failed to allocate cursor sprite texture: %s\n", error->message);
      cogl_error_free (error);
    }

  if (sprite != NULL)
    {
      MetaCursorSprite *cursor_sprite = meta_cursor_sprite_new ();
      meta_cursor_sprite_set_texture (cursor_sprite,
                                      sprite,
                                      cursor_image->xhot,
                                      cursor_image->yhot);
      cogl_object_unref (sprite);
      tracker->xfixes_cursor = cursor_sprite;
    }
  XFree (cursor_image);
}

/**
 * meta_cursor_tracker_get_sprite:
 *
 * Returns: (transfer none):
 */
CoglTexture *
meta_cursor_tracker_get_sprite (MetaCursorTracker *tracker)
{
  MetaCursorSprite *cursor_sprite;

  g_return_val_if_fail (META_IS_CURSOR_TRACKER (tracker), NULL);

  if (meta_is_wayland_compositor ())
    {
      cursor_sprite = tracker->displayed_cursor;
    }
  else
    {
      ensure_xfixes_cursor (tracker);
      cursor_sprite = tracker->xfixes_cursor;
    }

  if (cursor_sprite)
    return meta_cursor_sprite_get_cogl_texture (cursor_sprite);
  else
    return NULL;
}

/**
 * meta_cursor_tracker_get_hot:
 * @tracker:
 * @x: (out):
 * @y: (out):
 *
 */
void
meta_cursor_tracker_get_hot (MetaCursorTracker *tracker,
                             int               *x,
                             int               *y)
{
  MetaCursorSprite *cursor_sprite;

  g_return_if_fail (META_IS_CURSOR_TRACKER (tracker));

  if (meta_is_wayland_compositor ())
    {
      cursor_sprite = tracker->displayed_cursor;
    }
  else
    {
      ensure_xfixes_cursor (tracker);
      cursor_sprite = tracker->xfixes_cursor;
    }

  if (cursor_sprite)
    meta_cursor_sprite_get_hotspot (cursor_sprite, x, y);
  else
    {
      if (x)
        *x = 0;
      if (y)
        *y = 0;
    }
}

void
meta_cursor_tracker_set_window_cursor (MetaCursorTracker *tracker,
                                       MetaCursorSprite  *cursor_sprite)
{
  set_window_cursor (tracker, TRUE, cursor_sprite);
}

void
meta_cursor_tracker_unset_window_cursor (MetaCursorTracker *tracker)
{
  set_window_cursor (tracker, FALSE, NULL);
}

void
meta_cursor_tracker_set_root_cursor (MetaCursorTracker *tracker,
                                     MetaCursorSprite  *cursor_sprite)
{
  g_clear_object (&tracker->root_cursor);
  if (cursor_sprite)
    tracker->root_cursor = g_object_ref (cursor_sprite);

  sync_cursor (tracker);
}

void
meta_cursor_tracker_update_position (MetaCursorTracker *tracker,
                                     int                new_x,
                                     int                new_y)
{
  g_assert (meta_is_wayland_compositor ());

  meta_cursor_renderer_set_position (tracker->renderer, new_x, new_y);
}

static void
get_pointer_position_gdk (int         *x,
                          int         *y,
                          int         *mods)
{
  GdkSeat *gseat;
  GdkDevice *gdevice;
  GdkScreen *gscreen;

  gseat = gdk_display_get_default_seat (gdk_display_get_default ());
  gdevice = gdk_seat_get_pointer (gseat);

  gdk_device_get_position (gdevice, &gscreen, x, y);
  if (mods)
    gdk_device_get_state (gdevice,
                          gdk_screen_get_root_window (gscreen),
                          NULL, (GdkModifierType*)mods);
}

static void
get_pointer_position_clutter (int         *x,
                              int         *y,
                              int         *mods)
{
  ClutterDeviceManager *cmanager;
  ClutterInputDevice *cdevice;
  ClutterPoint point;

  cmanager = clutter_device_manager_get_default ();
  cdevice = clutter_device_manager_get_core_device (cmanager, CLUTTER_POINTER_DEVICE);

  clutter_input_device_get_coords (cdevice, NULL, &point);
  if (x)
    *x = point.x;
  if (y)
    *y = point.y;
  if (mods)
    *mods = clutter_input_device_get_modifier_state (cdevice);
}

void
meta_cursor_tracker_get_pointer (MetaCursorTracker   *tracker,
                                 int                 *x,
                                 int                 *y,
                                 ClutterModifierType *mods)
{
  /* We can't use the clutter interface when not running as a wayland compositor,
     because we need to query the server, rather than using the last cached value.
     OTOH, on wayland we can't use GDK, because that only sees the events
     we forward to xwayland.
  */
  if (meta_is_wayland_compositor ())
    get_pointer_position_clutter (x, y, (int*)mods);
  else
    get_pointer_position_gdk (x, y, (int*)mods);
}

void
meta_cursor_tracker_set_pointer_visible (MetaCursorTracker *tracker,
                                         gboolean           visible)
{
  if (visible == tracker->is_showing)
    return;
  tracker->is_showing = visible;

  sync_cursor (tracker);
}

MetaCursorSprite *
meta_cursor_tracker_get_displayed_cursor (MetaCursorTracker *tracker)
{
  return tracker->displayed_cursor;
}
