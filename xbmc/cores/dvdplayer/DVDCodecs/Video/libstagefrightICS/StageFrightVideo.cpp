/*
 *      Copyright (C) 2013 Team XBMC
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */
/***************************************************************************/

//#define DEBUG_VERBOSE 1

#include "system.h"
#include "system_gl.h"

#include "DVDVideoCodecStageFright.h"
#include "StageFrightVideo.h"
#include "StageFrightVideoPrivate.h"

#include "DVDClock.h"
#include "utils/log.h"
#include "utils/fastmemcpy.h"
#include "threads/Thread.h"
#include "threads/Event.h"
#include "android/jni/Build.h"
#include "settings/AdvancedSettings.h"
#include "DVDCodecs/DVDCodecInterface.h"
#include "cores/VideoRenderers/RenderManager.h"

#include "xbmc/guilib/FrameBufferObject.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include "windowing/egl/EGLWrapper.h"
#include "windowing/WindowingFactory.h"

#include <new>

#define OMX_QCOM_COLOR_FormatYVU420SemiPlanar 0x7FA30C00
#define OMX_TI_COLOR_FormatYUV420PackedSemiPlanar 0x7F000100

#define CLASSNAME "CStageFrightVideo"

#define EGL_NATIVE_BUFFER_ANDROID 0x3140
#define EGL_IMAGE_PRESERVED_KHR   0x30D2

using namespace android;

static int64_t pts_dtoi(double pts)
{
  return (int64_t)(pts);
}

/***********************************************************/

class CStageFrightMediaSource : public MediaSource
{
public:
  CStageFrightMediaSource(CStageFrightVideoPrivate *priv, sp<MetaData> meta)
  {
    p = priv;
    source_meta = meta;
  }

  virtual sp<MetaData> getFormat()
  {
    return source_meta;
  }

  virtual status_t start(MetaData *params)
  {
    return OK;
  }

  virtual status_t stop()
  {
    return OK;
  }

  virtual status_t read(MediaBuffer **buffer,
                        const MediaSource::ReadOptions *options)
  {
    Frame *frame;
    status_t ret;
    *buffer = NULL;
    int64_t time_us = -1;
    MediaSource::ReadOptions::SeekMode mode;

    if (options && options->getSeekTo(&time_us, &mode))
    {
#if defined(DEBUG_VERBOSE)
      CLog::Log(LOGDEBUG, "%s: reading source(%d): seek:%llu\n", CLASSNAME,p->in_queue.size(), time_us);
#endif
    }
    else
    {
#if defined(DEBUG_VERBOSE)
      CLog::Log(LOGDEBUG, "%s: reading source(%d)\n", CLASSNAME,p->in_queue.size());
#endif
    }

    p->in_mutex.lock();
    while (p->in_queue.empty() && p->decode_thread)
      p->in_condition.wait(p->in_mutex);

    if (p->in_queue.empty())
    {
      p->in_mutex.unlock();
      return VC_ERROR;
    }

    std::list<Frame*>::iterator it = p->in_queue.begin();
    frame = *it;
    ret = frame->status;
    *buffer = frame->medbuf;

    p->in_queue.erase(it);
    p->in_mutex.unlock();

#if defined(DEBUG_VERBOSE)
    CLog::Log(LOGDEBUG, ">>> exiting reading source(%d); pts:%llu\n", p->in_queue.size(),frame->pts);
#endif

    free(frame);

    return ret;
  }

private:
  sp<MetaData> source_meta;
  CStageFrightVideoPrivate *p;
};

/********************************************/

class CStageFrightDecodeThread : public CThread
{
protected:
  CStageFrightVideoPrivate *p;

public:
  CStageFrightDecodeThread(CStageFrightVideoPrivate *priv)
  : CThread("CStageFrightDecodeThread")
  , p(priv)
  {}

  void OnStartup()
  {
  #if defined(DEBUG_VERBOSE)
    CLog::Log(LOGDEBUG, "%s: entering decode thread\n", CLASSNAME);
  #endif
  }

  void OnExit()
  {
  #if defined(DEBUG_VERBOSE)
    CLog::Log(LOGDEBUG, "%s: exited decode thread\n", CLASSNAME);
  #endif
  }

