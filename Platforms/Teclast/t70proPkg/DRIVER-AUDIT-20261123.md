# T70 Pro EDK2 · ACPI · Windows 드라이버 리뷰 결과 (2026-11-23)

- **대상 범위**: EDK2(EFI 펌웨어·ACPI) 및 Windows 드라이버·Setup 한정 (**Linux DKMS 제외**)
- **검증 경계 (`AGENTS.md`)**: 정적 소스·컴파일 산출물·단위 테스트 대조 결과이며 실기기 실행이 아니다. 모든 수정·테스트·빌드는 OrbStack `builder` 안에서만 수행하며, 실기기 조작·부팅·플래시는 금지한다.

---

## 1. Critical (즉시 수정 필수)

### [E1] `0x43F7F100..0x43F7F13F` 물리 핸드오프 주소 충돌 (CCCI vs MUSB)
- **위치**:
  - CCCI: `Silicon/MediaTek/MediaTekPkg/Include/Library/MtkCcciMemoryHandoff.h:7-8`, `Platforms/Teclast/t70proPkg/Library/MemoryMapLib/MemoryMapLib.c:840-871, 886`, `Silicium-ACPI/Platforms/Teclast/t70pro/Cellular.asl:26`, `Windows/MtkWwan/DpmaifResources.c:41-42`
  - MUSB: `Platforms/Teclast/t70proPkg/Drivers/MusbHandoffDxe/MusbHandoffDxe.c:40-41, 3095-3139`, `Silicium-ACPI/Platforms/Teclast/t70pro/DSDT.asl:245, 599`
- **원인**: CCCI 핸드오프와 MUSB 핸드오프가 모두 `0x43F7F100` (64B)을 점유. `ExitBootServices`에서 `MusbHandoffDxe`가 CCCI 레코드를 0으로 덮어쓰고, Windows에서는 `USB0`와 `DPM0`가 동일 `Memory32Fixed`를 점유해 PnP Code 12 충돌 발생.
- **수정/검증 요구**:
  - CCCI 핸드오프를 `0x43F7F080..0x43F7F147` 밖의 빈 예약 영역(예: `0x43F7F180`, 64B)으로 이동하고 `MtkCcciMemoryHandoff.h`, `Cellular.asl`(ReadOnly 권장), `DpmaifResources.c`, `verify-acpi.py`, `BootShim.S:105-117`의 부팅 시 클리어 범위를 모두 일괄 갱신.
  - `DSDT.asl` + 모든 SSDT 간 `Memory32Fixed` / 핸드오프 주소 비중복을 자동 검증하는 테스트 추가.

### [E2] `MemoryMapLib.c` DXE 동기 Data Abort(MMU 미매핑 읽기) + 캐시 플러시 누락
- **위치**: `Platforms/Teclast/t70proPkg/Library/MemoryMapLib/MemoryMapLib.c:330-339, 648-658, 807-811, 840-917`
- **원인**:
  - SEC에서 `EfiReservedMemoryType`인 CCCI 태그 블록(`Base..Base+0x10000`)을 `HobOnlyNoCacheSetting`(MMU 미매핑)으로 생성.
  - 이후 MMU가 켜진 DXE에서 정적 링크된 모듈(`PmicWrapperDxe`, `ClockDxe`, `GpioDxe`, `MsdcDxe`, `KeypadDeviceLib` 등)이 `GetMemoryMap()`을 다시 호출하면 `mMapCached == FALSE`로 `BuildRuntimeMemoryMap()`을 재실행하면서 `0x43F7F100`을 지우고(`:886`) 미매핑 물리주소 `Base`를 `CopyMem`(`:337`)해 즉시 Data Abort 발생.
  - `PublishCcciMemoryHandoff()`가 `WRITE_BACK` 매핑 영역(`0x43F7F000`)에 쓰고 `WriteBackDataCacheRange()`를 호출하지 않음(Windows `NdisMMapIoSpace`는 비캐시로 읽음).
- **수정/검증 요구**:
  - `ReadCcciTagBlockPhysical()`과 `PublishCcciMemoryHandoff()`는 SEC(`mRuntimeTraceActive` 또는 사전 MMU 단계)에서만 실행되도록 제한하거나, DXE 재호출 시 기존 예약 핸드오프를 덮어쓰거나 미매핑 물리주소를 역참조하지 않도록 차단.
  - `PublishCcciMemoryHandoff()`에 `WriteBackDataCacheRange()` 추가.
  - `RuntimeMemoryTest.c`에 DXE 재호출(미매핑 태그 블록 읽기 시 크래시 유도 목 포함) 회귀 테스트 추가.

