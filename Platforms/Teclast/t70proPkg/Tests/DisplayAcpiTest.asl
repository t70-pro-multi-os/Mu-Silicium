// Test-only SSDT; never added to firmware.
DefinitionBlock ("", "SSDT", 2, "TECLST", "DSPTEST ", 1)
{
    External (\_SB.DRAD, FieldUnitObj)
    External (\_SB.DREV, FieldUnitObj)
    External (\_SB.DBLO, FieldUnitObj)
    External (\_SB.DBHI, FieldUnitObj)
    External (\_SB.DSLO, FieldUnitObj)
    External (\_SB.DSHI, FieldUnitObj)
    External (\_SB.DWID, FieldUnitObj)
    External (\_SB.DHEI, FieldUnitObj)
    External (\_SB.DPIT, FieldUnitObj)
    External (\_SB.DFMT, FieldUnitObj)
    External (\_SB.DRL0, FieldUnitObj)
    External (\_SB.DRL1, FieldUnitObj)
    Scope (\_SB)
    {
        Method (DINI, 0, NotSerialized)
        {
            Store (0x1, DRAD)
            Store (0x1, DREV)
            Store (0x80000000, DBLO)
            Store (0x0, DBHI)
            Store (0xC00000, DSLO)
            Store (0x0, DSHI)
            Store (0x578, DWID)
            Store (0x8C0, DHEI)
            Store (0x15E0, DPIT)
            Store (0x1, DFMT)
            Store (0x0, DRL0)
            Store (0x0, DRL1)
        }
        Method (DSET, 2, Serialized)
        {
            Switch (ToInteger (Arg0))
            {
                Case (0) { Store (Arg1, DRAD) }
                Case (1) { Store (Arg1, DREV) }
                Case (2) { Store (Arg1, DBLO) }
                Case (3) { Store (Arg1, DBHI) }
                Case (4) { Store (Arg1, DSLO) }
                Case (5) { Store (Arg1, DSHI) }
                Case (6) { Store (Arg1, DWID) }
                Case (7) { Store (Arg1, DHEI) }
                Case (8) { Store (Arg1, DPIT) }
                Case (9) { Store (Arg1, DFMT) }
                Case (10) { Store (Arg1, DRL0) }
                Case (11) { Store (Arg1, DRL1) }
            }
        }
    }
}