  void Process()
  {
    Frame* frame;
    int32_t w, h;
    int decode_done = 0;
    MediaSource::ReadOptions readopt;
    // GLuint texid;

    //SetPriority(THREAD_PRIORITY_ABOVE_NORMAL);
    do
    {
      #if defined(DEBUG_VERBOSE)
      unsigned int time = XbmcThreads::SystemClockMillis();
      CLog::Log(LOGDEBUG, "%s: >>> Handling frame\n", CLASSNAME);
      #endif
      p->cur_frame = NULL;
      frame = (Frame*)malloc(sizeof(Frame));
      if (!frame)
      {
        decode_done   = 1;
        continue;
      }

      frame->eglimg = EGL_NO_IMAGE_KHR;
      frame->medbuf = NULL;
      if (p->resetting)
      {
        readopt.setSeekTo(0);
        p->resetting = false;
      }
      frame->status = p->decoder->read(&frame->medbuf, &readopt);
      readopt.clearSeekTo();

      if (frame->status == OK)
      {
        if (!frame->medbuf->graphicBuffer().get())  // hw buffers
        {
          if (frame->medbuf->range_length() == 0)
          {
            CLog::Log(LOGERROR, "%s - Invalid buffer\n", CLASSNAME);
            frame->status = VC_ERROR;
            decode_done   = 1;
            frame->medbuf->release();
            frame->medbuf = NULL;
          }
          else
            frame->format = RENDER_FMT_YUV420P;
        }
        else
          frame->format = RENDER_FMT_EGLIMG;
      }

      if (frame->status == OK)
      {
        frame->width = p->width;
        frame->height = p->height;
        frame->pts = 0;

        sp<MetaData> outFormat = p->decoder->getFormat();
        outFormat->findInt32(kKeyWidth , &w);
        outFormat->findInt32(kKeyHeight, &h);
        frame->medbuf->meta_data()->findInt64(kKeyTime, &(frame->pts));
      }
      else if (frame->status == INFO_FORMAT_CHANGED)
      {
        int32_t cropLeft, cropTop, cropRight, cropBottom;
        sp<MetaData> outFormat = p->decoder->getFormat();

        outFormat->findInt32(kKeyWidth , &p->width);
        outFormat->findInt32(kKeyHeight, &p->height);

        cropLeft = cropTop = cropRight = cropBottom = 0;
        if (!outFormat->findRect(kKeyCropRect, &cropLeft, &cropTop, &cropRight, &cropBottom))
        {
          p->x = 0;
          p->y = 0;
        }
        else
        {
          p->x = cropLeft;
          p->y = cropTop;
          p->width = cropRight - cropLeft + 1;
          p->height = cropBottom - cropTop + 1;
        }
        outFormat->findInt32(kKeyColorFormat, &p->videoColorFormat);
        if (!outFormat->findInt32(kKeyStride, &p->videoStride))
          p->videoStride = p->width;
        if (!outFormat->findInt32(kKeySliceHeight, &p->videoSliceHeight))
          p->videoSliceHeight = p->height;

#if defined(DEBUG_VERBOSE)
        CLog::Log(LOGDEBUG, ">>> new format col:%d, w:%d, h:%d, sw:%d, sh:%d, ctl:%d,%d; cbr:%d,%d\n", p->videoColorFormat, p->width, p->height, p->videoStride, p->videoSliceHeight, cropTop, cropLeft, cropBottom, cropRight);
#endif

        if (frame->medbuf)
          frame->medbuf->release();
        frame->medbuf = NULL;
        free(frame);
        continue;
      }
      else
      {
        CLog::Log(LOGERROR, "%s - decoding error (%d)\n", CLASSNAME,frame->status);
        if (frame->medbuf)
          frame->medbuf->release();
        frame->medbuf = NULL;
        free(frame);
        continue;
      }

      if (frame->format == RENDER_FMT_EGLIMG)
      {
        if (!p->eglInitialized)
        {
          p->InitializeEGL(frame->width, frame->height);
        }
        else if (p->texwidth != frame->width || p->texheight != frame->height)
        {
          p->ReleaseEGL();
          p->InitializeEGL(frame->width, frame->height);
        }

        ANativeWindowBuffer* graphicBuffer = frame->medbuf->graphicBuffer()->getNativeBuffer();
        native_window_set_buffers_timestamp(p->mVideoNativeWindow.get(), frame->pts * 1000);
        int err = p->mVideoNativeWindow.get()->queueBuffer(p->mVideoNativeWindow.get(), graphicBuffer);
        if (err == 0)
          frame->medbuf->meta_data()->setInt32(kKeyRendered, 1);
        frame->medbuf->release();
        frame->medbuf = NULL;
        p->UpdateSurfaceTexture();

        if (!p->drop_state)
        {
          p->free_mutex.lock();

          stSlot* cur_slot = p->getFreeSlot();
          if (!cur_slot)
          {
            CLog::Log(LOGERROR, "STF: No free output buffers\n");
            continue;
          }

          p->fbo.BindToTexture(GL_TEXTURE_2D, cur_slot->texid);
          p->fbo.BeginRender();

          glDisable(GL_DEPTH_TEST);
          //glClear(GL_COLOR_BUFFER_BIT);

          const GLfloat triangleVertices[] = {
          -1.0f, 1.0f,
          -1.0f, -1.0f,
          1.0f, -1.0f,
          1.0f, 1.0f,
          };

          glVertexAttribPointer(p->mPositionHandle, 2, GL_FLOAT, GL_FALSE, 0, triangleVertices);
          glEnableVertexAttribArray(p->mPositionHandle);

          glUseProgram(p->mPgm);
          glUniform1i(p->mTexSamplerHandle, 0);

          glBindTexture(GL_TEXTURE_EXTERNAL_OES, p->mVideoTextureId);

          GLfloat texMatrix[16];
          p->GetSurfaceTextureTransformMatrix(texMatrix);
          glUniformMatrix4fv(p->mTexMatrixHandle, 1, GL_FALSE, texMatrix);

          glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

          glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
          p->fbo.EndRender();

          glBindTexture(GL_TEXTURE_2D, 0);

          frame->eglimg = cur_slot->eglimg;
          p->free_mutex.unlock();
        }
      }

    #if defined(DEBUG_VERBOSE)
      CLog::Log(LOGDEBUG, "%s: >>> pushed OUT frame; w:%d, h:%d, img:%p, tm:%d\n", CLASSNAME, frame->width, frame->height, frame->eglimg, XbmcThreads::SystemClockMillis() - time);
    #endif

      p->out_mutex.lock();
      p->cur_frame = frame;
      while (p->cur_frame)
        p->out_condition.wait(p->out_mutex);
      p->out_mutex.unlock();
    }
    while (!decode_done && !m_bStop);

    if (p->eglInitialized)
      p->ReleaseEGL();

  }
};

