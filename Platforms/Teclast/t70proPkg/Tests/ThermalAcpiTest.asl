// SPDX-License-Identifier: BSD-2-Clause-Patent
DefinitionBlock ("", "SSDT", 2, "TECLST", "THRMTEST", 1)
{
    External (\_SB.AXCK.AXST, FieldUnitObj)
    External (\_SB.AXCK.AXSE, FieldUnitObj)
    External (\_SB.AXCK.AXCL, FieldUnitObj)
    External (\_SB.NTC0.TVST, FieldUnitObj)
    External (\_SB.NTC0.TVCL, FieldUnitObj)

    Method (\_SB.VTST, 1, NotSerialized) { Store (Arg0, \_SB.NTC0.TVST) }
    Method (\_SB.VTRD, 0, NotSerialized)
    {
        Return (Package () { \_SB.NTC0.TVST, \_SB.NTC0.TVCL })
    }

    Method (\_SB.NSET, 1, NotSerialized) { Store (Arg0, \_SB.AXCK.AXST) }
    Method (\_SB.NGET, 0, NotSerialized)
    {
        Return (Package () { \_SB.AXCK.AXSE, \_SB.AXCK.AXCL })
    }
}
