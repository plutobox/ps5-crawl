#include "AppHdr.h"

#ifdef USE_TILE_LOCAL
#ifdef USE_GL

#include "glwrapper-ogl.h"

// How do we get access to the GL calls?
// If other UI types use the -ogl wrapper they should
// include more conditional includes here.
#ifdef USE_SDL
# ifdef USE_GLES
#  ifdef __ANDROID__
#   include <SDL.h>
#  else
#   include <SDL2/SDL.h>
#   include <SDL_gles.h>
#  endif
#  include <GLES/gl.h>
# else
#  include <SDL_opengl.h>
#  include <SDL_video.h>
# endif
#endif

#include "options.h"
#include "stringutil.h"
#include "tilesdl.h"

#ifdef __ANDROID__
# include <android/log.h>
#endif

#if defined(USE_GL_CORE) && defined(USE_GLES)
# error "USE_GL_CORE and USE_GLES cannot be combined"
#endif

#ifdef USE_GL_CORE
// OpenGL 3.3 core profile renderer.
//
// The legacy path below uses the fixed-function pipeline (glVertexPointer,
// glMatrixMode, glAlphaFunc, ...), which core profiles (and platforms that only
// offer one, such as Mesa-based console ports) do not provide. Under
// USE_GL_CORE the same GLStateManager interface is implemented with a single
// shader program that reproduces what the fixed-function state did:
//   - vertex colour (or the constant state colour) modulated by the texture
//     (GL_MODULATE),
//   - glAlphaFunc(GL_NOTEQUAL, ref) as a discard,
//   - glOrtho projection and glTranslate/glScale modelview as uniforms.
// Vertex data is streamed into shared buffers on every draw, exactly as the
// client-side arrays were read on every draw before.
namespace
{
// Functions beyond OpenGL 1.1 are not exported by every platform's GL library
// (e.g. opengl32.dll), so they are loaded at runtime.
#define CORE_GL_FUNCS(X) \
    X(PFNGLCREATESHADERPROC,             CreateShader) \
    X(PFNGLSHADERSOURCEPROC,             ShaderSource) \
    X(PFNGLCOMPILESHADERPROC,            CompileShader) \
    X(PFNGLGETSHADERIVPROC,              GetShaderiv) \
    X(PFNGLGETSHADERINFOLOGPROC,         GetShaderInfoLog) \
    X(PFNGLDELETESHADERPROC,             DeleteShader) \
    X(PFNGLCREATEPROGRAMPROC,            CreateProgram) \
    X(PFNGLATTACHSHADERPROC,             AttachShader) \
    X(PFNGLBINDATTRIBLOCATIONPROC,       BindAttribLocation) \
    X(PFNGLLINKPROGRAMPROC,              LinkProgram) \
    X(PFNGLGETPROGRAMIVPROC,             GetProgramiv) \
    X(PFNGLGETPROGRAMINFOLOGPROC,        GetProgramInfoLog) \
    X(PFNGLUSEPROGRAMPROC,               UseProgram) \
    X(PFNGLDELETEPROGRAMPROC,            DeleteProgram) \
    X(PFNGLGETUNIFORMLOCATIONPROC,       GetUniformLocation) \
    X(PFNGLUNIFORM1IPROC,                Uniform1i) \
    X(PFNGLUNIFORM1FPROC,                Uniform1f) \
    X(PFNGLUNIFORM3FPROC,                Uniform3f) \
    X(PFNGLUNIFORMMATRIX4FVPROC,         UniformMatrix4fv) \
    X(PFNGLGENVERTEXARRAYSPROC,          GenVertexArrays) \
    X(PFNGLBINDVERTEXARRAYPROC,          BindVertexArray) \
    X(PFNGLDELETEVERTEXARRAYSPROC,       DeleteVertexArrays) \
    X(PFNGLGENBUFFERSPROC,               GenBuffers) \
    X(PFNGLBINDBUFFERPROC,               BindBuffer) \
    X(PFNGLBUFFERDATAPROC,               BufferData) \
    X(PFNGLDELETEBUFFERSPROC,            DeleteBuffers) \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC,  EnableVertexAttribArray) \
    X(PFNGLDISABLEVERTEXATTRIBARRAYPROC, DisableVertexAttribArray) \
    X(PFNGLVERTEXATTRIBPOINTERPROC,      VertexAttribPointer) \
    X(PFNGLVERTEXATTRIB2FPROC,           VertexAttrib2f) \
    X(PFNGLVERTEXATTRIB4FPROC,           VertexAttrib4f) \
    X(PFNGLGENERATEMIPMAPPROC,           GenerateMipmap)

struct CoreGL
{
#define X(type, name) type name = nullptr;
    CORE_GL_FUNCS(X)
#undef X
};

CoreGL glc;

