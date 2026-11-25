# T70Pro EFI / Windows 드라이버 정적 감사 및 재검수 결과 (2026-11-22)

> **검증 범위 구분 (`AGENTS.md` 준수)**
> - 본 문서는 **정적 소스 코드 감사** 및 **Codex가 수행한 호스트 모의(fake-MMIO) 단위 테스트·빌드 결과**를 기준으로 정리했습니다.
> - **실기기(MT6789 태블릿) 부팅/하드웨어 동작 검증은 미실행** 상태입니다.

---

## 1. 최초 감사에서 확인한 네 건 — 수정 및 호스트 검증 완료

### 2026-11-22 수정 결과

| # | 수정 내용 | 검사 결과 |
|---|---|---|
| 1 | MUSB core interrupt IN/OUT은 MPS 단위로 반복하고, short packet에서 멈추며, INTERRUPT_POLL은 패킷 하나만 처리한다. Windows는 URB 길이와 endpoint service interval로 제한 시간 예산을 산출한다. | 생산 MUSB core 호스트 검사 7,313개(ASan/UBSan), Windows MtkUsb ARM64 콜백 검사 통과, unsigned ARM64 SYS 84,480바이트 링크. 코어 직접 검사와 UCX 콜백 모의 검사는 각각 분리되어 있다. |
| 2 | Himax 비동기 복귀 종료 시 타이머를 취소하고, 취소 요청이 실패하면 callback에서 재시도한다. 남은 GPIO152 lease는 Board Ready 확인 전에 반환을 재시도한다. | HimaxDxeTest 13,002개(ASan/UBSan) 통과. lease 반환 실패·재시도, Ready 호출 순서, timer 취소 실패 후 재시도 포함. |
| 3 | RTC 쓰기 격리 상태 검사를 TPL_NOTIFY 임계 구역 안으로 옮기고 mRtcReading 경합을 먼저 판정한다. 쓰기 중 TPL_CALLBACK GetTime은 EFI_NOT_READY를 반환한다. | Boot 1,115,477개, Set 1,351,700개, Runtime 21,688개 호스트 검사 통과. PMIC 접근은 가짜 transport만 사용했다. |
| 4 | UFS 전원 필터가 페이징·최대절전·덤프 사용 알림의 경로 수를 추적하고, 하위 드라이버 완료 순서에 맞춰 DO_POWER_PAGABLE을 설정·해제한다. 마지막 경로 제거 전 설정하며 실패하면 이전 플래그를 복구한다. | 실제 ARM64 WDM 필터 콜백 호스트 검사 1,166개 통과, unsigned ARM64 MtkUfsPower.sys 15,872바이트 링크. I/O 관리자·Storport·PRM은 테스트에서 대역했다. |

EFI/MediaTek 생산 C 호스트 검사 57개도 ASan/UBSan으로 모두 통과했다. 전체 T70Pro firmware-trial ARM64 빌드는 Build/driver-audit-fix-20261122/RELEASE_CLANGPDB에서 완료됐다. 이 검증에는 태블릿 실행이 포함되지 않는다.

아래 세부 내용은 각 결함의 최초 근거와 당시 수정 요건을 보존한다.

### [1] [High — 미구현/모킹 누락] `Windows/MtkUsb` + `MusbHostCore.c` 멀티패킷 인터럽트 미지원
- **위치**
  - `Windows/MtkUsb/Driver.c:516-519`
  - `Silicon/MediaTek/MediaTekPkg/Library/MusbHostCoreLib/MusbHostCore.c:1556-1559, 1625-1660, 1716-1746`
  - `Windows/MtkUsb/DriverTest.c:1625-1646` (모킹으로 누락 은폐)
