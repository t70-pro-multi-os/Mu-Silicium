# T70 Pro GNSS and A-GPS integration handoff

## Observed device provenance

From the existing hashed stock vendor_a GNSS extraction and ELF dynamic
symbols, not from network or receiver operation:

| Stock artifact | SHA-256 | Observed role |
| --- | --- | --- |
| gps_drv_stp.ko | 938639b15d88f4a2d777c14bdd777408c561c341cc8805c250c4db3568908260 | GPS character-device transport over WMT/STP; read/write transport uses channel 2 |
| mnld | 84a1fb54c907a5b67db931cadf2af674163110f7b30703c62fc27839fce2c390 | Imports the vendor assistance setters plus EPO/QEPO/MTKNAV update functions |
| libmnl.so | ae71dc763ef0f69ee2be01fde6dd7c48e4c4ddd2aab4cd3eeaa868d0abb3c319 | Exports EPO verification, extraction and update, along with assistance agents |
| android.hardware.gnss-impl-mediatek.so | 968f41196e50b2be082fc105435962f0676c75dcbe059f932d1a27fbaef28ef2 | Exports the MediaTek dispatcher for SUPL server settings and time/location injection, separate from its NMEA callback |

Static references do not prove that the stock modem firmware, assistance
network or position engine accepted a request. The STP payload is vendor
binary data, not a raw NMEA stream. The offline shared frame tests and
NMEA tests therefore cover different boundaries.

## Work in dependency order

1. Keep the existing conninfra hardware owner. Connect one real receiver
   provider to the Windows child; do not create a fake root-enumerated
   receiver or add overlapping ACPI resource ownership. The existing
   [UMDF consumer](Driver.c) needs the [receiver contract](Receiver.h)
   and expects the child device ID in [the INF](MtkGnss.inf).
2. Connect valid position-engine output and prove that an observed fix
   comes from the current lease, with an independent fix timestamp and
   correct uncertainty. Passing frame decoding, GET queue or NMEA tests
   does not by itself supply a position engine.
3. Implement assistance only after the provider can accept and acknowledge
   the specific data types it really supports. The current Windows front
   end accepts the platform capability handshake but does not implement
   LISTEN_AGNSS or INJECT_AGNSS or advertise assistance formats. A clear
   assistance command is not data injection. Establish time/position and
   any extended-ephemeris support separately. Do not infer Windows support
   from a stock Android SUPL dispatcher or reuse its endpoints or settings.

Windows references: [driver requirements](https://learn.microsoft.com/en-us/windows-hardware/drivers/gnss/gnss-driver-requirements),
[AGNSS injection](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/gnssdriver/ni-gnssdriver-ioctl_gnss_inject_agnss),
and [device capabilities](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/gnssdriver/ns-gnssdriver-gnss_device_capability).
No assistance server was contacted and no device settings or binaries changed
in this cross-check.
