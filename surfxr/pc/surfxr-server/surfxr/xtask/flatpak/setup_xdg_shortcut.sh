#!/bin/sh -e

# Flatpaks usually export their own shortcut - but SURFXR is an extension to steamvr, and we can't put our shortcut in steams folder
# so for now we need to install the shortcut manually

# Shortcut can be installed for user or systemwide - but system folder needs sudo

# Note that newly installed shortcuts will not appear available until the desktop environment reloads it's settings
# This can be done by restarting computer, or by logging off and on, or similar command like "plasmashell --replace"

# systemwide shortcut
# sudo cp com.valvesoftware.Steam.Utility.surfxr.desktop /var/lib/flatpak/exports/share/applications/

# users local folder
cp com.valvesoftware.Steam.Utility.surfxr.desktop $HOME/.local/share/flatpak/exports/share/applications/

# copy icon as well
xdg-icon-resource install --size 256 surfxr_icon.png application-surfxr-launcher
