/**
 * @file src/platform/linux/labwc_startup_diagnostics.cpp
 * @brief Bounded, generation-scoped evidence from a labwc startup client.
 */

#include "labwc_startup_diagnostics.h"

#ifdef __linux__

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <charconv>
#include <limits>
#include <poll.h>
#include <regex>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace labwc_startup_diagnostics {
  namespace {
    std::string shell_quote(std::string_view value) {
      std::string result {"'"};
      for (const char ch : value) {
        if (ch == '\'') {
          result += "'\\''";
        } else {
          result += ch;
        }
      }
      result += '\'';
      return result;
    }

    std::string replace_all(
      const std::string &input,
      const std::regex &pattern,
      std::string_view replacement
    ) {
      return std::regex_replace(input, pattern, std::string {replacement});
    }

    std::optional<int> parse_status_line(std::string_view line) {
      while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
        line.remove_prefix(1);
      }
      while (!line.empty() &&
             (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
        line.remove_suffix(1);
      }
      int status = -1;
      const auto parsed = std::from_chars(line.data(), line.data() + line.size(), status);
      if (parsed.ec != std::errc {} || parsed.ptr != line.data() + line.size() ||
          status < 0 || status > 255) {
        return std::nullopt;
      }
      return status;
    }

    bool long_value_character(unsigned char ch) {
      return (ch >= 'a' && ch <= 'z') ||
             (ch >= 'A' && ch <= 'Z') ||
             (ch >= '0' && ch <= '9') ||
             ch == '_' || ch == '+' || ch == '=' || ch == '-';
    }

    std::string redact_long_values(std::string_view input) {
      std::string output;
      output.reserve(input.size());
      std::size_t cursor = 0;
      while (cursor < input.size()) {
        const auto begin = cursor;
        while (cursor < input.size() &&
               long_value_character(static_cast<unsigned char>(input[cursor]))) {
          ++cursor;
        }
        if (cursor - begin >= 40) {
          output += "[redacted-value]";
        } else {
          output.append(input.substr(begin, cursor - begin));
        }
        if (cursor < input.size()) {
          output += input[cursor++];
        }
      }
      return output;
    }

    bool ip_literal_character(unsigned char ch) {
      return (ch >= '0' && ch <= '9') ||
             (ch >= 'a' && ch <= 'f') ||
             (ch >= 'A' && ch <= 'F') ||
             ch == ':' || ch == '.';
    }

    std::string redact_ip_literals(std::string_view input) {
      std::string output;
      output.reserve(input.size());
      std::size_t cursor = 0;
      while (cursor < input.size()) {
        if (!ip_literal_character(static_cast<unsigned char>(input[cursor]))) {
          output += input[cursor++];
          continue;
        }

        const auto begin = cursor;
        while (cursor < input.size() &&
               ip_literal_character(static_cast<unsigned char>(input[cursor]))) {
          ++cursor;
        }
        const auto address_end = cursor;
        if (cursor < input.size() && input[cursor] == '%') {
          ++cursor;
          while (cursor < input.size()) {
            const auto ch = static_cast<unsigned char>(input[cursor]);
            if (!((ch >= 'a' && ch <= 'z') ||
                  (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') ||
                  ch == '_' || ch == '.' || ch == '-')) {
              break;
            }
            ++cursor;
          }
        }

        const std::string candidate {input.substr(begin, address_end - begin)};
        std::array<unsigned char, 16> parsed {};
        if (inet_pton(AF_INET6, candidate.c_str(), parsed.data()) == 1 ||
            inet_pton(AF_INET, candidate.c_str(), parsed.data()) == 1) {
          output += "[ip]";
        } else {
          output.append(input.substr(begin, cursor - begin));
        }
      }
      return output;
    }

    void close_descriptor(int &fd) {
      if (fd >= 0) {
        close(fd);
        fd = -1;
      }
    }
  }  // namespace

  collector_t::collector_t(std::string session_instance_id):
      session_instance_id_ {std::move(session_instance_id)} {
  }

  void collector_t::append_stderr(std::string_view bytes) {
    if (bytes.empty()) {
      return;
    }

    const std::lock_guard lock {mutex_};
    if (bytes.size() > std::numeric_limits<std::size_t>::max() - stderr_bytes_seen_) {
      stderr_bytes_seen_ = std::numeric_limits<std::size_t>::max();
    } else {
      stderr_bytes_seen_ += bytes.size();
    }

    if (bytes.size() >= max_retained_stderr_bytes) {
      stderr_tail_.assign(bytes.end() - max_retained_stderr_bytes, bytes.end());
      stderr_truncated_ = true;
      return;
    }

    const auto overflow = stderr_tail_.size() + bytes.size() > max_retained_stderr_bytes ?
      stderr_tail_.size() + bytes.size() - max_retained_stderr_bytes :
      0;
    if (overflow > 0) {
      stderr_tail_.erase(0, overflow);
      stderr_truncated_ = true;
    }
    stderr_tail_.append(bytes);
  }

  void collector_t::record_shell_exit_status(int status) {
    if (status < 0 || status > 255) {
      return;
    }
    const std::lock_guard lock {mutex_};
    if (!shell_exit_status_) {
      shell_exit_status_ = status;
    }
  }

  std::optional<snapshot_t> collector_t::snapshot(
    std::string_view expected_session_instance_id
  ) const {
    std::string raw_stderr;
    snapshot_t result;
    {
      const std::lock_guard lock {mutex_};
      if (expected_session_instance_id.empty() ||
          expected_session_instance_id != session_instance_id_) {
        return std::nullopt;
      }
      result.shell_exit_status = shell_exit_status_;
      result.stderr_bytes_seen = stderr_bytes_seen_;
      result.stderr_truncated = stderr_truncated_;
      raw_stderr = stderr_tail_;
    }

    auto redacted = redact_stderr_for_log(raw_stderr);
    result.stderr_truncated = result.stderr_truncated || redacted.truncated;
    result.stderr_summary = std::move(redacted.text);
    return result;
  }

  redacted_text_t redact_stderr_for_log(std::string_view raw) {
    constexpr std::size_t max_redaction_input_bytes = max_log_summary_bytes * 2;
    const bool input_truncated = raw.size() > max_redaction_input_bytes;
    if (input_truncated) {
      raw.remove_prefix(raw.size() - max_redaction_input_bytes);
    }

    std::string normalized;
    normalized.reserve(raw.size());
    bool previous_space = true;
    for (std::size_t i = 0; i < raw.size(); ++i) {
      const auto ch = static_cast<unsigned char>(raw[i]);
      if (ch == 0x1b && i + 1 < raw.size() && raw[i + 1] == '[') {
        i += 2;
        while (i < raw.size()) {
          const auto terminal = static_cast<unsigned char>(raw[i]);
          if (terminal >= 0x40 && terminal <= 0x7e) {
            break;
          }
          ++i;
        }
        continue;
      }

      const bool space = ch <= 0x20 || ch == 0x7f;
      if (space) {
        if (!previous_space && !normalized.empty()) {
          normalized += ' ';
        }
        previous_space = true;
        continue;
      }
      normalized += static_cast<char>(ch);
      previous_space = false;
    }
    while (!normalized.empty() && normalized.back() == ' ') {
      normalized.pop_back();
    }

    // The retained tail can begin in the middle of a credential or path. Never
    // log that partial token: without its key/prefix it cannot be redacted with
    // confidence and carries little diagnostic value anyway.
    if (input_truncated && !normalized.empty()) {
      const auto first_space = normalized.find(' ');
      normalized.replace(
        0,
        first_space == std::string::npos ? normalized.size() : first_space,
        "[truncated-prefix]"
      );
    }

    static const std::regex url {
      R"(\b[A-Za-z][A-Za-z0-9+.-]*://[^ ]+)"
    };
    static const std::regex unix_home {R"((/(var/home|home|Users)/)[^ /]+)"};
    static const std::regex runtime_user {R"((/run/user/)[0-9]+)"};
    static const std::regex windows_home {R"(([A-Za-z]:\\Users\\)[^ \\]+)"};
    static const std::regex ipv4 {R"(\b([0-9]{1,3}\.){3}[0-9]{1,3}\b)"};
    static const std::regex email {
      R"(\b[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}\b)"
    };
    static const std::regex bearer {R"((\bbearer[ ]+)[^ ,;]+)", std::regex::icase};
    static const std::regex sensitive_flag {
      R"((--(password|passwd|token|secret|api[-_]?key)[ =]+)[^ ,;]+)",
      std::regex::icase
    };
    static const std::regex sensitive_assignment {
      R"(((authorization|proxy[-_]?authorization|access[-_]?token|refresh[-_]?token|session[-_]?token|api[-_]?key|password|passwd|secret|cookie)[ ]*[:=][ ]*)("[^"]*"|'[^']*'|[^ ,;&]+))",
      std::regex::icase
    };
    static const std::regex identity_assignment {
      R"((\b(host(name)?|server|endpoint|address|user(name)?)[ ]*[:=][ ]*)("[^"]*"|'[^']*'|[^ ,;&]+))",
      std::regex::icase
    };

    normalized = replace_all(normalized, url, "[url]");
    normalized = replace_all(normalized, unix_home, "$1[user]");
    normalized = replace_all(normalized, runtime_user, "$1[uid]");
    normalized = replace_all(normalized, windows_home, "$1[user]");
    normalized = replace_all(normalized, ipv4, "[ip]");
    normalized = redact_ip_literals(normalized);
    normalized = replace_all(normalized, email, "[email]");
    normalized = replace_all(normalized, bearer, "$1[redacted]");
    normalized = replace_all(normalized, sensitive_flag, "$1[redacted]");
    normalized = replace_all(normalized, sensitive_assignment, "$1[redacted]");
    normalized = replace_all(normalized, identity_assignment, "$1[redacted]");
    normalized = redact_long_values(normalized);

    redacted_text_t result;
    result.truncated = input_truncated;
    if (normalized.size() > max_log_summary_bytes) {
      result.text = normalized.substr(normalized.size() - max_log_summary_bytes);
      const auto first_space = result.text.find(' ');
      result.text.replace(
        0,
        first_space == std::string::npos ? result.text.size() : first_space,
        "[truncated-prefix]"
      );
      result.text.resize(std::min(result.text.size(), max_log_summary_bytes));
      result.truncated = true;
    } else {
      result.text = std::move(normalized);
    }
    return result;
  }

  std::string describe_for_log(const snapshot_t &snapshot) {
    std::string result = "startup_client_exit_status=";
    result += snapshot.shell_exit_status ?
      std::to_string(*snapshot.shell_exit_status) :
      "pending";
    result += " startup_client_stderr_bytes=" +
              std::to_string(snapshot.stderr_bytes_seen);
    result += " startup_client_stderr_truncated=";
    result += snapshot.stderr_truncated ? "true" : "false";
    if (!snapshot.stderr_summary.empty()) {
      result += " startup_client_stderr_tail=[" + snapshot.stderr_summary + ']';
    }
    return result;
  }

  std::string instrument_shell_command(
    std::string_view command,
    int stderr_fd,
    int status_fd
  ) {
    if (command.empty() || stderr_fd < 3 || status_fd < 3 || stderr_fd == status_fd) {
      return std::string {command};
    }

    const std::string status_variable = "__polaris_startup_status";
    return "bash -c " + shell_quote(command) +
           " 2>&" + std::to_string(stderr_fd) +
           "; " + status_variable + "=$?; printf '%d\\n' \"$" + status_variable +
           "\" >&" + std::to_string(status_fd) +
           "; exit \"$" + status_variable + "\"";
  }

  std::string make_labwc_startup_command(std::string_view shell_program) {
    return "bash -c " + shell_quote(shell_program);
  }

  void drain_pipes(
    int stderr_fd,
    int status_fd,
    const std::shared_ptr<collector_t> &collector
  ) {
    if (!collector) {
      close_descriptor(stderr_fd);
      close_descriptor(status_fd);
      return;
    }

    std::string status_buffer;
    while (stderr_fd >= 0 || status_fd >= 0) {
      pollfd descriptors[2] {
        {stderr_fd, static_cast<short>(POLLIN | POLLHUP | POLLERR), 0},
        {status_fd, static_cast<short>(POLLIN | POLLHUP | POLLERR), 0},
      };
      const auto ready = poll(descriptors, 2, -1);
      if (ready < 0) {
        if (errno == EINTR) {
          continue;
        }
        break;
      }

      if (stderr_fd >= 0 && descriptors[0].revents != 0) {
        char buffer[2048];
        const auto count = read(stderr_fd, buffer, sizeof(buffer));
        if (count > 0) {
          collector->append_stderr(std::string_view {buffer, static_cast<std::size_t>(count)});
        } else if (count == 0 || (errno != EINTR && errno != EAGAIN)) {
          close_descriptor(stderr_fd);
        }
      }

      if (status_fd >= 0 && descriptors[1].revents != 0) {
        char buffer[32];
        const auto count = read(status_fd, buffer, sizeof(buffer));
        if (count > 0) {
          status_buffer.append(buffer, static_cast<std::size_t>(count));
          const auto newline = status_buffer.find('\n');
          if (newline != std::string::npos || status_buffer.size() > 16) {
            const auto line = std::string_view {status_buffer}.substr(0, newline);
            if (const auto status = parse_status_line(line)) {
              collector->record_shell_exit_status(*status);
            }
            close_descriptor(status_fd);
          }
        } else if (count == 0 || (errno != EINTR && errno != EAGAIN)) {
          if (const auto status = parse_status_line(status_buffer)) {
            collector->record_shell_exit_status(*status);
          }
          close_descriptor(status_fd);
        }
      }
    }

    close_descriptor(stderr_fd);
    close_descriptor(status_fd);
  }

}  // namespace labwc_startup_diagnostics

#endif  // __linux__