void _load_core_gl()
{
    string missing;
#define X(type, name) \
    glc.name = reinterpret_cast<type>(SDL_GL_GetProcAddress("gl" #name)); \
    if (!glc.name) \
        missing += " gl" #name;
    CORE_GL_FUNCS(X)
#undef X
    if (!missing.empty())
        die("OpenGL core profile functions unavailable:%s", missing.c_str());
}
#undef CORE_GL_FUNCS

enum core_attrib
{
    ATTRIB_POS = 0,
    ATTRIB_TEX = 1,
    ATTRIB_COL = 2,
};

const char *_vertex_shader_src =
    "#version 330 core\n"
    "in vec3 a_pos;\n"
    "in vec2 a_tex;\n"
    "in vec4 a_col;\n"
    "uniform mat4 u_proj;\n"
    "uniform vec3 u_trans;\n"
    "uniform vec3 u_scale;\n"
    "out vec2 v_tex;\n"
    "out vec4 v_col;\n"
    "void main()\n"
    "{\n"
    "    v_tex = a_tex;\n"
    "    v_col = a_col;\n"
    "    gl_Position = u_proj * vec4(a_pos * u_scale + u_trans, 1.0);\n"
    "}\n";

const char *_fragment_shader_src =
    "#version 330 core\n"
    "in vec2 v_tex;\n"
    "in vec4 v_col;\n"
    "uniform sampler2D u_tex;\n"
    "uniform bool u_use_texture;\n"
    "uniform bool u_alpha_test;\n"
    "uniform float u_alpha_ref;\n"
    "out vec4 frag_colour;\n"
    "void main()\n"
    "{\n"
    "    vec4 c = v_col;\n"
    "    if (u_use_texture)\n"
    "        c *= texture(u_tex, v_tex);\n"
    // glAlphaFunc(GL_NOTEQUAL, ref): keep only fragments whose alpha != ref.
    "    if (u_alpha_test && abs(c.a - u_alpha_ref) < 0.5 / 255.0)\n"
    "        discard;\n"
    "    frag_colour = c;\n"
    "}\n";

GLuint _compile_shader(GLenum type, const char *src)
{
    GLuint shader = glc.CreateShader(type);
    glc.ShaderSource(shader, 1, &src, nullptr);
    glc.CompileShader(shader);
    GLint ok = GL_FALSE;
    glc.GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[1024] = "";
        glc.GetShaderInfoLog(shader, sizeof(log), nullptr, log);
        die("Failed to compile %s shader: %s",
            type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
    }
    return shader;
}
}
#endif // USE_GL_CORE

// TODO: if this gets big enough, pull out into opengl-utils.cc/h or sth
namespace opengl
{
    bool check_texture_size(const char *name, int width, int height)
    {
        int max_texture_size;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size);
        if (width > max_texture_size || height > max_texture_size)
        {
            mprf(MSGCH_ERROR,
                "Texture %s is bigger than maximum driver texture size "
                "(%d,%d vs. %d). Sprites from this texture will not display "
                "properly.",
                name, width, height, max_texture_size);
            return false;
        }
        return true;
    }

    static string _gl_error_to_string(GLenum e)
    {
        switch (e)
        {
        case GL_NO_ERROR:
            return "GL_NO_ERROR";
        case GL_INVALID_ENUM:
            return "GL_INVALID_ENUM";
        case GL_INVALID_VALUE:
            return "GL_INVALID_VALUE";
        case GL_INVALID_OPERATION:
            return "GL_INVALID_OPERATION";
#ifndef __ANDROID__
        case GL_INVALID_FRAMEBUFFER_OPERATION:
            return "GL_INVALID_FRAMEBUFFER_OPERATION";
#endif
        case GL_OUT_OF_MEMORY:
            return "GL_OUT_OF_MEMORY (fatal)";
        case GL_STACK_UNDERFLOW:
            return "GL_STACK_UNDERFLOW";
        case GL_STACK_OVERFLOW:
            return "GL_STACK_OVERFLOW";
        default:
            return make_stringf("Unknown OpenGL error %d", e);
        }
    }

    /**
     * Log any opengl errors to console. Will crash if a really bad one occurs.
     *
     * @return true if there were any errors.
     */
    bool flush_opengl_errors()
    {
        GLenum e = GL_NO_ERROR;
        bool fatal = false;
        bool errors = false;
        do
        {
            e = glGetError();
            if (e != GL_NO_ERROR)
            {
                errors = true;
                if (e == GL_OUT_OF_MEMORY)
                    fatal = true;
                mprf(MSGCH_ERROR, "OpenGL error %s",
                                        _gl_error_to_string(e).c_str());
            }
        } while (e != GL_NO_ERROR);
        if (fatal)
            die("Fatal OpenGL error; giving up");
        return errors;
    }
}

