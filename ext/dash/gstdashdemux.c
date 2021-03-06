/*
 * DASH demux plugin for GStreamer
 *
 * gstdashdemux.c
 *
 * Copyright (C) 2012 Orange
 * 
 * Authors:
 *   David Corvoysier <david.corvoysier@orange.com>
 *   Hamid Zakari <hamid.zakari@gmail.com>
 *
 * Copyright (C) 2013 Smart TV Alliance
 *  Author: Thiago Sousa Santos <thiago.sousa.santos@collabora.com>, Collabora Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library (COPYING); if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */
/**
 * SECTION:element-dashdemux
 *
 * DASH demuxer element.
 * <title>Example launch line</title>
 * |[
 * gst-launch playbin2 uri="http://www-itec.uni-klu.ac.at/ftp/datasets/mmsys12/RedBullPlayStreets/redbull_4s/RedBullPlayStreets_4s_isoffmain_DIS_23009_1_v_2_1c2_2011_08_30.mpd"
 * ]|
 */

/* Implementation notes:
 * 
 * The following section describes how dashdemux works internally.
 * 
 * Introduction:
 * 
 * dashdemux is a "fake" demux, as unlike traditional demux elements, it
 * doesn't split data streams contained in an enveloppe to expose them
 * to downstream decoding elements.
 * 
 * Instead, it parses an XML file called a manifest to identify a set of
 * individual stream fragments it needs to fetch and expose to the actual
 * demux elements that will handle them (this behavior is sometimes 
 * referred as the "demux after a demux" scenario).
 * 
 * For a given section of content, several representations corresponding
 * to different bitrates may be available: dashdemux will select the most
 * appropriate representation based on local conditions (typically the 
 * available bandwidth and the amount of buffering available, capped by
 * a maximum allowed bitrate). 
 * 
 * The representation selection algorithm can be configured using
 * specific properties: max bitrate, min/max buffering, bandwidth ratio.
 * 
 * 
 * General Design:
 * 
 * dashdemux has a single sink pad that accepts the data corresponding 
 * to the manifest, typically fetched from an HTTP or file source.
 * 
 * dashdemux exposes the streams it recreates based on the fragments it
 * fetches through dedicated src pads corresponding to the caps of the
 * fragments container (ISOBMFF/MP4 or MPEG2TS).
 * 
 * During playback, new representations will typically be exposed as a
 * new set of pads (see 'Switching between representations' below).
 * 
 * Fragments downloading is performed using a dedicated task that fills
 * an internal queue. Another task is in charge of popping fragments
 * from the queue and pushing them downstream.
 * 
 * Switching between representations:
 * 
 * Decodebin supports scenarios allowing to seamlessly switch from one 
 * stream to another inside the same "decoding chain".
 * 
 * To achieve that, it combines the elements it autoplugged in chains
 *  and groups, allowing only one decoding group to be active at a given
 * time for a given chain.
 *
 * A chain can signal decodebin that it is complete by sending a 
 * no-more-pads event, but even after that new pads can be added to
 * create new subgroups, providing that a new no-more-pads event is sent.
 *
 * We take advantage of that to dynamically create a new decoding group
 * in order to select a different representation during playback.
 *
 * Typically, assuming that each fragment contains both audio and video,
 * the following tree would be created:
 * 
 * chain "DASH Demux"
 * |_ group "Representation set 1"
 * |   |_ chain "Qt Demux 0"
 * |       |_ group "Stream 0"
 * |           |_ chain "H264"
 * |           |_ chain "AAC"
 * |_ group "Representation set 2"
 *     |_ chain "Qt Demux 1"
 *         |_ group "Stream 1"
 *             |_ chain "H264"
 *             |_ chain "AAC"
 *
 * Or, if audio and video are contained in separate fragments:
 *
 * chain "DASH Demux"
 * |_ group "Representation set 1"
 * |   |_ chain "Qt Demux 0"
 * |   |   |_ group "Stream 0"
 * |   |       |_ chain "H264"
 * |   |_ chain "Qt Demux 1"
 * |       |_ group "Stream 1"
 * |           |_ chain "AAC" 
 * |_ group "Representation set 2"
 *     |_ chain "Qt Demux 3"
 *     |   |_ group "Stream 2"
 *     |       |_ chain "H264"
 *     |_ chain "Qt Demux 4"
 *         |_ group "Stream 3"
 *             |_ chain "AAC" 
 *
 * In both cases, when switching from Set 1 to Set 2 an EOS is sent on
 * each end pad corresponding to Rep 0, triggering the "drain" state to
 * propagate upstream.
 * Once both EOS have been processed, the "Set 1" group is completely
 * drained, and decodebin2 will switch to the "Set 2" group.
 * 
 * Note: nothing can be pushed to the new decoding group before the 
 * old one has been drained, which means that in order to be able to 
 * adapt quickly to bandwidth changes, we will not be able to rely
 * on downstream buffering, and will instead manage an internal queue.
 * 
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <string.h>
#include <inttypes.h>
#include <gst/base/gsttypefindhelper.h>
#include "gst/gst-i18n-plugin.h"
#include "gstdashdemux.h"
#include "gstdash_debug.h"

static GstStaticPadTemplate srctemplate = GST_STATIC_PAD_TEMPLATE ("src_%u",
    GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS_ANY);

static GstStaticPadTemplate sinktemplate = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/dash+xml"));

GST_DEBUG_CATEGORY (gst_dash_demux_debug);
#define GST_CAT_DEFAULT gst_dash_demux_debug

enum
{
  PROP_0,

  PROP_MAX_BUFFERING_TIME,
  PROP_BANDWIDTH_USAGE,
  PROP_MAX_BITRATE,
  PROP_LAST
};

/* Default values for properties */
#define DEFAULT_MAX_BUFFERING_TIME       30     /* in seconds */
#define DEFAULT_BANDWIDTH_USAGE         0.8     /* 0 to 1     */
#define DEFAULT_MAX_BITRATE        24000000     /* in bit/s  */

#define DEFAULT_FAILED_COUNT 3
#define DOWNLOAD_RATE_HISTORY_MAX 3

#define GST_DASH_DEMUX_CLIENT_LOCK(d) g_mutex_lock (&d->client_lock)
#define GST_DASH_DEMUX_CLIENT_UNLOCK(d) g_mutex_unlock (&d->client_lock)

/* GObject */
static void gst_dash_demux_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_dash_demux_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static void gst_dash_demux_dispose (GObject * obj);

/* GstElement */
static GstStateChangeReturn
gst_dash_demux_change_state (GstElement * element, GstStateChange transition);

/* GstDashDemux */
static GstFlowReturn gst_dash_demux_pad (GstPad * pad, GstObject * parent,
    GstBuffer * buf);
static gboolean gst_dash_demux_sink_event (GstPad * pad, GstObject * parent,
    GstEvent * event);
static gboolean gst_dash_demux_src_event (GstPad * pad, GstObject * parent,
    GstEvent * event);
static gboolean gst_dash_demux_src_query (GstPad * pad, GstObject * parent,
    GstQuery * query);
static void gst_dash_demux_stream_download_loop (GstDashDemuxStream * stream);
static void gst_dash_demux_stop (GstDashDemux * demux);
static void gst_dash_demux_wait_stop (GstDashDemux * demux);
static void gst_dash_demux_resume_download_task (GstDashDemux * demux);
static gboolean gst_dash_demux_setup_all_streams (GstDashDemux * demux);
static GstEvent
    * gst_dash_demux_stream_select_representation_unlocked (GstDashDemuxStream *
    stream);
static GstFlowReturn gst_dash_demux_stream_get_next_fragment (GstDashDemuxStream
    * stream, GstClockTime * ts);
static gboolean gst_dash_demux_advance_period (GstDashDemux * demux);
static void gst_dash_demux_download_wait (GstDashDemuxStream * stream,
    GstClockTime time_diff);

static void gst_dash_demux_expose_streams (GstDashDemux * demux);
static void gst_dash_demux_remove_streams (GstDashDemux * demux,
    GSList * streams);
static void gst_dash_demux_stream_free (GstDashDemuxStream * stream);
static void gst_dash_demux_reset (GstDashDemux * demux, gboolean dispose);
static GstCaps *gst_dash_demux_get_input_caps (GstDashDemux * demux,
    GstActiveStream * stream);
static GstPad *gst_dash_demux_create_pad (GstDashDemux * demux);

#define gst_dash_demux_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstDashDemux, gst_dash_demux, GST_TYPE_ELEMENT,
    GST_DEBUG_CATEGORY_INIT (gst_dash_demux_debug, "dashdemux", 0,
        "dashdemux element");
    );

static void
gst_dash_demux_dispose (GObject * obj)
{
  GstDashDemux *demux = GST_DASH_DEMUX (obj);

  gst_dash_demux_reset (demux, TRUE);

  if (demux->downloader != NULL) {
    g_object_unref (demux->downloader);
    demux->downloader = NULL;
  }

  g_mutex_clear (&demux->client_lock);

  G_OBJECT_CLASS (parent_class)->dispose (obj);
}

