/**
 * @file tests/tests_environment.h
 * @brief Declarations for PolarisEnvironment.
 */
#pragma once
#include "tests_common.h"
#include "tests_paths.h"

struct PolarisEnvironment: testing::Environment {
  void SetUp() override {
    mail::man = std::make_shared<safe::mail_raw_t>();
    restore_logging();
  }

  void TearDown() override {
    deinit_log = {};
    mail::man = {};
  }

  static void restore_logging() {
    // Release the prior guard before init installs the replacement sinks.
    deinit_log.reset();
    deinit_log = logging::init(0, test_paths::log_file().string());
  }

  inline static std::unique_ptr<logging::deinit_t> deinit_log;
};