- **원인**
  - `Windows/MtkUsb/Driver.c`에서 `E->Type == USB_ENDPOINT_TYPE_INTERRUPT && Length > E->MaximumPacket` 거절 조건만 제거했으나, 실제 전송 엔진인 `MusbHostCore.c`는 수정되지 않았습니다.
  - **Interrupt OUT**: `MusbHostCore.c:1556-1559`가 여전히 `(!Receive && (DataLength > MaximumPacketLength))`일 때 `MusbInvalid`를 반환하고 멀티패킷 송신 루프가 없습니다.
  - **Interrupt IN**: `MusbHostCore.c:1716-1746`이 단일 패킷 1회만 수신하고 루프가 없습니다(Bulk IN `:1670-1715`와 달리 반복 수신 없음). `Length > MaximumPacketLength` URB에 대해 `USBD_SHORT_TRANSFER_OK`가 없으면 `USBD_STATUS_ERROR_SHORT_TRANSFER` 실패, 있으면 첫 패킷 뒤 조용히 잘려 후속 패킷이 장치에 남아 HID 리포트가 어긋납니다.
  - `Windows/MtkUsb/DriverTest.c`가 `MusbHostDataTransferRouted`를 모킹해 `UsbTestCoreDone = 16`을 반환했기 때문에 테스트가 통과한 것처럼 보였습니다.
- **수정 요건**
  - `MusbHostCore.c`의 `MUSB_TRANSFER_INTERRUPT` OUT/IN 경로가 `MaximumPacketLength` 단위로 멀티패킷 루프(short packet 종료 포함, `SinglePoll` 의미 유지)를 처리하도록 구현하고, `MusbHostCoreTest.c`에 실제 코어 멀티패킷 인터럽트 OUT/IN 회귀 테스트를 추가할 것.

---

### [2] [High — 새 회귀] `HimaxTouchDxe.c` 비동기 `ControlResume` 데드락 및 실패 시 타이머 미취소
- **위치**
  - `Silicon/MediaTek/MediaTekPkg/Drivers/HimaxTouchDxe/HimaxTouchDxe.c:242, 246-260, 278-295, 387-415`
  - 참조: `Platforms/Teclast/t70proPkg/Drivers/GpioPlatformDxe/GpioPlatformDxe.c:1316-1352` (`TouchReady()` → `TouchGpioReady()`)
- **원인**
  1. **복구 데드락**: 비동기 `ControlResume` 수행 중 `Poll()`에서 `ReleasePinLease(T)`가 실패(278행)하고 이후 재시도(248행)도 실패해 `++T->ResumeAttempts == 5`(286행)에 도달하면, `T->PinLeaseHeld == TRUE`가 남은 채 `ResumeRunning = FALSE; ResumeFinished = TRUE;`로 종료됩니다(291–293행).
     - `ControlResume`에서 기존 `if (T->PinLeaseHeld) { ReleasePinLease(T); ... }` 가드가 삭제되었기 때문에, 다음 `ControlResume` 재시도 시 `T->Board->Ready()`(407행)가 GPIO152 임시 리스 획득에 실패해 `EFI_NOT_READY`를 반환하고, `Poll()`은 `T->Stopped && !T->ResumeRunning`(242행)으로 즉시 반환해 영구 데드락에 빠집니다.
  2. **타이머 누수(미취소)**: `ControlResume` 409행에서 `gBS->SetTimer(T->Poll, TimerPeriodic, 100000)`으로 10ms 타이머를 켠 뒤, `Poll()`(257–260행, 291–294행) 또는 `ControlResume()`(394–396행)에서 resume이 실패해도 `SetTimer(T->Poll, TimerCancel, 0)`를 호출하지 않아 quiesce 상태에서 10ms `TPL_CALLBACK` 타이머가 계속 실행됩니다.
- **수정 요건**
  - `ControlResume()`에서 `T->Board->Ready()` 호출 전에 `T->PinLeaseHeld` 해제 재시도를 복구하고, 실패 시 `EFI_NOT_READY` 반환 전/실패 완료 시 `SetTimer(T->Poll, TimerCancel, 0)`로 타이머를 반드시 취소할 것.
  - `HimaxDxeTest.c`에 `ReleasePinLease` 실패 후 `ControlResume` 재시도 복구 및 실패 시 타이머 취소 회귀 테스트 추가.

---

### [3] [Medium — 새 회귀] `StockRtcRead.h` 동시 `GetTime()` 호출 시 오탐 `EFI_DEVICE_ERROR`
- **위치**
  - `Platforms/Teclast/t70proPkg/Library/UnavailableRealTimeClockLib/StockRtcRead.h:90-125`
  - `Silicon/MediaTek/MediaTekPkg/Include/Library/MtkRtcCalendar.h:90-105`
