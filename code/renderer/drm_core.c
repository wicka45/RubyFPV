/*
    Ruby Licence
    Copyright (c) 2020-2025 Petru Soroaga petrusoroaga@yahoo.com
    All rights reserved.

    Redistribution and/or use in source and/or binary forms, with or without
    modification, are permitted provided that the following conditions are met:
        * Redistributions and/or use of the source code (partially or complete) must retain
        the above copyright notice, this list of conditions and the following disclaimer
        in the documentation and/or other materials provided with the distribution.
        * Redistributions in binary form (partially or complete) must reproduce
        the above copyright notice, this list of conditions and the following disclaimer
        in the documentation and/or other materials provided with the distribution.
        * Copyright info and developer info must be preserved as is in the user
        interface, additions could be made to that info.
        * Neither the name of the organization nor the
        names of its contributors may be used to endorse or promote products
        derived from this software without specific prior written permission.
        * Military use is not per
        mited.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
    ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
    WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
    DISCLAIMED. IN NO EVENT SHALL THE AUTHOR (PETRU SOROAGA) BE LIABLE FOR ANY
    DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
    (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
    LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
    ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
    SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/


#include "drm_core.h"
#include "../base/base.h"
#include "../base/hardware.h"
#include <errno.h>
#include <fcntl.h>
#include <unistd.h> 
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

int s_fdDRM = -1;
static int s_bOsdOnOverlay = 0;   // x64: OSD is on an ARGB overlay plane (above the primary video plane)
type_drm_display_attributes s_DRMDisplayAttributes;
type_drm_runtime_state s_DRMRuntimeState;

// ---------------- Windowed (X11 / XWayland) output backend (x64) ----------------
// When the GS is launched inside a graphical session, render into an X window instead of taking the
// DRM/KMS device (kiosk). RenderEngineCairo draws the OSD + composited video into FIXED-size buffers
// exactly like the DRM draw buffers; on present we scale-blit them TO FIT the window via cairo-xlib,
// which uses the X RENDER extension (GPU-accelerated on i915) -- so maximize/resize scales the picture
// (aspect-preserved, letterboxed) instead of clipping it. Runs natively on X and, via XWayland, on
// Wayland. EVERYTHING here is gated on s_bWindowed; when 0 the DRM path below is byte-for-byte unchanged.
#if defined(HW_PLATFORM_X64)
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>
static int s_bWindowed = 0;
static Display* s_pXDisplay = NULL;
static Window s_XWindow = 0;
static Atom s_XWMDelete = 0;
static int s_bWindowClosed = 0;
static int s_bFullscreen = 0;     // windowed: current _NET_WM_STATE_FULLSCREEN state (toggled by F11)
static int s_iWinW = 0, s_iWinH = 0;              // current on-screen window size (updated on resize)
static int s_iPendingWinW = 0, s_iPendingWinH = 0; // resize requested by the pump (keyboard thread)
static int s_iRenderW = 0, s_iRenderH = 0;        // fixed render size: OSD + composited video drawn at this
static uint8_t* s_pWinBuf[2] = { NULL, NULL };    // render buffers (ARGB32) that RenderEngineCairo draws into
static cairo_surface_t* s_pImgSurf[2] = { NULL, NULL }; // cairo image surfaces wrapping those buffers
static Visual* s_pXVisual = NULL;
static int s_iXDepth = 24;
static GC s_XGC = NULL;
static Pixmap s_BackPixmap = 0;                   // off-screen back buffer (double-buffering; window-sized)
static cairo_surface_t* s_pBackSurface = NULL;    // cairo-xlib surface on the back pixmap (X RENDER GPU scaling)
static cairo_t* s_pBackCtx = NULL;

int ruby_drm_core_window_closed() { return s_bWindowClosed; }
int ruby_drm_core_is_windowed() { return s_bWindowed; }

// Create/recreate the off-screen back buffer (window-sized pixmap + cairo surface). Returns 0 on success.
static int _ruby_x11_make_backbuffer(int w, int h)
{
   if ( NULL != s_pBackCtx )     { cairo_destroy(s_pBackCtx); s_pBackCtx = NULL; }
   if ( NULL != s_pBackSurface ) { cairo_surface_destroy(s_pBackSurface); s_pBackSurface = NULL; }
   if ( 0 != s_BackPixmap )      { XFreePixmap(s_pXDisplay, s_BackPixmap); s_BackPixmap = 0; }
   if ( w < 1 ) w = 1;
   if ( h < 1 ) h = 1;
   s_BackPixmap = XCreatePixmap(s_pXDisplay, s_XWindow, (unsigned)w, (unsigned)h, (unsigned)s_iXDepth);
   s_pBackSurface = cairo_xlib_surface_create(s_pXDisplay, s_BackPixmap, s_pXVisual, w, h);
   s_pBackCtx = (NULL != s_pBackSurface) ? cairo_create(s_pBackSurface) : NULL;
   return (NULL != s_pBackCtx) ? 0 : -1;
}

// ---------------- GPU present path (EGL + GLES2) ----------------
// The default windowed present uploads the FIXED render-res frame as a texture once per frame and lets the
// GPU scale it to the window (GL_LINEAR). So CPU stays ~flat regardless of window size -- unlike the cairo /
// X RENDER fallback, which under XWayland scales in SOFTWARE (CPU grew with window pixels -> video chop when
// enlarged). Falls back to the cairo path if EGL init fails or RUBY_WINDOW_NOGL is set. Same thread as
// _ruby_x11_present (the render thread) keeps the context current; the keyboard thread never touches GL.
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include "../base/shared_mem_video_disp.h"
static int s_bUseGL = 0;
static EGLDisplay s_eglDisplay = EGL_NO_DISPLAY;
static EGLContext s_eglContext = EGL_NO_CONTEXT;
static EGLSurface s_eglSurface = EGL_NO_SURFACE;
static GLuint s_glTex = 0;
static GLuint s_glProg = 0;
static GLint s_glAttrPos = -1, s_glAttrUV = -1, s_glUniTex = -1;
static int s_iGLTexW = 0;   // texture width in pixels = stride/4 (>= render width; sample only the valid sub-width)
// Two-texture GPU composite (shared by windowed EGL-on-X and console EGL-on-GBM): the decoded video is its
// own texture (read from /RUBY_VIDEO_DISP) blended UNDER the OSD by the GPU, so ruby_central skips its
// software video->OSD blit (see ruby_drm_core_is_gpu_composite()).
static GLuint s_glTexVideo = 0;
static GLint s_glUniForceOpaque = -1;       // shader: 1.0 -> opaque (video pass) / 0.0 -> texel alpha (OSD pass)
static int s_iVidTexW = 0, s_iVidTexH = 0;  // allocated video-texture dims (= stride/4 x height)
static int s_iVidSrcW = 0, s_iVidSrcH = 0;  // last video frame's real w/h (aspect)
static uint32_t s_uVidLastSeq = 0xFFFFFFFFu;
static type_video_disp_shm* s_pVidShm = NULL;
static void _ruby_gl_upload_video(void);    // refreshes s_glTexVideo from /RUBY_VIDEO_DISP (defined below)

static GLuint _ruby_gl_compile(GLenum type, const char* szSrc)
{
   GLuint sh = glCreateShader(type);
   glShaderSource(sh, 1, &szSrc, NULL);
   glCompileShader(sh);
   GLint ok = 0; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
   if ( ! ok )
   {
      char szLog[512]; szLog[0] = 0; glGetShaderInfoLog(sh, sizeof(szLog), NULL, szLog);
      log_softerror_and_alarm("[DRMCore] GL shader compile failed: %s", szLog);
      glDeleteShader(sh);
      return 0;
   }
   return sh;
}

static void _ruby_egl_uninit()
{
   if ( EGL_NO_DISPLAY == s_eglDisplay )
      return;
   eglMakeCurrent(s_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
   if ( 0 != s_glTex )  { glDeleteTextures(1, &s_glTex); s_glTex = 0; }
   if ( 0 != s_glTexVideo ) { glDeleteTextures(1, &s_glTexVideo); s_glTexVideo = 0; }
   if ( 0 != s_glProg ) { glDeleteProgram(s_glProg); s_glProg = 0; }
   if ( EGL_NO_CONTEXT != s_eglContext ) { eglDestroyContext(s_eglDisplay, s_eglContext); s_eglContext = EGL_NO_CONTEXT; }
   if ( EGL_NO_SURFACE != s_eglSurface ) { eglDestroySurface(s_eglDisplay, s_eglSurface); s_eglSurface = EGL_NO_SURFACE; }
   eglTerminate(s_eglDisplay);
   s_eglDisplay = EGL_NO_DISPLAY;
   if ( NULL != s_pVidShm ) { munmap(s_pVidShm, SHARED_MEM_VIDEO_DISP_SIZE); s_pVidShm = NULL; }
   s_iVidTexW = 0; s_iVidTexH = 0; s_iVidSrcW = 0; s_iVidSrcH = 0; s_uVidLastSeq = 0xFFFFFFFFu;
   s_bUseGL = 0;
}

// Build the EGL context + GLES2 program/texture on the existing X window. Returns 0 on success (sets s_bUseGL).
static int _ruby_egl_init()
{
   if ( NULL != getenv("RUBY_WINDOW_NOGL") ) { log_line("[DRMCore] RUBY_WINDOW_NOGL set -> cairo present path"); return -1; }

   s_eglDisplay = eglGetDisplay((EGLNativeDisplayType)s_pXDisplay);
   if ( EGL_NO_DISPLAY == s_eglDisplay ) return -1;
   EGLint iMajor = 0, iMinor = 0;
   if ( ! eglInitialize(s_eglDisplay, &iMajor, &iMinor) ) { s_eglDisplay = EGL_NO_DISPLAY; return -1; }
   eglBindAPI(EGL_OPENGL_ES_API);

   const EGLint cfgAttr[] = {
      EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
      EGL_NONE
   };
   EGLConfig cfgs[32]; EGLint nCfg = 0;
   if ( (! eglChooseConfig(s_eglDisplay, cfgAttr, cfgs, 32, &nCfg)) || (nCfg < 1) ) { _ruby_egl_uninit(); return -1; }
   // Prefer the config whose native visual matches the window's visual (avoids EGL_BAD_MATCH on the surface).
   VisualID winVid = XVisualIDFromVisual(s_pXVisual);
   EGLConfig cfg = cfgs[0];
   for( EGLint i = 0; i < nCfg; i++ )
   {
      EGLint vid = 0;
      if ( eglGetConfigAttrib(s_eglDisplay, cfgs[i], EGL_NATIVE_VISUAL_ID, &vid) && ((VisualID)vid == winVid) ) { cfg = cfgs[i]; break; }
   }
   s_eglSurface = eglCreateWindowSurface(s_eglDisplay, cfg, (EGLNativeWindowType)s_XWindow, NULL);
   if ( EGL_NO_SURFACE == s_eglSurface ) { log_softerror_and_alarm("[DRMCore] eglCreateWindowSurface failed (0x%x)", (unsigned)eglGetError()); _ruby_egl_uninit(); return -1; }
   const EGLint ctxAttr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
   s_eglContext = eglCreateContext(s_eglDisplay, cfg, EGL_NO_CONTEXT, ctxAttr);
   if ( EGL_NO_CONTEXT == s_eglContext ) { _ruby_egl_uninit(); return -1; }
   if ( ! eglMakeCurrent(s_eglDisplay, s_eglSurface, s_eglSurface, s_eglContext) ) { _ruby_egl_uninit(); return -1; }
   // No app-side vsync by default: under a compositor (Mutter via XWayland) the compositor does the final
   // sync, so blocking the app on its own vsync just adds a frame of latency (the old cairo path never
   // blocked) -- this is what made the windowed display feel delayed/choppy. Mutter still presents whole
   // frames so there's no tearing. Set RUBY_WINDOW_VSYNC=1 to force app vsync (e.g. on a non-compositing WM).
   { const char* e = getenv("RUBY_WINDOW_VSYNC"); eglSwapInterval(s_eglDisplay, (e && (0 != atoi(e))) ? 1 : 0); }

   static const char* szVS =
      "attribute vec2 aPos; attribute vec2 aUV; varying vec2 vUV;\n"
      "void main(){ vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }\n";
   // cairo ARGB32 LE = bytes B,G,R,A (premultiplied); .bgr is RGB; alpha = uForceOpaque?1:texel.a (video opaque, OSD premult).
   static const char* szFS =
      "precision mediump float; varying vec2 vUV; uniform sampler2D uTex; uniform float uForceOpaque;\n"
      "void main(){ vec4 t=texture2D(uTex, vUV); gl_FragColor = vec4(t.bgr, mix(t.a,1.0,uForceOpaque)); }\n";
   GLuint vs = _ruby_gl_compile(GL_VERTEX_SHADER, szVS);
   GLuint fs = _ruby_gl_compile(GL_FRAGMENT_SHADER, szFS);
   if ( (0 == vs) || (0 == fs) ) { if (vs) glDeleteShader(vs); if (fs) glDeleteShader(fs); _ruby_egl_uninit(); return -1; }
   s_glProg = glCreateProgram();
   glAttachShader(s_glProg, vs);
   glAttachShader(s_glProg, fs);
   glLinkProgram(s_glProg);
   glDeleteShader(vs);
   glDeleteShader(fs);
   GLint linked = 0; glGetProgramiv(s_glProg, GL_LINK_STATUS, &linked);
   if ( ! linked ) { char szLog[512]; szLog[0]=0; glGetProgramInfoLog(s_glProg, sizeof(szLog), NULL, szLog); log_softerror_and_alarm("[DRMCore] GL link failed: %s", szLog); _ruby_egl_uninit(); return -1; }
   s_glAttrPos = glGetAttribLocation(s_glProg, "aPos");
   s_glAttrUV  = glGetAttribLocation(s_glProg, "aUV");
   s_glUniTex  = glGetUniformLocation(s_glProg, "uTex");
   s_glUniForceOpaque = glGetUniformLocation(s_glProg, "uForceOpaque");

   // Frame texture: width = stride/4 (padded), height = render height. The valid image is the left sub-width.
   s_iGLTexW = (int)(s_DRMRuntimeState.drawBuffers[0].uStride / 4);
   if ( s_iGLTexW < 1 ) s_iGLTexW = s_iRenderW;
   glGenTextures(1, &s_glTex);
   glBindTexture(GL_TEXTURE_2D, s_glTex);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, s_iGLTexW, s_iRenderH, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
   // Video-layer texture (lazily sized to the stream in _ruby_gl_upload_video).
   glGenTextures(1, &s_glTexVideo);
   glBindTexture(GL_TEXTURE_2D, s_glTexVideo);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   s_iVidTexW = 0; s_iVidTexH = 0; s_iVidSrcW = 0; s_iVidSrcH = 0; s_uVidLastSeq = 0xFFFFFFFFu;

   glDisable(GL_BLEND);
   glDisable(GL_DEPTH_TEST);
   glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
   s_bUseGL = 1;
   log_line("[DRMCore] WINDOWED GPU present: EGL %d.%d / GLES2, tex %dx%d (GPU scale-to-window)", iMajor, iMinor, s_iGLTexW, s_iRenderH);
   return 0;
}

// GPU present: upload the front render buffer as a texture, draw a letterboxed quad scaled to the window.
static void _ruby_x11_present_gl(int iFront)
{
   _ruby_gl_upload_video();   // refresh the video layer from /RUBY_VIDEO_DISP (GPU blends it; CPU does no blit)

   // OSD layer = the draw buffer (OSD-only / transparent where there's no OSD, since the blit is skipped).
   int iTexW = (int)(s_DRMRuntimeState.drawBuffers[iFront].uStride / 4);
   if ( iTexW < 1 ) iTexW = s_iGLTexW;
   glBindTexture(GL_TEXTURE_2D, s_glTex);
   glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
   glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, iTexW, s_iRenderH, GL_RGBA, GL_UNSIGNED_BYTE, s_pWinBuf[iFront]);

   glViewport(0, 0, s_iWinW, s_iWinH);
   glClear(GL_COLOR_BUFFER_BIT);   // black letterbox bars

   double fScale = (double)s_iWinW / (double)s_iRenderW;
   double fSY = (double)s_iWinH / (double)s_iRenderH;
   if ( fSY < fScale ) fScale = fSY;
   if ( fScale <= 0.0 ) fScale = 1.0;
   float ndcW = (float)(((double)s_iRenderW * fScale) / (double)s_iWinW);   // half-extent of the fitted render area in NDC
   float ndcH = (float)(((double)s_iRenderH * fScale) / (double)s_iWinH);

   glUseProgram(s_glProg);
   glActiveTexture(GL_TEXTURE0);
   glUniform1i(s_glUniTex, 0);
   glEnableVertexAttribArray(s_glAttrPos);
   glEnableVertexAttribArray(s_glAttrUV);

   // PASS 1: video (opaque), filling the render area (the FPV feed underlays the OSD). Skipped until 1st frame.
   if ( (s_iVidTexW > 0) && (s_iVidSrcW > 0) )
   {
      float uMaxV = (float)((double)s_iVidSrcW / (double)s_iVidTexW);
      const GLfloat vv[] = {
         -ndcW,  ndcH, 0.0f,  0.0f,  -ndcW, -ndcH, 0.0f,  1.0f,
          ndcW,  ndcH, uMaxV, 0.0f,   ndcW, -ndcH, uMaxV, 1.0f,
      };
      glDisable(GL_BLEND);
      glUniform1f(s_glUniForceOpaque, 1.0f);
      glBindTexture(GL_TEXTURE_2D, s_glTexVideo);
      glVertexAttribPointer(s_glAttrPos, 2, GL_FLOAT, GL_FALSE, 4*sizeof(GLfloat), vv);
      glVertexAttribPointer(s_glAttrUV,  2, GL_FLOAT, GL_FALSE, 4*sizeof(GLfloat), vv+2);
      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
   }

   // PASS 2: OSD (premultiplied alpha) over the render area.
   {
      float uMaxO = (s_iGLTexW > 0) ? (float)((double)s_iRenderW / (double)s_iGLTexW) : 1.0f;
      const GLfloat ov[] = {
         -ndcW,  ndcH, 0.0f,  0.0f,  -ndcW, -ndcH, 0.0f,  1.0f,
          ndcW,  ndcH, uMaxO, 0.0f,   ndcW, -ndcH, uMaxO, 1.0f,
      };
      glEnable(GL_BLEND);
      glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);   // cairo ARGB32 is premultiplied
      glUniform1f(s_glUniForceOpaque, 0.0f);
      glBindTexture(GL_TEXTURE_2D, s_glTex);
      glVertexAttribPointer(s_glAttrPos, 2, GL_FLOAT, GL_FALSE, 4*sizeof(GLfloat), ov);
      glVertexAttribPointer(s_glAttrUV,  2, GL_FLOAT, GL_FALSE, 4*sizeof(GLfloat), ov+2);
      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
      glDisable(GL_BLEND);
   }

   eglSwapBuffers(s_eglDisplay, s_eglSurface);
}

// ---------------- Console GPU present (EGL-on-GBM + DRM page-flip) ----------------
// Opt-in (RUBY_DRM_GL=1): render the draw buffer with GLES2 into a GBM scanout buffer and page-flip it,
// instead of the software-only dumb-buffer present. STAGE 1: single-texture -- uploads the already-composited
// OSD+video draw buffer (ruby_central still software-composites it) and GPU-presents it 1:1 to the mode.
// Proves the GBM/EGL/page-flip path with no central change + auto-fallback to the dumb-buffer path on any
// failure. STAGE 2 (later) adds the second video texture + skips the software blit = the actual CPU/thermal win.
#include <gbm.h>
#include <EGL/eglext.h>
#include <poll.h>
static int s_bDrmGL = 0;
static struct gbm_device* s_pGbm = NULL;
static struct gbm_surface* s_pGbmSurf = NULL;
static struct gbm_bo* s_pGbmPrevBo = NULL;
static int s_bDrmGLFirst = 1;
static int s_iDrmModeW = 0, s_iDrmModeH = 0;   // GBM/scanout (panel) size, distinct from the OSD render res (s_iRenderW/H)
// (two-texture video-composite statics + shared_mem_video_disp.h are declared up with the windowed GL statics)

static void _ruby_gbm_fb_destroy(struct gbm_bo* bo, void* data)
{
   uint32_t fb = (uint32_t)(uintptr_t)data;
   if ( 0 != fb )
      drmModeRmFB(gbm_device_get_fd(gbm_bo_get_device(bo)), fb);
}
// Get-or-create a DRM framebuffer for a gbm_bo, cached on the bo via user_data (freed on bo destroy).
static uint32_t _ruby_gbm_fb_for_bo(struct gbm_bo* bo)
{
   void* p = gbm_bo_get_user_data(bo);
   if ( NULL != p )
      return (uint32_t)(uintptr_t)p;
   uint32_t uW = gbm_bo_get_width(bo), uH = gbm_bo_get_height(bo), uStride = gbm_bo_get_stride(bo);
   uint32_t uHandle = gbm_bo_get_handle(bo).u32;
   uint32_t handles[4] = { uHandle, 0, 0, 0 }, pitches[4] = { uStride, 0, 0, 0 }, offsets[4] = { 0, 0, 0, 0 };
   uint32_t fb = 0;
   if ( drmModeAddFB2(s_fdDRM, uW, uH, DRM_FORMAT_XRGB8888, handles, pitches, offsets, &fb, 0) )
   {
      log_softerror_and_alarm("[DRMCore] GBM: drmModeAddFB2 failed (%d)", errno);
      return 0;
   }
   gbm_bo_set_user_data(bo, (void*)(uintptr_t)fb, _ruby_gbm_fb_destroy);
   return fb;
}

static void _ruby_drm_gl_uninit()
{
   if ( EGL_NO_DISPLAY != s_eglDisplay )
   {
      eglMakeCurrent(s_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      if ( 0 != s_glTex )  { glDeleteTextures(1, &s_glTex); s_glTex = 0; }
      if ( 0 != s_glTexVideo ) { glDeleteTextures(1, &s_glTexVideo); s_glTexVideo = 0; }
      if ( 0 != s_glProg ) { glDeleteProgram(s_glProg); s_glProg = 0; }
      if ( EGL_NO_CONTEXT != s_eglContext ) { eglDestroyContext(s_eglDisplay, s_eglContext); s_eglContext = EGL_NO_CONTEXT; }
      if ( EGL_NO_SURFACE != s_eglSurface ) { eglDestroySurface(s_eglDisplay, s_eglSurface); s_eglSurface = EGL_NO_SURFACE; }
      eglTerminate(s_eglDisplay); s_eglDisplay = EGL_NO_DISPLAY;
   }
   if ( NULL != s_pGbmPrevBo ) { gbm_surface_release_buffer(s_pGbmSurf, s_pGbmPrevBo); s_pGbmPrevBo = NULL; }
   if ( NULL != s_pGbmSurf )   { gbm_surface_destroy(s_pGbmSurf); s_pGbmSurf = NULL; }
   if ( NULL != s_pGbm )       { gbm_device_destroy(s_pGbm); s_pGbm = NULL; }
   for( int i=0; i<2; i++ ) { if ( NULL != s_pWinBuf[i] ) { free(s_pWinBuf[i]); s_pWinBuf[i] = NULL; } }
   if ( NULL != s_pVidShm ) { munmap(s_pVidShm, SHARED_MEM_VIDEO_DISP_SIZE); s_pVidShm = NULL; }
   s_iVidTexW = 0; s_iVidTexH = 0; s_iVidSrcW = 0; s_iVidSrcH = 0; s_uVidLastSeq = 0xFFFFFFFFu;
   s_bDrmGL = 0;
}
// True when the GPU two-texture composite is active (console EGL-on-GBM): ruby_central then SKIPS its
// software video->OSD blit and renders OSD-only (transparent) -- the GPU blends the video under it.
int ruby_drm_core_is_gpu_composite() { return (s_bUseGL || s_bDrmGL); }

// Build the EGL-on-GBM context + GLES2 program/texture + CPU draw buffers at the mode size. Returns 0 on success.
static int _ruby_drm_gl_init(int iModeW, int iModeH)
{
   if ( (iModeW <= 0) || (iModeH <= 0) ) return -1;
   log_line("[DRMCore] GBM: init start (fd=%d, mode %dx%d)", s_fdDRM, iModeW, iModeH);
   s_pGbm = gbm_create_device(s_fdDRM);
   if ( NULL == s_pGbm ) { log_softerror_and_alarm("[DRMCore] gbm_create_device failed"); return -1; }
   s_pGbmSurf = gbm_surface_create(s_pGbm, (uint32_t)iModeW, (uint32_t)iModeH, GBM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
   if ( NULL == s_pGbmSurf ) { log_softerror_and_alarm("[DRMCore] gbm_surface_create failed"); _ruby_drm_gl_uninit(); return -1; }

   PFNEGLGETPLATFORMDISPLAYEXTPROC pGetPlatDisp = (PFNEGLGETPLATFORMDISPLAYEXTPROC) eglGetProcAddress("eglGetPlatformDisplayEXT");
   if ( NULL != pGetPlatDisp ) s_eglDisplay = pGetPlatDisp(EGL_PLATFORM_GBM_KHR, s_pGbm, NULL);
   else                        s_eglDisplay = eglGetDisplay((EGLNativeDisplayType)s_pGbm);
   if ( EGL_NO_DISPLAY == s_eglDisplay ) { log_softerror_and_alarm("[DRMCore] eglGetDisplay(GBM) failed"); _ruby_drm_gl_uninit(); return -1; }
   log_line("[DRMCore] GBM: eglDisplay ok, initializing");
   EGLint iMajor = 0, iMinor = 0;
   if ( ! eglInitialize(s_eglDisplay, &iMajor, &iMinor) ) { log_softerror_and_alarm("[DRMCore] GBM: eglInitialize failed 0x%x", (unsigned)eglGetError()); s_eglDisplay = EGL_NO_DISPLAY; _ruby_drm_gl_uninit(); return -1; }
   if ( ! eglBindAPI(EGL_OPENGL_ES_API) ) log_softerror_and_alarm("[DRMCore] GBM: eglBindAPI(ES) failed 0x%x", (unsigned)eglGetError());
   log_line("[DRMCore] GBM: EGL %d.%d initialized", iMajor, iMinor);
   const EGLint cfgAttr[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };
   EGLConfig cfgs[32]; EGLint nCfg = 0;
   if ( (! eglChooseConfig(s_eglDisplay, cfgAttr, cfgs, 32, &nCfg)) || (nCfg < 1) ) { log_softerror_and_alarm("[DRMCore] GBM: eglChooseConfig failed (n=%d) 0x%x", (int)nCfg, (unsigned)eglGetError()); _ruby_drm_gl_uninit(); return -1; }
   EGLConfig cfg = cfgs[0];
   for( EGLint i = 0; i < nCfg; i++ )
   {
      EGLint vid = 0;
      if ( eglGetConfigAttrib(s_eglDisplay, cfgs[i], EGL_NATIVE_VISUAL_ID, &vid) && ((uint32_t)vid == (uint32_t)GBM_FORMAT_XRGB8888) ) { cfg = cfgs[i]; break; }
   }
   log_line("[DRMCore] GBM: %d EGL configs; creating window surface", (int)nCfg);
   s_eglSurface = eglCreateWindowSurface(s_eglDisplay, cfg, (EGLNativeWindowType)s_pGbmSurf, NULL);
   if ( EGL_NO_SURFACE == s_eglSurface ) { log_softerror_and_alarm("[DRMCore] eglCreateWindowSurface(GBM) failed 0x%x", (unsigned)eglGetError()); _ruby_drm_gl_uninit(); return -1; }
   const EGLint ctxAttr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
   s_eglContext = eglCreateContext(s_eglDisplay, cfg, EGL_NO_CONTEXT, ctxAttr);
   if ( EGL_NO_CONTEXT == s_eglContext ) { log_softerror_and_alarm("[DRMCore] GBM: eglCreateContext failed 0x%x", (unsigned)eglGetError()); _ruby_drm_gl_uninit(); return -1; }
   if ( ! eglMakeCurrent(s_eglDisplay, s_eglSurface, s_eglSurface, s_eglContext) ) { log_softerror_and_alarm("[DRMCore] GBM: eglMakeCurrent failed 0x%x", (unsigned)eglGetError()); _ruby_drm_gl_uninit(); return -1; }
   log_line("[DRMCore] GBM: EGL context current; building GL program + buffers");

   static const char* szVS = "attribute vec2 aPos; attribute vec2 aUV; varying vec2 vUV;\nvoid main(){ vUV=aUV; gl_Position=vec4(aPos,0.0,1.0);}\n";
   // cairo ARGB32 = premultiplied, byte order B,G,R,A -> .bgr is RGB; alpha = uForceOpaque?1:texel.a (video=opaque, OSD=premult).
   static const char* szFS = "precision mediump float; varying vec2 vUV; uniform sampler2D uTex; uniform float uForceOpaque;\nvoid main(){ vec4 t=texture2D(uTex,vUV); gl_FragColor=vec4(t.bgr, mix(t.a,1.0,uForceOpaque));}\n";
   GLuint vs = _ruby_gl_compile(GL_VERTEX_SHADER, szVS);
   GLuint fs = _ruby_gl_compile(GL_FRAGMENT_SHADER, szFS);
   if ( (0 == vs) || (0 == fs) ) { if (vs) glDeleteShader(vs); if (fs) glDeleteShader(fs); _ruby_drm_gl_uninit(); return -1; }
   s_glProg = glCreateProgram(); glAttachShader(s_glProg, vs); glAttachShader(s_glProg, fs); glLinkProgram(s_glProg); glDeleteShader(vs); glDeleteShader(fs);
   GLint linked = 0; glGetProgramiv(s_glProg, GL_LINK_STATUS, &linked); if ( ! linked ) { log_softerror_and_alarm("[DRMCore] GBM: GL program link failed"); _ruby_drm_gl_uninit(); return -1; }
   s_glAttrPos = glGetAttribLocation(s_glProg, "aPos"); s_glAttrUV = glGetAttribLocation(s_glProg, "aUV"); s_glUniTex = glGetUniformLocation(s_glProg, "uTex");
   s_glUniForceOpaque = glGetUniformLocation(s_glProg, "uForceOpaque");

   // Scanout (GBM surface) is the panel mode; the OSD/video RENDER res = RUBY_WINDOW_W/H if set (per-host CPU/
   // battery knob), else the panel mode. When they differ the present letterboxes + GPU-upscales render->panel.
   s_iDrmModeW = iModeW; s_iDrmModeH = iModeH;
   int iRenderW = iModeW, iRenderH = iModeH;
   { const char* e = getenv("RUBY_WINDOW_W"); if ( (NULL != e) && (atoi(e) > 0) ) iRenderW = atoi(e); }
   { const char* e = getenv("RUBY_WINDOW_H"); if ( (NULL != e) && (atoi(e) > 0) ) iRenderH = atoi(e); }
   s_iRenderW = iRenderW; s_iRenderH = iRenderH;
   // RenderEngineCairo draws at the display-attributes size -> it MUST match the draw buffers (the render res),
   // not the panel scanout, or the OSD renders at the wrong stride into a smaller buffer (garbled screen).
   s_DRMDisplayAttributes.iWidth = iRenderW; s_DRMDisplayAttributes.iHeight = iRenderH;
   int iStride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, iRenderW);
   s_iGLTexW = iStride / 4;
   for( int i=0; i<2; i++ )
   {
      s_pWinBuf[i] = (uint8_t*) calloc(1, (size_t)iStride * iRenderH);
      if ( NULL == s_pWinBuf[i] ) { log_softerror_and_alarm("[DRMCore] GBM: draw buffer alloc failed"); _ruby_drm_gl_uninit(); return -1; }
      memset(&s_DRMRuntimeState.drawBuffers[i], 0, sizeof(type_drm_buffer));
      s_DRMRuntimeState.drawBuffers[i].uWidth = (uint32_t)iRenderW; s_DRMRuntimeState.drawBuffers[i].uHeight = (uint32_t)iRenderH;
      s_DRMRuntimeState.drawBuffers[i].uStride = (uint32_t)iStride; s_DRMRuntimeState.drawBuffers[i].uSize = (uint32_t)iStride * (uint32_t)iRenderH;
      s_DRMRuntimeState.drawBuffers[i].pData = s_pWinBuf[i]; s_DRMRuntimeState.drawBuffers[i].uBufferId = (uint32_t)(i+1);
   }
   glGenTextures(1, &s_glTex); glBindTexture(GL_TEXTURE_2D, s_glTex);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, s_iGLTexW, iRenderH, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
   // Video-layer texture (allocated lazily on first frame, grown to the stream size).
   glGenTextures(1, &s_glTexVideo); glBindTexture(GL_TEXTURE_2D, s_glTexVideo);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   s_iVidTexW = 0; s_iVidTexH = 0; s_iVidSrcW = 0; s_iVidSrcH = 0; s_uVidLastSeq = 0xFFFFFFFFu;
   glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

   s_DRMRuntimeState.iActiveOnScreenDrawBuffer = 0;
   s_bDrmGLFirst = 1; s_bDrmGL = 1;
   log_line("[DRMCore] CONSOLE GPU present: EGL-on-GBM %d.%d / GLES2, scanout %dx%d, render %dx%d", iMajor, iMinor, iModeW, iModeH, iRenderW, iRenderH);
   return 0;
}

static void _ruby_gbm_flip_handler(int fd, unsigned int frame, unsigned int sec, unsigned int usec, void* data)
{
   (void)fd; (void)frame; (void)sec; (void)usec;
   if ( NULL != data ) *(int*)data = 0;
}

// Refresh the video-layer texture from /RUBY_VIDEO_DISP (direct GL upload, no staging copy). Keeps the last
// frame if none new / writer mid-frame, so the video layer never flashes black between frames.
static void _ruby_gl_upload_video()
{
   if ( NULL == s_pVidShm )
   {
      int fd = shm_open(SHARED_MEM_VIDEO_DISP_NAME, O_RDONLY, S_IRUSR | S_IWUSR);
      if ( fd < 0 ) return;
      s_pVidShm = (type_video_disp_shm*) mmap(NULL, SHARED_MEM_VIDEO_DISP_SIZE, PROT_READ, MAP_SHARED, fd, 0);
      close(fd);
      if ( (MAP_FAILED == s_pVidShm) || (NULL == s_pVidShm) ) { s_pVidShm = NULL; return; }
   }
   type_video_disp_shm* pv = s_pVidShm;
   if ( pv->uMagic != VIDEO_DISP_MAGIC ) return;
   uint32_t uSeq = pv->uSeq;
   if ( 0 != (uSeq & 1u) ) return;            // writer mid-frame -> keep last texture
   if ( uSeq == s_uVidLastSeq ) return;       // no new frame -> keep last texture
   uint32_t w = pv->uWidth, h = pv->uHeight, stride = pv->uStride, sz = pv->uFrameSize;
   if ( (w == 0) || (h == 0) || (sz == 0) || (sz > (uint32_t)VIDEO_DISP_MAX_FRAME_SIZE) ) return;
   int iTexW = (int)(stride / 4);
   glBindTexture(GL_TEXTURE_2D, s_glTexVideo);
   glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
   if ( (iTexW != s_iVidTexW) || ((int)h != s_iVidTexH) )
   {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, iTexW, (int)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pv->data);
      s_iVidTexW = iTexW; s_iVidTexH = (int)h;
   }
   else
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, iTexW, (int)h, GL_RGBA, GL_UNSIGNED_BYTE, pv->data);
   __sync_synchronize();
   if ( pv->uSeq == uSeq ) { s_uVidLastSeq = uSeq; s_iVidSrcW = (int)w; s_iVidSrcH = (int)h; }  // not torn during upload
}

static void _ruby_drm_gl_present()
{
   int iFront = s_DRMRuntimeState.iActiveOnScreenDrawBuffer;

   _ruby_gl_upload_video();   // STAGE 2: refresh the video layer from shm (GPU blends it; CPU does no blit)

   // OSD layer = the draw buffer (OSD-only/transparent when GPU-compositing).
   glBindTexture(GL_TEXTURE_2D, s_glTex);
   glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
   glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, s_iGLTexW, s_iRenderH, GL_RGBA, GL_UNSIGNED_BYTE, s_pWinBuf[iFront]);

   glViewport(0, 0, s_iDrmModeW, s_iDrmModeH);   // full panel (scanout)
   glClear(GL_COLOR_BUFFER_BIT);
   glUseProgram(s_glProg);
   glActiveTexture(GL_TEXTURE0);
   glUniform1i(s_glUniTex, 0);
   glEnableVertexAttribArray(s_glAttrPos);
   glEnableVertexAttribArray(s_glAttrUV);

   // The render area is letterboxed within the panel: identity when render == panel; bars when a smaller/
   // differently-shaped RUBY_WINDOW_W/H render res is used (e.g. 1080p 16:9 on a 2880x1800 16:10 panel).
   double fRS = (double)s_iDrmModeW / (double)s_iRenderW;
   double fRSy = (double)s_iDrmModeH / (double)s_iRenderH;
   if ( fRSy < fRS ) fRS = fRSy;
   if ( fRS <= 0.0 ) fRS = 1.0;
   float ndcRW = (float)(((double)s_iRenderW * fRS) / (double)s_iDrmModeW);
   float ndcRH = (float)(((double)s_iRenderH * fRS) / (double)s_iDrmModeH);

   // PASS 1: video (opaque), aspect-fit within the render area. Skipped until the first frame arrives.
   if ( (s_iVidTexW > 0) && (s_iVidSrcW > 0) && (s_iVidSrcH > 0) )
   {
      double fs = (double)s_iRenderW / (double)s_iVidSrcW;
      double fy = (double)s_iRenderH / (double)s_iVidSrcH;
      if ( fy < fs ) fs = fy;
      float ndcW = ndcRW * (float)(((double)s_iVidSrcW * fs) / (double)s_iRenderW);
      float ndcH = ndcRH * (float)(((double)s_iVidSrcH * fs) / (double)s_iRenderH);
      float uMaxV = (s_iVidTexW > 0) ? (float)((double)s_iVidSrcW / (double)s_iVidTexW) : 1.0f;
      const GLfloat vv[] = { -ndcW, ndcH, 0.f, 0.f,  -ndcW, -ndcH, 0.f, 1.f,  ndcW, ndcH, uMaxV, 0.f,  ndcW, -ndcH, uMaxV, 1.f };
      glDisable(GL_BLEND);
      glUniform1f(s_glUniForceOpaque, 1.0f);
      glBindTexture(GL_TEXTURE_2D, s_glTexVideo);
      glVertexAttribPointer(s_glAttrPos, 2, GL_FLOAT, GL_FALSE, 4*sizeof(GLfloat), vv);
      glVertexAttribPointer(s_glAttrUV,  2, GL_FLOAT, GL_FALSE, 4*sizeof(GLfloat), vv+2);
      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
   }

   // PASS 2: OSD (premultiplied alpha) over the render area.
   {
      float uMaxO = (s_iGLTexW > 0) ? (float)((double)s_iRenderW / (double)s_iGLTexW) : 1.0f;
      const GLfloat ov[] = { -ndcRW, ndcRH, 0.f, 0.f,  -ndcRW, -ndcRH, 0.f, 1.f,  ndcRW, ndcRH, uMaxO, 0.f,  ndcRW, -ndcRH, uMaxO, 1.f };
      glEnable(GL_BLEND);
      glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);   // cairo ARGB32 is premultiplied
      glUniform1f(s_glUniForceOpaque, 0.0f);
      glBindTexture(GL_TEXTURE_2D, s_glTex);
      glVertexAttribPointer(s_glAttrPos, 2, GL_FLOAT, GL_FALSE, 4*sizeof(GLfloat), ov);
      glVertexAttribPointer(s_glAttrUV,  2, GL_FLOAT, GL_FALSE, 4*sizeof(GLfloat), ov+2);
      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
      glDisable(GL_BLEND);
   }

   eglSwapBuffers(s_eglDisplay, s_eglSurface);

   struct gbm_bo* bo = gbm_surface_lock_front_buffer(s_pGbmSurf);
   if ( NULL == bo ) return;
   uint32_t fb = _ruby_gbm_fb_for_bo(bo);
   if ( 0 == fb ) { gbm_surface_release_buffer(s_pGbmSurf, bo); return; }

   if ( s_bDrmGLFirst )
   {
      drmModeSetCrtc(s_fdDRM, s_DRMRuntimeState.objInfoCRTc.uObjId, fb, 0, 0, &s_DRMRuntimeState.objInfoConnector.uObjId, 1, &s_DRMRuntimeState.targetModeInfo);
      s_bDrmGLFirst = 0;
   }
   else
   {
      int iFlipPending = 1;
      if ( 0 == drmModePageFlip(s_fdDRM, s_DRMRuntimeState.objInfoCRTc.uObjId, fb, DRM_MODE_PAGE_FLIP_EVENT, &iFlipPending) )
      {
         drmEventContext ev; memset(&ev, 0, sizeof(ev)); ev.version = 2; ev.page_flip_handler = _ruby_gbm_flip_handler;
         struct pollfd pfd; pfd.fd = s_fdDRM; pfd.events = POLLIN;
         while ( iFlipPending ) { if ( poll(&pfd, 1, 100) <= 0 ) break; drmHandleEvent(s_fdDRM, &ev); }
      }
      else   // page-flip failed (e.g. mode lost) -> re-assert the mode
         drmModeSetCrtc(s_fdDRM, s_DRMRuntimeState.objInfoCRTc.uObjId, fb, 0, 0, &s_DRMRuntimeState.objInfoConnector.uObjId, 1, &s_DRMRuntimeState.targetModeInfo);
   }
   if ( NULL != s_pGbmPrevBo ) gbm_surface_release_buffer(s_pGbmSurf, s_pGbmPrevBo);
   s_pGbmPrevBo = bo;
}

// Ask the window manager to add/remove _NET_WM_STATE_FULLSCREEN on our window (true fullscreen: whole
// screen, no top bar / title bar). F11 toggles it; RUBY_WINDOW_FULLSCREEN=1 sets it at startup. The
// resulting resize comes back as a ConfigureNotify, so the present path re-fits the picture automatically.
static void _ruby_x11_set_fullscreen(int bOn)
{
   if ( (NULL == s_pXDisplay) || (0 == s_XWindow) )
      return;
   Atom aState = XInternAtom(s_pXDisplay, "_NET_WM_STATE", False);
   Atom aFull  = XInternAtom(s_pXDisplay, "_NET_WM_STATE_FULLSCREEN", False);
   XEvent xev;
   memset(&xev, 0, sizeof(xev));
   xev.type = ClientMessage;
   xev.xclient.window = s_XWindow;
   xev.xclient.message_type = aState;
   xev.xclient.format = 32;
   xev.xclient.data.l[0] = bOn ? 1 : 0;   // _NET_WM_STATE_ADD : _NET_WM_STATE_REMOVE
   xev.xclient.data.l[1] = (long)aFull;
   xev.xclient.data.l[2] = 0;
   xev.xclient.data.l[3] = 1;             // source indication: normal application
   XSendEvent(s_pXDisplay, RootWindow(s_pXDisplay, DefaultScreen(s_pXDisplay)), False,
              SubstructureNotifyMask | SubstructureRedirectMask, &xev);
   XFlush(s_pXDisplay);
   s_bFullscreen = bOn ? 1 : 0;
}

static int _ruby_x11_init(int iW, int iH)
{
   { const char* e = getenv("RUBY_WINDOW_W"); if ( NULL != e ) iW = atoi(e); }
   { const char* e = getenv("RUBY_WINDOW_H"); if ( NULL != e ) iH = atoi(e); }
   // Render at 1080p by default: the vehicles stream 1080p, so a 720p buffer would downscale the video on
   // composite and then the present would upscale it again (detail thrown away in the middle). At 1080p the
   // 1080p video composites 1:1 and the GPU present just scales that to the actual window size. Override with
   // RUBY_WINDOW_W/H (e.g. match a different stream resolution, or drop to 720p to save compositing CPU).
   if ( iW <= 0 ) iW = 1920;
   if ( iH <= 0 ) iH = 1080;

   XInitThreads();   // render thread does XShmPutImage while the keyboard thread does XNextEvent -> Xlib must be thread-safe
   s_pXDisplay = XOpenDisplay(NULL);
   if ( NULL == s_pXDisplay ) { log_softerror_and_alarm("[DRMCore] Windowed: XOpenDisplay(NULL) failed (DISPLAY=%s)", getenv("DISPLAY")?getenv("DISPLAY"):"(null)"); return -1; }
   int iScreen = DefaultScreen(s_pXDisplay);
   Visual* pVisual = DefaultVisual(s_pXDisplay, iScreen);
   int iDepth = DefaultDepth(s_pXDisplay, iScreen);
   s_iWinW = iW; s_iWinH = iH;

   s_XWindow = XCreateSimpleWindow(s_pXDisplay, RootWindow(s_pXDisplay, iScreen), 0, 0, iW, iH, 0,
       BlackPixel(s_pXDisplay, iScreen), BlackPixel(s_pXDisplay, iScreen));
   XStoreName(s_pXDisplay, s_XWindow, "RubyFPV Ground Station");
   // WM_CLASS lets the desktop environment identify this window and match it to rubyfpv.desktop
   // (which sets StartupWMClass=RubyFPV), so the taskbar shows the Ruby logo + proper name instead
   // of a generic gear labelled "unknown".
   {
      XClassHint* pClassHint = XAllocClassHint();
      if ( NULL != pClassHint )
      {
         pClassHint->res_name = (char*)"rubyfpv";
         pClassHint->res_class = (char*)"RubyFPV";
         XSetClassHint(s_pXDisplay, s_XWindow, pClassHint);
         XFree(pClassHint);
      }
   }
   s_XWMDelete = XInternAtom(s_pXDisplay, "WM_DELETE_WINDOW", False);
   XSetWMProtocols(s_pXDisplay, s_XWindow, &s_XWMDelete, 1);
   XSelectInput(s_pXDisplay, s_XWindow, StructureNotifyMask | KeyPressMask | KeyReleaseMask);
   XMapWindow(s_pXDisplay, s_XWindow);
   // Off-screen back buffer for double-buffering: render the whole frame here, then XCopyArea it to the
   // window in one op so the window never shows a half-drawn frame (no flashing/tearing).
   s_pXVisual = pVisual; s_iXDepth = iDepth;
   s_XGC = XCreateGC(s_pXDisplay, s_XWindow, 0, NULL);
   if ( 0 != _ruby_x11_make_backbuffer(iW, iH) ) { log_softerror_and_alarm("[DRMCore] Windowed: back buffer create failed"); return -1; }

   // Fixed render size (OSD + composited video drawn here); the window can be any size -> scale on present.
   s_iRenderW = iW; s_iRenderH = iH;
   int iStride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, iW);
   for( int i=0; i<2; i++ )
   {
      s_pWinBuf[i] = (uint8_t*) calloc(1, (size_t)iStride * iH);
      if ( NULL == s_pWinBuf[i] ) { log_softerror_and_alarm("[DRMCore] Windowed: render buffer alloc failed"); return -1; }
      s_pImgSurf[i] = cairo_image_surface_create_for_data(s_pWinBuf[i], CAIRO_FORMAT_ARGB32, iW, iH, iStride);
      memset(&s_DRMRuntimeState.drawBuffers[i], 0, sizeof(type_drm_buffer));
      s_DRMRuntimeState.drawBuffers[i].uWidth = iW;
      s_DRMRuntimeState.drawBuffers[i].uHeight = iH;
      s_DRMRuntimeState.drawBuffers[i].uStride = (uint32_t)iStride;
      s_DRMRuntimeState.drawBuffers[i].uSize = (uint32_t)iStride * iH;
      s_DRMRuntimeState.drawBuffers[i].pData = s_pWinBuf[i];
      s_DRMRuntimeState.drawBuffers[i].uBufferId = i+1;
   }
   XSync(s_pXDisplay, False);

   s_DRMRuntimeState.iActiveOnScreenDrawBuffer = 0;
   memset(&s_DRMDisplayAttributes, 0, sizeof(s_DRMDisplayAttributes));
   s_DRMDisplayAttributes.iWidth = iW;
   s_DRMDisplayAttributes.iHeight = iH;
   s_DRMDisplayAttributes.iBPP = 32;
   s_DRMDisplayAttributes.iRefreshRate = 60;
   // Try the GPU present path (EGL/GLES2 -> GPU scale-to-window). On any failure s_bUseGL stays 0 and present
   // uses the cairo-xlib path (software scale under XWayland). RUBY_WINDOW_NOGL=1 forces the cairo path.
   _ruby_egl_init();
   s_bWindowed = 1;
   log_line("[DRMCore] WINDOWED output ready: X11, render %dx%d depth %d (%s)", iW, iH, iDepth,
      s_bUseGL ? "GPU EGL/GLES2 scale-to-window" : "cairo-xlib software scale-to-window");
   if ( NULL != getenv("RUBY_WINDOW_FULLSCREEN") )
      _ruby_x11_set_fullscreen(1);   // start fullscreen; F11 still toggles back to a normal window
   return 0;
}

// Windowed keyboard: the GS's normal input path reads raw evdev (/dev/input), which is global and
// focus-agnostic -- correct for the DRM kiosk (sole app) but wrong in a window (it would grab every
// key regardless of focus and leak keys to/from other windows). So in windowed mode we instead take
// keys from THIS window's X KeyPress/KeyRelease events and queue them for keyboard.cpp to drain.
// X keycodes are the Linux evdev keycode + 8, so we store (keycode-8) and the existing code mapping works.
#define RUBY_X_KEY_QUEUE 128
static int s_XKeyQueue[RUBY_X_KEY_QUEUE][2];   // [evdev keycode, pressed?]
static int s_iXKeyHead = 0;
static int s_iXKeyTail = 0;

static void _ruby_x11_pump_events()
{
   if ( NULL == s_pXDisplay )
      return;
   while ( XPending(s_pXDisplay) )
   {
      XEvent ev; XNextEvent(s_pXDisplay, &ev);
      if ( (ev.type == ClientMessage) && ((Atom)ev.xclient.data.l[0] == s_XWMDelete) )
         s_bWindowClosed = 1;
      else if ( (ev.type == KeyPress) || (ev.type == KeyRelease) )
      {
         if ( 95 == ev.xkey.keycode )   // F11 (evdev 87 + 8): toggle fullscreen here; never forward to the GS
         {
            if ( ev.type == KeyPress )
               _ruby_x11_set_fullscreen(! s_bFullscreen);
         }
         else
         {
            int iNext = (s_iXKeyTail + 1) % RUBY_X_KEY_QUEUE;
            if ( iNext != s_iXKeyHead )   // queue not full -> enqueue (drop if full)
            {
               s_XKeyQueue[s_iXKeyTail][0] = (int)ev.xkey.keycode - 8;   // X keycode -> Linux evdev keycode
               s_XKeyQueue[s_iXKeyTail][1] = (ev.type == KeyPress) ? 1 : 0;
               s_iXKeyTail = iNext;
            }
         }
      }
      else if ( ev.type == ConfigureNotify )   // window resized/maximized: record it; present() applies it on the render thread
      {
         s_iPendingWinW = ev.xconfigure.width;
         s_iPendingWinH = ev.xconfigure.height;
      }
   }
}

static void _ruby_x11_present()
{
   // Apply a pending resize HERE (render thread). The EGL window surface auto-tracks the window size; the
   // cairo fallback needs a freshly-sized back pixmap.
   if ( (s_iPendingWinW > 0) && ((s_iPendingWinW != s_iWinW) || (s_iPendingWinH != s_iWinH)) )
   {
      s_iWinW = s_iPendingWinW; s_iWinH = s_iPendingWinH;
      if ( (! s_bUseGL) && (0 != _ruby_x11_make_backbuffer(s_iWinW, s_iWinH)) )
         return;
   }
   int iFront = s_DRMRuntimeState.iActiveOnScreenDrawBuffer;

   if ( s_bUseGL )   // GPU path: upload the frame as a texture and let the GPU scale it to the window
   {
      _ruby_x11_present_gl(iFront);
      return;
   }

   if ( (NULL == s_pBackCtx) || (NULL == s_pBackSurface) )
      return;
   cairo_surface_mark_dirty(s_pImgSurf[iFront]);   // RenderEngineCairo wrote the buffer outside this cairo ctx

   // Scale to fit the window, preserving aspect ratio (letterbox). X RENDER does the scale-blit on the GPU.
   double fScale = (double)s_iWinW / (double)s_iRenderW;
   double fScaleY = (double)s_iWinH / (double)s_iRenderH;
   if ( fScaleY < fScale ) fScale = fScaleY;
   if ( fScale <= 0.0 ) fScale = 1.0;
   double fDrawW = (double)s_iRenderW * fScale;
   double fDrawH = (double)s_iRenderH * fScale;

   // Render the COMPLETE frame into the off-screen back buffer first.
   cairo_save(s_pBackCtx);
   cairo_set_source_rgb(s_pBackCtx, 0, 0, 0);
   cairo_paint(s_pBackCtx);                              // black letterbox bars
   cairo_translate(s_pBackCtx, (s_iWinW - fDrawW)/2.0, (s_iWinH - fDrawH)/2.0);
   cairo_scale(s_pBackCtx, fScale, fScale);
   cairo_set_source_surface(s_pBackCtx, s_pImgSurf[iFront], 0, 0);
   cairo_pattern_set_filter(cairo_get_source(s_pBackCtx), CAIRO_FILTER_BILINEAR);
   cairo_paint(s_pBackCtx);
   cairo_restore(s_pBackCtx);
   cairo_surface_flush(s_pBackSurface);                  // ensure cairo's drawing has landed in the pixmap

   // One atomic copy: back buffer -> window. The window never shows a partially drawn frame (no flashing).
   XCopyArea(s_pXDisplay, s_BackPixmap, s_XWindow, s_XGC, 0, 0, (unsigned)s_iWinW, (unsigned)s_iWinH, 0, 0);
   XFlush(s_pXDisplay);
   // X events (keys, window-close, resize) are pumped by ruby_drm_core_poll_key() on the keyboard thread.
}

// Drain one queued window key event: *pCode = Linux evdev keycode, *pPressed = 1 press / 0 release.
// Returns 1 if an event was dequeued, 0 if the queue is empty (or not windowed).
int ruby_drm_core_poll_key(int* pCode, int* pPressed)
{
   if ( ! s_bWindowed )
      return 0;
   _ruby_x11_pump_events();
   if ( s_iXKeyHead == s_iXKeyTail )
      return 0;
   if ( NULL != pCode )    *pCode = s_XKeyQueue[s_iXKeyHead][0];
   if ( NULL != pPressed ) *pPressed = s_XKeyQueue[s_iXKeyHead][1];
   s_iXKeyHead = (s_iXKeyHead + 1) % RUBY_X_KEY_QUEUE;
   return 1;
}

// Called by ruby_central when a graphical session is detected. Returns 0 on success.
int ruby_drm_core_init_windowed(int iWidth, int iHeight)
{
   return _ruby_x11_init(iWidth, iHeight);
}
#else
int ruby_drm_core_window_closed() { return 0; }
int ruby_drm_core_is_windowed() { return 0; }
int ruby_drm_core_is_gpu_composite() { return 0; }
int ruby_drm_core_init_windowed(int iWidth, int iHeight) { (void)iWidth; (void)iHeight; return -1; }
int ruby_drm_core_poll_key(int* pCode, int* pPressed) { (void)pCode; (void)pPressed; return 0; }
#endif


int s_iDRMCoreInitialized = 0;
int s_iDRMEnableVSync = 1;

static const char *_ruby_drm_core_get_connector_str(uint32_t conn_type)
{
   switch (conn_type)
   {
      case DRM_MODE_CONNECTOR_Unknown:     return "Unknown";
      case DRM_MODE_CONNECTOR_VGA:         return "VGA";
      case DRM_MODE_CONNECTOR_DVII:        return "DVI-I";
      case DRM_MODE_CONNECTOR_DVID:        return "DVI-D";
      case DRM_MODE_CONNECTOR_DVIA:        return "DVI-A";
      case DRM_MODE_CONNECTOR_Composite:   return "Composite";
      case DRM_MODE_CONNECTOR_SVIDEO:      return "SVIDEO";
      case DRM_MODE_CONNECTOR_LVDS:        return "LVDS";
      case DRM_MODE_CONNECTOR_Component:   return "Component";
      case DRM_MODE_CONNECTOR_9PinDIN:     return "DIN";
      case DRM_MODE_CONNECTOR_DisplayPort: return "DP";
      case DRM_MODE_CONNECTOR_HDMIA:       return "HDMI-A";
      case DRM_MODE_CONNECTOR_HDMIB:       return "HDMI-B";
      case DRM_MODE_CONNECTOR_TV:          return "TV";
      case DRM_MODE_CONNECTOR_eDP:         return "eDP";
      case DRM_MODE_CONNECTOR_VIRTUAL:     return "Virtual";
      case DRM_MODE_CONNECTOR_DSI:         return "DSI";
      default:                             return "Unknown";
   }
   return "N/A";
}

const char* _ruby_drm_fourcc_to_string(uint32_t fourcc)
{
    char* result = malloc(5);
    result[0] = (char)((fourcc >> 0) & 0xFF);
    result[1] = (char)((fourcc >> 8) & 0xFF);
    result[2] = (char)((fourcc >> 16) & 0xFF);
    result[3] = (char)((fourcc >> 24) & 0xFF);
    result[4] = '\0';
    return result;
}

int _ruby_drm_get_object_properties(type_drm_object_info* pObject)
{
   if ( NULL == pObject )
      return -1;
   if ( 0xFFFFFFFF == pObject->uObjId )
      return -2;

   const char *szType;

   switch( pObject->uObjType )
   {
      case DRM_MODE_OBJECT_CONNECTOR: szType = "Connector"; break;
      case DRM_MODE_OBJECT_PLANE: szType = "Plane"; break;
      case DRM_MODE_OBJECT_CRTC: szType = "CRTC"; break;
      default: szType = "unknown type"; break;
   }
   pObject->pProperties = drmModeObjectGetProperties(s_fdDRM, pObject->uObjId, pObject->uObjType);
   if (! pObject->pProperties)
   {
      log_softerror_and_alarm("[DRMCore] Can't get object properties, Object type: %s, id: %u, error: %s",
         szType, pObject->uObjId, strerror(errno));
      return -1;
   }

   pObject->ppPropertiesInfo = calloc( pObject->pProperties->count_props, sizeof(drmModePropertyRes*));
   log_line("[DRMCore] Object %s, id: %u has %d properties", szType, pObject->uObjId, pObject->pProperties->count_props);
   for (int i = 0; i < pObject->pProperties->count_props; i++)
   {
       pObject->ppPropertiesInfo[i] = drmModeGetProperty(s_fdDRM, pObject->pProperties->props[i]);
       //if ( NULL != pObject->ppPropertiesInfo[i] )
       //if ( NULL != pObject->ppPropertiesInfo[i]->name )
       //   log_line("[DRMCore] Object %s property %d: %s", szType, i, pObject->ppPropertiesInfo[i]->name);
   }
   return 0;
}

void _ruby_drm_free_object_properties(type_drm_object_info* pObject)
{
   if ( NULL == pObject )
      return;
   for ( int i = 0; i < pObject->pProperties->count_props; i++ )
      drmModeFreeProperty(pObject->ppPropertiesInfo[i]);
   free(pObject->ppPropertiesInfo);
   drmModeFreeObjectProperties(pObject->pProperties);
}


int64_t _ruby_drm_get_object_property_value(drmModeObjectPropertiesPtr pProps, const char *szName)
{
   if ( NULL == szName )
      return -1;

   drmModePropertyPtr prop;
   uint64_t uValue = 0;
   int iFound = 0;
   
   for (int i = 0; (i < pProps->count_props) && (!iFound); i++)
   {
      prop = drmModeGetProperty(s_fdDRM, pProps->props[i]);
      if ( 0 == strcmp(prop->name, szName) )
      {
         uValue = pProps->prop_values[i];
         iFound = 1;
         log_line("[DRMCore] Got object property %s value: %u", szName, (u32)uValue);
      }
      drmModeFreeProperty(prop);
   }

   if (! iFound)
      return -1;
   return uValue;
}

static int _ruby_drm_try_open_device(const char* szPath)
{
   int iRet;
   uint64_t cap;
   int fd = open(szPath, O_RDWR | O_NONBLOCK);
   if ( fd < 0 )
      return -1;

   iRet = drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
   if ( iRet ) { close(fd); return iRet; }

   iRet = drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);
   if ( iRet ) { close(fd); return iRet; }

   if ( drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &cap) < 0 || (!cap) )
   { close(fd); return -EOPNOTSUPP; }

   if ( drmGetCap(fd, DRM_CAP_CRTC_IN_VBLANK_EVENT, &cap) < 0 || (!cap) )
   { close(fd); return -EOPNOTSUPP; }

   s_fdDRM = fd;
   log_line("[DRMCore] Opened DRM device: %s", szPath);
   return 0;
}

int _ruby_drm_open_device()
{
   const char* szEnv = getenv("RUBY_DRI_CARD");
   if ( szEnv && szEnv[0] )
   {
      if ( 0 == _ruby_drm_try_open_device(szEnv) )
         return 0;
      log_softerror_and_alarm("[DRMCore] RUBY_DRI_CARD=%s failed, probing cardN", szEnv);
   }

   char szPath[64];
   for ( int i = 0; i < 8; i++ )
   {
      snprintf(szPath, sizeof(szPath), "/dev/dri/card%d", i);
      if ( access(szPath, R_OK | W_OK) != 0 )
         continue;
      if ( 0 == _ruby_drm_try_open_device(szPath) )
         return 0;
   }

   log_softerror_and_alarm("[DRMCore] Failed to open any DRM graphics device (/dev/dri/card0..card7).");
   s_fdDRM = -1;
   return -1;
}


int _ruby_drm_core_enumerate_find_resources()
{
   if ( s_fdDRM < 0 )
      return -1;
  
   s_DRMRuntimeState.pAllDRMResources = drmModeGetResources(s_fdDRM);
   if ( !s_DRMRuntimeState.pAllDRMResources )
   {
      log_softerror_and_alarm("[DRMCore] Cannot retrieve DRM resources (%d)", errno);
      return -errno;
   }
 
   if ( s_DRMRuntimeState.pAllDRMResources->count_connectors <= 0 )
   {
      log_softerror_and_alarm("[DRMCore] No connectors available (%d)", errno);
      return -1;
   }
 
   log_line("[DRMCore] (Enumerate find resources) Finding resources (%d connectors, %d crtcs)...",
      s_DRMRuntimeState.pAllDRMResources->count_connectors, s_DRMRuntimeState.pAllDRMResources->count_crtcs);

   // Find connectors (displays, aka video hardware connectors (HDMI,DVI...))

   s_DRMRuntimeState.pConnector = NULL;

   for (int i = 0; i < s_DRMRuntimeState.pAllDRMResources->count_connectors; i++)
   {
      s_DRMRuntimeState.pConnector = drmModeGetConnector(s_fdDRM, s_DRMRuntimeState.pAllDRMResources->connectors[i]);
      if (!s_DRMRuntimeState.pConnector)
      {
         log_softerror_and_alarm("[DRMCore] Cannot retrieve DRM connector %u:%u (%d)",
              i, s_DRMRuntimeState.pAllDRMResources->connectors[i], errno);
         continue;
      }

      log_line("[DRMCore] Found connector: %s, %d, %s",
         _ruby_drm_core_get_connector_str(s_DRMRuntimeState.pConnector->connector_type),
         s_DRMRuntimeState.pConnector->connector_type_id,
         s_DRMRuntimeState.pConnector->connection == DRM_MODE_CONNECTED ? "connected" : "disonnected");

      if ( s_DRMRuntimeState.pConnector->count_modes <= 0 )
      {
         log_line("[DRMCore] Connector does not support any modes.");
         drmModeFreeConnector(s_DRMRuntimeState.pConnector);
         s_DRMRuntimeState.pConnector = NULL;
         continue;
      }

      // List supported modes for each display (connector)
      for (int j = 0; j < s_DRMRuntimeState.pConnector->count_modes; j++)
      {
         drmModeModeInfo *mode = &s_DRMRuntimeState.pConnector->modes[j];

         log_line("[DRMCore] Connector mode %d:  %dx%d%s@%d",
            j, mode->hdisplay, mode->vdisplay,
            mode->flags & DRM_MODE_FLAG_INTERLACE ? "i" : "",
            mode->vrefresh);

          if ( (mode->hdisplay == s_DRMDisplayAttributes.iWidth) &&
               (mode->vdisplay == s_DRMDisplayAttributes.iHeight) &&
               (mode->vrefresh == s_DRMDisplayAttributes.iRefreshRate) )
          {
             s_DRMDisplayAttributes.iInterleaved = (mode->flags & DRM_MODE_FLAG_INTERLACE )?1:0;
             s_DRMRuntimeState.objInfoConnector.uObjId = s_DRMRuntimeState.pConnector->connector_id;
             s_DRMRuntimeState.objInfoConnector.iObjIndex = i;
             s_DRMRuntimeState.targetModeInfo = s_DRMRuntimeState.pConnector->modes[j];

             memcpy(&s_DRMDisplayAttributes.currentMode, &s_DRMRuntimeState.pConnector->modes[j], sizeof(s_DRMDisplayAttributes.currentMode));
             
             drmModeCreatePropertyBlob(s_fdDRM, &s_DRMDisplayAttributes.currentMode, sizeof(s_DRMDisplayAttributes.currentMode), &s_DRMRuntimeState.uModeIdBlob);

             log_line("[DRMCore] Using this mode. Index %d", j);
             break;
          }
      }
      // Use first/native mode if autoselection or no exact match
      if ( s_DRMRuntimeState.objInfoConnector.uObjId == 0xFFFFFFFF &&
           s_DRMRuntimeState.pConnector->count_modes > 0 )
      {
         drmModeModeInfo *mode = &s_DRMRuntimeState.pConnector->modes[0];

         log_line("[DRMCore] Using auto mode: connector mode 0:  %dx%d%s@%d",
            mode->hdisplay, mode->vdisplay,
            mode->flags & DRM_MODE_FLAG_INTERLACE ? "i" : "",
            mode->vrefresh);

         s_DRMDisplayAttributes.iWidth = mode->hdisplay;
         s_DRMDisplayAttributes.iHeight = mode->vdisplay;
         s_DRMDisplayAttributes.iRefreshRate = mode->vrefresh;
         s_DRMDisplayAttributes.iInterleaved = (mode->flags & DRM_MODE_FLAG_INTERLACE )?1:0;

         s_DRMRuntimeState.objInfoConnector.uObjId = s_DRMRuntimeState.pConnector->connector_id;
         s_DRMRuntimeState.objInfoConnector.iObjIndex = i;
         s_DRMRuntimeState.targetModeInfo = s_DRMRuntimeState.pConnector->modes[0];

         memcpy(&s_DRMDisplayAttributes.currentMode, &s_DRMRuntimeState.pConnector->modes[0], sizeof(s_DRMDisplayAttributes.currentMode));
         
         drmModeCreatePropertyBlob(s_fdDRM, &s_DRMDisplayAttributes.currentMode, sizeof(s_DRMDisplayAttributes.currentMode), &s_DRMRuntimeState.uModeIdBlob);
      }

      if ( s_DRMRuntimeState.objInfoConnector.uObjId != 0xFFFFFFFF )
         break;
      drmModeFreeConnector(s_DRMRuntimeState.pConnector);
      s_DRMRuntimeState.pConnector = NULL;
   }

   // Find the actual display/CRTc for this connector (connector->encoder->crt/display)

   log_line("[DRMCore] (Enumerate find resources) Finding display...");

   if ( NULL == s_DRMRuntimeState.pConnector )
   {
      log_softerror_and_alarm("[DRMCore] No connected DRM connector with a usable mode");
      return -1;
   }

   s_DRMRuntimeState.pEncoder = NULL;
   s_DRMRuntimeState.pCRTc = NULL;
   s_DRMRuntimeState.objInfoCRTc.iObjIndex = -1;

   if ( s_DRMRuntimeState.pConnector->encoder_id )
      s_DRMRuntimeState.pEncoder = drmModeGetEncoder(s_fdDRM, s_DRMRuntimeState.pConnector->encoder_id);

   if ( s_DRMRuntimeState.pEncoder && (s_DRMRuntimeState.pEncoder->crtc_id > 0) )
   {
      s_DRMRuntimeState.pCRTc = drmModeGetCrtc(s_fdDRM, s_DRMRuntimeState.pEncoder->crtc_id);
      s_DRMRuntimeState.pOriginalCRTc = s_DRMRuntimeState.pCRTc;
      s_DRMRuntimeState.objInfoCRTc.uObjId = s_DRMRuntimeState.pEncoder->crtc_id;  
      s_DRMRuntimeState.objInfoCRTc.iObjIndex = -1;
      for (int i = 0; i < s_DRMRuntimeState.pAllDRMResources->count_crtcs; i++)
      {
         if ( s_DRMRuntimeState.pAllDRMResources->crtcs[i] == s_DRMRuntimeState.pEncoder->crtc_id )
         {
            s_DRMRuntimeState.objInfoCRTc.iObjIndex = i;
            break;
         }
      } 
      log_line("[DRMCore] Found CRTc for encoder. crtc id: %u, crt index: %d",
         s_DRMRuntimeState.pEncoder->crtc_id, s_DRMRuntimeState.objInfoCRTc.iObjIndex);
   }
   else
   {
      drmModeFreeEncoder(s_DRMRuntimeState.pEncoder);
      s_DRMRuntimeState.pEncoder = NULL;
      s_DRMRuntimeState.objInfoCRTc.iObjIndex = -1;
   }

   if ( (NULL == s_DRMRuntimeState.pEncoder) || (NULL == s_DRMRuntimeState.pCRTc) )
   {
      log_line("[DRMCore] Iterating displays on all encoders...");
      for (int i = 0; i < s_DRMRuntimeState.pConnector->count_encoders; i++)
      {
         s_DRMRuntimeState.pEncoder = drmModeGetEncoder(s_fdDRM, s_DRMRuntimeState.pConnector->encoders[i]);
         if ( ! s_DRMRuntimeState.pEncoder )
         {
            log_softerror_and_alarm("[DRMCore] Cannot retrieve encoder %d: %u (%d)",
                i, s_DRMRuntimeState.pConnector->encoders[i], errno);
            continue;
         }
         
         s_DRMRuntimeState.objInfoCRTc.uObjId = 0xFFFFFFFF;
         for (int j = 0; j < s_DRMRuntimeState.pAllDRMResources->count_crtcs; j++)
         {
            if ( !(s_DRMRuntimeState.pEncoder->possible_crtcs & (1 << j)) )
               continue;

            if ( s_DRMRuntimeState.pAllDRMResources->crtcs[j] > 0 )
            {
               s_DRMRuntimeState.pCRTc = drmModeGetCrtc(s_fdDRM, s_DRMRuntimeState.pAllDRMResources->crtcs[j]);
               s_DRMRuntimeState.pOriginalCRTc = s_DRMRuntimeState.pCRTc;
               s_DRMRuntimeState.objInfoCRTc.iObjIndex = j;
               s_DRMRuntimeState.objInfoCRTc.uObjId = s_DRMRuntimeState.pAllDRMResources->crtcs[j];
               log_line("[DRMCore] Found display for crtc id %u, crtc index %d, for encoder %u",
                  s_DRMRuntimeState.pAllDRMResources->crtcs[j], j, s_DRMRuntimeState.pConnector->encoders[i]);
               break;
            }
         }
         if ( s_DRMRuntimeState.objInfoCRTc.uObjId != 0xFFFFFFFF )
            break;
      }
   }

   if ( NULL == s_DRMRuntimeState.pEncoder )
   {
      log_softerror_and_alarm("[DRMCore] (Enumerate find resources) Could not find an encoder.");
      return -1;
   }
   if ( NULL == s_DRMRuntimeState.pCRTc )
   {
      log_softerror_and_alarm("[DRMCore] (Enumerate find resources) Could not find a display.");
      return -1;
   }
   log_line("[DRMCore] (Enumerate find resources) Completed.");
   return 0;
}

int _ruby_drm_find_target_plane()
{
   // -------------------------------------------------------------------
   // Finding planes supported by currently selected connector/display
   // Select the desired plane as the one to use

   log_line("[DRMCore] Finding planes supported by current display (target plane index is %d, target plane format is: %s)...",
      s_DRMRuntimeState.objInfoPlane.iObjIndex, _ruby_drm_fourcc_to_string(s_DRMRuntimeState.uPlaneFormat) );

   s_DRMRuntimeState.pPlanesResources = drmModeGetPlaneResources(s_fdDRM);
   if ( !s_DRMRuntimeState.pPlanesResources )
   {
      log_softerror_and_alarm("[DRMCore] drmModeGetPlaneResources failed: %s", strerror(errno));
      return -1;
   }

   log_line("[DRMCore] Total supported planes by current display/connector: %d", s_DRMRuntimeState.pPlanesResources->count_planes);
   char szBuff[1024];
   szBuff[0] = 0;
   for (int i = 0; i < s_DRMRuntimeState.pPlanesResources->count_planes; i++)
   {
      int plane_id = s_DRMRuntimeState.pPlanesResources->planes[i];
      char szTmp[16];
      if ( 0 == i )
         sprintf(szTmp, "%d", plane_id);
      else
         sprintf(szTmp, ", %d", plane_id);

      strcat(szBuff, szTmp);
   }
   log_line("[DRMCore] Planes Ids: [%s]", szBuff);

   s_DRMRuntimeState.objInfoPlane.uObjId = 0xFFFFFFFF;
   s_DRMRuntimeState.iPlaneFormatIndex = -1;

   for (int i = 0; i < s_DRMRuntimeState.pPlanesResources->count_planes; i++)
   {
      if ( s_DRMRuntimeState.objInfoPlane.iObjIndex != -1 )
      if ( s_DRMRuntimeState.objInfoPlane.iObjIndex != i )
         continue;

      s_DRMRuntimeState.pPlane = drmModeGetPlane(s_fdDRM, s_DRMRuntimeState.pPlanesResources->planes[i]);
      if (!s_DRMRuntimeState.pPlane)
      {
         log_softerror_and_alarm("[DRMCore] drmModeGetPlane(%u) failed: %s", s_DRMRuntimeState.pPlanesResources->planes[i], strerror(errno));
         continue;
      }

      if ( s_DRMRuntimeState.pPlane->possible_crtcs & (1 << s_DRMRuntimeState.objInfoCRTc.iObjIndex) )
      {
         log_line("[DRMCore] Plane index %d (plane id %u) supports %d formats on current crt/display",
            i, s_DRMRuntimeState.pPlanesResources->planes[i], s_DRMRuntimeState.pPlane->count_formats);
         for (int j=0; j<s_DRMRuntimeState.pPlane->count_formats; j++)
         {
            log_line("[DRMCore] Found plane-%d format %d: %s",
             i, j, _ruby_drm_fourcc_to_string(s_DRMRuntimeState.pPlane->formats[j]));
            uint32_t fmt = s_DRMRuntimeState.pPlane->formats[j];
            if ( fmt == s_DRMRuntimeState.uPlaneFormat ||
                 (s_DRMRuntimeState.uPlaneFormat == DRM_FORMAT_ARGB8888 && fmt == DRM_FORMAT_XRGB8888) )
            {
               if ( fmt != s_DRMRuntimeState.uPlaneFormat )
               {
                  log_line("[DRMCore] Using XR24 instead of AR24 on plane %d", i);
                  s_DRMRuntimeState.uPlaneFormat = fmt;
               }
               s_DRMRuntimeState.objInfoPlane.uObjId = s_DRMRuntimeState.pPlanesResources->planes[i];
               s_DRMRuntimeState.objInfoPlane.iObjIndex = i;
               s_DRMRuntimeState.iPlaneFormatIndex = j;
               break;
            }
         }
      }
      else
         log_line("[DRMCore] Skipping plane index %d as it's not supported by currently used crtc index %d", i, s_DRMRuntimeState.objInfoCRTc.iObjIndex);

      if ( s_DRMRuntimeState.objInfoPlane.uObjId != 0xFFFFFFFF )
         break;

      drmModeFreePlane(s_DRMRuntimeState.pPlane);
   } 

   if ( s_DRMRuntimeState.objInfoPlane.uObjId != 0xFFFFFFFF )
      log_line("[DRMCore] Found plane format for format %s: plane id %d, plane index %d",
         _ruby_drm_fourcc_to_string(s_DRMRuntimeState.uPlaneFormat),
         s_DRMRuntimeState.objInfoPlane.uObjId, s_DRMRuntimeState.objInfoPlane.iObjIndex);
   else
   {
      log_softerror_and_alarm("[DRMCore] Can't find suitable plane for current display/crt.");   
      return -1;
   }

   return 0;
}

static int _ruby_drm_get_plane_type(uint32_t uPlaneId)
{
   int iType = -1;
   drmModeObjectProperties* pProps = drmModeObjectGetProperties(s_fdDRM, uPlaneId, DRM_MODE_OBJECT_PLANE);
   if ( NULL == pProps )
      return -1;
   for( uint32_t i=0; i<pProps->count_props; i++ )
   {
      drmModePropertyRes* pp = drmModeGetProperty(s_fdDRM, pProps->props[i]);
      if ( NULL == pp )
         continue;
      if ( (NULL != pp->name) && (0 == strcmp(pp->name, "type")) )
      { iType = (int)pProps->prop_values[i]; drmModeFreeProperty(pp); break; }
      drmModeFreeProperty(pp);
   }
   drmModeFreeObjectProperties(pProps);
   return iType;
}

// x64 OSD-on-top layout: find an OVERLAY plane that supports ARGB8888 (per-pixel alpha) on the
// current CRTC, so the OSD renders ABOVE the video (which then takes the primary plane). Returns 0
// and sets objInfoPlane on success.
int _ruby_drm_find_overlay_argb_plane()
{
   if ( NULL == s_DRMRuntimeState.pPlanesResources )
      s_DRMRuntimeState.pPlanesResources = drmModeGetPlaneResources(s_fdDRM);
   if ( NULL == s_DRMRuntimeState.pPlanesResources )
   { log_softerror_and_alarm("[DRMCore] no plane resources (overlay search)"); return -1; }

   s_DRMRuntimeState.objInfoPlane.uObjId = 0xFFFFFFFF;
   s_DRMRuntimeState.iPlaneFormatIndex = -1;
   for( int i=0; i<s_DRMRuntimeState.pPlanesResources->count_planes; i++ )
   {
      uint32_t uPid = s_DRMRuntimeState.pPlanesResources->planes[i];
      drmModePlanePtr pPl = drmModeGetPlane(s_fdDRM, uPid);
      if ( NULL == pPl )
         continue;
      int bUsable = 0;
      if ( pPl->possible_crtcs & (1 << s_DRMRuntimeState.objInfoCRTc.iObjIndex) )
      if ( DRM_PLANE_TYPE_OVERLAY == _ruby_drm_get_plane_type(uPid) )
         for( int j=0; j<pPl->count_formats; j++ )
            if ( pPl->formats[j] == DRM_FORMAT_ARGB8888 ) { bUsable = 1; s_DRMRuntimeState.iPlaneFormatIndex = j; break; }
      drmModeFreePlane(pPl);
      if ( bUsable )
      {
         s_DRMRuntimeState.objInfoPlane.uObjId = uPid;
         s_DRMRuntimeState.objInfoPlane.iObjIndex = i;
         s_DRMRuntimeState.uPlaneFormat = DRM_FORMAT_ARGB8888;
         log_line("[DRMCore] OSD will use OVERLAY plane id %u (index %d), ARGB8888 (on top of the primary video plane).", uPid, i);
         return 0;
      }
   }
   log_softerror_and_alarm("[DRMCore] No ARGB-capable overlay plane found for OSD-on-top layout; will fall back to default plane.");
   return -1;
}

int _ruby_drm_create_drm_surface_buffer(type_drm_buffer* pOutputBufferInfo)
{
   if ( NULL == pOutputBufferInfo )
      return -1;

   memset(pOutputBufferInfo, 0, sizeof(type_drm_buffer));
   struct drm_mode_create_dumb creq;
   struct drm_mode_destroy_dumb dreq;
   struct drm_mode_map_dumb mreq;
 
   int iRet = 0;
   uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};

   memset(&creq, 0, sizeof(creq));
   creq.width = s_DRMDisplayAttributes.iWidth;
   creq.height = s_DRMDisplayAttributes.iHeight;
   creq.bpp = s_DRMDisplayAttributes.iBPP;
   iRet = drmIoctl(s_fdDRM, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
   if ( iRet < 0 )
   {
      log_softerror_and_alarm("[DRMCore] Cannot create buffer (%d)", errno);
      return -errno;
   }
   pOutputBufferInfo->uWidth = creq.width;
   pOutputBufferInfo->uHeight = creq.height;
   pOutputBufferInfo->uStride = creq.pitch;
   pOutputBufferInfo->uSize = creq.size;
   pOutputBufferInfo->uHandle = creq.handle;

   handles[0] = pOutputBufferInfo->uHandle;
   pitches[0] = pOutputBufferInfo->uStride;

   iRet = drmModeAddFB2(s_fdDRM, s_DRMDisplayAttributes.iWidth, s_DRMDisplayAttributes.iHeight, s_DRMRuntimeState.uPlaneFormat,
       handles, pitches, offsets, &(pOutputBufferInfo->uBufferId), 0);
   if ( iRet )
   {
      log_softerror_and_alarm("[DRMCore] Cannot create framebuffer (%d)", errno);
      memset(&dreq, 0, sizeof(dreq));
      dreq.handle = pOutputBufferInfo->uHandle;
      drmIoctl(s_fdDRM, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
      return -1;
   }

   memset(&mreq, 0, sizeof(mreq));
   mreq.handle = pOutputBufferInfo->uHandle;
   iRet = drmIoctl(s_fdDRM, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
   if ( iRet )
   {
      log_softerror_and_alarm("[DRMCore] Cannot map buffer (%d)", errno);
      drmModeRmFB(s_fdDRM, pOutputBufferInfo->uBufferId); 
      memset(&dreq, 0, sizeof(dreq));
      dreq.handle = pOutputBufferInfo->uHandle;
      drmIoctl(s_fdDRM, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
      return -1;
   }

   pOutputBufferInfo->pData = mmap(0, pOutputBufferInfo->uSize, PROT_READ | PROT_WRITE, MAP_SHARED,
          s_fdDRM, mreq.offset);
   if ( pOutputBufferInfo->pData == MAP_FAILED )
   {
      log_softerror_and_alarm("[DRMCore] Cannot mmap buffer (%d)", errno);
      drmModeRmFB(s_fdDRM, pOutputBufferInfo->uBufferId); 
      memset(&dreq, 0, sizeof(dreq));
      dreq.handle = pOutputBufferInfo->uHandle;
      drmIoctl(s_fdDRM, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
      return -1;
   }

   memset(pOutputBufferInfo->pData, 0, pOutputBufferInfo->uSize);
 
   log_line("[DRMCore] Created new surface buffer, size: %d, w/h: %d/%d, stride: %d, handle: %u, buffer id: %u",
      pOutputBufferInfo->uSize, pOutputBufferInfo->uWidth, pOutputBufferInfo->uHeight,
      pOutputBufferInfo->uStride, pOutputBufferInfo->uHandle, pOutputBufferInfo->uBufferId);
   return 0;
}

int _ruby_drm_destroy_drm_surface_buffer(type_drm_buffer* pBuffer)
{
   if ( NULL == pBuffer )
      return -1;
   if ( (0 == pBuffer->pData) || (0 == pBuffer->uBufferId) )
      return -1;

   log_line("[DRMCore] Destroy frame buffer id %u, handle %u ...", pBuffer->uBufferId, pBuffer->uHandle);

   munmap(pBuffer->pData, pBuffer->uSize);
   drmModeRmFB(s_fdDRM, pBuffer->uBufferId);

   struct drm_mode_destroy_dumb dreq;
   memset(&dreq, 0, sizeof(dreq));
   dreq.handle = pBuffer->uHandle;
   drmIoctl(s_fdDRM, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
   return 0;
}


int _ruby_drm_set_mode()
{
   //if ( (0 == s_iDRMTargetPlaneIndex) || (-1 == s_iDRMTargetPlaneIndex) )
   /*
   {
      int iRet = drmModeSetCrtc(s_fdDRM, s_DRMRuntimeState.objInfoCRTc.uObjId, s_DRMRuntimeState.drawBuffers[s_DRMRuntimeState.iActiveOnScreenDrawBuffer].uBufferId, 0, 0,
         &s_DRMRuntimeState.objInfoConnector.uObjId, 1, &s_DRMRuntimeState.targetModeInfo);
      if ( iRet < 0 )
      {
         log_softerror_and_alarm("[DRMCore] Failed to set new mode.");
         return -1;
      }
      log_line("[DRMCore] Did set DRM mode for CRTc for plane index %d, plane id: %u",
          s_DRMRuntimeState.objInfoPlane.iObjIndex, s_DRMRuntimeState.objInfoPlane.uObjId);
   }
   */
   /*
   else
   {
      drmModeSetPlane( s_fdDRM, (u32)s_iDRMPlaneId, s_uDRMCurrentCrtcId,
                    s_DRMDrawBuffers[s_iDRMActiveOnScreenDrawBuffer].uBufferId,
                    0,
                    0, 0,
                    s_DRMDisplayAttributes.iWidth, s_DRMDisplayAttributes.iHeight,
                    0, 0,
                    ((uint16_t) s_DRMDisplayAttributes.iWidth) << 16, ((uint16_t) s_DRMDisplayAttributes.iHeight) << 16);
      log_line("[DRMCore] Did set plane %d for current CRTc.", s_iDRMTargetPlaneIndex );
   }
   */
   return 0;
}