/////////////////////////////////////////////////////////////////////////////
// Static functions from GLStateManager

GLStateManager *glmanager = nullptr;

void GLStateManager::init()
{
    if (glmanager)
        return;

    glmanager = new OGLStateManager();
}

void GLStateManager::shutdown()
{
    delete glmanager;
    glmanager = nullptr;
}

/////////////////////////////////////////////////////////////////////////////
// Static functions from GLShapeBuffer

GLShapeBuffer *GLShapeBuffer::create(bool texture, bool colour,
                                     drawing_modes prim)
{
    return new OGLShapeBuffer(texture, colour, prim);
}

/////////////////////////////////////////////////////////////////////////////
// OGLStateManager

OGLStateManager::OGLStateManager()
{
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(0.0, 0.0, 0.0, 1.0f);
    glDepthFunc(GL_LEQUAL);

    m_window_height = 0;
#ifdef USE_GL_CORE
    init_core_profile();
#elif !defined(USE_GLES)
    // TODO: we probably can do this for GLES TOO, but maybe requires tweaks?

    // OpenGL doesn't specify what the GetProcAddress function returns
    // if the implementation does not support a function
    // (e.g. because it doesn't support the OpenGL version in question)
    // So we need to check the version before we try to get the
    // glGenerateMipmap function.
    const GLubyte* versionString = glGetString(GL_VERSION);
    if (versionString == nullptr)
    {
        mprf("Mipmap Setup: Failed to load OpenGL version.");
        return;
    }
    // We will never see 2 digit OpenGL major versions - 4.6 came out in 2016,
    // and Vulkan is carrying the torch now
    //
    // It's doubtful we'll even see an OpenGL 5.
    // But we'll be paranoid. We'll consider OpenGL 3.X - 9.X as all fine
    bool supported_first_digit = ('3' <= versionString[0]) &&
                                 (versionString[0] <= '9');
    // Anything other than X.Y would be very weird.
    // It's incredibly unlikely OpenGL 10 will ever exist.
    bool second_character_is_dot = versionString[1] == '.';
    if (!supported_first_digit || !second_character_is_dot)
    {
        mprf("Mipmap Setup: Disabled because OpenGL version: %s does not "
             "provide glGenerateMipmap.", versionString);
        return;
    }

    // We have to load the library dynamically before we can load the function
    // from the library via GetProcAddress.
    // That's how dynamic loading works.
    // It's possible the library is already loaded anyway,
    // but we're being careful here.
    if (SDL_GL_LoadLibrary(NULL) != 0)
    {
        // success == 0 for this API.
        // If we can't load it, we probably wouldn't get this far at all.
        // But just in case, we'll handle it.
        mprf("Mipmap Setup: Disabled because SDL_GL_LoadLibrary failed.");
        return;
    }

    // Because we already checked the version is higher enough,
    // SDL_GL_GetProcAddress should always get a non-null pointer back.
    // But we'll log in case this does somehow happen.
    m_mipmapFn = SDL_GL_GetProcAddress("glGenerateMipmap");
    if (m_mipmapFn == nullptr)
    {
        mprf("Mipmap Setup: Failed to load glGenerateMipmap function.");
        return;
    }
    else
    {
        mprf("Mipmap Setup: success, loaded with OpenGL version: %s",
             versionString);
    }
#else
    mprf("Mipmap Setup: skipped, not supported in this build configuration.");
#endif
}

