// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#include "OpenGLShader.h"
#include "Logger.h"

// We need GL 2.0+ entry points (glCreateShader, glCompileShader, …) that
// aren't in the stock <GL/gl.h> 1.1 header on Linux/Windows. Strategy:
//   * macOS  — <OpenGL/gl3.h> declares them directly.
//   * Emscripten / WebGL2 — <GLES3/gl3.h> declares them directly.
//   * Linux / Windows — pull in PFN typedefs from <GL/glext.h> and
//                       resolve the symbols lazily via glfwGetProcAddress
//                       (GLFW is already linked everywhere POM2 runs).
//
// Keeping this self-contained avoids dragging in GLEW / GLAD just for a
// handful of shader entry points.

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

#include "Pom2GL.h"
#include <GLFW/glfw3.h>

#if POM2_GL_ES
#elif defined(__APPLE__)
#else

// Function-pointer slots for the GL 2.0+ entry points we use. Resolved
// at first call by loadEntryPoints(); zero until then.
namespace {
PFNGLCREATESHADERPROC      glCreateShader_      = nullptr;
PFNGLSHADERSOURCEPROC      glShaderSource_      = nullptr;
PFNGLCOMPILESHADERPROC     glCompileShader_     = nullptr;
PFNGLGETSHADERIVPROC       glGetShaderiv_       = nullptr;
PFNGLGETSHADERINFOLOGPROC  glGetShaderInfoLog_  = nullptr;
PFNGLDELETESHADERPROC      glDeleteShader_      = nullptr;
PFNGLCREATEPROGRAMPROC     glCreateProgram_     = nullptr;
PFNGLATTACHSHADERPROC      glAttachShader_      = nullptr;
PFNGLLINKPROGRAMPROC       glLinkProgram_       = nullptr;
PFNGLGETPROGRAMIVPROC      glGetProgramiv_      = nullptr;
PFNGLGETPROGRAMINFOLOGPROC glGetProgramInfoLog_ = nullptr;
PFNGLDELETEPROGRAMPROC     glDeleteProgram_     = nullptr;
PFNGLBINDATTRIBLOCATIONPROC glBindAttribLocation_ = nullptr;
bool entryPointsLoaded_ = false;

bool loadEntryPoints()
{
    if (entryPointsLoaded_) return true;
    auto get = [](const char* name) {
        return reinterpret_cast<void*>(glfwGetProcAddress(name));
    };
    glCreateShader_      = reinterpret_cast<PFNGLCREATESHADERPROC>     (get("glCreateShader"));
    glShaderSource_      = reinterpret_cast<PFNGLSHADERSOURCEPROC>     (get("glShaderSource"));
    glCompileShader_     = reinterpret_cast<PFNGLCOMPILESHADERPROC>    (get("glCompileShader"));
    glGetShaderiv_       = reinterpret_cast<PFNGLGETSHADERIVPROC>      (get("glGetShaderiv"));
    glGetShaderInfoLog_  = reinterpret_cast<PFNGLGETSHADERINFOLOGPROC> (get("glGetShaderInfoLog"));
    glDeleteShader_      = reinterpret_cast<PFNGLDELETESHADERPROC>     (get("glDeleteShader"));
    glCreateProgram_     = reinterpret_cast<PFNGLCREATEPROGRAMPROC>    (get("glCreateProgram"));
    glAttachShader_      = reinterpret_cast<PFNGLATTACHSHADERPROC>     (get("glAttachShader"));
    glLinkProgram_       = reinterpret_cast<PFNGLLINKPROGRAMPROC>      (get("glLinkProgram"));
    glGetProgramiv_      = reinterpret_cast<PFNGLGETPROGRAMIVPROC>     (get("glGetProgramiv"));
    glGetProgramInfoLog_ = reinterpret_cast<PFNGLGETPROGRAMINFOLOGPROC>(get("glGetProgramInfoLog"));
    glDeleteProgram_     = reinterpret_cast<PFNGLDELETEPROGRAMPROC>    (get("glDeleteProgram"));
    glBindAttribLocation_ = reinterpret_cast<PFNGLBINDATTRIBLOCATIONPROC>(get("glBindAttribLocation"));
    entryPointsLoaded_ =
        glCreateShader_ && glShaderSource_ && glCompileShader_ &&
        glGetShaderiv_ && glGetShaderInfoLog_ && glDeleteShader_ &&
        glCreateProgram_ && glAttachShader_ && glLinkProgram_ &&
        glGetProgramiv_ && glGetProgramInfoLog_ && glDeleteProgram_ &&
        glBindAttribLocation_;
    return entryPointsLoaded_;
}
} // namespace

