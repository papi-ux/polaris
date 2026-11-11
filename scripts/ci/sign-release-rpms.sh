#!/usr/bin/env bash
# Only the public release job supplies the provisioned repository signer.
set -euo pipefail
shopt -s nullglob
export LC_ALL=C LANG=C

die() { printf '%s\n' "$*" >&2; exit 1; }
expected=58017EDFFA9F803E07ED26F835F13F14FAAD15CC
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
public_key="$repo_root/packaging/linux/keys/polaris.gpg"
assets="${1:?Supply the finalized release asset directory}"
[[ $# == 1 && -d "$assets" ]] || die 'Supply one finalized release asset directory.'
[[ -n "${POLARIS_REPO_GPG_PRIVATE_KEY:-}" ]] || die 'POLARIS_REPO_GPG_PRIVATE_KEY is required; release RPMs must be signed.'
[[ "${POLARIS_REPO_GPG_KEY_ID:-}" == "$expected" ]] || die 'The configured signing fingerprint is not the trusted Polaris key.'

packages=()
for release in 44 45; do
    main="$assets/Polaris-fedora${release}-x86_64.rpm"
    kms="$assets/Polaris-kms-fedora${release}-x86_64.rpm"
    if [[ -e "$main" || -e "$kms" ]]; then
        [[ -f "$main" && ! -L "$main" && -f "$kms" && ! -L "$kms" ]] || die 'A finalized Fedora package pair is incomplete.'
        packages+=("$main" "$kms")
    fi
done
[[ ${#packages[@]} -gt 0 ]] || die 'No finalized Fedora package pairs are present.'
python3 - "$public_key" <<'PY'
import hashlib
import sys
from pathlib import Path
assert hashlib.sha256(Path(sys.argv[1]).read_bytes()).hexdigest() == '3387618dcfa246abb5c53abc3a27893791a2de34916fc0954e6195b3a3bb0fe1', 'packaged public signing key changed'
PY

umask 077
work="$(mktemp -d "${RUNNER_TEMP:-${TMPDIR:-/tmp}}/polaris-rpm-sign.XXXXXXXX")"
export GNUPGHOME="$work/gnupg"
mkdir "$GNUPGHOME" "$work/rpmdb"
cleanup() {
    gpgconf --homedir "$GNUPGHOME" --kill gpg-agent || true
    rm -rf -- "$work" # exclusively created above; contains only this signing context
}
trap cleanup EXIT
printf '%s' "$POLARIS_REPO_GPG_PRIVATE_KEY" | gpg --batch --quiet --import
unset POLARIS_REPO_GPG_PRIVATE_KEY
keys="$(gpg --batch --with-colons --list-secret-keys --fingerprint)"
fingerprints="$(printf '%s\n' "$keys" | awk -F: '$1 == "sec" { primary=1; next } primary && $1 == "fpr" { print $10; primary=0 }')"
[[ "$fingerprints" == "$expected" ]] || die 'Use exactly the provisioned Polaris signing identity.'
algorithms="$(printf '%s\n' "$keys" | awk -F: '$1 == "sec" { print $4 }')"
[[ "$algorithms" == 1 || "$algorithms" == 3 ]] || die 'The existing RPM signer must be RSA.'

# Same signing operation and RSAHEADER postcondition as packages repository's
# scripts/build-package-repos.sh. Sign release copies before uploading them;
# repository publication retains its separate metadata/provenance signatures.
rpmkeys --define '_keyring rpmdb' --dbpath "$work/rpmdb" --import "$public_key"
for package in "${packages[@]}"; do
    rpmsign --define "_gpg_name $expected" --addsign "$package"
    signature="$(rpm --queryformat '%{RSAHEADER:pgpsig}' -qp "$package")"
    normalized="$(printf '%s' "$signature" | tr 'A-Z' 'a-z')"
    case "$normalized" in
        *'key id 35f13f14faad15cc'*) ;;
        *) die 'rpmsign did not produce the expected Polaris RSA signature.' ;;
    esac
    rpmkeys --define '_keyring rpmdb' --dbpath "$work/rpmdb" --checksig "$package"
    printf 'Signed and verified %s (%s)\n' "$(basename -- "$package")" "$signature"
done