static void
gst_dash_demux_class_init (GstDashDemuxClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;

  gobject_class->set_property = gst_dash_demux_set_property;
  gobject_class->get_property = gst_dash_demux_get_property;
  gobject_class->dispose = gst_dash_demux_dispose;

  g_object_class_install_property (gobject_class, PROP_MAX_BUFFERING_TIME,
      g_param_spec_uint ("max-buffering-time", "Maximum buffering time",
          "Maximum number of seconds of buffer accumulated during playback",
          2, G_MAXUINT, DEFAULT_MAX_BUFFERING_TIME,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_BANDWIDTH_USAGE,
      g_param_spec_float ("bandwidth-usage",
          "Bandwidth usage [0..1]",
          "Percentage of the available bandwidth to use when selecting representations",
          0, 1, DEFAULT_BANDWIDTH_USAGE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_MAX_BITRATE,
      g_param_spec_uint ("max-bitrate", "Max bitrate",
          "Max of bitrate supported by target decoder",
          1000, G_MAXUINT, DEFAULT_MAX_BITRATE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gstelement_class->change_state =
      GST_DEBUG_FUNCPTR (gst_dash_demux_change_state);

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&srctemplate));

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&sinktemplate));

  gst_element_class_set_static_metadata (gstelement_class,
      "DASH Demuxer",
      "Codec/Demuxer",
      "Dynamic Adaptive Streaming over HTTP demuxer",
      "David Corvoysier <david.corvoysier@orange.com>\n\
                Hamid Zakari <hamid.zakari@gmail.com>\n\
                Gianluca Gennari <gennarone@gmail.com>");
}

static void
gst_dash_demux_init (GstDashDemux * demux)
{
  /* sink pad */
  demux->sinkpad = gst_pad_new_from_static_template (&sinktemplate, "sink");
  gst_pad_set_chain_function (demux->sinkpad,
      GST_DEBUG_FUNCPTR (gst_dash_demux_pad));
  gst_pad_set_event_function (demux->sinkpad,
      GST_DEBUG_FUNCPTR (gst_dash_demux_sink_event));
  gst_element_add_pad (GST_ELEMENT (demux), demux->sinkpad);

  gst_segment_init (&demux->segment, GST_FORMAT_TIME);

  /* Downloader */
  demux->downloader = gst_uri_downloader_new ();

  /* Properties */
  demux->max_buffering_time = DEFAULT_MAX_BUFFERING_TIME * GST_SECOND;
  demux->bandwidth_usage = DEFAULT_BANDWIDTH_USAGE;
  demux->max_bitrate = DEFAULT_MAX_BITRATE;

  g_mutex_init (&demux->client_lock);
}

