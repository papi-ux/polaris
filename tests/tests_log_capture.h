/**
 * @file tests/tests_log_capture.h
 * @brief Collects what Polaris logs while a test runs, for tests about what an operator reads.
 */
#pragma once

#include <array>
#include <sstream>
#include <string>

#include <boost/core/null_deleter.hpp>
#include <boost/log/attributes/value_extraction.hpp>
#include <boost/log/core.hpp>
#include <boost/log/core/record_view.hpp>
#include <boost/log/utility/formatting_ostream.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/log/sinks/text_ostream_backend.hpp>
#include <boost/smart_ptr/make_shared_object.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>

/**
 * Every message logged from any thread while it is alive, one per line, led by
 * its level as the host log prints it ("Warning: ") and without the timestamp,
 * so a test can ask whether a line says what it should at the level it should.
 */
class test_log_capture_t {
public:
  test_log_capture_t():
      stream_ {boost::make_shared<std::ostringstream>()} {
    auto backend = boost::make_shared<boost::log::sinks::text_ostream_backend>();
    backend->add_stream(boost::shared_ptr<std::ostream> {stream_.get(), boost::null_deleter {}});
    backend->auto_flush(true);
    sink_ = boost::make_shared<sink_t>(backend);
    sink_->set_formatter([](const boost::log::record_view &view, boost::log::formatting_ostream &out) {
      static constexpr std::array<const char *, 6> levels {"Verbose", "Debug", "Info", "Warning", "Error", "Fatal"};
      if (const auto level = view.attribute_values()["Severity"].extract<int>(); level && level.get() >= 0 && level.get() < 6)
        out << levels[static_cast<std::size_t>(level.get())] << ": ";
      if (const auto message = view.attribute_values()["Message"].extract<std::string>()) out << message.get();
    });
    boost::log::core::get()->add_sink(sink_);
  }

  ~test_log_capture_t() {
    boost::log::core::get()->remove_sink(sink_);
  }

  test_log_capture_t(const test_log_capture_t &) = delete;
  test_log_capture_t &operator=(const test_log_capture_t &) = delete;

  [[nodiscard]] std::string text() const {
    // The backend lock orders this read after any write another thread is making.
    const auto locked = sink_->locked_backend();
    return stream_->str();
  }

private:
  using sink_t = boost::log::sinks::synchronous_sink<boost::log::sinks::text_ostream_backend>;
  boost::shared_ptr<std::ostringstream> stream_;
  boost::shared_ptr<sink_t> sink_;
};
