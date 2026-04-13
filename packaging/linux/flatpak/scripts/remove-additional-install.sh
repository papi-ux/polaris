#!/bin/sh

# User Service
systemctl --user stop polaris
rm "$HOME/.config/systemd/user/polaris.service"
systemctl --user daemon-reload
echo "Polaris User Service has been removed."

# Remove rules
flatpak-spawn --host pkexec sh -c "rm /etc/modules-load.d/60-polaris.conf"
flatpak-spawn --host pkexec sh -c "rm /etc/udev/rules.d/60-polaris.rules"
echo "Input rules removed. Restart computer to take effect."