static void
gst_dash_demux_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstDashDemux *demux = GST_DASH_DEMUX (object);

  switch (prop_id) {
    case PROP_MAX_BUFFERING_TIME:
      demux->max_buffering_time = g_value_get_uint (value) * GST_SECOND;
      break;
    case PROP_BANDWIDTH_USAGE:
      demux->bandwidth_usage = g_value_get_float (value);
      break;
    case PROP_MAX_BITRATE:
      demux->max_bitrate = g_value_get_uint (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_dash_demux_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstDashDemux *demux = GST_DASH_DEMUX (object);

  switch (prop_id) {
    case PROP_MAX_BUFFERING_TIME:
      g_value_set_uint (value, demux->max_buffering_time / GST_SECOND);
      break;
    case PROP_BANDWIDTH_USAGE:
      g_value_set_float (value, demux->bandwidth_usage);
      break;
    case PROP_MAX_BITRATE:
      g_value_set_uint (value, demux->max_bitrate);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static GstStateChangeReturn
gst_dash_demux_change_state (GstElement * element, GstStateChange transition)
{
  GstStateChangeReturn ret;
  GstDashDemux *demux = GST_DASH_DEMUX (element);

  switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      gst_dash_demux_reset (demux, FALSE);
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      break;
    default:
      break;
  }
  return ret;
}

static gboolean
gst_dash_demux_src_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstDashDemux *demux;

  demux = GST_DASH_DEMUX (parent);

  switch (event->type) {
    case GST_EVENT_SEEK:
    {
      gdouble rate;
      GstFormat format;
      GstSeekFlags flags;
      GstSeekType start_type, stop_type;
      gint64 start, stop;
      GList *list;
      GstClockTime current_pos, target_pos;
      guint current_period;
      GstStreamPeriod *period;
      GSList *iter;
      gboolean update;

      GST_INFO_OBJECT (demux, "Received seek event");

      if (gst_mpd_client_is_live (demux->client)) {
        GST_WARNING_OBJECT (demux, "Received seek event for live stream");
        return FALSE;
      }

      gst_event_parse_seek (event, &rate, &format, &flags, &start_type, &start,
          &stop_type, &stop);

      if (format != GST_FORMAT_TIME)
        return FALSE;

      GST_DEBUG_OBJECT (demux,
          "seek event, rate: %f type: %d start: %" GST_TIME_FORMAT " stop: %"
          GST_TIME_FORMAT, rate, start_type, GST_TIME_ARGS (start),
          GST_TIME_ARGS (stop));

      gst_segment_do_seek (&demux->segment, rate, format, flags, start_type,
          start, stop_type, stop, &update);

      if (update) {
        GstEvent *seg_evt;

        GST_DASH_DEMUX_CLIENT_LOCK (demux);

        if (flags & GST_SEEK_FLAG_FLUSH) {
          GST_DEBUG_OBJECT (demux, "sending flush start");
          for (iter = demux->streams; iter; iter = g_slist_next (iter)) {
            GstDashDemuxStream *stream;
            stream = iter->data;
            gst_pad_push_event (stream->pad, gst_event_new_flush_start ());
          }
        }

        gst_dash_demux_stop (demux);
        GST_DASH_DEMUX_CLIENT_UNLOCK (demux);
        gst_dash_demux_wait_stop (demux);

        /* select the requested Period in the Media Presentation */
        target_pos = (GstClockTime) demux->segment.start;
        GST_DEBUG_OBJECT (demux, "Seeking to target %" GST_TIME_FORMAT,
            GST_TIME_ARGS (target_pos));
        current_period = 0;
        for (list = g_list_first (demux->client->periods); list;
            list = g_list_next (list)) {
          period = list->data;
          current_pos = period->start;
          current_period = period->number;
          GST_DEBUG_OBJECT (demux, "Looking at period %u pos %" GST_TIME_FORMAT,
              current_period, GST_TIME_ARGS (current_pos));
          if (current_pos <= target_pos
              && target_pos < current_pos + period->duration) {
            break;
          }
        }
        if (list == NULL) {
          GST_WARNING_OBJECT (demux, "Could not find seeked Period");
          return FALSE;
        }
        if (current_period != gst_mpd_client_get_period_index (demux->client)) {
          GSList *streams = NULL;

          GST_DEBUG_OBJECT (demux, "Seeking to Period %d", current_period);
          streams = demux->streams;
          demux->streams = NULL;
          /* clean old active stream list, if any */
          gst_active_streams_free (demux->client);

          /* setup video, audio and subtitle streams, starting from the new Period */
          if (!gst_mpd_client_set_period_index (demux->client, current_period)
              || !gst_dash_demux_setup_all_streams (demux))
            return FALSE;

          gst_dash_demux_expose_streams (demux);
          gst_dash_demux_remove_streams (demux, streams);
        }

        /* Update the current sequence on all streams */
        seg_evt = gst_event_new_segment (&demux->segment);
        gst_event_set_seqnum (seg_evt, gst_event_get_seqnum (event));
        for (iter = demux->streams; iter; iter = g_slist_next (iter)) {
          GstDashDemuxStream *stream = iter->data;
          gst_mpd_client_stream_seek (demux->client, stream->active_stream,
              target_pos);

          gst_event_replace (&stream->pending_segment, seg_evt);
        }
        gst_event_unref (seg_evt);

        if (flags & GST_SEEK_FLAG_FLUSH) {
          GST_DEBUG_OBJECT (demux, "Sending flush stop on all pad");
          for (iter = demux->streams; iter; iter = g_slist_next (iter)) {
            GstDashDemuxStream *stream;

            stream = iter->data;
            stream->need_header = TRUE;
            stream->stream_eos = FALSE;
            gst_pad_push_event (stream->pad, gst_event_new_flush_stop (TRUE));
          }
        }

        /* Restart the demux */
        GST_DASH_DEMUX_CLIENT_LOCK (demux);
        demux->cancelled = FALSE;
        demux->end_of_manifest = FALSE;
        for (iter = demux->streams; iter; iter = g_slist_next (iter)) {
          GstDashDemuxStream *stream = iter->data;
          stream->last_ret = GST_FLOW_OK;
          gst_uri_downloader_reset (stream->downloader);
        }
        demux->timestamp_offset = 0;
        gst_uri_downloader_reset (demux->downloader);
        GST_DEBUG_OBJECT (demux, "Resuming tasks after seeking");
        gst_dash_demux_resume_download_task (demux);
        GST_DASH_DEMUX_CLIENT_UNLOCK (demux);
      }

      return TRUE;
    }
    case GST_EVENT_RECONFIGURE:{
      GSList *iter;

      GST_DASH_DEMUX_CLIENT_LOCK (demux);
      for (iter = demux->streams; iter; iter = g_slist_next (iter)) {
        GstDashDemuxStream *stream = iter->data;

        if (stream->pad == pad) {
          if (stream->last_ret == GST_FLOW_NOT_LINKED) {
            stream->last_ret = GST_FLOW_OK;
            stream->restart_download = TRUE;
            stream->need_header = TRUE;
            GST_DEBUG_OBJECT (stream->pad, "Restarting download loop");
            gst_task_start (stream->download_task);
          }
          GST_DASH_DEMUX_CLIENT_UNLOCK (demux);
          gst_event_unref (event);
          return TRUE;
        }
      }
      GST_DASH_DEMUX_CLIENT_UNLOCK (demux);
    }
      break;
    default:
      break;
  }

  return gst_pad_event_default (pad, parent, event);
}

static gboolean
gst_dash_demux_setup_mpdparser_streams (GstDashDemux * demux,
    GstMpdClient * client)
{
  gboolean has_streams = FALSE;
  GList *adapt_sets, *iter;

  adapt_sets = gst_mpd_client_get_adaptation_sets (client);
  for (iter = adapt_sets; iter; iter = g_list_next (iter)) {
    GstAdaptationSetNode *adapt_set_node = iter->data;

    gst_mpd_client_setup_streaming (client, adapt_set_node);
    has_streams = TRUE;
  }

  if (!has_streams) {
    GST_ELEMENT_ERROR (demux, STREAM, DEMUX, ("Manifest has no playable "
            "streams"), ("No streams could be activated from the manifest"));
  }
  return has_streams;
}

static gboolean
gst_dash_demux_all_streams_eop (GstDashDemux * demux)
{
  GSList *iter = NULL;

  GST_DEBUG_OBJECT (demux, "Checking if all streams are EOP");

  for (iter = demux->streams; iter; iter = g_slist_next (iter)) {
    GstDashDemuxStream *stream = iter->data;

    GST_LOG_OBJECT (stream->pad, "EOP: %d (linked: %d)", stream->stream_eos,
        stream->last_ret != GST_FLOW_NOT_LINKED);

    if (!stream->stream_eos && stream->last_ret != GST_FLOW_NOT_LINKED)
      return FALSE;
  }

  return TRUE;
}

static gboolean
gst_dash_demux_setup_all_streams (GstDashDemux * demux)
{
  guint i;
  GSList *streams = NULL;

  GST_DEBUG_OBJECT (demux, "Setting up streams for period %d",
      gst_mpd_client_get_period_index (demux->client));

  GST_MPD_CLIENT_LOCK (demux->client);
  /* clean old active stream list, if any */
  gst_active_streams_free (demux->client);

  if (!gst_dash_demux_setup_mpdparser_streams (demux, demux->client)) {
    return FALSE;
  }

  GST_DEBUG_OBJECT (demux, "Creating stream objects");
  for (i = 0; i < gst_mpdparser_get_nb_active_stream (demux->client); i++) {
    GstDashDemuxStream *stream;
    GstActiveStream *active_stream;
    GstCaps *caps;
    GstEvent *event;
    gchar *stream_id;

    active_stream = gst_mpdparser_get_active_stream_by_index (demux->client, i);
    if (active_stream == NULL)
      continue;

    stream = g_new0 (GstDashDemuxStream, 1);
    stream->demux = demux;
    stream->active_stream = active_stream;
    caps = gst_dash_demux_get_input_caps (demux, active_stream);

    g_rec_mutex_init (&stream->download_task_lock);
    stream->download_task =
        gst_task_new ((GstTaskFunction) gst_dash_demux_stream_download_loop,
        stream, NULL);
    gst_task_set_lock (stream->download_task, &stream->download_task_lock);
    g_cond_init (&stream->download_cond);
    g_mutex_init (&stream->download_mutex);
    stream->downloader = gst_uri_downloader_new ();

    stream->index = i;
    stream->input_caps = caps;
    stream->need_header = TRUE;
    gst_download_rate_init (&stream->dnl_rate);
    gst_download_rate_set_max_length (&stream->dnl_rate,
        DOWNLOAD_RATE_HISTORY_MAX);

    GST_LOG_OBJECT (demux, "Creating stream %d %" GST_PTR_FORMAT, i, caps);
    streams = g_slist_prepend (streams, stream);
    stream->pad = gst_dash_demux_create_pad (demux);

    stream_id =
        gst_pad_create_stream_id_printf (stream->pad,
        GST_ELEMENT_CAST (demux), "%d", i);

    event =
        gst_pad_get_sticky_event (demux->sinkpad, GST_EVENT_STREAM_START, 0);
    if (event) {
      if (gst_event_parse_group_id (event, &demux->group_id))
        demux->have_group_id = TRUE;
      else
        demux->have_group_id = FALSE;
      gst_event_unref (event);
    } else if (!demux->have_group_id) {
      demux->have_group_id = TRUE;
      demux->group_id = gst_util_group_id_next ();
    }
    event = gst_event_new_stream_start (stream_id);
    if (demux->have_group_id)
      gst_event_set_group_id (event, demux->group_id);

    gst_pad_push_event (stream->pad, event);
    g_free (stream_id);

    gst_pad_push_event (stream->pad, gst_event_new_caps (caps));
  }
  streams = g_slist_reverse (streams);

  demux->next_periods = g_slist_append (demux->next_periods, streams);
  GST_MPD_CLIENT_UNLOCK (demux->client);

  return TRUE;
}

static gboolean
gst_dash_demux_sink_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstDashDemux *demux = GST_DASH_DEMUX (parent);

  switch (event->type) {
    case GST_EVENT_FLUSH_STOP:
      gst_dash_demux_reset (demux, FALSE);
      break;
    case GST_EVENT_EOS:{
      gchar *manifest;
      GstQuery *query;
      gboolean query_res;
      gboolean ret = TRUE;
      GstMapInfo mapinfo;

      if (demux->manifest == NULL) {
        GST_WARNING_OBJECT (demux, "Received EOS without a manifest.");
        break;
      }

      GST_DEBUG_OBJECT (demux, "Got EOS on the sink pad: manifest fetched");

      GST_DASH_DEMUX_CLIENT_LOCK (demux);
      if (demux->client)
        gst_mpd_client_free (demux->client);
      demux->client = gst_mpd_client_new ();

      query = gst_query_new_uri ();
      query_res = gst_pad_peer_query (pad, query);
      if (query_res) {
        gst_query_parse_uri (query, &demux->client->mpd_uri);
        GST_DEBUG_OBJECT (demux, "Fetched MPD file at URI: %s",
            demux->client->mpd_uri);
      } else {
        GST_WARNING_OBJECT (demux, "MPD URI query failed.");
      }
      gst_query_unref (query);

      if (gst_buffer_map (demux->manifest, &mapinfo, GST_MAP_READ)) {
        manifest = (gchar *) mapinfo.data;
        if (!gst_mpd_parse (demux->client, manifest, mapinfo.size)) {
          /* In most cases, this will happen if we set a wrong url in the
           * source element and we have received the 404 HTML response instead of
           * the manifest */
          GST_ELEMENT_ERROR (demux, STREAM, DECODE, ("Invalid manifest."),
              (NULL));
          ret = FALSE;
        }
        gst_buffer_unmap (demux->manifest, &mapinfo);
      } else {
        GST_WARNING_OBJECT (demux, "Error validating the manifest.");
        ret = FALSE;
      }
      gst_buffer_unref (demux->manifest);
      demux->manifest = NULL;

      if (!ret)
        goto seek_quit;

      if (!gst_mpd_client_setup_media_presentation (demux->client)) {
        GST_ELEMENT_ERROR (demux, STREAM, DECODE,
            ("Incompatible manifest file."), (NULL));
        ret = FALSE;
        goto seek_quit;
      }

      /* setup video, audio and subtitle streams, starting from first Period */
      if (!gst_mpd_client_set_period_index (demux->client, 0) ||
          !gst_dash_demux_setup_all_streams (demux)) {
        ret = FALSE;
        goto seek_quit;
      }

      gst_dash_demux_advance_period (demux);

      /* If stream is live, try to find the segment that is closest to current time */
      if (gst_mpd_client_is_live (demux->client)) {
        GSList *iter;
        GstDateTime *now = gst_date_time_new_now_utc ();
        gint seg_idx;

        GST_DEBUG_OBJECT (demux,
            "Seeking to current time of day for live stream ");
        if (demux->client->mpd_node->suggestedPresentationDelay != -1) {
          GstDateTime *target = gst_mpd_client_add_time_difference (now,
              demux->client->mpd_node->suggestedPresentationDelay * -1000);
          gst_date_time_unref (now);
          now = target;
        }
        for (iter = demux->streams; iter; iter = g_slist_next (iter)) {
          GstDashDemuxStream *stream = iter->data;
          GstActiveStream *active_stream = stream->active_stream;

          /* Get segment index corresponding to current time. */
          seg_idx =
              gst_mpd_client_get_segment_index_at_time (demux->client,
              active_stream, now);
          if (seg_idx < 0) {
            GST_WARNING_OBJECT (demux,
                "Failed to find a segment that is available "
                "at this point in time for stream %d.", stream->index);
            seg_idx = 0;
          }
          GST_INFO_OBJECT (demux,
              "Segment index corresponding to current time for stream "
              "%d is %d.", stream->index, seg_idx);
          gst_mpd_client_set_segment_index (active_stream, seg_idx);
        }

        gst_date_time_unref (now);
      } else {
        GST_DEBUG_OBJECT (demux,
            "Seeking to first segment for on-demand stream ");

        /* start playing from the first segment */
        gst_mpd_client_set_segment_index_for_all_streams (demux->client, 0);
      }

      /* Send duration message */
      if (!gst_mpd_client_is_live (demux->client)) {
        GstClockTime duration =
            gst_mpd_client_get_media_presentation_duration (demux->client);

        if (duration != GST_CLOCK_TIME_NONE) {
          GST_DEBUG_OBJECT (demux,
              "Sending duration message : %" GST_TIME_FORMAT,
              GST_TIME_ARGS (duration));
          gst_element_post_message (GST_ELEMENT (demux),
              gst_message_new_duration_changed (GST_OBJECT (demux)));
        } else {
          GST_DEBUG_OBJECT (demux,
              "mediaPresentationDuration unknown, can not send the duration message");
        }
      }
      demux->timestamp_offset = -1;
      gst_dash_demux_resume_download_task (demux);
      GST_DASH_DEMUX_CLIENT_UNLOCK (demux);

    seek_quit:
      gst_event_unref (event);
      return ret;
    }
    case GST_EVENT_SEGMENT:
      /* Swallow newsegments, we'll push our own */
      gst_event_unref (event);
      return TRUE;
    default:
      break;
  }

  return gst_pad_event_default (pad, parent, event);
}

