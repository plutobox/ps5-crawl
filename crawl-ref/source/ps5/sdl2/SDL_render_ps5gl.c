/* Apotris for PS5 - SDL2 render driver on the console's OpenGL.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The console's SDL build has a single OpenGL context and no window surface, so
 * SDL's own renderers cannot run. This driver draws SDL_Render*() calls with
 * OpenGL 3.3 core shaders in the context the game's shader path shares
 * (SDL_ps5gl.c hands the same context to every SDL_GL_CreateContext call).
 * It follows the structure of SDL's OpenGL ES 2 renderer: geometry, points and
 * lines, with rectangles and copies queued by SDL as geometry.
 */
#include "SDL_internal.h"

#if defined(SDL_VIDEO_RENDER_PS5GL) && !defined(SDL_RENDER_DISABLED)

#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>

#include "render/SDL_sysrender.h"
#include "SDL_hints.h"

extern SDL_RenderDriver PS5GL_RenderDriver;

typedef struct
{
    GLuint texture;
    GLenum format; /* GL_RGBA, or GL_BGRA for ARGB8888 */
    int w, h;
    void *pixels; /* streaming textures: the pixels SDL_LockTexture hands out */
    int pitch;
    SDL_Rect locked;
} PS5GL_TextureData;

typedef struct
{
    SDL_GLContext context;
    GLuint program_solid, program_texture;
    GLint viewport_solid, viewport_texture;
    GLuint vao, vbo;
    size_t vbo_size;
    SDL_bool have_context;
} PS5GL_RenderData;

/* The game also draws with its own shaders in this context and deletes its handle to it
 * (SDL then leaves no context current): take it back before touching GL. */
static void activate(SDL_Renderer *renderer)
{
    PS5GL_RenderData *data = (PS5GL_RenderData *)renderer->driverdata;
    if (data->have_context && SDL_GL_GetCurrentContext() != data->context) {
        SDL_GL_MakeCurrent(renderer->window, data->context);
    }
}

static const char *k_vertex_source =
    "#version 330 core\n"
    "layout(location = 0) in vec2 in_position;\n"
    "layout(location = 1) in vec4 in_color;\n"
    "layout(location = 2) in vec2 in_texcoord;\n"
    "uniform vec2 u_viewport;\n"
    "out vec4 v_color;\n"
    "out vec2 v_texcoord;\n"
    "void main() {\n"
    "    gl_Position = vec4(in_position.x * 2.0 / u_viewport.x - 1.0,\n"
    "                       1.0 - in_position.y * 2.0 / u_viewport.y, 0.0, 1.0);\n"
    "    gl_PointSize = 1.0;\n"
    "    v_color = in_color;\n"
    "    v_texcoord = in_texcoord;\n"
    "}\n";

static const char *k_fragment_solid =
    "#version 330 core\n"
    "in vec4 v_color;\n"
    "in vec2 v_texcoord;\n"
    "out vec4 out_color;\n"
    "void main() { out_color = v_color; }\n";

static const char *k_fragment_texture =
    "#version 330 core\n"
    "uniform sampler2D u_texture;\n"
    "in vec4 v_color;\n"
    "in vec2 v_texcoord;\n"
    "out vec4 out_color;\n"
    "void main() { out_color = texture(u_texture, v_texcoord) * v_color; }\n";

