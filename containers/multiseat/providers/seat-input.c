#include "seat-input.h"
#include <stdint.h>

/* These ranges are part of the pinned inputtino host mouse contract. */
#define ABS_WIDTH 19200
#define ABS_HEIGHT 12000

static bool valid(const struct seat_input *state, const struct input_event *event) {
  if (event->type == EV_MSC) return event->code == MSC_SCAN;
  if (event->type == EV_KEY) {
    if (event->value < 0 || event->value > 2) return false;
    return state->role == SEAT_KEYBOARD ? event->code > 0 && event->code <= KEY_MAX :
      event->code >= BTN_LEFT && event->code <= BTN_TASK && event->value <= 1;
  }
  if (state->role == SEAT_RELATIVE && event->type == EV_REL) {
    return (event->code == REL_X || event->code == REL_Y || event->code == REL_WHEEL ||
      event->code == REL_HWHEEL || event->code == REL_WHEEL_HI_RES || event->code == REL_HWHEEL_HI_RES) &&
      event->value >= -32768 && event->value <= 32767;
  }
  if (state->role == SEAT_ABSOLUTE && event->type == EV_ABS) {
    return event->value >= 0 && ((event->code == ABS_X && event->value <= ABS_WIDTH) ||
                                (event->code == ABS_Y && event->value <= ABS_HEIGHT));
  }
  /* EV_REP and LED notifications do not carry client input. */
  return state->role == SEAT_KEYBOARD && (event->type == EV_REP || event->type == EV_LED);
}

bool seat_input_event(struct seat_input *state, const struct input_event *event, seat_send send, void *opaque) {
  if (!state || !event || !send || state->role > SEAT_ABSOLUTE || !state->width || !state->height) return false;
  if (event->type != EV_SYN) {
    if (!valid(state, event) || state->count >= 256) return false;
    state->pending[state->count++] = *event;
    return true;
  }
  if (event->code != SYN_REPORT) return false; /* Includes SYN_DROPPED: fail this stream. */
  int64_t dx = 0, dy = 0, wheel_x = 0, wheel_y = 0, high_x = 0, high_y = 0;
  bool have_high_x = false, have_high_y = false, absolute = false;
  for (size_t i = 0; i < state->count; ++i) {
    const struct input_event *e = &state->pending[i];
    if (e->type == EV_REL) switch (e->code) {
      case REL_X: dx += e->value; break;
      case REL_Y: dy += e->value; break;
      case REL_HWHEEL: wheel_x += e->value; break;
      case REL_WHEEL: wheel_y += e->value; break;
      case REL_HWHEEL_HI_RES: have_high_x = true; high_x += e->value; break;
      case REL_WHEEL_HI_RES: have_high_y = true; high_y += e->value; break;
    }
    if (e->type == EV_ABS) {
      if (e->code == ABS_X) state->absolute_x = e->value;
      if (e->code == ABS_Y) state->absolute_y = e->value;
      absolute = true;
    }
  }
  struct seat_command command = {0};
  if (dx || dy) {
    command.kind = SEAT_MOVE_RELATIVE; command.x = dx; command.y = dy;
    if (!send(opaque, &command)) return false;
  }
  if (absolute) {
    command.kind = SEAT_MOVE_ABSOLUTE;
    command.x = (double)state->absolute_x * (state->width - 1) / ABS_WIDTH;
    command.y = (double)state->absolute_y * (state->height - 1) / ABS_HEIGHT;
    if (!send(opaque, &command)) return false;
  }
  for (size_t i = 0; i < state->count; ++i) {
    const struct input_event *e = &state->pending[i];
    if (e->type != EV_KEY || e->value == 2) continue;
    bool pressed = e->value == 1;
    if (state->pressed[e->code] == pressed) continue;
    command = (struct seat_command){.kind = state->role == SEAT_KEYBOARD ? SEAT_KEY : SEAT_BUTTON,
                                    .code = e->code, .pressed = pressed};
    if (!send(opaque, &command)) return false;
    state->pressed[e->code] = pressed;
  }
  if (wheel_x || wheel_y || high_x || high_y) {
    command = (struct seat_command){.kind = SEAT_AXIS,
      .x = have_high_x ? high_x : wheel_x * 120, .y = -(have_high_y ? high_y : wheel_y * 120)};
    if (!send(opaque, &command)) return false;
  }
  state->count = 0;
  return true;
}

void seat_input_release(struct seat_input *state, seat_send send, void *opaque) {
  if (!state || !send) return;
  state->count = 0;
  for (unsigned code = 0; code <= KEY_MAX; ++code) {
    if (!state->pressed[code]) continue;
    struct seat_command command = {.kind = state->role == SEAT_KEYBOARD ? SEAT_KEY : SEAT_BUTTON, .code = code};
    (void)send(opaque, &command);
    state->pressed[code] = false;
  }
}