/***********************************************************/

CStageFrightVideo::CStageFrightVideo(CDVDCodecInterface* interface)
{
#if defined(DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::ctor: %d\n", CLASSNAME, sizeof(CStageFrightVideo));
#endif
  p = new CStageFrightVideoPrivate;
  p->m_g_application = interface->GetApplication();
  p->m_g_applicationMessenger = interface->GetApplicationMessenger();
  p->m_g_Windowing = (CWinSystemEGL*)interface->GetWindowSystem();
  p->m_g_advancedSettings = interface->GetAdvancedSettings();
  p->m_g_renderManager = interface->GetRenderManager();
}

CStageFrightVideo::~CStageFrightVideo()
{
  delete p;
}

bool CStageFrightVideo::Open(CDVDStreamInfo &hints)
{
#if defined(DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::Open\n", CLASSNAME);
#endif

  // stagefright crashes with null size. Trap this...
  if (!hints.width || !hints.height)
  {
    CLog::Log(LOGERROR, "%s::%s - %s\n", CLASSNAME, __func__,"null size, cannot handle");
    return false;
  }
  p->width     = hints.width;
  p->height    = hints.height;
  if (!hints.forced_aspect)
    p->aspect_ratio = hints.aspect;
  else
    p->aspect_ratio = 1.0;

  p->meta = new MetaData;
  if (p->meta == NULL)
  {
    CLog::Log(LOGERROR, "%s::%s - %s\n", CLASSNAME, __func__,"cannot allocate MetaData");
    return false;
  }

  const char* mimetype;
  std::string use_codec;
  std::map<std::string, std::string> codec_config = p->m_g_advancedSettings->m_codecconfigs["stagefright"];

  if (codec_config["useSwRenderer"] == "true" || codec_config["useSwRenderer"] == "1")
    p->quirks |= QuirkSWRender;

  switch (hints.codec)
  {
    case AV_CODEC_ID_HEVC:
      use_codec = codec_config["useHEVCcodec"];
      if (use_codec == "0"
          || (use_codec == "sd" && hints.width > 800)
          || (use_codec == "hd" && hints.width <= 800)
          || (use_codec == "uhd" && hints.width <= 2000))
        return false;
      mimetype = "video/hevc";
      break;
    case AV_CODEC_ID_H264:
      //  case AV_CODEC_ID_H264MVC:
      use_codec = codec_config["useAVCcodec"];
      if (use_codec == "0"
          || (use_codec == "sd" && hints.width > 800)
          || (use_codec == "hd" && hints.width <= 800)
          || (use_codec == "uhd" && hints.width <= 2000))
        return false;
      mimetype = "video/avc";
      if (hints.extradata && *(uint8_t*)hints.extradata == 1)
        p->meta->setData(kKeyAVCC, kTypeAVCC, hints.extradata, hints.extrasize);
      break;
    case AV_CODEC_ID_MPEG4:
      use_codec = codec_config["useMP4codec"];
      if (use_codec == "0"
          || (use_codec == "sd" && hints.width > 800)
          || (use_codec == "hd" && hints.width <= 800)
          || (use_codec == "uhd" && hints.width <= 2000))
        return false;
      mimetype = "video/mp4v-es";
      break;
    case AV_CODEC_ID_MPEG2VIDEO:
      use_codec = codec_config["useMPEG2codec"];
      if (use_codec == "0"
          || (use_codec == "sd" && hints.width > 800)
          || (use_codec == "hd" && hints.width <= 800)
          || (use_codec == "uhd" && hints.width <= 2000))
        return false;
      mimetype = "video/mpeg2";
      break;
    case AV_CODEC_ID_VP3:
    case AV_CODEC_ID_VP6:
    case AV_CODEC_ID_VP6F:
      use_codec = codec_config["useVPXcodec"];
      if (use_codec == "0"
          || (use_codec == "sd" && hints.width > 800)
          || (use_codec == "hd" && hints.width <= 800)
          || (use_codec == "uhd" && hints.width <= 2000))
        return false;
      mimetype = "video/vp6";
      break;
    case AV_CODEC_ID_VP8:
      use_codec = codec_config["useVPXcodec"];
      if (use_codec == "0"
          || (use_codec == "sd" && hints.width > 800)
          || (use_codec == "hd" && hints.width <= 800)
          || (use_codec == "uhd" && hints.width <= 2000))
        return false;
      return false;
      mimetype = "video/x-vnd.on2.vp8";
      break;
    case AV_CODEC_ID_VC1:
    case AV_CODEC_ID_WMV3:
      use_codec = codec_config["useVC1codec"];
      if (use_codec == "0"
          || (use_codec == "sd" && hints.width > 800)
          || (use_codec == "hd" && hints.width <= 800)
          || (use_codec == "uhd" && hints.width <= 2000))
        return false;
      mimetype = "video/vc1";
      break;
    default:
      return false;
      break;
  }

  p->meta->setCString(kKeyMIMEType, mimetype);
  p->meta->setInt32(kKeyWidth, p->width);
  p->meta->setInt32(kKeyHeight, p->height);

  android::ProcessState::self()->startThreadPool();

  p->source    = new CStageFrightMediaSource(p, p->meta);
  p->client    = new OMXClient;

  if (p->source == NULL || p->client == NULL)
  {
    p->meta = NULL;
    p->source = NULL;
    p->decoder = NULL;
    CLog::Log(LOGERROR, "%s::%s - %s\n", CLASSNAME, __func__,"Cannot obtain source / client");
    return false;
  }

  if (p->client->connect() !=  OK)
  {
    p->meta = NULL;
    p->source = NULL;
    delete p->client;
    p->client = NULL;
    CLog::Log(LOGERROR, "%s::%s - %s\n", CLASSNAME, __func__,"Cannot connect OMX client");
    return false;
  }

  if ((p->quirks & QuirkSWRender) == 0)
    if (!p->InitSurfaceTexture())
    {
      p->meta = NULL;
      p->source = NULL;
      delete p->client;
      p->client = NULL;
      CLog::Log(LOGERROR, "%s::%s - %s\n", CLASSNAME, __func__,"Cannot allocate texture");
      return false;
    }

  p->decoder  = OMXCodec::Create(p->client->interface(), p->meta,
                                         false, p->source, NULL,
                                         OMXCodec::kHardwareCodecsOnly | (p->quirks & QuirkSWRender ? OMXCodec::kClientNeedsFramebuffer : 0),
                                         p->mVideoNativeWindow
                                         );

  if (!(p->decoder != NULL && p->decoder->start() ==  OK))
  {
    p->meta = NULL;
    p->source = NULL;
    p->decoder = NULL;
    return false;
  }

  sp<MetaData> outFormat = p->decoder->getFormat();

  if (!outFormat->findInt32(kKeyWidth, &p->width) || !outFormat->findInt32(kKeyHeight, &p->height)
        || !outFormat->findInt32(kKeyColorFormat, &p->videoColorFormat))
  {
    p->meta = NULL;
    p->source = NULL;
    p->decoder = NULL;
    return false;
  }

  const char *component;
  if (outFormat->findCString(kKeyDecoderComponent, &component))
  {
    CLog::Log(LOGDEBUG, "%s::%s - component: %s\n", CLASSNAME, __func__, component);

    //Blacklist
    if (!strncmp(component, "OMX.google", 10))
    {
      // On some platforms, software decoders are returned anyway
      CLog::Log(LOGERROR, "%s::%s - %s\n", CLASSNAME, __func__,"Blacklisted component (software)");
      return false;
    }
    else if (!strncmp(component, "OMX.Nvidia.mp4.decode", 21) && codec_config["useMP4codec"] != "1")
    {
      // Has issues with some XVID encoded MP4. Only fails after actual decoding starts...
      CLog::Log(LOGERROR, "%s::%s - %s\n", CLASSNAME, __func__,"Blacklisted component (MP4)");
      return false;
    }
  }

  int32_t cropLeft, cropTop, cropRight, cropBottom;
  cropLeft = cropTop = cropRight = cropBottom = 0;
  if (!outFormat->findRect(kKeyCropRect, &cropLeft, &cropTop, &cropRight, &cropBottom))
  {
    p->x = 0;
    p->y = 0;
  }
  else
  {
    p->x = cropLeft;
    p->y = cropTop;
    p->width = cropRight - cropLeft + 1;
    p->height = cropBottom - cropTop + 1;
  }

  if (!outFormat->findInt32(kKeyStride, &p->videoStride))
    p->videoStride = p->width;
  if (!outFormat->findInt32(kKeySliceHeight, &p->videoSliceHeight))
    p->videoSliceHeight = p->height;

  for (int i=0; i<INBUFCOUNT; ++i)
  {
    p->inbuf[i] = new MediaBuffer(100000);
    p->inbuf[i]->setObserver(p);
  }

  p->m_g_renderManager->RegisterRenderLockCallBack((const void*)this, RenderLockCallBack);
  p->m_g_renderManager->RegisterRenderReleaseCallBack((const void*)this, RenderReleaseCallBack);

  p->decode_thread = new CStageFrightDecodeThread(p);
  p->decode_thread->Create(true /*autodelete*/);

#if defined(DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, ">>> format col:%d, w:%d, h:%d, sw:%d, sh:%d, ctl:%d,%d; cbr:%d,%d\n", p->videoColorFormat, p->width, p->height, p->videoStride, p->videoSliceHeight, cropTop, cropLeft, cropBottom, cropRight);
#endif

  return true;
}