### [W1] `Windows/MtkWwan/Driver.c`: `WwSendNetBufferLists` 확정 BSOD + `DriverEntry` NDIS 6.30 로드 실패 + 번역 IRQ 오용 + ARM64 `MmNonCached` Alignment Fault
- **위치**:
  - `Windows/MtkWwan/Driver.c:803-820` (`WwSendNetBufferLists`): `NdisMSendNetBufferListsComplete`에 `Adapter->NdisHandle` 대신 `MiniportAdapterContext` 전달(즉시 BSOD) + `SendFlags`를 `SendCompleteFlags`로 변환 없이 그대로 전달(`NDIS_TEST_SEND_AT_DISPATCH_LEVEL(SendFlags) ? NDIS_SEND_COMPLETE_FLAGS_DISPATCH_LEVEL : 0` 필요).
  - `Windows/MtkWwan/Driver.c:914-922, 520-523`: `/DNDIS630_MINIPORT=1` 빌드인데 `NDIS_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_1` 및 `GENERAL_ATTRIBUTES_REVISION_1` 사용 → `NDIS_STATUS_BAD_VERSION`으로 로드 실패 (`REVISION_2` 및 `SIZEOF_..._REVISION_2` 필요).
  - `Windows/MtkWwan/Driver.c:86-175, 552-560`: `InitParameters->AllocatedResources`(번역 리소스)를 `Raw`로 넘겨 `T70DpmaifMapResources`의 GSI `273` 검증 실패 (`NdisMGetDeviceProperty` 5번째 인자 `AllocatedResources` raw 리스트 사용 필요).
  - `Windows/MtkWwan/Driver.c:225-232, 354-396`, `DpmaifBatCore.c:91`, `DpmaifWdmDma.c:457`, `Cellular.asl:8-28`: `DPM0`에 `Name (_CCA, Zero)` 누락; `MmNonCached`(ARM64 Device-nGnRnE)에 `RtlCopyMemory`/`NdisZeroMemory`/`memset` 사용 → `DC ZVA`/비정렬 접근 Alignment Fault 위험 (`RtlCopyDeviceMemory`/`RtlZeroDeviceMemory` 또는 비캐시 일반 메모리 `MmWriteCombined` + `AllocateCommonBufferEx` 사용).
- **검증 요구**: `Driver.c`와 `DpmaifWdmDma.c`를 문자열 검사가 아닌 실제 컴파일/호출되는 NDIS/WDM 콜백 단위 테스트로 검증.

### [W2] `Windows/MtkBtif/Driver.c`: `BtifRestoreFifoInterrupts` 에러 판정 반전 + `IOCTL_BTHX_READ_HCI` 1바이트 거부 + 취소 경합 UAF/이중 완료
- **위치**:
  - `Windows/MtkBtif/Driver.c:332`: `if (!BtifWrite(Owner, 0, 4, 4, Value)) return STATUS_IO_DEVICE_ERROR;` — `BtifWrite`는 성공 시 `0`, 실패 시 `-1` 반환이므로 판정이 완전히 반전됨(첫 RX 백프레셔 복구 시 BTIF 영구 fault).
  - `Windows/MtkBtif/Driver.c:957, 1066`: `IOCTL_BTHX_READ_HCI` 입력 버퍼로 `sizeof(ULONG)`(4B) 요구 → 실제 Windows `bthmini.sys`의 1바이트 `UCHAR` 입력을 모두 거부(`OwnerTest.c:753`이 4B 배열로 가림).
  - `Windows/MtkBtif/Driver.c:918-921, 965-995, 1003-1025`: `BtifReadCanceled`가 즉시 `WdfRequestComplete()`하는데 호출 스레드가 버퍼 접근 후 `WdfRequestUnmarkCancelable()` 호출 → 동시 취소 시 UAF/`WDF_VIOLATION` 이중 완료 및 패킷 유실.
- **검증 요구**: `OwnerTest.c`에 `RxPaused` 복구, 1바이트 `IOCTL_BTHX_READ_HCI`, 취소 경합 회귀 테스트 추가.

