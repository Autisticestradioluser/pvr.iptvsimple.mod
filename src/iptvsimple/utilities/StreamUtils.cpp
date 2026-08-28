/*
 *  Copyright (C) 2005-2021 Team Kodi (https://kodi.tv)
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include "StreamUtils.h"

#include "../InstanceSettings.h"
#include "FileUtils.h"
#include "Logger.h"
#include "WebStreamExtractor.h"
#include "WebUtils.h"

#include <kodi/General.h>
#include <kodi/tools/StringUtils.h>

using namespace kodi::tools;
using namespace iptvsimple;
using namespace iptvsimple::data;
using namespace iptvsimple::utilities;

namespace
{
bool SplitUrlProtocolOpts(const std::string& streamURL,
                          std::string& url,
                          std::string& encodedProtocolOptions)
{
  size_t found = streamURL.find_first_of('|');
  if (found != std::string::npos)
  {
      // Headers found, split and url-encode them
      url = streamURL.substr(0, found);
      const std::string& protocolOptions = streamURL.substr(found + 1, streamURL.length());
      encodedProtocolOptions = StreamUtils::GetUrlEncodedProtocolOptions(protocolOptions);
      return true;
  }
  return false;
}
} // unnamed namespace

void StreamUtils::SetAllStreamProperties(std::vector<kodi::addon::PVRStreamProperty>& properties, const iptvsimple::data::Channel& channel, const std::string& streamURL, bool isChannelURL, std::map<std::string, std::string>& catchupProperties, std::shared_ptr<InstanceSettings>& settings)
{
  // Check if the channel has explicitly set up the use of inputstream.adaptive,
  // if so, the best behaviour for media services is:
  // - Always add mimetype to prevent kodi core to make an HTTP HEADER requests
  //   this because in some cases services refuse this request and can also deny downloads
  // - If requested by settings, always add the "user-agent" header to ISA properties
  const bool isISAdaptiveSet =
      channel.GetProperty(PVR_STREAM_PROPERTY_INPUTSTREAM) == INPUTSTREAM_ADAPTIVE;

  if (!isISAdaptiveSet && ChannelSpecifiesInputstream(channel))
  {
    const std::string& inputstreamName = channel.GetInputStreamName();
    if (inputstreamName != PVR_STREAM_PROPERTY_VALUE_INPUTSTREAMFFMPEG)
      CheckInputstreamInstalledAndEnabled(inputstreamName);

    if (inputstreamName == INPUTSTREAM_FFMPEGDIRECT || inputstreamName == PVR_STREAM_PROPERTY_VALUE_INPUTSTREAMFFMPEG)
    {
      // Build the reconnect-augmented URL for FFmpeg-based inputstreams so stalled/EOF'd
      // live streams auto-reconnect instead of hanging (see GetURLWithFFmpegReconnectOptions).
      StreamType streamType = StreamUtils::GetStreamType(streamURL, channel.GetMimeType(), channel.IsCatchupTSStream());
      if (streamType == StreamType::OTHER_TYPE)
        streamType = StreamUtils::InspectStreamType(streamURL, channel.GetCatchupMode());
      std::string effectiveStreamUrl = StreamUtils::GetURLWithFFmpegReconnectOptions(
          streamURL, streamType, inputstreamName,
          channel.GetProperty("http-reconnect") == "true", settings);
      properties.emplace_back(PVR_STREAM_PROPERTY_STREAMURL, effectiveStreamUrl);

      Logger::Log(LogLevel::LEVEL_INFO, "%s - Stream property set: STREAMURL = %s", __FUNCTION__, WebUtils::RedactUrl(effectiveStreamUrl).c_str());

      if (inputstreamName == INPUTSTREAM_FFMPEGDIRECT)
      {
        InspectAndSetFFmpegDirectStreamProperties(properties, channel.GetMimeType(), channel.GetProperty("inputstream.ffmpegdirect.manifest_type"), channel.GetCatchupMode(), channel.IsCatchupTSStream(), effectiveStreamUrl, settings);

        if (channel.SupportsLiveStreamTimeshifting() && isChannelURL &&
            channel.GetProperty("inputstream.ffmpegdirect.stream_mode").empty() &&
            settings->AlwaysEnableTimeshiftModeIfMissing())
        {
          properties.emplace_back("inputstream.ffmpegdirect.stream_mode", "timeshift");
          // Set is_realtime_stream explicitly: true for HLS, false for everything else.
          // It must NEVER be omitted: ISF's Properties::m_isRealTimeStream is only
          // assigned when the property is present, so an absent property leaves it
          // reading indeterminate memory (observed as true), which disables ISF's
          // stream analysis and yields garbage codec parameters on H.264 TS.
          if (streamType == StreamType::HLS)
            properties.emplace_back("inputstream.ffmpegdirect.is_realtime_stream", "true");
          else
            properties.emplace_back("inputstream.ffmpegdirect.is_realtime_stream", "false");
        }

        // Force FFmpeg open mode so the |reconnect/rw_timeout/seekable protocol
        // options reach FFmpeg's HTTP protocol (CURL mode never invokes it). Respect
        // an explicit channel override if present.
        bool shouldForceFFmpeg = ShouldForceISFForFFmpegMode(streamType, effectiveStreamUrl,
                                                             channel.GetProperty("http-reconnect") == "true", settings);
        Logger::Log(LogLevel::LEVEL_INFO, "%s - Channel ISF explicit path: streamType=%d, hasHTTPReconnect=%s, shouldForceFFmpeg=%s, existingOpenMode='%s'",
                    __FUNCTION__, static_cast<int>(streamType),
                    (channel.GetProperty("http-reconnect") == "true") ? "true" : "false",
                    shouldForceFFmpeg ? "true" : "false",
                    channel.GetProperty("inputstream.ffmpegdirect.open_mode").c_str());
        if (shouldForceFFmpeg &&
            channel.GetProperty("inputstream.ffmpegdirect.open_mode").empty())
        {
          properties.emplace_back("inputstream.ffmpegdirect.open_mode", "ffmpeg");
          Logger::Log(LogLevel::LEVEL_INFO, "%s - Setting inputstream.ffmpegdirect.open_mode=ffmpeg for TS/Other stream", __FUNCTION__);
        }
      }
    }
    else
    {
      // Channel explicitly specifies another inputstream (e.g. inputstream.adaptive)
      properties.emplace_back(PVR_STREAM_PROPERTY_STREAMURL, streamURL);
    }
  }
  else
  {
    StreamType streamType = StreamUtils::GetStreamType(streamURL, channel.GetProperty(PVR_STREAM_PROPERTY_MIMETYPE), channel.IsCatchupTSStream());
    if (streamType == StreamType::OTHER_TYPE)
      streamType = StreamUtils::InspectStreamType(streamURL, channel.GetCatchupMode());

    // Using kodi's built in inputstreams
    if (!isISAdaptiveSet && StreamUtils::UseKodiInputstreams(streamType, settings))
    {
      std::string ffmpegStreamURL = StreamUtils::GetURLWithFFmpegReconnectOptions(streamURL, streamType, channel.GetProperty(PVR_STREAM_PROPERTY_INPUTSTREAM), channel.GetProperty("http-reconnect") == "true" , settings);

      properties.emplace_back(PVR_STREAM_PROPERTY_STREAMURL, ffmpegStreamURL);
      Logger::Log(LogLevel::LEVEL_INFO, "%s - Built-in inputstream path: STREAMURL = %s", __FUNCTION__, WebUtils::RedactUrl(ffmpegStreamURL).c_str());
      if (!channel.HasMimeType() && StreamUtils::HasMimeType(streamType))
        properties.emplace_back(PVR_STREAM_PROPERTY_MIMETYPE, StreamUtils::GetMimeType(streamType));

      bool shouldForceFFmpeg = ShouldForceISFForFFmpegMode(streamType, ffmpegStreamURL,
                                                            channel.GetProperty("http-reconnect") == "true", settings);
      Logger::Log(LogLevel::LEVEL_INFO, "%s - Built-in path: streamType=%d, useKodiInputstreams=true, shouldForceFFmpeg=%s", __FUNCTION__, static_cast<int>(streamType), shouldForceFFmpeg ? "true" : "false");

      if (streamType == StreamType::HLS || streamType == StreamType::TS || streamType == StreamType::OTHER_TYPE)
      {
        if (channel.IsCatchupSupported() && channel.CatchupSupportsTimeshifting() &&
            CheckInputstreamInstalledAndEnabled(CATCHUP_INPUTSTREAM_NAME))
        {
          properties.emplace_back(PVR_STREAM_PROPERTY_INPUTSTREAM, CATCHUP_INPUTSTREAM_NAME);
          // this property is required to force VideoPlayer for Radio channels
          properties.emplace_back("inputstream-player", "videodefaultplayer");
          SetFFmpegDirectManifestTypeStreamProperty(properties, channel.GetProperty("inputstream.ffmpegdirect.manifest_type"), streamURL, streamType);

          // Force FFmpeg open mode so the |reconnect/rw_timeout/seekable protocol
          // options reach FFmpeg's HTTP protocol (CURL mode never invokes it).
          if (ShouldForceISFForFFmpegMode(streamType, ffmpegStreamURL,
                                          channel.GetProperty("http-reconnect") == "true", settings) &&
              channel.GetProperty("inputstream.ffmpegdirect.open_mode").empty())
          {
            properties.emplace_back("inputstream.ffmpegdirect.open_mode", "ffmpeg");
            Logger::Log(LogLevel::LEVEL_INFO, "%s - Catchup path: setting open_mode=ffmpeg for streamType=%d", __FUNCTION__, static_cast<int>(streamType));
          }
        }
        else if (channel.SupportsLiveStreamTimeshifting() && isChannelURL &&
                 CheckInputstreamInstalledAndEnabled(INPUTSTREAM_FFMPEGDIRECT))
        {
          properties.emplace_back(PVR_STREAM_PROPERTY_INPUTSTREAM, INPUTSTREAM_FFMPEGDIRECT);
          // this property is required to force VideoPlayer for Radio channels
          properties.emplace_back("inputstream-player", "videodefaultplayer");
          SetFFmpegDirectManifestTypeStreamProperty(properties, channel.GetProperty("inputstream.ffmpegdirect.manifest_type"), streamURL, streamType);
          properties.emplace_back("inputstream.ffmpegdirect.stream_mode", "timeshift");
          // Set explicitly (true for HLS, false otherwise) — see the ISF explicit path
          // comment above for why omitting the property is not an option.
          if (streamType == StreamType::HLS)
            properties.emplace_back("inputstream.ffmpegdirect.is_realtime_stream", "true");
          else
            properties.emplace_back("inputstream.ffmpegdirect.is_realtime_stream", "false");

          // Force FFmpeg open mode so the |reconnect/rw_timeout/seekable protocol
          // options reach FFmpeg's HTTP protocol (CURL mode never invokes it).
          if (ShouldForceISFForFFmpegMode(streamType, ffmpegStreamURL,
                                          channel.GetProperty("http-reconnect") == "true", settings) &&
              channel.GetProperty("inputstream.ffmpegdirect.open_mode").empty())
          {
            properties.emplace_back("inputstream.ffmpegdirect.open_mode", "ffmpeg");
            Logger::Log(LogLevel::LEVEL_INFO, "%s - Timeshift path: setting open_mode=ffmpeg for streamType=%d", __FUNCTION__, static_cast<int>(streamType));
          }
        }
        else if (streamType == StreamType::HLS || streamType == StreamType::TS || streamType == StreamType::OTHER_TYPE)
        {
          properties.emplace_back(PVR_STREAM_PROPERTY_INPUTSTREAM, PVR_STREAM_PROPERTY_VALUE_INPUTSTREAMFFMPEG);
        }
      }
    }
    else // inputstream.adaptive
    {
      CheckInputstreamInstalledAndEnabled(INPUTSTREAM_ADAPTIVE);

      bool streamUrlSet = false;

      // If no media headers are explicitly set for inputstream.adaptive,
      // strip the headers from streamURL and put it to media headers property

      if (channel.GetProperty("inputstream.adaptive.manifest_headers").empty() &&
          channel.GetProperty("inputstream.adaptive.stream_headers").empty() &&
          channel.GetProperty("inputstream.adaptive.common_headers").empty())
      {
        // No stream headers declared by property, check if stream URL has any
        std::string url;
        std::string encodedProtocolOptions;
        if (SplitUrlProtocolOpts(streamURL, url, encodedProtocolOptions))
        {
          // Set stream URL without headers and encoded headers as property
          properties.emplace_back(PVR_STREAM_PROPERTY_STREAMURL, url);
          properties.emplace_back("inputstream.adaptive.common_headers", encodedProtocolOptions);
          streamUrlSet = true;
        }
      }

      // Set intact stream URL if not previously set
      if (!streamUrlSet)
        properties.emplace_back(PVR_STREAM_PROPERTY_STREAMURL, streamURL);

      if (!isISAdaptiveSet)
        properties.emplace_back(PVR_STREAM_PROPERTY_INPUTSTREAM, INPUTSTREAM_ADAPTIVE);

      if (streamType == StreamType::HLS || streamType == StreamType::DASH ||
          streamType == StreamType::SMOOTH_STREAMING)
        properties.emplace_back(PVR_STREAM_PROPERTY_MIMETYPE, StreamUtils::GetMimeType(streamType));
    }
  }

  if (!channel.GetProperties().empty())
  {
    for (auto& prop : channel.GetProperties())
      properties.emplace_back(prop.first, prop.second);
  }

  if (!catchupProperties.empty())
  {
    for (auto& prop : catchupProperties)
      properties.emplace_back(prop.first, prop.second);
  }

  // Log all final stream properties for debugging reconnect issues
  std::string propList;
  for (size_t i = 0; i < properties.size(); i++)
  {
    const std::string& propName = properties[i].GetName();
    std::string propValue = properties[i].GetValue();
    if (propName == PVR_STREAM_PROPERTY_STREAMURL)
      propValue = WebUtils::RedactUrl(propValue);
    if (i > 0)
      propList += ", ";
    propList += propName + "=" + propValue;
  }
  Logger::Log(LogLevel::LEVEL_INFO, "%s - Final stream properties (%zu total): %s", __FUNCTION__, properties.size(), propList.c_str());
}

void StreamUtils::SetAllStreamProperties(std::vector<kodi::addon::PVRStreamProperty>& properties, const iptvsimple::data::MediaEntry& mediaEntry, const std::string& streamURL, std::shared_ptr<InstanceSettings>& settings)
{
  // Check if the media entry has explicitly set up the use of inputstream.adaptive,
  // if so, the best behaviour for media services is:
  // - Always add mimetype to prevent kodi core to make an HTTP HEADER requests
  //   this because in some cases services refuse this request and can also deny downloads
  // - If requested by settings, always add the "user-agent" header to ISA properties
  const bool isISAdaptiveSet =
      mediaEntry.GetProperty(PVR_STREAM_PROPERTY_INPUTSTREAM) == INPUTSTREAM_ADAPTIVE;

  if (!isISAdaptiveSet && !mediaEntry.GetInputStreamName().empty())
  {
    const std::string& inputstreamName = mediaEntry.GetInputStreamName();
    if (inputstreamName != PVR_STREAM_PROPERTY_VALUE_INPUTSTREAMFFMPEG)
      CheckInputstreamInstalledAndEnabled(inputstreamName);

    if (inputstreamName == INPUTSTREAM_FFMPEGDIRECT || inputstreamName == PVR_STREAM_PROPERTY_VALUE_INPUTSTREAMFFMPEG)
    {
      // Build the reconnect-augmented URL for FFmpeg-based inputstreams so stalled/EOF'd
      // live streams auto-reconnect instead of hanging (see GetURLWithFFmpegReconnectOptions).
      StreamType streamType = StreamUtils::GetStreamType(streamURL, mediaEntry.GetMimeType(), false);
      if (streamType == StreamType::OTHER_TYPE)
        streamType = StreamUtils::InspectStreamType(streamURL, CatchupMode::DISABLED);
      std::string effectiveStreamUrl = StreamUtils::GetURLWithFFmpegReconnectOptions(
          streamURL, streamType, inputstreamName,
          mediaEntry.GetProperty("http-reconnect") == "true", settings);
      properties.emplace_back(PVR_STREAM_PROPERTY_STREAMURL, effectiveStreamUrl);

      if (inputstreamName == INPUTSTREAM_FFMPEGDIRECT)
      {
        InspectAndSetFFmpegDirectStreamProperties(properties, mediaEntry.GetMimeType(), mediaEntry.GetProperty("inputstream.ffmpegdirect.manifest_type"), CatchupMode::DISABLED, false, effectiveStreamUrl, settings);

        // Force FFmpeg open mode so the |reconnect/rw_timeout/seekable protocol
        // options reach FFmpeg's HTTP protocol (CURL mode never invokes it). Respect
        // an explicit media entry override if present.
        bool shouldForceFFmpeg = ShouldForceISFForFFmpegMode(streamType, effectiveStreamUrl,
                                                             mediaEntry.GetProperty("http-reconnect") == "true", settings);
        Logger::Log(LogLevel::LEVEL_INFO, "%s - MediaEntry ISF explicit path: streamType=%d, hasHTTPReconnect=%s, shouldForceFFmpeg=%s, existingOpenMode='%s'",
                    __FUNCTION__, static_cast<int>(streamType),
                    (mediaEntry.GetProperty("http-reconnect") == "true") ? "true" : "false",
                    shouldForceFFmpeg ? "true" : "false",
                    mediaEntry.GetProperty("inputstream.ffmpegdirect.open_mode").c_str());
        if (shouldForceFFmpeg &&
            mediaEntry.GetProperty("inputstream.ffmpegdirect.open_mode").empty())
        {
          properties.emplace_back("inputstream.ffmpegdirect.open_mode", "ffmpeg");
          Logger::Log(LogLevel::LEVEL_INFO, "%s - Setting inputstream.ffmpegdirect.open_mode=ffmpeg for MediaEntry TS/Other stream", __FUNCTION__);
        }
      }
    }
    else
    {
      // Media entry explicitly specifies another inputstream (e.g. inputstream.adaptive)
      properties.emplace_back(PVR_STREAM_PROPERTY_STREAMURL, streamURL);
    }
  }
  else
  {
    StreamType streamType = StreamUtils::GetStreamType(streamURL, mediaEntry.GetProperty(PVR_STREAM_PROPERTY_MIMETYPE), false);
    if (streamType == StreamType::OTHER_TYPE)
      streamType = StreamUtils::InspectStreamType(streamURL, CatchupMode::DISABLED);

    // Using kodi's built in inputstreams
    if (!isISAdaptiveSet && StreamUtils::UseKodiInputstreams(streamType, settings))
    {
      std::string ffmpegStreamURL = StreamUtils::GetURLWithFFmpegReconnectOptions(streamURL, streamType, mediaEntry.GetProperty(PVR_STREAM_PROPERTY_INPUTSTREAM), mediaEntry.GetProperty("http-reconnect") == "true" , settings);

      properties.emplace_back(PVR_STREAM_PROPERTY_STREAMURL, ffmpegStreamURL);
      if (!mediaEntry.HasMimeType() && StreamUtils::HasMimeType(streamType))
        properties.emplace_back(PVR_STREAM_PROPERTY_MIMETYPE, StreamUtils::GetMimeType(streamType));

      if (streamType == StreamType::HLS || streamType == StreamType::TS || streamType == StreamType::OTHER_TYPE)
      {
        properties.emplace_back(PVR_STREAM_PROPERTY_INPUTSTREAM, PVR_STREAM_PROPERTY_VALUE_INPUTSTREAMFFMPEG);

        // For MediaEntry built-in inputstream.ffmpeg paths, also set open_mode=ffmpeg
        // when using ISF with reconnect enabled — this handles VOD/media TS streams
        // that don't specify an explicit inputstream.
        bool shouldForceFFmpeg = ShouldForceISFForFFmpegMode(streamType, ffmpegStreamURL,
                                                            mediaEntry.GetProperty("http-reconnect") == "true", settings);
        if (shouldForceFFmpeg &&
            mediaEntry.GetProperty("inputstream.ffmpegdirect.open_mode").empty())
        {
          properties.emplace_back("inputstream.ffmpegdirect.open_mode", "ffmpeg");
          Logger::Log(LogLevel::LEVEL_INFO, "%s - MediaEntry built-in path: setting open_mode=ffmpeg for streamType=%d", __FUNCTION__, static_cast<int>(streamType));
        }
      }
    }
    else // inputstream.adaptive
    {
      CheckInputstreamInstalledAndEnabled(INPUTSTREAM_ADAPTIVE);

      bool streamUrlSet = false;

      // If no media headers are explicitly set for inputstream.adaptive,
      // strip the headers from streamURL and put it to media headers property

      if (mediaEntry.GetProperty("inputstream.adaptive.manifest_headers").empty() &&
          mediaEntry.GetProperty("inputstream.adaptive.stream_headers").empty())
      {
        // No stream headers declared by property, check if stream URL has any
        std::string url;
        std::string encodedProtocolOptions;
        if (SplitUrlProtocolOpts(streamURL, url, encodedProtocolOptions))
        {
          // Set stream URL without headers and encoded headers as property
          properties.emplace_back(PVR_STREAM_PROPERTY_STREAMURL, url);
          properties.emplace_back("inputstream.adaptive.manifest_headers", encodedProtocolOptions);
          properties.emplace_back("inputstream.adaptive.stream_headers", encodedProtocolOptions);
          streamUrlSet = true;
        }
      }

      // Set intact stream URL if not previously set
      if (!streamUrlSet)
        properties.emplace_back(PVR_STREAM_PROPERTY_STREAMURL, streamURL);

      if (!isISAdaptiveSet)
        properties.emplace_back(PVR_STREAM_PROPERTY_INPUTSTREAM, INPUTSTREAM_ADAPTIVE);

      if (streamType == StreamType::HLS || streamType == StreamType::DASH ||
          streamType == StreamType::SMOOTH_STREAMING)
        properties.emplace_back(PVR_STREAM_PROPERTY_MIMETYPE, StreamUtils::GetMimeType(streamType));
    }
  }

  if (!mediaEntry.GetProperties().empty())
  {
    for (auto& prop : mediaEntry.GetProperties())
      properties.emplace_back(prop.first, prop.second);
  }

  // Log all final stream properties for debugging reconnect issues
  std::string propList;
  for (size_t i = 0; i < properties.size(); i++)
  {
    const std::string& propName = properties[i].GetName();
    std::string propValue = properties[i].GetValue();
    if (propName == PVR_STREAM_PROPERTY_STREAMURL)
      propValue = WebUtils::RedactUrl(propValue);
    if (i > 0)
      propList += ", ";
    propList += propName + "=" + propValue;
  }
  Logger::Log(LogLevel::LEVEL_INFO, "%s - Final stream properties (%zu total): %s", __FUNCTION__, properties.size(), propList.c_str());
}

bool StreamUtils::CheckInputstreamInstalledAndEnabled(const std::string& inputstreamName)
{
  std::string version;
  bool enabled;

  if (kodi::IsAddonAvailable(inputstreamName, version, enabled))
  {
    if (!enabled)
    {
      std::string message = StringUtils::Format(kodi::addon::GetLocalizedString(30502).c_str(), inputstreamName.c_str());
      kodi::QueueNotification(QueueMsg::QUEUE_ERROR, kodi::addon::GetLocalizedString(30500), message);
    }
  }
  else // Not installed
  {
    std::string message = StringUtils::Format(kodi::addon::GetLocalizedString(30501).c_str(), inputstreamName.c_str());
    kodi::QueueNotification(QueueMsg::QUEUE_ERROR, kodi::addon::GetLocalizedString(30500), message);
  }

  return true;
}

void StreamUtils::InspectAndSetFFmpegDirectStreamProperties(std::vector<kodi::addon::PVRStreamProperty>& properties, const std::string& mimeType, const std::string& manifestType, CatchupMode catchupMode, bool isCatchupTSStream, const std::string& streamURL, std::shared_ptr<InstanceSettings>& settings)
{
  // If there is no MIME type and no manifest type (BOTH!) set then potentially inspect the stream and set them
  if (!mimeType.empty() && !manifestType.empty())
  {
    StreamType streamType = StreamUtils::GetStreamType(streamURL, mimeType, isCatchupTSStream);
    if (streamType == StreamType::OTHER_TYPE)
      streamType = StreamUtils::InspectStreamType(streamURL, catchupMode);

    if (mimeType.empty() && StreamUtils::HasMimeType(streamType))
      properties.emplace_back(PVR_STREAM_PROPERTY_MIMETYPE, StreamUtils::GetMimeType(streamType));

    SetFFmpegDirectManifestTypeStreamProperty(properties, manifestType, streamURL, streamType);
  }
}

void StreamUtils::SetFFmpegDirectManifestTypeStreamProperty(std::vector<kodi::addon::PVRStreamProperty>& properties, const std::string& manifestType, const std::string& streamURL, const StreamType& streamType)
{
  std::string newManifestType;
  if (manifestType.empty())
    newManifestType = StreamUtils::GetManifestType(streamType);
  if (!newManifestType.empty())
    properties.emplace_back("inputstream.ffmpegdirect.manifest_type", newManifestType);
}

std::string StreamUtils::GetEffectiveInputStreamName(const StreamType& streamType, const iptvsimple::data::Channel& channel, std::shared_ptr<InstanceSettings>& settings)
{
  std::string inputStreamName = channel.GetInputStreamName();

  if (inputStreamName.empty())
  {
    if (StreamUtils::UseKodiInputstreams(streamType, settings))
    {
      if (streamType == StreamType::HLS || streamType == StreamType::TS || streamType == StreamType::OTHER_TYPE)
      {
        if (channel.IsCatchupSupported() && channel.CatchupSupportsTimeshifting())
          inputStreamName = CATCHUP_INPUTSTREAM_NAME;
        else
          inputStreamName = PVR_STREAM_PROPERTY_VALUE_INPUTSTREAMFFMPEG;
      }
    }
    else // inputstream.adpative
    {
      inputStreamName = "inputstream.adaptive";
    }
  }

  return inputStreamName;
}

const StreamType StreamUtils::GetStreamType(const std::string& url, const std::string& mimeType, bool isCatchupTSStream)
{
  if (StringUtils::StartsWith(url, "plugin://"))
    return StreamType::PLUGIN;

  if (url.find(".m3u8") != std::string::npos ||
      mimeType == "application/x-mpegURL" ||
      mimeType == "application/vnd.apple.mpegurl")
    return StreamType::HLS;

  if (url.find(".mpd") != std::string::npos || mimeType == "application/xml+dash")
    return StreamType::DASH;

  if (url.find(".ism") != std::string::npos &&
      !(url.find(".ismv") != std::string::npos || url.find(".isma") != std::string::npos))
    return StreamType::SMOOTH_STREAMING;

  if (url.find(".ts") != std::string::npos || mimeType == "video/mp2t" || isCatchupTSStream)
    return StreamType::TS;

  // it has a MIME type but not one we recognise
  if (!mimeType.empty())
    return StreamType::MIME_TYPE_UNRECOGNISED;

  return StreamType::OTHER_TYPE;
}

const StreamType StreamUtils::InspectStreamType(const std::string& url, CatchupMode catchupMode)
{
  if (!FileUtils::FileExists(url))
    return StreamType::OTHER_TYPE;

  int httpCode = 0;
  const std::string source = WebUtils::ReadFileContentsStartOnly(url, &httpCode);

  if (httpCode == 200)
  {
    if (StringUtils::StartsWith(source, "#EXTM3U") && (source.find("#EXT-X-STREAM-INF") != std::string::npos || source.find("#EXT-X-VERSION") != std::string::npos))
      return StreamType::HLS;

    if (source.find("<MPD") != std::string::npos)
      return StreamType::DASH;

    if (source.find("<SmoothStreamingMedia") != std::string::npos)
      return StreamType::SMOOTH_STREAMING;
  }

  // If we can't inspect the stream type the only option left for default, append or shift mode is TS
  if (catchupMode == CatchupMode::DEFAULT ||
      catchupMode == CatchupMode::APPEND ||
      catchupMode == CatchupMode::SHIFT ||
      catchupMode == CatchupMode::TIMESHIFT)
    return StreamType::TS;

  return StreamType::OTHER_TYPE;
}

const std::string StreamUtils::GetManifestType(const StreamType& streamType)
{
  switch (streamType)
  {
    case StreamType::HLS:
      return "hls";
    case StreamType::DASH:
      return "mpd";
    case StreamType::SMOOTH_STREAMING:
      return "ism";
    default:
      return "";
  }
}

const std::string StreamUtils::GetMimeType(const StreamType& streamType)
{
  switch (streamType)
  {
    case StreamType::HLS:
      return "application/x-mpegURL";
    case StreamType::DASH:
      return "application/xml+dash";
    case StreamType::SMOOTH_STREAMING:
      return "application/vnd.ms-sstr+xml";
    case StreamType::TS:
      return "video/mp2t";
    default:
      return "";
  }
}

bool StreamUtils::HasMimeType(const StreamType& streamType)
{
  return streamType != StreamType::OTHER_TYPE && streamType != StreamType::SMOOTH_STREAMING;
}

// Note: FFmpeg protocol options MUST be transported as Kodi protocol options
// (|key=value&key2=value2), not as URL query parameters (?key=value).
// Kodi's CURL::Parse routes '?query' into GetOptions() and only the '|'-pipe part
// into GetProtocolOptions(); both ISF (FFmpegStream::GetFFMpegOptionsFromInput) and
// Kodi's built-in demuxer (DVDDemuxFFmpeg::GetFFMpegOptionsFromInput) build their
// av_dict exclusively from GetProtocolOptions(), and FFmpeg itself never parses
// query strings as options. Query params therefore reach neither FFmpeg nor the
// whitelist — they are just sent to the HTTP server, which ignores them.
std::string StreamUtils::GetURLWithFFmpegReconnectOptions(const std::string& streamUrl, const StreamType& streamType, const std::string& inputstreamName, bool hasHTTPReconnect, std::shared_ptr<InstanceSettings>& settings)
{
  std::string newStreamUrl = streamUrl;

  if (WebUtils::IsHttpUrl(streamUrl) && SupportsFFmpegReconnect(streamType, inputstreamName) &&
      (hasHTTPReconnect || settings->UseFFmpegReconnect()))
  {
    newStreamUrl = AddProtocolOptionToStreamUrl(newStreamUrl, "reconnect", "1");
    // reconnect_at_eof triggers reconnection when the server sends a graceful EOF
    // (TCP FIN / HTTP connection close). Without it, FFmpeg returns EOF to the caller
    // instead of reconnecting — this is the primary reason live TS streams stall when
    // the server periodically closes connections. reconnect_streamed=1 (set below) tells
    // FFmpeg the URL is non-seekable, so reconnect_at_eof will reconnect to the live
    // edge without seeking to the beginning.
    //
    // HLS is excluded when ISF is used because ISF fetches segments itself and handles
    // EOF internally. But for Kodi's built-in inputstream.ffmpeg (libavformat HLS
    // demuxer), reconnect_at_eof is beneficial — the demuxer passes the URL directly to
    // FFmpeg's http protocol, which can reconnect on manifest-level EOF.
    if (streamType != StreamType::HLS || inputstreamName == PVR_STREAM_PROPERTY_VALUE_INPUTSTREAMFFMPEG)
      newStreamUrl = AddProtocolOptionToStreamUrl(newStreamUrl, "reconnect_at_eof", "1");
    newStreamUrl = AddProtocolOptionToStreamUrl(newStreamUrl, "reconnect_streamed", "1");
    newStreamUrl = AddProtocolOptionToStreamUrl(newStreamUrl, "reconnect_delay_max", std::to_string(settings->GetReconnectDelayMaxSecs()));

    if (settings->ReconnectOnNetworkError())
      newStreamUrl = AddProtocolOptionToStreamUrl(newStreamUrl, "reconnect_on_network_error", "1");

    if (settings->ReconnectOnHttpError())
      // FFmpeg's reconnect_on_http_error takes a comma-separated list of status
      // codes/patterns (matched via av_match_list), not a boolean.
      newStreamUrl = AddProtocolOptionToStreamUrl(newStreamUrl, "reconnect_on_http_error", "4xx,5xx");

    // Read timeout (microseconds) so a stall on an otherwise open socket forces a drop -> reconnect,
    // instead of hanging forever at a low buffer percentage. Requires the patched ISF
    // whitelist (rw_timeout is not in stock ISF's option list).
    if (settings->ReconnectReadTimeoutSecs() > 0)
      newStreamUrl = AddProtocolOptionToStreamUrl(newStreamUrl, "rw_timeout",
          std::to_string(settings->ReconnectReadTimeoutSecs() * 1000000));

    // Force non-seekable at the FFmpeg AVIO layer. seekable is a valid FFmpeg http/https
    // protocol AVOption. Passed here as a Kodi protocol option (|seekable=0) — the only
    // format that reaches FFmpeg's option dict via ISF/Kodi whitelists. Without this,
    // reconnect_at_eof can still seek to the beginning of seekable URLs even when
    // reconnect_streamed=1 is set.
    newStreamUrl = AddProtocolOptionToStreamUrl(newStreamUrl, "seekable", "0");

    Logger::Log(LogLevel::LEVEL_INFO, "%s - FFmpeg Reconnect enabled. Stream Type: %d, InputStream: '%s', Final URL: %s", __FUNCTION__, static_cast<int>(streamType), inputstreamName.c_str(), WebUtils::RedactUrl(newStreamUrl).c_str());
  }

  return newStreamUrl;
}

std::string StreamUtils::AddQueryToStreamUrl(const std::string& streamUrl, const std::string& paramName, const std::string& paramValue)
{
  std::string newUrl = streamUrl;

  // Check if this param already exists in query string (before any | if present)
  size_t pipePos = newUrl.find('|');
  size_t searchEnd = (pipePos != std::string::npos) ? pipePos : newUrl.length();

  // Check if param exists as a proper query parameter (preceded by ? or &)
  // This avoids false matches like "reconnect=" matching inside "myreconnect="
  bool paramExists = false;
  size_t pos = newUrl.find(paramName + "=");
  while (pos != std::string::npos && pos < searchEnd)
  {
    if (pos == 0 || newUrl[pos - 1] == '?' || newUrl[pos - 1] == '&')
    {
      paramExists = true;
      break;
    }
    pos = newUrl.find(paramName + "=", pos + 1);
  }

  if (!paramExists)
  {
    // Preserve protocol options (headers after |). Insert query params before
    // the | separator.
    std::string protocolOptions;
    if (pipePos != std::string::npos)
    {
      protocolOptions = newUrl.substr(pipePos);
      newUrl = newUrl.substr(0, pipePos);
    }

    // Add query parameter - use ? if no existing query, & if query exists
    size_t questionMark = newUrl.find('?');
    if (questionMark != std::string::npos)
      newUrl += "&";
    else
      newUrl += "?";
    newUrl += paramName + "=" + paramValue;

    // Re-append protocol options
    newUrl += protocolOptions;
  }

  return newUrl;
}

std::string StreamUtils::AddProtocolOptionToStreamUrl(const std::string& streamUrl, const std::string& optionName, const std::string& optionValue)
{
  std::string newUrl = streamUrl;

  // Kodi protocol options live after the '|' separator as key=value pairs joined by '&'.
  // CURL::Parse routes this segment into GetProtocolOptions(), which is what ISF's and
  // Kodi's GetFFMpegOptionsFromInput() whitelists read — the only transport through
  // which FFmpeg protocol options (reconnect*, rw_timeout, seekable) reach FFmpeg.
  size_t pipePos = newUrl.find('|');
  if (pipePos == std::string::npos)
    return newUrl + "|" + optionName + "=" + optionValue;

  // Check if this option already exists in the pipe segment (preceded by | or &)
  // to avoid false matches like "reconnect=" matching inside "myreconnect="
  bool optionExists = false;
  size_t pos = newUrl.find(optionName + "=", pipePos);
  while (pos != std::string::npos)
  {
    if (newUrl[pos - 1] == '|' || newUrl[pos - 1] == '&')
    {
      optionExists = true;
      break;
    }
    pos = newUrl.find(optionName + "=", pos + 1);
  }

  if (!optionExists)
    newUrl += "&" + optionName + "=" + optionValue;

  return newUrl;
}

std::string StreamUtils::AddHeaderToStreamUrl(const std::string& streamUrl, const std::string& headerName, const std::string& headerValue)
{
  return StreamUtils::AddHeader(streamUrl, headerName, headerValue, false);
}

std::string StreamUtils::AddHeader(const std::string& headerTarget, const std::string& headerName, const std::string& headerValue, bool encodeHeaderValue)
{
  std::string newHeaderTarget = headerTarget;

  bool hasProtocolOptions = false;
  bool addHeader = true;
  size_t found = newHeaderTarget.find("|");

  if (found != std::string::npos)
  {
    hasProtocolOptions = true;
    addHeader = newHeaderTarget.find(headerName + "=", found + 1) == std::string::npos;
  }

  if (addHeader)
  {
    if (!hasProtocolOptions)
      newHeaderTarget += "|";
    else
      newHeaderTarget += "&";

    newHeaderTarget += headerName + "=" + (encodeHeaderValue ? WebUtils::UrlEncode(headerValue) : headerValue);
  }

  return newHeaderTarget;
}

bool StreamUtils::UseKodiInputstreams(const StreamType& streamType, std::shared_ptr<iptvsimple::InstanceSettings>& settings)
{
  return streamType == StreamType::OTHER_TYPE || streamType == StreamType::TS || streamType == StreamType::PLUGIN ||
        (streamType == StreamType::HLS && !settings->UseInputstreamAdaptiveforHls());
}

bool StreamUtils::ChannelSpecifiesInputstream(const iptvsimple::data::Channel& channel)
{
  return !channel.GetInputStreamName().empty();
}

// ISF (inputstream.ffmpegdirect) defaults to CURL mode for non-HLS streams (plain
// TS over HTTPS). In CURL mode, FFmpeg's HTTP protocol is never invoked — data
// flows through Kodi's cURL layer via AVIOContext callbacks, so FFmpeg protocol
// options (reconnect*, rw_timeout, seekable) passed as | protocol options would
// not apply (Kodi's cURL layer ignores them; it has its own low-speed abort).
// Forcing open_mode=ffmpeg makes ISF call avformat_open_input directly, so the
// pipe-transported options are applied by FFmpeg's HTTP protocol and reconnect
// actually engages.
//
// HLS is excluded — ISF already auto-selects FFMPEG mode for HLS (detected via
// mimetype "application/x-mpegURL" or manifest_type "hls"), so the property
// would be redundant.
bool StreamUtils::ShouldForceISFForFFmpegMode(const StreamType& streamType, const std::string& streamUrl, bool hasHTTPReconnect, std::shared_ptr<iptvsimple::InstanceSettings>& settings)
{
  return (streamType == StreamType::TS || streamType == StreamType::OTHER_TYPE) &&
         WebUtils::IsHttpUrl(streamUrl) &&
         (hasHTTPReconnect || settings->UseFFmpegReconnect());
}

bool StreamUtils::SupportsFFmpegReconnect(const StreamType& streamType, const std::string& inputstreamName)
{
  return streamType == StreamType::HLS ||
         streamType == StreamType::TS ||
         streamType == StreamType::OTHER_TYPE ||
         inputstreamName == PVR_STREAM_PROPERTY_VALUE_INPUTSTREAMFFMPEG ||
         inputstreamName == INPUTSTREAM_FFMPEGDIRECT;
}

std::string StreamUtils::GetUrlEncodedProtocolOptions(const std::string& protocolOptions)
{
  std::string encodedProtocolOptions = "";

  std::vector<std::string> headers = StringUtils::Split(protocolOptions, "&");
  for (std::string header : headers)
  {
    std::string::size_type pos(header.find('='));
    if(pos == std::string::npos)
      continue;

    encodedProtocolOptions = StreamUtils::AddHeader(encodedProtocolOptions, header.substr(0, pos), header.substr(pos + 1), true);
  }

  // We'll return the protocol options without the leading '|'
  if (!encodedProtocolOptions.empty() && encodedProtocolOptions[0] == '|')
    encodedProtocolOptions.erase(0, 1);

  return encodedProtocolOptions;
}

std::string StreamUtils::WebStreamExtractor(const std::string& webUrl,
                                            const iptvsimple::data::Channel& currentChannel)
{
  bool isWebUrl = currentChannel.GetProperty("isWebUrl") == "true";
  if (isWebUrl)
  {
    std::string webPattern = currentChannel.GetProperty("web-regex");
    std::string webHeaders = currentChannel.GetProperty("web-headers");
    std::string mediaHeaders;
    std::string tempUrl = webUrl;
    size_t pos = tempUrl.find('|');

    if (pos != std::string::npos)
    {
      mediaHeaders = tempUrl.substr(pos);
      tempUrl = tempUrl.substr(0, pos);
    }

    std::string extractedUrl =
        WebStreamExtractor::ExtractStreamUrl(tempUrl, webPattern, webHeaders, false);
    Logger::Log(LEVEL_DEBUG,
                "%s - Extracted URL: '%s', webPattern: '%s', webHeaders: '%s', webUrl: '%s'",
                __FUNCTION__, extractedUrl.c_str(), webPattern.c_str(), webHeaders.c_str(),
                tempUrl.c_str());

    if (!extractedUrl.empty() && !mediaHeaders.empty())
      extractedUrl += mediaHeaders;

    return extractedUrl;
  }
  return webUrl;
}

std::string StreamUtils::WebStreamExtractor(const std::string& webUrl,
                                            const iptvsimple::data::MediaEntry& currentMediaEntry)
{
  bool isWebUrl = currentMediaEntry.GetProperty("isWebUrl") == "true";
  if (isWebUrl)
  {
    std::string webPattern = currentMediaEntry.GetProperty("web-regex");
    std::string webHeaders = currentMediaEntry.GetProperty("web-headers");
    std::string mediaHeaders;
    std::string tempUrl = webUrl;
    size_t pos = tempUrl.find('|');
    
    if (pos != std::string::npos)
    {
      mediaHeaders = tempUrl.substr(pos);
      tempUrl = tempUrl.substr(0, pos);
    }

    std::string extractedUrl =
        WebStreamExtractor::ExtractStreamUrl(webUrl, webPattern, webHeaders, true);
    Logger::Log(
        LEVEL_DEBUG, "%s - Extracted URL: '%s', webPattern: '%s', webHeaders: '%s', webUrl: '%s'",
        __FUNCTION__, extractedUrl.c_str(), webPattern.c_str(), webHeaders.c_str(), webUrl.c_str());

    if (!extractedUrl.empty() && !mediaHeaders.empty())    
      extractedUrl += mediaHeaders;
    
    return extractedUrl;
  }
  return webUrl;
}
