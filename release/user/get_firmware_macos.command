#!/bin/bash
# OMKit firmware tool for macOS: finds Python 3.9 or newer and runs omkit_firmware.py from this folder.
# Double-click it in Finder, or run it in Terminal: bash get_firmware_macos.command [--firmware FILE]
cd "$(dirname "$0")" || exit 1

ok_python() {
    "$1" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 9) else 1)' >/dev/null 2>&1
}

PY=""
for cand in /opt/homebrew/bin/python3 /usr/local/bin/python3 \
            /Library/Frameworks/Python.framework/Versions/Current/bin/python3 python3; do
    path=$(command -v "$cand" 2>/dev/null) || continue
    # /usr/bin/python3 without the Command Line Tools only opens an installer dialog
    if [ "$path" = /usr/bin/python3 ] && ! xcode-select -p >/dev/null 2>&1; then
        continue
    fi
    if ok_python "$path"; then
        PY=$path
        break
    fi
done

if [ -z "$PY" ]; then
    echo "Python 3.9 or newer was not found on this Mac."
    echo
    echo "Install Python, then run this file again:"
    echo "  - from https://www.python.org/downloads/macos/ (afterwards run 'Install Certificates.command'"
    echo "    in the Python folder of Applications once);"
    echo "  - or with Homebrew: brew install python"
    echo
    read -r -p "Press Enter to close this window. " _
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
if [ -t 0 ]; then
    read -r -p "Press Enter to close this window. " _
fi
exit $rc