#ifdef USE_GL_CORE
void OGLStateManager::init_core_profile()
{
    _load_core_gl();

    const GLubyte* versionString = glGetString(GL_VERSION);
    mprf("OpenGL core profile renderer, version: %s",
         versionString ? (const char *)versionString : "unknown");

    GLuint vs = _compile_shader(GL_VERTEX_SHADER, _vertex_shader_src);
    GLuint fs = _compile_shader(GL_FRAGMENT_SHADER, _fragment_shader_src);
    m_program = glc.CreateProgram();
    glc.AttachShader(m_program, vs);
    glc.AttachShader(m_program, fs);
    glc.BindAttribLocation(m_program, ATTRIB_POS, "a_pos");
    glc.BindAttribLocation(m_program, ATTRIB_TEX, "a_tex");
    glc.BindAttribLocation(m_program, ATTRIB_COL, "a_col");
    glc.LinkProgram(m_program);
    GLint ok = GL_FALSE;
    glc.GetProgramiv(m_program, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        char log[1024] = "";
        glc.GetProgramInfoLog(m_program, sizeof(log), nullptr, log);
        die("Failed to link shader program: %s", log);
    }
    glc.DeleteShader(vs);
    glc.DeleteShader(fs);

    // The program is the only one Crawl uses, so it stays bound.
    glc.UseProgram(m_program);
    m_loc_proj = glc.GetUniformLocation(m_program, "u_proj");
    m_loc_trans = glc.GetUniformLocation(m_program, "u_trans");
    m_loc_scale = glc.GetUniformLocation(m_program, "u_scale");
    m_loc_use_texture = glc.GetUniformLocation(m_program, "u_use_texture");
    m_loc_alpha_test = glc.GetUniformLocation(m_program, "u_alpha_test");
    m_loc_alpha_ref = glc.GetUniformLocation(m_program, "u_alpha_ref");
    glc.Uniform1i(glc.GetUniformLocation(m_program, "u_tex"), 0);
    glc.Uniform1i(m_loc_use_texture, 0);
    glc.Uniform1i(m_loc_alpha_test, 0);
    glc.Uniform1f(m_loc_alpha_ref, 0.0f);

    for (float &f : m_proj)
        f = 0.0f;
    m_proj[0] = m_proj[5] = m_proj[10] = m_proj[15] = 1.0f;
    glc.UniformMatrix4fv(m_loc_proj, 1, GL_FALSE, m_proj);
    upload_transform();

    // Core profiles require a vertex array object and buffer objects; the
    // same streaming buffers are reused for every draw.
    glc.GenVertexArrays(1, &m_vao);
    glc.BindVertexArray(m_vao);
    GLuint bufs[4];
    glc.GenBuffers(4, bufs);
    m_vbo_pos = bufs[0];
    m_vbo_tex = bufs[1];
    m_vbo_col = bufs[2];
    m_ibo = bufs[3];
    glc.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ibo);

    glDebug("init_core_profile");
}

void OGLStateManager::upload_transform()
{
    glc.Uniform3f(m_loc_trans, m_trans.x, m_trans.y, m_trans.z);
    glc.Uniform3f(m_loc_scale, m_scale.x, m_scale.y, m_scale.z);
}

void OGLStateManager::draw_stream(drawing_modes prim,
                                  const vector<GLW_3VF> &pos,
                                  const vector<GLW_2VF> *tex,
                                  const vector<VColour> *col,
                                  const vector<unsigned short int> &ind)
{
    glc.BindBuffer(GL_ARRAY_BUFFER, m_vbo_pos);
    glc.BufferData(GL_ARRAY_BUFFER, pos.size() * sizeof(GLW_3VF), pos.data(),
                   GL_STREAM_DRAW);
    glc.VertexAttribPointer(ATTRIB_POS, 3, GL_FLOAT, GL_FALSE,
                            sizeof(GLW_3VF), nullptr);
    glc.EnableVertexAttribArray(ATTRIB_POS);

    if (tex)
    {
        glc.BindBuffer(GL_ARRAY_BUFFER, m_vbo_tex);
        glc.BufferData(GL_ARRAY_BUFFER, tex->size() * sizeof(GLW_2VF),
                       tex->data(), GL_STREAM_DRAW);
        glc.VertexAttribPointer(ATTRIB_TEX, 2, GL_FLOAT, GL_FALSE,
                                sizeof(GLW_2VF), nullptr);
        glc.EnableVertexAttribArray(ATTRIB_TEX);
    }
    else
    {
        glc.DisableVertexAttribArray(ATTRIB_TEX);
        glc.VertexAttrib2f(ATTRIB_TEX, 0.0f, 0.0f);
    }

    if (col)
    {
        glc.BindBuffer(GL_ARRAY_BUFFER, m_vbo_col);
        glc.BufferData(GL_ARRAY_BUFFER, col->size() * sizeof(VColour),
                       col->data(), GL_STREAM_DRAW);
        glc.VertexAttribPointer(ATTRIB_COL, 4, GL_UNSIGNED_BYTE, GL_TRUE,
                                sizeof(VColour), nullptr);
        glc.EnableVertexAttribArray(ATTRIB_COL);
    }
    else
    {
        // Equivalent of glColor4f with the colour array disabled.
        const VColour &c = m_current_state.colour;
        glc.DisableVertexAttribArray(ATTRIB_COL);
        glc.VertexAttrib4f(ATTRIB_COL, c.r / 255.0f, c.g / 255.0f,
                           c.b / 255.0f, c.a / 255.0f);
    }

    switch (prim)
    {
    case GLW_RECTANGLE:
        glc.BufferData(GL_ELEMENT_ARRAY_BUFFER,
                       ind.size() * sizeof(unsigned short int), ind.data(),
                       GL_STREAM_DRAW);
        glDrawElements(GL_TRIANGLE_STRIP, ind.size(), GL_UNSIGNED_SHORT,
                       nullptr);
        break;
    case GLW_LINES:
        glDrawArrays(GL_LINES, 0, pos.size());
        break;
    default:
        die("Invalid primitive type");
        break;
    }
    glDebug("draw_stream");
}
#endif // USE_GL_CORE

