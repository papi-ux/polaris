#pragma once

#include <optional>
#include <string>

namespace ai_optimizer::claude_cli {
  struct status_t {
    bool available = false;
    std::optional<bool> authenticated;
  };

  struct result_t {
    std::optional<std::string> response;
    std::string code;
    std::string error;
    std::string action;
  };

  // Uses the running service account's normal Claude credential store.
  // Account identifiers and raw CLI diagnostics never leave this adapter.
  status_t status(const std::string &executable = {});

  result_t explain(const std::string &model,
                   const std::string &system_prompt,
                   const std::string &schema,
                   const std::string &evidence,
                   int timeout_ms,
                   const std::string &executable = {});
}
