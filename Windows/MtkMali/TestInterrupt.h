/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* OS interrupt-priority/spin-lock boundary only. Production calls real WDK
   services. These deterministic tests do not execute Windows multiprocessors. */
#ifndef T70_MALI_TEST_INTERRUPT_H
#define T70_MALI_TEST_INTERRUPT_H
static unsigned TestIrqLockHeld,TestIrqAcquires,TestIrqReleases,TestIrqRaises;
static PKSPIN_LOCK TestIrqOwnedLock;
static void (*TestIrqBeforeAcquire)(void),(*TestIrqAfterLower)(void);
static void TestIrqInitialize(PKSPIN_LOCK Lock) {CHECK(Lock && !TestIrqLockHeld);*Lock=0;}
static void TestIrqRaise(KIRQL Level,PKIRQL Previous) {
    CHECK(Previous && Level>=Irql && Level>DISPATCH_LEVEL && Level<PROFILE_LEVEL);
    *Previous=Irql;Irql=Level;++TestIrqRaises;
}
static void TestIrqAcquire(PKSPIN_LOCK Lock) {
    if(TestIrqBeforeAcquire){void (*F)(void)=TestIrqBeforeAcquire;TestIrqBeforeAcquire=0;F();}
    CHECK(Lock && !*Lock && !TestIrqLockHeld && Irql>DISPATCH_LEVEL && Irql<PROFILE_LEVEL);
    *Lock=1;TestIrqOwnedLock=Lock;TestIrqLockHeld=1;++TestIrqAcquires;
}
static void TestIrqRelease(PKSPIN_LOCK Lock) {
    CHECK(Lock && *Lock==1 && TestIrqLockHeld && TestIrqOwnedLock==Lock && Irql>DISPATCH_LEVEL);
    *Lock=0;TestIrqLockHeld=0;TestIrqOwnedLock=0;++TestIrqReleases;
}
static void TestIrqLower(KIRQL Level) {
    CHECK(Level<=Irql && !TestIrqLockHeld);Irql=Level;
    if(TestIrqAfterLower){void (*F)(void)=TestIrqAfterLower;TestIrqAfterLower=0;F();}
}
static void TestIrqResources(CM_PARTIAL_RESOURCE_DESCRIPTOR *R) {
    RtlZeroMemory(R,3u*sizeof(*R));
    for(UINT I=0;I<3;I++) {
        R[I].Type=CmResourceTypeInterrupt;R[I].ShareDisposition=CmResourceShareDeviceExclusive;
        R[I].u.Interrupt.Level=(USHORT)(5+I);R[I].u.Interrupt.Vector=200+I;R[I].u.Interrupt.Affinity=0xff;
    }
}
#undef KeInitializeSpinLock
#undef KeRaiseIrql
#undef KeLowerIrql
#undef KeAcquireSpinLockAtDpcLevel
#undef KeReleaseSpinLockFromDpcLevel
#define KeInitializeSpinLock TestIrqInitialize
#define KeRaiseIrql TestIrqRaise
#define KeLowerIrql TestIrqLower
#define KeAcquireSpinLockAtDpcLevel TestIrqAcquire
#define KeReleaseSpinLockFromDpcLevel TestIrqRelease
#endif
