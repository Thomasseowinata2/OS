#!/usr/bin/env bash
# setup_wsl.sh - install the packages that this project needs on Ubuntu
# 20.04 or newer, under WSL 2 or on real hardware.
#
# The script installs nothing until you agree. It prints each command before
# it runs the command.
set -u

echo
echo "tsim build environment check"
echo

# --- check the system ----------------------------------------------------
if [ ! -r /etc/os-release ]; then
    echo "  cannot read /etc/os-release - this does not look like Linux."
    exit 1
fi
. /etc/os-release
echo "  distribution     ${PRETTY_NAME:-unknown}"
echo "  kernel           $(uname -r)"

case "${VERSION_ID:-0}" in
    2[0-9].*|3[0-9].*) : ;;
    *) echo "  warning: this project is tested on Ubuntu 20.04 and newer." ;;
esac

if grep -qi microsoft /proc/version 2>/dev/null; then
    echo "  environment      WSL (a real Linux kernel, so POSIX timers and"
    echo "                   signals operate as they do on hardware)"
else
    echo "  environment      native Linux"
fi
echo

# --- find the missing packages -------------------------------------------
NEED=""
have() { command -v "$1" >/dev/null 2>&1; }

for tool in gcc make; do
    if have "$tool"; then
        echo "  found            $("$tool" --version | head -1)"
    else
        echo "  MISSING          $tool"
        NEED="$NEED build-essential"
    fi
done

# strace is optional. The demonstration uses it to show the system calls.
if have strace; then
    echo "  found            strace (optional, used by the demo)"
else
    echo "  missing          strace (optional, used by the demo)"
    NEED="$NEED strace"
fi

# build-essential supplies the headers for timer_create(2). Check for them
# so that the message is clear after a partial installation.
if [ ! -r /usr/include/signal.h ]; then
    echo "  MISSING          the C library headers"
    NEED="$NEED libc6-dev"
fi

NEED=$(printf '%s\n' $NEED | sort -u | tr '\n' ' ')
echo

if [ -z "${NEED// /}" ]; then
    echo "  Everything needed is already installed."
else
    echo "  The following packages are needed:"
    echo "      $NEED"
    echo
    echo "  This will run:"
    echo "      sudo apt-get update && sudo apt-get install -y $NEED"
    echo
    printf '  Install them now? [y/N] '
    read -r answer
    case "$answer" in
        [yY]*)
            sudo apt-get update && sudo apt-get install -y $NEED || {
                echo "  installation failed"; exit 1; }
            ;;
        *)
            echo "  Nothing was installed.  Run the command above yourself when ready."
            exit 0
            ;;
    esac
fi

# --- build and test ------------------------------------------------------
cd "$(dirname "$0")/.." || exit 1
echo
echo "  Building..."
if make -s; then
    echo
    echo "  The build succeeded. A short run to show that the timer works:"
    echo
    ./tsim -Q -C -w workloads/io-heavy.txt | sed -n '/simulated time/p;/context switches/p'
    echo
    echo "  Ready.  Try:   make run      make compare      make check"
else
    echo "  Build failed."
    exit 1
fi