### [W3] `Windows/MtkUsb/Driver.c`: 단일 워커 511ms~60s 정지 + Interrupt OUT 무한 재시도 + 냉간 복구 `ResumeClock=0` 고착/MMIO 누수
- **위치**:
  - `Windows/MtkUsb/Driver.c:535-544, 575-578, 594-595, 604`: `MusbHostDataTransferRouted`에 `MUSB_TRANSFER_INTERRUPT_POLL` 대신 `USB_ENDPOINT_TYPE_INTERRUPT`를 넘겨 유휴 허브/HID 폴링마다 최대 60초(표준 허브 511ms+) 동안 단일 `UsbWork` 스레드가 스핀하며 Bulk/Control을 모두 기아시킴. `Retry`(`:604`)가 `Input`을 확인하지 않아 Interrupt OUT 타임아웃이 무한 재큐잉됨. `Length == 0` ZLP도 거부됨(`:537`).
  - `Windows/MtkUsb/Driver.c:2041-2046, 2115-2119, 2186, 2230-2236, 2533-2546, 2581-2592`: 냉간 복구 성공 후 탈착/재연결(`:2234`)이나 컨트롤러 리셋(`:2186`)이 `ResumeClock = 0`을 만들면, 이후 `UsbD0Exit`/`UsbRelease`에서 `UsbStopColdResume()`가 `Live != 0`으로 항상 실패해 `UsbUnmap()`을 건너뛰고 MMIO/Target 영구 누수. 또한 `:2544`가 `UsbCloseTcpc()` 전에 반환.
- **검증 요구**: `MusbHostCore` 실제 호출(비모킹) Interrupt IN NAK/다중패킷/OUT 타임아웃/ZLP 테스트 및 냉간 복구 → 탈착/리셋 → D0Exit/Release 테스트 추가.

### [W4] `Windows/MtkBattery/Driver.c` + `BatteryCore.c`: `BatteryD0Exit` 락 없는 `Core` 변경 + 알림 누락 + `CycleCount` 오류
- **위치**:
  - `Windows/MtkBattery/Driver.c:87-147, 226-230`: 전처리 IOCTL(`BatteryPreprocessDeviceControl`)과 동시 실행될 수 있는 `BatteryD0Exit:229`가 `StateLock` 없이 `MtkBatteryCoreSuspend(&Context->Core)` 호출(데이터 경합/플래그 유실).
  - `Windows/MtkBattery/BatteryCore.c:312-324` & `Driver.c:129-146`: `PowerStateMask == 0` 무시 및 bitmask 조건 오류(`BATTERY_CRITICAL` 추가 시 알림 억제), `NotifyClass=FALSE` 쿼리가 `Device->State`를 덮어써 다음 폴링에서 전이 알림 영구 누락.
  - `Windows/MtkBattery/Driver.c:603`: 미지원 `CycleCount`에 `BATTERY_UNKNOWN_CAPACITY`(`0xFFFFFFFF`) 대신 `0` 설정 필요.

### [W5] `Windows/MtkCpu/Driver.c` + `CpuPpmCore.c` + `MtkCpu.inf`: 언로드 가능 드라이버의 비가역 `PoFxRegisterPluginEx` 등록 + 부팅 경합 + PPM 요청 거부
- **위치**:
  - `Windows/MtkCpu/Driver.c:505-522`, `MtkCpu.inf:36`: 해제 API가 없는 `PoFxRegisterPluginEx` 등록 후 KMDF 드라이버 언로드가 가능함(장치 비활성/제거 시 커널 콜백 포인터 댕글링 BSOD — 언로드 차단/이미지 핀 필요), `StartType=1`은 부팅 단계 `ACPI0007` PEP 등록보다 늦음.
  - `Windows/MtkCpu/Driver.c:321-338, 388-402`: `ACPI0007`이 `HANA6799` `CpuPrepare` 전에 등록되면 `PEP_NOTIFY_PPM_QUERY_CAPABILITIES`가 실패해 PPM 영구 비활성(테스트가 순서 고정으로 가림).
  - `Windows/MtkCpu/Driver.c:423-434`, `CpuPpmCore.c:41-52`: `DesiredPerformance == 0`, `Minimum < Lowest`, `Maximum > Highest`, `Tolerance > Desired`, 구간 내 이산 OPP 부재 등 정상 Windows PPM 요청을 클램프하지 않고 거부.

---

## 2. High (반드시 함께 수정)