int ruby_drm_core_is_display_connected()
{
   int iMustCloseDevice = 0;
   if ( s_fdDRM < 0 )
   {
      iMustCloseDevice = 1;
      if ( _ruby_drm_open_device() < 0 )
         return -1;
   }

   drmModeRes* pAllDRMResources = drmModeGetResources(s_fdDRM);
   if ( !pAllDRMResources )
   {
      log_softerror_and_alarm("[DRMCore] Cannot retrieve DRM resources (%d)", errno);
      return -errno;
   }
 
   if ( pAllDRMResources->count_connectors <= 0 )
   {
      log_softerror_and_alarm("[DRMCore] No connectors available (%d)", errno);
      return -1;
   }
 
   log_line("[DRMCore] (Check display connected) Finding resources (%d connectors, %d crtcs)...",
      pAllDRMResources->count_connectors, pAllDRMResources->count_crtcs);

   // Find connectors (displays, aka video hardware connectors (HDMI,DVI...))

   drmModeConnector* pConnector = NULL;

   for (int i = 0; i < pAllDRMResources->count_connectors; i++)
   {
      pConnector = drmModeGetConnector(s_fdDRM, pAllDRMResources->connectors[i]);
      if (!pConnector)
      {
         log_softerror_and_alarm("[DRMCore] Cannot retrieve DRM connector %u:%u (%d)",
              i, pAllDRMResources->connectors[i], errno);
         continue;
      }

      if ( pConnector->count_modes <= 0 )
      {
         drmModeFreeConnector(pConnector);
         continue;
      }
      if ( pConnector->connection == DRM_MODE_CONNECTED )
      {
         drmModeFreeConnector(pConnector);
         drmModeFreeResources(pAllDRMResources);
         if ( iMustCloseDevice )
         {
            close(s_fdDRM);
            s_fdDRM = -1;
         }
         return 1;
      }
      
      drmModeFreeConnector(pConnector);
      pConnector = NULL;
   }

   drmModeFreeResources(pAllDRMResources);

   if ( iMustCloseDevice )
   {
      close(s_fdDRM);
      s_fdDRM = -1;
   }
   return 0;
}

