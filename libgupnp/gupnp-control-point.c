/* 
 * Copyright (C) 2006 OpenedHand Ltd.
 *
 * Author: Jorn Baayen <jorn@openedhand.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include <string.h>

#include "gupnp-control-point.h"

G_DEFINE_TYPE (GUPnPControlPoint,
               gupnp_control_point,
               GSSDP_TYPE_SERVICE_BROWSER);

struct _GUPnPControlPointPrivate {
        GList *devices;
        GList *services;

        GHashTable *doc_cache;
};

enum {
        DEVICE_PROXY_AVAILABLE,
        DEVICE_PROXY_UNAVAILABLE,
        SERVICE_PROXY_AVAILABLE,
        SERVICE_PROXY_UNAVAILABLE,
        SIGNAL_LAST
};

static guint signals[SIGNAL_LAST];

typedef struct {
        xmlDoc *doc;

        int ref_count;
} DescriptionDoc;

void
description_doc_free (DescriptionDoc *doc)
{
        xmlFreeDoc (doc->doc);

        g_slice_free (DescriptionDoc, doc);
}

static void
gupnp_control_point_init (GUPnPControlPoint *control_point)
{
        control_point->priv =
                G_TYPE_INSTANCE_GET_PRIVATE (control_point,
                                             GUPNP_TYPE_CONTROL_POINT,
                                             GUPnPControlPointPrivate);

        control_point->priv->doc_cache =
                g_hash_table_new_full (g_str_hash,
                                       g_str_equal,
                                       g_free,
                                       (GDestroyNotify) description_doc_free);
}

static void
gupnp_control_point_dispose (GObject *object)
{
        GUPnPControlPoint *control_point;

        control_point = GUPNP_CONTROL_POINT (object);

        while (control_point->priv->devices) {
                g_object_unref (control_point->priv->devices->data);
                control_point->priv->devices =
                        g_list_delete_link (control_point->priv->devices,
                                            control_point->priv->devices);
        }

        while (control_point->priv->services) {
                g_object_unref (control_point->priv->services->data);
                control_point->priv->services =
                        g_list_delete_link (control_point->priv->services,
                                            control_point->priv->services);
        }
}

static void
gupnp_control_point_finalize (GObject *object)
{
        GUPnPControlPoint *control_point;

        control_point = GUPNP_CONTROL_POINT (object);

        g_hash_table_destroy (control_point->priv->doc_cache);
}

/**
 * Called when the description document is loaded.
 *
 * Return value: TRUE if a proxy could be created.
 **/
static gboolean
description_loaded (GUPnPControlPoint *control_point,
                    xmlDoc            *doc,
                    const char        *udn,
                    const char        *service_type,
                    const char        *description_url)
{
        GUPnPContext *context;
        gboolean ret;
        char *full_udn;

        context = gupnp_control_point_get_context (control_point);

        ret = FALSE;

        full_udn = g_strdup_printf ("uuid:%s", udn);

        if (service_type) {
                GUPnPServiceProxy *proxy;

                proxy = gupnp_service_proxy_new (context,
                                                 doc,
                                                 full_udn,
                                                 service_type,
                                                 description_url);
                if (proxy) {
                        control_point->priv->services =
                                g_list_prepend (control_point->priv->services,
                                                proxy);

                        g_signal_emit (control_point,
                                       signals[SERVICE_PROXY_AVAILABLE],
                                       0,
                                       proxy);

                        ret = TRUE;
                }
        } else {
                GUPnPDeviceProxy *proxy;

                proxy = gupnp_device_proxy_new (context,
                                                doc,
                                                full_udn,
                                                description_url);
                if (proxy) {
                        control_point->priv->devices =
                                g_list_prepend (control_point->priv->devices,
                                                proxy);

                        g_signal_emit (control_point,
                                       signals[DEVICE_PROXY_AVAILABLE],
                                       0,
                                       proxy);

                        ret = TRUE;
                }
        }

        g_free (full_udn);

        return ret;
}

