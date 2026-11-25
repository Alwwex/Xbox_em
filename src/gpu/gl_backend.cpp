#include "gpu/gl_backend.h"

#include <dlfcn.h>
#include <sys/stat.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/log.h"
#include "gpu/gl_api.h"

namespace xbem::gpu {
namespace {

// ------------------------------------------------------------------- EGL

using EGLDisplay = void*;
using EGLConfig = void*;
using EGLContext = void*;
using EGLSurface = void*;
using EGLint = int32_t;
using EGLBoolean = unsigned;

constexpr EGLint EGL_NONE = 0x3038, EGL_SURFACE_TYPE = 0x3033, EGL_WINDOW_BIT = 0x0004, EGL_PBUFFER_BIT = 0x0001,
                 EGL_RENDERABLE_TYPE = 0x3040, EGL_OPENGL_BIT = 0x0008, EGL_RED_SIZE = 0x3024,
                 EGL_GREEN_SIZE = 0x3023, EGL_BLUE_SIZE = 0x3022, EGL_CONTEXT_MAJOR_VERSION = 0x3098,
                 EGL_CONTEXT_MINOR_VERSION = 0x30FB, EGL_CONTEXT_OPENGL_PROFILE_MASK = 0x30FD;
constexpr unsigned EGL_OPENGL_API = 0x30A2;

struct Egl {
  EGLDisplay (*GetDisplay)(void*);
  EGLBoolean (*Initialize)(EGLDisplay, EGLint*, EGLint*);
  EGLBoolean (*ChooseConfig)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*);
  EGLBoolean (*BindAPI)(unsigned);
  EGLContext (*CreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint*);
  EGLSurface (*CreateWindowSurface)(EGLDisplay, EGLConfig, unsigned long, const EGLint*);
  EGLBoolean (*MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
  EGLBoolean (*SwapBuffers)(EGLDisplay, EGLSurface);
  EGLBoolean (*SwapInterval)(EGLDisplay, EGLint);
  void* (*GetProcAddress)(const char*);
  EGLint (*GetError)();
};

GlApi g_gl{};
std::atomic<bool> g_active{false};

struct GpuThread {
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<std::function<void()>> jobs;
  // The newest frame to present (older ones are dropped).
  std::vector<uint32_t> frame;
  int frame_width = 0, frame_height = 0;
  bool frame_pending = false;
};
GpuThread g_thread;
std::mutex g_screenshot_mutex;
std::string g_screenshot_path;

void save_screenshot(int width, int height) {
  std::string path;
  {
    std::lock_guard lock(g_screenshot_mutex);
    path.swap(g_screenshot_path);
  }
  if (path.empty()) return;
  std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3);
  g_gl.ReadPixels(0, 0, width, height, 0x1907 /*GL_RGB*/, GL_UNSIGNED_BYTE, rgb.data());
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  std::fprintf(f, "P6\n%d %d\n255\n", width, height);
  for (int y = height - 1; y >= 0; --y) std::fwrite(rgb.data() + static_cast<size_t>(y) * width * 3, 1, width * 3, f);
  std::fclose(f);
  XBEM_INFO("gpu", "screenshot of the presented frame: {}", path);
}

template <class F>
bool load(void* lib, F& f, const char* name) {
  f = reinterpret_cast<F>(dlsym(lib, name));
  return f != nullptr;
}

bool load_egl(Egl& e, std::string& reason) {
  void* lib = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!lib) {
    reason = "libEGL.so.1 is not installed (sudo apt install libegl1 libegl-mesa0)";
    return false;
  }
  bool ok = load(lib, e.GetDisplay, "eglGetDisplay") & load(lib, e.Initialize, "eglInitialize") &
            load(lib, e.ChooseConfig, "eglChooseConfig") & load(lib, e.BindAPI, "eglBindAPI") &
            load(lib, e.CreateContext, "eglCreateContext") &
            load(lib, e.CreateWindowSurface, "eglCreateWindowSurface") &
            load(lib, e.MakeCurrent, "eglMakeCurrent") & load(lib, e.SwapBuffers, "eglSwapBuffers") &
            load(lib, e.SwapInterval, "eglSwapInterval") & load(lib, e.GetProcAddress, "eglGetProcAddress") &
            load(lib, e.GetError, "eglGetError");
  if (!ok) reason = "libEGL.so.1 lacks EGL 1.4 functions";
  return ok;
}