int ruby_drm_core_wait_for_display_connected()
{
#if defined(HW_PLATFORM_X64)
   if ( (NULL == getenv("RUBY_FORCE_DRM")) && ((NULL != getenv("DISPLAY")) || (NULL != getenv("WAYLAND_DISPLAY"))) )
   {
      log_line("[DRMCore] Graphical session detected; windowed output -> no DRM display wait.");
      return 1;
   }
#endif
   if ( getenv("RUBY_GS_HEADLESS") )
   {
      log_line("[DRMCore] RUBY_GS_HEADLESS set; skip blocking display wait");
      return 1;
   }

   log_line("[DRMCore] Waiting for display to be conncted...");

   if ( ruby_drm_core_is_display_connected() )
   {
      log_line("[DRMCore] Display is connected.");
      return 1;
   }
   int iWait = 0;
   do
   {
      hardware_sleep_ms(400);
      iWait++;
      log_line("[DRMCore] Waiting again for display to be conncted...");
   } while (ruby_drm_core_is_display_connected() <= 0 && iWait < 30 );
   hardware_sleep_ms(100);
   if ( ruby_drm_core_is_display_connected() <= 0 )
      log_softerror_and_alarm("[DRMCore] Display not connected after wait; continuing for DRM probe");
   else
      log_line("[DRMCore] Display is connected.");
   return 1;
}