void OGLStateManager::set(const GLState& state)
{
#ifdef USE_GL_CORE
    // Vertex arrays are enabled per draw in draw_stream(), and the constant
    // colour is applied there too.
    if (state.texture != m_current_state.texture)
        glc.Uniform1i(m_loc_use_texture, state.texture ? 1 : 0);

    if (state.alphatest != m_current_state.alphatest
        || state.alpharef != m_current_state.alpharef)
    {
        glc.Uniform1i(m_loc_alpha_test, state.alphatest ? 1 : 0);
        // glAlphaFunc takes a clamped float, and alpharef was passed to it
        // directly, so any nonzero value acted as 1.0.
        glc.Uniform1f(m_loc_alpha_ref, min<float>(state.alpharef, 1.0f));
    }

    if (state.blend != m_current_state.blend)
    {
        if (state.blend)
            glEnable(GL_BLEND);
        else
            glDisable(GL_BLEND);
        glDebug("glEnable/glDisable(GL_BLEND)");
    }

    if (state.depthtest != m_current_state.depthtest)
    {
        if (state.depthtest)
            glEnable(GL_DEPTH_TEST);
        else
            glDisable(GL_DEPTH_TEST);
        glDebug("glEnable/glDisable(GL_DEPTH_TEST)");
    }

    m_current_state = state;
#else

    if (state.array_vertex != m_current_state.array_vertex)
    {
        if (state.array_vertex)
        {
            glEnableClientState(GL_VERTEX_ARRAY);
            glDebug("glEnableClientState(GL_VERTEX_ARRAY)");
        }
        else
        {
            glDisableClientState(GL_VERTEX_ARRAY);
            glDebug("glDisableClientState(GL_VERTEX_ARRAY)");
        }
    }

    if (state.array_texcoord != m_current_state.array_texcoord)
    {
        if (state.array_texcoord)
        {
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            glDebug("glEnableClientState(GL_TEXTURE_COORD_ARRAY)");
        }
        else
        {
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);
            glDebug("glDisableClientState(GL_TEXTURE_COORD_ARRAY)");
        }
    }

    if (state.array_colour != m_current_state.array_colour)
    {
        if (state.array_colour)
        {
            glEnableClientState(GL_COLOR_ARRAY);
            glDebug("glEnableClientState(GL_COLOR_ARRAY)");
        }
        else
        {
            glDisableClientState(GL_COLOR_ARRAY);
            glDebug("glDisableClientState(GL_COLOR_ARRAY)");

            // [enne] This should *not* be necessary, but the Linux OpenGL
            // driver that I'm using sets this to the last colour of the
            // colour array. So, we need to unset it here.
            glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
            glDebug("glColor4f(1.0f, 1.0f, 1.0f, 1.0f)");
        }
    }

    if (state.texture != m_current_state.texture)
    {
        if (state.texture)
        {
            glEnable(GL_TEXTURE_2D);
            glDebug("glEnable(GL_TEXTURE_2D)");
        }
        else
        {
            glDisable(GL_TEXTURE_2D);
            glDebug("glDisable(GL_TEXTURE_2D)");
        }
    }

    if (state.blend != m_current_state.blend)
    {
        if (state.blend)
        {
            glEnable(GL_BLEND);
            glDebug("glEnable(GL_BLEND)");
        }
        else
        {
            glDisable(GL_BLEND);
            glDebug("glDisable(GL_BLEND)");
        }
    }

    if (state.depthtest != m_current_state.depthtest)
    {
        if (state.depthtest)
        {
            glEnable(GL_DEPTH_TEST);
            glDebug("glEnable(GL_DEPTH_TEST)");
        }
        else
        {
            glDisable(GL_DEPTH_TEST);
            glDebug("glEnable(GL_DEPTH_TEST)");
        }
    }

    if (state.alphatest != m_current_state.alphatest
        || state.alpharef != m_current_state.alpharef)
    {
        if (state.alphatest)
        {
            glEnable(GL_ALPHA_TEST);
            glAlphaFunc(GL_NOTEQUAL, state.alpharef);
            glDebug("glAlphaFunc(GL_NOTEQUAL, state.alpharef)");
        }
        else
        {
            glDisable(GL_ALPHA_TEST);
            glDebug("glDisable(GL_ALPHA_TEST)");
        }
    }

    if (state.colour != m_current_state.colour)
    {
        glColor4f(state.colour.r, state.colour.g,
                  state.colour.b, state.colour.a);
        glDebug("glColor4f");
    }

    m_current_state = state;
#endif
}

struct {
    GLW_3VF trans, scale;
} current_transform;

