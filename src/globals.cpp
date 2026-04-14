/**
 * @file globals.cpp
 * @brief Definitions for globally accessible variables and functions.
 */
// local includes
#include "globals.h"

safe::mail_t mail::man;
thread_pool_util::ThreadPool task_pool;
bool display_cursor = false;

namespace cursor {
  void set_visible(bool visible) {
    display_cursor = visible;
  }

  bool visible() {
    return display_cursor;
  }

  bool toggle_visible() {
    display_cursor = !display_cursor;
    return display_cursor;
  }
}  // namespace cursor

#ifdef _WIN32
nvprefs::nvprefs_interface nvprefs_instance;
#endif
