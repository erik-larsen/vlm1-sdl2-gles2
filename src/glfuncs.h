/*
 * GLES2 entry points, loaded at runtime via SDL_GL_GetProcAddress.
 *
 * On macOS, SDL dlopens ANGLE's libEGL/libGLESv2 itself; linking those
 * dylibs directly would give us a second ANGLE instance with no current
 * context. Loading through SDL works identically under Emscripten, so
 * this is the one shared path. gl2.h is included for enums/types only.
 */
#ifndef VLM_GLFUNCS_H
#define VLM_GLFUNCS_H

#include <stdbool.h>

#include <GLES2/gl2.h>

#define GL_FUNC_LIST(X) \
    X(const GLubyte *, GetString, (GLenum)) \
    X(GLuint, CreateShader, (GLenum)) \
    X(void, ShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
    X(void, CompileShader, (GLuint)) \
    X(void, GetShaderiv, (GLuint, GLenum, GLint *)) \
    X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(GLuint, CreateProgram, (void)) \
    X(void, AttachShader, (GLuint, GLuint)) \
    X(void, LinkProgram, (GLuint)) \
    X(void, GetProgramiv, (GLuint, GLenum, GLint *)) \
    X(GLint, GetAttribLocation, (GLuint, const GLchar *)) \
    X(GLint, GetUniformLocation, (GLuint, const GLchar *)) \
    X(void, GenBuffers, (GLsizei, GLuint *)) \
    X(void, BindBuffer, (GLenum, GLuint)) \
    X(void, BufferData, (GLenum, GLsizeiptr, const void *, GLenum)) \
    X(void, GenTextures, (GLsizei, GLuint *)) \
    X(void, BindTexture, (GLenum, GLuint)) \
    X(void, TexParameteri, (GLenum, GLenum, GLint)) \
    X(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    X(void, TexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
    X(void, Viewport, (GLint, GLint, GLsizei, GLsizei)) \
    X(void, ClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, Clear, (GLbitfield)) \
    X(void, UseProgram, (GLuint)) \
    X(void, ActiveTexture, (GLenum)) \
    X(void, Uniform1i, (GLint, GLint)) \
    X(void, EnableVertexAttribArray, (GLuint)) \
    X(void, VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
    X(void, DrawArrays, (GLenum, GLint, GLsizei))

typedef struct {
#define GL_DECL(ret, name, args) ret (*name) args;
    GL_FUNC_LIST(GL_DECL)
#undef GL_DECL
} GlFuncs;

extern GlFuncs GL;

#ifndef VLM_GLFUNCS_NO_MACROS
/* Route bare gl* names through the loaded table. */
#define glGetString GL.GetString
#define glCreateShader GL.CreateShader
#define glShaderSource GL.ShaderSource
#define glCompileShader GL.CompileShader
#define glGetShaderiv GL.GetShaderiv
#define glGetShaderInfoLog GL.GetShaderInfoLog
#define glCreateProgram GL.CreateProgram
#define glAttachShader GL.AttachShader
#define glLinkProgram GL.LinkProgram
#define glGetProgramiv GL.GetProgramiv
#define glGetAttribLocation GL.GetAttribLocation
#define glGetUniformLocation GL.GetUniformLocation
#define glGenBuffers GL.GenBuffers
#define glBindBuffer GL.BindBuffer
#define glBufferData GL.BufferData
#define glGenTextures GL.GenTextures
#define glBindTexture GL.BindTexture
#define glTexParameteri GL.TexParameteri
#define glTexImage2D GL.TexImage2D
#define glTexSubImage2D GL.TexSubImage2D
#define glViewport GL.Viewport
#define glClearColor GL.ClearColor
#define glClear GL.Clear
#define glUseProgram GL.UseProgram
#define glActiveTexture GL.ActiveTexture
#define glUniform1i GL.Uniform1i
#define glEnableVertexAttribArray GL.EnableVertexAttribArray
#define glVertexAttribPointer GL.VertexAttribPointer
#define glDrawArrays GL.DrawArrays
#endif /* VLM_GLFUNCS_NO_MACROS */

/* Load all entries; returns false if any are missing. */
bool gl_load_funcs(void);

#endif