- **원인**
  - `StockRtcAccess()`에서 `TPL_NOTIFY` 보유를 풀고 `TPL_APPLICATION`에서 `MtkRtcWriteCalendar(&Io, Values, &mRtcWriteFault)`(102행)를 실행하도록 바꾸면서, `MtkRtcWriteCalendar`가 쓰기 시작 전에 선제적으로 `mRtcWriteFault = TRUE`를 설정하고 완료 시 `FALSE`로 복원하는 동안 `TPL_CALLBACK` 타이머가 선점할 수 있게 되었습니다.
  - `StockRtcGetTime()`(113행)과 `StockRtcSetTime()`(123행)이 `StockRtcAccess()`의 `mRtcReading` 검사(94행)보다 **먼저** `if (mRtcWriteFault) return EFI_DEVICE_ERROR;`를 검사하므로, 정상적인 `SetTime()` 도중 선점한 `GetTime()`이 `EFI_NOT_READY` 대신 `EFI_DEVICE_ERROR`를 잘못 반환합니다.
- **수정 요건**
  - `StockRtcAccess()`에서 로컬 `BOOLEAN Fault = FALSE;`를 넘긴 뒤 종료 시 `TPL_NOTIFY` 하에 `if (Fault) mRtcWriteFault = TRUE;`로 커밋하거나, `mRtcWriteFault` 검사를 `TPL_NOTIFY` 하 `mRtcReading` 검사 뒤로 이동할 것.
  - `SetRtcTest.c` / `BootRtcTest.c`에 `SetTime()` 도중 `TPL_CALLBACK` `GetTime()` 선점 시 `EFI_NOT_READY` 반환 검증 추가.

---

### [4] [Medium — 부분 수정] `Windows/MtkUfsPower/Driver.c` `DO_POWER_PAGABLE` vs `IRP_MN_DEVICE_USAGE_NOTIFICATION`
- **위치**
  - `Windows/MtkUfsPower/Driver.c:456-527, 544-546`
- **원인**
  - `AddDevice`에서 `DO_POWER_INRUSH`와의 상호 배타 충돌 및 무조건 `DO_POWER_PAGABLE` OR은 제거되었으나, 546행에서 `Device->Lower->Flags & DO_POWER_PAGABLE`을 복사한 뒤 `MtkUfsPowerDispatch`가 `IRP_MN_DEVICE_USAGE_NOTIFICATION`을 처리하지 않습니다.
  - StorPort FDO가 초기에 `DO_POWER_PAGABLE`을 켰다가 LU2 페이징/최대절전/크래시덤프 경로 진입 시(`DeviceUsageTypePaging`, `InPath == TRUE`) `DO_POWER_PAGABLE`을 끄면, 상위 필터에만 `DO_POWER_PAGABLE`이 남아 WDM 전원 규칙 위반이 발생합니다.
- **수정 요건**
  - 이 필터는 이미 비페이징(`DISPATCH_LEVEL` 안전)이므로 `AddDevice`에서 `DO_POWER_PAGABLE`을 아예 복사/설정하지 않거나, `IRP_MN_DEVICE_USAGE_NOTIFICATION`에서 하위 완료 후 `Device->Lower->Flags & DO_POWER_PAGABLE`과 동기화할 것.

---

## 2. 수정 완료 확인된 항목 요약 (정적 코드 재검수 통과)

