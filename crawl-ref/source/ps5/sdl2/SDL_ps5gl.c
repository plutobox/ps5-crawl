/* PS5 OpenGL - SDL2 video driver for the public EGL contract.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Built inside SDL2; public SDL APIs and the upstream event queue are unchanged.
 *
 * Altered for Apotris: the console only has a full-screen output, so any window
 * the game asks for (any size, resizable or not) becomes that output, and a
 * second SDL_GL_CreateContext returns the one context instead of failing.
 */
#include "SDL_internal.h"
#include "video/SDL_sysvideo.h"
#include "events/SDL_keyboard_c.h"
#include "events/SDL_windowevents_c.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#include "ps5g19_display.h"
#if __has_include(<ps5_opengl_display_modes.h>)
#include <ps5_opengl_display_modes.h>
#define G19_RUNTIME_MODE 1
#endif

/* Mode the application chose with eglSetDisplayModePS5 before SDL_Init; the
 * compile-time profile is only the fallback. */
static int g19_w = PS5_OPENGL_NATIVE_WIDTH;
static int g19_h = PS5_OPENGL_NATIVE_HEIGHT;
static int g19_fps = PS5_OPENGL_NATIVE_FPS;

/* ponytail: one fixed-profile window/context on the video thread; extend only with a
 * separately validated EGL ownership contract. No private presentation API. */
typedef struct {
    EGLDisplay display;
    EGLConfig config;
    EGLSurface surface;
    EGLContext context;
    SDL_Window *window;
    SDL_threadID thread;
    int interval;
    int context_users;
} G19;

static int egl_error(const char *operation)
{
    return SDL_SetError("G19 %s: EGL error 0x%04x", operation, eglGetError());
}

static int on_thread(G19 *g)
{
    return SDL_ThreadID() == g->thread ? 0 :
        SDL_SetError("G19 requires the video thread");
}

static int video_init(_THIS)
{
    G19 *g = _this->driverdata;
    int index;
    SDL_DisplayMode mode;
    g->thread = SDL_ThreadID();
    g->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g->display == EGL_NO_DISPLAY) return egl_error("eglGetDisplay");
    if (!eglInitialize(g->display, NULL, NULL)) {
        g->display = EGL_NO_DISPLAY;
        return egl_error("eglInitialize");
    }
#ifdef G19_RUNTIME_MODE
    {
        EGLint w = 0, h = 0, f = 0;
        if (eglGetDisplayModePS5(g->display, &w, &h, &f) && w > 0 && h > 0) {
            g19_w = w;
            g19_h = h;
            if (f > 0) g19_fps = f;
        }
    }
#endif
    mode.format = SDL_PIXELFORMAT_ABGR8888;
    mode.w = g19_w;
    mode.h = g19_h;
    mode.refresh_rate = g19_fps;
    mode.driverdata = NULL;
    index = SDL_AddBasicVideoDisplay(&mode);
    if (index < 0) return -1;
    return SDL_AddDisplayMode(&_this->displays[index], &mode) ? 0 : -1;
}

static int load_library(_THIS, const char *path)
{
    (void)_this;
    return path ? SDL_SetError("G19 is statically linked; use a NULL GL library path") : 0;
}

/* The game's shader loader only writes a #version line for Android, PortMaster and
 * macOS; on a desktop it leaves it out, and a desktop driver then takes a looser
 * dialect than GLSL 1.10. This one is strict, so shaders without a version get the
 * one the game uses on macOS (4.10 core) in front of everything. */
