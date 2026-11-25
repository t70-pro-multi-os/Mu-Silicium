// SPDX-License-Identifier: BSD-2-Clause-Patent
// Test-only SSDT. verify-acpi starts with a valid rev1 record and mutates fields for rev2 cases.
// ACPICA uses simulated RAM; this is never included in the FV.
DefinitionBlock ("", "SSDT", 2, "TECLST", "MUSBTEST", 1)
{
    External (\_SB.MHHO, OpRegionObj)
    External (\_SB.MUMX, FieldUnitObj)
    External (\_SB.MUGT, FieldUnitObj)
    External (\_SB.MUGC, FieldUnitObj)
    External (\_SB.MUPC, FieldUnitObj)
    External (\_SB.MUPW, FieldUnitObj)
    External (\_SB.MUPD, FieldUnitObj)
    External (\_SB.UPC0, FieldUnitObj)
    External (\_SB.UPC1, FieldUnitObj)
    External (\_SB.UPWR, FieldUnitObj)
    Scope (\_SB)
    {
        Field (MHHO, DWordAcc, NoLock, Preserve) { MRWS, 512 }
        Method (MINI, 0, NotSerialized)
        {
            Store (Buffer (64) {
                1,0,0,0, 1,0,0,0, 0x7F,0,0,0, 0,0,0x20,0x11,
                0,0,1,0, 0,0,0xF4,0x11, 0,0,1,0, 0xA0,0,0,0,
                16,0,0,0, 1,0,0,0, 1,0,0,0, 0x30,0,0,0,
                0,1,0,0, 0,0,0,0, 1,0,0,0, 0,0,0,0
            }, MRWS)
        }
        Method (MLIV, 7, Serialized)
        {
            Store (Arg0, MUMX)
            Store (Arg1, MUGT)
            Store (Arg2, MUPC)
            Store (Arg3, MUPW)
            Store (Arg4, UPC0)
            Store (Arg5, UPC1)
            Store (Arg6, UPWR)
        }
        Method (MSTC, 1, Serialized) { Store (Arg0, MUPD) }
        Method (MGWR, 1, Serialized) { Store (Arg0, MUGC) }
        Method (MGRD, 0, Serialized) { Return (MUGC) }
        Method (MSET, 2, Serialized)
        {
            If (LGreaterEqual (Arg0, 16)) { Return () }
            Store (MRWS, Local0)
            Multiply (Arg0, 4, Local1)
            CreateDWordField (Local0, Local1, TEMP)
            Store (Arg1, TEMP)
            Store (Local0, MRWS)
        }
    }
}
