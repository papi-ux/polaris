#!/bin/sh
# Opens pattern.html in its own Firefox profile: fullscreen for the stream, or windowed as the
# reference clock on the host monitor.
set -eu

mode=${1:-pattern}
case "$mode" in
  pattern) kiosk=--kiosk ;;
  reference) kiosk= ;;
  *) echo "usage: $0 [pattern|reference]" >&2; exit 2 ;;
esac

here=$(cd "$(dirname "$0")" && pwd)
# measure_run.py finds the page's processes by this profile name.
profile="${XDG_RUNTIME_DIR:?XDG_RUNTIME_DIR is not set}/vdpattern-$mode"
mkdir -p -m 700 "$profile"
# A new profile would otherwise open its welcome page in front of the pattern.
cat > "$profile/user.js" <<'EOF'
user_pref("browser.aboutwelcome.enabled", false);
user_pref("browser.shell.checkDefaultBrowser", false);
user_pref("browser.startup.homepage_override.mstone", "ignore");
user_pref("datareporting.policy.dataSubmissionPolicyBypassNotification", true);
user_pref("startup.homepage_welcome_url", "");
user_pref("startup.homepage_welcome_url.additional", "");
user_pref("trailhead.firstrun.didSeeAboutWelcome", true);
EOF
# shellcheck disable=SC2086 # kiosk is empty or one flag
exec firefox --no-remote --profile "$profile" $kiosk "file://$here/pattern.html#$mode"
