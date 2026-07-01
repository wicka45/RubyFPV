/*
    Ruby Licence
    Copyright (c) 2020-2025 Petru Soroaga petrusoroaga@yahoo.com
    All rights reserved.

    x64 decoded-frame display transport.
    On x64 the GS console has no display server (DRM/KMS only) and i915 forbids a
    second process from doing DRM plane commits while ruby_central holds DRM master.
    So the player (ruby_player_x64) decodes via VA-API and publishes raw decoded
    frames here; ruby_central (the sole DRM master) reads the latest frame and puts
    it on a dedicated DRM video plane (zpos below the OSD plane).

    Single-frame seqlock: the writer increments uSeq to odd before writing the frame
    body, then to even when done. The reader samples uSeq (must be even), reads the
    body, then re-checks uSeq is unchanged; otherwise it retries / skips.
*/
#pragma once
#include <stdint.h>

#define SHARED_MEM_VIDEO_DISP_NAME "/RUBY_VIDEO_DISP"

// Max supported decoded frame: 1920x1088 (H264 16-aligned), 4 bytes/pixel (BGRx/XRGB8888)
#define VIDEO_DISP_MAX_WIDTH   1920
#define VIDEO_DISP_MAX_HEIGHT  1088
#define VIDEO_DISP_BYTES_PER_PIXEL 4
#define VIDEO_DISP_MAX_FRAME_SIZE (VIDEO_DISP_MAX_WIDTH * VIDEO_DISP_MAX_HEIGHT * VIDEO_DISP_BYTES_PER_PIXEL)

#define VIDEO_DISP_MAGIC 0x52564944u  // 'RVID'
#define VIDEO_DISP_FOURCC_XRGB8888 0x34325258u  // DRM_FORMAT_XRGB8888 = fourcc('X','R','2','4'); body is little-endian BGRx

typedef struct
{
   volatile uint32_t uSeq;      // seqlock; odd = write in progress, even = stable
   uint32_t uMagic;             // VIDEO_DISP_MAGIC once the writer is live
   uint32_t uWidth;             // frame width in pixels
   uint32_t uHeight;            // frame height in pixels
   uint32_t uStride;            // bytes per row (>= uWidth*4)
   uint32_t uFourCC;            // DRM_FORMAT_XRGB8888 (frame body is little-endian BGRx)
   uint32_t uFrameSize;         // valid bytes in data[] = uStride * uHeight
   uint32_t uReserved;
   uint8_t  data[VIDEO_DISP_MAX_FRAME_SIZE];
} type_video_disp_shm;

#define SHARED_MEM_VIDEO_DISP_SIZE ((int)sizeof(type_video_disp_shm))