/**
 * Downloads and parses (or takes from cache) @description_url,
 * creating:
 *  - A #GUPnPDeviceProxy for the device specified by @udn if @service_type
 *    is NULL.
 *  - A #GUPnPServiceProxy for the service of type @service_type from the device
 *    specified by @udn if @service_type is not NULL.
 **/
static void
load_description (GUPnPControlPoint *control_point,
                  const char        *description_url,
                  const char        *udn,
                  const char        *service_type)
{
        DescriptionDoc *doc;

        doc = g_hash_table_lookup (control_point->priv->doc_cache,
                                   description_url);
        if (doc) {
                if (description_loaded (control_point,
                                        doc->doc,
                                        udn,
                                        service_type,
                                        description_url)) {
                        doc->ref_count++;
                }
        } else {
                xmlDoc *xml_doc;

                /* XXX async */
                xml_doc = xmlParseFile (description_url);
                if (xml_doc) {
                        if (description_loaded (control_point,
                                                xml_doc,
                                                udn,
                                                service_type,
                                                description_url)) {
                                doc = g_slice_new (DescriptionDoc);
                                
                                doc->doc       = xml_doc;
                                doc->ref_count = 1;
                                
                                g_hash_table_insert
                                        (control_point->priv->doc_cache,
                                         g_strdup (description_url),
                                         doc);
                        }
                } else
                        g_warning ("Failed to parse %s", description_url);
        }
}

static gboolean
parse_usn (const char *usn,
           char      **udn,
           char      **service_type)
{
        gboolean ret;
        char **bits;
        guint count, i;

        ret = FALSE;

        *udn = *service_type = NULL;

        /* Verify we have a valid USN */
        if (strncmp (usn, "uuid:", strlen ("uuid:"))) {
                g_warning ("Invalid USN: %s", usn);

                return FALSE;
        } 

        /* Parse USN */
        bits = g_strsplit (usn, ":", -1);

        /* Count elements */
        for (count = 0; bits[count]; count++);
        
        if (count < 1) {
                g_warning ("Invalid USN: %s", usn);

        } else if (count == 1) {
                /* uuid:device-UUID */

                *udn = bits[1];

                ret = TRUE;

        } else if (count == 5) {
                /* uuid:device-UUID::upnp:rootdevice */

                if (!strcmp (bits[3], "upnp") &&
                    !strcmp (bits[4], "rootdevice")) {
                        *udn = bits[1];

                        ret = TRUE;
                } else
                        g_warning ("Invalid USN: %s", usn);

        } else if (count == 8) {
                /* uuid:device-IID::urn:domain-name:service/device:type:v */
                
                if (!strcmp (bits[3], "urn")) {
                        if (!strcmp (bits[5], "device")) {
                                *udn = bits[1];

                                ret = TRUE;
                        } else if (!strcmp (bits[5], "service")) {
                                *udn = bits[1];
                                *service_type = bits[6];

                                ret = TRUE;
                        } else
                                g_warning ("Invalid USN: %s", usn);
                } else
                        g_warning ("Invalid USN: %s", usn);

        } else
                g_warning ("Invalid USN: %s", usn);

        for (i = 0; i < count; i++) {
                if ((bits[i] != *udn) &&
                    (bits[i] != *service_type))
                        g_free (bits[i]);
        }

        g_free (bits);

        return ret;
}

static void
gupnp_control_point_service_available (GSSDPServiceBrowser *service_browser,
                                       const char          *usn,
                                       const GList         *locations)
{
        GUPnPControlPoint *control_point;
        char *udn, *service_type;

        control_point = GUPNP_CONTROL_POINT (service_browser);

        /* Verify we have a location */
        if (!locations) {
                g_warning ("No Location header for device with USN %s", usn);
                return;
        }

        /* Parse USN */
        if (!parse_usn (usn, &udn, &service_type))
                return;

        load_description (control_point,
                          locations->data,
                          udn,
                          service_type);

        g_free (udn);
        g_free (service_type);
}