int ruby_drm_core_init(int iPlaneIndex, uint32_t uFormat, int iWidth, int iHeight, int iRefreshRate)
{
   log_line("[DRMCore] Init (on plane index %d, format %s, w/h/r: %dx%d@%d)...",
      iPlaneIndex, _ruby_drm_fourcc_to_string(uFormat), iWidth, iHeight, iRefreshRate);

#if defined(HW_PLATFORM_X64)
   // Auto-detect a graphical session: render into a window (X11 / XWayland) instead of taking DRM/KMS.
   // RUBY_FORCE_DRM=1 forces the kiosk DRM path even under a (possibly stray) DISPLAY.
   if ( (NULL == getenv("RUBY_FORCE_DRM")) && ((NULL != getenv("DISPLAY")) || (NULL != getenv("WAYLAND_DISPLAY"))) )
   {
      log_line("[DRMCore] Graphical session (DISPLAY/WAYLAND_DISPLAY) detected -> windowed output.");
      return _ruby_x11_init(iWidth, iHeight);
   }
#endif

   s_DRMDisplayAttributes.iWidth = iWidth;
   s_DRMDisplayAttributes.iHeight = iHeight;
   s_DRMDisplayAttributes.iRefreshRate = iRefreshRate;
   s_DRMDisplayAttributes.iInterleaved = 0;
   s_DRMDisplayAttributes.iBPP = 32;

   memset(&s_DRMRuntimeState, 0, sizeof(type_drm_runtime_state));
   s_DRMRuntimeState.uPlaneFormat = uFormat;
   s_DRMRuntimeState.objInfoPlane.iObjIndex = iPlaneIndex;
   s_DRMRuntimeState.objInfoPlane.uObjId = 0xFFFFFFFF;
   s_DRMRuntimeState.iPlaneFormatIndex = 0;
   s_DRMRuntimeState.objInfoConnector.uObjType = DRM_MODE_OBJECT_CONNECTOR;
   s_DRMRuntimeState.objInfoConnector.uObjId = 0xFFFFFFFF;
   s_DRMRuntimeState.objInfoCRTc.uObjType = DRM_MODE_OBJECT_CRTC;
   s_DRMRuntimeState.objInfoCRTc.uObjId = 0xFFFFFFFF;
   s_DRMRuntimeState.objInfoPlane.uObjType = DRM_MODE_OBJECT_PLANE;
   s_DRMRuntimeState.objInfoPlane.uObjId = 0xFFFFFFFF;

   s_DRMRuntimeState.iVideoSourceWidth = -1;
   s_DRMRuntimeState.iVideoSourceHeight = -1;

   if ( _ruby_drm_open_device() != 0 )
      return -1;

   if ( _ruby_drm_core_enumerate_find_resources() != 0 )
   {
      close(s_fdDRM);
      s_fdDRM = -1;
      return -1;
   }

   int iFindRet;
   s_bOsdOnOverlay = 0;
   if ( -2 == iPlaneIndex )
   {
      // x64 OSD-on-top: put the OSD on an ARGB overlay so video (primary) renders below it.
      iFindRet = _ruby_drm_find_overlay_argb_plane();
      if ( 0 == iFindRet )
         s_bOsdOnOverlay = 1;
      else
      {
         // No suitable overlay: fall back to the default plane selection (OSD on primary, as before).
         s_DRMRuntimeState.objInfoPlane.iObjIndex = 0;
         s_DRMRuntimeState.objInfoPlane.uObjId = 0xFFFFFFFF;
         s_DRMRuntimeState.uPlaneFormat = uFormat;
         iFindRet = _ruby_drm_find_target_plane();
      }
   }
   else
      iFindRet = _ruby_drm_find_target_plane();
   if ( 0 != iFindRet )
   {
      close(s_fdDRM);
      s_fdDRM = -1;
      return -1;
   }

   log_line("[DRMCore] Init: finding object properties for target plane %d", iPlaneIndex);

   _ruby_drm_get_object_properties(&s_DRMRuntimeState.objInfoConnector);
   _ruby_drm_get_object_properties(&s_DRMRuntimeState.objInfoCRTc);
   _ruby_drm_get_object_properties(&s_DRMRuntimeState.objInfoPlane);

#if defined(HW_PLATFORM_X64)
   // Console GPU present (EGL-on-GBM): render with GLES2 into a GBM scanout buffer + page-flip, instead of
   // the dumb-buffer software path. DEFAULT ON (proven); disable with RUBY_DRM_GL=0. Reuses the connector/
   // CRTC/mode found above, and falls back to the dumb-buffer path below if GBM/EGL init fails.
   const char* szDrmGL = getenv("RUBY_DRM_GL");
   if ( ((NULL == szDrmGL) || (szDrmGL[0] != '0')) &&
        (0 == _ruby_drm_gl_init(s_DRMRuntimeState.targetModeInfo.hdisplay, s_DRMRuntimeState.targetModeInfo.vdisplay)) )
   {
      s_iDRMCoreInitialized = 1;
      log_line("[DRMCore] Init complete (console EGL-on-GBM GL path, mode %dx%d).",
         s_DRMRuntimeState.targetModeInfo.hdisplay, s_DRMRuntimeState.targetModeInfo.vdisplay);
      return 0;
   }
#endif

   s_DRMRuntimeState.pAtomicRequest = drmModeAtomicAlloc();
   
   //int64_t iZPos = _ruby_drm_get_object_property_value(s_DRMRuntimeState.objInfoPlane.pProperties, "zpos");

   _ruby_drm_create_drm_surface_buffer(&s_DRMRuntimeState.drawBuffers[0]);
   _ruby_drm_create_drm_surface_buffer(&s_DRMRuntimeState.drawBuffers[1]);

   log_line("[DRMCore] Init: created drm surfaces target plane %d", iPlaneIndex);

   s_DRMRuntimeState.iActiveOnScreenDrawBuffer = 0;
   s_iDRMCoreInitialized = 1;

   log_line("[DRMCore] Init complete (on plane index %d, format %s, w/h/r: %dx%d@%d)",
      iPlaneIndex, _ruby_drm_fourcc_to_string(uFormat), iWidth, iHeight, iRefreshRate);
   return 0;
}