/*** Decode ***/
int  CStageFrightVideo::Decode(uint8_t *pData, int iSize, double dts, double pts)
{
#if defined(DEBUG_VERBOSE)
  unsigned int time = XbmcThreads::SystemClockMillis();
  CLog::Log(LOGDEBUG, "%s::Decode - d:%p; s:%d; dts:%f; pts:%f\n", CLASSNAME, pData, iSize, dts, pts);
#endif

  Frame *frame;
  int demuxer_bytes = iSize;
  uint8_t *demuxer_content = pData;
  int ret = 0;

  if (demuxer_content)
  {
    frame = (Frame*)malloc(sizeof(Frame));
    if (!frame)
      return VC_ERROR;

    frame->status  = OK;
    frame->pts = (pts != DVD_NOPTS_VALUE) ? pts_dtoi(pts) : ((dts != DVD_NOPTS_VALUE) ? pts_dtoi(dts) : 0);

    // No valid pts? libstagefright asserts on this.
    if (frame->pts < 0)
    {
      free(frame);
      return ret;
    }

    frame->medbuf = p->getBuffer(demuxer_bytes);
    if (!frame->medbuf)
    {
      CLog::Log(LOGWARNING, "STF: Cannot get input buffer\n");
      free(frame);
      return VC_ERROR;
    }

    fast_memcpy(frame->medbuf->data(), demuxer_content, demuxer_bytes);
    frame->medbuf->set_range(0, demuxer_bytes);
    frame->medbuf->meta_data()->clear();
    frame->medbuf->meta_data()->setInt64(kKeyTime, frame->pts);

    p->in_mutex.lock();
    p->in_queue.push_back(frame);
    p->in_condition.notify();
    p->in_mutex.unlock();
  }

  if (p->inputBufferAvailable() && p->in_queue.size() < INBUFCOUNT)
    ret |= VC_BUFFER;
  else
    usleep(1000);
  if (p->cur_frame != NULL)
    ret |= VC_PICTURE;
#if defined(DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::Decode: pushed IN frame (%d); tm:%d\n", CLASSNAME,p->in_queue.size(), XbmcThreads::SystemClockMillis() - time);
#endif

  return ret;
}

