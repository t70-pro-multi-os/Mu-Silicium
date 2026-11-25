#!/bin/sh
# Read-only connectivity preflight plus an explicit foreground nl80211 service.
# Probe reads sysfs only. Association and DHCP require separate explicit calls.
# BT HCI attach is explicit; WMT catalog/control initialization remains a prerequisite.
set -e
LC_ALL=C
export LC_ALL

TEST_ROOT="$TECLAST_CONNECTIVITY_TEST_ROOT"
if [ -n "$TEST_ROOT" ]; then
  case "$TEST_ROOT" in /*) ;; *) echo "error=test root must be absolute" >&2; exit 64;; esac
  [ -d "$TEST_ROOT" ] || { echo "error=test root does not exist" >&2; exit 64; }
  SYS_ROOT="$TEST_ROOT/sys"
  DEV_ROOT="$TEST_ROOT/dev"
  RUN_ROOT="$TEST_ROOT/run"
  ETC_ROOT="$TEST_ROOT/etc"
  PATH="$TEST_ROOT/bin:$PATH"
  export PATH
  TEST_MODE=yes
else
  SYS_ROOT=/sys
  DEV_ROOT=/dev
  RUN_ROOT=/run
  ETC_ROOT=/etc
  TEST_MODE=no
fi

usage() {
  echo "usage: teclast-connectivity probe | wifi-run IFACE CONFIG | wifi-status IFACE | wifi-dhcp-run IFACE | wmt-run | wifi-power-on | wifi-power-off | bt-run | dbus-run | bt-service-run" >&2
  exit 64
}
fail() {
  printf 'error=%s\n' "$1" >&2
  exit "$2"
}
module_state() {
  if [ -d "$SYS_ROOT/module/$1" ]; then printf loaded; else printf missing; fi
}
has_wiphy() {
  for path in "$SYS_ROOT"/class/ieee80211/phy*; do
    [ -d "$path" ] && return 0
  done
  return 1
}
has_wifi_iface() {
  for path in "$SYS_ROOT"/class/net/*; do
    wifi_iface_present "${path##*/}" && return 0
  done
  return 1
}
valid_ifname() {
  [ "${#1}" -le 15 ] || return 1
  case "$1" in
    ""|.|..|*[!abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-]*) return 1;;
    *) return 0;;
  esac
}
wifi_iface_present() {
  valid_ifname "$1" || return 1
  netdev="$SYS_ROOT/class/net/$1"
  [ -d "$netdev/wireless" ] && return 0
  # cfg80211 exposes this symlink even without the legacy wireless group.
  # Only accept a link resolving to an actually registered wiphy.
  link="$netdev/phy80211"
  [ -L "$link" ] && [ -d "$link" ] || return 1
  for phy in "$SYS_ROOT"/class/ieee80211/phy*; do
    [ -d "$phy" ] && [ "$link" -ef "$phy" ] && return 0
  done
  return 1
}
has_bt_hci() {
  for path in "$SYS_ROOT"/class/bluetooth/hci*; do
    [ -d "$path" ] && return 0
  done
  return 1
}
has_gnss_class_device() {
  for path in "$SYS_ROOT"/class/gnss/gnss*; do
    [ -d "$path" ] && return 0
  done
  return 1
}
has_gnss_char_device() {
  for path in "$DEV_ROOT"/gnss*; do
    [ -c "$path" ] && return 0
  done
  return 1
}
available() {
  command -v "$1" >/dev/null 2>&1
}
wifi_control_dir() {
  printf '%s/wpa_supplicant' "$RUN_ROOT"
}
emit_probe() {
  if has_wiphy; then wiphy=present; else wiphy=missing; fi
  if has_wifi_iface; then netdev=present; else netdev=missing; fi
  if available wpa_supplicant; then wpa=available; else wpa=missing; fi
  wifi_control=not-observed
  for path in "$(wifi_control_dir)"/*; do
    if [ -S "$path" ]; then wifi_control=present; break; fi
  done
  if [ "$wiphy" = present ] && [ "$netdev" = present ] && [ "$wpa" = available ] &&
     [ "$(module_state wlan_drv_gen4m_6789)" = loaded ]; then
    wifi_handoff=eligible-not-tested
  else
    wifi_handoff=blocked
  fi

  if has_bt_hci; then hci=present; else hci=missing; fi
  if [ -S "$RUN_ROOT/dbus/system_bus_socket" ]; then dbus=present; else dbus=missing; fi
  if available bluetoothd; then bluez=available; else bluez=missing; fi
  if [ "$hci" = present ] && [ "$dbus" = present ] && [ "$bluez" = available ]; then
    bt_handoff=eligible-not-tested
  else
    bt_handoff=blocked
  fi

  if has_gnss_class_device; then gnss_class=registered; else gnss_class=not-observed; fi
  if has_gnss_char_device; then gnss_node=present; else gnss_node=missing; fi
  if available gpsd; then gpsd_state=available; else gpsd_state=missing; fi

  printf 'wmt.module=%s\n' "$(module_state wmt_drv)"
  printf 'wmt.adapter_module=%s\n' "$(module_state connadp)"
  if available teclast-wmt-control; then
    printf 'wmt.control=available\n'
    control=$(service_binary teclast-wmt-control) || control=
    if [ -n "$control" ] && "$control" --ready >/dev/null 2>&1; then
      printf 'wmt.ready=yes\n'
    else
      printf 'wmt.ready=no\n'
    fi
  else
    printf 'wmt.control=missing\nwmt.ready=no\n'
  fi
  printf 'wmt.initialization=explicit-wmt-run-before-radio-power\n'
  printf 'wmt.firmware_request=kernel-request_firmware-with-launcher-catalog\n'
  printf 'wifi.module=%s\n' "$(module_state wlan_drv_gen4m_6789)"
  printf 'wifi.wmt_transport=%s\n' "$(module_state wmt_chrdev_wifi)"
  printf 'wifi.firmware_request=WIFI_RAM_CODE_soc2_2_<flavor>_<eco>.bin\n'
  printf 'wifi.power_control=wmtWifi-write-1-after-WMT-initialization\n'
  printf 'wifi.wiphy=%s\n' "$wiphy"
  printf 'wifi.netdev=%s\n' "$netdev"
  printf 'wifi.wpa_supplicant=%s\n' "$wpa"
  printf 'wifi.service_handoff=%s\n' "$wifi_handoff"
  printf 'wifi.control_socket=%s\n' "$wifi_control"
  printf 'wifi.connection=not-queried\n'
  ip_state=not-managed
  for path in "$RUN_ROOT"/teclast-connectivity/lease-*/state; do
    if [ -f "$path" ]; then ip_state=service-state-present; break; fi
  done
  printf 'wifi.ip_configuration=%s\n' "$ip_state"
  printf 'bt.vendor_module=%s\n' "$(module_state bt_drv_connac1x)"
  printf 'bt.transport_module=%s\n' "$(module_state btif_drv)"
  printf 'bt.core_module=%s\n' "$(module_state bluetooth)"
  printf 'bt.hci_uart_module=%s\n' "$(module_state hci_uart)"
  if available teclast-bt-hci; then printf 'bt.h4_bridge=available\n'; else printf 'bt.h4_bridge=missing\n'; fi
  printf 'bt.hci=%s\n' "$hci"
  printf 'bt.bluetoothd=%s\n' "$bluez"
  printf 'bt.dbus_socket=%s\n' "$dbus"
  printf 'bt.service_handoff=%s\n' "$bt_handoff"
  printf 'bt.power=not-set\n'
  printf 'bt.connection=not-queried\n'
  printf 'gnss.vendor_module=%s\n' "$(module_state gps_drv_stp)"
  printf 'gnss.core_module=%s\n' "$(module_state gnss)"
  printf 'gnss.power_module=%s\n' "$(module_state gps_pwr)"
  printf 'gnss.class_device=%s\n' "$gnss_class"
  printf 'gnss.char_device=%s\n' "$gnss_node"
  printf 'gnss.gpsd=%s\n' "$gpsd_state"
  printf 'gnss.protocol=unverified\n'
  printf 'gnss.data=not-queried\n'
}
run_wifi_power() {
  [ "$#" -eq 1 ] || usage
  for module in wmt_drv wmt_chrdev_wifi wlan_drv_gen4m_6789; do
    [ "$(module_state "$module")" = loaded ] || fail "required Wi-Fi module is not loaded: $module" 69
  done
  [ -c "$DEV_ROOT/wmtWifi" ] || fail "stock Wi-Fi power control is absent" 69
  control=$(service_binary teclast-wmt-control) || fail "WMT control program is unavailable" 69
  if [ "$1" = on ]; then
    "$control" --ready || fail "WMT ROM-patch service is not ready; run wmt-run first" 69
  fi
  "$control" "--wifi-$1" || { result=$?; exit "$result"; }
  if [ "$1" = off ]; then printf 'wifi.power_request=off-completed\n'; return; fi
  "$control" --ready || fail "WMT ROM-patch service stopped during Wi-Fi power-on" 69
  attempts=0
  while [ "$attempts" -lt 50 ]; do
    if has_wiphy && has_wifi_iface; then
      "$control" --ready || fail "WMT ROM-patch service stopped before Wi-Fi registration was confirmed" 69
      printf 'wifi.driver_interfaces=registered\n'; return
    fi
    sleep 0.2
    attempts=$((attempts + 1))
  done
  fail "power request completed but Wi-Fi interfaces did not register; inspect WMT control service and kernel log" 69
}
run_dbus() {
  [ "$#" -eq 0 ] || usage
  [ ! -e "$RUN_ROOT/dbus/system_bus_socket" ] || fail "system D-Bus socket already exists" 73
  [ -f "$ETC_ROOT/teclast/dbus-system.conf" ] || fail "packaged D-Bus configuration is absent" 69
  daemon=$(service_binary dbus-daemon) || fail "D-Bus daemon is unavailable" 69
  uuid=$(service_binary dbus-uuidgen) || fail "D-Bus ID generator is unavailable" 69
  [ ! -L "$RUN_ROOT/dbus" ] || fail "D-Bus directory is a symlink" 73
  (umask 077; mkdir -p "$RUN_ROOT/dbus") || fail "cannot create D-Bus directory" 73
  "$uuid" "--ensure=$RUN_ROOT/dbus/machine-id" || { result=$?; exit "$result"; }
  exec "$daemon" --nofork --nopidfile --nosyslog "--config-file=$ETC_ROOT/teclast/dbus-system.conf"
}
run_bluez() {
  [ "$#" -eq 0 ] || usage
  has_bt_hci || fail "Bluetooth HCI controller is absent; start WMT and the H4 bridge first" 69
  [ -S "$RUN_ROOT/dbus/system_bus_socket" ] || fail "system D-Bus is not running" 69
  daemon=$(service_binary bluetoothd) || fail "BlueZ daemon is unavailable" 69
  printf 'bt.service=delegating controller_power=explicit-bluetoothctl\n'
  exec "$daemon" --nodetach
}
run_wmt() {
  [ "$#" -eq 0 ] || usage
  [ "$(module_state wmt_drv)" = loaded ] || fail "WMT module is not loaded" 69
  # The native service creates stpwmt through the original common-init ioctl.
  [ -c "$DEV_ROOT/wmtdetect" ] || fail "stock WMT detector node is absent" 69
  control=$(service_binary teclast-wmt-control) || fail "WMT control program is unavailable" 69
  printf 'wmt.service=delegating catalog=runtime-chip-and-revision power=driver-managed\n'
  exec "$control" --run
}
run_bt() {
  [ "$#" -eq 0 ] || usage
  for module in wmt_drv btif_drv bt_drv_connac1x bluetooth hci_uart; do
    [ "$(module_state "$module")" = loaded ] || fail "required BT module is not loaded: $module" 69
  done
  [ -c "$DEV_ROOT/stpbt" ] || fail "stock BT transport is absent" 69
  control=$(service_binary teclast-wmt-control) || fail "WMT control program is unavailable" 69
  "$control" --ready || fail "WMT ROM-patch service is not ready; run wmt-run first" 69
  bridge=$(service_binary teclast-bt-hci) || fail "H4 bridge is unavailable" 69
  printf 'bt.service=starting transport=stpbt protocol=H4 power_owner=transport-open-close\n'
  exec "$bridge" --run
}
run_wifi() {
  [ "$#" -eq 2 ] || usage
  iface=$1
  config=$2
  valid_ifname "$iface" || fail "invalid interface name" 64
  [ "$(module_state wlan_drv_gen4m_6789)" = loaded ] ||
    fail "stock Wi-Fi module is not loaded" 69
  has_wiphy || fail "cfg80211 wiphy is not registered" 69
  wifi_iface_present "$iface" || fail "interface is not exposed as a wireless netdev" 69
  case "$config" in /*) ;; *) fail "configuration path must be absolute" 64;; esac
  [ -f "$config" ] && [ -r "$config" ] && [ ! -L "$config" ] ||
    fail "configuration is not a readable regular file" 66
  WPA_BIN=$(command -v wpa_supplicant) ||
    fail "wpa_supplicant is unavailable" 69
  if [ "$TEST_MODE" = yes ]; then
    case "$WPA_BIN" in "$TEST_ROOT"/bin/*) ;; *) fail "test mode requires its fake service binary" 69;; esac
  fi
  ctrl_dir=$(wifi_control_dir)
  [ ! -L "$ctrl_dir" ] || fail "control directory is a symlink" 73
  if [ ! -d "$ctrl_dir" ]; then
    (umask 077; mkdir -p "$ctrl_dir") || fail "cannot create control directory" 73
  fi
  [ ! -S "$ctrl_dir/$iface" ] || fail "a control socket already exists for this interface" 73
  printf 'wifi.service=delegating backend=nl80211 address_configuration=explicit-wifi-dhcp-run\n'
  # Foreground mode preserves wpa_supplicant diagnostics and exit status for
  # the caller's service manager. No link toggle, DHCP, or config rewrite here.
  exec "$WPA_BIN" -D nl80211 -i "$iface" -c "$config" -O "$ctrl_dir"
}
query_wifi() {
  [ "$#" -eq 1 ] || usage
  iface=$1
  valid_ifname "$iface" || fail "invalid interface name" 64
  ctrl_dir=$(wifi_control_dir)
  [ -S "$ctrl_dir/$iface" ] || {
    printf 'wifi.service=not-running\nwifi.connection=unknown\n' >&2
    exit 69
  }
  WPA_CLI=$(command -v wpa_cli) || fail "wpa_cli is unavailable" 69
  if [ "$TEST_MODE" = yes ]; then
    case "$WPA_CLI" in "$TEST_ROOT"/bin/*) ;; *) fail "test mode requires its fake control client" 69;; esac
  fi
  if STATUS_OUTPUT=$("$WPA_CLI" -p "$ctrl_dir" -i "$iface" status); then
    :
  else
    status_rc=$?
    printf 'error=wpa_cli status query failed exit=%s\n' "$status_rc" >&2
    exit "$status_rc"
  fi
  state=$(printf '%s\n' "$STATUS_OUTPUT" | while IFS='=' read -r key value; do
    if [ "$key" = wpa_state ]; then printf '%s\n' "$value"; break; fi
  done)
  case "$state" in
    DISCONNECTED|INTERFACE_DISABLED|INACTIVE|SCANNING|AUTHENTICATING|ASSOCIATING|ASSOCIATED|4WAY_HANDSHAKE|GROUP_HANDSHAKE|COMPLETED) ;;
    *) printf 'wifi.connection=unknown\nwifi.ip_configuration=unknown\n'; exit 3;;
  esac
  printf 'wifi.connection=%s\n' "$state"
  ip_state=not-managed
  if [ -f "$RUN_ROOT/teclast-connectivity/lease-$iface/state" ]; then
    IFS= read -r ip_state < "$RUN_ROOT/teclast-connectivity/lease-$iface/state" || :
    case "$ip_state" in dhcp-running|lease-published|waiting|lease-rejected|cleanup-failed) ;; *) ip_state=unknown;; esac
  fi
  printf 'wifi.ip_configuration=%s\n' "$ip_state"
}

# udhcpc supplies untrusted DHCP options to this same executable as event argv.
# No option is evaluated as shell code or handed to ip before full validation.
service_binary() {
  local binary
  binary=$(command -v "$1") || return 1
  case "$binary" in /*) ;; *) return 1;; esac
  if [ "$TEST_MODE" = yes ]; then
    case "$binary" in "$TEST_ROOT"/bin/*) ;; *) return 1;; esac
  fi
  printf '%s' "$binary"
}
ipv4_number() {
  local value octet number=0 oldifs
  value=$1
  [ "${#value}" -le 15 ] || return 1
  case "$value" in ''|*[!0123456789.]*|.*|*.|*..*) return 1;; esac
  oldifs=$IFS; IFS=.; set -- $value; IFS=$oldifs
  [ "$#" -eq 4 ] || return 1
  for octet in "$@"; do
    case "$octet" in ''|0[0123456789]*) return 1;; esac
    [ "${#octet}" -le 3 ] && [ "$octet" -le 255 ] || return 1
    number=$((number * 256 + octet))
  done
  printf '%s' "$number"
}
unicast_ipv4() {
  local number
  number=$(ipv4_number "$1") || return 1
  [ "$number" -ge 16777216 ] && [ "$number" -lt 3758096384 ] &&
    [ "$((number / 16777216))" -ne 127 ]
}
mask_prefix() {
  local mask inverse prefix=32
  mask=$(ipv4_number "$1") || return 1
  inverse=$((4294967295 - mask))
  [ "$((inverse & (inverse + 1)))" -eq 0 ] || return 1
  while [ "$inverse" -gt 0 ]; do inverse=$((inverse / 2)); prefix=$((prefix - 1)); done
  printf '%s' "$prefix"
}
valid_destination() {
  local destination prefix number hostmask
  destination=$1; prefix=${destination#*/}
  [ "$prefix" != "$destination" ] || return 1
  case "$prefix" in ''|*[!0123456789]*|0[0123456789]*) return 1;; esac
  [ "${#prefix}" -le 2 ] && [ "$prefix" -le 32 ] || return 1
  number=$(ipv4_number "${destination%/*}") || return 1
  hostmask=$((4294967295 >> prefix))
  [ "$((number & hostmask))" -eq 0 ]
}
dhcp_paths() {
  DHCP_BASE="$RUN_ROOT/teclast-connectivity"
  LEASE_DIR="$DHCP_BASE/lease-$iface"
}
publish_dns() {
  local attempt=0 dns_file dns_tmp action=${1:-publish}
  # Serialize publication across interfaces. Each private lease owns only its
  # own fragment; readers see an atomic, nameserver-only aggregate.
  while ! mkdir "$DHCP_BASE/dns.lock" 2>/dev/null; do
    attempt=$((attempt + 1)); [ "$attempt" -lt 20 ] || return 1
    sleep 0.05
  done
  dns_tmp="$DHCP_BASE/resolv.conf.$$"
  if (
    # Fragment replacement/removal shares the publication lock. Otherwise a
    # concurrent interface exit could unlink a fragment between glob and cat.
    case "$action" in
      install) mv "$LEASE_DIR/dns.next" "$LEASE_DIR/resolv.conf" || exit 1;;
      remove) rm -f "$LEASE_DIR/resolv.conf" || exit 1;;
      publish) :;;
      *) exit 1;;
    esac
    umask 022
    : > "$dns_tmp" || exit 1
    for dns_file in "$DHCP_BASE"/lease-*/resolv.conf; do
      [ -f "$dns_file" ] && [ ! -L "$dns_file" ] || continue
      cat "$dns_file" >> "$dns_tmp" || exit 1
    done
    chmod 644 "$dns_tmp" && mv "$dns_tmp" "$DHCP_BASE/resolv.conf"
  ); then
    rmdir "$DHCP_BASE/dns.lock"
  else
    rm -f "$dns_tmp"; rmdir "$DHCP_BASE/dns.lock"; return 1
  fi
}
route_command() {
  local operation=$1 destination=$2 gateway=$3 metric=$4
  if [ "$gateway" = 0.0.0.0 ]; then
    "$IP_BIN" -f inet route "$operation" "$destination" dev "$iface" proto 16 metric "$metric"
  else
    "$IP_BIN" -f inet route "$operation" "$destination" via "$gateway" dev "$iface" proto 16 metric "$metric"
  fi
}
clear_lease() {
  local route_file destination gateway metric extra address failed=0
  # Delete only successful adds recorded by this instance. In particular, no
  # addr/route flush, replace or deletion of another service's configuration.
  for route_file in "$LEASE_DIR"/route.*; do
    [ -f "$route_file" ] || continue
    IFS=' ' read -r destination gateway metric extra < "$route_file" || return 1
    if route_command del "$destination" "$gateway" "$metric" 2> "$LEASE_DIR/ip-error"; then
      rm -f "$route_file" || return 1
    elif grep -q 'No such process' "$LEASE_DIR/ip-error"; then
      rm -f "$route_file" || return 1
    else
      failed=1
    fi
  done
  # Retain the address while deletion of an owned route has failed.
  if [ "$failed" -eq 0 ] && [ -f "$LEASE_DIR/address" ]; then
    IFS= read -r address < "$LEASE_DIR/address" || return 1
    if "$IP_BIN" -f inet addr del "$address" dev "$iface" 2> "$LEASE_DIR/ip-error"; then
      rm -f "$LEASE_DIR/address" || return 1
    elif grep -q 'Cannot assign requested address' "$LEASE_DIR/ip-error"; then
      rm -f "$LEASE_DIR/address" || return 1
    else
      failed=1
    fi
  fi
  [ "$failed" -eq 0 ] || return 1
  rm -f "$LEASE_DIR/lease" || return 1
  publish_dns remove || return 1
}
plan_lease() {
  local prefix number hostmask value count destination gateway metric=600
  unicast_ipv4 "${ip:-}" || return 1
  prefix=$(mask_prefix "${subnet:-}") || return 1
  number=$(ipv4_number "$ip"); hostmask=$((4294967295 >> prefix))
  if [ "$prefix" -lt 31 ]; then
    [ "$((number & hostmask))" -ne 0 ] && [ "$((number & hostmask))" -ne "$hostmask" ] || return 1
  fi
  # Limit both parsing and file output. These character sets also exclude
  # wildcard expansion, newlines and option/command injection when splitting.
  [ "${#router}" -le 512 ] && [ "${#dns}" -le 512 ] && [ "${#staticroutes}" -le 2048 ] || return 1
  case "${router:-} ${dns:-}" in *[!0123456789.\ ]*) return 1;; esac
  case "${staticroutes:-}" in *[!0123456789./\ ]*) return 1;; esac
  count=0
  for value in ${router:-}; do
    count=$((count + 1)); [ "$count" -le 8 ] && unicast_ipv4 "$value" && [ "$value" != "$ip" ] || return 1
  done
  count=0
  : > "$LEASE_DIR/dns.next" || return 1
  for value in ${dns:-}; do
    count=$((count + 1)); [ "$count" -le 8 ] && unicast_ipv4 "$value" || return 1
    printf 'nameserver %s\n' "$value" >> "$LEASE_DIR/dns.next" || return 1
  done
  printf '%s/%s\n' "$ip" "$prefix" > "$LEASE_DIR/plan" || return 1
  # RFC 3442 option 121 takes precedence over routers, including an explicitly
  # empty classless option. BusyBox supplies IP/PREFIX GATEWAY pairs.
  if [ "${staticroutes+x}" = x ]; then
    set -- $staticroutes
    [ "$#" -le 32 ] && [ "$(($# % 2))" -eq 0 ] || return 1
    while [ "$#" -gt 0 ]; do
      destination=$1; gateway=$2; shift 2
      valid_destination "$destination" || return 1
      if [ "$gateway" != 0.0.0.0 ]; then
        unicast_ipv4 "$gateway" && [ "$gateway" != "$ip" ] || return 1
      fi
      printf '%s %s %s\n' "$destination" "$gateway" "$metric" >> "$LEASE_DIR/plan" || return 1
      metric=$((metric + 1))
    done
  elif [ -n "${router:-}" ]; then
    set -- $router
    printf '0.0.0.0/0 %s 600\n' "$1" >> "$LEASE_DIR/plan" || return 1
  fi
}
apply_lease() {
  local address destination gateway metric extra index=0
  if [ -f "$LEASE_DIR/lease" ] && cmp -s "$LEASE_DIR/plan" "$LEASE_DIR/lease"; then
    : # The common renewal path leaves the address and routes in place.
  else
    clear_lease || return 1
    {
      IFS= read -r address || return 1
      "$IP_BIN" -f inet addr add "$address" dev "$iface" || return 1
      printf '%s\n' "$address" > "$LEASE_DIR/address" || return 1
      while IFS=' ' read -r destination gateway metric extra; do
        route_command add "$destination" "$gateway" "$metric" || return 1
        printf '%s %s %s\n' "$destination" "$gateway" "$metric" > "$LEASE_DIR/route.$index" || return 1
        index=$((index + 1))
      done
    } < "$LEASE_DIR/plan"
    mv "$LEASE_DIR/plan" "$LEASE_DIR/lease" || return 1
  fi
  publish_dns install || return 1
  printf 'lease-published\n' > "$LEASE_DIR/state"
}
proc_parent() {
  local pid=$1 field value
  case "$pid" in ''|*[!0123456789]*) return 1;; esac
  while read -r field value; do
    if [ "$field" = PPid: ]; then
      case "$value" in ''|*[!0123456789]*) return 1;; esac
      printf '%s' "$value"
      return 0
    fi
  done < "/proc/$pid/status"
  return 1
}
stop_rejected_renewal() {
  local client supervisor
  # Notify only our live grandparent (wrapper -> udhcpc -> hook). The
  # private state file or inherited environment alone is not signal authority.
  client=$(proc_parent "$$") || return 1
  supervisor=$(proc_parent "$client") || return 1
  [ "$supervisor" = "$owner" ] || return 1
  kill -USR1 "$supervisor"
}
dhcp_event() {
  [ "$#" -eq 1 ] || usage
  iface=${TECLAST_DHCP_IFACE:-}
  valid_ifname "$iface" && [ "${interface:-}" = "$iface" ] || fail "unowned DHCP event" 64
  dhcp_paths
  [ "${TECLAST_DHCP_DIR:-}" = "$LEASE_DIR" ] && [ -d "$LEASE_DIR" ] && [ ! -L "$LEASE_DIR" ] || fail "unowned DHCP state" 73
  case "${TECLAST_DHCP_OWNER:-}" in ''|*[!0123456789]*) fail "invalid DHCP owner" 73;; esac
  owner=$(cat "$LEASE_DIR/owner") || fail "missing DHCP owner" 73
  [ "$owner" = "$TECLAST_DHCP_OWNER" ] && [ "$owner" -gt 1 ] && kill -0 "$owner" 2>/dev/null || fail "inactive DHCP owner" 73
  IP_BIN=$(service_binary ip) || fail "ip is unavailable" 69
  case "$1" in
    bound|renew)
      if plan_lease; then
        if apply_lease; then return 0; fi
        # A failed add must unwind only this instance's successful partial adds.
        if ! clear_lease; then
          printf 'cleanup-failed\n' > "$LEASE_DIR/state"
          fail "DHCP apply and owned configuration cleanup failed" 74
        fi
      elif [ -f "$LEASE_DIR/lease" ]; then
        # udhcpc adopts the ACK even if its hook fails. The old configuration
        # must not outlive its lease while the client waits on that new ACK.
        if ! clear_lease; then
          printf 'cleanup-failed\n' > "$LEASE_DIR/state"
          stop_rejected_renewal || :
          fail "DHCP renewal rejected and owned configuration cleanup failed" 74
        fi
        printf 'lease-rejected\n' > "$LEASE_DIR/state"
        stop_rejected_renewal || :
        fail "DHCP renewal rejected; previous owned configuration removed" 74
      fi
      printf 'lease-rejected\n' > "$LEASE_DIR/state"
      fail "DHCP lease rejected or application failed" 74;;
    deconfig|nak|leasefail)
      if clear_lease; then printf 'waiting\n' > "$LEASE_DIR/state"; else
        printf 'cleanup-failed\n' > "$LEASE_DIR/state"; fail "owned DHCP configuration cleanup failed" 74
      fi;;
    *) usage;;
  esac
}
dhcp_shutdown() {
  local result=$1
  trap - 0 HUP INT TERM USR1
  if [ -n "$dhcp_pid" ]; then
    # Only the child created by this shell is signalled; a PID file is never
    # used as authority to stop an unrelated process.
    kill -TERM "$dhcp_pid" 2>/dev/null || :
    wait "$dhcp_pid" 2>/dev/null || :
  fi
  if clear_lease; then
    rm -f "$LEASE_DIR"/*
    rmdir "$LEASE_DIR" || result=74
  else
    printf 'cleanup-failed\n' > "$LEASE_DIR/state"
    printf 'error=owned DHCP configuration retained after cleanup failure\n' >&2
    result=74
  fi
  exit "$result"
}
run_dhcp() {
  [ "$#" -eq 1 ] || usage
  iface=$1
  valid_ifname "$iface" || fail "invalid interface name" 64
  [ "$(module_state wlan_drv_gen4m_6789)" = loaded ] && has_wiphy && wifi_iface_present "$iface" || fail "Wi-Fi interface is not ready" 69
  status=$(query_wifi "$iface") || fail "cannot query Wi-Fi association" 69
  case "$status" in *wifi.connection=COMPLETED*) ;; *) fail "Wi-Fi is not associated" 69;; esac
  IP_BIN=$(service_binary ip) || fail "ip is unavailable" 69
  UDHCPC=$(service_binary udhcpc) || fail "udhcpc is unavailable" 69
  case "$0" in /*) ;; *) fail "DHCP event helper requires an absolute executable path" 64;; esac
  dhcp_paths
  [ ! -L "$DHCP_BASE" ] && [ ! -L "$LEASE_DIR" ] || fail "DHCP state directory is a symlink" 73
  [ -L "$ETC_ROOT/resolv.conf" ] && [ "$(readlink "$ETC_ROOT/resolv.conf")" = "$DHCP_BASE/resolv.conf" ] || fail "resolver is not delegated to this service" 73
  (umask 022; mkdir -p "$DHCP_BASE") || fail "cannot create DHCP state directory" 73
  (umask 077; mkdir "$LEASE_DIR") || fail "DHCP service or retained state already exists" 73
  dhcp_pid=
  trap 'dhcp_shutdown $?' 0
  trap 'exit 129' HUP
  trap 'exit 130' INT
  trap 'exit 143' TERM
  trap 'exit 74' USR1
  umask 077
  printf '%s\n' "$$" > "$LEASE_DIR/owner"
  printf 'dhcp-running\n' > "$LEASE_DIR/state"
  TECLAST_DHCP_IFACE=$iface TECLAST_DHCP_DIR=$LEASE_DIR TECLAST_DHCP_OWNER=$$
  export TECLAST_DHCP_IFACE TECLAST_DHCP_DIR TECLAST_DHCP_OWNER
  # Do not inherit stale DHCP option variables into the client/event process.
  unset ip subnet router dns staticroutes interface
  "$UDHCPC" -f -R -i "$iface" -s "$0" -p "$LEASE_DIR/client.pid" -O staticroutes > "$LEASE_DIR/client.log" 2>&1 &
  dhcp_pid=$!
  if wait "$dhcp_pid"; then result=0; else result=$?; fi
  dhcp_pid=
  state=$(cat "$LEASE_DIR/state")
  if [ "$result" -eq 0 ]; then
    case "$state" in lease-rejected|cleanup-failed) result=74;; esac
  fi
  exit "$result"
}

[ "$#" -ge 1 ] || usage
command_name=$1
shift
case "$command_name" in
  probe) [ "$#" -eq 0 ] || usage; emit_probe;;
  wmt-run) run_wmt "$@";;
  wifi-power-on) [ "$#" -eq 0 ] || usage; run_wifi_power on;;
  wifi-power-off) [ "$#" -eq 0 ] || usage; run_wifi_power off;;
  dbus-run) run_dbus "$@";;
  bt-service-run) run_bluez "$@";;
  bt-run) run_bt "$@";;
  wifi-run) run_wifi "$@";;
  wifi-status) query_wifi "$@";;
  wifi-dhcp-run) run_dhcp "$@";;
  bound|renew|deconfig|nak|leasefail) [ "$#" -eq 0 ] || usage; dhcp_event "$command_name";;
  *) usage;;
esac
