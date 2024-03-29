/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 *  nsc-gstreamer.c
 * 
 *  Copyright (C) 2008-2010 Brian Pepple
 *  Copyright (C) 2003-2007 Ross Burton <ross@burtonini.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public
 *  License along with this library; if not, write to the Free
 *  Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *  Author: Ross Burton <ross@burtonini.com>
 *          Brian Pepple <bpepple@fedoraproject.org>
 * 
 */

#include <config.h>

#include <string.h>
#include <glib/gerror.h>
#include <glib/gtypes.h>
#include <glib/gi18n.h>
#include <glib-object.h>
#include <gst/gst.h>
#include <profiles/gnome-media-profiles.h>

#include "nsc-error.h"
#include "nsc-gstreamer.h"

/* Properties */
enum {
	PROP_0,
	PROP_PROFILE,
};

/* Signals */
enum {
	PROGRESS,
	DURATION,
	COMPLETION,
	ERROR,
	LAST_SIGNAL,
};

static guint signals[LAST_SIGNAL] = { 0 };

/* Element names */
#define FILE_SOURCE "giosrc"
#define FILE_SINK   "giosink"

struct NscGStreamerPrivate {
	/* The current audio profile */
	GMAudioProfile *profile;

	/* If the pipeline needs to be re-created */
	gboolean        rebuild_pipeline;

	/* The gstreamer pipline elements */
	GstElement     *pipeline;
	GstElement     *filesrc;
	GstElement     *decode;
	GstElement     *encode;
	GstElement     *filesink;

	/* Misc */
	int             seconds;
	GError         *construct_error;
	guint           tick_id;
};

/*
 * GObject methods
 */
G_DEFINE_TYPE (NscGStreamer, nsc_gstreamer, G_TYPE_OBJECT);

#define NSC_GSTREAMER_GET_PRIVATE(o)                           \
	((NscGStreamerPrivate *)((NSC_GSTREAMER(o))->priv))

static void
nsc_gstreamer_set_property (GObject      *object,
			    guint         property_id,
			    const GValue *value,
			    GParamSpec   *pspec)
{
	NscGStreamer        *self = NSC_GSTREAMER (object);
	NscGStreamerPrivate *priv = NSC_GSTREAMER_GET_PRIVATE (self);

	switch (property_id) {
	case PROP_PROFILE:
		if (priv->profile)
			g_object_unref (priv->profile);

		priv->profile = GM_AUDIO_PROFILE (g_value_dup_object (value));
		priv->rebuild_pipeline = TRUE;

		g_object_notify (object, "profile");
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
	}
}

static void
nsc_gstreamer_get_property (GObject    *object,
			    guint       property_id,
			    GValue     *value,
			    GParamSpec *pspec)
{
	NscGStreamer        *self = NSC_GSTREAMER (object);
	NscGStreamerPrivate *priv = NSC_GSTREAMER_GET_PRIVATE (self);

	switch (property_id) {
	case PROP_PROFILE:
		g_value_set_object (value, priv->profile);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
	}
}

static void
nsc_gstreamer_dispose (GObject *object)
{
	NscGStreamer        *self = (NscGStreamer *) object;
	NscGStreamerPrivate *priv = NSC_GSTREAMER_GET_PRIVATE (self);

	/* Check if not NULL! To avoid calling dispose multiple times */
	if (priv != NULL) {
		if (priv->profile) {
			g_object_unref (priv->profile);
			priv->profile = NULL;
		}

		if (priv->pipeline) {
			gst_element_set_state (priv->pipeline, GST_STATE_NULL);
			g_object_unref (priv->pipeline);
			priv->pipeline = NULL;
		}
	}

	G_OBJECT_CLASS (nsc_gstreamer_parent_class)->dispose (object);
}

static void
nsc_gstreamer_finalize (GObject *object)
{
	NscGStreamer        *self = (NscGStreamer *) object;
	NscGStreamerPrivate *priv = NSC_GSTREAMER_GET_PRIVATE (self);

	if (priv != NULL) {
		if (priv->tick_id)
			g_source_remove (priv->tick_id);

		if (priv->construct_error)
			g_error_free (priv->construct_error);


		g_free (priv);

		(NSC_GSTREAMER (self))->priv = NULL;
	}

	G_OBJECT_CLASS (nsc_gstreamer_parent_class)->finalize (object);
}