static void ps5_glShaderSource(GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length)
{
    static const GLchar version[] = "#version 410 core\n";
    const GLchar **strings;
    GLint *lengths;
    GLsizei i;
    int has_version = 0;

    for (i = 0; i < count && !has_version; i++) {
        const GLchar *text = string[i];
        if (text && (length == NULL || length[i] < 0 ? SDL_strstr(text, "#version") != NULL
                                                    : (size_t)length[i] >= 8 && SDL_strstr(text, "#version") != NULL)) {
            has_version = 1;
        }
    }
    if (has_version || count <= 0) {
        glShaderSource(shader, count, string, length);
        return;
    }
    strings = (const GLchar **)SDL_malloc((count + 1) * sizeof(*strings));
    lengths = (GLint *)SDL_malloc((count + 1) * sizeof(*lengths));
    if (!strings || !lengths) {
        SDL_free(strings);
        SDL_free(lengths);
        glShaderSource(shader, count, string, length);
        return;
    }
    strings[0] = version;
    lengths[0] = (GLint)(sizeof(version) - 1);
    for (i = 0; i < count; i++) {
        strings[i + 1] = string[i];
        lengths[i + 1] = length ? length[i] : -1;
    }
    glShaderSource(shader, count + 1, strings, lengths);
    SDL_free(strings);
    SDL_free(lengths);
}

static void *get_proc(_THIS, const char *name)
{
    void *proc;
    (void)_this;
    if (!name || !*name) {
        SDL_SetError("G19 requires a GL procedure name");
        return NULL;
    }
    if (SDL_strcmp(name, "glShaderSource") == 0) return (void *)ps5_glShaderSource;
    proc = (void *)eglGetProcAddress(name);
    if (!proc) SDL_SetError("G19 has no GL procedure %s", name);
    return proc;
}

static int create_window(_THIS, SDL_Window *window)
{
    G19 *g = _this->driverdata;
    EGLint count = 0, width = 0, height = 0;
    const EGLint attributes[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, _this->gl_config.red_size,
        EGL_GREEN_SIZE, _this->gl_config.green_size,
        EGL_BLUE_SIZE, _this->gl_config.blue_size,
        EGL_ALPHA_SIZE, _this->gl_config.alpha_size,
        EGL_DEPTH_SIZE, _this->gl_config.depth_size,
        EGL_STENCIL_SIZE, _this->gl_config.stencil_size, EGL_NONE
    };
    if (on_thread(g) < 0) return -1;
    if (g->window || g->surface || !(window->flags & SDL_WINDOW_OPENGL))
        return SDL_SetError("PS5 supports one OpenGL window");
    /* the window is the full-screen output whatever size was asked for */
    window->x = window->windowed.x = 0;
    window->y = window->windowed.y = 0;
    window->w = window->windowed.w = g19_w;
    window->h = window->windowed.h = g19_h;
    window->flags |= SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_SHOWN | SDL_WINDOW_INPUT_FOCUS;
    window->flags &= ~SDL_WINDOW_RESIZABLE;
    if (_this->gl_config.stereo || _this->gl_config.multisamplebuffers ||
        _this->gl_config.multisamplesamples || _this->gl_config.floatbuffers ||
        _this->gl_config.framebuffer_srgb_capable || _this->gl_config.accum_red_size ||
        _this->gl_config.accum_green_size || _this->gl_config.accum_blue_size ||
        _this->gl_config.accum_alpha_size || _this->gl_config.buffer_size > 32 ||
        !_this->gl_config.double_buffer)
        return SDL_SetError("G19 SDL supports double-buffered RGBA8 without MSAA, stereo, accumulation or sRGB");
    if (!eglChooseConfig(g->display, attributes, &g->config, 1, &count))
        return egl_error("eglChooseConfig");
    if (count != 1) return SDL_SetError("G19 has no matching EGL config");
    g->surface = eglCreateWindowSurface(g->display, g->config, (EGLNativeWindowType)0, NULL);
    if (!g->surface) return egl_error("eglCreateWindowSurface");
    /* Establish ownership before queries, so SDL's failed-create cleanup works. */
    g->window = window;
    if (!eglQuerySurface(g->display, g->surface, EGL_WIDTH, &width) ||
        !eglQuerySurface(g->display, g->surface, EGL_HEIGHT, &height))
        return egl_error("eglQuerySurface");
    if (width != window->w || height != window->h)
        return SDL_SetError("G19 EGL drawable %dx%d does not match the requested %dx%d window",
                            width, height, window->w, window->h);
    g->interval = 1;
    SDL_SetKeyboardFocus(window); /* SDL also uses focus to gate joystick events. */
    /* A desktop window manager reports the size of a new window; the game reads
     * its drawable size only from this event (4:3 until it arrives). */
    SDL_SendWindowEvent(window, SDL_WINDOWEVENT_SIZE_CHANGED, window->w, window->h);
    return 0;
}