static void
gupnp_control_point_service_unavailable (GSSDPServiceBrowser *service_browser,
                                         const char          *usn)
{
        GUPnPControlPoint *control_point;
        char *udn, *service_type;
        GList *l;
        DescriptionDoc *doc;
        
        control_point = GUPNP_CONTROL_POINT (service_browser);

        /* Parse USN */
        if (!parse_usn (usn, &udn, &service_type))
                return;

        /* Find proxy */
        if (service_type) {
                GUPnPServiceProxy *proxy;
                GUPnPServiceInfo *info;

                proxy = NULL;
                
                for (l = control_point->priv->services; l; l = l->next) {
                        info = GUPNP_SERVICE_INFO (l->data);

                        if ((!strcmp (udn,
                                      gupnp_service_info_get_udn (info))) &&
                            (!strcmp (service_type,
                                      gupnp_service_info_get_service_type
                                                                 (info)))) {
                                proxy = GUPNP_SERVICE_PROXY (l->data);

                                break;
                        }
                }

                if (proxy) {
                        const char *location;

                        g_signal_emit (control_point,
                                       signals[SERVICE_PROXY_UNAVAILABLE],
                                       0,
                                       proxy);

                        location = gupnp_service_info_get_location
                                        (GUPNP_SERVICE_INFO (proxy));

                        doc = g_hash_table_lookup
                                        (control_point->priv->doc_cache,
                                         location);
                        if (doc) {
                                doc->ref_count--;

                                if (!doc->ref_count) {
                                        g_hash_table_remove
                                                (control_point->priv->doc_cache,
                                                 location);
                                }
                        }

                        g_object_unref (proxy);
                }
        } else {
                GUPnPDeviceProxy *proxy;
                GUPnPDeviceInfo *info;

                proxy = NULL;
                
                for (l = control_point->priv->devices; l; l = l->next) {
                        info = GUPNP_DEVICE_INFO (l->data);

                        if (!strcmp (udn,
                                     gupnp_device_info_get_udn (info))) {
                                proxy = GUPNP_DEVICE_PROXY (l->data);

                                break;
                        }
                }

                if (proxy) {
                        const char *location;

                        g_signal_emit (control_point,
                                       signals[DEVICE_PROXY_UNAVAILABLE],
                                       0,
                                       proxy);

                        location = gupnp_device_info_get_location
                                        (GUPNP_DEVICE_INFO (proxy));

                        doc = g_hash_table_lookup
                                        (control_point->priv->doc_cache,
                                         location);
                        if (doc) {
                                doc->ref_count--;

                                if (!doc->ref_count) {
                                        g_hash_table_remove
                                                (control_point->priv->doc_cache,
                                                 location);
                                }
                        }

                        g_object_unref (proxy);
                }
        }

        g_free (udn);
        g_free (service_type);
}

