# DualSense report ordering and thread ownership

The canonical inputtino pin is `504f0abc7da8ebc351f8300fb2ed98db5438ee48`.
CMake checks the original hashes and applies this backport to a build-directory
copy. A changed dependency fails configuration until the patch is reviewed.

The periodic sender can serialize old input, then deliver it after a newer
button report. A per-pad lock now spans state mutation, serialization, sequence
updates and the UHID write. Callback registration uses a separate lock; copied
callbacks execute outside it. Sender and UHID reader threads remain joinable,
stop flags are atomic, and moving a pad transfers its sender. Teardown joins the
threads before releasing the device and descriptor. Set the transport mode before
starting its reader and keep a descriptor guard until reader/device ownership
is established.

Callbacks execute on the reader thread. They may register callbacks or submit
input; destroying the pad or stopping its reader must remain on an owning
thread outside the callback. Polaris feedback callbacks already follow this
ownership rule.

This addresses a reproduced report-order defect during investigation of #634.
The reporter's selected virtual pad and kernel-side trace are still needed to
establish whether it caused that report. Xbox and Switch report behavior is
unchanged. Kernel input delivery, Steam recognition and physical rumble require
separate hardware validation.

Run the small device-free suite independently on Linux:

```sh
cmake -S tests/fixtures/inputtino -B build-inputtino
cmake --build build-inputtino -j2
ctest --test-dir build-inputtino --output-on-failure
```

The report test substitutes UHID transport and exercises the real report code,
repeat loop, moves and teardown. The device test uses private sockets while
exercising the real UHID poll/read/join owner, including a callback in flight and
construction failures. Neither test opens an input node. Both run in the native
ASan/UBSan CI lane. Use a separate build with `-fsanitize=thread` for TSan.

## uinput physical identity (#494)

The second patch adapts the creation-helper approach from inputtino PR #48 at
`8da0031e73217706931b5a4f2902d7bec4e01f58` to the canonical pin. All nine uinput
creation sites pass the existing `device_phys` value. A nonempty value uses one
`O_CLOEXEC` descriptor for `UI_SET_PHYS` and device creation; any failure rejects
the device instead of dropping its identity. The shared handle destroys the
virtual device before closing that descriptor. Empty values retain libevdev's
managed-descriptor behavior. Names, input reports, permissions, and the normal
UHID PS5 path retain their existing contracts.

The reserved udev markers and the multiseat inventory checks already exist;
this patch supplies their requested kernel metadata. It does not change device
mount admission or establish separation from other processes with the same UID
or input-group authority. Actual Linux sysfs/udev and desktop input behavior
remain physical acceptance gates.

## DualSense resting axes (#634)

The third patch gives every axis in `dualsense_input_report` its own
initializer. A comma-separated declaration binds the initializer only to the
member it follows, so `x`, `rx` and `z` had none: a freshly created pad reported
both sticks hard left, and `rz` took the centre value, which for a trigger means
half pulled. Nothing corrected any of it until the client sent its first stick
or trigger update, because the periodic sender starts as soon as the pad exists.

Triggers rest at `PS5_AXIS_MIN` rather than `PS5_AXIS_NEUTRAL`, matching what
`set_triggers(0, 0)` scales to. The Xbox and Switch pads were measured resting
at zero on every axis and are unchanged; only the UHID DualSense was affected.

`src/uhid/include/uhid/ps5.hpp` joins the hashed inputs with this patch. The
first patch already edits that header, so an upstream change to it was only
caught as a patch failure rather than as a named review prompt.

`report_order.cpp` asserts the resting report directly: it creates a pad,
submits nothing, and reads the first report the periodic sender emits.
