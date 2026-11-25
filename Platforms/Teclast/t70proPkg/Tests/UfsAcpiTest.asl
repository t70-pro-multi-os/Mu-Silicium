// Test-only SSDT: never added to firmware.
DefinitionBlock ("", "SSDT", 2, "TECLST", "UFSTEST ", 1)
{
    External (\_SB.URAD, FieldUnitObj)
    External (\_SB.UBAR, FieldUnitObj)
    External (\_SB.ULUN, FieldUnitObj)
    External (\_SB.UREV, FieldUnitObj)
    Scope (\_SB)
    {
        Method (USET, 4, NotSerialized)
        {
            Store (Arg0, URAD)
            Store (Arg1, UBAR)
            Store (Arg2, ULUN)
            Store (Arg3, UREV)
        }
    }
}