### [E3] `HimaxTouchDxe.c` 비동기 `ControlResume` 성공 시 `T->Poll` 영구 정지 (이전 감사 회귀)
- **위치**: `Silicon/MediaTek/MediaTekPkg/Drivers/HimaxTouchDxe/HimaxTouchDxe.c:246-251, 309, 387, 394-396, 421-441`
- **내용**: `FinishResume(T, EFI_SUCCESS)`가 `T->Poll`을 취소하고 `ControlResume()`이 `TimerPeriodic`을 재설정하지 않아 정상 resume 후 터치 폴링이 영구 중단됨. `ControlQuiesce()`에서 `T->ResumeCancelPending`도 미초기화.

### [W6] `Windows/MtkUfsPower/Driver.c` `DeviceUsageTypeBoot` 거부 (이전 감사 회귀) + 페이징 경로 전원 IRP 할당/큐 오류
- **위치**: `Windows/MtkUfsPower/Driver.c:459-466, 498-504, 534-538, 593-616`
- **내용**:
  - `MtkUfsUsageIndex()`가 `Paging`/`Hibernation`/`DumpFile` 외(`DeviceUsageTypeBoot` 등)를 `-1`로 돌려 하위로 전달하지 않고 `STATUS_NOT_SUPPORTED`로 완료 → 미인식 usage 타입은 반드시 `Device->Lower`로 포워딩해야 함.
  - `LowerPowerPagable`이 `TRUE`로 래치된 뒤 `Total == 0`에서 하위가 비트 해제 유지 시 갱신되지 않음.
  - `PowerDeviceD0`/`D3` IRP에서 `ExAllocatePool2`/`IoAllocateWorkItem` 동적 할당과 `DelayedWorkQueue` 사용 → 사전 할당 워크아이템 + `CriticalWorkQueue`로 변경 필요.

### [E4] `Silicium-ACPI/Platforms/Teclast/t70pro/DSDT.asl`: `_LPI` 정적 `Package` 내 `NameSeg`(`LPWF`/`LPNC`) + `DISP` 모듈-레벨 `CreateDWordField`
- **위치**: `DSDT.asl:1329-1430` (`LPNC`/`LPWF`/`LPIE`/`LPIP`), `DSDT.asl:657-677` (`DISP`)
- **내용**: 컴파일된 `DSDT.aml`에 `Package`/`Buffer` 대신 원시 AML `NameSeg`가 들어가 있어 Windows `acpi.sys` AMLI가 역참조하지 못함(인라인 `Package`/`ResourceTemplate`으로 변경 필요). `DISP`의 `CreateDWordField` 10줄도 `Method (DFBI)` 내부 로컬 버퍼로 이동 필요.

### [E5] `MusbHandoffDxe.c` 유지 Type-C 호스트 EBS 인계 중단 + 부스트 누수
- **위치**: `Platforms/Teclast/t70proPkg/Drivers/MusbHandoffDxe/MusbHandoffDxe.c:2054, 2416-2425, 2639, 2655-2658, 2925-2926, 3128-3131`
- **내용**: `SetHostPowerPath()`가 `AdoptRetainedHost()`의 `RouteCcPin`을 `0`으로 덮어써 `EbsPublishNotify()`가 매번 중단됨. 또한 `AdoptRetainedHost()`가 `!OtgBound && !ColdTypecBound` 검사 없이 펌웨어가 켠 부스트를 `RetainedTypecSource`로 오분류해 해제 시 부스트/Rp 누수.

### [W7] `Windows/MtkWwan/` 코어·DMA·ACPI High 결함
- `Windows/MtkWwan/DpmaifHwCore.c:286-339`: MD1 메모리 512 MiB 미만 거부(64~256 MiB에서 실패), `RemapValue` 32 MiB 정렬 및 `[35:34]` 상위 비트 절단 미검사, 8레지스터(768 MiB) 초과 루프.
- `Windows/MtkWwan/DpmaifHwCore.c:209-211, 229-255, 535-579`: PIT/BAT init 쓰기 전 `NOT_READY` 폴링 누락, BAT 크레딧 0·큐 비활성 상태로 `InitDone` 발행.
- `Windows/MtkWwan/DpmaifDescriptor.c:282-298`: `PacketComplete == 1`일 때 `Next.HaveMessage = 0`이 `Result.HaveMessage` 복사 전에 실행되어 완료 패킷에서 항상 0, 이전 `ChannelId`/`Checksum`/`DataPath` 미초기화.
- `Windows/MtkWwan/DpmaifWdmDma.c:96-141, 249-257, 344`: `T70DpmaifWdmDmaRelease` 동시 `PutDmaAdapter` double-free, `PutScatterGatherList` IRQL(`DISPATCH_LEVEL`) 미보장, `DmaListComplete`에서 `ScatterGather == NULL` 시 콜백 누락.
- `Windows/MtkWwan/Driver.c:414-466`: `WwReleaseAdapter` 조기 반환 시 잔여 리소스·MMIO·어댑터 누수.
- `Silicium-ACPI/Platforms/Teclast/t70pro/Cellular.asl:14, 34`: `_STA = 0x07`(bit 3 미설정).

