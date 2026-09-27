/**
 * @file src/artwork_sweep.h
 * @brief One pass over the games that have no cover, proposing a match for each.
 *
 * A player who imports three hundred ROMs gets three hundred blank tiles, and the only cure was
 * Find Cover, once per game. This runs the same lookup over all of them at once.
 *
 * It proposes and does not decide. A run writes nothing a player can see: it collects a match per
 * game and waits, and a separate step applies the ones the player kept. That is deliberate, because
 * a title search over a ROM set is right most of the time rather than always, and three hundred
 * wrong covers cost more to undo than to pick.
 *
 * The lookup itself is injected. Everything here is ordering, pacing, counting and the answer to
 * "how far along is it", so the whole engine is testable without a network.
 */
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace artwork_sweep {

  /// The pause between games, so a library of hundreds reaches the provider as a trickle.
  inline constexpr std::int64_t between_games_milliseconds = 150;

  /// How long the first wait after a rate limit lasts. Each further one doubles it.
  inline constexpr std::int64_t first_rate_limit_wait_milliseconds = 2'000;

  /**
   * @brief The longest a wait goes without looking at whether it has been cancelled.
   *
   * A rate limit wait reaches half a minute, and a run that only checked between waits would take
   * that long to stop. Nothing would be wrong with the answer, but the host would sit in a static
   * destructor at shutdown for as long as the wait had left to run.
   */
  inline constexpr std::int64_t cancel_check_interval_milliseconds = 250;

  /// How many times a run waits out a rate limit before it gives up on the rest.
  inline constexpr int maximum_rate_limit_waits = 5;

  /// The most games one run will look up, so a pathological library cannot run for an hour.
  inline constexpr std::size_t maximum_games_per_run = 500;

  /// Where a run is.
  enum class state_e {
    searching,  ///< still looking games up
    ready,  ///< finished, and its proposals are waiting to be applied or dropped
    failed,  ///< stopped early, and `message` says why
  };

  [[nodiscard]] inline std::string_view state_name(state_e state) {
    switch (state) {
      case state_e::searching:
        return "searching";
      case state_e::ready:
        return "ready";
      case state_e::failed:
        return "failed";
    }
    return "ready";
  }

  /// A game a run was asked about.
  struct candidate_t {
    std::string uuid;
    std::string name;  ///< the entry's name, which is what gets searched
  };

  /// The game a provider offered for one candidate.
  struct match_t {
    std::string provider_game_id;
    std::string title;
    int confidence = 0;  ///< how close the provider's title is to the one searched, 0 to 100
    std::optional<int> release_year;
  };

  /// What a run decided about one game.
  enum class outcome_e {
    proposed,  ///< there is a match with a poster, waiting for a player to keep or drop it
    no_match,  ///< the provider knows no game by that name
    no_poster,  ///< the provider knows the game and has no poster for it
    refused,  ///< the lookup itself failed, and `note` says how
    skipped,  ///< the run stopped before reaching this game
  };

  [[nodiscard]] inline std::string_view outcome_name(outcome_e outcome) {
    switch (outcome) {
      case outcome_e::proposed:
        return "proposed";
      case outcome_e::no_match:
        return "no_match";
      case outcome_e::no_poster:
        return "no_poster";
      case outcome_e::refused:
        return "refused";
      case outcome_e::skipped:
        return "skipped";
    }
    return "skipped";
  }

  /// One game's place in a run.
  struct proposal_t {
    std::string uuid;
    std::string name;
    outcome_e outcome = outcome_e::skipped;
    std::optional<match_t> match;
    std::string note;  ///< why there is nothing to propose, in a sentence a player can read
  };

  /// What one lookup answered.
  struct lookup_t {
    std::optional<match_t> match;  ///< empty when the provider knows no such game
    bool has_match_without_poster = false;  ///< the game is known and its poster is not
    bool rate_limited = false;  ///< the provider asked us to slow down; the run waits and retries
    std::string note;  ///< set when the lookup failed outright
    bool refused = false;
  };

  /// Look one game up. Runs on the sweep's own thread, so it may block.
  using lookup_fn_t = std::function<lookup_t(const candidate_t &)>;

  /// Wait, so a test can run a whole sweep without one.
  using sleep_fn_t = std::function<void(std::int64_t milliseconds)>;

  /// Epoch seconds, so a test can pin them.
  using clock_fn_t = std::function<std::int64_t()>;

  /// A snapshot of a run, safe to read while it is going.
  struct job_t {
    std::string id;  ///< opaque identity; a replacement run must not inherit an apply
    state_e state = state_e::ready;
    std::size_t total = 0;  ///< how many games the run was given
    std::size_t looked_at = 0;  ///< how many it has answered for
    std::size_t proposed = 0;  ///< how many of those have a match to keep
    std::string message;
    std::int64_t started_at = 0;
    std::int64_t finished_at = 0;  ///< 0 while it is still going
    std::vector<proposal_t> proposals;
  };

  enum class start_e {
    started,
    already_running,
    nothing_to_do,  ///< no games were given, so there is nothing to propose
  };

  /**
   * @brief The one run a host has at a time.
   *
   * One at a time on purpose. Two runs would race each other for the provider's patience and would
   * leave a player with two sets of proposals for overlapping games.
   */
  class sweeper_t {
  public:
    sweeper_t(lookup_fn_t lookup, sleep_fn_t sleep, clock_fn_t clock):
        lookup_ {std::move(lookup)}, sleep_ {std::move(sleep)}, clock_ {std::move(clock)} {}

    sweeper_t(const sweeper_t &) = delete;
    sweeper_t &operator=(const sweeper_t &) = delete;

    ~sweeper_t() {
      cancel();
      // A lookup may outlive a guessed timeout. Keep the owner and its mutex alive
      // until the worker has returned and released its thread-owned resources.
      if (worker_.joinable()) worker_.join();
    }

    /// Begin a run over these games. At most `maximum_games_per_run` of them are looked up.
    start_e start(std::vector<candidate_t> games, std::string id = {}) {
      std::unique_lock lock(mutex_);
      if (running_) {
        return start_e::already_running;
      }

      // Nothing to look up forgets the run before it, rather than leaving its proposals on screen.
      // Those proposals are about games that now have covers, and applying them a second time would
      // write over what is already there, including a cover somebody picked by hand.
      if (games.empty()) {
        job_ = job_t {};
        return start_e::nothing_to_do;
      }

      const bool clipped = games.size() > maximum_games_per_run;
      if (clipped) {
        games.resize(maximum_games_per_run);
      }

      const auto lookup = lookup_;
      const auto sleep = sleep_;
      job_t next;
      next.id = std::move(id);
      next.state = state_e::searching;
      next.total = games.size();
      next.started_at = now_seconds();
      next.message = clipped ? "Looking up the first " + std::to_string(games.size()) + " games without a cover." : "Looking up " + std::to_string(games.size()) + " games without a cover.";
      next.proposals.reserve(games.size());
      for (const auto &game : games) {
        next.proposals.push_back(proposal_t {game.uuid, game.name, outcome_e::skipped, std::nullopt, {}});
      }
      // All preparation that can throw finishes before reserving the worker.
      job_ = std::move(next);
      cancelled_ = false;
      running_ = true;
      auto previous = std::move(worker_);
      lock.unlock();
      // Reserve the new run before releasing the lock, so another start cannot
      // replace it while we join the old worker. Never join with mutex_ held.
      if (previous.joinable()) previous.join();
      lock.lock();
      try {
        worker_ = std::thread([this, games = std::move(games), lookup, sleep]() {
          try {
            run(games, lookup, sleep);
          } catch (...) {
            std::lock_guard failure_lock(mutex_);
            fail_run();
          }
        });
      } catch (...) {
        fail_run();
        throw;
      }
      return start_e::started;
    }

    [[nodiscard]] job_t job() const {
      std::lock_guard lock(mutex_);
      return job_;
    }

    /// Stop a run. It finishes the game it is already on and leaves what it has.
    void cancel() {
      std::lock_guard lock(mutex_);
      cancelled_ = true;
      // Observers in other browser tabs must retire automatic approval immediately,
      // even if an in-flight lookup later leaves a proposal for manual review.
      job_.id.clear();
    }

    /**
     * @brief Forget a finished run.
     * @return false while a run is still going, in which case nothing is forgotten.
     */
    bool clear() {
      std::lock_guard lock(mutex_);
      if (job_.state == state_e::searching) {
        return false;
      }
      job_ = job_t {};
      return true;
    }

    /// Wait for job processing to finish. Destruction also joins the worker thread.
    bool wait_for_idle(std::chrono::milliseconds timeout) {
      std::unique_lock lock(mutex_);
      return idle_.wait_for(lock, timeout, [this] { return !running_; });
    }

  private:
    // Called with mutex_ held. Keep provider exception text out of console state.
    void fail_run() {
      job_.state = state_e::failed;
      job_.message = "The cover search stopped after an unexpected error. Try again.";
      try {
        job_.finished_at = now_seconds();
      } catch (...) {
        job_.finished_at = job_.started_at;
      }
      running_ = false;
      idle_.notify_all();
    }

    [[nodiscard]] std::int64_t now_seconds() const {
      if (clock_) {
        return clock_();
      }
      return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
    }

    void run(const std::vector<candidate_t> &games, const lookup_fn_t &lookup, const sleep_fn_t &sleep) {
      const auto cancelled = [this] {
        std::lock_guard lock(mutex_);
        return cancelled_;
      };

      // Waits in slices, so being asked to stop is noticed within a quarter of a second however long
      // the wait is. An injected sleep is called once with the whole amount: a test has no thread to
      // stop and wants to see what was asked for.
      const auto pause = [&sleep, &cancelled](std::int64_t milliseconds) {
        if (milliseconds <= 0) {
          return;
        }
        if (sleep) {
          sleep(milliseconds);
          return;
        }
        std::int64_t left = milliseconds;
        while (left > 0) {
          const auto slice = std::min(left, cancel_check_interval_milliseconds);
          std::this_thread::sleep_for(std::chrono::milliseconds {slice});
          left -= slice;
          if (cancelled()) {
            return;
          }
        }
      };

      std::string stopped_because;
      int waits = 0;
      std::size_t index = 0;
      const auto stopped_here = [&games](std::size_t at) {
        return "Stopped before " + std::to_string(games.size() - at) + " of the games were looked up.";
      };
      for (; index < games.size(); ++index) {
        if (cancelled()) {
          stopped_because = stopped_here(index);
          break;
        }
        if (index > 0) {
          pause(between_games_milliseconds);
          if (cancelled()) {
            stopped_because = stopped_here(index);
            break;
          }
        }

        auto answer = lookup ? lookup(games[index]) : lookup_t {};

        // A provider that asks us to slow down is answered by slowing down, each wait longer than
        // the last. The budget belongs to the whole run rather than to each game, so a host that is
        // being refused outright stops after a minute of trying instead of once per game.
        //
        // Nothing else in this codebase waits out a rate limit, because nothing else makes hundreds
        // of requests in a row. A run does, so the first refusal would otherwise cost every game
        // after it.
        bool stopped_waiting = false;
        while (answer.rate_limited && waits < maximum_rate_limit_waits) {
          pause(first_rate_limit_wait_milliseconds << waits);
          ++waits;
          stopped_waiting = cancelled();
          if (stopped_waiting) {
            break;
          }
          answer = lookup ? lookup(games[index]) : lookup_t {};
        }

        // Asked to stop while waiting out a rate limit. The answer in hand still says rate limited,
        // so without this the run would tell the player the provider refused them when what actually
        // happened is that they pressed Stop.
        if (stopped_waiting) {
          stopped_because = stopped_here(index);
          break;
        }
        if (answer.rate_limited) {
          stopped_because = "The artwork provider is rate limiting this host. Try the rest in a few minutes.";
          break;
        }

        record(index, answer);
      }

      std::lock_guard lock(mutex_);
      job_.state = stopped_because.empty() ? state_e::ready : state_e::failed;
      if (!stopped_because.empty()) {
        job_.message = stopped_because;
      }
      else {
        job_.message = summary(job_.proposed, job_.total);
      }
      job_.finished_at = now_seconds();
      running_ = false;
      idle_.notify_all();
    }

    void record(std::size_t index, const lookup_t &answer) {
      std::lock_guard lock(mutex_);
      if (index >= job_.proposals.size()) {
        return;
      }
      auto &proposal = job_.proposals[index];
      if (answer.refused) {
        proposal.outcome = outcome_e::refused;
        proposal.note = answer.note.empty() ? "The artwork provider could not be reached." : answer.note;
      }
      else if (answer.match) {
        proposal.outcome = outcome_e::proposed;
        proposal.match = answer.match;
        ++job_.proposed;
      }
      else if (answer.has_match_without_poster) {
        proposal.outcome = outcome_e::no_poster;
        proposal.note = "The provider knows this game and has no poster for it.";
      }
      else {
        proposal.outcome = outcome_e::no_match;
        proposal.note = "No game by that name.";
      }
      ++job_.looked_at;
    }

    [[nodiscard]] static std::string summary(std::size_t proposed, std::size_t total) {
      if (proposed == 0) {
        return "Found no covers for the " + std::to_string(total) + " games without one.";
      }
      return "Found a cover for " + std::to_string(proposed) + " of " + std::to_string(total) + " games.";
    }

    mutable std::mutex mutex_;
    std::condition_variable idle_;
    std::thread worker_;
    lookup_fn_t lookup_;
    sleep_fn_t sleep_;
    clock_fn_t clock_;
    job_t job_;
    bool running_ = false;
    bool cancelled_ = false;
  };

}  // namespace artwork_sweep
