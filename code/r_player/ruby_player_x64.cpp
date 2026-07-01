/*
    Ruby Licence - GStreamer player for x64 (live SM stream + offline DVR file playback)
    Auto-detects HW decode: NVIDIA NVDEC, VA-API (Intel/AMD), V4L2 stateless, then software.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <semaphore.h>
#include <errno.h>
#include <time.h>

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>

#include "../base/base.h"
#include "../base/config.h"
#include "../base/config_obj_names.h"
#include "../base/shared_mem.h"
#include "../base/shared_mem_video_disp.h"
#include "../base/hardware_procs.h"
#include "../base/ctrl_settings.h"

#define PIPE_BUFFER_SIZE 200000
#define MAX_PIPELINE_ELEMENTS 10

static volatile bool g_bQuit = false;
static bool g_bPlayFile = false;
static bool g_bUseH265Decoder = false;
static bool g_bForceSoftwareDecode = false;
static char g_szPlayFileName[MAX_FILE_PATH_SIZE];
static char g_szDecoderName[64] = "unknown";
static int g_iFileFPS = 30;
static int g_iFileTempSlices = 1;
static int g_iFileDetectedSlices = 1;

static GstElement* pipeline = NULL;
static GstElement* appsrc = NULL;
static int s_iMaxLiveLagMs = 200;   // live appsrc latency cap (ms); 0 = unbounded; env RUBY_MAXLAG_MS

// x64 console (no X/Wayland): decoded frames are published to shared memory and
// ruby_central (the DRM master) puts them on a dedicated DRM video plane. When a
// display server IS present we keep the normal gstreamer video sink (X/Wayland).
static bool g_bHeadless = false;
static type_video_disp_shm* g_pVideoDispSHM = NULL;
static volatile unsigned int g_uDecodedFrameCount = 0;   // total frames published to the display shm; the GS derives actual render FPS from this (a drop-aware keep-up signal the backlog can't see)

typedef enum
{
   HW_VENDOR_SOFTWARE = 0,
   HW_VENDOR_NVIDIA,
   HW_VENDOR_VAAPI,
   HW_VENDOR_V4L2
} type_hw_vendor;

typedef struct
{
   const char* szDecoder;
   const char* szMidElement;
   type_hw_vendor eVendor;
} type_decoder_option;

static void handle_sigint(int sig)
{
   log_line("Caught signal %d, quitting...", sig);
   g_bQuit = true;
   if (pipeline)
      gst_element_set_state(pipeline, GST_STATE_NULL);
}

static void _signal_play_file_finished()
{
   sem_t* ps = sem_open(SEMAPHORE_VIDEO_FILE_PLAYBACK_FINISHED, O_CREAT, S_IWUSR | S_IRUSR, 0);
   if ((NULL != ps) && (SEM_FAILED != ps))
   {
      sem_post(ps);
      sem_close(ps);
   }
}

static bool _player_is_h264_vcl_nal(u32 uNALType)
{
   return (uNALType == 1) || (uNALType == 5);
}

static bool _player_is_h265_vcl_nal(u32 uNALType)
{
   return uNALType <= 31;
}

static void _player_pace_frame(u32* puFrameCount, u32* puPlaybackStartMs)
{
   if (g_iFileFPS < 1)
      return;
   if (0 == *puPlaybackStartMs)
      *puPlaybackStartMs = get_current_timestamp_ms();
   (*puFrameCount)++;
   u32 uTargetMs = *puPlaybackStartMs + ((*puFrameCount) * 1000) / (u32)g_iFileFPS;
   u32 uNow = get_current_timestamp_ms();
   if (uTargetMs > uNow)
      hardware_sleep_ms(uTargetMs - uNow);
}

static GstElement* _make_element(const char* szFactory, const char* szName)
{
   GstElement* pElement = gst_element_factory_make(szFactory, szName);
   if (NULL == pElement)
      log_line("[PlayerX64] GStreamer element not available: %s", szFactory);
   return pElement;
}

static const char* _vendor_name(type_hw_vendor eVendor)
{
   switch (eVendor)
   {
      case HW_VENDOR_NVIDIA: return "NVIDIA";
      case HW_VENDOR_VAAPI: return "VA-API";
      case HW_VENDOR_V4L2: return "V4L2";
      default: return "software";
   }
}

static void _video_disp_shm_init()
{
   int fd = shm_open(SHARED_MEM_VIDEO_DISP_NAME, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
   if (fd < 0)
   {
      log_softerror_and_alarm("[PlayerX64] video disp shm_open failed: %s", strerror(errno));
      return;
   }
   if (0 != ftruncate(fd, SHARED_MEM_VIDEO_DISP_SIZE))
   {
      log_softerror_and_alarm("[PlayerX64] video disp ftruncate failed: %s", strerror(errno));
      close(fd);
      return;
   }
   g_pVideoDispSHM = (type_video_disp_shm*) mmap(NULL, SHARED_MEM_VIDEO_DISP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
   close(fd);
   if ((MAP_FAILED == g_pVideoDispSHM) || (NULL == g_pVideoDispSHM))
   {
      g_pVideoDispSHM = NULL;
      log_softerror_and_alarm("[PlayerX64] video disp mmap failed: %s", strerror(errno));
      return;
   }
   g_pVideoDispSHM->uSeq = 0;
   g_pVideoDispSHM->uMagic = VIDEO_DISP_MAGIC;
   g_pVideoDispSHM->uWidth = 0;
   g_pVideoDispSHM->uHeight = 0;
   log_line("[PlayerX64] Video display shared mem ready (%s, %d bytes) for DRM video plane.", SHARED_MEM_VIDEO_DISP_NAME, SHARED_MEM_VIDEO_DISP_SIZE);
}

// appsink callback (headless): publish the latest decoded BGRx frame to shared memory via a seqlock.
static GstFlowReturn _on_new_sample(GstAppSink* pSink, gpointer pUserData)
{
   (void)pUserData;
   GstSample* pSample = gst_app_sink_pull_sample(pSink);
   if (NULL == pSample)
      return GST_FLOW_OK;
   GstBuffer* pBuf = gst_sample_get_buffer(pSample);
   GstCaps* pCaps = gst_sample_get_caps(pSample);
   if ((NULL == pBuf) || (NULL == pCaps) || (NULL == g_pVideoDispSHM))
   {
      gst_sample_unref(pSample);
      return GST_FLOW_OK;
   }
   int iW = 0, iH = 0;
   GstStructure* pStruct = gst_caps_get_structure(pCaps, 0);
   gst_structure_get_int(pStruct, "width", &iW);
   gst_structure_get_int(pStruct, "height", &iH);
   GstMapInfo map;
   if ((iW > 0) && (iH > 0) && (iW <= VIDEO_DISP_MAX_WIDTH) && (iH <= VIDEO_DISP_MAX_HEIGHT) &&
       gst_buffer_map(pBuf, &map, GST_MAP_READ))
   {
      uint32_t uStride = (uint32_t)(map.size / iH);   // packed BGRx single plane: size = stride*height
      uint32_t uSize = uStride * (uint32_t)iH;
      if ((uStride >= (uint32_t)(iW * 4)) && (uSize <= (uint32_t)VIDEO_DISP_MAX_FRAME_SIZE) && (uSize <= map.size))
      {
         type_video_disp_shm* p = g_pVideoDispSHM;
         __sync_fetch_and_add(&p->uSeq, 1);   // -> odd: write in progress
         __sync_synchronize();
         p->uWidth = (uint32_t)iW;
         p->uHeight = (uint32_t)iH;
         p->uStride = uStride;
         p->uFourCC = VIDEO_DISP_FOURCC_XRGB8888;
         p->uFrameSize = uSize;
         memcpy(p->data, map.data, uSize);
         __sync_synchronize();
         __sync_fetch_and_add(&p->uSeq, 1);   // -> even: stable
         static int s_iFrames = 0;
         if ((0 == s_iFrames) || (0 == (s_iFrames % 150)))
            log_line("[PlayerX64] Decoded frame %d -> shm: %dx%d stride %u (%u bytes)", s_iFrames, iW, iH, uStride, uSize);
         s_iFrames++;
         g_uDecodedFrameCount++;   // rendered-frame tick: the GS derives actual decode/display FPS from this
      }
      gst_buffer_unmap(pBuf, &map);
   }
   gst_sample_unref(pSample);
   return GST_FLOW_OK;
}

// Headless: an appsink that hands decoded BGRx frames to ruby_central via shared memory.
static GstElement* _make_appsink_for_shm()
{
   GstElement* pSink = _make_element("appsink", "sink");
   if (NULL == pSink)
      return NULL;
   GstCaps* pCaps = gst_caps_from_string("video/x-raw,format=BGRx");
   g_object_set(G_OBJECT(pSink), "caps", pCaps, "emit-signals", TRUE, "sync", FALSE,
                "max-buffers", 1, "drop", TRUE, NULL);
   gst_caps_unref(pCaps);
   GstAppSinkCallbacks cbs;
   memset(&cbs, 0, sizeof(cbs));
   cbs.new_sample = _on_new_sample;
   gst_app_sink_set_callbacks(GST_APP_SINK(pSink), &cbs, NULL, NULL);
   log_line("[PlayerX64] Using headless appsink -> shared mem (DRM video plane in ruby_central)");
   return pSink;
}

static GstElement* _pick_video_sink(type_hw_vendor eVendor)
{
   const char* nvidia_sinks[] = {"glimagesink", "xvimagesink", "ximagesink", "autovideosink", NULL};
   const char* vaapi_sinks[] = {"vaapisink", "glimagesink", "xvimagesink", "ximagesink", "autovideosink", NULL};
   const char* generic_sinks[] = {"glimagesink", "xvimagesink", "ximagesink", "autovideosink", NULL};
   const char** sink_names = generic_sinks;

   if (HW_VENDOR_NVIDIA == eVendor)
      sink_names = nvidia_sinks;
   else if (HW_VENDOR_VAAPI == eVendor)
      sink_names = vaapi_sinks;

   for (int i = 0; sink_names[i] != NULL; i++)
   {
      GstElement* pSink = _make_element(sink_names[i], "sink");
      if (NULL != pSink)
      {
         log_line("[PlayerX64] Using video sink: %s", sink_names[i]);
         g_object_set(G_OBJECT(pSink), "sync", FALSE, NULL);
         return pSink;
      }
   }
   return NULL;
}

static void _configure_nv_decoder(GstElement* pDecoder, const char* szDecoderName)
{
   if ((NULL == pDecoder) || (NULL == szDecoderName))
      return;
   if (0 == strncmp(szDecoderName, "nvh", 3))
      g_object_set(G_OBJECT(pDecoder), "num-output-surfaces", 1, NULL);
}

static void _destroy_pipeline()
{
   if (NULL != pipeline)
   {
      gst_element_set_state(pipeline, GST_STATE_NULL);
      gst_object_unref(pipeline);
      pipeline = NULL;
      appsrc = NULL;
   }
}

static bool _try_build_pipeline(bool bLive, const type_decoder_option* pOption, bool bH265)
{
   const char* szParser = bH265 ? "h265parse" : "h264parse";

   GstElement* pParser = _make_element(szParser, "parser");
   GstElement* pDecoder = _make_element(pOption->szDecoder, "decoder");
   GstElement* pMid = (NULL != pOption->szMidElement) ? _make_element(pOption->szMidElement, "mid") : NULL;
   GstElement* pConvert = _make_element("videoconvert", "converter");
   type_hw_vendor eVendor = pOption->eVendor;
   GstElement* pSink = NULL;
   GstElement* elements[MAX_PIPELINE_ELEMENTS];
   int iCount = 0;

   if (0 == strncmp(pOption->szDecoder, "avdec_", 6))
      eVendor = HW_VENDOR_SOFTWARE;
   pSink = g_bHeadless ? _make_appsink_for_shm() : _pick_video_sink(eVendor);

   if ((NULL == pParser) || (NULL == pDecoder) || (NULL == pConvert) || (NULL == pSink))
   {
      if (pParser) gst_object_unref(pParser);
      if (pDecoder) gst_object_unref(pDecoder);
      if (pMid) gst_object_unref(pMid);
      if (pConvert) gst_object_unref(pConvert);
      if (pSink) gst_object_unref(pSink);
      return false;
   }
   if ((NULL != pOption->szMidElement) && (NULL == pMid))
   {
      gst_object_unref(pParser);
      gst_object_unref(pDecoder);
      gst_object_unref(pConvert);
      gst_object_unref(pSink);
      return false;
   }

   _configure_nv_decoder(pDecoder, pOption->szDecoder);

   pipeline = gst_pipeline_new("video-player");
   appsrc = _make_element("appsrc", "source");
   if ((NULL == pipeline) || (NULL == appsrc))
   {
      if (pParser) gst_object_unref(pParser);
      if (pDecoder) gst_object_unref(pDecoder);
      if (pMid) gst_object_unref(pMid);
      if (pConvert) gst_object_unref(pConvert);
      if (pSink) gst_object_unref(pSink);
      _destroy_pipeline();
      return false;
   }

   g_object_set(G_OBJECT(appsrc),
                "stream-type", 0,
                "format", GST_FORMAT_TIME,
                "is-live", bLive ? TRUE : FALSE,
                "do-timestamp", bLive ? TRUE : FALSE,
                NULL);
   // LIVE low-latency: cap the appsrc's internal queue by TIME and drop the OLDEST buffers on
   // overflow. The software decoder runs a hair under realtime at 1080p60, so with the default
   // UNBOUNDED queue (max-bytes=0) the tiny per-frame deficit accumulates without limit (observed
   // ~24s). Leaking downstream keeps the decoder fed with recent data -> latency bounded to ~max-time
   // (a few dropped frames instead of seconds of delay; decoder resyncs at the next keyframe).
   // File/DVR playback (bLive=false) keeps the default unbounded, non-leaky queue so it plays every frame.
   if ( bLive && (s_iMaxLiveLagMs > 0) )
   {
      g_object_set(G_OBJECT(appsrc),
                   "leaky-type", 2,                                   // GST_APP_LEAKY_TYPE_DOWNSTREAM (drop oldest)
                   "max-time", (guint64)s_iMaxLiveLagMs * GST_MSECOND,
                   "max-bytes", (guint64)0,
                   "max-buffers", (guint64)0,
                   NULL);
      log_line("[PlayerX64] Live appsrc bounded to %d ms (leaky downstream) for low latency.", s_iMaxLiveLagMs);
   }
   // Tell h264parse what we are pushing (raw Annex-B byte-stream from the radio link). Without caps,
   // appsrc -> h264parse negotiation can stall and no frames ever reach the decoder.
   {
      GstCaps* pSrcCaps = gst_caps_from_string(bH265 ? "video/x-h265,stream-format=byte-stream" : "video/x-h264,stream-format=byte-stream");
      gst_app_src_set_caps(GST_APP_SRC(appsrc), pSrcCaps);
      gst_caps_unref(pSrcCaps);
   }

   elements[iCount++] = appsrc;
   elements[iCount++] = pParser;
   elements[iCount++] = pDecoder;
   if (NULL != pMid)
      elements[iCount++] = pMid;
   elements[iCount++] = pConvert;
   elements[iCount++] = pSink;

   for (int i = 0; i < iCount; i++)
      gst_bin_add(GST_BIN(pipeline), elements[i]);

   for (int i = 0; i < iCount - 1; i++)
   {
      if (!gst_element_link(elements[i], elements[i + 1]))
      {
         log_line("[PlayerX64] Pipeline link failed: %s (%s)", pOption->szDecoder, _vendor_name(eVendor));
         _destroy_pipeline();
         return false;
      }
   }

   strncpy(g_szDecoderName, pOption->szDecoder, sizeof(g_szDecoderName) - 1);
   log_line("[PlayerX64] Video pipeline: %s -> %s (%s)", szParser, pOption->szDecoder, _vendor_name(eVendor));
   return true;
}

static bool _create_decode_pipeline(bool bLive)
{
   static const type_decoder_option s_H264Hw[] = {
      {"nvh264dec", "cudadownload", HW_VENDOR_NVIDIA},
      {"nvh264dec", NULL, HW_VENDOR_NVIDIA},
      {"vah264dec", NULL, HW_VENDOR_VAAPI},
      {"vaapih264dec", "vaapipostproc", HW_VENDOR_VAAPI},
      {"v4l2slh264dec", NULL, HW_VENDOR_V4L2},
      {"v4l2h264dec", NULL, HW_VENDOR_V4L2},
      {NULL, NULL, HW_VENDOR_SOFTWARE}
   };
   static const type_decoder_option s_H265Hw[] = {
      {"nvh265dec", "cudadownload", HW_VENDOR_NVIDIA},
      {"nvh265dec", NULL, HW_VENDOR_NVIDIA},
      {"vah265dec", NULL, HW_VENDOR_VAAPI},
      {"vaapih265dec", "vaapipostproc", HW_VENDOR_VAAPI},
      {"v4l2slh265dec", NULL, HW_VENDOR_V4L2},
      {"v4l2h265dec", NULL, HW_VENDOR_V4L2},
      {NULL, NULL, HW_VENDOR_SOFTWARE}
   };
   static const type_decoder_option s_H264Sw = {"avdec_h264", NULL, HW_VENDOR_SOFTWARE};
   static const type_decoder_option s_H265Sw = {"avdec_h265", NULL, HW_VENDOR_SOFTWARE};

   _destroy_pipeline();

   if (!g_bForceSoftwareDecode)
   {
      const type_decoder_option* pList = g_bUseH265Decoder ? s_H265Hw : s_H264Hw;
      for (int i = 0; pList[i].szDecoder != NULL; i++)
      {
         if (_try_build_pipeline(bLive, &pList[i], g_bUseH265Decoder))
            return true;
      }
      log_softerror_and_alarm("[PlayerX64] No HW decoder available, falling back to software");
   }

   const type_decoder_option* pSw = g_bUseH265Decoder ? &s_H265Sw : &s_H264Sw;
   return _try_build_pipeline(bLive, pSw, g_bUseH265Decoder);
}

static void _parse_args(int argc, char* argv[])
{
   if (NULL != getenv("RUBY_PLAYER_FORCE_SW"))
      g_bForceSoftwareDecode = true;

   for (int i = 1; i < argc; i++)
   {
      if (0 == strcmp(argv[i], "-file") && (i + 1 < argc))
      {
         g_bPlayFile = true;
         strncpy(g_szPlayFileName, argv[++i], MAX_FILE_PATH_SIZE - 1);
         if (strstr(g_szPlayFileName, ".h265"))
            g_bUseH265Decoder = true;
      }
      else if (0 == strcmp(argv[i], "-fps") && (i + 1 < argc))
      {
         g_iFileFPS = atoi(argv[++i]);
         if (g_iFileFPS < 5 || g_iFileFPS > 120)
            g_iFileFPS = 30;
      }
      else if (0 == strcmp(argv[i], "-h265"))
         g_bUseH265Decoder = true;
      else if (0 == strcmp(argv[i], "-sw"))
         g_bForceSoftwareDecode = true;
   }
}

static int _do_file_mode()
{
   log_line("x64 file playback: %s @ %d fps, codec: %s",
      g_szPlayFileName, g_iFileFPS, g_bUseH265Decoder ? "H265" : "H264");

   if (!_create_decode_pipeline(false))
   {
      log_error_and_alarm("Failed to create GStreamer pipeline for file playback");
      _signal_play_file_finished();
      return -1;
   }
   if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
   {
      log_error_and_alarm("Failed to start file playback with decoder %s", g_szDecoderName);
      _signal_play_file_finished();
      return -1;
   }
   log_line("[PlayerX64] File playback started with decoder %s", g_szDecoderName);

   FILE* fp = fopen(g_szPlayFileName, "rb");
   if (NULL == fp)
   {
      log_error_and_alarm("Failed to open %s", g_szPlayFileName);
      _signal_play_file_finished();
      return -1;
   }

   u8 uBuffer[4096];
   int nRead = 0;
   u32 uCurrentParseToken = 0x11111111;
   u32 uNALType = 0;
   u32 uPrevNALType = 0;
   u32 uFrameCount = 0;
   u32 uPlaybackStartMs = 0;

   while (!g_bQuit)
   {
      nRead = (int)fread(uBuffer, 1, sizeof(uBuffer), fp);
      if (nRead <= 0)
         break;

      u8* pTmp = uBuffer;
      for (int i = 0; i < nRead; i++)
      {
         uCurrentParseToken = (uCurrentParseToken << 8) | (*pTmp);
         pTmp++;
         // Match both 4-byte (00 00 00 01) and 3-byte (00 00 01) NAL start codes; many encoders
         // (incl. this intro clip) use 3-byte starts, which a 4-byte-only test misses -> almost no
         // frames detected -> file playback paces to a fraction of a second instead of realtime.
         if (((uCurrentParseToken & 0x00FFFFFF) != 0x00000001) || i >= (nRead - 1))
            continue;

         if (g_bUseH265Decoder)
         {
            uNALType = ((*pTmp) >> 1) & 0x3F;
            if (!_player_is_h265_vcl_nal(uNALType))
               continue;
            _player_pace_frame(&uFrameCount, &uPlaybackStartMs);
         }
         else
         {
            uNALType = (*pTmp) & 0x1F;
            if ((uPrevNALType == 5) && (uNALType != 5))
               g_iFileDetectedSlices = g_iFileTempSlices;
            if (uPrevNALType == uNALType)
               g_iFileTempSlices++;
            else
               g_iFileTempSlices = 1;
            uPrevNALType = uNALType;
            if (!_player_is_h264_vcl_nal(uNALType))
               continue;
            if ((g_iFileTempSlices % g_iFileDetectedSlices) != 0)
               continue;
            _player_pace_frame(&uFrameCount, &uPlaybackStartMs);
         }
      }

      while ((access(CONFIG_FILE_FULLPATH_PAUSE_VIDEO_PLAYER, R_OK) != -1) && !g_bQuit)
         hardware_sleep_ms(50);

      GstBuffer* buffer = gst_buffer_new_allocate(NULL, nRead, NULL);
      GstMapInfo map;
      gst_buffer_map(buffer, &map, GST_MAP_WRITE);
      memcpy(map.data, uBuffer, nRead);
      gst_buffer_unmap(buffer, &map);
      if (gst_app_src_push_buffer(GST_APP_SRC(appsrc), buffer) != GST_FLOW_OK)
         log_softerror_and_alarm("appsrc push failed during file playback");
   }

   fclose(fp);
   gst_app_src_end_of_stream(GST_APP_SRC(appsrc));
   hardware_sleep_ms(200);
   log_line("File playback finished: %s", g_szPlayFileName);
   _signal_play_file_finished();
   return 0;
}

static void _poll_pipeline_bus()
{
   if (NULL == pipeline)
      return;
   GstBus* bus = gst_element_get_bus(pipeline);
   if (NULL == bus)
      return;
   GstMessage* msg;
   while (NULL != (msg = gst_bus_pop_filtered(bus, (GstMessageType)(GST_MESSAGE_ERROR | GST_MESSAGE_WARNING))))
   {
      GError* err = NULL;
      gchar* dbg = NULL;
      if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR)
      {
         gst_message_parse_error(msg, &err, &dbg);
         log_softerror_and_alarm("[PlayerX64] GST ERROR from %s: %s | %s", GST_OBJECT_NAME(msg->src), err ? err->message : "?", dbg ? dbg : "");
      }
      else
      {
         gst_message_parse_warning(msg, &err, &dbg);
         log_softerror_and_alarm("[PlayerX64] GST WARN from %s: %s", GST_OBJECT_NAME(msg->src), err ? err->message : "?");
      }
      if (err) g_error_free(err);
      if (dbg) g_free(dbg);
      gst_message_unref(msg);
   }
   gst_object_unref(bus);
}

// Live catch-up: if the SM-ring read falls more than this many bytes behind the writer (a backlog
// from a decode/pipeline hiccup), skip forward to the newest data so the live view stays current
// (the decoder resyncs at the next keyframe). 0 disables. Tunable at runtime via RUBY_MAXLAG_KB.
// Recording is a separate tap in ruby_rt_station and is never affected by this.
static int s_iMaxLiveLagBytes = 512 * 1024;

static int _do_sm_mode()
{
   sem_t* pSemaphore = sem_open(SEMAPHORE_SM_VIDEO_DATA_AVAILABLE, O_RDONLY);
   if ((NULL == pSemaphore) || (SEM_FAILED == pSemaphore))
   {
      log_error_and_alarm("Failed to open semaphore %s", SEMAPHORE_SM_VIDEO_DATA_AVAILABLE);
      return -1;
   }

   int fdSMem = shm_open(SM_STREAMER_NAME, O_RDONLY, S_IRUSR | S_IWUSR);
   if (fdSMem < 0)
   {
      sem_close(pSemaphore);
      return -1;
   }

   u8* pSMem = (u8*)mmap(NULL, SM_STREAMER_SIZE, PROT_READ, MAP_SHARED, fdSMem, 0);
   close(fdSMem);
   if ((pSMem == MAP_FAILED) || (pSMem == NULL))
   {
      sem_close(pSemaphore);
      return -1;
   }

#if defined(HW_PLATFORM_X64)
   // Keep heavy software video decode OFF the radio-RX core (0) and the router core (1). The router
   // (ruby_rt_station) runs a tight ~60fps loop pinned to core 1; if SW HEVC decode threads land
   // there they delay it, so received video blocks age past the retransmission window and the router
   // DISCARDS them -- which the adaptive engine misreads as a bad link and collapses the rate/datarate
   // (confirmed: H.265 walk-downs were 100% "bad video blocks" with 0 retransmissions, RF healthy).
   // Pin our main thread now, before the pipeline starts, so the gst decode threads it spawns inherit
   // this mask. Portable: only restricts when there are spare cores beyond 0/1.
   {
      long nDecodeCores = sysconf(_SC_NPROCESSORS_ONLN);
      if ( nDecodeCores >= 4 )
         hw_set_current_thread_affinity("PlayerX64 decode", 2, (int)(nDecodeCores - 1));
   }
#endif

   if (!_create_decode_pipeline(true) ||
       gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
   {
      munmap(pSMem, SM_STREAMER_SIZE);
      sem_close(pSemaphore);
      return -1;
   }
   log_line("[PlayerX64] Live SM playback started with decoder %s", g_szDecoderName);

   shared_mem_process_stats* pProcessStats = shared_mem_process_stats_open_write(SHARED_MEM_WATCHDOG_MPP_PLAYER);
   u8 pipeBuffer[PIPE_BUFFER_SIZE];
   u32 uSharedMemReadPos = 2 * sizeof(u32);

   while (!g_bQuit)
   {
      if (NULL != pProcessStats)
      {
         pProcessStats->lastActiveTime = get_current_timestamp_ms();
         // GS keep-up signal for the station's adaptive controller: publish the live appsrc backlog
         // (ms). With the leaky cap it pins near s_iMaxLiveLagMs when the software decoder is behind
         // realtime, and stays low when it keeps up. (Spare field; Radxa/Pi/central never read it.)
         if (NULL != appsrc)
         {
            guint64 uLevelNs = 0;
            g_object_get(G_OBJECT(appsrc), "current-level-time", &uLevelNs, NULL);
            pProcessStats->uLoopCounter2 = (u32)(uLevelNs / 1000000ULL);
         }
         // Rendered-frame count: the GS computes actual decode/display FPS from this. The leaky appsrc
         // holds the backlog (uLoopCounter2) near 0 by DROPPING frames, so a low backlog can still be
         // choppy -- an FPS deficit vs the configured stream FPS catches that strain the backlog hides.
         pProcessStats->uLoopCounter3 = g_uDecodedFrameCount;
      }

      _poll_pipeline_bus();

      u32* pWritePos1 = (u32*)pSMem;
      u32* pWritePos2 = (u32*)(&(pSMem[sizeof(u32)]));
      u32 uBytesToRead = 0;

      while ((uBytesToRead == 0) && !g_bQuit)
      {
         u32 uWritePos1 = *pWritePos1;
         u32 uWritePos2 = *pWritePos2;
         if ((uSharedMemReadPos == uWritePos1) || (uWritePos1 != uWritePos2))
         {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 10000000LL;
            if (0 != sem_timedwait(pSemaphore, &ts))
               continue;
            is_semaphore_signaled_clear_logok(pSemaphore, SEMAPHORE_SM_VIDEO_DATA_AVAILABLE, 0);
            uWritePos1 = *pWritePos1;
            uWritePos2 = *pWritePos2;
            if ((uSharedMemReadPos == uWritePos1) || (uWritePos1 != uWritePos2))
               continue;
         }

         // Live catch-up + lag instrumentation. uWritePos1 is now valid with unread data. If we've
         // fallen too far behind the writer (a backlog from a pipeline/decode hiccup), skip forward
         // to the newest data so the live view stays current; the decoder resyncs at the next
         // keyframe. The log line below reports the real ring lag so we can see WHERE delay sits
         // (large here = ring backlog the skip fixes; ~0 here = delay is downstream in gstreamer).
         {
            u32 uLag = (uWritePos1 >= uSharedMemReadPos)
                     ? (uWritePos1 - uSharedMemReadPos)
                     : (((u32)SM_STREAMER_SIZE - uSharedMemReadPos) + (uWritePos1 - 2*(u32)sizeof(u32)));
            static u32 s_uLagLogMs = 0;
            u32 uNowMs = get_current_timestamp_ms();
            if ( (0 == s_uLagLogMs) || (uNowMs > s_uLagLogMs + 1000) )
            {
               log_line("[PlayerX64] Live ring lag: %u bytes behind writer (maxlag %d bytes).", uLag, s_iMaxLiveLagBytes);
               s_uLagLogMs = uNowMs;
            }
            if ( (s_iMaxLiveLagBytes > 0) && (uLag > (u32)s_iMaxLiveLagBytes) )
            {
               log_line("[PlayerX64] Live catch-up: %u bytes behind (> %d) -> skip to latest.", uLag, s_iMaxLiveLagBytes);
               uSharedMemReadPos = uWritePos1;
               continue;   // re-loop: next read starts from the newest data
            }
         }

         if (uWritePos1 > uSharedMemReadPos)
         {
            uBytesToRead = uWritePos1 - uSharedMemReadPos;
            if (uBytesToRead > PIPE_BUFFER_SIZE) uBytesToRead = PIPE_BUFFER_SIZE;
            memcpy(pipeBuffer, &pSMem[uSharedMemReadPos], uBytesToRead);
            uSharedMemReadPos += uBytesToRead;
            if (uSharedMemReadPos >= SM_STREAMER_SIZE)
               uSharedMemReadPos = 2 * sizeof(u32);
         }
         else
         {
            uBytesToRead = SM_STREAMER_SIZE - uSharedMemReadPos;
            if (uBytesToRead > PIPE_BUFFER_SIZE) uBytesToRead = PIPE_BUFFER_SIZE;
            memcpy(pipeBuffer, &pSMem[uSharedMemReadPos], uBytesToRead);
            uSharedMemReadPos += uBytesToRead;
            if (uSharedMemReadPos >= SM_STREAMER_SIZE)
               uSharedMemReadPos = 2 * sizeof(u32);
            if ((PIPE_BUFFER_SIZE - uBytesToRead > 0) && (uWritePos1 > 0))
            {
               u32 uBytesToRead2 = uWritePos1 - 2 * sizeof(u32);
               if (uBytesToRead2 + uBytesToRead > PIPE_BUFFER_SIZE)
                  uBytesToRead2 = PIPE_BUFFER_SIZE - uBytesToRead;
               memcpy(&(pipeBuffer[uBytesToRead]), &pSMem[uSharedMemReadPos], uBytesToRead2);
               uSharedMemReadPos += uBytesToRead2;
               uBytesToRead += uBytesToRead2;
            }
         }
      }

      if (g_bQuit || uBytesToRead == 0)
         continue;

      GstBuffer* buffer = gst_buffer_new_allocate(NULL, uBytesToRead, NULL);
      GstMapInfo map;
      gst_buffer_map(buffer, &map, GST_MAP_WRITE);
      memcpy(map.data, pipeBuffer, uBytesToRead);
      gst_buffer_unmap(buffer, &map);
      gst_app_src_push_buffer(GST_APP_SRC(appsrc), buffer);
      {
         static u32 s_uPushed = 0, s_uBytes = 0, s_uLastLog = 0;
         s_uPushed++; s_uBytes += uBytesToRead;
         u32 uNow = get_current_timestamp_ms();
         if ((0 == s_uLastLog) || (uNow > s_uLastLog + 3000))
         {
            log_line("[PlayerX64] Fed appsrc: %u buffers, %u bytes total", s_uPushed, s_uBytes);
            s_uLastLog = uNow;
         }
      }
   }

   if (pProcessStats)
      shared_mem_process_stats_close(SHARED_MEM_WATCHDOG_MPP_PLAYER, pProcessStats);
   sem_close(pSemaphore);
   munmap(pSMem, SM_STREAMER_SIZE);
   return 0;
}

int main(int argc, char* argv[])
{
   signal(SIGINT, handle_sigint);
   signal(SIGTERM, handle_sigint);
   log_init("PlayerX64");
   // Older Intel GPUs (Sandybridge/Ivybridge HD graphics) need the i965 VA-API driver; the newer
   // iHD driver fails to init there and the gstreamer "va" plugin does not fall back. Default to
   // i965 (overridable: set LIBVA_DRIVER_NAME=iHD on newer Intel, or to anything for AMD/NVIDIA).
   if (NULL == getenv("LIBVA_DRIVER_NAME"))
   {
      setenv("LIBVA_DRIVER_NAME", "i965", 1);
      log_line("[PlayerX64] Defaulting LIBVA_DRIVER_NAME=i965 (older Intel VA-API; iHD fails on Gen6/7). Set LIBVA_DRIVER_NAME to override.");
   }
   gst_init(&argc, &argv);
   _parse_args(argc, argv);

   // Windowed (X/Wayland) vs headless (DRM kiosk) selection. Auto-detect on DISPLAY / WAYLAND_DISPLAY:
   // launched inside a graphical session -> render to a window via a gstreamer video sink; from a bare
   // TTY (the kiosk GS) -> headless -> shm -> ruby_central -> DRM/KMS. Explicit overrides win so a STRAY
   // DISPLAY (e.g. a VNC :1 side-channel or an SSH X-forward) can be locked out on the kiosk:
   // RUBY_PLAYER_HEADLESS=1 forces DRM (set it in the kiosk launcher if the GS env might carry a stray
   // DISPLAY); RUBY_PLAYER_X_SINK=1 forces the window.
   {
      bool bHaveDisplay = (NULL != getenv("DISPLAY")) || (NULL != getenv("WAYLAND_DISPLAY"));
      if ( NULL != getenv("RUBY_PLAYER_HEADLESS") )
         g_bHeadless = true;
      else if ( NULL != getenv("RUBY_PLAYER_X_SINK") )
         g_bHeadless = false;
      else
         g_bHeadless = ! bHaveDisplay;
   }
   if (g_bHeadless)
   {
      log_line("[PlayerX64] Headless -> DRM video plane: decoded frames published to shared mem for ruby_central.");
      _video_disp_shm_init();
   }
   else
      log_line("[PlayerX64] DISPLAY/WAYLAND_DISPLAY detected -> rendering to a gstreamer video sink (window).");

   log_line("[PlayerX64] HW decode: %s (NVIDIA/VA-API/V4L2 auto-detect; RUBY_PLAYER_FORCE_SW=1 or -sw to disable)",
      g_bForceSoftwareDecode ? "disabled" : "enabled");

   { const char* szMaxLag = getenv("RUBY_MAXLAG_KB");
     if ( NULL != szMaxLag ) { int k = atoi(szMaxLag); s_iMaxLiveLagBytes = (k > 0) ? (k * 1024) : 0;
        log_line("[PlayerX64] Live catch-up maxlag = %d bytes (RUBY_MAXLAG_KB=%s).", s_iMaxLiveLagBytes, szMaxLag); } }
   { const char* szMaxLagMs = getenv("RUBY_MAXLAG_MS");
     if ( NULL != szMaxLagMs ) { int m = atoi(szMaxLagMs); s_iMaxLiveLagMs = (m > 0) ? m : 0;
        log_line("[PlayerX64] Live appsrc latency cap = %d ms (RUBY_MAXLAG_MS=%s).", s_iMaxLiveLagMs, szMaxLagMs); } }

   int iRes = g_bPlayFile ? _do_file_mode() : _do_sm_mode();

   _destroy_pipeline();
   return iRes;
}