bool CStageFrightVideo::ClearPicture(DVDVideoPicture* pDvdVideoPicture)
{
 #if defined(DEBUG_VERBOSE)
  unsigned int time = XbmcThreads::SystemClockMillis();
#endif
  if (pDvdVideoPicture->format == RENDER_FMT_EGLIMG)
    ReleaseBuffer((EGLImageKHR)pDvdVideoPicture->render_ctx);

  if (p->prev_frame) {
    if (p->prev_frame->medbuf)
      p->prev_frame->medbuf->release();
    free(p->prev_frame);
    p->prev_frame = NULL;
  }
#if defined(DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::ClearPicture (%d)\n", CLASSNAME, XbmcThreads::SystemClockMillis() - time);
#endif

  return true;
}

bool CStageFrightVideo::GetPicture(DVDVideoPicture* pDvdVideoPicture)
{
#if defined(DEBUG_VERBOSE)
  unsigned int time = XbmcThreads::SystemClockMillis();
  CLog::Log(LOGDEBUG, "%s::GetPicture\n", CLASSNAME);
  if (p->cycle_time != 0)
    CLog::Log(LOGDEBUG, ">>> cycle dur:%d\n", XbmcThreads::SystemClockMillis() - p->cycle_time);
  p->cycle_time = time;
#endif

  status_t status;

  p->out_mutex.lock();
  if (!p->cur_frame)
  {
    CLog::Log(LOGERROR, "%s::%s - Error getting frame\n", CLASSNAME, __func__);
    p->out_condition.notify();
    p->out_mutex.unlock();
    return false;
  }

  Frame *frame = p->cur_frame;
  status  = frame->status;

  pDvdVideoPicture->format = frame->format;
  pDvdVideoPicture->dts = DVD_NOPTS_VALUE;
  pDvdVideoPicture->pts = frame->pts;
  pDvdVideoPicture->iWidth  = p->width;
  pDvdVideoPicture->iHeight = p->height;
  pDvdVideoPicture->iDisplayWidth = p->width;
  pDvdVideoPicture->iDisplayHeight = p->height;
  if (p->aspect_ratio > 1.0)
  {
    pDvdVideoPicture->iDisplayWidth  = ((int)lrint(pDvdVideoPicture->iHeight * p->aspect_ratio)) & -3;
    if (pDvdVideoPicture->iDisplayWidth > pDvdVideoPicture->iWidth)
    {
      pDvdVideoPicture->iDisplayWidth  = pDvdVideoPicture->iWidth;
      pDvdVideoPicture->iDisplayHeight = ((int)lrint(pDvdVideoPicture->iWidth / p->aspect_ratio)) & -3;
    }
  }
  pDvdVideoPicture->iFlags  = DVP_FLAG_ALLOCATED;
  pDvdVideoPicture->render_ctx = NULL;

  if (status != OK)
  {
    CLog::Log(LOGERROR, "%s::%s - Error getting picture from frame(%d)\n", CLASSNAME, __func__,status);
    if (frame->medbuf) {
      frame->medbuf->release();
    }
    free(frame);
    p->cur_frame = NULL;
    p->out_condition.notify();
    p->out_mutex.unlock();
    return false;
  }

  if (pDvdVideoPicture->format == RENDER_FMT_EGLIMG)
  {
    if (frame->eglimg != EGL_NO_IMAGE_KHR)
    {
      pDvdVideoPicture->render_ctx = (void*)frame->eglimg;
      LockBuffer(frame->eglimg);
    } else
      pDvdVideoPicture->iFlags |= DVP_FLAG_DROPPED;
#if defined(DEBUG_VERBOSE)
    CLog::Log(LOGDEBUG, ">>> pic dts:%f, pts:%llu, img:%p, tm:%d\n", pDvdVideoPicture->dts, frame->pts, pDvdVideoPicture->stfbuf, XbmcThreads::SystemClockMillis() - time);
  #endif
  }
  else if (pDvdVideoPicture->format == RENDER_FMT_YUV420P)
  {
    pDvdVideoPicture->color_range  = 0;
    pDvdVideoPicture->color_matrix = 4;

    unsigned int luma_pixels = frame->width  * frame->height;
    unsigned int chroma_pixels = luma_pixels/4;
    uint8_t* data = NULL;
    if (frame->medbuf && !p->drop_state)
    {
      data = (uint8_t*)((long)frame->medbuf->data() + frame->medbuf->range_offset());
    }
    switch (p->videoColorFormat)
    {
      case OMX_COLOR_FormatYUV420Planar:
        pDvdVideoPicture->iLineSize[0] = frame->width;
        pDvdVideoPicture->iLineSize[1] = frame->width / 2;
        pDvdVideoPicture->iLineSize[2] = frame->width / 2;
        pDvdVideoPicture->iLineSize[3] = 0;
        pDvdVideoPicture->data[0] = data;
        pDvdVideoPicture->data[1] = pDvdVideoPicture->data[0] + luma_pixels;
        pDvdVideoPicture->data[2] = pDvdVideoPicture->data[1] + chroma_pixels;
        pDvdVideoPicture->data[3] = 0;
        break;
      case OMX_COLOR_FormatYUV420SemiPlanar:
      case OMX_QCOM_COLOR_FormatYVU420SemiPlanar:
      case OMX_TI_COLOR_FormatYUV420PackedSemiPlanar:
        pDvdVideoPicture->iLineSize[0] = frame->width;
        pDvdVideoPicture->iLineSize[1] = frame->width;
        pDvdVideoPicture->iLineSize[2] = 0;
        pDvdVideoPicture->iLineSize[3] = 0;
        pDvdVideoPicture->data[0] = data;
        pDvdVideoPicture->data[1] = pDvdVideoPicture->data[0] + luma_pixels;
        pDvdVideoPicture->data[2] = pDvdVideoPicture->data[1] + chroma_pixels;
        pDvdVideoPicture->data[3] = 0;
        break;
      default:
        CLog::Log(LOGERROR, "%s::%s - Unsupported color format(%d)\n", CLASSNAME, __func__,p->videoColorFormat);
    }
  #if defined(DEBUG_VERBOSE)
    CLog::Log(LOGDEBUG, ">>> pic pts:%f, data:%p, col:%d, w:%d, h:%d, tm:%d\n", pDvdVideoPicture->pts, data, p->videoColorFormat, frame->width, frame->height, XbmcThreads::SystemClockMillis() - time);
  #endif
  }

  if (p->drop_state)
    pDvdVideoPicture->iFlags |= DVP_FLAG_DROPPED;

  p->prev_frame = p->cur_frame;
  p->cur_frame = NULL;
  p->out_condition.notify();
  p->out_mutex.unlock();

  return true;
}