static gboolean
gst_dash_demux_src_query (GstPad * pad, GstObject * parent, GstQuery * query)
{
  GstDashDemux *dashdemux;
  gboolean ret = FALSE;

  if (query == NULL)
    return FALSE;

  dashdemux = GST_DASH_DEMUX (parent);

  switch (query->type) {
    case GST_QUERY_DURATION:{
      GstClockTime duration = -1;
      GstFormat fmt;

      gst_query_parse_duration (query, &fmt, NULL);
      if (fmt == GST_FORMAT_TIME) {
        duration =
            gst_mpd_client_get_media_presentation_duration (dashdemux->client);
        if (GST_CLOCK_TIME_IS_VALID (duration) && duration > 0) {
          gst_query_set_duration (query, GST_FORMAT_TIME, duration);
          ret = TRUE;
        }
      }
      GST_DEBUG_OBJECT (dashdemux,
          "GST_QUERY_DURATION returns %s with duration %" GST_TIME_FORMAT,
          ret ? "TRUE" : "FALSE", GST_TIME_ARGS (duration));
      break;
    }
    case GST_QUERY_SEEKING:{
      GstFormat fmt;
      gint64 start;
      gint64 end;
      gint64 stop = -1;

      gst_query_parse_seeking (query, &fmt, NULL, &start, &end);
      GST_DEBUG_OBJECT (dashdemux,
          "Received GST_QUERY_SEEKING with format %d - %" G_GINT64_FORMAT
          " %" G_GINT64_FORMAT, fmt, start, end);
      if (fmt == GST_FORMAT_TIME) {
        GstClockTime duration;

        duration =
            gst_mpd_client_get_media_presentation_duration (dashdemux->client);
        if (GST_CLOCK_TIME_IS_VALID (duration) && duration > 0)
          stop = duration;

        gst_query_set_seeking (query, fmt,
            !gst_mpd_client_is_live (dashdemux->client), 0, stop);
        ret = TRUE;

        GST_DEBUG_OBJECT (dashdemux, "GST_QUERY_SEEKING returning with stop : %"
            GST_TIME_FORMAT, GST_TIME_ARGS (stop));
      }
      break;
    }
    case GST_QUERY_LATENCY:
    {
      gboolean live;
      GstClockTime min, max;

      gst_query_parse_latency (query, &live, &min, &max);

      if (dashdemux->client && gst_mpd_client_is_live (dashdemux->client))
        live = TRUE;

      if (dashdemux->max_buffering_time > 0)
        max += dashdemux->max_buffering_time;

      gst_query_set_latency (query, live, min, max);
      ret = TRUE;
      break;
    }
    default:{
      /* By default, do not forward queries upstream */
      break;
    }
  }

  return ret;
}

static GstFlowReturn
gst_dash_demux_pad (GstPad * pad, GstObject * parent, GstBuffer * buf)
{
  GstDashDemux *demux = GST_DASH_DEMUX (parent);

  if (demux->manifest == NULL)
    demux->manifest = buf;
  else
    demux->manifest = gst_buffer_append (demux->manifest, buf);

  return GST_FLOW_OK;
}

static void
gst_dash_demux_wait_stop (GstDashDemux * demux)
{
  GSList *iter;

  GST_DEBUG_OBJECT (demux, "Waiting for threads to stop");
  for (iter = demux->streams; iter; iter = g_slist_next (iter)) {
    GstDashDemuxStream *stream = iter->data;

    gst_task_join (stream->download_task);
  }
}

static void
gst_dash_demux_stop (GstDashDemux * demux)
{
  GSList *iter;

  GST_DEBUG_OBJECT (demux, "Stopping demux");
  demux->cancelled = TRUE;

  if (demux->downloader)
    gst_uri_downloader_cancel (demux->downloader);

  for (iter = demux->streams; iter; iter = g_slist_next (iter)) {
    GstDashDemuxStream *stream = iter->data;

    stream->last_ret = GST_FLOW_FLUSHING;
    stream->need_header = TRUE;
    gst_task_stop (stream->download_task);
    GST_TASK_SIGNAL (stream->download_task);
    gst_uri_downloader_cancel (stream->downloader);
  }
}

static GstPad *
gst_dash_demux_create_pad (GstDashDemux * demux)
{
  GstPad *pad;

  /* Create and activate new pads */
  pad = gst_pad_new_from_static_template (&srctemplate, NULL);
  gst_pad_set_event_function (pad,
      GST_DEBUG_FUNCPTR (gst_dash_demux_src_event));
  gst_pad_set_query_function (pad,
      GST_DEBUG_FUNCPTR (gst_dash_demux_src_query));
  gst_pad_set_element_private (pad, demux);
  gst_pad_set_active (pad, TRUE);
  GST_INFO_OBJECT (demux, "Creating srcpad %s:%s", GST_DEBUG_PAD_NAME (pad));
  return pad;
}

static void
gst_dash_demux_expose_streams (GstDashDemux * demux)
{
  GSList *iter;

  for (iter = demux->streams; iter; iter = g_slist_next (iter)) {
    GstDashDemuxStream *stream = iter->data;

    GST_LOG_OBJECT (stream->pad, "Exposing stream %d %" GST_PTR_FORMAT,
        stream->index, stream->input_caps);

    gst_element_add_pad (GST_ELEMENT (demux), gst_object_ref (stream->pad));
  }
  gst_element_no_more_pads (GST_ELEMENT_CAST (demux));
}

static void
gst_dash_demux_remove_streams (GstDashDemux * demux, GSList * streams)
{
  GSList *iter;
  GstEvent *eos = gst_event_new_eos ();

  for (iter = streams; iter; iter = g_slist_next (iter)) {
    GstDashDemuxStream *stream = iter->data;;

    GST_LOG_OBJECT (stream->pad, "Removing stream %d %" GST_PTR_FORMAT,
        stream->index, stream->input_caps);
    gst_pad_push_event (stream->pad, gst_event_ref (eos));
    gst_pad_set_active (stream->pad, FALSE);
    gst_element_remove_pad (GST_ELEMENT (demux), stream->pad);
    gst_dash_demux_stream_free (stream);
  }
  gst_event_unref (eos);
  g_slist_free (streams);
}

static gboolean
gst_dash_demux_advance_period (GstDashDemux * demux)
{
  GSList *old_period = NULL;
  GSList *iter;
  GstEvent *seg_evt;

  GST_DEBUG_OBJECT (demux, "Advancing period from %p", demux->streams);

  if (demux->streams) {
    g_assert (demux->streams == demux->next_periods->data);

    demux->next_periods = g_slist_remove (demux->next_periods, demux->streams);
    old_period = demux->streams;
    demux->streams = NULL;
  }

  GST_DEBUG_OBJECT (demux, "Next period %p", demux->next_periods);

  if (demux->next_periods) {
    demux->streams = demux->next_periods->data;
  } else {
    GST_DEBUG_OBJECT (demux, "No next periods, return FALSE");
    GST_DASH_DEMUX_CLIENT_UNLOCK (demux);
    return FALSE;
  }

  GST_DASH_DEMUX_CLIENT_UNLOCK (demux);

  /* TODO protect with lock, using the client lock isn't useful
   * because causes deadlocks with the event_src handler */
  gst_dash_demux_expose_streams (demux);
  seg_evt = gst_event_new_segment (&demux->segment);
  for (iter = demux->streams; iter; iter = g_slist_next (iter)) {
    GstDashDemuxStream *stream = iter->data;

    gst_event_replace (&stream->pending_segment, seg_evt);
  }
  gst_event_unref (seg_evt);
  gst_dash_demux_remove_streams (demux, old_period);

  GST_DASH_DEMUX_CLIENT_LOCK (demux);
  return TRUE;
}