void OGLStateManager::set_transform(const GLW_3VF &trans, const GLW_3VF &scale)
{
#ifdef USE_GL_CORE
    m_trans = trans;
    m_scale = scale;
    upload_transform();
    current_transform = { trans, scale };
#else
    glLoadIdentity();
    glTranslatef(trans.x, trans.y, trans.z);
    glScalef(scale.x, scale.y, scale.z);
    current_transform = { trans, scale };
#endif
}

void OGLStateManager::reset_transform()
{
    set_transform({0,0,0}, {1,1,1});
}

void OGLStateManager::get_transform(GLW_3VF *trans, GLW_3VF *scale)
{
    if (trans)
        *trans = current_transform.trans;
    if (scale)
        *scale = current_transform.scale;
}

int OGLStateManager::logical_to_device(int n) const
{
    return display_density.logical_to_device(n);
}

int OGLStateManager::device_to_logical(int n, bool round) const
{
    return display_density.device_to_logical(n, round);
}

void OGLStateManager::set_scissor(int x, int y, unsigned int w, unsigned int h)
{
    glEnable(GL_SCISSOR_TEST);
    glScissor(logical_to_device(x), logical_to_device(m_window_height-y-h),
                logical_to_device(w), logical_to_device(h));
}

void OGLStateManager::reset_scissor()
{
    glDisable(GL_SCISSOR_TEST);
}

void OGLStateManager::reset_view_for_resize(const coord_def &m_windowsz,
                                            const coord_def &m_drawablesz)
{
    glViewport(0, 0, m_drawablesz.x, m_drawablesz.y);
    m_window_height = m_windowsz.y;

#ifdef USE_GL_CORE
    // glOrtho(0, w, h, 0, -1000, 1000), column-major.
    const float l = 0, r = m_windowsz.x, b = m_windowsz.y, t = 0;
    const float n = -1000, f = 1000;
    for (float &v : m_proj)
        v = 0.0f;
    m_proj[0] = 2.0f / (r - l);
    m_proj[5] = 2.0f / (t - b);
    m_proj[10] = -2.0f / (f - n);
    m_proj[12] = -(r + l) / (r - l);
    m_proj[13] = -(t + b) / (t - b);
    m_proj[14] = -(f + n) / (f - n);
    m_proj[15] = 1.0f;
    glc.UniformMatrix4fv(m_loc_proj, 1, GL_FALSE, m_proj);
    glDebug("reset_view_for_resize");
#else

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();

    // For ease, vertex positions are pixel positions.
#ifdef USE_GLES
# ifdef __ANDROID__
    glOrthof(0, m_windowsz.x, m_windowsz.y, 0, -1000, 1000);
# else
    glOrthox(0, m_windowsz.x, m_windowsz.y, 0, -1000, 1000);
# endif
#else
    glOrtho(0, m_windowsz.x, m_windowsz.y, 0, -1000, 1000);
#endif
    glDebug("glOrthof");
#endif
}

void OGLStateManager::pixelstore_unpack_alignment(unsigned int bpp)
{
    glPixelStorei(GL_UNPACK_ALIGNMENT, bpp);
    glDebug("glPixelStorei");
}

void OGLStateManager::delete_textures(size_t count, unsigned int *textures)
{
    glDeleteTextures(count, (GLuint*)textures);
    glDebug("glDeleteTextures");
}

void OGLStateManager::generate_textures(size_t count, unsigned int *textures)
{
    glGenTextures(count, (GLuint*)textures);
    glDebug("glGenTextures");
}

void OGLStateManager::bind_texture(unsigned int texture)
{
    glBindTexture(GL_TEXTURE_2D, texture);
    glDebug("glBindTexture");
}

void OGLStateManager::load_texture(unsigned char *pixels, unsigned int width,
                                   unsigned int height, MipMapOptions mip_opt,
                                   int xoffset, int yoffset)
{
    // Assumptions...
#ifdef __ANDROID__
    const GLenum bpp = GL_RGBA;
#else
    const unsigned int bpp = 4;
#endif
    const GLenum texture_format = GL_RGBA;
    const GLenum format = GL_UNSIGNED_BYTE;
    // Also assume that the texture is already bound using bind_texture

#ifdef USE_GL_CORE
    UNUSED(bpp);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    const GLint filter = Options.tile_filter_scaling ? GL_LINEAR : GL_NEAREST;
    const bool mipmap = mip_opt == MIPMAP_CREATE
                        && !(xoffset >= 0 && yoffset >= 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    mipmap ? GL_LINEAR_MIPMAP_NEAREST : filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    if (xoffset >= 0 && yoffset >= 0)
    {
        glTexSubImage2D(GL_TEXTURE_2D, 0, xoffset, yoffset, width, height,
                        texture_format, format, pixels);
    }
    else
    {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
                     texture_format, format, pixels);
    }
    if (mipmap)
        glc.GenerateMipmap(GL_TEXTURE_2D);
    glDebug("load_texture");
#else

    glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glDebug("glTexEnvf");

#ifdef GL_CLAMP
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
#else
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glDebug("glTexParameterf GL_TEXTURE_WRAP_S");
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glDebug("glTexParameterf GL_TEXTURE_WRAP_T");
#endif
#ifndef USE_GLES
    if (mip_opt == MIPMAP_CREATE)
    {
        // TODO: should min react to Options.tile_filter_scaling?
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                        m_mipmapFn != nullptr ? GL_LINEAR_MIPMAP_NEAREST :
                        Options.tile_filter_scaling ? GL_LINEAR :
                        GL_NEAREST);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                        Options.tile_filter_scaling ? GL_LINEAR : GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, bpp, width, height, 0,
                     texture_format, format, pixels);
        // TODO: possibly restructure this into the main block below
        // so that we support mipmapping when glTexSubImage2D should be called.
        if (m_mipmapFn != nullptr)
        {
            PFNGLGENERATEMIPMAPPROC mipmapFn =
                    reinterpret_cast<PFNGLGENERATEMIPMAPPROC>(m_mipmapFn);
            mipmapFn(GL_TEXTURE_2D);
        }
    }
    else
