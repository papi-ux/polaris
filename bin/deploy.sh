#!/bin/bash
set -e

# Polaris deploy script — build, install binary + web assets, restart service
cd "$(dirname "$0")/.."

echo "Building..."
cd build
ninja -j$(nproc)
cd ..

echo "Stopping service..."
systemctl --user stop polaris 2>/dev/null || true
sleep 1

echo "Installing binary..."
sudo -A cp build/polaris-0.0.0.dirty /usr/bin/polaris-0.0.0

echo "Installing web assets..."
sudo -A mkdir -p /usr/share/polaris/web
sudo -A rm -rf /usr/share/polaris/web/assets
sudo -A cp -r build/assets/web/* /usr/share/polaris/web/

echo "Restarting service..."
systemctl --user restart polaris

CONFIG_FILE="${XDG_CONFIG_HOME:-$HOME/.config}/polaris/polaris.conf"
WEB_BASE_PORT=47989

if [ -f "$CONFIG_FILE" ]; then
  CONFIGURED_PORT=$(awk -F'=' '/^[[:space:]]*port[[:space:]]*=/ {gsub(/[[:space:]]/, "", $2); print $2; exit}' "$CONFIG_FILE")
  if [[ "$CONFIGURED_PORT" =~ ^[0-9]+$ ]]; then
    WEB_BASE_PORT="$CONFIGURED_PORT"
  fi
fi

WEB_UI_PORT=$((WEB_BASE_PORT + 1))

sleep 2
if systemctl --user is-active --quiet polaris; then
  echo ""
  echo "Polaris deployed and running."
  echo "  Binary: /usr/bin/polaris-0.0.0"
  echo "  Web UI: https://localhost:${WEB_UI_PORT}"
  echo "  Logs:   journalctl --user -u polaris -f"
else
  echo "ERROR: Polaris failed to start!"
  systemctl --user status polaris
  exit 1
fi