static GstFlowReturn
gst_dash_demux_push (GstDashDemuxStream * stream, GstBuffer * buffer)
{
  GstDashDemux *demux = stream->demux;
  GstClockTime timestamp, duration;
  GstFlowReturn ret = GST_FLOW_OK;

  timestamp = GST_BUFFER_TIMESTAMP (buffer);

  if (stream->pending_segment) {
    if (demux->timestamp_offset == -1)
      demux->timestamp_offset = timestamp;
    else
      demux->timestamp_offset = MIN (timestamp, demux->timestamp_offset);

    /* And send a newsegment */
    gst_pad_push_event (stream->pad, stream->pending_segment);
    stream->pending_segment = NULL;
  }

  /* make timestamp start from 0 by subtracting the offset */
  timestamp -= demux->timestamp_offset;
  duration = GST_BUFFER_DURATION (buffer);

  GST_BUFFER_TIMESTAMP (buffer) = timestamp;

  GST_DEBUG_OBJECT (stream->pad,
      "Pushing fragment %p #%" G_GUINT64_FORMAT " (stream %d) ts:%"
      GST_TIME_FORMAT " dur:%" GST_TIME_FORMAT, buffer,
      GST_BUFFER_OFFSET (buffer), stream->index,
      GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (buffer)),
      GST_TIME_ARGS (GST_BUFFER_DURATION (buffer)));
  ret = gst_pad_push (stream->pad, gst_buffer_ref (buffer));
  GST_DEBUG_OBJECT (stream->pad, "Push result: %d %s", ret,
      gst_flow_get_name (ret));

  demux->segment.position = timestamp;
  stream->position = timestamp;
  if (GST_CLOCK_TIME_IS_VALID (duration))
    stream->position += duration;

  return ret;
}

static void
gst_dash_demux_stream_free (GstDashDemuxStream * stream)
{
  gst_download_rate_deinit (&stream->dnl_rate);
  if (stream->input_caps) {
    gst_caps_unref (stream->input_caps);
    stream->input_caps = NULL;
  }
  if (stream->pending_segment) {
    gst_event_unref (stream->pending_segment);
  }
  if (stream->pad) {
    gst_object_unref (stream->pad);
    stream->pad = NULL;
  }
  if (stream->download_task) {
    gst_task_stop (stream->download_task);
    gst_object_unref (stream->download_task);
    g_rec_mutex_clear (&stream->download_task_lock);
  }
  g_cond_clear (&stream->download_cond);
  g_mutex_clear (&stream->download_mutex);

  if (stream->downloader != NULL) {
    g_object_unref (stream->downloader);
  }

  g_free (stream);
}

static void
gst_dash_demux_reset (GstDashDemux * demux, gboolean dispose)
{
  GSList *iter;

  GST_DEBUG_OBJECT (demux, "Resetting demux");

  demux->end_of_period = FALSE;
  demux->end_of_manifest = FALSE;

  gst_dash_demux_stop (demux);
  gst_dash_demux_wait_stop (demux);
  if (demux->downloader)
    gst_uri_downloader_reset (demux->downloader);

  if (demux->next_periods) {
    g_assert (demux->next_periods->data == demux->streams);
    demux->next_periods =
        g_slist_delete_link (demux->next_periods, demux->next_periods);
  }

  for (iter = demux->streams; iter; iter = g_slist_next (iter)) {
    GstDashDemuxStream *stream = iter->data;
    if (stream->pad) {
      GST_INFO_OBJECT (demux, "Removing stream pad %s:%s",
          GST_DEBUG_PAD_NAME (stream->pad));
      gst_element_remove_pad (GST_ELEMENT (demux), stream->pad);
    }
    gst_dash_demux_stream_free (stream);
  }
  g_slist_free (demux->streams);
  demux->streams = NULL;

  for (iter = demux->next_periods; iter; iter = g_slist_next (iter)) {
    GSList *streams = iter->data;
    g_slist_free_full (streams, (GDestroyNotify) gst_dash_demux_stream_free);
  }
  g_slist_free (demux->next_periods);
  demux->next_periods = NULL;

  if (demux->manifest) {
    gst_buffer_unref (demux->manifest);
    demux->manifest = NULL;
  }
  if (demux->client) {
    gst_mpd_client_free (demux->client);
    demux->client = NULL;
  }
  if (!dispose) {
    demux->client = gst_mpd_client_new ();
  }

  gst_segment_init (&demux->segment, GST_FORMAT_TIME);
  demux->last_manifest_update = GST_CLOCK_TIME_NONE;
  demux->cancelled = FALSE;
}

static GstFlowReturn
gst_dash_demux_refresh_mpd (GstDashDemux * demux)
{
  GstFragment *download;
  GstBuffer *buffer;
  GstClockTime duration, now = gst_util_get_timestamp ();
  gint64 update_period = demux->client->mpd_node->minimumUpdatePeriod;

  if (update_period == -1) {
    GST_DEBUG_OBJECT (demux, "minimumUpdatePeriod unspecified, "
        "will not update MPD");
    return GST_FLOW_OK;
  }

  /* init reference time for manifest file updates */
  if (!GST_CLOCK_TIME_IS_VALID (demux->last_manifest_update))
    demux->last_manifest_update = now;

  GST_DEBUG_OBJECT (demux,
      "Next update: %" GST_TIME_FORMAT " now: %" GST_TIME_FORMAT,
      GST_TIME_ARGS ((demux->last_manifest_update +
              update_period * GST_MSECOND)), GST_TIME_ARGS (now));

  /* update the manifest file */
  if (now >= demux->last_manifest_update + update_period * GST_MSECOND) {
    GST_DEBUG_OBJECT (demux, "Updating manifest file from URL %s",
        demux->client->mpd_uri);
    download = gst_uri_downloader_fetch_uri (demux->downloader,
        demux->client->mpd_uri);
    if (download) {
      GstMpdClient *new_client = NULL;

      buffer = gst_fragment_get_buffer (download);
      g_object_unref (download);
      /* parse the manifest file */
      if (buffer != NULL) {
        GstMapInfo mapinfo;

        new_client = gst_mpd_client_new ();
        new_client->mpd_uri = g_strdup (demux->client->mpd_uri);

        gst_buffer_map (buffer, &mapinfo, GST_MAP_READ);

        if (gst_mpd_parse (new_client, (gchar *) mapinfo.data, mapinfo.size)) {
          const gchar *period_id;
          guint period_idx;
          GSList *iter;

          /* prepare the new manifest and try to transfer the stream position
           * status from the old manifest client  */

          gst_buffer_unmap (buffer, &mapinfo);
          gst_buffer_unref (buffer);

          GST_DEBUG_OBJECT (demux, "Updating manifest");

          period_id = gst_mpd_client_get_period_id (demux->client);
          period_idx = gst_mpd_client_get_period_index (demux->client);

          /* setup video, audio and subtitle streams, starting from current Period */
          if (!gst_mpd_client_setup_media_presentation (new_client)) {
            /* TODO */
          }

          if (period_idx) {
            if (!gst_mpd_client_set_period_id (new_client, period_id)) {
              GST_DEBUG_OBJECT (demux,
                  "Error setting up the updated manifest file");
              return GST_FLOW_EOS;
            }
          } else {
            if (!gst_mpd_client_set_period_index (new_client, period_idx)) {
              GST_DEBUG_OBJECT (demux,
                  "Error setting up the updated manifest file");
              return GST_FLOW_EOS;
            }
          }

          if (!gst_dash_demux_setup_mpdparser_streams (demux, new_client)) {
            GST_ERROR_OBJECT (demux, "Failed to setup streams on manifest "
                "update");
            return GST_FLOW_ERROR;
          }

          /* update the streams to play from the next segment */
          for (iter = demux->streams; iter; iter = g_slist_next (iter)) {
            GstDashDemuxStream *demux_stream = iter->data;
            GstActiveStream *new_stream = demux_stream->active_stream;
            GstClockTime ts;

            if (!new_stream) {
              GST_DEBUG_OBJECT (demux,
                  "Stream of index %d is missing from manifest update",
                  demux_stream->index);
              return GST_FLOW_EOS;
            }

            if (gst_mpd_client_get_next_fragment_timestamp (demux->client,
                    demux_stream->index, &ts)) {
              gst_mpd_client_stream_seek (new_client, new_stream, ts);
            } else
                if (gst_mpd_client_get_last_fragment_timestamp (demux->client,
                    demux_stream->index, &ts)) {
              /* try to set to the old timestamp + 1 */
              gst_mpd_client_stream_seek (new_client, new_stream, ts + 1);
            }
          }

          gst_mpd_client_free (demux->client);
          demux->client = new_client;

          /* Send an updated duration message */
          duration =
              gst_mpd_client_get_media_presentation_duration (demux->client);

          if (duration != GST_CLOCK_TIME_NONE) {
            GST_DEBUG_OBJECT (demux,
                "Sending duration message : %" GST_TIME_FORMAT,
                GST_TIME_ARGS (duration));
            gst_element_post_message (GST_ELEMENT (demux),
                gst_message_new_duration_changed (GST_OBJECT (demux)));
          } else {
            GST_DEBUG_OBJECT (demux,
                "mediaPresentationDuration unknown, can not send the duration message");
          }
          demux->last_manifest_update = gst_util_get_timestamp ();
          GST_DEBUG_OBJECT (demux, "Manifest file successfully updated");
        } else {
          /* In most cases, this will happen if we set a wrong url in the
           * source element and we have received the 404 HTML response instead of
           * the manifest */
          GST_WARNING_OBJECT (demux, "Error parsing the manifest.");
          gst_buffer_unmap (buffer, &mapinfo);
          gst_buffer_unref (buffer);
        }
      } else {
        /* download suceeded, but resulting buffer is NULL */
        GST_WARNING_OBJECT (demux, "Error validating the manifest.");
      }
    } else {
      /* download failed */
      GST_WARNING_OBJECT (demux,
          "Failed to update the manifest file from URL %s",
          demux->client->mpd_uri);
    }
  }
  return GST_FLOW_OK;
}

