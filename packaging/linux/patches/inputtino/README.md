# DualSense report ordering and thread ownership

The canonical inputtino pin is `504f0abc7da8ebc351f8300fb2ed98db5438ee48`.
CMake checks the original hashes and applies this backport to a build-directory
copy. A changed dependency fails configuration until the patch is reviewed.

The periodic sender can serialize old input, then deliver it after a newer
button report. A per-pad lock now spans state mutation, serialization, sequence
updates and the UHID write. Callback registration uses a separate lock; copied
callbacks execute outside it. Sender and UHID reader threads remain joinable,
stop flags are atomic, and moving a pad transfers its sender. Teardown joins the
threads before releasing the device and descriptor.

This addresses a reproduced report-order defect during investigation of #634.
The reporter's selected virtual pad and kernel-side trace are still needed to
establish whether it caused that report. Xbox and Switch report behavior is
unchanged. Kernel input delivery, Steam recognition and physical rumble require
separate hardware validation.
