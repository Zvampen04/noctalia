#include "render/custom_effect/custom_effect_program.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif

namespace {
class HeadlessGles2 {
public:
  HeadlessGles2() {
    const auto platformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (platformDisplay != nullptr)
      m_display = platformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    if (m_display == EGL_NO_DISPLAY) m_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (m_display == EGL_NO_DISPLAY || eglInitialize(m_display, nullptr, nullptr) != EGL_TRUE)
      throw std::runtime_error("cannot initialize a headless EGL display");
    if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE)
      throw std::runtime_error("cannot bind the OpenGL ES API");

    constexpr EGLint configAttributes[]{
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE};
    EGLint count = 0;
    if (eglChooseConfig(m_display, configAttributes, &m_config, 1, &count) != EGL_TRUE || count != 1)
      throw std::runtime_error("cannot choose a headless RGBA EGL config");
    constexpr EGLint surfaceAttributes[]{EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE};
    m_surface = eglCreatePbufferSurface(m_display, m_config, surfaceAttributes);
    if (m_surface == EGL_NO_SURFACE) throw std::runtime_error("cannot create the EGL pbuffer");
    createAndBindContext();
  }

  ~HeadlessGles2() {
    if (m_display == EGL_NO_DISPLAY) return;
    eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (m_context != EGL_NO_CONTEXT) eglDestroyContext(m_display, m_context);
    if (m_surface != EGL_NO_SURFACE) eglDestroySurface(m_display, m_surface);
    eglTerminate(m_display);
  }

  void replaceContext() {
    eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    assert(m_context != EGL_NO_CONTEXT);
    eglDestroyContext(m_display, m_context);
    m_context = EGL_NO_CONTEXT;
    createAndBindContext();
  }

private:
  void createAndBindContext() {
    constexpr EGLint contextAttributes[]{EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    m_context = eglCreateContext(m_display, m_config, EGL_NO_CONTEXT, contextAttributes);
    if (m_context == EGL_NO_CONTEXT
        || eglMakeCurrent(m_display, m_surface, m_surface, m_context) != EGL_TRUE)
      throw std::runtime_error("cannot create or bind the GLES2 context");
    glViewport(0, 0, 64, 64);
    glDisable(GL_BLEND);
  }

  EGLDisplay m_display = EGL_NO_DISPLAY;
  EGLConfig m_config = nullptr;
  EGLSurface m_surface = EGL_NO_SURFACE;
  EGLContext m_context = EGL_NO_CONTEXT;
};

std::shared_ptr<CustomEffectAsset> asset(std::string stableId, char digest, std::string source) {
  return std::make_shared<CustomEffectAsset>(CustomEffectAsset{
      .stableId = std::move(stableId),
      .sha256Digest = std::string(64, digest),
      .source = std::move(source),
      .maxSampleRadiusPx = digest == 'a' ? 4.0F : 128.0F,
  });
}

constexpr auto kValidSource = R"glsl(
vec4 noctalia_effect(vec4 source_pm, vec4 backdrop_pm, vec2 local_uv,
    vec2 local_px, vec2 size_px, vec4 p0, vec4 p1, vec4 p2, vec4 p3,
    vec4 p4, vec4 p5, vec4 p6, vec4 p7) {
  return vec4(p0.rgb, 1.0);
}
)glsl";

// This obeys the intentionally small source subset, so validation admits it,
// but a real GLES compiler must reject the undefined identifier.
constexpr auto kGlInvalidSource = R"glsl(
vec4 noctalia_effect(vec4 source_pm, vec4 backdrop_pm, vec2 local_uv,
    vec2 local_px, vec2 size_px, vec4 p0, vec4 p1, vec4 p2, vec4 p3,
    vec4 p4, vec4 p5, vec4 p6, vec4 p7) {
  return vec4(no_such_identifier);
}
)glsl";

RoundedRectStyle styleFor(const std::shared_ptr<CustomEffectAsset>& effect, Color color) {
  RoundedRectStyle style;
  style.fill = Color{1.0F, 1.0F, 1.0F, 1.0F};
  style.customBackground = CustomEffectBinding{.asset = effect};
  style.customBackground->parameters[0] = {color.r, color.g, color.b, color.a};
  return style;
}

