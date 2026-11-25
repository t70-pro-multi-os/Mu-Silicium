// Test-only SSDT; never included in device firmware.
DefinitionBlock ("", "SSDT", 2, "TECLST", "HALLTEST", 1)
{
    External (\_SB.LRAD, FieldUnitObj)
    External (\_SB.LPIN, FieldUnitObj)
    External (\_SB.LFAR, FieldUnitObj)
    External (\_SB.LREV, FieldUnitObj)
    External (\_SB.GPO0.HPIN, FieldUnitObj)
    External (\_SB.GPO0._DSM, MethodObj)
    Scope (\_SB)
    {
        Method (LSET, 4, Serialized)
        { Store (Arg0, LRAD) Store (Arg1, LPIN) Store (Arg2, LFAR) Store (Arg3, LREV) }
        Method (LDSM, 3, Serialized)
        {
            If (Arg0) { Return (\_SB.GPO0._DSM (ToUUID ("4F248F40-D5E2-499F-834C-27758EA1CD3F"), Arg1, Arg2, Package () {})) }
            Return (\_SB.GPO0._DSM (ToUUID ("00000000-0000-0000-0000-000000000000"), Arg1, Arg2, Package () {}))
        }
        // AcpiExec virtual GPIO only. Production AML never writes this input.
        Method (HRAW, 1, Serialized) { Store (Arg0, \_SB.GPO0.HPIN) }
    }
}
