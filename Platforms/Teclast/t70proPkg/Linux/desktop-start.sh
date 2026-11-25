#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause-Patent
# One opt-in early desktop session; PID1 owns udev and the rescue shell.
set -eu
umask 077
export PATH=/bin:/sbin:/usr/bin:/usr/sbin LC_ALL=C.UTF-8
RUN=/run/teclast-desktop
STARTUP=/run/teclast-startup/state
DRI=/dev/dri
MALI=/dev/mali0
UDEVADM=/usr/bin/udevadm
WESTON=/usr/bin/weston
FC_CACHE=/usr/bin/fc-cache
TIMEOUT=/bin/timeout
SLEEP=/bin/sleep
SETSID=/bin/setsid
[ "$#" = 1 ] || exit 64
case "$1" in vulkan|pixman) renderer=$1;; *) exit 64;; esac
mkdir -m 700 "$RUN" || exit 73
state() { printf '%s\n' "$1" > "$RUN/state.tmp"; mv "$RUN/state.tmp" "$RUN/state"; }
node() { [ -c "$1" ]; }
child=
finish() {
    trap - HUP INT TERM
    if [ -n "$child" ]; then
        kill -TERM "-$child" 2>/dev/null || :
        count=0
        while kill -0 "-$child" 2>/dev/null && [ "$count" -lt 30 ]; do
            "$SLEEP" 0.1
            count=$((count + 1))
        done
        if kill -0 "-$child" 2>/dev/null; then kill -KILL "-$child" 2>/dev/null || :; fi
        wait "$child" 2>/dev/null || :
    fi
}
trap 'state interrupted; finish; exit 143' HUP INT TERM
state waiting-for-udev
count=0
until "$TIMEOUT" -s TERM -k 1 2 "$UDEVADM" control --ping --timeout=1; do
    count=$((count + 1))
    if [ "$count" -ge 10 ]; then state udev-unavailable; exit 1; fi
    "$SLEEP" 0.1
done
state coldplug
if ! "$TIMEOUT" -s TERM -k 2 30 "$UDEVADM" trigger --type=subsystems --action=add ||
   ! "$TIMEOUT" -s TERM -k 2 30 "$UDEVADM" trigger --type=devices --action=add ||
   ! "$TIMEOUT" -s TERM -k 2 30 "$UDEVADM" settle --timeout=20; then
    state coldplug-failed; exit 1
fi
state waiting-for-drivers
count=0
while :; do
    ready=0
    for card in "$DRI"/card*; do if node "$card"; then ready=1; break; fi; done
    if [ "$ready" = 1 ] && { [ "$renderer" = pixman ] || node "$MALI"; }; then break; fi
    if [ -f "$STARTUP" ]; then
        read -r result < "$STARTUP" || result=
        case "$result" in
            failed|disabled|interrupted|device-node-scan-failed|bindings-observed|bindings-incomplete)
                state required-device-missing; exit 1;;
        esac
    fi
    count=$((count + 1))
    if [ "$count" -ge 150 ]; then state driver-timeout; exit 1; fi
    "$SLEEP" 1
done
mkdir -m 700 "$RUN/wayland" "$RUN/home" "$RUN/cache" "$RUN/font-cache"
export XDG_RUNTIME_DIR="$RUN/wayland" XDG_CACHE_HOME="$RUN/cache" HOME="$RUN/home"
export XDG_CONFIG_HOME=/etc/xdg XDG_DATA_DIRS=/usr/share SHELL=/bin/sh
export LIBSEAT_BACKEND=builtin SEATD_VTBOUND=0 XCURSOR_THEME=DMZ-White XCURSOR_SIZE=32
export FONTCONFIG_FILE=/etc/fonts/fonts.conf
export VK_DRIVER_FILES=/usr/share/vulkan/icd.d/teclast-panvk.json
if ! "$TIMEOUT" -s TERM -k 2 30 "$FC_CACHE"; then state font-cache-failed; exit 1; fi
state starting
"$SETSID" "$WESTON" --backend=drm --renderer="$renderer" --config=/etc/xdg/weston/weston.ini --socket=wayland-0 --idle-time=0 &
child=$!
count=0
while [ ! -S "$XDG_RUNTIME_DIR/wayland-0" ]; do
    if ! kill -0 "$child" 2>/dev/null; then
        if wait "$child"; then code=0; else code=$?; fi
        child=
        state "exited-before-socket:$code"; exit 1
    fi
    count=$((count + 1))
    if [ "$count" -ge 300 ]; then state compositor-timeout; finish; exit 1; fi
    "$SLEEP" 0.1
done
state socket-created
if wait "$child"; then code=0; else code=$?; fi
child=
state "exited:$code"
exit "$code"
