/**
 * @file tests/unit/test_gamepad_feedback_router.cpp
 * @brief Regression coverage for feedback routing across preallocated gamepad adoption.
 */
#include "../tests_common.h"

#include "src/platform/gamepad_feedback_router.h"

TEST(GamepadFeedbackRouter, TransfersQueuedAndFutureFeedbackWhenRebound) {
  auto prelaunch_mail = std::make_shared<safe::mail_raw_t>();
  auto live_mail = std::make_shared<safe::mail_raw_t>();
  auto prelaunch = prelaunch_mail->queue<platf::gamepad_feedback_msg_t>(mail::gamepad_feedback);
  auto live = live_mail->queue<platf::gamepad_feedback_msg_t>(mail::gamepad_feedback);
  platf::gamepad_feedback_router_t router {prelaunch};

  router.raise(platf::gamepad_feedback_msg_t::make_motion_event_state(0, LI_MOTION_TYPE_ACCEL, 100));
  router.raise(platf::gamepad_feedback_msg_t::make_motion_event_state(0, LI_MOTION_TYPE_GYRO, 100));
  ASSERT_TRUE(prelaunch->peek());
  EXPECT_FALSE(live->peek());

  router.rebind(live);
  EXPECT_FALSE(prelaunch->peek());

  auto accel = live->pop();
  auto gyro = live->pop();
  ASSERT_TRUE(accel);
  ASSERT_TRUE(gyro);
  EXPECT_EQ(accel->type, platf::gamepad_feedback_e::set_motion_event_state);
  EXPECT_EQ(accel->data.motion_event_state.motion_type, LI_MOTION_TYPE_ACCEL);
  EXPECT_EQ(gyro->type, platf::gamepad_feedback_e::set_motion_event_state);
  EXPECT_EQ(gyro->data.motion_event_state.motion_type, LI_MOTION_TYPE_GYRO);

  router.raise(platf::gamepad_feedback_msg_t::make_rumble(0, 1000, 2000));
  auto rumble = live->pop();
  ASSERT_TRUE(rumble);
  EXPECT_EQ(rumble->type, platf::gamepad_feedback_e::rumble);
  EXPECT_EQ(rumble->data.rumble.lowfreq, 1000);
  EXPECT_EQ(rumble->data.rumble.highfreq, 2000);
  EXPECT_FALSE(prelaunch->peek());
}

TEST(GamepadFeedbackRouter, RebindingToCurrentQueueIsANoOp) {
  auto mail = std::make_shared<safe::mail_raw_t>();
  auto queue = mail->queue<platf::gamepad_feedback_msg_t>(mail::gamepad_feedback);
  platf::gamepad_feedback_router_t router {queue};

  router.raise(platf::gamepad_feedback_msg_t::make_rgb_led(0, 1, 2, 3));
  router.rebind(queue);

  auto message = queue->pop();
  ASSERT_TRUE(message);
  EXPECT_EQ(message->type, platf::gamepad_feedback_e::set_rgb_led);
  EXPECT_FALSE(queue->peek());
}
