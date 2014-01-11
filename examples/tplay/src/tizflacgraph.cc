/**
 * Copyright (C) 2011-2013 Aratelia Limited - Juan A. Rubio
 *
 * This file is part of Tizonia
 *
 * Tizonia is free software: you can redistribute it and/or modify it under the
 * terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Tizonia is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Tizonia.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file   tizflacgraph.cc
 * @author Juan A. Rubio <juan.rubio@aratelia.com>
 *
 * @brief  OpenMAX IL flac graph impl
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "tizflacgraph.h"
#include "tizgraphconfig.h"
#include "tizprobe.h"

#include <tizosal.h>
#include <OMX_Core.h>
#include <OMX_Component.h>
#include <OMX_TizoniaExt.h>

#include <boost/foreach.hpp>
#include <boost/make_shared.hpp>

#ifdef TIZ_LOG_CATEGORY_NAME
#undef TIZ_LOG_CATEGORY_NAME
#define TIZ_LOG_CATEGORY_NAME "tiz.play.graph.flac"
#endif


namespace                       // unnamed namespace
{
  // Two demuxers to try, ogg demuxer and binary file reader
  const int kMaxAvailableDemuxers = 2;
}

tizflacgraph::tizflacgraph (tizprobe_ptr_t probe_ptr)
  : tizgraph (3, probe_ptr), demuxer_index_ (0), demux_attempts_ (0)
{
}

OMX_ERRORTYPE
tizflacgraph::do_load ()
{
  OMX_ERRORTYPE ret = OMX_ErrorNone;

  assert (OMX_StateLoaded == current_graph_state_);

  component_names_t comp_list;
  assert (demuxer_index_ < kMaxAvailableDemuxers);
  if (demuxer_index_ == 0)
    {
      comp_list.push_back ("OMX.Aratelia.container_demuxer.ogg");
    }
  else
    {
      comp_list.push_back ("OMX.Aratelia.file_reader.binary");
    }

  comp_list.push_back ("OMX.Aratelia.audio_decoder.flac");
  comp_list.push_back ("OMX.Aratelia.audio_renderer_nb.pcm");

  if (OMX_ErrorNone != (ret = verify_existence (comp_list)))
    {
      return ret;
    }

  component_roles_t role_list;
  if (demuxer_index_ == 0)
    {
      role_list.push_back ("container_demuxer.ogg");
    }
  else
    {
      role_list.push_back ("audio_reader.binary");
    }
  role_list.push_back ("audio_decoder.flac");
  role_list.push_back ("audio_renderer.pcm");

  if (OMX_ErrorNone != (ret = verify_role_list (comp_list, role_list)))
    {
      return ret;
    }

  if (OMX_ErrorNone != (ret = instantiate_list (comp_list)))
    {
      return ret;
    }

  return ret;
}

OMX_ERRORTYPE
tizflacgraph::disable_demuxer_video_port ()
{
  OMX_ERRORTYPE error = OMX_ErrorNone;

  // There is no video port in the binary file reader.
  if (demuxer_index_ ==  1)
    {
      return OMX_ErrorNone;
    }
  
  const OMX_COMMANDTYPE cmd = OMX_CommandPortDisable;
  // Port 1 = video port
  OMX_U32 port_id = 1;
  error = OMX_SendCommand (handles_[0], cmd, port_id, NULL);

  if (error == OMX_ErrorNone)
    {
      waitevent_list_t event_list;
      event_list.push_back(waitevent_info(handles_[0],
                                          OMX_EventCmdComplete,
                                          cmd,
                                          port_id,
                                          (OMX_PTR) OMX_ErrorNone));
      tiz_check_omx_err (cback_handler_.wait_for_event_list(event_list));
    }
  return error;
}

OMX_ERRORTYPE
tizflacgraph::configure_flac_graph (const int file_index)
{
  OMX_ERRORTYPE ret = OMX_ErrorNone;

  TIZ_LOG (TIZ_PRIORITY_TRACE, "Configure current_file_index_ [%d]...",
           current_file_index_);

  assert (file_index < file_list_.size ());
  assert (OMX_StateLoaded == current_graph_state_);

  tiz_check_omx_err (disable_demuxer_video_port ());

  bool quiet = false;            // Current version of libav doesn't support flac
  tiz_check_omx_err (probe_uri (file_index, quiet));

  // Set the new URI
  OMX_PARAM_CONTENTURITYPE *p_uritype = NULL;

  if (NULL == (p_uritype = (OMX_PARAM_CONTENTURITYPE *) tiz_mem_calloc
               (1, sizeof (OMX_PARAM_CONTENTURITYPE)
                + OMX_MAX_STRINGNAME_SIZE)))
    {
      return OMX_ErrorInsufficientResources;
    }

  p_uritype->nSize = sizeof (OMX_PARAM_CONTENTURITYPE)
    + OMX_MAX_STRINGNAME_SIZE;
  p_uritype->nVersion.nVersion = OMX_VERSION;

  const size_t uri_offset = offsetof (OMX_PARAM_CONTENTURITYPE, contentURI);
  strncpy ((char *) p_uritype + uri_offset, probe_ptr_->get_uri ().c_str (),
           OMX_MAX_STRINGNAME_SIZE);
  p_uritype->contentURI[strlen (probe_ptr_->get_uri ().c_str ())] = '\0';

  ret = OMX_SetParameter (handles_[0], OMX_IndexParamContentURI, p_uritype);

  tiz_mem_free (p_uritype);
  p_uritype = NULL;

  if (OMX_ErrorNone != ret)
    {
      return ret;
    }

  // Retrieve the current flac settings from the decoder's port #0
  OMX_TIZONIA_AUDIO_PARAM_FLACTYPE flactype;
  OMX_TIZONIA_AUDIO_PARAM_FLACTYPE flactype_orig;

  flactype.nSize             = sizeof (OMX_TIZONIA_AUDIO_PARAM_FLACTYPE);
  flactype.nVersion.nVersion = OMX_VERSION;
  flactype.nPortIndex        = 0;

  tiz_check_omx_err (OMX_GetParameter (handles_[1],
                                       static_cast<OMX_INDEXTYPE>
                                       (OMX_TizoniaIndexParamAudioFlac),
                                       &flactype));
  flactype_orig = flactype;

  // Set the flac settings on decoder's port #0

  probe_ptr_->get_flac_codec_info (flactype);
  flactype.nPortIndex = 0;
  tiz_check_omx_err (OMX_SetParameter (handles_[1],
                                       static_cast<OMX_INDEXTYPE>
                                       (OMX_TizoniaIndexParamAudioFlac),
                                       &flactype));

  if (flactype_orig.nSampleRate != flactype.nSampleRate
      || flactype_orig.nChannels != flactype.nChannels)
    {
      // Await port settings change event on decoders's port #1
      waitevent_list_t event_list
        (1,
         waitevent_info
         (handles_[1],
          OMX_EventPortSettingsChanged,
          1,     //nData1
          (OMX_U32) OMX_IndexParamAudioPcm,       //nData2
          NULL));
      cback_handler_.wait_for_event_list (event_list);
    }

  // Set the pcm settings on renderer's port #0
  OMX_AUDIO_PARAM_PCMMODETYPE pcmtype;

  probe_ptr_->get_pcm_codec_info (pcmtype);
  pcmtype.nSize = sizeof (OMX_AUDIO_PARAM_PCMMODETYPE);
  pcmtype.nVersion.nVersion = OMX_VERSION;
  pcmtype.nPortIndex = 0;

  tiz_check_omx_err (OMX_SetParameter (handles_[2], OMX_IndexParamAudioPcm,
                                       &pcmtype));

  // Only output the pcm banner the first time we attempt the demuxing of the
  // current item in the playlist
  if (demux_attempts_ != 0)
    {
      dump_pcm_info (pcmtype);
    }
  
  return OMX_ErrorNone;
}

void
tizflacgraph::unload_flac_graph ()
{
  (void) transition_all (OMX_StateIdle, OMX_StateExecuting);
  (void) transition_all (OMX_StateLoaded, OMX_StateIdle);
  tear_down_tunnels ();
  destroy_list ();
}

OMX_ERRORTYPE
tizflacgraph::do_configure (const tizgraphconfig_ptr_t &config)
{
  OMX_ERRORTYPE ret = OMX_ErrorNone;

  config_ = config;
  file_list_ = config_->get_uris ();
  current_file_index_ = 0;

  tiz_check_omx_err (setup_suppliers ());
  tiz_check_omx_err (setup_tunnels ());

  return OMX_ErrorNone;
}

OMX_ERRORTYPE
tizflacgraph::do_execute ()
{
  TIZ_LOG (TIZ_PRIORITY_TRACE, "current_file_index_ [%d] list size [%d]..."
           " demux_attempts_ [%d]",
           current_file_index_, file_list_.size (), demux_attempts_);

  assert (OMX_StateLoaded == current_graph_state_);

  if (current_file_index_ >= file_list_.size ())
    {
      current_file_index_ = 0;
    }

  tiz_check_omx_err (configure_flac_graph (current_file_index_));
  tiz_check_omx_err (transition_all (OMX_StateIdle, OMX_StateLoaded));
  tiz_check_omx_err (transition_all (OMX_StateExecuting, OMX_StateIdle));

  // Increment the number of times we are attempting to demux this file
  demux_attempts_++;

  return OMX_ErrorNone;
}

OMX_ERRORTYPE
tizflacgraph::do_pause ()
{
  if (OMX_StateExecuting == current_graph_state_)
    {
      return transition_all (OMX_StatePause, OMX_StateExecuting);
    }
  else if (OMX_StatePause == current_graph_state_)
    {
      return transition_all (OMX_StateExecuting, OMX_StatePause);
    }
  return OMX_ErrorNone;
}

OMX_ERRORTYPE
tizflacgraph::do_seek ()
{
  return OMX_ErrorNone;
}

OMX_ERRORTYPE
tizflacgraph::do_skip (const int jump)
{
  if (jump == 0)
    {
      return OMX_ErrorNone;
    }

  if (OMX_StateExecuting != current_graph_state_
      && OMX_StatePause != current_graph_state_)
    {
      // Only allow skip to next/prev song if the graph is in executing or
      // pause states.
      return OMX_ErrorNone;
    }

  if (OMX_StateExecuting == current_graph_state_)
    {
      tiz_check_omx_err (transition_all (OMX_StateIdle, OMX_StateExecuting));
      tiz_check_omx_err (transition_all (OMX_StateLoaded, OMX_StateIdle));
    }
  else if (OMX_StatePause == current_graph_state_)
    {
      tiz_check_omx_err (transition_all (OMX_StateIdle, OMX_StatePause));
      tiz_check_omx_err (transition_all (OMX_StateLoaded, OMX_StateIdle));
    }

  current_file_index_ += jump;

  if (current_file_index_ < 0)
    {
      current_file_index_ = 0;
    }

  if (current_file_index_ > file_list_.size ())
    {
      current_file_index_ = file_list_.size ();
    }

  TIZ_LOG (TIZ_PRIORITY_TRACE, "Configure current_file_index_ [%d]...",
           current_file_index_);

  // This count as a successful playback, reset the demuxing count.
  demux_attempts_ = 0;

  return do_execute ();
}

OMX_ERRORTYPE
tizflacgraph::do_volume (const int step)
{
  // Use helper method in base class
  OMX_U32 renderers_input_port = 0;
  return apply_volume (handles_[2], renderers_input_port, step);
}

OMX_ERRORTYPE
tizflacgraph::do_mute ()
{
  // Use helper method in base class
  OMX_U32 renderers_input_port = 0;
  return apply_mute (handles_[2], renderers_input_port);
}

void
tizflacgraph::do_unload ()
{
  unload_flac_graph ();
  demuxer_index_ = 0;
  demux_attempts_ = 0;
}

void
tizflacgraph::do_error (const OMX_ERRORTYPE error)
{
  TIZ_LOG (TIZ_PRIORITY_TRACE, "[%s] - demux_attempts_ [%d]..."
           " current_file_index_ [%d]", tiz_err_to_str (error),
           demux_attempts_, current_file_index_);
  
  if ((error == OMX_ErrorFormatNotDetected
       || error == OMX_ErrorStreamCorrupt)
      && demux_attempts_ < kMaxAvailableDemuxers)
    {
      // Tear down the current graph...
      unload_flac_graph ();
      assert (OMX_StateLoaded == current_graph_state_);

      // ... and re-try the same file with an alternative demuxer.
      demuxer_index_ = (demuxer_index_ + 1 == kMaxAvailableDemuxers ? 0 : demuxer_index_ + 1);
      (void) do_load ();
      (void) setup_suppliers ();
      (void) setup_tunnels ();
      (void) do_execute ();
    }
  else
    {
      notify_graph_error (error, "");
    }
}

void
tizflacgraph::do_eos (const OMX_HANDLETYPE handle)
{
  TIZ_LOG (TIZ_PRIORITY_TRACE, "demux_attempts_ [%d]..."
           " current_file_index_ [%d]", demux_attempts_, current_file_index_);
  if (handle == handles_[2])
    {
      // This was a successful playback, reset the demuxing count.
      demux_attempts_ = 0;
      current_file_index_++;
      if (config_->continuous_playback ()
          || current_file_index_ < file_list_.size ())
        {
          (void) transition_all (OMX_StateIdle, OMX_StateExecuting);
          (void) transition_all (OMX_StateLoaded, OMX_StateIdle);
          (void) do_execute ();
        }
      else
        {
          notify_graph_end_of_play ();
        }
    }
}

OMX_ERRORTYPE
tizflacgraph::probe_uri (const int uri_index, const bool quiet)
{
  assert (uri_index < file_list_.size ());

  // Only do probing the first time we attempt the demuxing of the
  // current item in the playlist
  if (demux_attempts_ != 0)
    {
      return OMX_ErrorNone;
    }
  
  const std::string &uri = file_list_[uri_index];

  if (!uri.empty ())
    {
      // Probe a new uri
      probe_ptr_.reset ();
      bool quiet_probing = true;
      probe_ptr_ = boost::make_shared < tizprobe > (uri, quiet_probing);
      if (probe_ptr_->get_omx_domain () != OMX_PortDomainAudio
          || probe_ptr_->get_audio_coding_type () != OMX_AUDIO_CodingFLAC)
        {
          return OMX_ErrorContentURIError;
        }
      if (!quiet)
        {
          dump_graph_info ("flac", "decode", uri);
        }
    }
  return OMX_ErrorNone;
}