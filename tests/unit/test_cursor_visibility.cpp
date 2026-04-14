/**
 * @file tests/unit/test_cursor_visibility.cpp
 * @brief Test cursor visibility helpers.
 */
#include "../tests_common.h"

#include <src/globals.h>

namespace {
  class CursorVisibilityTest: public testing::Test {
  protected:
    void SetUp() override {
      original_visibility = cursor::visible();
      cursor::set_visible(false);
    }

    void TearDown() override {
      cursor::set_visible(original_visibility);
    }

    bool original_visibility = false;
  };
}  // namespace

TEST_F(CursorVisibilityTest, SetVisibleUpdatesState) {
  EXPECT_FALSE(cursor::visible());

  cursor::set_visible(true);
  EXPECT_TRUE(cursor::visible());

  cursor::set_visible(false);
  EXPECT_FALSE(cursor::visible());
}

TEST_F(CursorVisibilityTest, ToggleVisibleFlipsState) {
  EXPECT_TRUE(cursor::toggle_visible());
  EXPECT_TRUE(cursor::visible());

  EXPECT_FALSE(cursor::toggle_visible());
  EXPECT_FALSE(cursor::visible());
}