int ruby_drm_core_uninit()
{
   log_line("[DRMCore] Uninit");

#if defined(HW_PLATFORM_X64)
   if ( s_bWindowed )
   {
      _ruby_egl_uninit();   // tear down GL (no-op if the cairo path was used); must precede XDestroyWindow
      for( int i=0; i<2; i++ )
      {
         if ( NULL != s_pImgSurf[i] ) { cairo_surface_destroy(s_pImgSurf[i]); s_pImgSurf[i] = NULL; }
         if ( NULL != s_pWinBuf[i] )  { free(s_pWinBuf[i]); s_pWinBuf[i] = NULL; }
      }
      if ( NULL != s_pBackCtx )     { cairo_destroy(s_pBackCtx); s_pBackCtx = NULL; }
      if ( NULL != s_pBackSurface ) { cairo_surface_destroy(s_pBackSurface); s_pBackSurface = NULL; }
      if ( 0 != s_BackPixmap )      { XFreePixmap(s_pXDisplay, s_BackPixmap); s_BackPixmap = 0; }
      if ( NULL != s_XGC )          { XFreeGC(s_pXDisplay, s_XGC); s_XGC = NULL; }
      if ( 0 != s_XWindow ) { XDestroyWindow(s_pXDisplay, s_XWindow); s_XWindow = 0; }
      if ( NULL != s_pXDisplay ) { XCloseDisplay(s_pXDisplay); s_pXDisplay = NULL; }
      s_bWindowed = 0;
      return 0;
   }
   if ( s_bDrmGL )
   {
      _ruby_drm_gl_uninit();
      s_fdDRM = -1;
      return 0;
   }
#endif

   int iRet = drmModeSetCrtc(s_fdDRM, s_DRMRuntimeState.pOriginalCRTc->crtc_id, s_DRMRuntimeState.pOriginalCRTc->buffer_id, s_DRMRuntimeState.pOriginalCRTc->x, s_DRMRuntimeState.pOriginalCRTc->y,
      &s_DRMRuntimeState.objInfoConnector.uObjId, 1, &s_DRMRuntimeState.pOriginalCRTc->mode);
   if ( iRet < 0 )
   {
      log_softerror_and_alarm("[DRMCore] Failed to restore old mode.");
   }

  // int64_t iZPos = _ruby_drm_get_object_property_value(s_DRMRuntimeState.objInfoPlane.pProperties, "zpos");
 /*
 ret = modeset_atomic_prepare_commit(fd, output_list, output_list->video_request, &output_list->video_plane, buf->fb, buf->width, buf->height, zpos);
 if (ret < 0) {
  fprintf(stderr, "prepare atomic commit failed for plane %d, %m\n", output_list->video_plane.id);
  return;
 }
 ret = drmModeAtomicCommit(fd, output_list->video_request, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
 if (ret < 0) 
  fprintf(stderr, "modeset atomic commit failed for plane %d, %m\n", output_list->video_plane.id);
*/
//////////

   _ruby_drm_free_object_properties(&s_DRMRuntimeState.objInfoPlane);
   _ruby_drm_free_object_properties(&s_DRMRuntimeState.objInfoCRTc);
   _ruby_drm_free_object_properties(&s_DRMRuntimeState.objInfoConnector);
   
   if ( NULL != s_DRMRuntimeState.pPlanesResources )
      drmModeFreePlaneResources(s_DRMRuntimeState.pPlanesResources);
   s_DRMRuntimeState.pPlanesResources = NULL;

   if ( NULL != s_DRMRuntimeState.pCRTc )
      drmModeFreeCrtc(s_DRMRuntimeState.pCRTc);
   s_DRMRuntimeState.pCRTc = NULL;

   if ( NULL != s_DRMRuntimeState.pEncoder )
      drmModeFreeEncoder(s_DRMRuntimeState.pEncoder);
   s_DRMRuntimeState.pEncoder = NULL;

   if ( NULL != s_DRMRuntimeState.pConnector )
      drmModeFreeConnector(s_DRMRuntimeState.pConnector);
   s_DRMRuntimeState.pConnector = NULL;

   if ( NULL != s_DRMRuntimeState.pAllDRMResources )
      drmModeFreeResources(s_DRMRuntimeState.pAllDRMResources);
   s_DRMRuntimeState.pAllDRMResources = NULL;

   _ruby_drm_destroy_drm_surface_buffer(&s_DRMRuntimeState.drawBuffers[0]);
   _ruby_drm_destroy_drm_surface_buffer(&s_DRMRuntimeState.drawBuffers[1]);

   if ( s_fdDRM >= 0 )
      close(s_fdDRM);
   s_fdDRM = -1;
   s_iDRMCoreInitialized = 0;
   return 0;
}