#endif
    {
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                        Options.tile_filter_scaling ? GL_LINEAR : GL_NEAREST);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                        Options.tile_filter_scaling ? GL_LINEAR : GL_NEAREST);
        if (xoffset >= 0 && yoffset >= 0)
        {
            glTexSubImage2D(GL_TEXTURE_2D, 0, xoffset, yoffset, width, height,
                         texture_format, format, pixels);
            glDebug("glTexSubImage2D");
        }
        else
        {
            glTexImage2D(GL_TEXTURE_2D, 0, bpp, width, height, 0,
                         texture_format, format, pixels);
            glDebug("glTexImage2D");
        }
    }
#endif
}

void OGLStateManager::reset_view_for_redraw()
{
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
#ifdef USE_GL_CORE
    m_trans = GLW_3VF(0.0f, 0.0f, 1.0f);
    m_scale = GLW_3VF(1.0f, 1.0f, 1.0f);
    upload_transform();
    glDebug("reset_view_for_redraw");
#else
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glTranslatef(0.0f, 0.0f, 1.0f);
    glDebug("glTranslatef");
#endif
}

bool OGLStateManager::glDebug(const char* msg) const
{
#if defined(__ANDROID__) || defined(DEBUG_DIAGNOSTICS)
    int e = glGetError();
    if (e > 0)
    {
# ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_INFO, "Crawl.gl", "ERROR %x: %s", e, msg);
# else
        fprintf(stderr, "OGLStateManager ERROR %x: %s\n", e, msg);
# endif
        return true;
    }
#else
    UNUSED(msg);
#endif
    return false;
}

/////////////////////////////////////////////////////////////////////////////
// OGLShapeBuffer

OGLShapeBuffer::OGLShapeBuffer(bool texture, bool colour, drawing_modes prim) :
    m_prim_type(prim),
    m_texture_verts(texture),
    m_colour_verts(colour)
{
    ASSERT(prim == GLW_RECTANGLE || prim == GLW_LINES);
}

const char *OGLShapeBuffer::print_statistics() const
{
    return nullptr;
}

unsigned int OGLShapeBuffer::size() const
{
    return m_position_buffer.size();
}

void OGLShapeBuffer::add(const GLWPrim &rect)
{
    switch (m_prim_type)
    {
    case GLW_RECTANGLE:
        add_rect(rect);
        break;
    case GLW_LINES:
        add_line(rect);
        break;
    default:
        die("Invalid primitive type");
        break;
    }
}

