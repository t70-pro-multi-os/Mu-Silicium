/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef T70_MALI_TEST_WORKER_H
#define T70_MALI_TEST_WORKER_H
static PKSTART_ROUTINE ThreadRoutine;
static PVOID ThreadContext;
static unsigned ThreadCreateFailure,ThreadWaitFailure,ThreadCloseFailure,Terminated,Closed,Signals,CallRefs,Closing;
static unsigned WaitStage,WorkerFault;
static LONG64 CounterFrequency=1000,CounterTicks;
static NTSTATUS CreateThread(PHANDLE Handle,ACCESS_MASK Access,POBJECT_ATTRIBUTES Attributes,
    HANDLE Process,PCLIENT_ID Client,PKSTART_ROUTINE Routine,PVOID Context) {
    CHECK(Irql==PASSIVE_LEVEL && Handle && Access==SYNCHRONIZE && Attributes && !Process && !Client);
    CHECK((Attributes->Attributes&OBJ_KERNEL_HANDLE) && Routine && Context==TestWorker);
    if(ThreadCreateFailure)return STATUS_INSUFFICIENT_RESOURCES;
    ThreadRoutine=Routine;ThreadContext=Context;Terminated=0;*Handle=(HANDLE)0x123456;return STATUS_SUCCESS;
}
static NTSTATUS WaitThread(HANDLE Thread,BOOLEAN Alert,PLARGE_INTEGER Timeout) {
    CHECK(Irql==PASSIVE_LEVEL && Thread==(HANDLE)0x123456 && !Alert && !Timeout);
    if(ThreadWaitFailure)return ThreadWaitFailure==2 ? STATUS_TIMEOUT : STATUS_UNSUCCESSFUL;
    unsigned Before=Signals;MtkMaliWorkerWake(TestWorker);CHECK(Signals==Before);
    if(!Terminated)ThreadRoutine(ThreadContext);
    CHECK(Terminated);return STATUS_SUCCESS;
}
static NTSTATUS CloseThread(HANDLE Thread) {CHECK(Thread==(HANDLE)0x123456 && Terminated);if(ThreadCloseFailure)return STATUS_UNSUCCESSFUL;++Closed;return STATUS_SUCCESS;}
static NTSTATUS Terminate(NTSTATUS Status) {CHECK(Irql==PASSIVE_LEVEL && Status==STATUS_SUCCESS);++Terminated;return STATUS_SUCCESS;}
static LARGE_INTEGER Counter(PLARGE_INTEGER Frequency) {
    if(Frequency)Frequency->QuadPart=CounterFrequency;
    LARGE_INTEGER Value;Value.QuadPart=CounterTicks++;return Value;
}
static void InitEvent(PRKEVENT Event,EVENT_TYPE Type,BOOLEAN Set) {
    CHECK(Event==&TestWorker->Signal && Type==SynchronizationEvent && !Set);
}
static LONG SignalEvent(PRKEVENT Event,KPRIORITY Increment,BOOLEAN Wait) {
    CHECK(Event==&TestWorker->Signal && !Increment && !Wait && Irql<=DISPATCH_LEVEL);++Signals;return 0;
}
static void InitCalls(PEX_RUNDOWN_REF Calls) {CHECK(Calls==&TestWorker->Calls);CallRefs=Closing=0;}
static BOOLEAN AcquireCalls(PEX_RUNDOWN_REF Calls) {CHECK(Calls==&TestWorker->Calls);if(Closing)return FALSE;++CallRefs;return TRUE;}
static void ReleaseCalls(PEX_RUNDOWN_REF Calls) {CHECK(Calls==&TestWorker->Calls && CallRefs);if(CallRefs)--CallRefs;}
static void DrainCalls(PEX_RUNDOWN_REF Calls) {CHECK(Calls==&TestWorker->Calls && !CallRefs);Closing=1;}
#define PsCreateSystemThread CreateThread
#define PsTerminateSystemThread Terminate
#define ZwWaitForSingleObject WaitThread
#define ZwClose CloseThread
#define KeQueryPerformanceCounter Counter
#define KeInitializeEvent InitEvent
#define KeSetEvent SignalEvent
#define ExInitializeRundownProtection InitCalls
#define ExAcquireRundownProtection AcquireCalls
#define ExReleaseRundownProtection ReleaseCalls
#define ExWaitForRundownProtectionRelease DrainCalls
#include "Worker.c"

#endif
