#!/data/data/com.termux/files/usr/bin/bash
# One-time installer for the Azahar GT6 Termux helper.
#   curl -fsSL https://raw.githubusercontent.com/abhishek-rathod01/azahar/gt6/tools/gt6/termux/install.sh | bash
set -eu

BRANCH="${GT6_BRANCH:-gt6}"
BASE="https://raw.githubusercontent.com/abhishek-rathod01/azahar/$BRANCH/tools/gt6/termux"

pkg update -y
pkg install -y android-tools curl tar
mkdir -p "$HOME/gt6/out"
curl -fsSL "$BASE/gt6.sh" -o "$HOME/gt6/gt6.sh"
chmod +x "$HOME/gt6/gt6.sh"
ln -sf "$HOME/gt6/gt6.sh" "$PREFIX/bin/gt6"
# Lets `gt6 pack` copy the bundle into Download/ so it can be uploaded from the Files app
[ -d "$HOME/storage/downloads" ] || termux-setup-storage || true

echo
echo "Installed. Now run:  gt6 help"