static GLuint compile(GLenum type, const char *source)
{
    GLuint shader = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        SDL_SetError("PS5GL shader: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint link_program(const char *fragment_source)
{
    GLuint vs = compile(GL_VERTEX_SHADER, k_vertex_source);
    GLuint fs = compile(GL_FRAGMENT_SHADER, fragment_source);
    GLuint program;
    GLint ok = 0;
    if (!vs || !fs) {
        return 0;
    }
    program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        SDL_SetError("PS5GL program did not link");
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

static void PS5GL_WindowEvent(SDL_Renderer *renderer, const SDL_WindowEvent *event)
{
    (void)renderer;
    (void)event;
}

static int PS5GL_GetOutputSize(SDL_Renderer *renderer, int *w, int *h)
{
    SDL_GetWindowSize(renderer->window, w, h);
    return 0;
}

static SDL_bool PS5GL_SupportsBlendMode(SDL_Renderer *renderer, SDL_BlendMode blendMode)
{
    (void)renderer;
    return (blendMode == SDL_BLENDMODE_NONE || blendMode == SDL_BLENDMODE_BLEND ||
            blendMode == SDL_BLENDMODE_ADD || blendMode == SDL_BLENDMODE_MOD ||
            blendMode == SDL_BLENDMODE_MUL)
               ? SDL_TRUE
               : SDL_FALSE;
}

static void set_filter(GLenum target, SDL_ScaleMode mode)
{
    GLint filter = mode == SDL_ScaleModeNearest ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, filter);
}

static int PS5GL_CreateTexture(SDL_Renderer *renderer, SDL_Texture *texture)
{
    activate(renderer);
    PS5GL_TextureData *data;
    (void)renderer;
    if (texture->format != SDL_PIXELFORMAT_ABGR8888 && texture->format != SDL_PIXELFORMAT_ARGB8888) {
        return SDL_SetError("PS5GL: unsupported texture format");
    }
    if (texture->access == SDL_TEXTUREACCESS_TARGET) {
        return SDL_SetError("PS5GL: render targets are not supported");
    }
    data = (PS5GL_TextureData *)SDL_calloc(1, sizeof(*data));
    if (!data) {
        return SDL_OutOfMemory();
    }
    data->format = texture->format == SDL_PIXELFORMAT_ARGB8888 ? GL_BGRA : GL_RGBA;
    data->w = texture->w;
    data->h = texture->h;
    glGenTextures(1, &data->texture);
    glBindTexture(GL_TEXTURE_2D, data->texture);
    set_filter(GL_TEXTURE_2D, texture->scaleMode);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, texture->w, texture->h, 0, data->format, GL_UNSIGNED_BYTE, NULL);
    if (texture->access == SDL_TEXTUREACCESS_STREAMING) {
        data->pitch = texture->w * 4;
        data->pixels = SDL_calloc(texture->h, data->pitch);
        if (!data->pixels) {
            glDeleteTextures(1, &data->texture);
            SDL_free(data);
            return SDL_OutOfMemory();
        }
    }
    texture->driverdata = data;
    return 0;
}

static int PS5GL_LockTexture(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *rect,
                             void **pixels, int *pitch)
{
    PS5GL_TextureData *data = (PS5GL_TextureData *)texture->driverdata;
    (void)renderer;
    if (!data->pixels) {
        return SDL_SetError("PS5GL: only streaming textures can be locked");
    }
    data->locked = *rect;
    *pixels = (Uint8 *)data->pixels + rect->y * data->pitch + rect->x * 4;
    *pitch = data->pitch;
    return 0;
}

static void PS5GL_UnlockTexture(SDL_Renderer *renderer, SDL_Texture *texture)
{
    activate(renderer);
    PS5GL_TextureData *data = (PS5GL_TextureData *)texture->driverdata;
    (void)renderer;
    glBindTexture(GL_TEXTURE_2D, data->texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, data->pitch / 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, data->locked.x, data->locked.y, data->locked.w, data->locked.h,
                    data->format, GL_UNSIGNED_BYTE,
                    (Uint8 *)data->pixels + data->locked.y * data->pitch + data->locked.x * 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

static int PS5GL_UpdateTexture(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *rect,
                               const void *pixels, int pitch)
{
    activate(renderer);
    PS5GL_TextureData *data = (PS5GL_TextureData *)texture->driverdata;
    (void)renderer;
    glBindTexture(GL_TEXTURE_2D, data->texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, pitch / 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, rect->x, rect->y, rect->w, rect->h, data->format, GL_UNSIGNED_BYTE, pixels);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    return 0;
}

static void PS5GL_SetTextureScaleMode(SDL_Renderer *renderer, SDL_Texture *texture, SDL_ScaleMode scaleMode)
{
    activate(renderer);
    PS5GL_TextureData *data = (PS5GL_TextureData *)texture->driverdata;
    (void)renderer;
    glBindTexture(GL_TEXTURE_2D, data->texture);
    set_filter(GL_TEXTURE_2D, scaleMode);
}

static int PS5GL_SetRenderTarget(SDL_Renderer *renderer, SDL_Texture *texture)
{
    (void)renderer;
    return texture ? SDL_SetError("PS5GL: render targets are not supported") : 0;
}

static int PS5GL_QueueNoop(SDL_Renderer *renderer, SDL_RenderCommand *cmd)
{
    (void)renderer;
    (void)cmd;
    return 0; /* viewport and draw color are read from the command when it runs */
}

static int PS5GL_QueueDrawPoints(SDL_Renderer *renderer, SDL_RenderCommand *cmd, const SDL_FPoint *points, int count)
{
    SDL_VertexSolid *verts = (SDL_VertexSolid *)SDL_AllocateRenderVertices(renderer, count * sizeof(*verts), 0, &cmd->data.draw.first);
    SDL_Color color;
    int i;
    if (!verts) {
        return -1;
    }
    color.r = cmd->data.draw.r;
    color.g = cmd->data.draw.g;
    color.b = cmd->data.draw.b;
    color.a = cmd->data.draw.a;
    cmd->data.draw.count = count;
    for (i = 0; i < count; i++) {
        verts[i].position.x = 0.5f + points[i].x;
        verts[i].position.y = 0.5f + points[i].y;
        verts[i].color = color;
    }
    return 0;
}

static int PS5GL_QueueDrawLines(SDL_Renderer *renderer, SDL_RenderCommand *cmd, const SDL_FPoint *points, int count)
{
    /* the same vertices as points, drawn as a line strip (or pairs) */
    return PS5GL_QueueDrawPoints(renderer, cmd, points, count);
}

static int PS5GL_QueueGeometry(SDL_Renderer *renderer, SDL_RenderCommand *cmd, SDL_Texture *texture,
                               const float *xy, int xy_stride, const SDL_Color *color, int color_stride,
                               const float *uv, int uv_stride, int num_vertices, const void *indices,
                               int num_indices, int size_indices, float scale_x, float scale_y)
{
    int count = indices ? num_indices : num_vertices;
    int i;
    size_indices = indices ? size_indices : 0;
    cmd->data.draw.count = count;

    if (texture) {
        SDL_Vertex *verts = (SDL_Vertex *)SDL_AllocateRenderVertices(renderer, count * sizeof(*verts), 0, &cmd->data.draw.first);
        if (!verts) {
            return -1;
        }
        for (i = 0; i < count; i++) {
            int j = i;
            const float *xy_;
            const float *uv_;
            if (size_indices == 4) {
                j = ((const Uint32 *)indices)[i];
            } else if (size_indices == 2) {
                j = ((const Uint16 *)indices)[i];
            } else if (size_indices == 1) {
                j = ((const Uint8 *)indices)[i];
            }
            xy_ = (const float *)((const char *)xy + j * xy_stride);
            uv_ = (const float *)((const char *)uv + j * uv_stride);
            verts[i].position.x = xy_[0] * scale_x;
            verts[i].position.y = xy_[1] * scale_y;
            verts[i].color = *(const SDL_Color *)((const char *)color + j * color_stride);
            verts[i].tex_coord.x = uv_[0];
            verts[i].tex_coord.y = uv_[1];
        }
    } else {
        SDL_VertexSolid *verts = (SDL_VertexSolid *)SDL_AllocateRenderVertices(renderer, count * sizeof(*verts), 0, &cmd->data.draw.first);
        if (!verts) {
            return -1;
        }
        for (i = 0; i < count; i++) {
            int j = i;
            const float *xy_;
            if (size_indices == 4) {
                j = ((const Uint32 *)indices)[i];
            } else if (size_indices == 2) {
                j = ((const Uint16 *)indices)[i];
            } else if (size_indices == 1) {
                j = ((const Uint8 *)indices)[i];
            }
            xy_ = (const float *)((const char *)xy + j * xy_stride);
            verts[i].position.x = xy_[0] * scale_x;
            verts[i].position.y = xy_[1] * scale_y;
            verts[i].color = *(const SDL_Color *)((const char *)color + j * color_stride);
        }
    }
    return 0;
}

static void apply_blend(SDL_BlendMode mode)
{
    if (mode == SDL_BLENDMODE_NONE) {
        glDisable(GL_BLEND);
        return;
    }
    glEnable(GL_BLEND);
    switch (mode) {
    case SDL_BLENDMODE_ADD:
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ZERO, GL_ONE);
        break;
    case SDL_BLENDMODE_MOD:
        glBlendFuncSeparate(GL_ZERO, GL_SRC_COLOR, GL_ZERO, GL_ONE);
        break;
    case SDL_BLENDMODE_MUL:
        glBlendFuncSeparate(GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
        break;
    default: /* SDL_BLENDMODE_BLEND */
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        break;
    }
}

static int PS5GL_RunCommandQueue(SDL_Renderer *renderer, SDL_RenderCommand *cmd, void *vertices, size_t vertsize)
{
    activate(renderer);
    PS5GL_RenderData *data = (PS5GL_RenderData *)renderer->driverdata;
    SDL_Rect viewport = { 0, 0, 0, 0 };
    SDL_Rect cliprect = { 0, 0, 0, 0 };
    SDL_bool clip_enabled = SDL_FALSE;
    int window_w = 0, window_h = 0;
    GLint saved_viewport[4];
    GLuint bound_program = 0;
    SDL_bool viewport_dirty = SDL_TRUE, clip_dirty = SDL_TRUE;

    SDL_GetWindowSize(renderer->window, &window_w, &window_h);
    viewport.w = window_w;
    viewport.h = window_h;

    glGetIntegerv(GL_VIEWPORT, saved_viewport);
    glBindVertexArray(data->vao);
    glBindBuffer(GL_ARRAY_BUFFER, data->vbo);
    if (vertsize > data->vbo_size) {
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)vertsize, vertices, GL_STREAM_DRAW);
        data->vbo_size = vertsize;
    } else if (vertsize) {
        glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)vertsize, vertices);
    }
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    while (cmd) {
        switch (cmd->command) {
        case SDL_RENDERCMD_SETVIEWPORT:
            viewport = cmd->data.viewport.rect;
            viewport_dirty = SDL_TRUE;
            clip_dirty = SDL_TRUE;
            break;

        case SDL_RENDERCMD_SETCLIPRECT:
            clip_enabled = cmd->data.cliprect.enabled;
            cliprect = cmd->data.cliprect.rect;
            clip_dirty = SDL_TRUE;
            break;

        case SDL_RENDERCMD_CLEAR:
            glDisable(GL_SCISSOR_TEST);
            glClearColor(cmd->data.color.r / 255.0f, cmd->data.color.g / 255.0f,
                         cmd->data.color.b / 255.0f, cmd->data.color.a / 255.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            clip_dirty = SDL_TRUE;
            break;

        case SDL_RENDERCMD_DRAW_LINES:
        case SDL_RENDERCMD_DRAW_POINTS:
        case SDL_RENDERCMD_GEOMETRY: {
            SDL_Texture *texture = cmd->data.draw.texture;
            SDL_BlendMode blend = cmd->data.draw.blend;
            size_t stride = texture ? sizeof(SDL_Vertex) : sizeof(SDL_VertexSolid);
            GLuint program = texture ? data->program_texture : data->program_solid;
            GLint viewport_uniform = texture ? data->viewport_texture : data->viewport_solid;
            GLenum mode = cmd->command == SDL_RENDERCMD_GEOMETRY ? GL_TRIANGLES
                          : cmd->command == SDL_RENDERCMD_DRAW_POINTS ? GL_POINTS
                          : (cmd->data.draw.count > 2 ? GL_LINE_STRIP : GL_LINES);

            if (viewport_dirty) {
                glViewport(viewport.x, window_h - viewport.y - viewport.h, viewport.w, viewport.h);
                viewport_dirty = SDL_FALSE;
                bound_program = 0;
            }
            if (clip_dirty) {
                if (clip_enabled) {
                    glEnable(GL_SCISSOR_TEST);
                    glScissor(viewport.x + cliprect.x, window_h - (viewport.y + cliprect.y) - cliprect.h,
                              cliprect.w, cliprect.h);
                } else {
                    glDisable(GL_SCISSOR_TEST);
                }
                clip_dirty = SDL_FALSE;
            }
            if (program != bound_program) {
                glUseProgram(program);
                bound_program = program;
            }
            glUniform2f(viewport_uniform, (GLfloat)viewport.w, (GLfloat)viewport.h);
            apply_blend(blend);

            /* attributes: position (2 floats), color (4 bytes), texture coordinate (2 floats) */
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, (GLsizei)stride, (const void *)(cmd->data.draw.first));
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, (GLsizei)stride,
                                  (const void *)(cmd->data.draw.first + 8));
            if (texture) {
                PS5GL_TextureData *tex = (PS5GL_TextureData *)texture->driverdata;
                glEnableVertexAttribArray(2);
                glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, (GLsizei)stride,
                                      (const void *)(cmd->data.draw.first + 12));
                glBindTexture(GL_TEXTURE_2D, tex->texture);
            } else {
                glDisableVertexAttribArray(2);
            }
            glDrawArrays(mode, 0, (GLsizei)cmd->data.draw.count);
            break;
        }

        case SDL_RENDERCMD_SETDRAWCOLOR:
        case SDL_RENDERCMD_FILL_RECTS: /* queued as geometry */
        case SDL_RENDERCMD_COPY:
        case SDL_RENDERCMD_COPY_EX:
        case SDL_RENDERCMD_NO_OP:
            break;
        }
        cmd = cmd->next;
    }

    /* hand the context back the way the game's own shader path expects it */
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glViewport(saved_viewport[0], saved_viewport[1], saved_viewport[2], saved_viewport[3]);
    return 0;
}

