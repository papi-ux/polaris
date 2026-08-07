#!/usr/bin/env bash
# Assemble dnf and pacman repositories from release assets that already exist.
#
# Polaris publishes four packages per tag, and until now the only way to take an
# upgrade was to download one by hand and install it by exact filename. This
# turns those same packages into repositories, so `dnf upgrade` and `pacman -Syu`
# carry Polaris along with everything else on the host.
#
# Nothing is rebuilt. The package a repository serves is the package the release
# published and CI tested; the only difference is a signature on the repository
# copy. Rebuilding would mean shipping a second binary nobody tested, and the
# Fedora spec pulls a CUDA toolkit over the network during %build, which no
# sandboxed rebuild service will do anyway.
#
# Each ecosystem needs its own toolchain, so --only runs one at a time and the
# results are combined into a single output tree:
#
#   --only fedora   needs createrepo_c, rpm, rpmsign   (Fedora container)
#   --only arch     needs repo-add                     (Arch container)
#
# Usage:
#   scripts/build-package-repos.sh --only fedora --assets DIR --version 1.3.7 \
#       --output DIR [--base-url URL] [--gpg-key-id KEYID]
#
# Without --gpg-key-id the repositories are assembled unsigned, which is useful
# for checking the layout locally. Publishing unsigned is not supported.

set -euo pipefail

BASE_URL_DEFAULT='https://papi-ux.com/polaris/repo'

only=''
assets_dir=''
version=''
output_dir=''
base_url="$BASE_URL_DEFAULT"
gpg_key_id=''

die() {
  printf 'build-package-repos: %s\n' "$1" >&2
  exit 1
}

require() {
  command -v "$1" >/dev/null 2>&1 || die "$1 is required but not installed"
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --only) only="${2:-}"; shift 2 ;;
    --assets) assets_dir="${2:-}"; shift 2 ;;
    --version) version="${2:-}"; shift 2 ;;
    --output) output_dir="${2:-}"; shift 2 ;;
    --base-url) base_url="${2:-}"; shift 2 ;;
    --gpg-key-id) gpg_key_id="${2:-}"; shift 2 ;;
    -h|--help) sed -n '2,27p' "$0"; exit 0 ;;
    *) die "unknown argument: $1" ;;
  esac
done

case "$only" in
  fedora|arch) ;;
  '') die 'missing --only (fedora or arch)' ;;
  *) die "--only must be fedora or arch, not: $only" ;;
esac
[ -n "$assets_dir" ] || die 'missing --assets'
[ -n "$version" ] || die 'missing --version'
[ -n "$output_dir" ] || die 'missing --output'
[ -d "$assets_dir" ] || die "assets directory does not exist: $assets_dir"

require gpg

mkdir -p "$output_dir"

# The exact release asset. Naming it rather than globbing means a renamed or
# missing asset fails here, instead of publishing a repository that quietly
# offers nothing.
case "$only" in
  fedora) asset="$assets_dir/Polaris-fedora44-x86_64.rpm" ;;
  arch) asset="$assets_dir/Polaris-arch-x86_64.pkg.tar.zst" ;;
esac
[ -f "$asset" ] || die "missing release asset: $asset"

printf 'Assembling %s repository for Polaris %s\n' "$only" "$version"

if [ "$only" = fedora ]; then
  require createrepo_c
  require rpm

  # Both package managers decide "is there an upgrade" from metadata inside the
  # package, never from its filename. A package whose internal version disagrees
  # with the release is one nobody ever upgrades to, and the failure is silent:
  # the repository works, it just never offers anything.
  # Packages carry the release version plus the commit -- 1.3.6.f851fad -- so
  # this is a prefix match on whole segments. Matching "$version"* instead would
  # accept 1.3.60 as a 1.3.6 package.
  rpm_version="$(rpm --queryformat '%{VERSION}' -qp "$asset" 2>/dev/null)"
  case "$rpm_version" in
    "$version"|"$version".*) ;;
    *) die "RPM reports version $rpm_version but the release is $version" ;;
  esac

  fedora_dir="$output_dir/fedora/x86_64"
  rm -rf "$output_dir/fedora"
  mkdir -p "$fedora_dir"
  cp "$asset" "$fedora_dir/"
  rpm_in_repo="$fedora_dir/$(basename "$asset")"

  if [ -n "$gpg_key_id" ]; then
    require rpmsign
    # Signs the repository copy only. The published release asset is untouched,
    # so a package installed by hand and one installed from the repository are
    # the same build.
    rpmsign --define "_gpg_name $gpg_key_id" --addsign "$rpm_in_repo"

    # The signature lands in the RSAHEADER header, not SIGPGP -- SIGPGP reads
    # back empty on a correctly signed package, so checking it would reject
    # every good build. The key has to be RSA: rpmsign exits 0 on an Ed25519
    # key and signs nothing at all, which is why this is verified rather than
    # trusted.
    signature="$(rpm --queryformat '%{RSAHEADER:pgpsig}' -qp "$rpm_in_repo" 2>/dev/null)"
    case "$signature" in
      *'Key ID'*) ;;
      *) die 'rpmsign exited 0 but the RPM carries no signature (is the key RSA?)' ;;
    esac
    printf '  signed %s (%s)\n' "$(basename "$rpm_in_repo")" "$signature"
  fi

  createrepo_c --quiet "$fedora_dir"
  [ -f "$fedora_dir/repodata/repomd.xml" ] || die 'createrepo_c produced no repomd.xml'

  if [ -n "$gpg_key_id" ]; then
    gpg --batch --yes --detach-sign --armor --local-user "$gpg_key_id" \
      "$fedora_dir/repodata/repomd.xml"
  fi

  cat > "$output_dir/fedora/polaris.repo" <<EOF
