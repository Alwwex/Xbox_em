#pragma once

// The OpenGL 4.6 entry points and constants xbem uses, declared here
// because GL headers are not a build dependency. Loaded by gl_backend.cpp
// through eglGetProcAddress; valid on the GPU thread only.

#include <cstddef>
#include <cstdint>

namespace xbem::gpu {

using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLboolean = unsigned char;
using GLbitfield = unsigned int;
using GLfloat = float;
using GLchar = char;
using GLsizeiptr = std::ptrdiff_t;
using GLintptr = std::ptrdiff_t;

constexpr GLenum GL_VENDOR = 0x1F00, GL_RENDERER = 0x1F01, GL_VERSION = 0x1F02,
                 GL_SHADING_LANGUAGE_VERSION = 0x8B8C;
constexpr GLenum GL_TEXTURE_2D = 0x0DE1, GL_TEXTURE_MIN_FILTER = 0x2801, GL_TEXTURE_MAG_FILTER = 0x2800,
                 GL_LINEAR = 0x2601, GL_NEAREST = 0x2600, GL_TEXTURE_WRAP_S = 0x2802, GL_TEXTURE_WRAP_T = 0x2803,
                 GL_CLAMP_TO_EDGE = 0x812F, GL_RGBA8 = 0x8058, GL_BGRA = 0x80E1, GL_RGBA = 0x1908,
                 GL_UNSIGNED_BYTE = 0x1401, GL_FLOAT = 0x1406, GL_UNSIGNED_INT = 0x1405;
constexpr GLenum GL_VERTEX_SHADER = 0x8B31, GL_FRAGMENT_SHADER = 0x8B30, GL_COMPILE_STATUS = 0x8B81,
                 GL_LINK_STATUS = 0x8B82, GL_INFO_LOG_LENGTH = 0x8B84;
constexpr GLenum GL_TRIANGLES = 0x0004, GL_COLOR_BUFFER_BIT = 0x4000, GL_FRAMEBUFFER = 0x8D40,
                 GL_COLOR_ATTACHMENT0 = 0x8CE0, GL_FRAMEBUFFER_COMPLETE = 0x8CD5;

struct GlApi {
  const unsigned char* (*GetString)(GLenum);
  GLenum (*GetError)();
  void (*Viewport)(GLint, GLint, GLsizei, GLsizei);
  void (*ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
  void (*Clear)(GLbitfield);
  void (*GenTextures)(GLsizei, GLuint*);
  void (*DeleteTextures)(GLsizei, const GLuint*);
  void (*BindTexture)(GLenum, GLuint);
  void (*TexParameteri)(GLenum, GLenum, GLint);
  void (*TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
  void (*TexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*);
  GLuint (*CreateShader)(GLenum);
  void (*ShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*);
  void (*CompileShader)(GLuint);
  void (*GetShaderiv)(GLuint, GLenum, GLint*);
  void (*GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
  void (*DeleteShader)(GLuint);
  GLuint (*CreateProgram)();
  void (*AttachShader)(GLuint, GLuint);
  void (*LinkProgram)(GLuint);
  void (*GetProgramiv)(GLuint, GLenum, GLint*);
  void (*GetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
  void (*UseProgram)(GLuint);
  void (*GenVertexArrays)(GLsizei, GLuint*);
  void (*BindVertexArray)(GLuint);
  void (*DrawArrays)(GLenum, GLint, GLsizei);
  void (*ActiveTexture)(GLenum);
  GLint (*GetUniformLocation)(GLuint, const GLchar*);
  void (*Uniform1i)(GLint, GLint);
  void (*Finish)();
  void (*ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
};

// The loaded table (on the GPU thread, after gl_start succeeded).
const GlApi& gl();

// Compiles and links a vertex + fragment program; 0 and `log` on failure.
GLuint gl_program(const char* vertex, const char* fragment, std::string* log);

}  // namespace xbem::gpu
