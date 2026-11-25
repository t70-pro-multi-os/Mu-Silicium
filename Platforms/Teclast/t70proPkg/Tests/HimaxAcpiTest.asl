// Test-only SSDT: never added to firmware.
DefinitionBlock ("", "SSDT", 2, "TECLST", "HIMAXTST", 1)
{
    External (\_SB.HRAD, FieldUnitObj)
    External (\_SB.HADR, FieldUnitObj)
    External (\_SB.HIRQ, FieldUnitObj)
    External (\_SB.HREV, FieldUnitObj)
    External (\_SB.IPIN, FieldUnitObj)
    External (\_SB.HXT0.HITD, FieldUnitObj)
    External (\_SB.HXT0.HRTD, FieldUnitObj)
    External (\_SB.HXT0.HRTL, FieldUnitObj)
    External (\_SB.HXT0.HITM, FieldUnitObj)
    External (\_SB.HXT0.HRTM, FieldUnitObj)
    External (\_SB.HXT0.HITX, FieldUnitObj)
    External (\_SB.HXT0.HRTX, FieldUnitObj)
    Scope (\_SB)
    {
        Method (HSET, 4, NotSerialized)
        {
            Store (Arg0, HRAD)
            Store (Arg1, HADR)
            Store (Arg2, HIRQ)
            Store (Arg3, HREV)
        }
        Method (HRTS, 1, NotSerialized)
        {
            Store (Zero, \_SB.HXT0.HITD)
            Store (0x01000000, \_SB.HXT0.HRTD)
            Store (0x01000000, \_SB.HXT0.HRTL)
            Store (Zero, \_SB.HXT0.HITM)
            Store (Zero, \_SB.HXT0.HRTM)
            Store (One, \_SB.HXT0.HITX)
            Store (0x00400000, \_SB.HXT0.HRTX)
            Store (0x01000000, IPIN)
            If (LEqual (Arg0, 1)) { Store (0x0200, \_SB.HXT0.HITD) }
            If (LEqual (Arg0, 2)) { Store (Zero, \_SB.HXT0.HRTD) }
            If (LEqual (Arg0, 3)) { Store (Zero, \_SB.HXT0.HRTL) }
            If (LEqual (Arg0, 4)) { Store (0xF0, \_SB.HXT0.HITM) }
            If (LEqual (Arg0, 5)) { Store (One, \_SB.HXT0.HRTM) }
            If (LEqual (Arg0, 6)) { Store (Zero, \_SB.HXT0.HITX) }
            If (LEqual (Arg0, 7)) { Store (Zero, \_SB.HXT0.HRTX) }
            If (LEqual (Arg0, 8)) { Store (Zero, IPIN) }
        }
    }
}