int ruby_drm_core_get_fd()
{
#if defined(HW_PLATFORM_X64)
   if ( s_bWindowed )
      return (NULL != s_pXDisplay) ? ConnectionNumber(s_pXDisplay) : -1;
#endif
   return s_fdDRM;
}

type_drm_display_attributes* ruby_drm_get_main_display_info()
{
   return &s_DRMDisplayAttributes;
}

type_drm_buffer* ruby_drm_core_get_main_draw_buffer()
{
   return &(s_DRMRuntimeState.drawBuffers[s_DRMRuntimeState.iActiveOnScreenDrawBuffer]);
}

type_drm_buffer* ruby_drm_core_get_back_draw_buffer()
{
   return &(s_DRMRuntimeState.drawBuffers[1-s_DRMRuntimeState.iActiveOnScreenDrawBuffer]);
}

uint32_t ruby_drm_core_get_main_draw_buffer_id()
{
   return s_DRMRuntimeState.drawBuffers[s_DRMRuntimeState.iActiveOnScreenDrawBuffer].uBufferId;
}
uint32_t ruby_drm_core_get_back_draw_buffer_id()
{
   return s_DRMRuntimeState.drawBuffers[1-s_DRMRuntimeState.iActiveOnScreenDrawBuffer].uBufferId;
}

