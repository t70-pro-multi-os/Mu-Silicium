# Windows ARM64: WinPE and the installed system have different boot paths

## Verified in the current WinPE media

The November 22 refreshed ARM64 WinPE has a BCD whose device and osdevice
are a RAM disk backed by sources/boot.wim, not the tablet's UFS or SD as
the Windows system volume. boot.wim SHA-256 is
66e18b59d384556481363c1d00b515c10177843e30bbc7b09e7a91d571bf4337;
BCD SHA-256 is 2ddcc303a3249ec2b4d58294755e2bb2ca0001dc4618374267cfc9007e4f7056.
The refreshed media has no install.wim/esd/swm. The older board-model
Windows Setup screen and the refreshed generic VM run are separate results.

Both WinPE indexes list the following packages in the existing DISM results.
The stored INF copies matched the tracked inputs when that image was built.
The source UFS INFs were subsequently revised for native boot from internal UFS;
the existing WinPE image and its hashes do not contain that revision.

| Tracked INF | Role | INF SHA-256 |
| --- | --- | --- |
| [MtkUfs](MtkUfs/MtkUfs.inf) | UFS miniport, SCSI Miniport group | cd4aa84c38c15ef80a55047d7a0c2a9981771c8383a1336d470724165a9b2203 |
| [MtkUfsPower](MtkUfsPower/MtkUfsPower.inf) | Upper filter for ACPI HANA678B | 42c98f8c6ff02e0c8dd1785290baa3348144c88db229f6f2eb34427f53951a83 |
| [MtkSd](MtkSd/MtkSd.inf) | SD host, System Bus Extender group | bf8572388812d28bc2ba1fe88824453ecacd57521f5b099cc86b3ff11c216ce5 |
| [MtkUsb](MtkUsb/MtkUsb.inf) | USB host controller with UCX dependency | 533c3611073d8d9a2dc5004f67e981037d6a2e84fc7023b89a5eead3265ef60d |

This establishes original package defaults, not the effective SYSTEM
registry's Start/StartOverride, an actual device binding, or an observed
storage read after Windows takes control. Listing partitions in EFI only
establishes the firmware-side path.

The current tracked UFS INF sources set both MtkUfs and its PRM upper filter
to StartType=0 (SERVICE_BOOT_START). The Storport miniport remains in the SCSI
Miniport group. MtkSd and its GPIO provider MtkGpio use demand-start
with BootFlags=0x88 for SD/WinPE boot. MtkUsb uses demand-start with
BootFlags=0x80 for WinPE.
Windows/test-storage-boot-inf.py checks the source settings and is run by
Windows/build.py for --ufs, --ufs-power, --sd, and --usb. ARM64 UFS, UFS
power-filter, SD, and USB links pass. The
existing WinPE image still contains the earlier demand-start UFS packages.
Source INF and unsigned link results do not prove signature acceptance or an
installed system boot.


## Unclosed gate for installed Windows on UFS or SD

1. Use an independently identified ARM64 install image and validate the
   registered storage services in that image, not just in boot.wim.
2. For the chosen system disk, verify the needed storage stack and any
   required filters/providers can start in the boot phase. UFS includes
   the ACPI HANA678B binding, Storport miniport, and PRM-dependent power
   filter. Verify the filter's early dependencies and its role before
   changing any start mode. A group name alone does not promote a service,
   and the boot loader does not use INF Dependencies to order boot drivers.
3. Observe volume identity and a successful OS read after the firmware
   handoff on the chosen path; then verify a first boot of the installed
   system. Distinguish this from a RAM-backed PE or a generic NVMe guest.

References: [Windows driver load order](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/specifying-driver-load-order)
and [installing a boot-start driver](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/installing-a-boot-start-driver).
This update changes only the tracked source INF files; it changes no installed registry, BCD, disk, or device state.
