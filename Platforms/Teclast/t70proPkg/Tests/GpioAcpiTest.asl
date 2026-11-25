// Test-only SSDT: never added to firmware.
DefinitionBlock ("", "SSDT", 2, "TECLST", "GPIOTEST", 1)
{
    External (\_SB.GRAD, FieldUnitObj)
    External (\_SB.GPIN, FieldUnitObj)
    External (\_SB.GIRQ, FieldUnitObj)
    External (\_SB.GREV, FieldUnitObj)
    Scope (\_SB)
    {
        Method (GSET, 4, NotSerialized)
        {
            Store (Arg0, GRAD)
            Store (Arg1, GPIN)
            Store (Arg2, GIRQ)
            Store (Arg3, GREV)
        }
    }
}