### A. EFI USB 경로 9건 (2026-11-21 1차 감사 → 수정 확인)
1. **[High]** `MtkHusb311TypecCore.c`: 빈 USB-C 포트 100~1,000ms 동기 busy-wait 제거 → `PollOpenAndQuiet` + `MtkHusb311TypecStepCleanup` 5ms 논블로킹 폴링 전환.
2. **[High]** `UsbMassBot.c` / `UsbMassImpl.c`: `MediaProbe` 40ms 일괄 상한 제거, `UsbBotGetMaxLun` STALL 외 에러 전파 및 반환값 검사.
3. **[High]** `MusbHandoffDxe.c`: `WriteHusb311Sense(..., Restore=TRUE)`로 `DeviceSettled == FALSE` 복원 데드락 해소.
4. **[High]** `MusbHostDxe.c`: `ExitBootServices` 내 `CloseEvent()` 제거, `SetTimer(..., TimerCancel, 0U)`만 유지.
5. **[High]** `MusbHandoffDxe.c` / `MtkMusbPlatformCore.c`: `BeforeExitNotify()`에서 미완성/전환 중 호스트 컨트롤러·PHY·클록·OTG 5V·GPIO48/86·HUSB311 Rp/RX·감지 설정 일괄 정리.
6. **[Medium]** `MusbHostCore.c`: `WaitTx()` `(TXCSR_H_WZC_BITS & ~TXCSR_H_NAKTIMEOUT) | TXCSR_TXPKTRDY` 수정.
7. **[Medium]** `MusbHandoffDxe.c`: `ConnectController()` 반환값 + `ReadyBusBound()`(`UsbBusDxe` `gEfiCallerIdGuid`) 검증 추가.
8. **[Medium]** `MusbHandoffDxe.c`: `SettleInheritedDevice()` `ValidateGcd`, `SettlePassiveDevice()`, 클록 OFF 시 `DisableSettledVconnOnLease()`, Pogo 전환 시 `TypecReleasing` 처리 추가.
9. **[Medium]** `UsbEnumer.c` / `T70UsbStorageDiagnostics.c`: `UsbQueuePortWork()` 기존 이벤트 해제, 진단 링 슬롯 재사용 + `Sequence` 검증으로 `EFI_NOT_READY`/`EFI_TIMEOUT`만 재시도.

*(저우선 참고: `MusbHostDxe.c:734` `DataBuffersNumber != 1U` 검사의 High-Speed 한정, `MtkMusbPlatformCore.c` `SWRST | 3` 이후 `SESSION`/`POWER_HOST_SET` 유지 가정의 실기 확인 필요.)*

