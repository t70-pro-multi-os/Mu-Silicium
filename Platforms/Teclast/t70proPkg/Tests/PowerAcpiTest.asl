// SPDX-License-Identifier: BSD-2-Clause-Patent
// Load only in AcpiExec to select the simulated charger IRQ handoff.
DefinitionBlock ("", "SSDT", 2, "TECLST", "POWRTEST", 1)
{
    External (\_SB.PARD, FieldUnitObj)
    Scope (\_SB)
    {
        Method (ASET, 1, NotSerialized) { Store (Arg0, PARD) }
    }
}