static int PS5GL_RenderReadPixels(SDL_Renderer *renderer, const SDL_Rect *rect, Uint32 format, void *pixels, int pitch)
{
    (void)renderer;
    (void)rect;
    (void)format;
    (void)pixels;
    (void)pitch;
    return SDL_Unsupported();
}

static int PS5GL_RenderPresent(SDL_Renderer *renderer)
{
    activate(renderer);
    return SDL_GL_SwapWindow(renderer->window), 0;
}

static void PS5GL_DestroyTexture(SDL_Renderer *renderer, SDL_Texture *texture)
{
    activate(renderer);
    PS5GL_TextureData *data = (PS5GL_TextureData *)texture->driverdata;
    (void)renderer;
    if (data) {
        glDeleteTextures(1, &data->texture);
        SDL_free(data->pixels);
        SDL_free(data);
        texture->driverdata = NULL;
    }
}

static void PS5GL_DestroyRenderer(SDL_Renderer *renderer)
{
    PS5GL_RenderData *data = (PS5GL_RenderData *)renderer->driverdata;
    if (data) {
        if (data->have_context) {
            glDeleteProgram(data->program_solid);
            glDeleteProgram(data->program_texture);
            glDeleteBuffers(1, &data->vbo);
            glDeleteVertexArrays(1, &data->vao);
            SDL_GL_DeleteContext(data->context);
        }
        SDL_free(data);
    }
    SDL_free(renderer);
}

