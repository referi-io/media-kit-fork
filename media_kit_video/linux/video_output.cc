// This file is a part of media_kit
// (https://github.com/media-kit/media-kit).
//
// Copyright © 2021 & onwards, Hitesh Kumar Saini <saini123hitesh@gmail.com>.
// All rights reserved.
// Use of this source code is governed by MIT license that can be found in the
// LICENSE file.

#include "include/media_kit_video/video_output.h"
#include "include/media_kit_video/texture_gl.h"
#include "include/media_kit_video/texture_sw.h"

#include <epoxy/egl.h>
#include <epoxy/glx.h>
#include <gdk/gdkwayland.h>
#include <gdk/gdkx.h>

struct _VideoOutput {
  GObject parent_instance;
  TextureGL* texture_gl;
  EGLDisplay egl_display; /* EGL display for mpv rendering (shared with flutter). */
  EGLContext egl_context; /* Isolated EGL context (non-shared). */
  EGLSurface egl_surface; /* Place holder surface for activating egl context */
  guint8* pixel_buffer;
  TextureSW* texture_sw;
  GMutex mutex; /* Only used in S/W rendering. */
  mpv_handle* handle;
  mpv_render_context* render_context;
  gint64 width;
  gint64 height;
  VideoOutputConfiguration configuration;
  TextureUpdateCallback texture_update_callback;
  gpointer texture_update_callback_context;
  FlTextureRegistrar* texture_registrar;
  gboolean destroyed;
};

G_DEFINE_TYPE(VideoOutput, video_output, G_TYPE_OBJECT)

static void video_output_dispose(GObject* object) {
  VideoOutput* self = VIDEO_OUTPUT(object);
  self->destroyed = TRUE;
  
  // Make sure that no more callbacks are invoked from mpv.
  if (self->render_context) {
    mpv_render_context_set_update_callback(self->render_context, NULL, NULL);
  }

  // H/W
  if (self->texture_gl) {
    fl_texture_registrar_unregister_texture(self->texture_registrar,
                                            FL_TEXTURE(self->texture_gl));
    
    // Save Flutter's current context before cleanup
    EGLDisplay current_display = eglGetCurrentDisplay();
    EGLContext flutter_context = eglGetCurrentContext();
    EGLSurface flutter_draw_surface = eglGetCurrentSurface(EGL_DRAW);
    EGLSurface flutter_read_surface = eglGetCurrentSurface(EGL_READ);
    
    // Free mpv_render_context with our own isolated EGL context
    if (self->render_context != NULL) {
      if (self->egl_context != EGL_NO_CONTEXT) {
        eglMakeCurrent(self->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, self->egl_context);
      }
      mpv_render_context_free(self->render_context);
      self->render_context = NULL;
      
      // Restore Flutter's context
      if (flutter_context != EGL_NO_CONTEXT) {
        eglMakeCurrent(current_display, flutter_draw_surface, flutter_read_surface, flutter_context);
      }
    }
    
    // Clean up EGL resources
    if (self->egl_context != EGL_NO_CONTEXT) {
      eglDestroyContext(self->egl_display, self->egl_context);
      self->egl_context = EGL_NO_CONTEXT;
    }
    
    g_object_unref(self->texture_gl);
  }
  // S/W
  if (self->texture_sw) {
    fl_texture_registrar_unregister_texture(self->texture_registrar,
                                            FL_TEXTURE(self->texture_sw));
    g_free(self->pixel_buffer);
    g_object_unref(self->texture_sw);
    if (self->render_context != NULL) {
      mpv_render_context_free(self->render_context);
      self->render_context = NULL;
    }
  }
  
  g_mutex_clear(&self->mutex);
  G_OBJECT_CLASS(video_output_parent_class)->dispose(object);
}

static void video_output_class_init(VideoOutputClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = video_output_dispose;
}

static void video_output_init(VideoOutput* self) {
  self->texture_gl = NULL;
  self->egl_display = EGL_NO_DISPLAY;
  self->egl_context = EGL_NO_CONTEXT;
  self->egl_surface = EGL_NO_SURFACE;
  self->texture_sw = NULL;
  self->pixel_buffer = NULL;
  self->handle = NULL;
  self->render_context = NULL;
  self->width = 0;
  self->height = 0;
  self->configuration = VideoOutputConfiguration{};
  self->texture_update_callback = NULL;
  self->texture_update_callback_context = NULL;
  self->texture_registrar = NULL;
  self->destroyed = FALSE;
  g_mutex_init(&self->mutex);
}