static int make_current(_THIS, SDL_Window *window, SDL_GLContext context)
{
    G19 *g = _this->driverdata;
    EGLSurface surface = context ? g->surface : EGL_NO_SURFACE;
    if (on_thread(g) < 0) return -1;
    if (context && (context != g->context || window != g->window))
        return SDL_SetError("G19 context/window mismatch");
    if (!eglMakeCurrent(g->display, surface, surface, (EGLContext)context))
        return egl_error("eglMakeCurrent");
    return 0;
}

static void delete_context(_THIS, SDL_GLContext context)
{
    G19 *g = _this->driverdata;
    if (on_thread(g) < 0 || !context) return;
    if (context != g->context) {
        SDL_SetError("G19 unknown context");
        return;
    }
    if (g->context_users > 1) { /* still used by another SDL_GL_CreateContext */
        g->context_users--;
        return;
    }
    /* SDL_GL_DeleteContext detaches first and updates SDL's TLS. If that
     * failed, EGL rejects destruction; keep both ownership records intact. */
    if (!eglDestroyContext(g->display, g->context)) {
        egl_error("eglDestroyContext");
        return;
    }
    g->context = EGL_NO_CONTEXT;
}

static SDL_GLContext create_context(_THIS, SDL_Window *window)
{
    G19 *g = _this->driverdata;
    const int major = _this->gl_config.major_version, minor = _this->gl_config.minor_version;
    const int version = major * 10 + minor;
    const int core = _this->gl_config.profile_mask == SDL_GL_CONTEXT_PROFILE_CORE;
    /* SDL's default mask (0) asks for the desktop default, which is Compatibility. */
    const int compatibility = !_this->gl_config.profile_mask ||
        _this->gl_config.profile_mask == SDL_GL_CONTEXT_PROFILE_COMPATIBILITY;
    const EGLint attributes[] = {
        EGL_CONTEXT_MAJOR_VERSION_KHR, major, EGL_CONTEXT_MINOR_VERSION_KHR, minor,
        EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, core ? EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR :
                                                    EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT_KHR,
        EGL_NONE
    };
    if (on_thread(g) < 0) return NULL;
    /* The console has a single context: the second request (the game makes one
     * for its renderer and one for its shaders) shares it. */
    if (g->context && window == g->window) {
        g->context_users++;
        /* SDL takes a new context to be current when the driver returns it: after
         * SDL_GL_DeleteContext of another user it is not, so make it so */
        if (make_current(_this, window, (SDL_GLContext)g->context) < 0) {
            g->context_users--;
            return NULL;
        }
        return (SDL_GLContext)g->context;
    }
    /* Core 3.3 to 4.6 is the conformance-tested path. Compatibility contexts are passed
     * through to EGL as requested; see the README for what is covered there. */
    if (g->context || window != g->window ||
        (!core && !compatibility) || major < 1 || minor < 0 || minor > 6 || version > 46 ||
        (core && version < 33) ||
        _this->gl_config.flags || _this->gl_config.share_with_current_context ||
        _this->gl_config.no_error || _this->gl_config.reset_notification ||
        _this->gl_config.release_behavior != SDL_GL_CONTEXT_RELEASE_BEHAVIOR_FLUSH) {
        SDL_SetError("G19 SDL requires one unshared OpenGL context with default flags: "
                     "3.3 to 4.6 Core, or Compatibility up to 4.6");
        return NULL;
    }
    if (!eglBindAPI(EGL_OPENGL_API)) {
        egl_error("eglBindAPI");
        return NULL;
    }
    g->context = eglCreateContext(g->display, g->config, EGL_NO_CONTEXT, attributes);
    if (!g->context) {
        egl_error("eglCreateContext");
        return NULL;
    }
    g->context_users = 1;
    if (make_current(_this, window, g->context) < 0) {
        delete_context(_this, g->context);
        return NULL;
    }
    return (SDL_GLContext)g->context;
}