[polaris]
name=Polaris
baseurl=$base_url/fedora/\$basearch
enabled=1
gpgcheck=1
repo_gpgcheck=1
gpgkey=$base_url/polaris.gpg
metadata_expire=6h
EOF
fi

if [ "$only" = arch ]; then
  require repo-add

  arch_dir="$output_dir/arch/x86_64"
  rm -rf "$output_dir/arch"
  mkdir -p "$arch_dir"
  cp "$asset" "$arch_dir/"
  arch_in_repo="$arch_dir/$(basename "$asset")"

  if [ -n "$gpg_key_id" ]; then
    gpg --batch --yes --detach-sign --no-armor --local-user "$gpg_key_id" "$arch_in_repo"
    [ -f "$arch_in_repo.sig" ] || die 'gpg reported success but no detached signature exists'
  fi

  # repo-add records the filename it is handed and pacman fetches exactly that,
  # so a release asset does not have to be named like a pacman package.
  if [ -n "$gpg_key_id" ]; then
    ( cd "$arch_dir" && repo-add --quiet --sign --key "$gpg_key_id" \
        polaris.db.tar.gz "$(basename "$arch_in_repo")" )
    [ -f "$arch_dir/polaris.db.tar.gz.sig" ] || die 'repo-add did not sign the database'
  else
    ( cd "$arch_dir" && repo-add --quiet polaris.db.tar.gz "$(basename "$arch_in_repo")" )
  fi
  [ -f "$arch_dir/polaris.db" ] || die 'repo-add produced no polaris.db'

  # repo-add leaves polaris.db and polaris.files as symlinks to the .tar.gz.
  # pacman fetches polaris.db by name over HTTP, and a static host serves a
  # symlink as its target's name in a text file rather than following it, so
  # these are materialized as real files.
  for link in polaris.db polaris.files polaris.db.sig polaris.files.sig; do
    if [ -L "$arch_dir/$link" ]; then
      target="$(readlink "$arch_dir/$link")"
      rm "$arch_dir/$link"
      cp "$arch_dir/$target" "$arch_dir/$link"
    fi
  done

  # The version pacman will compare against, read back out of the database it
  # just wrote rather than trusted from the filename.
  db_version="$(tar -xzOf "$arch_dir/polaris.db.tar.gz" --wildcards '*/desc' |
    awk '/^%VERSION%$/ { getline; print; exit }')"
  # Same shape as the RPM, with pacman's -pkgrel suffix on the end.
  case "$db_version" in
    "$version"-*|"$version".*-*) ;;
    *) die "pacman database reports version $db_version but the release is $version" ;;
  esac
  printf '  database records %s\n' "$db_version"

  # Without an explicit SigLevel pacman falls back to the checksum in the
  # database and reports "Validated By: SHA-256 Sum" -- it never looks at the
  # signature, so an attacker who can rewrite the database rewrites the
  # checksum with it. Requiring both closes that.
  cat > "$output_dir/arch/polaris.conf" <<EOF
[polaris]
SigLevel = Required DatabaseRequired
Server = $base_url/arch/\$arch
EOF
fi

if [ -n "$gpg_key_id" ]; then
  gpg --batch --yes --armor --export "$gpg_key_id" > "$output_dir/polaris.gpg"
  [ -s "$output_dir/polaris.gpg" ] || die 'exported public key is empty'
fi

printf 'Done. %s repository contents:\n' "$only"
find "$output_dir/$only" -type f | sort | sed 's/^/  /'