static int PS5GL_SetVSync(SDL_Renderer *renderer, const int vsync)
{
    (void)renderer;
    return SDL_GL_SetSwapInterval(vsync ? 1 : 0);
}

static SDL_Renderer *PS5GL_CreateRenderer(SDL_Window *window, Uint32 flags)
{
    SDL_Renderer *renderer;
    PS5GL_RenderData *data;

    if (!(SDL_GetWindowFlags(window) & SDL_WINDOW_OPENGL) || (flags & SDL_RENDERER_SOFTWARE)) {
        return NULL;
    }
    renderer = (SDL_Renderer *)SDL_calloc(1, sizeof(*renderer));
    data = (PS5GL_RenderData *)SDL_calloc(1, sizeof(*data));
    if (!renderer || !data) {
        SDL_free(renderer);
        SDL_free(data);
        SDL_OutOfMemory();
        return NULL;
    }
    renderer->driverdata = data;
    renderer->window = window;

    /* the context the game's shader path asks for next (4.1 core) is this one */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    data->context = SDL_GL_CreateContext(window);
    if (!data->context) {
        PS5GL_DestroyRenderer(renderer);
        return NULL;
    }
    data->have_context = SDL_TRUE;
    if (SDL_GL_MakeCurrent(window, data->context) < 0) {
        PS5GL_DestroyRenderer(renderer);
        return NULL;
    }

    data->program_solid = link_program(k_fragment_solid);
    data->program_texture = link_program(k_fragment_texture);
    if (!data->program_solid || !data->program_texture) {
        PS5GL_DestroyRenderer(renderer);
        return NULL;
    }
    data->viewport_solid = glGetUniformLocation(data->program_solid, "u_viewport");
    data->viewport_texture = glGetUniformLocation(data->program_texture, "u_viewport");
    glUseProgram(data->program_texture);
    glUniform1i(glGetUniformLocation(data->program_texture, "u_texture"), 0);
    glUseProgram(0);
    glGenVertexArrays(1, &data->vao);
    glGenBuffers(1, &data->vbo);

    renderer->WindowEvent = PS5GL_WindowEvent;
    renderer->GetOutputSize = PS5GL_GetOutputSize;
    renderer->SupportsBlendMode = PS5GL_SupportsBlendMode;
    renderer->CreateTexture = PS5GL_CreateTexture;
    renderer->UpdateTexture = PS5GL_UpdateTexture;
    renderer->LockTexture = PS5GL_LockTexture;
    renderer->UnlockTexture = PS5GL_UnlockTexture;
    renderer->SetTextureScaleMode = PS5GL_SetTextureScaleMode;
    renderer->SetRenderTarget = PS5GL_SetRenderTarget;
    renderer->QueueSetViewport = PS5GL_QueueNoop;
    renderer->QueueSetDrawColor = PS5GL_QueueNoop;
    renderer->QueueDrawPoints = PS5GL_QueueDrawPoints;
    renderer->QueueDrawLines = PS5GL_QueueDrawLines;
    renderer->QueueGeometry = PS5GL_QueueGeometry;
    renderer->RunCommandQueue = PS5GL_RunCommandQueue;
    renderer->RenderReadPixels = PS5GL_RenderReadPixels;
    renderer->RenderPresent = PS5GL_RenderPresent;
    renderer->DestroyTexture = PS5GL_DestroyTexture;
    renderer->DestroyRenderer = PS5GL_DestroyRenderer;
    renderer->SetVSync = PS5GL_SetVSync;

    renderer->rect_index_order[0] = 0;
    renderer->rect_index_order[1] = 1;
    renderer->rect_index_order[2] = 3;
    renderer->rect_index_order[3] = 1;
    renderer->rect_index_order[4] = 3;
    renderer->rect_index_order[5] = 2;

    renderer->info = PS5GL_RenderDriver.info;
    renderer->info.flags = SDL_RENDERER_ACCELERATED;
    SDL_GL_SetSwapInterval((flags & SDL_RENDERER_PRESENTVSYNC) ? 1 : 0);
    if (SDL_GL_GetSwapInterval() != 0) {
        renderer->info.flags |= SDL_RENDERER_PRESENTVSYNC;
    }
    return renderer;
}

SDL_RenderDriver PS5GL_RenderDriver = {
    PS5GL_CreateRenderer,
    {
        "ps5gl",
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC,
        2,
        { SDL_PIXELFORMAT_ABGR8888, SDL_PIXELFORMAT_ARGB8888 },
        16384,
        16384,
    }
};

#endif /* SDL_VIDEO_RENDER_PS5GL */
