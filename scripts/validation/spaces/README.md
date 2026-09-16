# Spaces security acceptance in a disposable VM

Use this check only in a disposable Fedora QEMU/KVM guest. It installs and removes
live SELinux policy, interrupts its own installer processes, and creates temporary
virtual input devices. A dedicated marker and virtualization check prevent an
accidental run on a physical gaming host. No GPU or physical controller is used.
SELinux must remain enforcing for every case.

## Prepare the guest

1. Obtain a Fedora Cloud Base image from the [official download page](https://fedoraproject.org/cloud/download/).
   Verify the image against Fedora's signed checksum, and retain both identities.
2. Boot a private QEMU/KVM guest with a normal user, an SSH key, at least 2 GiB RAM,
   and a private writable disk. Do not share host directories, input devices, or
   GPUs. If forwarding SSH, bind the forwarding port to loopback only.
3. Install the candidate native Polaris package in the guest. For a focused helper
   test, the exact CMake-staged helper, multiseat policy data, and packaged
   `60-polaris.rules` may be installed instead. Record which method was used;
   staged-file acceptance does not prove RPM installation or dependency closure.
   Stop Polaris before running the check.
4. Install the policy build tools in the guest:
   ```sh
   sudo dnf install selinux-policy-devel container-selinux make
   ```
5. Start with no Spaces policy modules or dedicated input rule. Do not remove
   unknown policies to meet this condition; use a fresh test guest instead.
6. Inside that guest, explicitly mark it as disposable:
   ```sh
   printf '%s\n' polaris-spaces-security-acceptance-v1 | sudo tee /etc/polaris-spaces-disposable-test
   ```

## Run and retain evidence

Obtain the expected helper SHA-256 from the candidate artifact before copying or
installing it in the guest. Use the actual candidate source commit. In the guest:

```sh
sudo python3 scripts/validation/spaces/security_vm.py \
  --helper /usr/bin/polaris-spaces-setup \
  --reader spacescheck \
  --expected-helper-sha256 EXPECTED_HELPER_SHA256 \
  --source-commit EXACT_SOURCE_COMMIT
```

Set `--reader` to the guest's normal user, and change `--helper` only if the
candidate was configured with a different install prefix. The check rejects a
helper whose bytes differ from the supplied artifact digest. Results go to
`/var/tmp/spaces-security-acceptance.json`. Keep console output and failure logs
with that receipt.

The check covers actual kernel policy installation, idempotency, unprivileged
readiness, private journal permissions, active-process and input-device refusal,
reserved versus ordinary input labels, and worker read access. It verifies that
the worker cannot write or grab its reserved input, or read ordinary input.
Locally changed rules and administrator-owned modules must remain untouched.
For install and removal, the test pauses the caller while its real `semodule`
child commits, kills that caller, and verifies that repeating the same operation
completes the saved transaction. No installer boundary is replaced by a fake.

## Reboot and cleanup

A successful run leaves the owned policies installed for a persistence check.
Record the current guest boot ID, then reboot **the guest** as a separate step.
After reconnecting, verify a different boot ID, SELinux enforcing, and successful
`polaris-spaces-setup status` as the normal user. Retain these results alongside
the acceptance receipt.

Finish with `sudo polaris-spaces-setup remove` inside the guest. Confirm that the
three Spaces modules and dedicated input rule are absent, and that unprivileged
status reports not ready. Shut down the guest and retain or discard its private
disk according to the test run's retention policy. The script does not shut down
or remove a VM automatically.

This acceptance covers the security helper and its input policy. GPU encoding,
Steam sign-in, game audio, controller gameplay, and fresh Arch host acceptance
remain separate physical validation gates.
