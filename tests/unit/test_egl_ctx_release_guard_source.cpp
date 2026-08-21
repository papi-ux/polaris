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

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

  std::string read_source(const std::string &relative_path) {
    const auto path = std::filesystem::path {POLARIS_SOURCE_DIR} / relative_path;
    std::ifstream file {path};
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
  }

  std::string strip_cpp_comments(std::string_view source) {
    enum class state_e {
      code,
      line_comment,
      block_comment,
      string_literal,
      char_literal,
    };

    std::string code;
    code.reserve(source.size());
    auto state = state_e::code;
    bool escaped = false;

    for (std::size_t i = 0; i < source.size(); ++i) {
      const auto ch = source[i];
      const auto next = i + 1 < source.size() ? source[i + 1] : '\0';

      switch (state) {
        case state_e::code:
          if (ch == '/' && next == '/') {
            code.push_back(' ');
            ++i;
            state = state_e::line_comment;
          } else if (ch == '/' && next == '*') {
            code.push_back(' ');
            ++i;
            state = state_e::block_comment;
          } else {
            code.push_back(ch);
            if (ch == '"') {
              state = state_e::string_literal;
              escaped = false;
            } else if (ch == '\'') {
              state = state_e::char_literal;
              escaped = false;
            }
          }
          break;

        case state_e::line_comment:
          if (ch == '\n') {
            code.push_back(ch);
            state = state_e::code;
          }
          break;

        case state_e::block_comment:
          if (ch == '*' && next == '/') {
            code.push_back(' ');
            ++i;
            state = state_e::code;
          }
          break;

        case state_e::string_literal:
        case state_e::char_literal: {
          code.push_back(ch);
          const auto terminator = state == state_e::string_literal ? '"' : '\'';
          if (escaped) {
            escaped = false;
          } else if (ch == '\\') {
            escaped = true;
          } else if (ch == terminator) {
            state = state_e::code;
          }
          break;
        }
      }
    }

    return code;
  }

  std::string normalize_code(std::string_view source) {
    const auto uncommented = strip_cpp_comments(source);
    std::string normalized;
    normalized.reserve(uncommented.size());
    bool pending_space = false;

    for (const auto ch : uncommented) {
      if (std::isspace(static_cast<unsigned char>(ch))) {
        pending_space = !normalized.empty();
        continue;
      }
      if (pending_space) {
        normalized.push_back(' ');
        pending_space = false;
      }
      normalized.push_back(ch);
    }

    return normalized;
  }

  std::size_t count_occurrences(std::string_view source, std::string_view needle) {
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = source.find(needle, position)) != std::string_view::npos) {
      ++count;
      position += needle.size();
    }
    return count;
  }

  bool ctx_release_contract_holds(std::string_view source) {
    const auto code = normalize_code(source);
    constexpr std::string_view contract =
      "if (eglGetCurrentContext() == ctx) { "
      "eglMakeCurrent(disp, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); "
      "} eglDestroyContext(disp, ctx);";
    return count_occurrences(code, "KITTY_USING_MOVE_T(ctx_t,") == 1 &&
           count_occurrences(code, contract) == 1;
  }

  bool missing_context_diagnostic_contract_holds(std::string_view source) {
    const auto code = normalize_code(source);
    constexpr std::string_view condition =
      "eglGetCurrentContext() == EGL_NO_CONTEXT ? "
      "\"; no EGL context is current on this thread\"sv : \"\"sv";
    return count_occurrences(code, "if (compile_error.empty()) {") == 1 &&
           count_occurrences(code, "compilation failed and the driver gave no reason") == 1 &&
           count_occurrences(code, condition) == 1;
  }

}  // namespace

TEST(EglCtxReleaseGuardSource, ContextReleaseIsScopedToTheContextWeBound) {
  const auto source = read_source("src/platform/linux/graphics.h");
  ASSERT_FALSE(source.empty()) << "could not read src/platform/linux/graphics.h via POLARIS_SOURCE_DIR";
  EXPECT_TRUE(ctx_release_contract_holds(source))
    << "~egl::ctx_t must guard the thread-wide release and keep destruction unconditional";
}

TEST(EglCtxReleaseGuardSource, EmptyShaderInfoLogNamesTheMissingContext) {
  const auto source = read_source("src/platform/linux/graphics.cpp");
  ASSERT_FALSE(source.empty()) << "could not read src/platform/linux/graphics.cpp via POLARIS_SOURCE_DIR";

  // An empty compile info log means the GL call never reached a live context. Saying
  // so is what separates this failure from a missing or unreadable shader file; the
  // bare "<path>: " line it used to print sent two reporters after their asset paths.
  EXPECT_TRUE(missing_context_diagnostic_contract_holds(source))
    << "an empty shader info log must report the missing-current-context condition";
}

TEST(EglCtxReleaseGuardMatcher, RejectsCommentedOutGuard) {
  constexpr std::string_view source = R"cpp(
    KITTY_USING_MOVE_T(ctx_t, tuple_t, , {
      // if (eglGetCurrentContext() == ctx)
      eglMakeCurrent(disp, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      }
      eglDestroyContext(disp, ctx);
    });
  )cpp";
  EXPECT_FALSE(ctx_release_contract_holds(source));
}

TEST(EglCtxReleaseGuardMatcher, RejectsDestroyHiddenInsideOuterGuard) {
  constexpr std::string_view source = R"cpp(
    KITTY_USING_MOVE_T(ctx_t, tuple_t, , {
      if (eglGetCurrentContext() == ctx) {
        eglMakeCurrent(disp, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (cleanup_enabled) {}
        eglDestroyContext(disp, ctx);
      }
    });
  )cpp";
  EXPECT_FALSE(ctx_release_contract_holds(source));
}

TEST(EglCtxReleaseGuardMatcher, RejectsInvertedMissingContextDiagnostic) {
  constexpr std::string_view source = R"cpp(
    eglGetCurrentContext() != EGL_NO_CONTEXT ?
      "; no EGL context is current on this thread"sv : ""sv;
    "compilation failed and the driver gave no reason"sv;
  )cpp";
  EXPECT_FALSE(missing_context_diagnostic_contract_holds(source));
}
