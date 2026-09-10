#!/bin/sh
set -eu
python3 /verify-inputs.py "$1"
# Prevent image-build package scripts from starting daemons.
printf '#!/bin/sh\nexit 101\n' > /usr/sbin/policy-rc.d
chmod 0755 /usr/sbin/policy-rc.d
export DEBIAN_FRONTEND=noninteractive
dpkg --force-confold --install /inputs/packages/*.deb
test -z "$(dpkg --audit)"
rm /usr/sbin/policy-rc.d
