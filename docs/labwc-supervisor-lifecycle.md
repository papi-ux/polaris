# Private compositor supervisor lifecycle

The Linux labwc supervisor starts by spawning the current executable through
`/proc/self/exe` into its private internal supervisor entry point. This entry
point runs before host configuration, logging, or worker initialization. It
avoids carrying mutex ownership and initialized libraries from a multithreaded
host into a long-lived, unexecuted fork child.

The host prepares the executable arguments, GPU selection, and an owned
environment snapshot before spawning. Host environment setters and apps.json's
native Boost environment updates share the snapshot mutex. App expansion uses
an owned copy after those updates, preserving the existing host environment
contract without holding the mutex throughout app parsing.

The spawn establishes a new session, resets the signal mask and termination
handlers, redirects standard descriptors to `/dev/null`, remaps optional startup
diagnostics to descriptors 3 and 4, and closes every remaining descriptor inside
the spawn operation. Linux builds require glibc's `posix_spawn_file_actions_addclosefrom_np`
(glibc 2.34 or newer), already covered by the supported Ubuntu 22.04 and newer
runtime baseline. No extra installed helper executable is required.

Re-executing a host installed with file capabilities can regain those
capabilities. The supervisor clears ambient, permitted, effective, and inheritable
capabilities and restores same-user `/proc` readability before starting labwc.
It preserves inherited `no_new_privs` policy so existing Steam and bubblewrap
sandbox helpers retain their prior launch behavior.

The supervisor retains the existing subreaper, PID/session ownership, adopted
child handling, and bounded TERM/KILL teardown. Tests exercise actual supervisor
execution, descriptor inheritance, environment separation, diagnostic stderr and
exit status, compositor exit, relaunch, and descendants that leave the process
group. A separate isolated test runs a copied executable with `cap_sys_admin+ep`,
drops parent capabilities before launch, and verifies that the new supervisor has
no capabilities and remains readable. The temporary executable and its file
capability are removed afterward. Native, ASan/UBSan, and targeted TSan evidence
is retained outside the source tree; this is not a game-stream acceptance result.