static GstFlowReturn
gst_dash_demux_combine_flows (GstDashDemux * demux)
{
  gboolean all_notlinked = TRUE;
  GSList *iter;

  for (iter = demux->streams; iter; iter = g_slist_next (iter)) {
    GstDashDemuxStream *stream = iter->data;

    if (stream->last_ret != GST_FLOW_NOT_LINKED)
      all_notlinked = FALSE;

    if (stream->last_ret <= GST_FLOW_NOT_NEGOTIATED
        || stream->last_ret == GST_FLOW_FLUSHING)
      return stream->last_ret;
  }
  if (all_notlinked)
    return GST_FLOW_NOT_LINKED;
  return GST_FLOW_OK;
}


/* gst_dash_demux_stream_download_loop:
 * 
 * Loop for the "download' task that fetches fragments based on the 
 * selected representations.
 * 
 * During playback:  
 * 
 * It sequentially fetches fragments corresponding to the current 
 * representations and pushes them downstream
 * 
 * When a new set of fragments has been downloaded, it evaluates the
 * download time to check if we can or should switch to a different 
 * set of representations.
 *
 * Teardown:
 * 
 * The task will exit when it encounters an error or when the end of the
 * manifest has been reached.
 */
static void
gst_dash_demux_stream_download_loop (GstDashDemuxStream * stream)
{
  GstDashDemux *demux = stream->demux;
  GstFlowReturn flow_ret = GST_FLOW_OK;
  GstClockTime fragment_ts = GST_CLOCK_TIME_NONE;
  GstEvent *caps_event;

  GST_LOG_OBJECT (stream->pad, "Starting download loop");

  GST_DASH_DEMUX_CLIENT_LOCK (demux);
  if (demux->cancelled) {
    goto cancelled;
  }

  if (gst_mpd_client_is_live (demux->client)
      && demux->client->mpd_uri != NULL) {
    switch (gst_dash_demux_refresh_mpd (demux)) {
      case GST_FLOW_EOS:
        GST_DASH_DEMUX_CLIENT_UNLOCK (demux);
        goto end_of_manifest;
      default:
        break;
    }
  }

  /* try to switch to another set of representations if needed */
  caps_event = gst_dash_demux_stream_select_representation_unlocked (stream);
  GST_DASH_DEMUX_CLIENT_UNLOCK (demux);

  if (G_UNLIKELY (caps_event && !gst_pad_push_event (stream->pad, caps_event))) {
    /* TODO fail if caps is rejected */
  }

  /* fetch the next fragment */
  flow_ret = gst_dash_demux_stream_get_next_fragment (stream, &fragment_ts);

  GST_DASH_DEMUX_CLIENT_LOCK (demux);
  if (demux->cancelled) {
    goto cancelled;
  }

  stream->last_ret = flow_ret;
  switch (flow_ret) {
    case GST_FLOW_OK:
      break;
    case GST_FLOW_NOT_LINKED:
      gst_task_pause (stream->download_task);
      if (gst_dash_demux_combine_flows (demux) == GST_FLOW_NOT_LINKED) {
        GST_ELEMENT_ERROR (demux, STREAM, FAILED,
            (_("Internal data stream error.")),
            ("stream stopped, reason %s",
                gst_flow_get_name (GST_FLOW_NOT_LINKED)));
      }
      break;

    case GST_FLOW_FLUSHING:
      gst_dash_demux_stop (demux);
      break;

    case GST_FLOW_EOS:
      GST_DEBUG_OBJECT (stream->pad, "EOS");
      stream->stream_eos = TRUE;
      if (gst_dash_demux_all_streams_eop (demux)) {
        GST_INFO_OBJECT (stream->pad, "Reached the end of the Period");

        if (gst_mpd_client_has_next_period (demux->client)) {

          GST_INFO_OBJECT (demux, "Starting next period");
          /* setup video, audio and subtitle streams, starting from the next Period */
          if (!gst_mpd_client_set_period_index (demux->client,
                  gst_mpd_client_get_period_index (demux->client) + 1)
              || !gst_dash_demux_setup_all_streams (demux)) {
          }

          stream->last_ret = GST_FLOW_OK;
          /* start playing from the first segment of the new period */
          gst_mpd_client_set_segment_index_for_all_streams (demux->client, 0);

          gst_dash_demux_advance_period (demux);

          gst_dash_demux_resume_download_task (demux);
          GST_DASH_DEMUX_CLIENT_UNLOCK (demux);

          /* This pad is now finished, we lost its reference */
          return;
        }
      }
      gst_task_pause (stream->download_task);
      break;
    case GST_FLOW_ERROR:
      /* Download failed 'by itself'
       * in case this is live, we might be ahead or before playback, where
       * segments don't exist (are still being created or were already deleted)
       * so we either wait or jump ahead */
      if (gst_mpd_client_is_live (demux->client)) {
        gint64 time_diff;
        gint pos;

        pos =
            gst_mpd_client_check_time_position (demux->client,
            stream->active_stream, fragment_ts, &time_diff);
        GST_DEBUG_OBJECT (stream->pad,
            "Checked position for fragment ts %" GST_TIME_FORMAT
            ", res: %d, diff: %" G_GINT64_FORMAT, GST_TIME_ARGS (fragment_ts),
            pos, time_diff);

        time_diff *= GST_USECOND;
        if (pos < 0) {
          /* we're behind, try moving to the 'present' */
          GDateTime *now = g_date_time_new_now_utc ();

          GST_DEBUG_OBJECT (stream->pad,
              "Falling behind live stream, moving forward");
          gst_mpd_client_seek_to_time (demux->client, now);
          g_date_time_unref (now);

        } else if (pos > 0) {
          /* we're ahead, wait a little */

          GST_DEBUG_OBJECT (stream->pad,
              "Waiting for next segment to be created");
          gst_mpd_client_set_segment_index (stream->active_stream,
              stream->active_stream->segment_idx - 1);
          gst_dash_demux_download_wait (stream, time_diff);
        } else {
          gst_mpd_client_set_segment_index (stream->active_stream,
              stream->active_stream->segment_idx - 1);
          demux->client->update_failed_count++;
        }
      } else {
        demux->client->update_failed_count++;
      }

      if (demux->client->update_failed_count < DEFAULT_FAILED_COUNT) {
        GST_WARNING_OBJECT (stream->pad, "Could not fetch the next fragment");
        goto quit;
      } else {
        goto error_downloading;
      }
      break;
    default:
      break;
  }

  if (G_LIKELY (stream->last_ret != GST_FLOW_ERROR))
    demux->client->update_failed_count = 0;

quit:
  GST_DASH_DEMUX_CLIENT_UNLOCK (demux);

  if (G_UNLIKELY (stream->last_ret == GST_FLOW_EOS)) {
    gst_pad_push_event (stream->pad, gst_event_new_eos ());
  }

  GST_DEBUG_OBJECT (stream->pad, "Finishing download loop");
  return;

cancelled:
  {
    GST_WARNING_OBJECT (stream->pad, "Cancelled, leaving download task");
    GST_DASH_DEMUX_CLIENT_UNLOCK (demux);
    return;
  }

end_of_manifest:
  {
    GST_INFO_OBJECT (stream->pad, "End of manifest, leaving download task");
    gst_task_stop (stream->download_task);
    /* TODO should push eos? */
    GST_DASH_DEMUX_CLIENT_UNLOCK (demux);
    return;
  }

error_downloading:
  {
    GST_ELEMENT_ERROR (demux, RESOURCE, NOT_FOUND,
        ("Could not fetch the next fragment, leaving download task"), (NULL));
    gst_task_stop (stream->download_task);
    GST_DASH_DEMUX_CLIENT_UNLOCK (demux);
    return;
  }
}