void CStageFrightVideo::Dispose()
{
#if defined(DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::Close\n", CLASSNAME);
#endif

  p->m_g_renderManager->RegisterRenderLockCallBack((const void*)NULL, NULL);
  p->m_g_renderManager->RegisterRenderReleaseCallBack((const void*)NULL, NULL);

  Frame *frame;

  if (p->decode_thread && p->decode_thread->IsRunning())
    p->decode_thread->StopThread(false);
  p->decode_thread = NULL;
  p->in_condition.notify();

  // Give decoder_thread time to process EOS, if stuck on reading
  usleep(50000);

#if defined(DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "Cleaning OUT\n");
#endif
  p->out_mutex.lock();
  if (p->cur_frame)
  {
    if (p->cur_frame->medbuf)
      p->cur_frame->medbuf->release();
    free(p->cur_frame);
    p->cur_frame = NULL;
  }
  p->out_condition.notify();
  p->out_mutex.unlock();

  if (p->prev_frame)
  {
    if (p->prev_frame->medbuf)
      p->prev_frame->medbuf->release();
    free(p->prev_frame);
    p->prev_frame = NULL;
  }

#if defined(DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "Stopping omxcodec\n");
#endif
  if (p->decoder != NULL)
  {
    p->decoder->stop();
    p->decoder = NULL;
  }
  if (p->client)
  {
    p->client->disconnect();
    delete p->client;
  }
  p->meta = NULL;

#if defined(DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "Cleaning IN(%d)\n", p->in_queue.size());
#endif
  std::list<Frame*>::iterator it;
  while (!p->in_queue.empty())
  {
    it = p->in_queue.begin();
    frame = *it;
    p->in_queue.erase(it);
    if (frame->medbuf)
      frame->medbuf->release();
    free(frame);
  }
  for (int i=0; i<INBUFCOUNT; ++i)
  {
    if (p->inbuf[i])
    {
      p->inbuf[i]->setObserver(NULL);
      p->inbuf[i]->release();
      p->inbuf[i] = NULL;
    }
  }
  p->source = NULL;

#if defined(DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "Cleaning libstagefright\n", p->in_queue.size());
#endif
  if ((p->quirks & QuirkSWRender) == 0)
    p->ReleaseSurfaceTexture();

#if defined(DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "Final Cleaning\n", p->in_queue.size());
#endif
  if (p->decoder_component)
    free(&p->decoder_component);
}

