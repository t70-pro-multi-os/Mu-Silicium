# T70 Pro ARM64 RAM diagnostics

This is a minimal diagnostic shell, not the Ubuntu desktop live ISO. The
Ubuntu 26.04 arm64 7.0.0-43-generic kernel, BusyBox root and eight modules from
../BUILDS.md share a single unsigned UKI. The EFI fallback loader is
EFI/BOOT/BOOTAA64.EFI in a GPT FAT32 ESP. Linux runs /init from the embedded
initramfs and does not mount the boot disk. The DKMS modules are present but
are never automatically inserted by this diagnostic init.

Inputs: the matching official Ubuntu linux-image/linux-modules packages
(version 7.0.0-43.43), systemd ARM64 linuxaa64.efi.stub and ukify, standalone
ARM64 BusyBox, and the exact module files listed in ../BUILDS.md. Verify
package index hashes and run ../verify-builds.py against the official target
headers before assembly. Build inside the Linux VM; no installation is needed.
Do not substitute an Android GKI or load these modules under another ABI.

1. Stage ./init as /init, BusyBox at /bin/busybox, and the BusyBox applet
   symlinks. Create /dev, /proc, /sys, /run, /tmp, /etc. Copy the eight
   verified modules under /lib/modules/7.0.0-43-generic/updates/dkms.
   Create /etc/t70-modules.sha256 from their verified relative paths.
2. Run embed-dependencies.py against the extracted Ubuntu
   usr/lib/modules/7.0.0-43-generic directory and the staging root, then
   run depmod -b ROOT 7.0.0-43-generic. Archive ROOT with newc cpio/gzip.
3. Run extract-ubuntu-kernel.py on the hash-verified Ubuntu vmlinuz. Ubuntu
   packages it with an outer EFI wrapper; do not nest that wrapper in ukify.
   Use ukify build with the ARM64 stub, the extracted kernel, that initrd,
   and the command line console=tty0 console=ttyAMA0,115200 loglevel=4.
   Run make-esp-image.py on the resulting BOOTAA64.EFI and a new .img path.
4. Run test-qemu-usb.py with the raw image and a new serial log path.
   The serial acceptance criteria are Linux 7.0.0-43-generic, a RAM root
   signal, successful hashes for all eight files and a responsive shell.
   Do not infer that real T70 USB, GPIO, or peripherals are configured
   from the virtual USB result.

   With --check-modules, the test also inserts and checks all eight modules
   in the disposable guest, with no T70 DT nodes, hardware or media writes.
   This tests registration and dependencies, not peripheral binding.

   The current image chooses ttyAMA0 when it exists and mirrors to tty0
   only when ttyAMA0 is absent. With QEMU ramfb, /proc/fb reported
   simpledrmdrmfb and the virtual screen accepted a direct tty0 message
   while T70_LIVE_READY appeared only in serial. A blank screen or absent
   READY marker therefore does not establish a boot failure. This test
   does not establish the behavior of the tablet's GOP handoff.

The diagnostic image and all its DKMS modules are unsigned and require an
EFI environment that accepts unsigned binaries. This project does not
change any platform's Secure Boot policy. Do not install the image into
internal storage or treat it as a complete T70 Linux installation.