static void
gst_dash_demux_resume_download_task (GstDashDemux * demux)
{
  GSList *iter;

  for (iter = demux->streams; iter; iter = g_slist_next (iter)) {
    GstDashDemuxStream *stream = iter->data;
    gst_task_start (stream->download_task);
  }
}

/*
 * gst_dash_demux_stream_select_representation_unlocked:
 *
 * Select the most appropriate media representation based on current target 
 * bitrate.
 */
static GstEvent *
gst_dash_demux_stream_select_representation_unlocked (GstDashDemuxStream *
    stream)
{
  GstActiveStream *active_stream = NULL;
  GList *rep_list = NULL;
  gint new_index;
  GstDashDemux *demux = stream->demux;
  guint64 bitrate;

  active_stream = stream->active_stream;
  if (active_stream == NULL)
    return FALSE;

  /* retrieve representation list */
  if (active_stream->cur_adapt_set)
    rep_list = active_stream->cur_adapt_set->Representations;
  if (!rep_list)
    return FALSE;

  bitrate =
      gst_download_rate_get_current_rate (&stream->dnl_rate) *
      demux->bandwidth_usage;
  GST_DEBUG_OBJECT (demux, "Trying to change to bitrate: %" G_GUINT64_FORMAT,
      bitrate);

  /* get representation index with current max_bandwidth */
  new_index = gst_mpdparser_get_rep_idx_with_max_bandwidth (rep_list, bitrate);

  /* if no representation has the required bandwidth, take the lowest one */
  if (new_index == -1)
    new_index = gst_mpdparser_get_rep_idx_with_min_bandwidth (rep_list);

  if (new_index != active_stream->representation_idx) {
    GstRepresentationNode *rep = g_list_nth_data (rep_list, new_index);
    GST_INFO_OBJECT (demux, "Changing representation idx: %d %d %u",
        stream->index, new_index, rep->bandwidth);
    if (gst_mpd_client_setup_representation (demux->client, active_stream, rep)) {
      stream->need_header = TRUE;
      GST_INFO_OBJECT (demux, "Switching bitrate to %d",
          active_stream->cur_representation->bandwidth);
      gst_caps_unref (stream->input_caps);
      stream->input_caps = gst_dash_demux_get_input_caps (demux, active_stream);
      return gst_event_new_caps (gst_caps_ref (stream->input_caps));
    } else {
      GST_WARNING_OBJECT (demux, "Can not switch representation, aborting...");
      return NULL;
    }
  }
  return NULL;
}

static GstBuffer *
gst_dash_demux_download_header_fragment (GstDashDemux * demux,
    GstDashDemuxStream * stream, gchar * path, gint64 range_start,
    gint64 range_end)
{
  GstBuffer *buffer = NULL;
  gchar *next_header_uri;
  GstFragment *fragment;

  if (strncmp (path, "http://", 7) != 0) {
    next_header_uri =
        g_strconcat (gst_mpdparser_get_baseURL (demux->client, stream->index),
        path, NULL);
    g_free (path);
  } else {
    next_header_uri = path;
  }

  fragment = gst_uri_downloader_fetch_uri_with_range (stream->downloader,
      next_header_uri, range_start, range_end);
  g_free (next_header_uri);
  if (fragment) {
    buffer = gst_fragment_get_buffer (fragment);
    g_object_unref (fragment);
  }
  return buffer;
}

static GstBuffer *
gst_dash_demux_get_next_header (GstDashDemux * demux,
    GstDashDemuxStream * stream)
{
  gchar *initializationURL;
  GstBuffer *header_buffer, *index_buffer = NULL;
  gint64 range_start, range_end;

  if (!gst_mpd_client_get_next_header (demux->client, &initializationURL,
          stream->index, &range_start, &range_end))
    return NULL;

  GST_INFO_OBJECT (demux, "Fetching header %s %" G_GINT64_FORMAT "-%"
      G_GINT64_FORMAT, initializationURL, range_start, range_end);
  header_buffer = gst_dash_demux_download_header_fragment (demux, stream,
      initializationURL, range_start, range_end);

  /* check if we have an index */
  if (header_buffer
      && gst_mpd_client_get_next_header_index (demux->client,
          &initializationURL, stream->index, &range_start, &range_end)) {
    GST_INFO_OBJECT (demux,
        "Fetching index %s %" G_GINT64_FORMAT "-%" G_GINT64_FORMAT,
        initializationURL, range_start, range_end);
    index_buffer =
        gst_dash_demux_download_header_fragment (demux, stream,
        initializationURL, range_start, range_end);
  }

  if (header_buffer == NULL) {
    GST_WARNING_OBJECT (demux, "Unable to fetch header");
    return NULL;
  }

  if (index_buffer) {
    header_buffer = gst_buffer_append (header_buffer, index_buffer);
  }

  return header_buffer;
}

static GstCaps *
gst_dash_demux_get_video_input_caps (GstDashDemux * demux,
    GstActiveStream * stream)
{
  guint width = 0, height = 0;
  const gchar *mimeType = NULL;
  GstCaps *caps = NULL;

  if (stream == NULL)
    return NULL;

  /* if bitstreamSwitching is true we dont need to swich pads on resolution change */
  if (!gst_mpd_client_get_bitstream_switching_flag (stream)) {
    width = gst_mpd_client_get_video_stream_width (stream);
    height = gst_mpd_client_get_video_stream_height (stream);
  }
  mimeType = gst_mpd_client_get_stream_mimeType (stream);
  if (mimeType == NULL)
    return NULL;

  caps = gst_caps_from_string (mimeType);
  if (width > 0 && height > 0) {
    gst_caps_set_simple (caps, "width", G_TYPE_INT, width, "height",
        G_TYPE_INT, height, NULL);
  }

  return caps;
}

static GstCaps *
gst_dash_demux_get_audio_input_caps (GstDashDemux * demux,
    GstActiveStream * stream)
{
  guint rate = 0, channels = 0;
  const gchar *mimeType;
  GstCaps *caps = NULL;

  if (stream == NULL)
    return NULL;

  /* if bitstreamSwitching is true we dont need to swich pads on rate/channels change */
  if (!gst_mpd_client_get_bitstream_switching_flag (stream)) {
    channels = gst_mpd_client_get_audio_stream_num_channels (stream);
    rate = gst_mpd_client_get_audio_stream_rate (stream);
  }
  mimeType = gst_mpd_client_get_stream_mimeType (stream);
  if (mimeType == NULL)
    return NULL;

  caps = gst_caps_from_string (mimeType);
  if (rate > 0) {
    gst_caps_set_simple (caps, "rate", G_TYPE_INT, rate, NULL);
  }
  if (channels > 0) {
    gst_caps_set_simple (caps, "channels", G_TYPE_INT, channels, NULL);
  }

  return caps;
}

static GstCaps *
gst_dash_demux_get_application_input_caps (GstDashDemux * demux,
    GstActiveStream * stream)
{
  const gchar *mimeType;
  GstCaps *caps = NULL;

  if (stream == NULL)
    return NULL;

  mimeType = gst_mpd_client_get_stream_mimeType (stream);
  if (mimeType == NULL)
    return NULL;

  caps = gst_caps_from_string (mimeType);

  return caps;
}

static GstCaps *
gst_dash_demux_get_input_caps (GstDashDemux * demux, GstActiveStream * stream)
{
  switch (stream->mimeType) {
    case GST_STREAM_VIDEO:
      return gst_dash_demux_get_video_input_caps (demux, stream);
    case GST_STREAM_AUDIO:
      return gst_dash_demux_get_audio_input_caps (demux, stream);
    case GST_STREAM_APPLICATION:
      return gst_dash_demux_get_application_input_caps (demux, stream);
    default:
      return GST_CAPS_NONE;
  }
}

static void
gst_dash_demux_wait_for_fragment_to_be_available (GstDashDemux * demux,
    GstDashDemuxStream * stream)
{
  GstDateTime *seg_end_time;
  GstDateTime *cur_time = gst_date_time_new_now_utc ();
  GstActiveStream *active_stream = stream->active_stream;

  seg_end_time =
      gst_mpd_client_get_next_segment_availability_end_time (demux->client,
      active_stream);

  if (seg_end_time) {
    gint64 diff;

    cur_time = gst_date_time_new_now_utc ();
    diff = gst_mpd_client_calculate_time_difference (cur_time, seg_end_time)
        / GST_MSECOND;
    gst_date_time_unref (seg_end_time);
    gst_date_time_unref (cur_time);
    if (diff > 0) {
      GST_DEBUG_OBJECT (demux,
          "Selected fragment has end timestamp > now (%" PRIi64
          "), delaying download", diff);
      gst_dash_demux_download_wait (stream, diff);
    }
  }
}

