// SPDX-License-Identifier: BSD-2-Clause-Patent
// Loaded only by ACPICA's simulated-memory test process.
DefinitionBlock ("", "SSDT", 2, "TECLST", "I2CTEST ", 1)
{
    External (\_SB.IRAD, FieldUnitObj)
    External (\_SB.ICL0, FieldUnitObj)
    External (\_SB.ICL6, FieldUnitObj)
    External (\_SB.IREV, FieldUnitObj)
    External (\_SB.ICL1, FieldUnitObj)
    External (\_SB.IST1, FieldUnitObj)
    External (\_SB.IDM1, FieldUnitObj)
    External (\_SB.IAXI, FieldUnitObj)
    External (\_SB.IMUX, FieldUnitObj)
    External (\_SB.IFG0, FieldUnitObj)
    External (\_SB.IFG2, FieldUnitObj)
    External (\_SB.IWG0, FieldUnitObj)
    External (\_SB.IWG6, FieldUnitObj)
    External (\_SB.IWC0, FieldUnitObj)
    External (\_SB.IWC6, FieldUnitObj)
    External (\_SB.IPM0, FieldUnitObj)
    External (\_SB.IPM6, FieldUnitObj)
    External (\_SB.IPIN, FieldUnitObj)
    External (\_SB.IST0, FieldUnitObj)
    External (\_SB.IST6, FieldUnitObj)
    External (\_SB.IDM0, FieldUnitObj)
    External (\_SB.IDM6, FieldUnitObj)
    External (\_SB.MPC0, FieldUnitObj)
    External (\_SB.MPC1, FieldUnitObj)
    External (\_SB.MPWR, FieldUnitObj)
    External (\_SB.UPC0, FieldUnitObj)
    External (\_SB.UPC1, FieldUnitObj)
    External (\_SB.UPWR, FieldUnitObj)
    Scope (\_SB)
    {
        Method (TLIV, 2, Serialized)
        {
            Switch (Arg0)
            {
                Case (0) { Store (Arg1, IAXI) }
                Case (1) { Store (Arg1, IMUX) }
                Case (2) { Store (Arg1, IFG0) }
                Case (3) { Store (Arg1, IFG2) }
                Case (4) { Store (Arg1, IWG0) }
                Case (5) { Store (Arg1, IWG6) }
                Case (6) { Store (Arg1, IPM0) }
                Case (7) { Store (Arg1, IPM6) }
                Case (8) { Store (Arg1, IPIN) }
                Case (9) { Store (Arg1, IST0) }
                Case (10) { Store (Arg1, IST6) }
                Case (11) { Store (Arg1, IDM0) }
                Case (12) { Store (Arg1, IDM6) }
                Case (13) { Store (Arg1, MPC0) }
                Case (14) { Store (Arg1, MPC1) }
                Case (15) { Store (Arg1, MPWR) }
                Case (16) { Store (Arg1, UPC0) }
                Case (17) { Store (Arg1, UPC1) }
                Case (18) { Store (Arg1, UPWR) }
                Case (19) { Store (Arg1, ICL1) }
                Case (20) { Store (Arg1, IST1) }
                Case (21) { Store (Arg1, IDM1) }
                Case (22) { Store (Arg1, IWC0) }
                Case (23) { Store (Arg1, IWC6) }
            }
        }
        Method (TWRP, 1, NotSerialized)
        {
            If (Arg0) { Return (IWC6) }
            Return (IWC0)
        }
        Method (TSET, 4, NotSerialized)
        {
            Store (Arg0, IRAD)
            Store (Arg1, ICL0)
            Store (Arg2, ICL6)
            Store (Arg3, IREV)
        }
    }
}
