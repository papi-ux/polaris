/**
 * @file tests/unit/test_egl_ctx_release_guard_source.cpp
 * @brief Source guard: ~egl::ctx_t must release the EGL context only when that
 *        context is the one currently bound to the calling thread.
 *
 * eglMakeCurrent with EGL_NO_CONTEXT is a thread-wide release. The display
 * argument does not scope it (EGL 1.5 3.7.3; Mesa's _eglBindContext unbinds the
 * thread's context whichever display it came from), so releasing unconditionally
 * in the destructor unbinds a *different*, live context that some other object
 * made current first.
 *
 * That is not theoretical. video::make_encode_device builds a replacement encode
 * device and only then destroys the original when it demotes a 10-bit request to
 * 8-bit, which is the ordinary path for any client that asks for HDR on a host
 * with no HDR output. va_t owns an egl::ctx_t, so the old device's destructor
 * unbound the context the new device had just made current. Every subsequent GL
 * call no-opped, all five conversion shaders reported a compile failure with an
 * empty driver info log, set_frame returned -1, and the session died before its
 * first frame: black video, working audio, no useful error (issues #516, #482).
 *
 * Reproducing that needs a VAAPI host taking the demotion, which no unit test and
 * no CI runner has, so the invariant is pinned at the source level the same way
 * test_video_hdr_probe_guard_source.cpp pins its probe contract.
 */
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

  std::string read_source(const std::string &relative_path) {
    const auto path = std::filesystem::path {POLARIS_SOURCE_DIR} / relative_path;
    std::ifstream file {path};
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
  }

}  // namespace

TEST(EglCtxReleaseGuardSource, ContextReleaseIsScopedToTheContextWeBound) {
  const auto source = read_source("src/platform/linux/graphics.h");
  ASSERT_FALSE(source.empty()) << "could not read src/platform/linux/graphics.h via POLARIS_SOURCE_DIR";

  const auto ctx_pos = source.find("KITTY_USING_MOVE_T(ctx_t,");
  ASSERT_NE(ctx_pos, std::string::npos) << "egl::ctx_t declaration not found";

  const auto body_end = source.find("});", ctx_pos);
  ASSERT_NE(body_end, std::string::npos) << "egl::ctx_t destructor body not found";
  const auto body = source.substr(ctx_pos, body_end - ctx_pos);

  const auto guard_pos = body.find("if (eglGetCurrentContext() == ctx)");
  EXPECT_NE(guard_pos, std::string::npos)
    << "~egl::ctx_t must release only when this context is the one bound to the calling "
       "thread: eglMakeCurrent with EGL_NO_CONTEXT is a thread-wide release and would "
       "otherwise unbind a live context belonging to a replacement encode device";

  const auto release_pos = body.find("eglMakeCurrent(disp, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT)");
  ASSERT_NE(release_pos, std::string::npos) << "the context release call is missing entirely";
  EXPECT_LT(guard_pos, release_pos)
    << "the release must sit inside the eglGetCurrentContext() guard, not before it";

  // Destruction itself must stay unconditional. A context that is current on some
  // other thread still has to be destroyed here; EGL defers the delete until it is
  // no longer current, which is exactly the behaviour we want.
  const auto destroy_pos = body.find("eglDestroyContext(disp, ctx)");
  ASSERT_NE(destroy_pos, std::string::npos) << "eglDestroyContext call not found";
  EXPECT_LT(release_pos, destroy_pos) << "release must precede destroy";
  const auto guard_close = body.find("}", release_pos);
  ASSERT_NE(guard_close, std::string::npos) << "guard block is unterminated";
  EXPECT_LT(guard_close, destroy_pos)
    << "eglDestroyContext must run unconditionally, outside the eglGetCurrentContext guard";
}

TEST(EglCtxReleaseGuardSource, EmptyShaderInfoLogNamesTheMissingContext) {
  const auto source = read_source("src/platform/linux/graphics.cpp");
  ASSERT_FALSE(source.empty()) << "could not read src/platform/linux/graphics.cpp via POLARIS_SOURCE_DIR";

  // An empty compile info log means the GL call never reached a live context. Saying
  // so is what separates this failure from a missing or unreadable shader file; the
  // bare "<path>: " line it used to print sent two reporters after their asset paths.
  EXPECT_NE(source.find("compilation failed and the driver gave no reason"), std::string::npos)
    << "an empty shader info log must report that the driver gave no reason, not print a bare colon";
  EXPECT_NE(source.find("no EGL context is current on this thread"), std::string::npos)
    << "an empty shader info log must name a missing current EGL context as the likely cause";
}