static GstBuffer *
gst_dash_demux_stream_download_fragment (GstDashDemux * demux,
    GstDashDemuxStream * stream, guint64 * size_buffer,
    GstClockTime * download_time)
{
  GstActiveStream *active_stream;
  GstFragment *download;
  GTimeVal now;
  GTimeVal start;
  guint stream_idx = stream->index;
  GstBuffer *buffer = NULL;
  GstBuffer *header_buffer;
  GstMediaFragmentInfo fragment;

  if (G_UNLIKELY (stream->restart_download)) {
    GstClockTime cur, ts;
    gint64 pos;
    GstEvent *gap;

    GST_DEBUG_OBJECT (stream->pad,
        "Reactivating stream after to reconfigure event");

    cur = GST_CLOCK_TIME_IS_VALID (stream->position) ? stream->position : 0;

    if (gst_pad_peer_query_position (stream->pad, GST_FORMAT_TIME, &pos)) {
      ts = (GstClockTime) pos;
      GST_DEBUG_OBJECT (stream->pad, "Downstream position: %"
          GST_TIME_FORMAT, GST_TIME_ARGS (ts));
    } else {
      ts = demux->segment.position;
      GST_DEBUG_OBJECT (stream->pad, "Downstream position query failed, "
          "failling back to looking at other pads");
    }

    GST_DEBUG_OBJECT (stream->pad, "Restarting stream at "
        "position %" GST_TIME_FORMAT ", current catch up %" GST_TIME_FORMAT,
        GST_TIME_ARGS (ts), GST_TIME_ARGS (demux->segment.position));

    if (GST_CLOCK_TIME_IS_VALID (ts)) {
      gst_mpd_client_stream_seek (demux->client, stream->active_stream, ts);

      if (cur < ts) {
        gap = gst_event_new_gap (cur, ts - cur);
        gst_pad_push_event (stream->pad, gap);
      }
    }

    /* This stream might be entering into catching up mode,
     * meaning that it will push buffers from this same download thread
     * until it reaches 'catch_up_timestamp'.
     *
     * The reason for this is that in case of stream switching, the other
     * stream that was previously active might be blocking the stream_loop
     * in case it is ahead enough that all queues are filled.
     * In this case, it is possible that a downstream input-selector is
     * blocking waiting for the currently active stream to reach the
     * same position of the old linked stream because of the 'sync-streams'
     * behavior.
     *
     * We can push from this thread up to 'catch_up_timestamp' as all other
     * streams should be around the same timestamp.
     */
    stream->last_ret = GST_FLOW_CUSTOM_SUCCESS;

    stream->restart_download = FALSE;
  }

  if (gst_mpd_client_get_next_fragment (demux->client, stream_idx, &fragment)) {
    g_get_current_time (&start);
    GST_INFO_OBJECT (stream->pad,
        "Fetching next fragment %s ts:%" GST_TIME_FORMAT " dur:%"
        GST_TIME_FORMAT " Range:%" G_GINT64_FORMAT "-%" G_GINT64_FORMAT,
        fragment.uri, GST_TIME_ARGS (fragment.timestamp),
        GST_TIME_ARGS (fragment.duration),
        fragment.range_start, fragment.range_end);

    download = gst_uri_downloader_fetch_uri_with_range (stream->downloader,
        fragment.uri, fragment.range_start, fragment.range_end);

    if (download == NULL) {
      gst_media_fragment_info_clear (&fragment);
      return NULL;
    }

    active_stream = stream->active_stream;
    if (active_stream == NULL) {
      gst_media_fragment_info_clear (&fragment);
      g_object_unref (download);
      return NULL;
    }

    buffer = gst_fragment_get_buffer (download);
    g_object_unref (download);

    /* it is possible to have an index per fragment, so check and download */
    if (fragment.index_uri || fragment.index_range_start
        || fragment.index_range_end != -1) {
      const gchar *uri = fragment.index_uri;
      GstBuffer *index_buffer;

      if (!uri)                 /* fallback to default media uri */
        uri = fragment.uri;

      GST_DEBUG_OBJECT (stream->pad,
          "Fragment index download: %s %" G_GINT64_FORMAT "-%"
          G_GINT64_FORMAT, uri, fragment.index_range_start,
          fragment.index_range_end);
      download =
          gst_uri_downloader_fetch_uri_with_range (stream->downloader, uri,
          fragment.index_range_start, fragment.index_range_end);
      if (download) {
        index_buffer = gst_fragment_get_buffer (download);
        if (index_buffer)
          buffer = gst_buffer_append (index_buffer, buffer);
        g_object_unref (download);
      }
    }

    if (stream->need_header) {
      /* We need to fetch a new header */
      if ((header_buffer =
              gst_dash_demux_get_next_header (demux, stream)) != NULL) {
        buffer = gst_buffer_append (header_buffer, buffer);
      }
      stream->need_header = FALSE;
    }
    g_get_current_time (&now);
    *download_time = (GST_TIMEVAL_TO_TIME (now) - GST_TIMEVAL_TO_TIME (start));

    buffer = gst_buffer_make_writable (buffer);

    GST_BUFFER_TIMESTAMP (buffer) = fragment.timestamp;
    GST_BUFFER_DURATION (buffer) = fragment.duration;
    GST_BUFFER_OFFSET (buffer) =
        gst_mpd_client_get_segment_index (active_stream) - 1;

    gst_media_fragment_info_clear (&fragment);
    *size_buffer += gst_buffer_get_size (buffer);
  } else {
    GST_WARNING_OBJECT (demux, "Failed to download fragment for stream %p %d",
        stream, stream->index);
  }
  return buffer;
}

/* gst_dash_demux_stream_get_next_fragment:
 *
 * Get the next fragments for the stream with the earlier timestamp.
 * It returns the selected timestamp so the caller can deal with
 * sync issues in case the stream is live.
 * 
 * This function uses the generic URI downloader API.
 *
 * Returns FALSE if an error occured while downloading fragments
 */
static GstFlowReturn
gst_dash_demux_stream_get_next_fragment (GstDashDemuxStream * stream,
    GstClockTime * ts)
{
  guint64 buffer_size = 0;
  GstClockTime diff;
  gboolean end_of_period = TRUE;
  GstBuffer *buffer = NULL;
  GstFlowReturn ret = GST_FLOW_OK;
  GstDashDemux *demux = stream->demux;

  if (stream->stream_eos)
    return GST_FLOW_EOS;

  if (stream->last_ret == GST_FLOW_NOT_LINKED) {
    GST_LOG_OBJECT (demux, "Skipping stream %p %s:%s : not-linked",
        stream, GST_DEBUG_PAD_NAME (stream->pad));
    return GST_FLOW_NOT_LINKED;
  }

  if (!gst_mpd_client_get_next_fragment_timestamp (demux->client,
          stream->index, ts)) {
    GST_INFO_OBJECT (demux,
        "This Period doesn't contain more fragments for stream %u",
        stream->index);

    /* check if this is live and we should wait for more data */
    if (gst_mpd_client_is_live (demux->client)
        && demux->client->mpd_node->minimumUpdatePeriod != -1) {
      end_of_period = FALSE;
      return GST_FLOW_OK;       /* TODO wait */
    }
    return GST_FLOW_EOS;
  }

  /*
   * If this is a live stream, check the segment end time to make sure
   * it is available to download
   */
  if (gst_mpd_client_is_live (demux->client) &&
      demux->client->mpd_node->minimumUpdatePeriod != -1) {

    gst_dash_demux_wait_for_fragment_to_be_available (demux, stream);
  }

  /* Get the fragment corresponding to each stream index */
  buffer =
      gst_dash_demux_stream_download_fragment (demux, stream,
      &buffer_size, &diff);
  end_of_period = FALSE;

  demux->end_of_period = end_of_period;

  if (buffer) {
    ret = gst_dash_demux_push (stream, buffer);
  } else {
    GST_WARNING_OBJECT (stream->pad, "Failed to download fragment");
    return GST_FLOW_ERROR;
  }

  if (end_of_period)
    return GST_FLOW_EOS;

  if (stream && buffer_size > 0 && diff > 0) {
#ifndef GST_DISABLE_GST_DEBUG
    guint64 brate;
#endif

    gst_download_rate_add_rate (&stream->dnl_rate, buffer_size, diff);

#ifndef GST_DISABLE_GST_DEBUG
    brate = (buffer_size * 8) / ((double) diff / GST_SECOND);
#endif
    GST_INFO_OBJECT (demux,
        "Stream: %d Download rate = %" G_GUINT64_FORMAT " Kbits/s (%"
        G_GUINT64_FORMAT " Ko in %.2f s)", stream->index, brate / 1000,
        buffer_size / 1024, ((double) diff / GST_SECOND));
  }
  return ret;
}

static void
gst_dash_demux_download_wait (GstDashDemuxStream * stream,
    GstClockTime time_diff)
{
  gint64 end_time = g_get_monotonic_time () + time_diff / GST_USECOND;

  GST_DEBUG_OBJECT (stream->pad, "Download waiting for %" GST_TIME_FORMAT,
      GST_TIME_ARGS (time_diff));
  g_cond_wait_until (&stream->download_cond, &stream->download_mutex, end_time);
  GST_DEBUG_OBJECT (stream->pad, "Download finished waiting");
}