void CStageFrightVideo::Reset(void)
{
#if defined(DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::Reset\n", CLASSNAME);
#endif
  Frame* frame;
  p->in_mutex.lock();
  std::list<Frame*>::iterator it;
  while (!p->in_queue.empty())
  {
    it = p->in_queue.begin();
    frame = *it;
    p->in_queue.erase(it);
    if (frame->medbuf)
      frame->medbuf->release();
    free(frame);
  }
  p->resetting = true;

  p->in_mutex.unlock();
}

void CStageFrightVideo::SetDropState(bool bDrop)
{
  if (bDrop == p->drop_state)
    return;

#if defined(DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "%s::SetDropState (%d->%d)\n", CLASSNAME,p->drop_state,bDrop);
#endif

  p->drop_state = bDrop;
}

void CStageFrightVideo::SetSpeed(int iSpeed)
{
}

void CStageFrightVideo::LockBuffer(EGLImageKHR eglimg)
{
#if defined(DEBUG_VERBOSE)
  unsigned int time = XbmcThreads::SystemClockMillis();
#endif
  p->free_mutex.lock();
  stSlot* slot = p->getSlot(eglimg);
  if (!slot)
  {
    CLog::Log(LOGDEBUG, "STF: LockBuffer: Unknown img(%p)", eglimg);
    p->free_mutex.unlock();
    return;
  }
  slot->use_cnt++;

#if defined(DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "STF: LockBuffer: Locking %p: cnt:%d tm:%d\n", eglimg, slot->use_cnt, XbmcThreads::SystemClockMillis() - time);
#endif
  p->free_mutex.unlock();
}

