#!/bin/sh

# User Service
mkdir -p ~/.config/systemd/user
cp "/app/share/polaris/systemd/user/polaris.service" "$HOME/.config/systemd/user/polaris.service"
echo "Polaris User Service has been installed."
echo "Use [systemctl --user enable polaris] once to autostart Polaris on login."

# Load uinput / uhid on the host
MODULES=$(cat /app/share/polaris/modules-load.d/60-polaris.conf)
echo "Enabling Polaris input modules on the host."
flatpak-spawn --host pkexec sh -c "echo '$MODULES' > /etc/modules-load.d/60-polaris.conf"
flatpak-spawn --host pkexec modprobe uinput
flatpak-spawn --host pkexec modprobe uhid

# Udev rule
UDEV=$(cat /app/share/polaris/udev/rules.d/60-polaris.rules)
echo "Configuring host input permissions."
flatpak-spawn --host pkexec sh -c "echo '$UDEV' > /etc/udev/rules.d/60-polaris.rules"
echo "Restart the computer or reload udev rules on the host to pick up the new permissions."