std::array<std::uint8_t, 4> centerPixel() {
  std::array<std::uint8_t, 4> pixel{};
  glFinish();
  glReadPixels(24, 40, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
  assert(glGetError() == GL_NO_ERROR);
  return pixel;
}

void clear() {
  glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
  glClear(GL_COLOR_BUFFER_BIT);
}

void expectGreen(const std::array<std::uint8_t, 4>& pixel) {
  assert(pixel[0] < 8);
  assert(pixel[1] > 247);
  assert(pixel[2] < 8);
  assert(pixel[3] > 247);
}
} // namespace

int main() {
  HeadlessGles2 gl;
  CustomEffectProgramCache cache;
  const auto good = asset("user.last-good", 'a', kValidSource);
  const auto bad = asset("user.last-good", 'b', kGlInvalidSource);
  const auto otherBad = asset("user.other-target", 'c', kGlInvalidSource);
  assert(validCustomEffectAsset(*good));
  assert(validCustomEffectAsset(*bad));
  assert(validCustomEffectAsset(*otherBad));

  auto goodStyle = styleFor(good, Color{1.0F, 0.0F, 0.0F, 1.0F});
  clear();
  assert(cache.draw(64, 64, 32, 32, goodStyle, Mat3::translation(8, 8)));
  auto status = cache.status(good->stableId);
  assert(status && status->active && status->activeDigest == good->sha256Digest);
  assert(status->requestedDigest == good->sha256Digest && status->log.empty());

  // A compile failure for another stable target has no last-good program of
  // its own. It must request native fallback without disturbing A's cache.
  auto otherBadStyle = styleFor(otherBad, Color{0.0F, 0.0F, 1.0F, 1.0F});
  clear();
  assert(!cache.draw(64, 64, 32, 32, otherBadStyle, Mat3::translation(8, 8)));
  const auto otherStatus = cache.status(otherBad->stableId);
  assert(otherStatus && !otherStatus->active && otherStatus->activeDigest.empty());
  assert(otherStatus->requestedDigest == otherBad->sha256Digest && !otherStatus->log.empty());
  status = cache.status(good->stableId);
  assert(status && status->active && status->activeDigest == good->sha256Digest);
  assert(status->requestedDigest == good->sha256Digest && status->log.empty());

  // B fails in the real driver compiler. Program A remains active, while the
  // status names requested B. A executes with B's live binding parameters.
  auto badStyle = styleFor(bad, Color{0.0F, 1.0F, 0.0F, 1.0F});
  clear();
  assert(cache.draw(64, 64, 32, 32, badStyle, Mat3::translation(8, 8)));
  expectGreen(centerPixel());
  status = cache.status(bad->stableId);
  assert(status && status->active);
  assert(status->activeDigest == good->sha256Digest);
  assert(status->requestedDigest == bad->sha256Digest);
  assert(!status->log.empty());

  // Last-consumer release deletes A. Retrying B has no live program and must
  // return false so the caller draws its resolved native background.
  cache.release(bad->stableId);
  cache.collectUnused();
  assert(!cache.status(bad->stableId));
  clear();
  assert(!cache.draw(64, 64, 32, 32, badStyle, Mat3::translation(8, 8)));
  status = cache.status(bad->stableId);
  assert(status && !status->active && status->activeDigest.empty());
  assert(status->requestedDigest == bad->sha256Digest && !status->log.empty());

  // Re-establish A, then exercise the production context-reset teardown across
  // an actual EGL context replacement. B cannot revive the abandoned A name.
  clear();
  assert(cache.draw(64, 64, 32, 32, goodStyle, Mat3::translation(8, 8)));
  assert(cache.status(good->stableId)->activeDigest == good->sha256Digest);
  cache.abandon();
  gl.replaceContext();
  clear();
  assert(!cache.draw(64, 64, 32, 32, badStyle, Mat3::translation(8, 8)));
  status = cache.status(bad->stableId);
  assert(status && !status->active && status->activeDigest.empty());
  assert(status->requestedDigest == bad->sha256Digest && !status->log.empty());

  cache.destroy();
  return 0;
}
