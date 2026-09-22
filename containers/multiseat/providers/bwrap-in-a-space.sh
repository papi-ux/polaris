#!/bin/sh
# /usr/bin/bwrap inside a launcher image. The packaged one is beside it as
# bwrap.real and everything but one request goes straight to it.
#
# GTK loads every icon through glycin, and glycin decodes each one inside a
# bwrap sandbox of its own that unshares the network. A Space is already a
# sandbox and refuses exactly that: setting up the loopback of a new network
# namespace is network administration, which the worker's SELinux domain
# withholds on purpose, and the devpts mount that follows is refused as well.
# bwrap then dies with "loopback: Failed RTM_NEWADDR", glycin reports a broken
# loader instead of an unavailable sandbox, GTK cannot draw even its
# missing-image icon, and the launcher aborts before its first window.
#
# glycin already has a path for a host that allows no such sandbox: it runs the
# loader directly when bwrap fails with one of five sentences it knows. It has
# no switch for it, in an environment variable or anywhere else, so the refusal
# is given here in one of those sentences. Proton's and Steam's own use of
# bwrap never unshares the network and is passed through untouched.
#
# Only the run of flags that opens the command line is read, which is where
# glycin puts --unshare-all. bwrap's options take values, and telling a value
# from an option needs its whole grammar; stopping at the first word that is
# not a flag means a value or a program argument that happens to spell
# --unshare-net is never mistaken for the request.
for argument in "$@"; do
  case "$argument" in
    --unshare-all|--unshare-net)
      echo "bwrap: No permissions to create new namespace, likely because this is a Polaris Space, which is a sandbox already and gives no process a network namespace of its own." >&2
      exit 1 ;;
    --) break ;;
    --*) ;;
    *) break ;;
  esac
done
exec /usr/bin/bwrap.real "$@"
