# Stock power read-only DXE service

This module installs an OEM protocol whose methods call the portable SC8989x and HUSB311 status core through the EFI_I2C_MASTER_PROTOCOL. It performs no bus access during entry-point installation.

Before a call can reach the bus, a platform provider must install gStockPowerBoardReadyProtocolGuid with revision 1, the read permission flag, the actual I2C master handle, and the verified 7-bit targets 0x6a (charger) and 0x4e (TCPC). The service rejects missing or mismatched markers and never enumerates or scans addresses. The platform provider must publish the marker only after it has established that this board's controller and target addresses are correct.

Each transfer contains exactly two I2C operations: write one register-pointer byte, then read one or two bytes in the second operation. The PI I2C master request contract defines a repeated START between the operations. No register-value write operation is exposed. The 7-bit addresses and marker checks come from the T70 Pro board integration contract.

The HUSB311 status calls verify the VID/DID pair before reading status. SC8989x status checks the stock driver's accepted part-code set before reading status. The ADC method is pure conversion only.

Register-pointer transactions can still have device-side effects. Static analysis does not establish whether status, alert, or fault reads clear or latch state; the method names and permission flag do not claim otherwise. The public service is synchronous and must be called at or below TPL_CALLBACK.

The INF is intended for DSC/source validation only and is deliberately absent from FDF. No actual I2C transfer or tablet boot is claimed.