static int set_interval(_THIS, int interval)
{
    G19 *g = _this->driverdata;
    if (on_thread(g) < 0) return -1;
    if (interval != 0 && interval != 1) return SDL_SetError("G19 swap interval must be 0 or 1");
    if (!eglSwapInterval(g->display, interval)) return egl_error("eglSwapInterval");
    g->interval = interval;
    return 0;
}

static int get_interval(_THIS) { return ((G19 *)_this->driverdata)->interval; }

static int swap_window(_THIS, SDL_Window *window)
{
    G19 *g = _this->driverdata;
    if (on_thread(g) < 0) return -1;
    if (window != g->window) return SDL_SetError("G19 unknown window");
    return eglSwapBuffers(g->display, g->surface) ? 0 : egl_error("eglSwapBuffers");
}

static void destroy_window(_THIS, SDL_Window *window)
{
    G19 *g = _this->driverdata;
    if (g->window != window) return; /* Includes rejected second-window creation. */
    if (on_thread(g) < 0) return;
    g->window = NULL;
    /* SDL contexts outlive windows; release the drawable, keep the context. */
    if (make_current(_this, NULL, NULL) < 0) return;
    if (!eglDestroySurface(g->display, g->surface)) {
        egl_error("eglDestroySurface");
        return;
    }
    g->surface = EGL_NO_SURFACE;
}

static void set_window_size(_THIS, SDL_Window *window)
{
    (void)_this;
    window->w = window->windowed.w = g19_w;
    window->h = window->windowed.h = g19_h;
    SDL_SetError("G19 SDL does not support resizing");
}

static void video_quit(_THIS)
{
    G19 *g = _this->driverdata;
    if (on_thread(g) < 0 || !g->display) return;
    delete_context(_this, g->context);
    if (g->context) return; /* Never release resources still owned by EGL. */
    if (g->surface) {
        if (!eglDestroySurface(g->display, g->surface)) {
            egl_error("eglDestroySurface(quit)");
            return;
        }
        g->surface = EGL_NO_SURFACE;
    }
    if (!eglTerminate(g->display)) {
        egl_error("eglTerminate");
        return;
    }
    g->display = EGL_NO_DISPLAY;
}

static void free_device(_THIS)
{
    SDL_free(_this->driverdata);
    SDL_free(_this);
}

/* Required SDL callback; joystick polling follows this in SDL_PumpEvents. */
static void pump_events(_THIS) { (void)_this; }

static SDL_VideoDevice *create_device(void)
{
    SDL_VideoDevice *device = SDL_calloc(1, sizeof(*device));
    if (!device) { SDL_OutOfMemory(); return NULL; }
    device->driverdata = SDL_calloc(1, sizeof(G19));
    if (!device->driverdata) { SDL_free(device); SDL_OutOfMemory(); return NULL; }
    device->VideoInit = video_init;
    device->VideoQuit = video_quit;
    device->PumpEvents = pump_events;
    device->CreateSDLWindow = create_window;
    device->DestroyWindow = destroy_window;
    device->SetWindowSize = set_window_size;
    device->GL_LoadLibrary = load_library;
    device->GL_GetProcAddress = get_proc;
    device->GL_CreateContext = create_context;
    device->GL_MakeCurrent = make_current;
    device->GL_DeleteContext = delete_context;
    device->GL_SetSwapInterval = set_interval;
    device->GL_GetSwapInterval = get_interval;
    device->GL_SwapWindow = swap_window;
    device->free = free_device;
    /* SDL_PumpEvents already updates the upstream joystick backend. No extra
     * polling thread or replacement event queue is required by this consumer. */
    return device;
}

VideoBootStrap PS5_bootstrap = { "ps5-g19", "PS5 frozen G19 EGL", create_device, NULL };