int ruby_drm_swap_mainback_buffers()
{
#if defined(HW_PLATFORM_X64)
   if ( s_bWindowed )
   {
      s_DRMRuntimeState.iActiveOnScreenDrawBuffer = 1 - s_DRMRuntimeState.iActiveOnScreenDrawBuffer;
      _ruby_x11_present();
      return 0;
   }
   if ( s_bDrmGL )
   {
      s_DRMRuntimeState.iActiveOnScreenDrawBuffer = 1 - s_DRMRuntimeState.iActiveOnScreenDrawBuffer;
      _ruby_drm_gl_present();
      return 0;
   }
#endif
   s_DRMRuntimeState.iActiveOnScreenDrawBuffer = 1 - s_DRMRuntimeState.iActiveOnScreenDrawBuffer;

   drmModeAtomicSetCursor(s_DRMRuntimeState.pAtomicRequest, 0);

   ruby_drm_set_object_property(&s_DRMRuntimeState.objInfoPlane, "FB_ID", s_DRMRuntimeState.drawBuffers[s_DRMRuntimeState.iActiveOnScreenDrawBuffer].uBufferId );

   int iRet = drmModeAtomicCommit(s_fdDRM, s_DRMRuntimeState.pAtomicRequest, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
   return iRet;

   //if ( (0 == s_iDRMTargetPlaneIndex) || (-1 == s_iDRMTargetPlaneIndex) )
   {
      /*
      int iRet = drmModeSetCrtc(s_fdDRM, s_DRMRuntimeState.objInfoCRTc.uObjId, s_DRMRuntimeState.drawBuffers[s_DRMRuntimeState.iActiveOnScreenDrawBuffer].uBufferId, 0, 0,
         &s_DRMRuntimeState.objInfoConnector.uObjId, 1, &s_DRMRuntimeState.targetModeInfo);
      if ( iRet < 0 )
      {
         log_softerror_and_alarm("[DRMCore] Failed to set new mode.");
         return;
      }
      */
   }
   /*
   else
   {
      drmModeSetPlane( s_fdDRM, (u32)s_iDRMPlaneId, s_uDRMCurrentCrtcId,
                    s_DRMDrawBuffers[s_iDRMActiveOnScreenDrawBuffer].uBufferId,
                    0,
                    0, 0,
                    s_DRMDisplayAttributes.iWidth, s_DRMDisplayAttributes.iHeight,
                    0, 0,
                    ((uint16_t) s_DRMDisplayAttributes.iWidth) << 16, ((uint16_t) s_DRMDisplayAttributes.iHeight) << 16);
   }
   */
}


int ruby_drm_core_set_plane_properties_and_buffer(uint32_t uBufferId)
{
#if defined(HW_PLATFORM_X64)
   if ( s_bWindowed )
      return 0;
#endif
   uint64_t uSrcWidth = s_DRMDisplayAttributes.iWidth;
   uint64_t uSrcHeight = s_DRMDisplayAttributes.iHeight;

   uint64_t zPos = 2;
   uint64_t uCrtX = 0;
   uint64_t uCrtY = 0;
   uint64_t uCrtW = uSrcWidth;
   uint64_t uCrtH = uSrcHeight;
   if ( (s_DRMRuntimeState.objInfoPlane.iObjIndex == 0) || s_bOsdOnOverlay )
   {
      // OSD plane (primary on Radxa, or an overlay on x64) - full screen, on top.
      zPos = 4;
   }
   else
   {
      // Video plane
      zPos = 2;

      int iVideoWidth = s_DRMDisplayAttributes.iWidth;
      int iVideoHeight = s_DRMDisplayAttributes.iHeight;
      if ( s_DRMRuntimeState.iVideoSourceWidth > 0 )
         iVideoWidth = s_DRMRuntimeState.iVideoSourceWidth;
      if ( s_DRMRuntimeState.iVideoSourceHeight> 0 )
         iVideoHeight = s_DRMRuntimeState.iVideoSourceHeight;

      uSrcWidth = iVideoWidth;
      uSrcHeight = iVideoHeight;

      // Avoid off by +1/-1 rounding errors
      if ( (iVideoWidth == s_DRMDisplayAttributes.iWidth) && (iVideoHeight == s_DRMDisplayAttributes.iHeight) )
      {
         uCrtX = 0;
         uCrtY = 0;
         uCrtW = iVideoWidth;
         uCrtH = iVideoHeight;
         log_line("[DRMCore] Set video plane 1:1 scalling: srcW: %u, srcH: %u, crtX: %u, crtY: %u, crtW: %u, crtH: %u",
            uSrcWidth, uSrcHeight, uCrtX, uCrtY, uCrtW, uCrtH);
      }
      else
      {
         float fVideoAspectRatio = (float)iVideoWidth/(float)iVideoHeight;
         float fDisplayAspectRatio = (float)s_DRMDisplayAttributes.iWidth/(float)s_DRMDisplayAttributes.iHeight;
         log_line("[DRMCore] Video plane has scalling. screenW: %d, screenH: %d, videoW: %d, videoH: %d",
            s_DRMDisplayAttributes.iWidth, s_DRMDisplayAttributes.iHeight,
            iVideoWidth, iVideoHeight);
         log_line("[DRMCore] Video plane has scalling. video aspect ratio: %f, display aspect ratio: %f",
            fVideoAspectRatio, fDisplayAspectRatio);
         if ( fVideoAspectRatio >= fDisplayAspectRatio )
         {
            uCrtX = 0;
            uCrtW = s_DRMDisplayAttributes.iWidth;
            uCrtH = iVideoHeight * (float) s_DRMDisplayAttributes.iWidth / (float) iVideoWidth;
            uCrtY = (s_DRMDisplayAttributes.iHeight - uCrtH)/2;
         }
         else
         {
            uCrtY = 0;
            uCrtH = s_DRMDisplayAttributes.iHeight;
            uCrtW = iVideoWidth * (float) s_DRMDisplayAttributes.iHeight / (float) iVideoHeight;
            uCrtX = (s_DRMDisplayAttributes.iWidth - uCrtW)/2;
         }
         log_line("[DRMCore] Set video plane custom scalling: srcW: %u, srcH: %u, crtX: %u, crtY: %u, crtW: %u, crtH: %u",
            uSrcWidth, uSrcHeight, uCrtX, uCrtY, uCrtW, uCrtH);

      }
      uSrcWidth = iVideoWidth;
      uSrcHeight = iVideoHeight;
   }
   log_line("[DRMCore] Setting current plane (id: %u, plane index %d) buffer id to %u, zindex %d",
      s_DRMRuntimeState.objInfoPlane.uObjId, s_DRMRuntimeState.objInfoPlane.iObjIndex, uBufferId, (int)zPos);

   ruby_drm_set_object_property(&s_DRMRuntimeState.objInfoConnector, "CRTC_ID", s_DRMRuntimeState.objInfoCRTc.uObjId );
   ruby_drm_set_object_property(&s_DRMRuntimeState.objInfoCRTc, "MODE_ID", s_DRMRuntimeState.uModeIdBlob );
   ruby_drm_set_object_property(&s_DRMRuntimeState.objInfoCRTc, "ACTIVE", 1 );
   ruby_drm_set_object_property(&s_DRMRuntimeState.objInfoPlane, "FB_ID", uBufferId );

   ruby_drm_set_object_property(&s_DRMRuntimeState.objInfoPlane, "CRTC_ID", s_DRMRuntimeState.objInfoCRTc.uObjId );
   ruby_drm_set_object_property(&s_DRMRuntimeState.objInfoPlane, "CRTC_X", uCrtX );
   ruby_drm_set_object_property(&s_DRMRuntimeState.objInfoPlane, "CRTC_Y", uCrtY );
   ruby_drm_set_object_property(&s_DRMRuntimeState.objInfoPlane, "CRTC_W", uCrtW );
   ruby_drm_set_object_property(&s_DRMRuntimeState.objInfoPlane, "CRTC_H", uCrtH );
   
   ruby_drm_set_object_property(&s_DRMRuntimeState.objInfoPlane, "SRC_X", 0 );
   ruby_drm_set_object_property(&s_DRMRuntimeState.objInfoPlane, "SRC_Y", 0 );
   ruby_drm_set_object_property(&s_DRMRuntimeState.objInfoPlane, "SRC_W", uSrcWidth<<16 );
   ruby_drm_set_object_property(&s_DRMRuntimeState.objInfoPlane, "SRC_H", uSrcHeight<<16 );

   // On x64 the OSD overlay's zpos is immutable (it is already above the primary video plane);
   // setting it would return EINVAL and fail the whole commit. Only set zpos when it is settable.
   if ( ! s_bOsdOnOverlay )
      ruby_drm_set_object_property(&s_DRMRuntimeState.objInfoPlane, "zpos", zPos );

   int iRet = drmModeAtomicCommit(s_fdDRM, s_DRMRuntimeState.pAtomicRequest, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

   log_line("[DRMCore] Done setting current plane (id: %u, index %d) buffer id to %u, zindex %d",
      s_DRMRuntimeState.objInfoPlane.uObjId, s_DRMRuntimeState.objInfoPlane.iObjIndex, uBufferId, (int)zPos);
   return iRet;
}

int ruby_drm_core_set_plane_buffer(uint32_t uBufferId)
{
#if defined(HW_PLATFORM_X64)
   if ( s_bWindowed )
      return 0;
#endif
   drmModeAtomicSetCursor(s_DRMRuntimeState.pAtomicRequest, 0);

   ruby_drm_set_object_property(&s_DRMRuntimeState.objInfoPlane, "FB_ID", uBufferId );
   int iRet = drmModeAtomicCommit(s_fdDRM, s_DRMRuntimeState.pAtomicRequest, s_iDRMEnableVSync?DRM_MODE_ATOMIC_ALLOW_MODESET:DRM_MODE_ATOMIC_NONBLOCK, NULL);
   return iRet;
}


type_drm_object_info* ruby_drm_get_plane_info()
{
  return &s_DRMRuntimeState.objInfoPlane;
}

int ruby_drm_set_object_property(type_drm_object_info* pObject, const char *szName, uint64_t uValue)
{
   if ( (NULL == pObject) || (NULL == szName) )
      return -1;
   uint32_t uPropId = 0;
   int iPropIndex = -1;
   for (int i = 0; i < pObject->pProperties->count_props; i++)
   {
      if ( 0 == strcmp(pObject->ppPropertiesInfo[i]->name, szName) )
      {
         uPropId = pObject->ppPropertiesInfo[i]->prop_id;
         iPropIndex = i;
         break;
      }
   }

   if ( (0 == uPropId) || (-1 == iPropIndex) )
   {
      log_softerror_and_alarm("[DRMCore] Can't set object property. Object id %u has no property named %s", pObject->uObjId, szName);
      return -EINVAL;
   }
 
   if ( 0 != strcmp(szName, "FB_ID") )
      log_line("[DRMCore] Set object id %u property %s (prop index %d) value to: %u",
          pObject->uObjId, szName, iPropIndex, (u32)uValue);
 
   return drmModeAtomicAddProperty(s_DRMRuntimeState.pAtomicRequest, pObject->uObjId, uPropId, uValue);
}

void ruby_drm_set_video_source_size(int iWidth, int iHeight)
{
   s_DRMRuntimeState.iVideoSourceWidth = iWidth;
   s_DRMRuntimeState.iVideoSourceHeight = iHeight;
}

void ruby_drm_enable_vsync(int iEnableVSync)
{
   s_iDRMEnableVSync = iEnableVSync;
}

// ====================================================================================
// x64 video plane: an ADDITIVE second DRM plane shown BELOW the OSD plane, fed with
// decoded frames (BGRx / XRGB8888) by ruby_central from the player's shared memory.
// It reuses the already-set-up master fd, connector and CRTC, but uses its own plane
// object, its own dumb buffers and its own atomic requests, so it never disturbs the
// OSD plane state. If any step fails it returns < 0 and the OSD keeps working.
// ====================================================================================
static type_drm_object_info s_VideoPlaneObj;
static type_drm_buffer s_VideoPlaneBuffers[2];
static int s_iVideoPlaneActiveBuffer = 0;
static int s_bVideoPlaneReady = 0;
static int s_iVideoPlaneW = 0;
static int s_iVideoPlaneH = 0;
static int s_iVideoSetZpos = 0;        // 1 if the video plane has a mutable zpos we should set
static uint64_t s_uVideoZpos = 0;      // zpos value to put the video plane below the OSD plane
static int s_bVideoPlaneNoScale = 0;   // set if hardware plane scaling is rejected -> use 1:1 centered

static int _ruby_drm_video_create_xrgb_buffer(type_drm_buffer* pBuf, int iW, int iH)
{
   memset(pBuf, 0, sizeof(type_drm_buffer));
   struct drm_mode_create_dumb creq;
   struct drm_mode_destroy_dumb dreq;
   struct drm_mode_map_dumb mreq;
   uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};

   memset(&creq, 0, sizeof(creq));
   creq.width = iW; creq.height = iH; creq.bpp = 32;
   if ( drmIoctl(s_fdDRM, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0 )
   { log_softerror_and_alarm("[DRMVideo] create dumb buffer failed (%d)", errno); return -1; }
   pBuf->uWidth = creq.width; pBuf->uHeight = creq.height; pBuf->uStride = creq.pitch;
   pBuf->uSize = creq.size; pBuf->uHandle = creq.handle;
   handles[0] = creq.handle; pitches[0] = creq.pitch;
   if ( drmModeAddFB2(s_fdDRM, iW, iH, DRM_FORMAT_XRGB8888, handles, pitches, offsets, &pBuf->uBufferId, 0) )
   {
      log_softerror_and_alarm("[DRMVideo] addFB2 failed (%d)", errno);
      memset(&dreq, 0, sizeof(dreq)); dreq.handle = creq.handle; drmIoctl(s_fdDRM, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
      return -1;
   }
   memset(&mreq, 0, sizeof(mreq)); mreq.handle = creq.handle;
   if ( drmIoctl(s_fdDRM, DRM_IOCTL_MODE_MAP_DUMB, &mreq) )
   {
      log_softerror_and_alarm("[DRMVideo] map dumb failed (%d)", errno);
      drmModeRmFB(s_fdDRM, pBuf->uBufferId);
      memset(&dreq, 0, sizeof(dreq)); dreq.handle = creq.handle; drmIoctl(s_fdDRM, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
      return -1;
   }
   pBuf->pData = (uint8_t*) mmap(0, pBuf->uSize, PROT_READ | PROT_WRITE, MAP_SHARED, s_fdDRM, mreq.offset);
   if ( pBuf->pData == MAP_FAILED )
   {
      log_softerror_and_alarm("[DRMVideo] mmap dumb failed (%d)", errno);
      drmModeRmFB(s_fdDRM, pBuf->uBufferId);
      memset(&dreq, 0, sizeof(dreq)); dreq.handle = creq.handle; drmIoctl(s_fdDRM, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
      pBuf->pData = NULL; return -1;
   }
   memset(pBuf->pData, 0, pBuf->uSize);
   return 0;
}

void ruby_drm_video_plane_uninit()
{
   s_bVideoPlaneReady = 0;
   for( int i=0; i<2; i++ )
   {
      if ( NULL != s_VideoPlaneBuffers[i].pData )
      {
         munmap(s_VideoPlaneBuffers[i].pData, s_VideoPlaneBuffers[i].uSize);
         drmModeRmFB(s_fdDRM, s_VideoPlaneBuffers[i].uBufferId);
         struct drm_mode_destroy_dumb dreq; memset(&dreq, 0, sizeof(dreq));
         dreq.handle = s_VideoPlaneBuffers[i].uHandle;
         drmIoctl(s_fdDRM, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
      }
      memset(&s_VideoPlaneBuffers[i], 0, sizeof(type_drm_buffer));
   }
   if ( NULL != s_VideoPlaneObj.pProperties )
   {
      for( int i=0; i<s_VideoPlaneObj.pProperties->count_props; i++ )
         if ( NULL != s_VideoPlaneObj.ppPropertiesInfo[i] ) drmModeFreeProperty(s_VideoPlaneObj.ppPropertiesInfo[i]);
      if ( NULL != s_VideoPlaneObj.ppPropertiesInfo ) free(s_VideoPlaneObj.ppPropertiesInfo);
      drmModeFreeObjectProperties(s_VideoPlaneObj.pProperties);
   }
   memset(&s_VideoPlaneObj, 0, sizeof(s_VideoPlaneObj));
   s_iVideoPlaneW = 0; s_iVideoPlaneH = 0; s_iVideoPlaneActiveBuffer = 0;
}

int ruby_drm_video_plane_init(int iSrcW, int iSrcH)
{
   if ( (s_fdDRM < 0) || (iSrcW <= 0) || (iSrcH <= 0) )
      return -1;
   if ( s_bVideoPlaneReady && (iSrcW == s_iVideoPlaneW) && (iSrcH == s_iVideoPlaneH) )
      return 0;
   ruby_drm_video_plane_uninit();

   if ( NULL == s_DRMRuntimeState.pPlanesResources )
   {
      s_DRMRuntimeState.pPlanesResources = drmModeGetPlaneResources(s_fdDRM);
      if ( NULL == s_DRMRuntimeState.pPlanesResources )
      { log_softerror_and_alarm("[DRMVideo] no plane resources"); return -1; }
   }

   memset(&s_VideoPlaneObj, 0, sizeof(s_VideoPlaneObj));
   s_VideoPlaneObj.uObjType = DRM_MODE_OBJECT_PLANE;
   s_VideoPlaneObj.uObjId = 0xFFFFFFFF;
   for( int i=0; i<s_DRMRuntimeState.pPlanesResources->count_planes; i++ )
   {
      uint32_t uPid = s_DRMRuntimeState.pPlanesResources->planes[i];
      if ( uPid == s_DRMRuntimeState.objInfoPlane.uObjId )   // never steal the OSD plane
         continue;
      drmModePlanePtr pPl = drmModeGetPlane(s_fdDRM, uPid);
      if ( NULL == pPl )
         continue;
      int bOk = 0;
      if ( pPl->possible_crtcs & (1 << s_DRMRuntimeState.objInfoCRTc.iObjIndex) )
         for( int j=0; j<pPl->count_formats; j++ )
            if ( pPl->formats[j] == DRM_FORMAT_XRGB8888 ) { bOk = 1; break; }
      drmModeFreePlane(pPl);
      if ( bOk ) { s_VideoPlaneObj.uObjId = uPid; s_VideoPlaneObj.iObjIndex = i; break; }
   }
   if ( s_VideoPlaneObj.uObjId == 0xFFFFFFFF )
   { log_softerror_and_alarm("[DRMVideo] no XRGB-capable plane available for video (OSD plane id %u)", s_DRMRuntimeState.objInfoPlane.uObjId); return -1; }

   if ( 0 != _ruby_drm_get_object_properties(&s_VideoPlaneObj) )
   { log_softerror_and_alarm("[DRMVideo] failed to load video plane properties"); return -1; }

   // Determine zpos: the video plane must render BELOW the OSD plane (which uses zpos 4). If this
   // plane's zpos is mutable, set it to its minimum (bottom). If immutable, we can't move it here.
   s_iVideoSetZpos = 0; s_uVideoZpos = 0;
   for( int i=0; i<s_VideoPlaneObj.pProperties->count_props; i++ )
   {
      drmModePropertyRes* pP = s_VideoPlaneObj.ppPropertiesInfo[i];
      if ( (NULL == pP) || (NULL == pP->name) || (0 != strcmp(pP->name, "zpos")) )
         continue;
      uint64_t uCur = s_VideoPlaneObj.pProperties->prop_values[i];
      int bImmutable = (pP->flags & DRM_MODE_PROP_IMMUTABLE) ? 1 : 0;
      uint64_t uMin = uCur, uMax = uCur;
      if ( (pP->flags & DRM_MODE_PROP_RANGE) && (pP->count_values >= 2) ) { uMin = pP->values[0]; uMax = pP->values[1]; }
      log_line("[DRMVideo] video plane zpos: current=%llu min=%llu max=%llu immutable=%d (OSD plane uses zpos 4)",
         (unsigned long long)uCur, (unsigned long long)uMin, (unsigned long long)uMax, bImmutable);
      if ( ! bImmutable )
      {
         s_iVideoSetZpos = 1;
         s_uVideoZpos = uMin;   // bottom-most -> below the OSD overlay
      }
      break;
   }
   // Log the OSD plane's type + zpos so we know the real layering (video must end up below it).
   {
      drmModeObjectProperties* pOsdProps = drmModeObjectGetProperties(s_fdDRM, s_DRMRuntimeState.objInfoPlane.uObjId, DRM_MODE_OBJECT_PLANE);
      if ( NULL != pOsdProps )
      {
         for( uint32_t k=0; k<pOsdProps->count_props; k++ )
         {
            drmModePropertyRes* pp = drmModeGetProperty(s_fdDRM, pOsdProps->props[k]);
            if ( NULL == pp ) continue;
            if ( (0 == strcmp(pp->name, "zpos")) || (0 == strcmp(pp->name, "type")) )
               log_line("[DRMVideo] OSD plane id %u prop %s = %llu (immutable=%d)",
                  s_DRMRuntimeState.objInfoPlane.uObjId, pp->name,
                  (unsigned long long)pOsdProps->prop_values[k], (pp->flags & DRM_MODE_PROP_IMMUTABLE)?1:0);
            drmModeFreeProperty(pp);
         }
         drmModeFreeObjectProperties(pOsdProps);
      }
      // Also log the video plane's type for context.
      for( int k=0; k<s_VideoPlaneObj.pProperties->count_props; k++ )
      {
         drmModePropertyRes* pp = s_VideoPlaneObj.ppPropertiesInfo[k];
         if ( pp && pp->name && (0 == strcmp(pp->name, "type")) )
            log_line("[DRMVideo] video plane id %u type = %llu", s_VideoPlaneObj.uObjId, (unsigned long long)s_VideoPlaneObj.pProperties->prop_values[k]);
      }
   }

   if ( (0 != _ruby_drm_video_create_xrgb_buffer(&s_VideoPlaneBuffers[0], iSrcW, iSrcH)) ||
        (0 != _ruby_drm_video_create_xrgb_buffer(&s_VideoPlaneBuffers[1], iSrcW, iSrcH)) )
   { ruby_drm_video_plane_uninit(); return -1; }

   s_iVideoPlaneW = iSrcW; s_iVideoPlaneH = iSrcH; s_iVideoPlaneActiveBuffer = 0; s_bVideoPlaneReady = 1;
   log_line("[DRMVideo] Video plane ready: plane id %u (index %d), %dx%d XRGB, zpos 2 under OSD plane id %u (zpos 4)",
      s_VideoPlaneObj.uObjId, s_VideoPlaneObj.iObjIndex, iSrcW, iSrcH, s_DRMRuntimeState.objInfoPlane.uObjId);
   return 0;
}

static int _ruby_drm_video_atomic_prop(drmModeAtomicReq* req, const char* szName, uint64_t uValue)
{
   for( int i=0; i<s_VideoPlaneObj.pProperties->count_props; i++ )
      if ( (NULL != s_VideoPlaneObj.ppPropertiesInfo[i]) && (0 == strcmp(s_VideoPlaneObj.ppPropertiesInfo[i]->name, szName)) )
         return drmModeAtomicAddProperty(req, s_VideoPlaneObj.uObjId, s_VideoPlaneObj.ppPropertiesInfo[i]->prop_id, uValue);
   return -1;
}

// Copy a decoded BGRx/XRGB frame to the back buffer and show it on the video plane,
// centered (no scaling - safe on Gen6 sprite planes), below the OSD.
int ruby_drm_video_plane_present(const uint8_t* pData, int iW, int iH, int iStride)
{
   if ( (NULL == pData) || (iW <= 0) || (iH <= 0) )
      return -1;
   if ( (!s_bVideoPlaneReady) || (iW != s_iVideoPlaneW) || (iH != s_iVideoPlaneH) )
   {
      if ( 0 != ruby_drm_video_plane_init(iW, iH) )
         return -1;
   }

   type_drm_buffer* pBuf = &s_VideoPlaneBuffers[s_iVideoPlaneActiveBuffer];
   int iCopyBytes = iW * 4;
   if ( iCopyBytes > (int)pBuf->uStride ) iCopyBytes = (int)pBuf->uStride;
   if ( iCopyBytes > iStride ) iCopyBytes = iStride;
   for( int y=0; y<iH; y++ )
      memcpy(pBuf->pData + (uint32_t)y * pBuf->uStride, pData + (uint32_t)y * iStride, iCopyBytes);

   int dispW = s_DRMDisplayAttributes.iWidth, dispH = s_DRMDisplayAttributes.iHeight;
   int cw, ch, cx, cy;
   if ( s_bVideoPlaneNoScale )
   {
      // Fallback: no hardware scaling on this plane -> center the frame at native size.
      cw = (iW < dispW) ? iW : dispW;
      ch = (iH < dispH) ? iH : dispH;
   }
   else
   {
      // Scale to fill the display, preserving aspect ratio (letterbox/pillarbox as needed).
      float fVid = (float)iW / (float)iH;
      float fDisp = (float)dispW / (float)dispH;
      if ( fVid >= fDisp ) { cw = dispW; ch = (int)((float)iH * (float)dispW / (float)iW + 0.5f); }
      else                 { ch = dispH; cw = (int)((float)iW * (float)dispH / (float)iH + 0.5f); }
      if ( cw > dispW ) cw = dispW;
      if ( ch > dispH ) ch = dispH;
   }
   cx = (dispW - cw)/2; if ( cx < 0 ) cx = 0;
   cy = (dispH - ch)/2; if ( cy < 0 ) cy = 0;

   drmModeAtomicReq* req = drmModeAtomicAlloc();
   if ( NULL == req )
      return -1;
   _ruby_drm_video_atomic_prop(req, "FB_ID", pBuf->uBufferId);
   _ruby_drm_video_atomic_prop(req, "CRTC_ID", s_DRMRuntimeState.objInfoCRTc.uObjId);
   _ruby_drm_video_atomic_prop(req, "CRTC_X", (uint64_t)cx);
   _ruby_drm_video_atomic_prop(req, "CRTC_Y", (uint64_t)cy);
   _ruby_drm_video_atomic_prop(req, "CRTC_W", (uint64_t)cw);
   _ruby_drm_video_atomic_prop(req, "CRTC_H", (uint64_t)ch);
   _ruby_drm_video_atomic_prop(req, "SRC_X", 0);
   _ruby_drm_video_atomic_prop(req, "SRC_Y", 0);
   _ruby_drm_video_atomic_prop(req, "SRC_W", ((uint64_t)iW) << 16);   // source = full decoded frame
   _ruby_drm_video_atomic_prop(req, "SRC_H", ((uint64_t)iH) << 16);   // (plane scales SRC -> CRTC)
   // Put the video plane below the OSD plane (zpos 4). Only set zpos if it is mutable on this plane
   // (setting an immutable zpos returns EINVAL and kills the commit).
   if ( s_iVideoSetZpos )
      _ruby_drm_video_atomic_prop(req, "zpos", s_uVideoZpos);

   // ALLOW_MODESET so the first commit can attach the plane to the already-active CRTC.
   int iRet = drmModeAtomicCommit(s_fdDRM, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
   drmModeAtomicFree(req);
   if ( 0 != iRet )
   {
      if ( (!s_bVideoPlaneNoScale) && ((iRet == -EINVAL) || (errno == EINVAL)) )
      {
         // This plane likely cannot scale (common on older Intel sprite planes). Fall back to
         // native-size centered for subsequent frames instead of dropping video entirely.
         log_line("[DRMVideo] scaled commit rejected (%d) - falling back to native-size centered (no plane scaling).", iRet);
         s_bVideoPlaneNoScale = 1;
         return -1;
      }
      static int s_iVideoCommitErrLogged = 0;
      if ( s_iVideoCommitErrLogged < 5 )
      { log_softerror_and_alarm("[DRMVideo] atomic plane commit failed: %d (%s)", iRet, strerror(errno)); s_iVideoCommitErrLogged++; }
      return -1;
   }
   s_iVideoPlaneActiveBuffer = 1 - s_iVideoPlaneActiveBuffer;
   return 0;
}