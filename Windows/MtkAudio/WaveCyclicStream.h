// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_WAVECYCLIC_STREAM_H
#define T70_WAVECYCLIC_STREAM_H
#include "WaveCyclicBuffer.h"
#include <stddef.h>
extern "C" {
#include "AfeConfigure.h"
#include "AfeStartRequest.h"
#include "AfeCur32.h"
#include "AfeStopRequest.h"
#include "AfePrefetch.h"
}
/* Concrete, nonaggregated, nonpaged software stream attached to an UNPUBLISHED
 * WaveCyclicBuffer. Owner retains buffer/port/context until Close and all stream
 * references end. Create takes a buffer lease and self-reference. Close is
 * mandatory at PASSIVE before final release; it drops both. A forgotten Close
 * intentionally retains the owner instead of freeing live storage.
 * State control is opt-in through real preparation/ready/idle providers. Without
 * those, RUN/ACQUIRE/PAUSE fail. A bound stream executes normal adjacent states,
 * freezes paused position and resets STOP to zero. WaveCyclicFactory binds the actual board service group and channel; standalone
 * objects are not automatically published. Notification period selection while
 * STOP updates the frame count used by the next real IRQ0 start request.
 * Buffer.Bytes remains allocation ownership. STOP->ACQUIRE reads the channel's
 * current ring size and selects a valid period within the last accepted
 * notification byte budget. Shrinking does not permanently shrink that budget.
 * Changed geometry after preparation is rejected; a detected RUN mismatch on
 * SetState attempts stock STOP then latches fault, retaining resources for Close.
 * SetBufferSize itself is metadata owned by the channel and is not intercepted:
 * concurrent/live resizing is NOT supported, nor stopped synchronously at setter
 * time. Caller serializes channel metadata changes with stream calls and must
 * change the ring only while STOP. GetPosition reports mismatch rather than a
 * position for a different ring. This is not cache/DMA lifetime validation.
 * Configure/start/stop callback completion is not a power or DMA-idle proof.
 * Close retains references through partial failure until actually removed.
 * All methods except COM refs externally serialized; no callback reentry.
 * Silence uses the alignment-safe Device Memory copy accessor, not memcpy.
 * Caller storage is resident, valid and disjoint from this object/owner. */
class T70WaveCyclicStream final:public IMiniportWaveCyclicStream {
public:
 static NTSTATUS Create(PUNKNOWN,T70_WAVECYCLIC_BUFFER *,ULONG Rate,T70WaveCyclicStream **);
 NTSTATUS Close();
 // Completed stock STOP can await callback rundown/CUR observation. Only a
 // repeated PAUSE retries that completion; RUN/position remain unavailable.
 // Explicit real providers, no defaults. PrepareFromStop establishes platform
 // admission for normal address/format/prefetch preparation; no separate CUR
 // reset register is assumed. Ready proves clock/route/cache/IRQ admission.
 // ConfirmStopped follows the stock STOP sequence and settles OS users plus
 // pause/resume position preservation. It must not invent a hardware idle bit.
 // Position preservation is a separate obligation: ALSA stock omits PAUSE.
 // Optional AbortRun seals notification submission after failed RUN admission/
 // start. Its error never replaces the original failure; StateFault and owned
 // resources remain until explicit Close. It is not hardware rollback or drain.
 // No power/reset or unproven resume behavior is fabricated. Bind before start.
 NTSTATUS BindStateControl(void *,NTSTATUS (*PrepareFromStop)(void *),
                           NTSTATUS (*Ready)(void *),NTSTATUS (*ConfirmStopped)(void *),
                           NTSTATUS (*AbortRun)(void *)=nullptr);
 // Explicit unpublished integration; use T70AfeIrqRecoveryExecute with the actual connected board IRQ owner.
 // ConfirmAccess proves stopped/exclusive writable resources. RemoveReferences
 // must later prove actual hardware references removed, even after partial error.
 NTSTATUS ConfigureDl1(const T70_AFE_FORMAT_IO &,NTSTATUS (*ConfirmAccess)(void *),
                       NTSTATUS (*RemoveReferences)(void *),void *,
                       NTSTATUS (*Execute)(void *,NTSTATUS (*)(void *),void *),void *,T70_AFE_CONFIG_RESULT *);
 // Ready additionally proves clock/route/amp, DMA visibility, IRQ/service and
 // recovery providers. Zero means register callbacks returned, NOT KSSTATE_RUN.
 // PASSIVE raw DMA observation only, using the configured channel's retained
 // allocation and real IRQ executor. Not a WaveCyclic DAC-position estimate.
 NTSTATUS ObserveDmaPosition(T70_AFE_POSITION *);
 // Bind actual <=DISPATCH read executor (T70AfeIrqRecoveryObserve) before start.
 // GetPosition uses uncompensated CUR as best available play-position estimate;
 // unknown FIFO/codec delay is NOT asserted to be zero or calibrated.
 NTSTATUS BindPositionExecutor(NTSTATUS (*)(void *,NTSTATUS (*)(void *),void *),void *);
 NTSTATUS RequestStartDl1(NTSTATUS (*Ready)(void *),void *,T70_AFE_START_RESULT *);
 STDMETHODIMP_(NTSTATUS) QueryInterface(REFIID,void **) override;
 STDMETHODIMP_(ULONG) AddRef() override;
 STDMETHODIMP_(ULONG) Release() override;
 STDMETHODIMP_(NTSTATUS) SetFormat(PKSDATAFORMAT) override;
 STDMETHODIMP_(ULONG) SetNotificationFreq(ULONG,PULONG) override;
 STDMETHODIMP_(NTSTATUS) SetState(KSSTATE) override;
 STDMETHODIMP_(NTSTATUS) GetPosition(PULONG) override;
 STDMETHODIMP_(NTSTATUS) NormalizePhysicalPosition(PLONGLONG) override;
 STDMETHODIMP_(void) Silence(void *,ULONG) override;
private:
 NTSTATUS RequestStartCore(NTSTATUS (*)(void *),void *,T70_AFE_START_RESULT *);
 NTSTATUS Transition(KSSTATE);
 NTSTATUS FinishPause();
 static void *operator new(size_t,void *P)noexcept{return P;}
 T70WaveCyclicStream(PUNKNOWN,T70_WAVECYCLIC_BUFFER *,ULONG);
 ~T70WaveCyclicStream()=default;
 volatile LONG Refs;PUNKNOWN Owner;T70_WAVECYCLIC_BUFFER *Buffer;
 ULONG Rate,Frame;bool Closed;
 uint64_t NotificationBudget; // last accepted byte budget, not resize-selected period
 T70_AFE_PREPARED HardwarePlan;T70_AFE_FORMAT_IO HardwareIo;
 NTSTATUS (*RemoveReferences)(void *);void *HardwareContext;
 NTSTATUS (*ExecuteIo)(void *,NTSTATUS (*)(void *),void *);void *ExecuteContext;
 NTSTATUS (*PositionIo)(void *,NTSTATUS (*)(void *),void *);void *PositionContext;
 bool HardwareTouched,Configured,StartAttempted,StartCompleted;
 void *StateContext;NTSTATUS (*PrepareFromStop)(void *),(*ReadyToRun)(void *),(*ConfirmStopped)(void *),(*AbortRun)(void *);
 KSSTATE State;ULONG FrozenPosition;bool StateBound,StateFault,PausePending,PauseDrained;
};
#endif