// Map the unqualified call sites below onto the loaded slots.
#  define glCreateShader      glCreateShader_
#  define glShaderSource      glShaderSource_
#  define glCompileShader     glCompileShader_
#  define glGetShaderiv       glGetShaderiv_
#  define glGetShaderInfoLog  glGetShaderInfoLog_
#  define glDeleteShader      glDeleteShader_
#  define glCreateProgram     glCreateProgram_
#  define glAttachShader      glAttachShader_
#  define glLinkProgram       glLinkProgram_
#  define glGetProgramiv      glGetProgramiv_
#  define glGetProgramInfoLog glGetProgramInfoLog_
#  define glDeleteProgram     glDeleteProgram_
#  define glBindAttribLocation glBindAttribLocation_
#endif

namespace pom2 {

bool shaderRunningOnGLES()
{
#if POM2_GL_ES
    return true;
#else
    return false;
#endif
}

void deleteShaderProgram(unsigned int program)
{
    if (!program) return;
#if POM2_GL_ES || defined(__APPLE__)
    glDeleteProgram(program);
#else
    if (loadEntryPoints() && glDeleteProgram_) glDeleteProgram_(program);
#endif
}

static unsigned int compileOne(unsigned int kind,
                               const char* versionLine,
                               const char* precisionLine,
                               const char* body,
                               std::string* errorOut,
                               bool quiet)
{
    unsigned int sh = glCreateShader(kind);
    if (!sh) {
        if (errorOut) *errorOut = "glCreateShader returned 0";
        return 0;
    }
    const char* parts[3] = { versionLine, precisionLine, body };
    glShaderSource(sh, 3, parts, nullptr);
    glCompileShader(sh);
    int ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = {0};
        int len = 0;
        glGetShaderInfoLog(sh, sizeof(log) - 1, &len, log);
        std::string msg = "shader compile failed: ";
        msg.append(log, len);
        if (errorOut) *errorOut = msg;
        if (!quiet) pom2::log().warn("NTSC", msg);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

// One full build attempt at a given `#version`. Returns the linked program or
// 0; on failure `errorOut` carries the reason. `quiet` suppresses the log for
// the intermediate attempts of the cascade below.
static unsigned int buildAt(const char* versionLine,
                            const char* precisionLine,
                            const char* vertexBody,
                            const char* fragmentBody,
                            std::string* errorOut,
                            bool quiet)
{
    unsigned int vs = compileOne(GL_VERTEX_SHADER, versionLine, precisionLine,
                                 vertexBody, errorOut, quiet);
    if (!vs) return 0;
    unsigned int fs = compileOne(GL_FRAGMENT_SHADER, versionLine, precisionLine,
                                 fragmentBody, errorOut, quiet);
    if (!fs) { glDeleteShader(vs); return 0; }

    unsigned int prog = glCreateProgram();
    if (!prog) {
        glDeleteShader(vs);
        glDeleteShader(fs);
        if (errorOut) *errorOut = "glCreateProgram returned 0";
        return 0;
    }
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    // Pin the fullscreen-quad position attribute to location 0 before linking.
    // Under GLSL 1.50 / 3.00 es the linker may otherwise assign `aPos` any
    // generic slot, yet callers hardcode glVertexAttribPointer(0, ...).
    // Binding a name absent from a given shader is a harmless no-op.
    glBindAttribLocation(prog, 0, "aPos");
    glLinkProgram(prog);
    int ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok) {
        char log[2048] = {0};
        int len = 0;
        glGetProgramInfoLog(prog, sizeof(log) - 1, &len, log);
        std::string msg = "shader link failed: ";
        msg.append(log, len);
        if (errorOut) *errorOut = msg;
        if (!quiet) pom2::log().warn("NTSC", msg);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

unsigned int compileShaderProgram(const char* vertexBody,
                                  const char* fragmentBody,
                                  std::string* errorOut)
{
#if !POM2_GL_ES && !defined(__APPLE__)
    if (!loadEntryPoints()) {
        if (errorOut) *errorOut = "GL 3.x entry points unavailable";
        pom2::log().warn("NTSC", "GL 3.x entry points unavailable — "
                                 "OpenEmulator shader disabled");
        return 0;
    }
#endif

#if POM2_GL_ES
    // GLES 3.0 / WebGL 2 is a single well-defined tier; nothing to negotiate.
    return buildAt("#version 300 es\n",
                   "precision highp float;\nprecision highp int;\n",
                   vertexBody, fragmentBody, errorOut, /*quiet=*/false);
#elif defined(__APPLE__)
    // macOS core profiles expose exactly GLSL 1.50 for GL 3.2 core; there is
    // no lower dialect to fall back to on that stack.
    return buildAt("#version 150\n", "\n",
                   vertexBody, fragmentBody, errorOut, /*quiet=*/false);
#else
    // ── Desktop GL: negotiate the dialect instead of demanding 1.50 ───────
    //
    // `#version 150` used to be hardcoded here, and on any driver that caps
    // below it the whole effect stack died with "GLSL 1.50 is not supported.
    // Supported versions are: 1.10, 1.20, 1.30, 1.40 …". Mesa's V3D — the
    // Raspberry Pi — caps *desktop* GL at 3.1, i.e. GLSL 1.40; llvmpipe on an
    // old Mesa and several VM drivers land in the same place.
    //
    // Nothing had to be rewritten: POM2's shader bodies only use GLSL 1.30
    // constructs (`in`/`out`, `texture()`, `fwidth()`). They were merely
    // *asking* for 1.50.
    //
    // So: read what the driver advertises, start there, and walk down. The
    // cascade is a safety net rather than an affectation — a driver can
    // advertise a version and still refuse it in *this* context, and only a
    // real compile settles the question. Intermediate failures stay silent,
    // and errorOut is CLEARED on success: otherwise the panel would report
    // "shader unavailable" while the stack is up and running.
    static const struct { int id; const char* line; } kDialects[] = {
        { 150, "#version 150\n" },
        { 140, "#version 140\n" },
        { 130, "#version 130\n" },
    };

    int advertised = 0;   // e.g. 140 for "1.40"
    if (const char* s = reinterpret_cast<const char*>(
            glGetString(GL_SHADING_LANGUAGE_VERSION))) {
        int major = 0, minor = 0;
        if (std::sscanf(s, "%d.%d", &major, &minor) == 2)
            advertised = major * 100 + minor;
    }

    size_t start = 0;
    while (start < (sizeof(kDialects) / sizeof(kDialects[0])) - 1 &&
           advertised > 0 && advertised < kDialects[start].id)
        ++start;

    for (size_t i = start; i < sizeof(kDialects) / sizeof(kDialects[0]); ++i) {
        const bool last = (i + 1 == sizeof(kDialects) / sizeof(kDialects[0]));
        unsigned int prog = buildAt(kDialects[i].line, "\n",
                                    vertexBody, fragmentBody, errorOut,
                                    /*quiet=*/!last);
        if (prog) {
            if (errorOut) errorOut->clear();
            static bool announced = false;
            if (!announced) {
                announced = true;
                char msg[96];
                std::snprintf(msg, sizeof(msg), "GLSL %d (driver: %d.%02d)",
                              kDialects[i].id, advertised / 100,
                              advertised % 100);
                pom2::log().info("NTSC", msg);
            }
            return prog;
        }
    }
    return 0;
#endif
}

} // namespace pom2