VideoOutput* video_output_new(FlTextureRegistrar* texture_registrar,
                              FlView* view,
                              gint64 handle,
                              VideoOutputConfiguration configuration) {
  VideoOutput* self = VIDEO_OUTPUT(g_object_new(video_output_get_type(), NULL));
  self->texture_registrar = texture_registrar;
  self->handle = (mpv_handle*)handle;
  self->width = configuration.width;
  self->height = configuration.height;
  self->configuration = configuration;
#ifndef MPV_RENDER_API_TYPE_SW
  // MPV_RENDER_API_TYPE_SW must be available for S/W rendering.
  if (!self->configuration.enable_hardware_acceleration) {
    g_printerr("media_kit: VideoOutput: S/W rendering is not supported.\n");
  }
  self->configuration.enable_hardware_acceleration = TRUE;
#endif
  mpv_set_option_string(self->handle, "video-sync", "audio");
  // Causes frame drops with `pulse` audio output. (SlotSun/dart_simple_live#42)
  // mpv_set_option_string(self->handle, "video-timing-offset", "0");
  gboolean hardware_acceleration_supported = FALSE;
  if (self->configuration.enable_hardware_acceleration) {
    // Detect display server type
    GdkDisplay* gdk_dpy = gdk_display_get_default();
    gboolean is_x11 = GDK_IS_X11_DISPLAY(gdk_dpy);

    if (!is_x11) {
      // Wayland: Flutter's EGL context is safe to use from any thread.
      // Query Flutter's EGL config for shared rendering.
      EGLDisplay flutter_display = eglGetCurrentDisplay();
      EGLContext flutter_context = eglGetCurrentContext();
      EGLConfig config = NULL;

      if (flutter_display != EGL_NO_DISPLAY && flutter_context != EGL_NO_CONTEXT) {
        self->egl_display = flutter_display;
        eglBindAPI(EGL_OPENGL_ES_API);
        EGLint config_id = 0;
        if (eglQueryContext(self->egl_display, flutter_context, EGL_CONFIG_ID, &config_id)) {
          g_print("media_kit: VideoOutput: Flutter's EGL config ID: %d\n", config_id);
          EGLint num_configs = 0;
          EGLint config_attribs[] = { EGL_CONFIG_ID, config_id, EGL_NONE };
          if (eglChooseConfig(self->egl_display, config_attribs, &config, 1, &num_configs) && num_configs > 0) {
            g_print("media_kit: VideoOutput: Using Flutter's EGL config.\n");
          } else {
            g_printerr("media_kit: VideoOutput: Failed to get Flutter's EGL config by ID.\n");
            config = NULL;
          }
        } else {
          g_printerr("media_kit: VideoOutput: Failed to query Flutter's EGL config ID.\n");
        }
      }
    } else {
      // --- STANDALONE WINDOW PATH: X11 (always) ---
      // On X11, Flutter 3.38+ uses EGL on the raster thread but GLX on the
      // platform thread. ANY eglMakeCurrent on this thread corrupts Flutter's
      // rendering permanently. The EGL availability check is unreliable: after
      // a warm X11 restart (without full reboot), eglGetCurrentDisplay() may
      // return a stale/valid display from the previous session, causing us to
      // take the EGL path and corrupt rendering. Always use standalone mpv
      // window on X11 — it's the only proven-stable approach.
      g_print("media_kit: VideoOutput: Using standalone mpv window (X11 isolation mode).\n");

      // Configure mpv for standalone fullscreen window output.
      // Use both set_option (pre-init style) and set_property (runtime) to
      // maximize chances. Log results to diagnose which calls succeed.
      int rc;
      rc = mpv_set_option_string(self->handle, "vo", "gpu");
      g_print("media_kit: VideoOutput: set vo=gpu: %s\n", mpv_error_string(rc));
      rc = mpv_set_option_string(self->handle, "force-window", "yes");
      g_print("media_kit: VideoOutput: set force-window=yes: %s\n", mpv_error_string(rc));
      rc = mpv_set_property_string(self->handle, "fullscreen", "yes");
      g_print("media_kit: VideoOutput: set fullscreen=yes: %s\n", mpv_error_string(rc));
      rc = mpv_set_property_string(self->handle, "ontop", "yes");
      g_print("media_kit: VideoOutput: set ontop=yes: %s\n", mpv_error_string(rc));
      mpv_set_property_string(self->handle, "border", "no");
      mpv_set_property_string(self->handle, "osc", "no");
      mpv_set_property_string(self->handle, "osd-level", "0");
      mpv_set_property_string(self->handle, "input-default-bindings", "no");
      mpv_set_property_string(self->handle, "input-vo-keyboard", "no");
      mpv_set_property_string(self->handle, "cursor-autohide", "always");

      // NO render_context created — mpv uses its own GL context in its own
      // window. Video rendering never touches Flutter's GLX/EGL state.

      // Create a dummy texture so Flutter's Video widget mounts correctly.
      // The actual video is visible in mpv's own fullscreen window on top.
      self->texture_gl = texture_gl_new(self);
      fl_texture_registrar_register_texture(texture_registrar,
                                            FL_TEXTURE(self->texture_gl));
      hardware_acceleration_supported = TRUE;
    }
  }
#ifdef MPV_RENDER_API_TYPE_SW
  if (!hardware_acceleration_supported) {
    g_printerr("media_kit: VideoOutput: S/W rendering.\n");
    // H/W rendering failed. Fallback to S/W rendering.
    self->pixel_buffer = g_new0(guint8, SW_RENDERING_PIXEL_BUFFER_SIZE);
    self->texture_gl = NULL;
    self->texture_sw = texture_sw_new(self);
    if (fl_texture_registrar_register_texture(texture_registrar,
                                              FL_TEXTURE(self->texture_sw))) {
      mpv_render_param params[] = {
          {MPV_RENDER_PARAM_API_TYPE, (void*)MPV_RENDER_API_TYPE_SW},
          {MPV_RENDER_PARAM_INVALID, (void*)0},
      };
      if (mpv_render_context_create(&self->render_context, self->handle,
                                    params) == 0) {
        mpv_render_context_set_update_callback(
            self->render_context,
            [](void* data) {
              gdk_threads_add_idle(
                  [](gpointer data) -> gboolean {
                    VideoOutput* self = (VideoOutput*)data;
                    if (self->destroyed) {
                      return FALSE;
                    }
                    g_mutex_lock(&self->mutex);
                    gint64 width = video_output_get_width(self);
                    gint64 height = video_output_get_height(self);
                    if (width > 0 && height > 0) {
                      gint32 size[]{(gint32)width, (gint32)height};
                      gint32 pitch = 4 * (gint32)width;
                      mpv_render_param params[]{
                          {MPV_RENDER_PARAM_SW_SIZE, size},
                          {MPV_RENDER_PARAM_SW_FORMAT, (void*)"rgb0"},
                          {MPV_RENDER_PARAM_SW_STRIDE, &pitch},
                          {MPV_RENDER_PARAM_SW_POINTER, self->pixel_buffer},
                          {MPV_RENDER_PARAM_INVALID, (void*)0},
                      };
                      mpv_render_context_render(self->render_context, params);
                      fl_texture_registrar_mark_texture_frame_available(
                          self->texture_registrar,
                          FL_TEXTURE(self->texture_sw));
                    }
                    g_mutex_unlock(&self->mutex);
                    return FALSE;
                  },
                  data);
            },
            self);
      }
    }
  }
#endif
  return self;
}

