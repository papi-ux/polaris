/**
 * @file tests/unit/test_artwork_sweep.cpp
 * @brief The sweep engine, driven without a network.
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "src/artwork_sweep.h"

using namespace std::chrono_literals;

namespace {

  /// A sweeper whose waits are recorded rather than waited out, and whose clock stands still.
  struct harness_t {
    std::vector<std::int64_t> waits;
    std::mutex waits_mutex;

    artwork_sweep::sleep_fn_t recorder() {
      return [this](std::int64_t milliseconds) {
        std::lock_guard lock(waits_mutex);
        waits.push_back(milliseconds);
      };
    }

    static artwork_sweep::clock_fn_t frozen_clock() {
      return [] { return std::int64_t {1'700'000'000}; };
    }

    [[nodiscard]] std::size_t wait_count() {
      std::lock_guard lock(waits_mutex);
      return waits.size();
    }
  };

  artwork_sweep::match_t a_match(std::string title, int confidence = 90) {
    artwork_sweep::match_t match;
    match.provider_game_id = "2254";
    match.title = std::move(title);
    match.confidence = confidence;
    return match;
  }

  std::vector<artwork_sweep::candidate_t> games(std::size_t count) {
    std::vector<artwork_sweep::candidate_t> list;
    for (std::size_t at = 0; at < count; ++at) {
      list.push_back({"uuid-" + std::to_string(at), "Game " + std::to_string(at)});
    }
    return list;
  }

}  // namespace

TEST(ArtworkSweep, AskingAboutNoGamesIsNothingToDo) {
  harness_t harness;
  artwork_sweep::sweeper_t sweeper {[](const artwork_sweep::candidate_t &) {
                                     return artwork_sweep::lookup_t {};
                                   },
                                   harness.recorder(), harness_t::frozen_clock()};

  EXPECT_EQ(sweeper.start({}), artwork_sweep::start_e::nothing_to_do);
  EXPECT_EQ(sweeper.job().state, artwork_sweep::state_e::ready);
  EXPECT_EQ(sweeper.job().total, 0u);
}

TEST(ArtworkSweep, EveryOutcomeIsRecordedAgainstItsOwnGame) {
  harness_t harness;
  artwork_sweep::sweeper_t sweeper {
    [](const artwork_sweep::candidate_t &game) {
      artwork_sweep::lookup_t answer;
      if (game.name == "Game 0") {
        answer.match = a_match("Half-Life 2");
      }
      else if (game.name == "Game 1") {
        answer.has_match_without_poster = true;
      }
      else if (game.name == "Game 2") {
        answer.refused = true;
        answer.note = "The artwork provider answered 500.";
      }
      return answer;  // Game 3 gets nothing at all.
    },
    harness.recorder(), harness_t::frozen_clock()};

  ASSERT_EQ(sweeper.start(games(4)), artwork_sweep::start_e::started);
  ASSERT_TRUE(sweeper.wait_for_idle(5s));

  const auto job = sweeper.job();
  EXPECT_EQ(job.state, artwork_sweep::state_e::ready);
  EXPECT_EQ(job.total, 4u);
  EXPECT_EQ(job.looked_at, 4u);
  EXPECT_EQ(job.proposed, 1u);
  EXPECT_EQ(job.finished_at, job.started_at);
  EXPECT_EQ(job.message, "Found a cover for 1 of 4 games.");

  ASSERT_EQ(job.proposals.size(), 4u);
  EXPECT_EQ(job.proposals[0].outcome, artwork_sweep::outcome_e::proposed);
  ASSERT_TRUE(job.proposals[0].match.has_value());
  EXPECT_EQ(job.proposals[0].match->title, "Half-Life 2");
  EXPECT_EQ(job.proposals[0].uuid, "uuid-0");

  EXPECT_EQ(job.proposals[1].outcome, artwork_sweep::outcome_e::no_poster);
  EXPECT_FALSE(job.proposals[1].match.has_value());
  EXPECT_FALSE(job.proposals[1].note.empty());

  EXPECT_EQ(job.proposals[2].outcome, artwork_sweep::outcome_e::refused);
  EXPECT_EQ(job.proposals[2].note, "The artwork provider answered 500.");

  EXPECT_EQ(job.proposals[3].outcome, artwork_sweep::outcome_e::no_match);
}

TEST(ArtworkSweep, GamesArePacedApartButTheFirstIsNotDelayed) {
  harness_t harness;
  artwork_sweep::sweeper_t sweeper {[](const artwork_sweep::candidate_t &) {
                                     artwork_sweep::lookup_t answer;
                                     answer.match = a_match("A Game");
                                     return answer;
                                   },
                                   harness.recorder(), harness_t::frozen_clock()};

  ASSERT_EQ(sweeper.start(games(3)), artwork_sweep::start_e::started);
  ASSERT_TRUE(sweeper.wait_for_idle(5s));

  // Three games, two gaps between them.
  std::lock_guard lock(harness.waits_mutex);
  ASSERT_EQ(harness.waits.size(), 2u);
  EXPECT_EQ(harness.waits[0], artwork_sweep::between_games_milliseconds);
  EXPECT_EQ(harness.waits[1], artwork_sweep::between_games_milliseconds);
}

TEST(ArtworkSweep, ARateLimitIsWaitedOutAndTheGameIsAskedAgain) {
  harness_t harness;
  std::atomic<int> asks {0};
  artwork_sweep::sweeper_t sweeper {[&asks](const artwork_sweep::candidate_t &) {
                                      artwork_sweep::lookup_t answer;
                                      if (++asks == 1) {
                                        answer.rate_limited = true;
                                        return answer;
                                      }
                                      answer.match = a_match("Patient Game");
                                      return answer;
                                    },
                                    harness.recorder(), harness_t::frozen_clock()};

  ASSERT_EQ(sweeper.start(games(1)), artwork_sweep::start_e::started);
  ASSERT_TRUE(sweeper.wait_for_idle(5s));

  const auto job = sweeper.job();
  EXPECT_EQ(job.state, artwork_sweep::state_e::ready);
  EXPECT_EQ(job.proposed, 1u);
  EXPECT_EQ(asks.load(), 2);

  std::lock_guard lock(harness.waits_mutex);
  ASSERT_EQ(harness.waits.size(), 1u);
  EXPECT_EQ(harness.waits[0], artwork_sweep::first_rate_limit_wait_milliseconds);
}

TEST(ArtworkSweep, EachWaitIsLongerThanTheLastAndTheRunGivesUpInTheEnd) {
  harness_t harness;
  artwork_sweep::sweeper_t sweeper {[](const artwork_sweep::candidate_t &) {
                                     artwork_sweep::lookup_t answer;
                                     answer.rate_limited = true;
                                     return answer;
                                   },
                                   harness.recorder(), harness_t::frozen_clock()};

  ASSERT_EQ(sweeper.start(games(3)), artwork_sweep::start_e::started);
  ASSERT_TRUE(sweeper.wait_for_idle(5s));

  const auto job = sweeper.job();
  EXPECT_EQ(job.state, artwork_sweep::state_e::failed);
  EXPECT_NE(job.message.find("rate limiting"), std::string::npos);
  // It gave up on the first game, so nothing after it was even asked about.
  EXPECT_EQ(job.looked_at, 0u);
  for (const auto &proposal : job.proposals) {
    EXPECT_EQ(proposal.outcome, artwork_sweep::outcome_e::skipped);
  }

  std::lock_guard lock(harness.waits_mutex);
  ASSERT_EQ(harness.waits.size(), static_cast<std::size_t>(artwork_sweep::maximum_rate_limit_waits));
  EXPECT_EQ(harness.waits[0], artwork_sweep::first_rate_limit_wait_milliseconds);
  EXPECT_EQ(harness.waits[1], artwork_sweep::first_rate_limit_wait_milliseconds * 2);
  EXPECT_GT(harness.waits.back(), harness.waits.front());
}

TEST(ArtworkSweep, StoppingDuringABackoffSaysStoppedRatherThanRateLimited) {
  harness_t harness;
  artwork_sweep::sweeper_t *handle = nullptr;
  artwork_sweep::sweeper_t sweeper {[&](const artwork_sweep::candidate_t &) {
                                     artwork_sweep::lookup_t answer;
                                     answer.rate_limited = true;
                                     // Pressed Stop while the run was waiting out the provider.
                                     if (handle) {
                                       handle->cancel();
                                     }
                                     return answer;
                                   },
                                   harness.recorder(), harness_t::frozen_clock()};
  handle = &sweeper;

  ASSERT_EQ(sweeper.start(games(4)), artwork_sweep::start_e::started);
  ASSERT_TRUE(sweeper.wait_for_idle(5s));

  const auto job = sweeper.job();
  EXPECT_EQ(job.state, artwork_sweep::state_e::failed);
  EXPECT_NE(job.message.find("4 of the games"), std::string::npos)
    << "a run the player stopped said: " << job.message;
  EXPECT_EQ(job.message.find("rate limiting"), std::string::npos)
    << "the player pressed Stop and was told the provider refused them";

  // It waited once before noticing, and did not spend the whole budget.
  std::lock_guard lock(harness.waits_mutex);
  EXPECT_EQ(harness.waits.size(), 1u);
}

TEST(ArtworkSweep, OneRunAtATimeAndAFinishedOneCanBeForgotten) {
  harness_t harness;
  std::mutex gate_mutex;
  std::condition_variable gate;
  bool released = false;

  artwork_sweep::sweeper_t sweeper {
    [&](const artwork_sweep::candidate_t &) {
      std::unique_lock lock(gate_mutex);
      gate.wait_for(lock, 5s, [&] { return released; });
      artwork_sweep::lookup_t answer;
      answer.match = a_match("Held Game");
      return answer;
    },
    harness.recorder(), harness_t::frozen_clock()};

  ASSERT_EQ(sweeper.start(games(1)), artwork_sweep::start_e::started);
  EXPECT_EQ(sweeper.start(games(1)), artwork_sweep::start_e::already_running);
  EXPECT_FALSE(sweeper.clear());
  EXPECT_EQ(sweeper.job().state, artwork_sweep::state_e::searching);

  {
    std::lock_guard lock(gate_mutex);
    released = true;
  }
  gate.notify_all();
  ASSERT_TRUE(sweeper.wait_for_idle(5s));

  EXPECT_EQ(sweeper.job().state, artwork_sweep::state_e::ready);
  EXPECT_TRUE(sweeper.clear());
  EXPECT_EQ(sweeper.job().total, 0u);
  EXPECT_TRUE(sweeper.job().proposals.empty());
}

TEST(ArtworkSweep, CancellingLeavesWhatItHasAndSaysHowMuchItSkipped) {
  harness_t harness;
  std::atomic<int> asks {0};
  artwork_sweep::sweeper_t *handle = nullptr;
  artwork_sweep::sweeper_t sweeper {[&](const artwork_sweep::candidate_t &) {
                                     artwork_sweep::lookup_t answer;
                                     answer.match = a_match("Doomed Game");
                                     if (++asks == 1 && handle) {
                                       handle->cancel();
                                     }
                                     return answer;
                                   },
                                   harness.recorder(), harness_t::frozen_clock()};
  handle = &sweeper;

  ASSERT_EQ(sweeper.start(games(5)), artwork_sweep::start_e::started);
  ASSERT_TRUE(sweeper.wait_for_idle(5s));

  const auto job = sweeper.job();
  EXPECT_EQ(job.state, artwork_sweep::state_e::failed);
  EXPECT_EQ(job.looked_at, 1u);
  EXPECT_EQ(job.proposed, 1u);
  EXPECT_NE(job.message.find("4 of the games"), std::string::npos);
  EXPECT_EQ(job.proposals[0].outcome, artwork_sweep::outcome_e::proposed);
  EXPECT_EQ(job.proposals[4].outcome, artwork_sweep::outcome_e::skipped);
}

TEST(ArtworkSweep, ARunIsClippedSoItCannotGoOnForever) {
  harness_t harness;
  artwork_sweep::sweeper_t sweeper {[](const artwork_sweep::candidate_t &) {
                                     return artwork_sweep::lookup_t {};
                                   },
                                   harness.recorder(), harness_t::frozen_clock()};

  ASSERT_EQ(sweeper.start(games(artwork_sweep::maximum_games_per_run + 12)),
            artwork_sweep::start_e::started);
  ASSERT_TRUE(sweeper.wait_for_idle(30s));

  const auto job = sweeper.job();
  EXPECT_EQ(job.total, artwork_sweep::maximum_games_per_run);
  EXPECT_EQ(job.proposals.size(), artwork_sweep::maximum_games_per_run);
}

TEST(ArtworkSweep, StateNamesAreTheOnesTheConsoleReads) {
  EXPECT_EQ(artwork_sweep::state_name(artwork_sweep::state_e::searching), "searching");
  EXPECT_EQ(artwork_sweep::state_name(artwork_sweep::state_e::ready), "ready");
  EXPECT_EQ(artwork_sweep::state_name(artwork_sweep::state_e::failed), "failed");

  EXPECT_EQ(artwork_sweep::outcome_name(artwork_sweep::outcome_e::proposed), "proposed");
  EXPECT_EQ(artwork_sweep::outcome_name(artwork_sweep::outcome_e::no_match), "no_match");
  EXPECT_EQ(artwork_sweep::outcome_name(artwork_sweep::outcome_e::no_poster), "no_poster");
  EXPECT_EQ(artwork_sweep::outcome_name(artwork_sweep::outcome_e::refused), "refused");
  EXPECT_EQ(artwork_sweep::outcome_name(artwork_sweep::outcome_e::skipped), "skipped");
}

TEST(ArtworkSweep, CancellationDuringTheBetweenGamesPauseStartsNoFurtherLookup) {
  artwork_sweep::sweeper_t *handle = nullptr;
  std::atomic<int> asks {0};
  artwork_sweep::sweeper_t sweeper {
    [&](const artwork_sweep::candidate_t &) {
      ++asks;
      return artwork_sweep::lookup_t {};
    },
    [&](std::int64_t) { handle->cancel(); },
    harness_t::frozen_clock()};
  handle = &sweeper;
  ASSERT_EQ(sweeper.start(games(3)), artwork_sweep::start_e::started);
  ASSERT_TRUE(sweeper.wait_for_idle(5s));
  EXPECT_EQ(asks.load(), 1);
  EXPECT_EQ(sweeper.job().looked_at, 1u);
  EXPECT_EQ(sweeper.job().state, artwork_sweep::state_e::failed);
}

TEST(ArtworkSweep, FailedPreparationDoesNotReserveARunThatCanNeverFinish) {
  bool fail_clock = true;
  artwork_sweep::sweeper_t sweeper {
    [](const artwork_sweep::candidate_t &) { return artwork_sweep::lookup_t {}; },
    [](std::int64_t) {},
    [&] {
      if (fail_clock) throw std::runtime_error("clock unavailable");
      return std::int64_t {1'700'000'000};
    }};
  EXPECT_THROW(sweeper.start(games(1)), std::runtime_error);
  EXPECT_TRUE(sweeper.wait_for_idle(100ms));
  EXPECT_EQ(sweeper.job().state, artwork_sweep::state_e::ready);
  fail_clock = false;
  ASSERT_EQ(sweeper.start(games(1)), artwork_sweep::start_e::started);
  ASSERT_TRUE(sweeper.wait_for_idle(5s));
  EXPECT_EQ(sweeper.job().looked_at, 1u);
}

TEST(ArtworkSweep, DestructionWaitsForTheWorkerThreadToExit) {
  struct exit_gate_t {
    std::promise<void> entered;
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
  };
  struct thread_cleanup_t {
    std::shared_ptr<exit_gate_t> gate;
    ~thread_cleanup_t() {
      gate->entered.set_value();
      gate->released.wait_for(5s);
    }
  };
  auto gate = std::make_shared<exit_gate_t>();
  auto entered = gate->entered.get_future();
  auto sweeper = std::make_unique<artwork_sweep::sweeper_t>(
    [gate](const artwork_sweep::candidate_t &) {
      thread_local thread_cleanup_t cleanup {gate};
      return artwork_sweep::lookup_t {};
    },
    artwork_sweep::sleep_fn_t {}, harness_t::frozen_clock());
  ASSERT_EQ(sweeper->start(games(1)), artwork_sweep::start_e::started);
  if (entered.wait_for(5s) != std::future_status::ready) {
    gate->release.set_value();
    FAIL() << "cover worker did not reach thread teardown";
  }
  // The job is finished, but thread-owned resources are still being released.
  ASSERT_TRUE(sweeper->wait_for_idle(5s));
  std::promise<void> destroying;
  auto started = destroying.get_future();
  auto destroyed = std::async(std::launch::async, [&] {
    destroying.set_value();
    sweeper.reset();
  });
  started.wait();
  const auto while_thread_alive = destroyed.wait_for(100ms);
  gate->release.set_value();
  EXPECT_EQ(while_thread_alive, std::future_status::timeout);
  EXPECT_EQ(destroyed.wait_for(5s), std::future_status::ready);
}

TEST(ArtworkSweepDeathTest, ALookupExceptionFailsTheJobWithoutTerminatingTheHost) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_EXIT(([] {
    int asks = 0;
    artwork_sweep::sweeper_t sweeper {
      [&](const artwork_sweep::candidate_t &) {
        if (++asks == 2) throw std::runtime_error("private lookup detail");
        artwork_sweep::lookup_t result;
        result.match = a_match("Kept result");
        return result;
      },
      [](std::int64_t) {}, harness_t::frozen_clock()};
    if (sweeper.start(games(3)) != artwork_sweep::start_e::started || !sweeper.wait_for_idle(5s)) std::_Exit(1);
    const auto job = sweeper.job();
    if (job.state != artwork_sweep::state_e::failed || job.looked_at != 1 || job.proposed != 1 ||
        job.message.empty() || job.message.find("private lookup detail") != std::string::npos) std::_Exit(2);
    if (sweeper.start(games(1)) != artwork_sweep::start_e::started || !sweeper.wait_for_idle(5s) ||
        sweeper.job().state != artwork_sweep::state_e::ready) std::_Exit(3);
    std::_Exit(0);
  }()), testing::ExitedWithCode(0), "");
}

TEST(ArtworkSweep, CancelRetiresTheRunIdentityBeforeThePendingLookupReturns) {
  std::promise<void> entered;
  std::promise<void> release;
  auto released = release.get_future();
  artwork_sweep::sweeper_t sweeper {
    [&](const auto &) {
      entered.set_value();
      released.wait_for(5s);
      artwork_sweep::lookup_t answer;
      answer.match = a_match("Held game");
      return answer;
    }, {}, harness_t::frozen_clock()};
  ASSERT_EQ(sweeper.start(games(1), "import-run"), artwork_sweep::start_e::started);
  ASSERT_EQ(entered.get_future().wait_for(5s), std::future_status::ready);
  EXPECT_EQ(sweeper.job().id, "import-run");
  sweeper.cancel();
  EXPECT_TRUE(sweeper.job().id.empty());
  EXPECT_FALSE(sweeper.clear());
  release.set_value();
  ASSERT_TRUE(sweeper.wait_for_idle(5s));
  // A retained proposal remains available for manual review without reviving automatic approval.
  EXPECT_EQ(sweeper.job().proposed, 1u);
  EXPECT_TRUE(sweeper.job().id.empty());
}