void OGLShapeBuffer::add_rect(const GLWPrim &rect)
{
    // Copy vert positions
    size_t last = m_position_buffer.size();
    m_position_buffer.resize(last + 4);
    m_position_buffer[last    ].set(rect.pos_sx, rect.pos_sy, rect.pos_z);
    m_position_buffer[last + 1].set(rect.pos_sx, rect.pos_ey, rect.pos_z);
    m_position_buffer[last + 2].set(rect.pos_ex, rect.pos_sy, rect.pos_z);
    m_position_buffer[last + 3].set(rect.pos_ex, rect.pos_ey, rect.pos_z);

    // Copy texture coords if necessary
    if (m_texture_verts)
    {
        last = m_texture_buffer.size();
        m_texture_buffer.resize(last + 4);
        m_texture_buffer[last    ].set(rect.tex_sx, rect.tex_sy);
        m_texture_buffer[last + 1].set(rect.tex_sx, rect.tex_ey);
        m_texture_buffer[last + 2].set(rect.tex_ex, rect.tex_sy);
        m_texture_buffer[last + 3].set(rect.tex_ex, rect.tex_ey);
    }

    // Copy vert colours if necessary
    if (m_colour_verts)
    {
        last = m_colour_buffer.size();
        m_colour_buffer.resize(last + 4);
        m_colour_buffer[last    ].set(rect.col_s);
        m_colour_buffer[last + 1].set(rect.col_e);
        m_colour_buffer[last + 2].set(rect.col_s);
        m_colour_buffer[last + 3].set(rect.col_e);
    }

    // build indices
    last = m_ind_buffer.size();

    if (last > 3)
    {
        // This is not the first box so make FOUR degenerate triangles
        m_ind_buffer.resize(last + 6);
        unsigned short int val = m_ind_buffer[last - 1];

        // the first three degens finish the previous box and move
        // to the first position of the new one we just added and
        // the fourth degen creates a triangle that is a line from p1 to p3
        m_ind_buffer[last    ] = val++;
        m_ind_buffer[last + 1] = val;

        // Now add as normal
        m_ind_buffer[last + 2] = val++;
        m_ind_buffer[last + 3] = val++;
        m_ind_buffer[last + 4] = val++;
        m_ind_buffer[last + 5] = val;
    }
    else
    {
        // This is the first box so don't bother making any degenerate triangles
        m_ind_buffer.resize(last + 4);
        m_ind_buffer[0] = 0;
        m_ind_buffer[1] = 1;
        m_ind_buffer[2] = 2;
        m_ind_buffer[3] = 3;
    }
}

void OGLShapeBuffer::add_line(const GLWPrim &rect)
{
    // Copy vert positions
    size_t last = m_position_buffer.size();
    m_position_buffer.resize(last + 2);
    m_position_buffer[last    ].set(rect.pos_sx, rect.pos_sy, rect.pos_z);
    m_position_buffer[last + 1].set(rect.pos_ex, rect.pos_ey, rect.pos_z);

    // Copy texture coords if necessary
    if (m_texture_verts)
    {
        last = m_texture_buffer.size();
        m_texture_buffer.resize(last + 2);
        m_texture_buffer[last    ].set(rect.tex_sx, rect.tex_sy);
        m_texture_buffer[last + 1].set(rect.tex_ex, rect.tex_ey);
    }

    // Copy vert colours if necessary
    if (m_colour_verts)
    {
        last = m_colour_buffer.size();
        m_colour_buffer.resize(last + 2);
        m_colour_buffer[last    ].set(rect.col_s);
        m_colour_buffer[last + 1].set(rect.col_e);
    }
}

// Draw the buffer
void OGLShapeBuffer::draw(const GLState &state)
{
    if (m_position_buffer.empty())
        return;

    if (!state.array_vertex)
        return;

    glmanager->set(state);

#ifdef USE_GL_CORE
    static_cast<OGLStateManager *>(glmanager)->draw_stream(
        m_prim_type, m_position_buffer,
        state.array_texcoord && m_texture_verts ? &m_texture_buffer : nullptr,
        state.array_colour && m_colour_verts ? &m_colour_buffer : nullptr,
        m_ind_buffer);
#else

    glVertexPointer(3, GL_FLOAT, 0, &m_position_buffer[0]);
    glDebug("glVertexPointer");

    if (state.array_texcoord && m_texture_verts)
        glTexCoordPointer(2, GL_FLOAT, 0, &m_texture_buffer[0]);
    glDebug("glTexCoordPointer");

    if (state.array_colour && m_colour_verts)
        glColorPointer(4, GL_UNSIGNED_BYTE, 0, &m_colour_buffer[0]);
    glDebug("glColorPointer");

    switch (m_prim_type)
    {
    case GLW_RECTANGLE:
        glDrawElements(GL_TRIANGLE_STRIP, m_ind_buffer.size(),
                       GL_UNSIGNED_SHORT, &m_ind_buffer[0]);
        break;
    case GLW_LINES:
        glDrawArrays(GL_LINES, 0, m_position_buffer.size());
        break;
    default:
        die("Invalid primitive type");
        break;
    }
    glDebug("glDrawElements");
#endif
}

void OGLShapeBuffer::clear()
{
    m_position_buffer.clear();
    m_ind_buffer.clear();
    m_texture_buffer.clear();
    m_colour_buffer.clear();
}

bool OGLShapeBuffer::glDebug(const char* msg) const
{
#if defined(__ANDROID__) || defined(DEBUG_DIAGNOSTICS)
    int e = glGetError();
    if (e > 0)
    {
# ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_INFO, "Crawl.gl", "ERROR %x: %s", e, msg);
# else
        fprintf(stderr, "OGLShapeBuffer ERROR %x: %s\n", e, msg);
# endif
        return true;
    }
#else
    UNUSED(msg);
#endif
    return false;
}

#endif // USE_GL
#endif // USE_TILE_LOCAL