void video_output_set_texture_update_callback(
    VideoOutput* self,
    TextureUpdateCallback texture_update_callback,
    gpointer texture_update_callback_context) {
  self->texture_update_callback = texture_update_callback;
  self->texture_update_callback_context = texture_update_callback_context;
  // Notify initial dimensions as (1, 1) if |width| & |height| are 0 i.e.
  // texture & video frame size is based on playing file's resolution. This
  // will make sure that `Texture` widget on Flutter's widget tree is actually
  // mounted & |fl_texture_registrar_mark_texture_frame_available| actually
  // invokes the |TextureGL| or |TextureSW| callbacks. Otherwise it will be a
  // never ending deadlock where no video frames are ever rendered.
  // Skip if deferred init hasn't created the texture yet
  if (self->texture_gl == NULL && self->texture_sw == NULL) {
    return;
  }
  gint64 texture_id = video_output_get_texture_id(self);
  if (self->width == 0 || self->height == 0) {
    self->texture_update_callback(texture_id, 1, 1,
                                  self->texture_update_callback_context);
  } else {
    self->texture_update_callback(texture_id, self->width, self->height,
                                  self->texture_update_callback_context);
  }
}

gboolean video_output_needs_deferred_init(VideoOutput* self) {
  return self->egl_context == EGL_NO_CONTEXT &&
         self->render_context == NULL &&
         self->configuration.enable_hardware_acceleration;
}

