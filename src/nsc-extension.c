/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 *  nsc-extension.c
 * 
 *  Copyright (C) 2008 Brian Pepple
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
 *  Author: Brian Pepple <bpepple@fedoraproject.org>
 * 
 */

#include <config.h> /* for GETTEXT_PACKAGE */

#include "nsc-converter.h"
#include "nsc-extension.h"
#include "nsc-gstreamer.h"

#include <libnautilus-extension/nautilus-menu-provider.h>

#include <glib/gi18n-lib.h>
#include <gtk/gtk.h>
#include <gst/gst.h>

#include <string.h> /* For strcmp */

static void   nsc_extension_instance_init  (NscExtension         *sound);
static void   nsc_extension_class_init     (NscExtensionClass    *class);
static GList *nsc_extension_get_file_items (NautilusMenuProvider *provider,
					    GtkWidget            *window,
					    GList                *files);

static GType sound_converter_type = 0;

/*
 * These are the formats we require, so
 * no check of plugin support is needed
 */
static gchar *mime_types[] = {
	"audio/x-flac",
	"audio/x-vorbis+ogg",
	"audio/ogg",
	"audio/x-wav",
	NULL
};

static gboolean
file_is_sound (NautilusFileInfo *file_info)
{
	gchar          *scheme;
	gint            i;
	GError         *error = NULL;

	/* Is this a file? */
	scheme = nautilus_file_info_get_uri_scheme (file_info);

	if (strcmp (scheme, "file") != 0) {
		g_free (scheme);
		return FALSE;
	}
	g_free (scheme);

	for (i = 0; mime_types[i] != NULL; i++)
		if (nautilus_file_info_is_mime_type (file_info, mime_types[i]))
			return TRUE;

	/* Check for mp3 support */
	if (nsc_gstreamer_supports_mp3 (&error)) {
		if (nautilus_file_info_is_mime_type (file_info, "audio/mpeg"))
			return TRUE;
	} else {
		g_error_free (error);
		error = NULL;
	}

	/* Check for aac suppport */
	if (nsc_gstreamer_supports_aac (&error)) {
		if (nautilus_file_info_is_mime_type (file_info, "audio/mp4"))
			return TRUE;
	} else {
		g_error_free (error);
		error = NULL;
	}

	/* Check for Musepack support */
	if (nsc_gstreamer_supports_musepack (&error)) {
		if (nautilus_file_info_is_mime_type (file_info, "audio/x-musepack"))
			return TRUE;
	} else {
		g_error_free (error);
		error = NULL;
	}

	/* Check for wma support */
	if (nsc_gstreamer_supports_wma (&error)) {
		if (nautilus_file_info_is_mime_type (file_info, "audio/x-ms-wma"))
			return TRUE;
	} else {
		g_error_free (error);
		error = NULL;
	}

	return FALSE;
}

static GList *
converter_filter_files (GList *files)
{
	GList *sounds = NULL;
	GList *file = NULL;

	for (file = files; file != NULL; file = file->next) {
		if (file_is_sound (file->data))
			sounds = g_list_prepend (sounds, file->data);
	}

	return sounds;
}

static void
sound_convert_callback (NautilusMenuItem *item,
		        GList            *files)
{
	NscConverter *converter;

	converter = nsc_converter_new (converter_filter_files (files));

	nsc_converter_show_dialog (converter);
}

static GList *
nsc_extension_get_background_items (NautilusMenuProvider  *provider,
				    GtkWidget             *window,
				    NautilusFileInfo      *file_info)
{
	return NULL;
}

static GList *
nsc_extension_get_file_items (NautilusMenuProvider *provider,
			      GtkWidget            *window,
			      GList                *files)
{
	NautilusMenuItem *item;
	GList            *scan = NULL;
	GList            *items = NULL;

	if (files == NULL)
		return NULL;

	for (scan = files; scan; scan = scan->next) {
		if (file_is_sound (scan->data)) {
			item = nautilus_menu_item_new ("NautilusSoundConverter::convert",
                                                       _("_Convert..."),
                                                       _("Convert each selected audio file"),
                                                       "audio-x-generic");

			g_signal_connect (item, "activate",
					  G_CALLBACK (sound_convert_callback),
					  nautilus_file_info_list_copy (files));

			items = g_list_prepend (items, item);
			items = g_list_reverse (items);

			return items;
		}
	}

	return NULL;
}

static void
nsc_extension_menu_provider_iface_init (NautilusMenuProviderIface *iface)
{
	iface->get_background_items = nsc_extension_get_background_items;
	iface->get_file_items = nsc_extension_get_file_items;
}

static void
nsc_extension_instance_init (NscExtension *sound)
{
	/*
	 * Initialize gstreamer, otherwise the
	 * profile chooser won't show any values.
	 */
	gst_init (NULL, NULL);
}

static void
nsc_extension_class_init (NscExtensionClass *class)
{
}

GType
nsc_extension_get_type (void)
{
	return sound_converter_type;
}

void
nsc_extension_register_type (GTypeModule *module)
{
	static const GTypeInfo info = {
		sizeof (NscExtensionClass),
		(GBaseInitFunc) NULL,
		(GBaseFinalizeFunc) NULL,
		(GClassInitFunc) nsc_extension_class_init,
		NULL,
		NULL,
		sizeof (NscExtension),
		0,
		(GInstanceInitFunc) nsc_extension_instance_init,
	};

	static const GInterfaceInfo menu_provider_iface_info = {
		(GInterfaceInitFunc) nsc_extension_menu_provider_iface_init,
		NULL,
		NULL,
	};

	sound_converter_type = g_type_module_register_type (module,
							   G_TYPE_OBJECT,
							   "NscExtension",
							   &info, 0);

	g_type_module_add_interface (module,
				     sound_converter_type,
				     NAUTILUS_TYPE_MENU_PROVIDER,
				     &menu_provider_iface_info);
}
