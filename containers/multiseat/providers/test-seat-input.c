#include "seat-input.h"
#include <assert.h>
#include <string.h>

struct output { struct seat_command values[32]; size_t count; };
static bool send(void *opaque, const struct seat_command *command) {
  struct output *out = opaque;
  assert(out->count < 32); out->values[out->count++] = *command; return true;
}
static bool event(struct seat_input *state, struct output *out, unsigned type, unsigned code, int value) {
  struct input_event input = {.type = type, .code = code, .value = value};
  return seat_input_event(state, &input, send, out);
}
int main(void) {
  struct output out = {0};
  struct seat_input keyboard = {.role = SEAT_KEYBOARD, .width = 1280, .height = 720};
  assert(event(&keyboard,&out,EV_KEY,KEY_W,1)); assert(out.count == 0);
  assert(event(&keyboard,&out,EV_SYN,SYN_REPORT,0));
  assert(out.count == 1 && out.values[0].kind == SEAT_KEY && out.values[0].code == KEY_W && out.values[0].pressed);
  assert(event(&keyboard,&out,EV_KEY,KEY_W,2)); assert(event(&keyboard,&out,EV_SYN,SYN_REPORT,0)); assert(out.count == 1);
  seat_input_release(&keyboard,send,&out); assert(out.count == 2 && !out.values[1].pressed);
  assert(!event(&keyboard,&out,EV_KEY,KEY_W,3));
  assert(!event(&keyboard,&out,EV_SYN,SYN_DROPPED,0));
  assert(!event(&keyboard,&out,EV_REL,REL_X,1));
  struct seat_input mouse = {.role = SEAT_RELATIVE, .width = 1280, .height = 720}; out.count = 0;
  assert(event(&mouse,&out,EV_REL,REL_X,7)); assert(event(&mouse,&out,EV_REL,REL_Y,-5));
  assert(event(&mouse,&out,EV_REL,REL_WHEEL,1)); assert(event(&mouse,&out,EV_REL,REL_WHEEL_HI_RES,120));
  assert(event(&mouse,&out,EV_KEY,BTN_LEFT,1)); assert(event(&mouse,&out,EV_SYN,SYN_REPORT,0));
  assert(out.count == 3 && out.values[0].x == 7 && out.values[0].y == -5);
  assert(out.values[1].kind == SEAT_BUTTON && out.values[1].code == BTN_LEFT);
  assert(out.values[2].kind == SEAT_AXIS && out.values[2].y == -120); /* Never double-count paired wheel events. */
  struct seat_input absolute = {.role = SEAT_ABSOLUTE, .width = 1280, .height = 720}; out.count = 0;
  assert(event(&absolute,&out,EV_ABS,ABS_X,19200)); assert(event(&absolute,&out,EV_ABS,ABS_Y,12000));
  assert(event(&absolute,&out,EV_SYN,SYN_REPORT,0));
  assert(out.count == 1 && out.values[0].x == 1279 && out.values[0].y == 719);
  assert(!event(&absolute,&out,EV_ABS,ABS_Y,12001)); assert(!event(&absolute,&out,EV_ABS,ABS_X,-1));
  for (int i=0;i<256;++i) assert(event(&keyboard,&out,EV_MSC,MSC_SCAN,1));
  assert(!event(&keyboard,&out,EV_KEY,KEY_W,1));
  return 0;
}