gboolean video_output_deferred_init(VideoOutput* self) {
  // Called from timer on the GTK main thread.
  // Check if Flutter's EGL context is available (it might not be on the
  // platform thread). If not, try to get the display from GDK X11.
  EGLDisplay flutter_display = eglGetCurrentDisplay();
  EGLContext flutter_context = eglGetCurrentContext();
  EGLSurface flutter_draw = EGL_NO_SURFACE;
  EGLSurface flutter_read = EGL_NO_SURFACE;
  EGLConfig config = NULL;

  if (flutter_display != EGL_NO_DISPLAY && flutter_context != EGL_NO_CONTEXT) {
    // Flutter's EGL context available — use it for config
    flutter_draw = eglGetCurrentSurface(EGL_DRAW);
    flutter_read = eglGetCurrentSurface(EGL_READ);
    self->egl_display = flutter_display;
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint config_id = 0;
    if (eglQueryContext(flutter_display, flutter_context, EGL_CONFIG_ID, &config_id)) {
      EGLint num_configs = 0;
      EGLint config_attribs[] = { EGL_CONFIG_ID, config_id, EGL_NONE };
      eglChooseConfig(flutter_display, config_attribs, &config, 1, &num_configs);
    }
    g_print("media_kit: VideoOutput: deferred_init: using Flutter EGL context.\n");
  } else {
    // No EGL context — get display from GDK X11 (same handle as Flutter)
    GdkDisplay* gdk_dpy = gdk_display_get_default();
    EGLNativeDisplayType native_display = EGL_DEFAULT_DISPLAY;
    if (GDK_IS_X11_DISPLAY(gdk_dpy)) {
      native_display = (EGLNativeDisplayType)gdk_x11_display_get_xdisplay(gdk_dpy);
    }
    self->egl_display = eglGetDisplay(native_display);
    if (self->egl_display == EGL_NO_DISPLAY) return FALSE;
    EGLint major, minor;
    if (!eglInitialize(self->egl_display, &major, &minor)) return FALSE;
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint fallback_attribs[] = {
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_NONE,
    };
    EGLint num_configs = 0;
    eglChooseConfig(self->egl_display, fallback_attribs, &config, 1, &num_configs);
    g_print("media_kit: VideoOutput: deferred_init: using X11 EGL display.\n");
  }

  if (self->egl_display == EGL_NO_DISPLAY || config == NULL) return FALSE;

  // Create isolated EGL context
  EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
  self->egl_context = eglCreateContext(self->egl_display, config, EGL_NO_CONTEXT, ctx_attribs);
  if (self->egl_context == EGL_NO_CONTEXT) {
    g_printerr("media_kit: VideoOutput: deferred_init: eglCreateContext failed (0x%x).\n", eglGetError());
    return FALSE;
  }

  // Need to clear GLX if active (platform thread on X11)
  Display* x_display = NULL;
  GLXContext saved_glx = NULL;
  GLXDrawable saved_drawable = None;
  GdkDisplay* gdk_dpy = gdk_display_get_default();
  if (GDK_IS_X11_DISPLAY(gdk_dpy)) {
    x_display = gdk_x11_display_get_xdisplay(gdk_dpy);
    saved_glx = glXGetCurrentContext();
    saved_drawable = glXGetCurrentDrawable();
    if (saved_glx != NULL) {
      glXMakeCurrent(x_display, None, NULL);
    }
  }

  if (!eglMakeCurrent(self->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, self->egl_context)) {
    g_printerr("media_kit: VideoOutput: deferred_init: eglMakeCurrent failed (0x%x).\n", eglGetError());
    if (saved_glx) glXMakeCurrent(x_display, saved_drawable, saved_glx);
    return FALSE;
  }

  // Create TextureGL and register
  self->texture_gl = texture_gl_new(self);
  if (!fl_texture_registrar_register_texture(self->texture_registrar, FL_TEXTURE(self->texture_gl))) {
    g_printerr("media_kit: VideoOutput: deferred_init: register_texture failed.\n");
    eglMakeCurrent(self->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (saved_glx) glXMakeCurrent(x_display, saved_drawable, saved_glx);
    return FALSE;
  }

  // Create mpv render context
  mpv_opengl_init_params gl_init_params{
      [](auto, auto name) { return (void*)eglGetProcAddress(name); }, NULL,
  };
  mpv_render_param params[] = {
      {MPV_RENDER_PARAM_API_TYPE, (void*)MPV_RENDER_API_TYPE_OPENGL},
      {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, (void*)&gl_init_params},
      {MPV_RENDER_PARAM_INVALID, (void*)0},
      {MPV_RENDER_PARAM_INVALID, (void*)0},
  };
  GdkDisplay* display = gdk_display_get_default();
  if (GDK_IS_WAYLAND_DISPLAY(display)) {
    params[2].type = MPV_RENDER_PARAM_WL_DISPLAY;
    params[2].data = gdk_wayland_display_get_wl_display(display);
  } else if (GDK_IS_X11_DISPLAY(display)) {
    params[2].type = MPV_RENDER_PARAM_X11_DISPLAY;
    params[2].data = gdk_x11_display_get_xdisplay(display);
  }
  if (mpv_render_context_create(&self->render_context, self->handle, params) != 0) {
    g_printerr("media_kit: VideoOutput: deferred_init: mpv_render_context_create failed.\n");
    eglMakeCurrent(self->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (saved_glx) glXMakeCurrent(x_display, saved_drawable, saved_glx);
    return FALSE;
  }

  // Set mpv update callback
  mpv_render_context_set_update_callback(self->render_context,
      [](void* data) {
        VideoOutput* self = (VideoOutput*)data;
        if (self->destroyed) return;
        fl_texture_registrar_mark_texture_frame_available(
            self->texture_registrar, FL_TEXTURE(self->texture_gl));
      }, self);

  // Restore contexts
  if (flutter_context != EGL_NO_CONTEXT) {
    eglMakeCurrent(flutter_display, flutter_draw, flutter_read, flutter_context);
  } else {
    eglMakeCurrent(self->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  }
  if (saved_glx) glXMakeCurrent(x_display, saved_drawable, saved_glx);

  // Notify Dart about texture
  if (self->texture_update_callback) {
    gint64 texture_id = video_output_get_texture_id(self);
    self->texture_update_callback(texture_id, 1, 1, self->texture_update_callback_context);
  }

  g_print("media_kit: VideoOutput: deferred_init: H/W rendering ready.\n");
  return TRUE;
}

void video_output_set_size(VideoOutput* self, gint64 width, gint64 height) {
  // Ideally, a mutex should be used here & |video_output_get_width| +
  // |video_output_get_height|. However, that is throwing everything into a
  // deadlock. Flutter itself seems to have some synchronization mechanism in
  // rendering & platform channels AFAIK.

  // H/W
  if (self->texture_gl) {
    self->width = width;
    self->height = height;
  }
  // S/W
  if (self->texture_sw) {
    self->width = CLAMP(width, 0, SW_RENDERING_MAX_WIDTH);
    self->height = CLAMP(height, 0, SW_RENDERING_MAX_HEIGHT);
  }
}

mpv_render_context* video_output_get_render_context(VideoOutput* self) {
  return self->render_context;
}

EGLDisplay video_output_get_egl_display(VideoOutput* self) {
  return self->egl_display;
}

EGLContext video_output_get_egl_context(VideoOutput* self) {
  return self->egl_context;
}

EGLSurface video_output_get_egl_surface(VideoOutput* self) {
  return self->egl_surface;
}

guint8* video_output_get_pixel_buffer(VideoOutput* self) {
  return self->pixel_buffer;
}

gint64 video_output_get_width(VideoOutput* self) {
  // Fixed width.
  if (self->width) {
    return self->width;
  }

  // Video resolution dependent width.
  gint64 width = 0;
  gint64 height = 0;

  mpv_node params;
  mpv_get_property(self->handle, "video-out-params", MPV_FORMAT_NODE, &params);

  int64_t dw = 0, dh = 0, rotate = 0;
  if (params.format == MPV_FORMAT_NODE_MAP) {
    for (int32_t i = 0; i < params.u.list->num; i++) {
      char* key = params.u.list->keys[i];
      auto value = params.u.list->values[i];
      if (value.format == MPV_FORMAT_INT64) {
        if (strcmp(key, "dw") == 0) {
          dw = value.u.int64;
        }
        if (strcmp(key, "dh") == 0) {
          dh = value.u.int64;
        }
        if (strcmp(key, "rotate") == 0) {
          rotate = value.u.int64;
        }
      }
    }
    mpv_free_node_contents(&params);
  }

  width = rotate == 0 || rotate == 180 ? dw : dh;
  height = rotate == 0 || rotate == 180 ? dh : dw;

  if (self->texture_sw != NULL) {
    // Make sure |width| & |height| fit between |SW_RENDERING_MAX_WIDTH| &
    // |SW_RENDERING_MAX_HEIGHT| while maintaining aspect ratio.
    if (width >= SW_RENDERING_MAX_WIDTH) {
      return SW_RENDERING_MAX_WIDTH;
    }
    if (height >= SW_RENDERING_MAX_HEIGHT) {
      return width / height * SW_RENDERING_MAX_HEIGHT;
    }
  }

  return width;
}

gint64 video_output_get_height(VideoOutput* self) {
  // Fixed height.
  if (self->width) {
    return self->height;
  }

  // Video resolution dependent height.
  gint64 width = 0;
  gint64 height = 0;

  mpv_node params;
  mpv_get_property(self->handle, "video-out-params", MPV_FORMAT_NODE, &params);

  int64_t dw = 0, dh = 0, rotate = 0;
  if (params.format == MPV_FORMAT_NODE_MAP) {
    for (int32_t i = 0; i < params.u.list->num; i++) {
      char* key = params.u.list->keys[i];
      auto value = params.u.list->values[i];
      if (value.format == MPV_FORMAT_INT64) {
        if (strcmp(key, "dw") == 0) {
          dw = value.u.int64;
        }
        if (strcmp(key, "dh") == 0) {
          dh = value.u.int64;
        }
        if (strcmp(key, "rotate") == 0) {
          rotate = value.u.int64;
        }
      }
    }
    mpv_free_node_contents(&params);
  }

  width = rotate == 0 || rotate == 180 ? dw : dh;
  height = rotate == 0 || rotate == 180 ? dh : dw;

  if (self->texture_sw != NULL) {
    // Make sure |width| & |height| fit between |SW_RENDERING_MAX_WIDTH| &
    // |SW_RENDERING_MAX_HEIGHT| while maintaining aspect ratio.
    if (height >= SW_RENDERING_MAX_HEIGHT) {
      return SW_RENDERING_MAX_HEIGHT;
    }
    if (width >= SW_RENDERING_MAX_WIDTH) {
      return height / width * SW_RENDERING_MAX_WIDTH;
    }
  }

  return height;
}

gint64 video_output_get_texture_id(VideoOutput* self) {
  // H/W
  if (self->texture_gl) {
    return (gint64)self->texture_gl;
  }
  // S/W
  if (self->texture_sw) {
    return (gint64)self->texture_sw;
  }
  g_assert_not_reached();
  return -1;
}

void video_output_notify_texture_update(VideoOutput* self) {
  gint64 id = video_output_get_texture_id(self);
  gint64 width = video_output_get_width(self);
  gint64 height = video_output_get_height(self);
  gpointer context = self->texture_update_callback_context;
  if (self->texture_update_callback != NULL) {
    self->texture_update_callback(id, width, height, context);
  }
}
