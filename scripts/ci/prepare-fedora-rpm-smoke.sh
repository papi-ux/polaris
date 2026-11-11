#!/usr/bin/env bash
# Ephemeral build-container fixture only. Never sign or upload the CPack originals.
set -euo pipefail
export LC_ALL=C LANG=C
source_dir="${1:?Supply the CPack artifact directory}"
output_dir="${2:?Supply a new private smoke directory outside the artifact directory}"
[[ $# == 2 ]] || exit 2
umask 077
work="$(mktemp -d "${RUNNER_TEMP:-${TMPDIR:-/tmp}}/polaris-rpm-smoke.XXXXXXXX")"
export GNUPGHOME="$work/gnupg"
mkdir "$GNUPGHOME"
cleanup() {
    gpgconf --homedir "$GNUPGHOME" --kill gpg-agent || true
    rm -rf -- "$work" # exclusively created above; only this disposable fixture
}
trap cleanup EXIT

python3 - "$source_dir" "$output_dir" "$work/originals.json" <<'PY'
import hashlib
import json
from pathlib import Path
import sys
source, target, manifest = map(Path, sys.argv[1:])
source, target = source.resolve(), target.resolve()
assert source.is_dir() and not target.exists()
assert source != target and source not in target.parents and target not in source.parents
rows = {}
for name in ('Polaris.rpm', 'Polaris-kms.rpm'):
    path = source / name
    assert path.is_file() and not path.is_symlink()
    rows[name] = hashlib.sha256(path.read_bytes()).hexdigest()
target.mkdir(mode=0o700)
manifest.write_text(json.dumps(rows))
PY
cp -- "$source_dir/Polaris.rpm" "$output_dir/Polaris.rpm"
cp -- "$source_dir/Polaris-kms.rpm" "$output_dir/Polaris-kms.rpm"
gpg --batch --pinentry-mode loopback --passphrase '' --quick-generate-key \
    'Polaris disposable build smoke <ci@invalid.local>' rsa2048 sign 1d
fingerprint="$(gpg --batch --with-colons --list-secret-keys --fingerprint | awk -F: '$1 == "sec" { primary=1; next } primary && $1 == "fpr" { print $10; primary=0 }')"
[[ "$fingerprint" =~ ^[0-9A-F]{40}$ && "$fingerprint" != 58017EDFFA9F803E07ED26F835F13F14FAAD15CC ]] || exit 1
gpg --batch --armor --export "$fingerprint" > "$work/lab-public.asc"
rpmkeys --import "$work/lab-public.asc" # only this job container; never the runner host
key_id="$(printf '%s' "$fingerprint" | tail -c 16 | tr 'A-Z' 'a-z')"
for name in Polaris.rpm Polaris-kms.rpm; do
    copy="$output_dir/$name"
    rpmsign --define '_openpgp_sign gpg' --define "_gpg_name $fingerprint" \
        --define "_gpg_path $GNUPGHOME" --addsign "$copy"
    signature="$(rpm -qp --queryformat '%{RSAHEADER:pgpsig}' "$copy" | tr 'A-Z' 'a-z')"
    [[ "$signature" == *"key id $key_id"* ]] || exit 1
    rpmkeys --checksig --verbose "$copy"
done
python3 - "$source_dir" "$output_dir" "$work/originals.json" "$fingerprint" <<'PY'
import hashlib
import json
from pathlib import Path
import sys
source, target, manifest = map(Path, sys.argv[1:4])
before = json.loads(manifest.read_text())
after = {name: hashlib.sha256((source / name).read_bytes()).hexdigest() for name in before}
assert before == after, 'CPack originals changed during smoke preparation'
result = dict(originals=after, smokeCopies={name: hashlib.sha256((target / name).read_bytes()).hexdigest() for name in before},
              labFingerprint=sys.argv[4], originalUploadPairUntouched=True, vendorSigning=False)
with (target / 'SMOKE-RECEIPT.json').open('x') as output:
    json.dump(result, output, indent=2)
print(json.dumps(result))
PY
