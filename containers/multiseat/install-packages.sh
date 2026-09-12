#!/bin/sh
set -eu
sh /verify-offline.sh /inputs
# Prevent image-build package scripts from starting daemons.
printf '#!/bin/sh\nexit 101\n' > /usr/sbin/policy-rc.d
chmod 0755 /usr/sbin/policy-rc.d
export DEBIAN_FRONTEND=noninteractive
for package in /inputs/packages/*_i386.deb; do
  if [ -f "$package" ]; then dpkg --add-architecture i386; break; fi
done
# APT orders pre-dependencies correctly on the minimal base. Every candidate is
# a verified local file; downloading, removal and recommendations are forbidden.
apt-get -o Dir::Cache::archives=/inputs/packages --yes --no-download --no-remove --no-install-recommends install /inputs/packages/*.deb
python3 /verify-inputs.py "$1"
test -z "$(dpkg --audit)"
rm /usr/sbin/policy-rc.d
