# T70 Pro: touch input is not proof of active pen support

The captured effective stock DT (SHA-256
a3dd45e28ee284f1995374091eca9634f68c88dfddc5ecd27a37cdcc877dd391)
has an enabled himax,hxcommon at /soc/i2c@11e00000/himax_ts@48.
It supplies the 1400 by 2240 geometry, touch report type and the signal
GPIOs. Neither that node nor the full captured effective DT contains a
stylus-func property, including the vendor-prefixed form.

The exact stock module hxchipset_i2c.ko (SHA-256
c1d7d3ae53580d3d3a0b7cfdd99e01785d5d8fd6b5fd42b5d89659c00bd64929)
has a conditional Himax pen implementation:

- himax_parse_dt_ic_info constructs a vendor-prefixed stylus-func property.
  Only a successful property read stores the configured stylus byte
  (.text offsets 0x1c634–0x1c65c).
- himax_mcu_touch_information converts the 0xff unset value to zero
  (0x14454–0x14460). himax_mcu_tp_info_check compares the configured value
  with a separate firmware observation (0x14274–0x14280); the pen-specific
  path is gated by the configured byte (0x142a0–0x142a4).
- himax_dev_set only allocates the separate himax-stylus input device
  when the configured byte is nonzero (0x1bf2c–0x1bf4c). The driver contains
  explicit hover, tip, pressure, tilt and side-button handling. Presence of
  those common module paths does not establish the tablet variant's sensor,
  a pen protocol, or compatibility with any pen model.

The [Windows driver](Driver.c) exposes a touchscreen/finger HID collection,
not a pen collection. The common decoder expects the 56-byte T70 touch report;
the optional 12-byte pen extension is not parsed. Finger contact pressure
must not be presented as active-pen pressure. Do not enlarge the touch
report or advertise a pen collection based only on the stock module names.

Before implementing an active-pen branch, obtain matching board-variant
evidence of a nonzero configured enable flag, a registered stock pen device,
and actual valid pen packets for the same panel. Until then the correct status
is: touch implemented; the captured DT does not enable stock active-pen
registration; hardware and pen-protocol compatibility unknown. This audit
did not transact with the controller or modify its firmware or DT.

## Exact Android-side selector

The stock parser reads himax,proj-name, falling back to the literal himax
when absent (.text 0x1c2bc–0x1c388). Both himax,proj-name and
himax,stylus-func are absent at the node in the captured DT. Thus this
variant's trial selector is the single 32-bit cell
himax,stylus-func = <1>; on that existing node, not a sibling node or an
unprefixed flag. A disposable, RAM-only dtc/fdtoverlay comparison confirmed
that it adds exactly that property and changes no other DT field. This
is a DT merge check only. It did not boot a modified image or prove any
pen events, and the stock DT file was unchanged.