### B. USB 외 EFI / Windows 드라이버 10개 묶음 (2026-11-21 2차 감사 → 수정 확인)
- **[1a–c] `UnavailableRealTimeClockLib.c` / `StockRtcRead.h`**: `gFdtTableGuid` 이벤트 지연 초기화, `gEfiEventBeforeExitBootServicesGuid` 훅(recovery shell 직접 부팅 대응), 비-NULL `Capabilities` 지원. *(단, [1d] 동시 `GetTime` 회귀는 섹션 1-[3] 참조)*
- **[2] `Windows/MtkSd/Driver.c`**: `FAST_MUTEX` → `KMUTEX` 교체 + `AcpiResult()` 비-threaded `IoAllocateIrp` + `STATUS_MORE_PROCESSING_REQUIRED`로 `APC_LEVEL` 데드락 제거.
- **[3a,b,d,e] `Windows/MtkUfsPower/Driver.c`**: `IRP_MN_REMOVE_DEVICE` dispatch 동기 detach/delete, `MtkUfsMiniportCall` 비-threaded `IoAllocateIrp`로 `IoCancelIrp` UAF 제거, `IRP_MN_SURPRISE_REMOVAL` 후속 Power/PnP 포워딩, `MtkUfsPowerWorker` `IoCompleteRequest` 후 `RemoveLock` 해제. *(단, [3c] `DO_POWER_PAGABLE`은 섹션 1-[4] 참조)*
- **[4] `Windows/MtkUfs/Driver.c`**: `UfsStartTransport` `IE(0x24)` 쓰기/읽기백 `InterruptLock` 보호, `UfsPowerService` 복귀 LU2 `READ CAPACITY(16)` `UNIT ATTENTION`/`NOT READY` 재시도 추가.
- **[5] `I2cPlatformDxe.c` / `I2cDxe.c` / `MtkI2cCore.c`**: `ExportToAcpi` `TPL_CALLBACK` 이동 + `MTK_I2C_DRIVER_HANDOFF` 확인, `I2cDxe::ExitBootServices` `Worker`/`Busy`/리스 정리 후 `Observer` 호출, `Claim(C, TRUE)` 내 `Configured`/`Ready` 원자 검사 + `This == NULL` 검사, `BindingBusy` `TPL_NOTIFY` 보호 및 `Observe` 후 타이머 arm.
- **[6] `ClockDxe.c` / `ClockImplLib.c`**: `TOP_MMPLL_D5_D2` `.Div = 10` 수정, `ClockSetFrequency` `CON1_PCW_CHG(BIT31)` 래치 및 `PcwMask` 적용, `PllCalcValues` 하한·오버플로·0나눗셈 방어, `TPL_NOTIFY` 락, RELEASE 빌드 OOB `return`, `ClockGetFrequency` `PcwShift` 적용.
- **[7] `MtkUfsTransport.c`**: PRDT DW3 Dword 정렬(`((Bytes + 3U) & ~3U) - 1U`) 및 제로잉 정렬.
- **[8] `MsdcDxe.c` / `MsdcImplLib.c`**: `MsdcSetMclk` `CCKPD` 마스킹·12비트 `Div` 검증·실패 롤백, `PowerControl` OFF 시 `ldo_vmc`→`Vmch` 순서 및 라이브 레일 전압 변경 방지, `ClockControl` 자식→부모 순서 및 롤백, `MSDC_INTEN = 0`, `MsdcResetDevice` `TPL_CALLBACK`, `SDC_BLK_NUM` idle 후 기록 + R1b `Bc` 가드 + R2 byte15=0, `MsdcPioRead` async FIFO drain 대기.
- **[9b,c] `HimaxTouchDxe.c` / `HimaxCore.c`**: `WaitForInput` `EVT_NOTIFY_WAIT` 단일 시그널, `TPL_NOTIFY` 동기화 일원화, 프로토콜 설치 후 `Poll` 타이머 arm, `This == NULL` 검사, `HmWaitBroadcastLine` off-by-one 수정. *(단, [9a] 비동기 `ControlResume` 회귀는 섹션 1-[2] 참조)*
- **[10] `KeypadDxe.c` / `KeypadTextIn.c` / `KeypadController.c`**: `KeypadReturnApiPushEfikeyBufTail` `break;` 추가, `KeypadSetState` 독립 `if`, `KeyNotifyProcessEvent`·프로토콜 설치 후 타이머 arm, `TPL_CALLBACK` 폴링, `KeypadWaitForKeyEx` partial key 보존, `KeypadReadKeyStroke` `Key == NULL` 검사.
- **[11b] `Windows/MtkGpio/Driver.c`**: `KeInitializeMutex(&D->ConfigurationLock, 0)`을 `D->Interfaces` 할당·`SurpriseRemoved` 조기 반환 이전으로 이동. *(단, [11a] `MtkUsb` `WdfExecutionLevelPassive`·0-len control 통과, 멀티패킷 인터럽트는 섹션 1-[1] 참조)*
- **[12] `MtkPmicBroker.c`**: `ReleaseConnRails` `!Core->ConnLeaseToken` 검사 + `AcquireConnRails` `ConnReleasedToken = 0` 클리어.
- **[13] `MtkUfsHandoffDxe.c`**: 비정렬 `BusMasterWrite` 및 `!Supports64BitDma` ≥4GiB 바운스 버퍼 적용. *(저우선 참고: 임시 바운스 버퍼 `EfiRuntimeServicesData` 대신 `EfiBootServicesData` 권장)*
- **[14] `DeviceBootManager.c` / `MT6358PmicDxe.c`**: `TPL_CALLBACK` 재귀 `ConnectController` 차단 + `!mInputServicesEnded` 시에만 `FreePool(Handles)`, `vmch_eint_*` disable 극성 검사 및 `0x1CD8`→`0x1D70` 롤백 순서 교정.

---

## 3. 감사 결과 심각 결함 미발견 드라이버 목록
- **EFI**: `DsiPlatformDxe`, `FramebufferGopDxe`, `FramebufferCoreLib`, `BootStatusDxe`, `MtkI2cCoreLib`, `GpioPlatformDxe`, `GpioDxe`, `GpioImplLib`, `PmicWrapperDxe`, `PmicWrapperImplLib`, `StockPowerReadOnlyDxe`, `StockPowerCoreLib`(Gauge/Thermal), `MtkUfsColdHost`, `MtkUfsReadOnlyBlockIoDxe`, `MtkUfsPrm`, `MtkUfsCore`, `MtkUfsPlatform`, `MtkUfsPowerCore`.
- **Windows**: `HimaxTouch`, `MtkI2c`, `MtkAudio`, `MtkCpu`, `MtkThermal`, `MtkUfsPower/PrmClient.c`, `MtkGpio/Hardware.c`.