bool load_gl(const Egl& e, std::string& reason) {
  bool ok = true;
  const auto get = [&](auto& f, const char* name) {
    f = reinterpret_cast<std::remove_reference_t<decltype(f)>>(e.GetProcAddress(name));
    if (!f) {
      ok = false;
      reason = std::format("OpenGL function {} is missing", name);
    }
  };
  get(g_gl.GetString, "glGetString");
  get(g_gl.GetError, "glGetError");
  get(g_gl.Viewport, "glViewport");
  get(g_gl.ClearColor, "glClearColor");
  get(g_gl.Clear, "glClear");
  get(g_gl.GenTextures, "glGenTextures");
  get(g_gl.DeleteTextures, "glDeleteTextures");
  get(g_gl.BindTexture, "glBindTexture");
  get(g_gl.TexParameteri, "glTexParameteri");
  get(g_gl.TexImage2D, "glTexImage2D");
  get(g_gl.TexSubImage2D, "glTexSubImage2D");
  get(g_gl.CreateShader, "glCreateShader");
  get(g_gl.ShaderSource, "glShaderSource");
  get(g_gl.CompileShader, "glCompileShader");
  get(g_gl.GetShaderiv, "glGetShaderiv");
  get(g_gl.GetShaderInfoLog, "glGetShaderInfoLog");
  get(g_gl.DeleteShader, "glDeleteShader");
  get(g_gl.CreateProgram, "glCreateProgram");
  get(g_gl.AttachShader, "glAttachShader");
  get(g_gl.LinkProgram, "glLinkProgram");
  get(g_gl.GetProgramiv, "glGetProgramiv");
  get(g_gl.GetProgramInfoLog, "glGetProgramInfoLog");
  get(g_gl.UseProgram, "glUseProgram");
  get(g_gl.GenVertexArrays, "glGenVertexArrays");
  get(g_gl.BindVertexArray, "glBindVertexArray");
  get(g_gl.DrawArrays, "glDrawArrays");
  get(g_gl.ActiveTexture, "glActiveTexture");
  get(g_gl.GetUniformLocation, "glGetUniformLocation");
  get(g_gl.Uniform1i, "glUniform1i");
  get(g_gl.Finish, "glFinish");
  get(g_gl.ReadPixels, "glReadPixels");
  return ok;
}

// The presentation pass: a fullscreen triangle sampling the frame texture.
constexpr const char* kPresentVertex = R"(#version 450 core
out vec2 uv;
void main() {
  vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
  uv = vec2(p.x, 1.0 - p.y);
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
})";
constexpr const char* kPresentFragment = R"(#version 450 core
in vec2 uv;
out vec4 color;
uniform sampler2D frame;
void main() { color = vec4(texture(frame, uv).rgb, 1.0); })";

void gpu_thread_main(Egl egl, EGLDisplay display, EGLSurface surface, EGLContext context, int width, int height) {
  egl.MakeCurrent(display, surface, surface, context);
  egl.SwapInterval(display, 0);
  std::string log;
  const GLuint program = gl_program(kPresentVertex, kPresentFragment, &log);
  if (!program) XBEM_ERROR("gpu", "presentation shaders: {}", log);
  GLuint vao = 0, texture = 0;
  g_gl.GenVertexArrays(1, &vao);
  g_gl.GenTextures(1, &texture);
  g_gl.BindTexture(GL_TEXTURE_2D, texture);
  g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  int tex_w = 0, tex_h = 0;
  std::vector<uint32_t> frame;
  for (;;) {
    std::deque<std::function<void()>> jobs;
    bool present = false;
    int fw = 0, fh = 0;
    {
      std::unique_lock lock(g_thread.mutex);
      g_thread.cv.wait(lock, [] { return !g_thread.jobs.empty() || g_thread.frame_pending; });
      jobs.swap(g_thread.jobs);
      if (g_thread.frame_pending) {
        frame.swap(g_thread.frame);
        fw = g_thread.frame_width;
        fh = g_thread.frame_height;
        g_thread.frame_pending = false;
        present = true;
      }
    }
    for (auto& job : jobs) job();
    if (!present || !program) continue;
    g_gl.ActiveTexture(0x84C0);  // GL_TEXTURE0
    g_gl.BindTexture(GL_TEXTURE_2D, texture);
    if (fw != tex_w || fh != tex_h) {
      g_gl.TexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(GL_RGBA8), fw, fh, 0, GL_BGRA, GL_UNSIGNED_BYTE,
                      frame.data());
      tex_w = fw;
      tex_h = fh;
    } else {
      g_gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, fw, fh, GL_BGRA, GL_UNSIGNED_BYTE, frame.data());
    }
    g_gl.Viewport(0, 0, width, height);
    g_gl.UseProgram(program);
    g_gl.Uniform1i(g_gl.GetUniformLocation(program, "frame"), 0);
    g_gl.BindVertexArray(vao);
    g_gl.DrawArrays(GL_TRIANGLES, 0, 3);
    save_screenshot(width, height);
    egl.SwapBuffers(display, surface);
  }
}

}  // namespace

const GlApi& gl() { return g_gl; }