static void
nsc_gstreamer_class_init (NscGStreamerClass *klass)
{
	GObjectClass *object_class;
	object_class = (GObjectClass *)klass;

	g_type_class_add_private (klass, sizeof (NscGStreamerPrivate));

	/* GObject */
	object_class->set_property = nsc_gstreamer_set_property;
	object_class->get_property = nsc_gstreamer_get_property;
	object_class->dispose      = nsc_gstreamer_dispose;
	object_class->finalize     = nsc_gstreamer_finalize;

	/* Properties */
	g_object_class_install_property (object_class, PROP_PROFILE,
					 g_param_spec_object ("profile",
							      _("Audio Profile"),
							      _("The GNOME Audio Profile used for encoding audio"),
							      GM_AUDIO_TYPE_PROFILE,
							      G_PARAM_READWRITE));

	/* Signals */
	signals[PROGRESS] = 
		g_signal_new ("progress",
			      G_TYPE_FROM_CLASS (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (NscGStreamerClass, progress),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__INT,
			      G_TYPE_NONE, 1, G_TYPE_INT);
	signals[DURATION] = 
		g_signal_new ("duration",
			      G_TYPE_FROM_CLASS (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (NscGStreamerClass, duration),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__INT,
			      G_TYPE_NONE, 1, G_TYPE_INT);
	signals[COMPLETION] =
		g_signal_new ("completion",
			      G_TYPE_FROM_CLASS (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (NscGStreamerClass, completion),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__VOID,
			      G_TYPE_NONE, 0);
	signals[ERROR] = 
		g_signal_new ("error",
			      G_TYPE_FROM_CLASS (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (NscGStreamerClass, error),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__POINTER,
			      G_TYPE_NONE, 1, G_TYPE_POINTER);
}

static void
nsc_gstreamer_init (NscGStreamer *self)
{
	/* Allocate Private data structure */
	(NSC_GSTREAMER (self))->priv = \
		(NscGStreamerPrivate *) g_malloc0 (sizeof (NscGStreamerPrivate));

	/* If correctly allocated, initialize parameters */
	if ((NSC_GSTREAMER (self))->priv != NULL) {
		NscGStreamerPrivate *priv = NSC_GSTREAMER_GET_PRIVATE (self);
		/* Initialize private data */
		priv->rebuild_pipeline = TRUE;
	}
}

/* 
 * Private Methods
 */
static void
eos_cb (GstBus     *bus,
	GstMessage *message,
	gpointer    user_data)
{
	NscGStreamer        *gstreamer;
	NscGStreamerPrivate *priv;

	gstreamer = NSC_GSTREAMER (user_data);
	priv = NSC_GSTREAMER_GET_PRIVATE (gstreamer);

	gst_element_set_state (priv->pipeline, GST_STATE_NULL);

	if (priv->tick_id) {
		g_source_remove (priv->tick_id);
		priv->tick_id = 0;
	}

	priv->rebuild_pipeline = TRUE;

	g_signal_emit (gstreamer, signals[COMPLETION], 0);
}

static GstElement*
build_encoder (NscGStreamer *gstreamer)
{
	NscGStreamerPrivate *priv;
	GstElement          *element = NULL;
	gchar               *pipeline;

	g_return_val_if_fail (NSC_IS_GSTREAMER (gstreamer), NULL);

	priv = NSC_GSTREAMER_GET_PRIVATE (gstreamer);
	g_return_val_if_fail (priv->profile != NULL, NULL);

	pipeline = g_strdup_printf ("audioconvert ! audioresample ! %s",
				    gm_audio_profile_get_pipeline (priv->profile));
	element = gst_parse_bin_from_description (pipeline, TRUE, NULL);
	g_free (pipeline);

	return element;
}

static void
error_cb (GstBus     *bus,
	  GstMessage *message,
	  gpointer    user_data)
{
	NscGStreamer        *gstreamer;
	NscGStreamerPrivate *priv;
	GError              *error = NULL;

	gstreamer = NSC_GSTREAMER (user_data);
	priv = NSC_GSTREAMER_GET_PRIVATE (gstreamer);

	/* Make sure the pipeline is not running any more */
	gst_element_set_state (priv->pipeline, GST_STATE_NULL);
	priv->rebuild_pipeline = TRUE;

	if (priv->tick_id) {
		g_source_remove (priv->tick_id);
		priv->tick_id = 0;
	}

	gst_message_parse_error (message, &error, NULL);
	g_signal_emit (gstreamer, signals[ERROR], 0, error);
	g_error_free (error);
}

static gboolean
just_say_yes (GstElement *element,
	      gpointer    filename,
	      gpointer    user_data)
{
	return TRUE;
}

/* Callback for when decodebin exposes a source pad */
static void
connect_decodebin_cb (GstElement *decodebin,
		      GstPad     *pad,
		      gboolean    last,
		      gpointer    data)
{
        GstPad *audiopad;

        /* Only link once */
        audiopad = gst_element_get_pad (data, "sink");
        if (GST_PAD_IS_LINKED (audiopad)) {
                g_object_unref (audiopad);
                return;
        }

        if (gst_pad_link (pad, audiopad) != GST_PAD_LINK_OK) {
                g_print ("Failed to link elements decodebin-encode\n");
        }
}


static void
build_pipeline (NscGStreamer *gstreamer)
{
	NscGStreamerPrivate *priv;
	GstBus              *bus;

	g_return_if_fail (NSC_IS_GSTREAMER (gstreamer));

	priv = NSC_GSTREAMER_GET_PRIVATE (gstreamer);

	if (priv->pipeline != NULL) {
		gst_object_unref (GST_OBJECT (priv->pipeline));
	}

	priv->pipeline = gst_pipeline_new ("pipeline");
	bus = gst_element_get_bus (priv->pipeline);
	gst_bus_add_signal_watch (bus);

	/* Connect the signals we want to listen to on the bus */
	g_signal_connect (G_OBJECT (bus), "message::error",
			  G_CALLBACK (error_cb),
			  gstreamer);
	g_signal_connect (G_OBJECT (bus), "message::eos",
			  G_CALLBACK (eos_cb),
			  gstreamer);

	/* Read from disk */
	priv->filesrc = gst_element_factory_make (FILE_SOURCE, "file_src");
	if (priv->filesrc == NULL) {
		g_set_error (&priv->construct_error,
			     NSC_ERROR, NSC_ERROR_INTERNAL_ERROR,
			     _("Could not create GStreamer file input"));
		return;
	}

	/* Decode */
	priv->decode = gst_element_factory_make ("decodebin", "decode");
	if (priv->decode == NULL) {
		g_set_error (&priv->construct_error,
			     NSC_ERROR, NSC_ERROR_INTERNAL_ERROR,
			     _("Could not create GStreamer file input"));
		return;
	}

	/* Encode */
	priv->encode = build_encoder (gstreamer);
	if (priv->encode == NULL) {
		g_set_error (&priv->construct_error,
			     NSC_ERROR, NSC_ERROR_INTERNAL_ERROR,
			     _("Could not create GStreamer encoders for %s"),
			     gm_audio_profile_get_name (priv->profile));
		return;
	}

	/* Decodebin uses dynamic pads, so lets set up a callback. */
	g_signal_connect (G_OBJECT (priv->decode), "new-decoded-pad",
			  G_CALLBACK (connect_decodebin_cb),
			  priv->encode);

	/* Write to disk */
	priv->filesink = gst_element_factory_make (FILE_SINK, "file_sink");
	if (priv->filesink == NULL) {
		g_set_error (&priv->construct_error,
			     NSC_ERROR, NSC_ERROR_INTERNAL_ERROR,
			     _("Could not create GStreamer file output"));
		return;
	}

	/*
	 * TODO: Eventually, we should ask the user if they want to 
	 *       overwrite any existing file.
	 */
	g_signal_connect (G_OBJECT (priv->filesink), "allow-overwrite",
			  G_CALLBACK (just_say_yes),
			  gstreamer);

	/* Add the elements to the pipeline */
	gst_bin_add_many (GST_BIN (priv->pipeline),
			  priv->filesrc, priv->decode,
			  priv->encode, priv->filesink,
			  NULL);

	/* Link filessrc and decoder */
	if (!gst_element_link_many (priv->filesrc, priv->decode, NULL)) {
		g_set_error (&priv->construct_error,
			     NSC_ERROR, NSC_ERROR_INTERNAL_ERROR,
			     _("Could not link pipeline"));
		return;
	}

	/* Link the rest */
	if (!gst_element_link (priv->encode, priv->filesink)) {
		g_set_error (&priv->construct_error,
			     NSC_ERROR, NSC_ERROR_INTERNAL_ERROR,
			     _("Could not link pipeline"));
		return;
	}

	priv->rebuild_pipeline = FALSE;
}

static gboolean
tick_timeout_cb (NscGStreamer *gstreamer)
{
	NscGStreamerPrivate *priv;
	gint64               nanos;
	gint                 secs;
	GstState             state;
	GstState             pending_state;
	static GstFormat     format = GST_FORMAT_TIME;

	g_return_val_if_fail (NSC_IS_GSTREAMER (gstreamer), FALSE);

	priv = NSC_GSTREAMER_GET_PRIVATE (gstreamer);

	gst_element_get_state (priv->pipeline,
			       &state,
			       &pending_state,
			       0);

	if (state != GST_STATE_PLAYING &&
	    pending_state != GST_STATE_PLAYING) {
		priv->tick_id = 0;
		return FALSE;
	}

	if (!gst_element_query_position (priv->pipeline,
					 &format,
					 &nanos)) {
		g_warning (_("Could not get current file position"));
		return TRUE;
	}

	secs = nanos / GST_SECOND;
	if (secs != priv->seconds) {
		g_signal_emit (gstreamer, signals[PROGRESS], 0, secs);
	}

	return TRUE;
}

/*
 * Public Methods
 */
NscGStreamer *
nsc_gstreamer_new (GMAudioProfile *profile)
{
	return g_object_new (NSC_TYPE_GSTREAMER, "profile", profile, NULL);
}

void
nsc_gstreamer_convert_file (NscGStreamer *gstreamer,
			    GFile        *src,
			    GFile        *sink,
			    GError      **error)
{
	GstStateChangeReturn  state_ret;
	NscGStreamerPrivate  *priv;
	gint64                nanos;
	static GstFormat      format = GST_FORMAT_TIME;

	g_return_if_fail (NSC_IS_GSTREAMER (gstreamer));

	g_return_if_fail (src != NULL);
	g_return_if_fail (sink != NULL);
       
	priv = NSC_GSTREAMER_GET_PRIVATE (gstreamer);
	
	/* See if we need to rebuild the pipeline */
	if (priv->rebuild_pipeline != FALSE) {
		build_pipeline (gstreamer);

		if (priv->construct_error != NULL) {
			g_propagate_error (error, priv->construct_error);
			priv->construct_error = NULL;
			return;
		}
	}

	/* Set the input file */
	gst_element_set_state (priv->filesrc, GST_STATE_NULL);
	g_object_set (G_OBJECT (priv->filesrc),
		      "file", src,
		      NULL);

	/* Set the output filename */
	gst_element_set_state (priv->filesink, GST_STATE_NULL);
	g_object_set (G_OBJECT (priv->filesink),
		      "file", sink,
		      NULL);

	/* Let's get ready to rumble! */
	state_ret = gst_element_set_state (priv->pipeline,
					   GST_STATE_PLAYING);

	if (state_ret == GST_STATE_CHANGE_ASYNC) {
		/* 
		 * Wait for the state change to either complete or fail,
		 * but not for too long just to catch immediate errors.
		 * The rest we'll handle asynchronously.
		 */
		state_ret = gst_element_get_state (priv->pipeline,
						   NULL,
						   NULL,
						   GST_SECOND / 2);
	}

	if (state_ret == GST_STATE_CHANGE_FAILURE) {
		GstMessage *msg;

		msg = gst_bus_poll (GST_ELEMENT_BUS (priv->pipeline),
				    GST_MESSAGE_ERROR,
				    0);

		if (msg) {
			gst_message_parse_error (msg, error, NULL);
			gst_message_unref (msg);
		} else if (error) {
			*error = g_error_new (NSC_ERROR, NSC_ERROR_INTERNAL_ERROR,
					      "Error starting converting pipeline");
		}

		gst_element_set_state (priv->pipeline, GST_STATE_NULL);
		priv->rebuild_pipeline = TRUE;

		return;
	}

	/* Get file duration */
	if (!gst_element_query_duration (priv->pipeline, &format, &nanos)) {
		g_warning (_("Could not get current file duration"));
	} else {
		gint secs;

		secs = nanos / GST_SECOND;
		g_signal_emit (gstreamer, signals[DURATION], 0, secs);
	}

	priv->tick_id = g_timeout_add (250, (GSourceFunc)tick_timeout_cb,
				       gstreamer);
}

void
nsc_gstreamer_cancel_convert (NscGStreamer *gstreamer)
{
	NscGStreamerPrivate *priv;
	GstState             state;
	GFile               *sink_file;
	gchar               *sink_uri;
	GError              *error = NULL;

	g_return_if_fail (NSC_IS_GSTREAMER (gstreamer));

	priv = NSC_GSTREAMER_GET_PRIVATE (gstreamer);

	gst_element_get_state (priv->pipeline,
			       &state,
			       NULL,
			       GST_CLOCK_TIME_NONE);

	if (state != GST_STATE_PLAYING) {
		return;
	}

	gst_element_set_state (priv->pipeline, GST_STATE_NULL);

	/*
	 * Remove the file that was being converted
	 * when the cancel button was pressed.
	 */
	g_object_get (G_OBJECT (priv->filesink),
		      "location", &sink_uri,
		      NULL);

	sink_file = g_file_new_for_uri (sink_uri);
	g_file_delete (sink_file, NULL, &error);

	if (error) {
		g_warning ("Unable to delete file; %s", error->message);
		g_error_free (error);
	}

	if (sink_file)
		g_object_unref (sink_file);

	g_free (sink_uri);

	priv->rebuild_pipeline = TRUE;
}

gboolean
nsc_gstreamer_supports_mp3 (GError **error)
{
	GstElement *element = NULL;

	element = gst_element_factory_make ("mad", "test");
	if (element == NULL) {
		g_set_error (error, NSC_ERROR, NSC_ERROR_INTERNAL_ERROR,
			     _("The plugin necessary for mp3 file access was not found"));
		return FALSE;
	}

	g_object_unref (element);

	return TRUE;
}

gboolean
nsc_gstreamer_supports_wav (GError **error)
{
	GstElement *element = NULL;

	element = gst_element_factory_make ("wavpackenc", "test");
	if (element == NULL) {
		g_set_error (error, NSC_ERROR, NSC_ERROR_INTERNAL_ERROR,
			     _("The plugin necessary for wav file access was not found"));
		return FALSE;
	}

	g_object_unref (element);

	return TRUE;
}

gboolean
nsc_gstreamer_supports_aac (GError **error)
{
	GstElement *element = NULL;

	element = gst_element_factory_make ("ffdemux_mov_mp4_m4a_3gp_3g2_mj2",
					    "test");
	if (element == NULL) {
		g_set_error (error, NSC_ERROR, NSC_ERROR_INTERNAL_ERROR,
			     _("The plugin necessary for aac file access was not found"));
		return FALSE;
	}

	g_object_unref (element);

	return TRUE;
}

gboolean
nsc_gstreamer_supports_musepack (GError **error)
{
	GstElement *element = NULL;

	element = gst_element_factory_make ("musepackdec", "test");
	if (element == NULL) {
		g_set_error (error, NSC_ERROR, NSC_ERROR_INTERNAL_ERROR,
			     _("The plugin necessary for musepack file access was not found"));
		return FALSE;
	}

	g_object_unref (element);

	return TRUE;
}

gboolean
nsc_gstreamer_supports_wma (GError **error)
{
	GstElement *element = NULL;

	element = gst_element_factory_make ("ffdec_wmav2", "test");
	if (element == NULL) {
		g_set_error (error, NSC_ERROR, NSC_ERROR_INTERNAL_ERROR,
			     _("The plugin necessary for wma file access was not found"));
		return FALSE;
	}

	g_object_unref (element);

	return TRUE;
}

gboolean
nsc_gstreamer_supports_profile (GMAudioProfile *profile)
{
	GstElement *element;
	GError     *error = NULL;
	gchar      *pipeline;

	pipeline = g_strdup_printf ("fakesrc ! %s",
				    gm_audio_profile_get_pipeline (profile));
	element = gst_parse_launch (pipeline, &error);
	g_free (pipeline);

	/*
	 * It is possible for both element and error to be non NULL,
	 * so let's check both.
	 */
	if (element) {
		gst_object_unref (GST_OBJECT (element));

		if (error) {
			g_warning ("Profile warning; %s", error->message);
			g_error_free (error);
		}

		return TRUE;
	} else {
		if (error) {
			g_warning ("Profile error: %s", error->message);
			g_error_free (error);
		}

		return FALSE;
	}
}
