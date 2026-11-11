#!/usr/bin/env bash
# After vendor signing, before upload: clean each-release signature/transaction gate.
set -euo pipefail
export LC_ALL=C LANG=C
assets="${1:?Supply the finalized signed release directory}"
[[ $# == 1 && -d "$assets" ]] || exit 2
assets="$(cd -- "$assets" && pwd)"
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
public="$root/packaging/linux/keys"
for release in 44 45; do
    [[ -f "$assets/Polaris-fedora${release}-x86_64.rpm" && ! -L "$assets/Polaris-fedora${release}-x86_64.rpm" ]]
    [[ -f "$assets/Polaris-kms-fedora${release}-x86_64.rpm" && ! -L "$assets/Polaris-kms-fedora${release}-x86_64.rpm" ]]
done
for release in 44 45; do
    docker pull "fedora:$release"
    image="$(docker image inspect --format '{{index .RepoDigests 0}}' "fedora:$release")"
    [[ "$image" == *@sha256:* ]] || exit 1
    printf 'Final Fedora %s package gate uses %s\n' "$release" "$image"
    # No signer, secret, lab key, host socket or writable package mount enters it.
    docker run --rm \
        --mount "type=bind,src=$assets,dst=/packages,readonly" \
        --mount "type=bind,src=$public,dst=/vendor,readonly" \
        "$image" bash -euc '
            export LC_ALL=C LANG=C
            . /etc/os-release
            test "$ID" = fedora && test "$VERSION_ID" = "$1"
            rpmkeys --import /vendor/polaris.gpg
            rpmkeys --list 58017EDFFA9F803E07ED26F835F13F14FAAD15CC
            main="/packages/Polaris-fedora${1}-x86_64.rpm"
            kms="/packages/Polaris-kms-fedora${1}-x86_64.rpm"
            rpmkeys --checksig --verbose "$main" "$kms"
            dnf -y --setopt=localpkg_gpgcheck=true --setopt=tsflags=test install "$main" "$kms"
        ' _ "$release"
done
