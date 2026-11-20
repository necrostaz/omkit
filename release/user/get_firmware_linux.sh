#!/bin/sh
# OMKit firmware tool for Linux: finds Python 3.9 or newer and runs omkit_firmware.py from this folder.
# Run it in a terminal: sh get_firmware_linux.sh [--firmware FILE]
cd "$(dirname "$0")" || exit 1

PY=""
for cand in python3 python; do
    if command -v "$cand" >/dev/null 2>&1 &&
        "$cand" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 9) else 1)' >/dev/null 2>&1; then
        PY=$cand
        break
    fi
done

if [ -z "$PY" ]; then
    echo "Python 3.9 or newer was not found."
    echo
    echo "Install it with your distribution's package manager, then run this file again, e.g.:"
    echo "  Debian, Ubuntu:  sudo apt install python3"
    echo "  Fedora:          sudo dnf install python3"
    echo "  Arch:            sudo pacman -S python"
    echo "  openSUSE:        sudo zypper install python3"
    exit 1
fi

"$PY" omkit_firmware.py "$@"
rc=$?
echo
if [ $rc -eq 0 ]; then
    echo "Finished. Where the files are: see the lines above."
else
    echo "The tool stopped, see the messages above."
fi
if [ -t 0 ] && [ -t 1 ]; then
    printf "Press Enter to close. "
    read -r _
fi
exit $rc
