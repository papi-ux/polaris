/**
 * @file tests/unit/platform/test_graphics.cpp
 * @brief Test Linux OpenGL helper behavior used by DMA-BUF capture.
 */
#include "../../tests_common.h"

#ifdef __linux__
  #include <glad/gl.h>
  #include <src/platform/linux/graphics.h>

namespace {
  GLenum fake_gl_error_once() {
    static int calls = 0;
    return calls++ == 0 ? GL_INVALID_OPERATION : GL_NO_ERROR;
  }

  GLenum fake_gl_no_error() {
    return GL_NO_ERROR;
  }
}  // namespace

TEST(GraphicsTests, DrainErrorsReportsWhetherAnyErrorWasDrained) {
  const auto original_get_error = gl::ctx.GetError;

  gl::ctx.GetError = fake_gl_no_error;
  EXPECT_FALSE(gl::drain_errors("test"));

  gl::ctx.GetError = fake_gl_error_once;
  EXPECT_TRUE(gl::drain_errors("test"));

  gl::ctx.GetError = original_get_error;
}
#else
TEST(GraphicsTests, LinuxOnly) {
  GTEST_SKIP() << "Linux-only test";
}
#endif
