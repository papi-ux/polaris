# Optional SELinux policy for reserved input nodes

Use this only when an enforcing SELinux host independently denies a correctly
mounted event node after the launching service and worker have been shown to
retain their actual supplementary groups. The runtime uses trusted crun,
`--userns=keep-id`, an explicit launching UID, and `--group-add=keep-groups`.
An account database entry alone does not establish running-process membership.

The CIL module allows containers to open and read one dedicated device type.
The companion udev rule labels only event nodes with the reserved
`Polaris multiseat ` name and `seat-polaris` assignment from `60-polaris.rules`.
The controller must still authenticate each allocation and mount only its
exact devices. Three evdev ioctls query the version, kernel name and physical
identity (`0x4501`, `0x4506`, `0x4507`). Device writes, grabs, revocation,
generic input access and access to uinput/uhid remain unavailable. This policy
does not enable any multiseat adapter.

Administrators opt in explicitly, with all multiseat workers stopped. Confirm
that the module and destination rule are absent first; preserve any existing
local policy instead of overwriting it. Record the installed module checksum
and rule hash so removal can verify ownership of both artifacts. After loading,
inspect effective permissions, including those inherited through `device_node`.
Only read/open/getattr and ioctl with those three extended permissions should
be granted to `container_t` for this type:

```sh
sudo semodule -i containers/multiseat/selinux/polaris_multiseat_input.cil
sudo install -o root -g root -m 0644 \
  containers/multiseat/selinux/97-polaris-multiseat-input.rules \
  /etc/udev/rules.d/97-polaris-multiseat-input.rules
sudo udevadm control --reload-rules
```

Create fresh allocations, verify their labels, then run the isolated physical
harness. Do not relabel the host's existing event nodes or trigger every input
device. Keep SELinux enforcing and the `container_use_devices` boolean off.

To remove the policy, first stop every multiseat worker and verify every
reserved virtual device was destroyed. Remove only the matching installed
rule, reload udev rules, then run `sudo semodule -r polaris_multiseat_input`.
No package or service installs this policy automatically.