### [W8] 기타 Windows 드라이버 High 결함
- `Windows/MtkUsb/Driver.c:1594-1598, 2064-2095`: `UsbColdPhyPrepare` 실패 후 `ColdPhyBound == TRUE` 상태로 다음 틱에 미전원 PHY에 `ColdBeginHost` 재호출.
- `Windows/MtkBtif/Driver.c:423-463, 643-646`: `BtifD0Exit`/`BtifD0Entry`에서 `RxPaused`·보류 버퍼 미초기화(슬립 후 영구 정지).
- `Windows/MtkGnss/Driver.c:134-217, 355-362`: `WDFWAITLOCK`(`D->Gate`) 보유 중 `WdfRequestComplete` 호출 및 `GnD0Exit`에서 non-waiting `WdfTimerStop(D->Timer, FALSE)`.
- `Windows/MtkConn/Driver.c:325-341, 462-470`: `ConnScoInterrupt`가 전원/공급자 상실 미확인 상태로 DIRQL MMIO 읽기(ARM64 SError 위험).
- `Windows/MtkMali/PmicPrm.c:1064-1067, 1349-1372`: `MtkMaliPmicPrmReadBatteryAdcRaw`에서 `PmicBatteryCurrentBusy` 락 및 리스 해제 실패 시 격리 누락.

---

## 3. Medium (함께 정리 권장)
- `Windows/MtkUsb/MtkUsb.inf:7` (`0.2.3.0`) vs `Windows/test-storage-boot-inf.py:54` (`0.2.2.0`) 버전 불일치 + `UsbD0Failed`의 `ColdStartPending`/`ColdStartOwned` 미초기화.
- `Windows/MtkWwan/CcciCcifCore.c:62-69` (`AckMask` 상위 예약비트 누수), `CcciRing.c:107-269` (모뎀 공유메모리 volatile TOCTOU 다중 읽기), `DpmaifBatCore.c:169-272` (`Next` 슬롯 `InFlight` 시 refill 정지 및 `BufferType` 미검사), `DpmaifWdmDma.c:82, 367` (`DmaAddressWidth` 부등호 반전), `Driver.c:748-750` (비영 패킷 필터 거부).
- `Windows/MtkSd/Driver.c:702-713` (`Quiesce` DMA 대기 100 µs로 단축되어 활성 DMA 정리 시 누수).
- `Windows/MtkConn/Driver.c:472-562` (`ReportPowerRelations` 할당 실패 시 IRP 미실패 처리, 재시작 시 구 인터페이스 잔존).
- `Windows/MtkMali/PmicPrm.c:1426-1429` (`ReadBatteryTemperatureDeciC` 락 전 `Owned` 선검사).
- `Windows/Setup/StartT70Setup.cmd`: `wpeinit`을 먼저 호출한 뒤 removable-storage 검색 완료를 기다리도록 순서를 맞췄다. 재시도는 설치 매체 검색만 반복하며 중복 매체는 거부한다. `Windows/Setup/test-start-t70-setup.py` 구조 검사가 통과했다. WinPE 실행·설치는 미실행.
- `Platforms/Teclast/t70proPkg/Library/MemoryMapLib/MemoryMapLib.c:361-380, 628-671` (CCCI 페이지 반올림 전 경계 검사 및 중복 `ccci,modem_info*` 노드 미거부).
- `Platforms/Teclast/t70proPkg/Drivers/MusbHandoffDxe/MusbHandoffDxe.c:2925-2926` (`ReadyInstalled` 상태에서 `SetHostPowerPath` 호출 시 프로토콜/내부상태 괴리).
