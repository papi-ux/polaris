/* Fixed host-created evdev roles; no device discovery or injection authority. */
#ifndef POLARIS_SEAT_INPUT_H
#define POLARIS_SEAT_INPUT_H
#include <linux/input.h>
#include <stdbool.h>
#include <stddef.h>

enum seat_role { SEAT_KEYBOARD, SEAT_RELATIVE, SEAT_ABSOLUTE };
enum seat_command_kind { SEAT_KEY, SEAT_BUTTON, SEAT_MOVE_RELATIVE, SEAT_MOVE_ABSOLUTE, SEAT_AXIS };
struct seat_command { enum seat_command_kind kind; unsigned code; bool pressed; double x, y; };
typedef bool (*seat_send)(void *, const struct seat_command *);
struct seat_input {
  enum seat_role role;
  struct input_event pending[256]; size_t count;
  bool pressed[KEY_MAX + 1];
  int absolute_x, absolute_y;
  unsigned width, height;
};
bool seat_input_event(struct seat_input *, const struct input_event *, seat_send, void *);
void seat_input_release(struct seat_input *, seat_send, void *);
#endif