bool CStageFrightVideo::ReleaseBuffer(EGLImageKHR eglimg)
{
#if defined(DEBUG_VERBOSE)
  unsigned int time = XbmcThreads::SystemClockMillis();
#endif
  p->free_mutex.lock();
  stSlot* slot = p->getSlot(eglimg);
  if (!slot)
  {
    CLog::Log(LOGDEBUG, "STF: ReleaseBuffer: Unknown img(%p)", eglimg);
    p->free_mutex.unlock();
    return true;
  }
  if (slot->use_cnt == 0)
  {
    CLog::Log(LOGDEBUG, "STF: ReleaseBuffer: already unlocked img(%p)", eglimg);
    p->free_mutex.unlock();
    return true;
  }
  slot->use_cnt--;

#if defined(DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "STF: ReleaseBuffer: Unlocking %p: cnt:%d tm:%d\n", eglimg, slot->use_cnt, XbmcThreads::SystemClockMillis() - time);
#endif
  p->free_mutex.unlock();
  return (slot->use_cnt == 0);
}

/**********************************/

void CStageFrightVideo::RenderLockCallBack(const void *ctx, const void* render_ctx)
{
  CStageFrightVideo *codec = (CStageFrightVideo*)ctx;
  if (codec)
    codec->LockBuffer((EGLImageKHR)render_ctx);
}

void CStageFrightVideo::RenderReleaseCallBack(const void *ctx, const void* render_ctx)
{
  CStageFrightVideo *codec = (CStageFrightVideo*)ctx;
  if (codec)
    codec->ReleaseBuffer((EGLImageKHR)render_ctx);
}