GLuint gl_program(const char* vertex, const char* fragment, std::string* log) {
  const auto compile = [&](GLenum type, const char* source) -> GLuint {
    const GLuint shader = g_gl.CreateShader(type);
    g_gl.ShaderSource(shader, 1, &source, nullptr);
    g_gl.CompileShader(shader);
    GLint ok = 0;
    g_gl.GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
      char text[4096] = {};
      g_gl.GetShaderInfoLog(shader, sizeof text, nullptr, text);
      if (log) *log += text;
      g_gl.DeleteShader(shader);
      return 0;
    }
    return shader;
  };
  const GLuint vs = compile(GL_VERTEX_SHADER, vertex), fs = compile(GL_FRAGMENT_SHADER, fragment);
  if (!vs || !fs) return 0;
  const GLuint program = g_gl.CreateProgram();
  g_gl.AttachShader(program, vs);
  g_gl.AttachShader(program, fs);
  g_gl.LinkProgram(program);
  g_gl.DeleteShader(vs);
  g_gl.DeleteShader(fs);
  GLint ok = 0;
  g_gl.GetProgramiv(program, GL_LINK_STATUS, &ok);
  if (!ok) {
    char text[4096] = {};
    g_gl.GetProgramInfoLog(program, sizeof text, nullptr, text);
    if (log) *log += text;
    return 0;
  }
  return program;
}

bool gl_start(unsigned long xid, int width, int height, std::string& info) {
  if (const char* off = std::getenv("XBEM_NO_GPU"); off && *off) {
    info = "XBEM_NO_GPU is set";
    return false;
  }
  // WSL2: OpenGL on the Windows GPU driver goes through Mesa's d3d12 driver;
  // without this Mesa picks llvmpipe (software) for an X11 display.
  struct stat st {};
  if (stat("/dev/dxg", &st) == 0 && !std::getenv("GALLIUM_DRIVER")) setenv("GALLIUM_DRIVER", "d3d12", 0);
  Egl egl{};
  if (!load_egl(egl, info)) return false;
  // EGL gets its own X connection; the window belongs to the run window's.
  void* x11 = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
  void* (*open_display)(const char*) = x11 ? reinterpret_cast<void* (*)(const char*)>(dlsym(x11, "XOpenDisplay")) : nullptr;
  void* native = open_display ? open_display(nullptr) : nullptr;
  if (!native) {
    info = "cannot open an X connection for EGL";
    return false;
  }
  EGLDisplay display = egl.GetDisplay(native);
  EGLint major = 0, minor = 0;
  if (!display || !egl.Initialize(display, &major, &minor)) {
    info = std::format("eglInitialize failed ({:#x})", egl.GetError());
    return false;
  }
  const EGLint config_attrs[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                                 EGL_RED_SIZE,     8,              EGL_GREEN_SIZE,      8,
                                 EGL_BLUE_SIZE,    8,              EGL_NONE};
  EGLConfig config = nullptr;
  EGLint count = 0;
  if (!egl.ChooseConfig(display, config_attrs, &config, 1, &count) || count < 1) {
    info = "no EGL configuration for an OpenGL window";
    return false;
  }
  egl.BindAPI(EGL_OPENGL_API);
  const EGLint context_attrs[] = {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 5,
                                  EGL_CONTEXT_OPENGL_PROFILE_MASK, 1, EGL_NONE};
  EGLContext context = egl.CreateContext(display, config, nullptr, context_attrs);
  if (!context) {
    info = std::format("no OpenGL 4.5 core context ({:#x})", egl.GetError());
    return false;
  }
  EGLSurface surface = egl.CreateWindowSurface(display, config, xid, nullptr);
  if (!surface) {
    info = std::format("eglCreateWindowSurface failed ({:#x})", egl.GetError());
    return false;
  }
  // Probe the context on this thread, then hand it to the GPU thread.
  if (!egl.MakeCurrent(display, surface, surface, context) || !load_gl(egl, info)) {
    if (info.empty()) info = "eglMakeCurrent failed";
    return false;
  }
  const auto str = [](GLenum name) {
    const unsigned char* s = g_gl.GetString(name);
    return s ? std::string(reinterpret_cast<const char*>(s)) : std::string("?");
  };
  info = std::format("OpenGL {} on {}", str(GL_VERSION), str(GL_RENDERER));
  egl.MakeCurrent(display, nullptr, nullptr, nullptr);
  std::thread(gpu_thread_main, egl, display, surface, context, width, height).detach();
  g_active.store(true);
  return true;
}

bool gl_active() { return g_active.load(); }

void gl_present_pixels(const uint32_t* pixels, int width, int height) {
  if (!g_active.load() || width <= 0 || height <= 0) return;
  {
    std::lock_guard lock(g_thread.mutex);
    g_thread.frame.assign(pixels, pixels + static_cast<size_t>(width) * height);
    g_thread.frame_width = width;
    g_thread.frame_height = height;
    g_thread.frame_pending = true;
  }
  g_thread.cv.notify_one();
}

void gl_request_screenshot(const std::string& path) {
  std::lock_guard lock(g_screenshot_mutex);
  g_screenshot_path = path;
}

void gl_run(std::function<void()> job) {
  if (!g_active.load()) return;
  {
    std::lock_guard lock(g_thread.mutex);
    g_thread.jobs.push_back(std::move(job));
  }
  g_thread.cv.notify_one();
}

}  // namespace xbem::gpu