static void
gupnp_control_point_class_init (GUPnPControlPointClass *klass)
{
        GObjectClass *object_class;
        GSSDPServiceBrowserClass *browser_class;

        object_class = G_OBJECT_CLASS (klass);

        object_class->dispose  = gupnp_control_point_dispose;
        object_class->finalize = gupnp_control_point_finalize;

        browser_class = GSSDP_SERVICE_BROWSER_CLASS (klass);

        browser_class->service_available =
                gupnp_control_point_service_available;
        browser_class->service_unavailable =
                gupnp_control_point_service_unavailable;
        
        g_type_class_add_private (klass, sizeof (GUPnPControlPointPrivate));

        signals[DEVICE_PROXY_AVAILABLE] =
                g_signal_new ("device-proxy-available",
                              GUPNP_TYPE_CONTROL_POINT,
                              G_SIGNAL_RUN_LAST,
                              G_STRUCT_OFFSET (GUPnPControlPointClass,
                                               device_proxy_available),
                              NULL,
                              NULL,
                              g_cclosure_marshal_VOID__OBJECT,
                              G_TYPE_NONE,
                              1,
                              GUPNP_TYPE_DEVICE_PROXY);

        signals[DEVICE_PROXY_UNAVAILABLE] =
                g_signal_new ("device-proxy-unavailable",
                              GUPNP_TYPE_CONTROL_POINT,
                              G_SIGNAL_RUN_LAST,
                              G_STRUCT_OFFSET (GUPnPControlPointClass,
                                               device_proxy_unavailable),
                              NULL,
                              NULL,
                              g_cclosure_marshal_VOID__OBJECT,
                              G_TYPE_NONE,
                              1,
                              GUPNP_TYPE_DEVICE_PROXY);

        signals[SERVICE_PROXY_AVAILABLE] =
                g_signal_new ("service-proxy-available",
                              GUPNP_TYPE_CONTROL_POINT,
                              G_SIGNAL_RUN_LAST,
                              G_STRUCT_OFFSET (GUPnPControlPointClass,
                                               service_proxy_available),
                              NULL,
                              NULL,
                              g_cclosure_marshal_VOID__OBJECT,
                              G_TYPE_NONE,
                              1,
                              GUPNP_TYPE_SERVICE_PROXY);

        signals[SERVICE_PROXY_UNAVAILABLE] =
                g_signal_new ("service-proxy-unavailable",
                              GUPNP_TYPE_CONTROL_POINT,
                              G_SIGNAL_RUN_LAST,
                              G_STRUCT_OFFSET (GUPnPControlPointClass,
                                               service_proxy_unavailable),
                              NULL,
                              NULL,
                              g_cclosure_marshal_VOID__OBJECT,
                              G_TYPE_NONE,
                              1,
                              GUPNP_TYPE_SERVICE_PROXY);
}

/**
 * gupnp_control_point_new
 * @ssdp_client: A #GSSDPClient
 * @target: The search target
 *
 * Return value: A new #GUPnPControlPoint object.
 **/
GUPnPControlPoint *
gupnp_control_point_new (GUPnPContext *context,
                         const char   *target)
{
        g_return_val_if_fail (GUPNP_IS_CONTEXT (context), NULL);

        return g_object_new (GUPNP_TYPE_CONTROL_POINT,
                             "client", context,
                             "target", target,
                             NULL);
}

/**
 * gupnp_control_point_get_context
 * @control_point: A #GUPnPControlPoint
 *
 * Return value: The #GUPnPContext associated with @control_point.
 **/
GUPnPContext *
gupnp_control_point_get_context (GUPnPControlPoint *control_point)
{
        GSSDPClient *client;
        
        g_return_val_if_fail (GUPNP_IS_CONTROL_POINT (control_point), NULL);

        client = gssdp_service_browser_get_client
                                (GSSDP_SERVICE_BROWSER (control_point));

        return GUPNP_CONTEXT (client);
}

/**
 * gupnp_control_point_list_device_proxies
 * @control_point: A #GUPnPControlPoint
 *
 * Return value: A #GList of discovered #GUPnPDeviceProxy objects. Do not
 * free the list nor its elements.
 **/
const GList *
gupnp_control_point_list_device_proxies (GUPnPControlPoint *control_point)
{
        g_return_val_if_fail (GUPNP_IS_CONTROL_POINT (control_point), NULL);

        return (const GList *) control_point->priv->devices;
}

/**
 * gupnp_control_point_list_service_proxies
 * @control_point: A #GUPnPControlPoint
 *
 * Return value: A #GList of discovered #GUPnPServiceProxy objects. Do not
 * free the list nor its elements.
 **/
const GList *
gupnp_control_point_list_service_proxies (GUPnPControlPoint *control_point)
{
        g_return_val_if_fail (GUPNP_IS_CONTROL_POINT (control_point), NULL);

        return (const GList *) control_point->priv->services;
}