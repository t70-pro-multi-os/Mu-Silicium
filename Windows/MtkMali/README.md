# Mali-G57 실행 제어와 Windows GPU 지원 코드

`MaliJm.c`는 실제 Mali JM/MMU 레지스터를 사용하는 공용 코어다. 순정 T70Pro 모듈의 AArch64-4K 주소 변환 형식을 구현한다. 원본 모듈·DTB 해시와 함수 위치는 프로젝트의 `analysis/mali-stock-evidence.json`에 있다.

Linux Mesa의 순정 kbase 연결도 원본 소스에서 수정했다. 작업 형식 확인 실패 시 장치 생성을 중단하고, 새 컨텍스트·완료 코드·작업 번호·두 사용자 데이터 값을 확인한다. 실제 포장/판별 코드는 ASan/UBSan·ARM64 모의 실행 각각 10,007개 검사, 순정 ARM64 입력 512개·필드 읽기 8,704개·완료 기록 1,024개 대조를 통과했다. 전체 Linux 백엔드 파일의 ARM64 객체 컴파일도 통과했다. 실행용 메모리의 일반 영역 반환 처리와 실패한 매핑의 임시 핸들 정리도 수정했고, 실제 할당/매핑/해제/가져오기 코드의 ARM64 Linux 모의 실행 364개·순정 핸들 정리 128가지 검사를 통과했다. 수정한 현재 소스로 ARM64 Linux용 PanVK 공유 라이브러리를 다시 빌드·링크했다. 객체 956개·라이브러리 62개, Vulkan 로더 진입점 3개와 실제 ELF를 확인했다. 누락된 Wayland 보조 소스·헤더를 복원하고 화면 생성·프로토콜 7종을 연결했으며 XML 원본과 실제 C 데이터의 실행 검사 1,006개를 통과했다. 기존 생성물 128개와 builder의 Linux 라이브러리를 재사용했으며 실제 Vulkan 실행·화면 출력은 남아 있다. 자세한 범위는 [PanvkBuild.md](PanvkBuild.md)와 `analysis/panvk-kbase-abi-verification.json`에 기록했다.

## 구현 범위

| 소스 | 구현한 동작 |
| --- | --- |
| `MaliJm.c` | 4단계 48비트 GPU 주소·40비트 물리 주소·4KiB 페이지, 읽기 전용/NX/캐시 속성, GPU 내부 전원 준비·리셋·AS/TLB·JM 제출과 IRQ 상태 판정. 오류 뒤 재사용 차단과 RESET_DONE 이후 중단 처리. |
| `Wddm.c` | 실제 WDK MMU 기능과 CPU 방식 페이지 표 갱신. 장치가 보관하는 변환/인코딩 공간에서 전체 입력을 검사한 뒤 표를 쓰고 캐시를 처리한다. |
| `Memory.c` | MDL 할당, 비캐시/실행 금지 매핑, 물리 주소 제한·캐시 처리, 작업별 참조 고정, 제출 실패 시 새 참조만 되돌리기, 완료/실제 리셋 후 해제. |
| `Execution.c` | 컨텍스트 64개, 대기 32개, 제출당 최대 16단계 JM 작업을 한 논리 노드에서 순서대로 실행. 오래된 컨텍스트 ID 거부, 루트/참조 수명, 32비트 fence 순환, DMA 작업 경계에서 선점, 시간 초과·오류 후 보존, 유휴 GPU/MMU 오류 소비. |
| `Ddi.c` | 실제 WDK 형식의 27개 호출: 프로세스·장치·컨텍스트 생성/해제, 루트 지정/크기, 가상 주소 제출, 선점, 완료 fence 조회, 엔진 리셋, 페이지 표 갱신/TLB 무효화, ISR·DPC 통지, 할당 생성/해제·열기/닫기·형식 조회·공유 메모리 주소 수신·기능 협상, 어댑터/MMU/메모리 영역·노드·엔진 조회와 참조 수명이 있는 기능 인터페이스. 커널 뮤텍스와 단일 원자 상태로 일반 호출과 DISPATCH 선점 요청을 연결한다. |
| `Umd.c`·`Umd.h` | 실제 D3D10 자원·전송·RTV·clear·Flush 22개와 셰이더 수명/선택 6개 콜백, 별도 KMT 장치의 보안 공유 할당·GPU 주소·메모리 상주·제출·완료 신호와 지연 해제. 업로드한 셰이더와 소유 자원을 정점/fragment 두 단계 명령 제출에 연결. 등록된 D3D 장치나 화면 표시 기능은 아님. |
| `Shader/ShaderCompiler.c` | 완전한 DXBC의 정점·픽셀 셰이더를 vkd3d-shader/SPIR-V/NIR을 거쳐 실제 G57 Valhall 코드로 변환. FAU·자원·varying·IDVS 정보 반환. 호스트 셰이더 20종, DXBC 연결 8종과 DDI 연결 10종, 각 32회 동시 컴파일. packed 성분 이동·공통 배치·noperspective 마스크 반환, Windows ARM64 DLL 생성·검사와 비트 배치 300개 실행. DDI 토큰 복원·좌표 보존·자료형별 컴파일과 셰이더 객체 연결 구현. 정점/fragment 명령 제출 API와 기존 픽셀 preload를 쓰는 제한된 반복 Draw에 연결했으며 일반 그리기·Present는 남음. [빌드·범위](Shader/README.md). |
| `Allocation.c` | 어댑터가 보관하는 자원/할당과 장치·프로세스별 열기 정보 분리, OS 핸들 조회와 읽기 전용 권한, 범위 검사, 실행 참조 수명, 여러 할당의 일괄 실패 정리, 기존 자원 확장. OS가 지원한 공유 메모리의 CPU 주소를 받아 범위 안의 내용을 복사한다. |
| `Vm.c` | OS가 갱신한 페이지 표의 소유 프로세스·물리 주소·할당 고유 번호를 추적. 처음 갱신·무효화·퇴거·주소 재사용을 처리하고 준비 자원이 남은 동안 매핑 변경을 차단한다. |
| `Residency.c` | 공유 backing에 새 MDL을 만들어 실제 페이지를 고정하고 GPU 주소의 PFN과 대조. 명령 사본과 검증한 쓰기 대상만 별도 4단계 실행 표에 매핑하고 완료·리셋 뒤 해제한다. |
| `Commands.c` | G57 arch-9 값 쓰기·캐시·compute·fragment·malloc-vertex·tiler·fullscreen의 형식/주소/연결 검사. 명시적 자원 목록으로 malloc-vertex/fragment 512바이트를 생성한다. 최대 64개 작업의 전체 길이를 보존하고 128바이트 경계의 명령 사본에서 순서·의존 관계를 다시 구성한다. |
| `Prepare.c` | 원본 DMA/개인 데이터 복사, 명령 검사와 자원 참조 고정 요청, 캐시 처리 후 제출, 최대 33개 준비 자원 유지와 오류 시 정리. 실제 DDI와 `Residency.c`의 공유 backing 메모리 관리에 연결한다. |
| `Worker.c` | 실제 시스템 스레드·동기화 이벤트·QPC 시계로 제출과 완료 확인을 처리한다. 일이 없으면 시간 제한 없이 잠들고, 처리 중이거나 통지를 재시도할 때만 50ms마다 확인한다. 스레드 생성 실패 정리, 종료 대기와 동시 Wake 호출 정리를 구현한다. |
| `Mfg.c` | MT6789 전원 도메인 4개·SRAM 대기·MFG1 버스 보호, mux/BG3D 클록과 레일·기준 클록을 연결한 전원 시작/종료·참조 수명·버스 유휴 대기, 실패 후 자원 보존. |
| `Miniport.c` | WDDM 3.1 DriverEntry 등록 호출·장치 추가/시작/중지/제거, GPU 자원 검사·PnP 보드/별도 전원 인터페이스 참조·MMIO 매핑·코어/실행 묶음과 객체 보존 D0/D3 연결, 실패 후 자원 보존·재시도. 실제 보드 제공자는 별도 필요. |
| `Board.c` | 기존 보드/별도 전원 인터페이스를 실제 MFG 제어기에 연결. 자원/접근 사용권·독립 참조·고유 사용권 번호·실패 후 보존·공유 하드웨어 격리 통지·철회 후 정리 수명 구현. 같은 PDO의 IRQ 대조·조회와 실제 WDM START/STOP/취소/제거 요청 처리 구현. 중지 취소·재시작, 강제 제거 후 MMIO 차단과 사용권 보존. 필터 부착·호출 종료는 Host.c에 연결. 공유 자원 제공자·캐시 처리는 Soc.c에 연결. 실제 기기 조건 확인은 별도 필요. |
| `Soc.c`·`Soc.h` | OS가 할당한 공유 레지스터 5영역의 매핑, SPM/클록 호출 전체의 잠금과 공용 PRM PMIC 연결, 실제 보드 팩토리와 참조, 캐시 처리·장치 전원·제거 수명. 필수 기기 조건 입력이 없으면 전원 확보 거부. |
| `PmicPrm.c`·`PmicPrm.h` | 고정 GUID의 공용 UFS/GPU PMIC 거래, 요청·응답 전체 검증, Begin부터 Ack까지 모듈 고정. 처리 여부 불명·확정 실패 뒤에는 고정과 실패 상태를 OS 재시작까지 보존하고 추가 호출 차단. |
| `Host.c`·`HostEntry.c` | 실제 WDM 하위 필터의 DriverEntry·AddDevice·장치 부착·요청 전달, 완료까지 유지하는 제거 잠금, 인터페이스별 장치 참조, 제거 완료 후 남은 참조/사용권 보존과 지연 정리. |
| `Host.h`·`Interfaces.c` | 실제 부모 자원 제공자 조회 형식과 보드/전원 GUID. 부모는 같은 PDO의 소유권·주소·캐시·기기 보정 OPP를 확인해야 함. |
| `MtkMaliSoc.inf` | ACPI\HANA678F의 System INF. GPU 필터와 같은 MtkMaliBoard 서비스·SYS로 공유 자원 장치를 관리. |
| `MtkMaliBoard.inf` | ACPI\HANA678E 전용 ARM64 Extension INF. 기능 드라이버를 대체하지 않는 하위 필터 서비스 지정. |
| `Runtime.c` | 시작/종료와 객체 보존 절전·복귀를 연결. 절전은 OS 객체·VM·기능 참조를 유지하고 제출·IRQ·스레드·코어를 중단한다. 복귀는 장치 정보·스레드·IRQ 복구 후 제출을 허용하며 부분 실패를 보존한다. |

전체 입력을 검사하기 위한 페이지 표 임시 배열을 커널 스택에서 제거했다. `MaliJmWritePtes`는 호출자가 보관하는 별도 작업 공간을 받으며 입력/대상과 겹치면 거부한다. 이 함수의 Windows ARM64 스택 프레임은 4,144바이트에서 16바이트로 줄었다. 빌드는 제품 함수마다 고정 2KiB 이하인지 검사한다. 이는 OS 코드를 포함한 전체 호출 사슬의 스택 검증과 구분한다.

## 미니포트 시작·중지와 보드 연결

`Miniport.c`의 DriverEntry는 WDDM 3.1 형식의 콜백 표를 만들고 실제 `DxgkInitialize`를 호출한다. AddDevice는 비페이지 메모리에 `MALI_MINIPORT`를 확보한다. 첫 멤버가 Runtime/실제 DDI 어댑터이므로 OS가 받은 장치 주소를 기존 조회·할당·제출 DDI의 hAdapter로 그대로 쓴다. 화면 출력 전용 MtkDisplay와는 별도 장치다.

별도 산출물 `Windows/Build/MtkMaliMiniport.sys`도 실제 WDK ARM64 라이브러리로 연결했다.
96,256바이트의 미서명 SYS이며 `GsDriverEntry`에서 Miniport의 `DriverEntry`,
WDK `displib.lib`의 정적 `DxgkInitialize`로 이어지는 실제 ARM64 호출을 확인한다.
함수 범위는 `.pdata`/`.xdata`의 길이로 한정하고 PE 체크섬·WDM 표시와 보드 필터 진입점 제외를 검사한다.
이 SYS의 그래픽 INF·서명·Windows 등록과 로드는 아직 없다.

StartDevice는 OS 콜백과 PDO, GPU MMIO `0x13000000/0x4000`, 실제 변환 IRQ 3개를 검사한다. 잘못된 IRQ는 공용 검사 함수에서 매핑·전원 확보·MMIO 접근 전에 거부한다. PnP `IRP_MN_QUERY_INTERFACE`로 같은 GPU 스택의 `MALI_BOARD_INTERFACE` 참조를 받고, `DxgkCbMapMemory`로 GPU를 매핑한 뒤 보드 사용권을 얻는다. 직접 물리 주소·캐시 처리·안전한 메모리 예산과 보드 준비가 확인돼야 실제 코어와 Runtime을 시작한다. 화면 소스와 자식 장치는 각각 0개다.

StopDevice는 살아 있는 OS 객체·작업·인터페이스 참조가 있으면 종료를 거부한다. 종료 가능하면 IRQ 마스크/연결 해제와 Worker 정리, ISR/DPC 호출 종료 대기, 코어 중지 또는 실패한 시작의 실제 리셋, 보드 사용권 해제, MMIO 해제, 인터페이스 참조 해제를 순서대로 수행한다. 어느 단계든 실패하면 남은 자원을 유지하고 다음 Stop에서 이어간다. RemoveDevice는 남은 단계나 호출이 있으면 객체를 해제하지 않는다.

D0와 저전력 콜백은 아래 객체 보존 Runtime 경로에 연결했다. 보드가 제공하는 저전력 경로 하나를 D1·D2·D3에서 공통으로 사용한다. 기존 보드 인터페이스를 유지하고 별도 GUID의 `MALI_BOARD_POWER_INTERFACE`를 같은 스택에 질의해 독립 참조를 얻는다. 이 선택 인터페이스가 없으면 D3는 MMIO 접근 전에 거부한다. D3는 작업 유휴·IRQ 해제·스레드 종료·코어 정지를 확인하고 MMIO 콜백을 닫아 끝까지 기다린 뒤 보드에 전원 전환을 요청한다. 보드 호출이 실패하면 전원 상태를 알 수 없는 것으로 보존하고, 명시적인 D0 성공과 Ready 확인 전에는 GPU 레지스터 읽기·쓰기를 차단한다.

복귀는 D0 보드 준비·원래 장치 정보·작업 스레드·IRQ를 확인한 뒤 제출을 다시 허용한다. D0에서는 Action 값에 의존하지 않는다. 이미 대기 중이던 DPC는 D3에서도 GPU에 접근하지 않고 OS 통지를 마친다. 전원 차단 중 Stop은 살아 있는 OS 객체가 있으면 보존하고, 정리 가능하면 GPU 리셋/MMIO 없이 보드 사용권·매핑·전원/기존 인터페이스 참조를 순서대로 반납한다. 실패한 해제도 재시도할 자원을 유지한다.

[Microsoft의 SetPowerState 문서](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dispmprt/nc-dispmprt-dxgkddi_set_power_state)는 정상 OS 전원 요청이 실패하지 않도록 요구한다. 현재 모의 실행용 연결 코드는 전환 상태가 입증되지 않으면 오류와 자원을 보존한다. 실제 보드 제공자·OS 전환 순서·장치 고장 처리 정책으로 이 요구를 충족하는 검증은 남아 있으며, 설치 가능한 전원 드라이버로 표시하지 않는다.

`Board.c`는 기존 보드 인터페이스의 Acquire/Ready/Release와 별도 전원 인터페이스의 SetPower를 실제 `MaliMfgController`에 연결한다. `Soc.c`가 공유 MMIO·주소 모델 확인 경로·캐시 처리·자원 사용권을 제공하며 `Board.c`가 같은 GPU의 IRQ를 대조한다. 전원 호출마다 Enter/Leave로 SoC 레지스터·레귤레이터·클록의 사용권과 뮤텍스를 유지하며, Enter 실패 시 하드웨어에 접근하지 않는다. PMIC 거래는 공용 PRM 처리부가 UFS와 같은 잠금·상태 기록으로 직렬화한다. 거래 완료 뒤에는 버스를 놓지만, 불명확한 명령이 남으면 새 접근을 막는다. PRM 모듈 고정은 상태 기록이 실행 중 교체로 사라지는 것을 막는 별도의 수명 처리다.

두 인터페이스는 별도 참조를 갖고, 성공한 게시가 초기 참조 하나를 소유한다. 버전 1 이상 요청에는 버전 1과 실제 크기만 반환한다. 부분 확보 실패도 정리용 사용권을 반환하며, 실패한 해제는 원래 사용권을 보존한다. 증가한 번호를 반환해 이전 사용권이 새 확보에 쓰이지 않게 하고 번호가 소진되면 새 확보 전에 거부한다. 0이 아닌 성공 계열 상태도 전원·자원 완료 증거로 받아들이지 않는다. 철회는 새 질의·확보를 막고 기존 사용자의 전원 끄기·해제를 허용하며, 참조나 자원이 남으면 메모리 해제를 허용하지 않는다. `Host.c`의 PnP 호스트가 아래 자원 연결/조회 처리 함수와 자체 요청 종료를 연결한다.

실제 보드·전원 인터페이스와 PMIC·SPM·클록 제어기를 함께 실행한 ARM64 검사에서 206,192개 항목을 통과했다. 16조합·48회 D3/D0 반복, 잘못된 초기화 18가지·확보 실패 13가지, 접근 거부·기존 전압 복원·이전 사용권·해제 재시도·공유 하드웨어 격리·참조 오류를 포함한다. OS 자원/IRQ/주소/캐시의 증명과 물리 레지스터는 모의 서비스다. Miniport/Runtime 검사와 이 보드 제어기 검사는 별도이며, 실제 Windows·실기기 전원 검증을 대신하지 않는다.

`MtkMaliBoardBindResources`는 하위 START_DEVICE 성공 후 받은 같은 PDO의 원본·변환 자원 목록을 대조한다. GPU 범위는 `0x13000000/0x4000`, 원본 IRQ는 GPU/MMU/JOB 순서의 GSIV 428/429/430이다. 각 원본 항목과 짝을 이루는 OS 변환 벡터·우선순위·CPU 마스크를 복사하며, 변환 벡터를 GSIV로 간주하지 않는다. PnP와 DDI는 `InterruptResources.h`의 같은 검사를 쓴다. Miniport는 질의의 추가 정보에 PDO와 검사한 변환 IRQ를 전달한다.

`MtkMaliBoardQueryInterfaceIrp`는 실제 WDM 질의에서 해당 GUID·추가 정보·저장한 IRQ를 대조하고 독립 참조가 있는 인터페이스를 반환한다. GUID는 값으로 비교한다. 알 수 없는 요청과 이미 처리된 응답은 변경하지 않는다. 반환값이 TRUE면 호스트가 IRP를 완료하고, FALSE면 그대로 아래로 전달해야 한다. 중지 시 사용권과 참조를 반납한 뒤 Unbind해야 하며, 제거 시 Close로 새 요청을 막고 기존 정리와 PnP 호출 종료를 마친다. 사용권·참조·고장 상태가 남은 인스턴스는 자원 연결이나 메모리를 해제하지 않는다.

이 자원 연결·조회·실제 제어기 경로는 ARM64에서 일반 IRQ 구조 206,192개, 프로세서 그룹을 별도로 담는 구조 206,210개 검사를 통과했다. 잘못된 원본/변환 자원 44/45가지, 잘못된 조회 23/24가지, 메모리 항목 위치 4가지, 입력 목록 수명·독립 참조·철회·부분 고장 후 보존을 확인했다. 실제 조회로 얻은 두 인터페이스에서 PMIC/SPM/클록 D3/D0를 실행했다. OS가 넘기는 목록·IRP와 물리 하드웨어는 모의 환경이며, Miniport 검사와 별도다.

`MtkMaliBoardDispatchPnp`는 하위 필터에서 호출하는 실제 WDM 요청 처리 함수다. START는 하위 완료와 자원 대조를 모두 통과한 뒤에만 인터페이스를 게시한다. QUERY_STOP/QUERY_REMOVE는 참조·사용권·하드웨어 정리 상태를 검사하고 새 질의와 확보를 막는다. 하위 CANCEL이 성공하면 일치하는 요청의 차단만 해제한다. STOP으로 자원 연결을 끊은 뒤 같은 제공자에서 다시 START할 수 있으며, START가 실패한 뒤의 STOP도 처리한다.

하위 요청은 즉시 완료·지연·실패·취소 어느 경우에도 완료 콜백이 끝나기까지 기다린다. IRP의 다음 스택과 스택 이벤트를 사용하므로 이 경로에 메모리 할당 실패나 조기 시간 초과를 넣지 않는다. 시작·취소·중지 완료 시 하드웨어 호출과의 경합도 기다려 처리한다. 일반 PnP 요청의 상태와 반환 정보는 그대로 전달한다. 강제 제거는 새 작업을 철회하고 진행 중인 하드웨어 호출이 끝난 뒤 하위 버스로 전달한다. 이후 Ready/전원/Release도 GPU에 접근하지 않으며, 남은 사용권과 고장 기록은 보존한다. 제거 IRP 완료를 자원 해제 성공으로 간주하지 않는다.

PnP 검사에는 같은 제공자의 재시작 3회, 즉시/지연 START 실패 6가지, 두 종류 중지 취소와 하위 취소 실패, START 실패 뒤 STOP, 호출 경합, 다른 PDO 거부, 참조만 있는 상태·살아 있는 사용권·고장 사용권·참조 손상 4가지 강제 제거가 포함된다. 일반/그룹 IRQ 구조의 전체 보드 검사 수는 206,192/206,210개다. 하위 I/O 관리자·장치 객체·동시 스케줄링은 모의 환경이다. `MtkMaliBoardProcessPnp`는 호스트가 제거 잠금을 정리한 뒤 완료할 수 있도록 IRP 완료를 분리한다. 기존 `MtkMaliBoardDispatchPnp`는 완료까지 수행하는 호출 방식도 유지한다. 실제 호출·보존 주체는 아래 `Host.c`에 연결했다. [Microsoft의 중지 질의 순서](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/handling-an-irp-mn-query-stop-device-request--windows-2000-and-later-)와 [제거 잠금 규칙](https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/wdm-removelockquerymnremove)을 기준으로 구현했다.

ACPI `GPU0`(`HANA678E`)의 실제 AML에서 위 MMIO·IRQ 자원과 비일관성 DMA 표시를 순정 DTB와 대조했다. EVENT/PWR는 순정 JM probe가 요청하지 않으므로 이 장치에 할당하지 않는다. `_STA`는 장치 존재를 뜻하며 전원 준비를 증명하지 않는다. 새 AML을 넣은 RELEASE 펌웨어와 시작 코드 14가지·내부 테이블/드라이버 일치 검사를 통과했다. 구현한 PnP 호스트의 필터 부착·제거 잠금·참조와 잔여 인스턴스 보존은 아래 별도 SYS에 연결했다. UFS/GPU의 공용 PRM 통신 조정은 연결했다. 보안 영역·외부 소비자와의 접근 배제, 공유 전원/클록 사용권, IRQ 물리 연결, 주소/캐시 일관성과 기기 보정 OPP 입증은 남아 있다. 실제 부모 인터페이스가 없으면 MMIO를 쓰지 않고 시작을 거부한다. 산출물에는 미서명 보드 하위 필터 SYS/INF가 포함되며 Windows 설치·등록 성공이나 그래픽 드라이버 완성을 뜻하지 않는다. [Microsoft의 DriverEntry 절차](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/driverentry-of-display-miniport-driver)와 [QueryInterface 참조 수명](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/irp-mn-query-interface)을 따른다.

## 실제 WDM 하위 필터와 제거 수명

`HostEntry.c`의 실제 DriverEntry가 `Host.c`의 AddDevice와 요청 처리 함수를 등록한다. AddDevice는 정확한 `ACPI\HANA678E` 하드웨어 ID를 확인하고 장치를 만든 뒤, 같은 스택 아래의 실제 SoC 자원 제공자에 QUERY_INTERFACE/Open을 요청한다. 반환한 사용권은 제어기·공유 자원·콜백 저장 공간 전체를 Close 성공까지 보존해야 한다. 하위 필터는 부모의 수명 콜백 주입을 거부하고 자체 장치 참조를 연결한 뒤 `IoAttachDeviceToDeviceStackSafe`로 부착한다. GPU MMIO 범위만 보고 공유 전원 소유권이나 직접 물리 주소 모델을 허용하는 경로는 없다.

일반/전원 요청은 하위 완료까지 `IO_REMOVE_LOCK`을 유지한다. REMOVE는 하위 완료를 받은 뒤 진행 중인 요청이 끝나기를 기다리고 스택에서 분리한다. 인터페이스 참조·살아 있는 사용권은 이 대기에 넣지 않으므로 고장 난 사용자가 시스템의 제거 요청을 무한 대기시키지 않는다. 인터페이스의 초기/추가 참조는 각각 장치 객체를 고정하며 마지막 반납 뒤 작업 항목으로 정리를 이어간다. 실행 중 추가 반납과 정리 요청의 경합도 잃지 않도록 처리했다. 사용권·부분 고장·잘못된 부모 반환값이 남으면 장치·부모 참조·콜백·정리 기록을 보존하고, 모든 Close가 정확히 성공한 뒤에만 해제한다.

실제 Host/Board/PMIC/SPM/클록 코드를 연결한 ARM64 모의 검사 29,755개가 일반/프로세서 그룹 IRQ 구조 각각에서 통과했다. 두 하위 완료 방식·각 3회 시작/중지·일반/지연 전원 I/O·완료 뒤 제거 잠금 해제 순서, AddDevice/지연 시작의 실패·처리 19가지·잘못되거나 부분 확보된 부모 5가지, 제거 후 남은 참조·사용권·고장 보존과 마지막 반납 정리를 확인했다. Windows I/O 관리자·부모 자원과 물리 MMIO는 검사 경계의 모의 서비스다.

`MtkMaliBoard.sys`는 이 하위 필터의 실제 ARM64 PE이고 `MtkMaliBoard.inf`는 해당 장치만 대상으로 하는 Extension INF다. 실제 링크 맵에서 HostEntry/Host가 선택되고 그래픽 Miniport의 DriverEntry가 포함되지 않는 것을 확인했다. PE 체크섬·WDM 표시·가져오는 함수·INF 서비스와 하위 필터 지정을 검사했다. 미서명 상태이며 CAT 생성·Windows InfVerif·Windows 설치/로드는 미실행이다. `Soc.c`의 실제 부모 자원 제공자를 연결했으며, 아래의 기기 조건 입력이 없으면 자원 확보를 거부한다. 부모가 AddDevice 시점에 준비되지 않았으면 필터만 부착하고, 하위 START 성공 뒤 실제 부모를 다시 조회한다. D3D UMD나 그래픽 미니포트 SYS는 이 패키지에 포함되지 않는다. [제거 잠금의 완료·분리 순서](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/using-remove-locks)와 [장치별 AddFilter 지정](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/inf-addfilter-directive)을 따른다.

## 공유 자원 제공자와 부모 장치

`ACPI\HANA678F` (`\_SB.MFG0`)가 SPM·INFRACFG_AO·MFG_TOP·TOPCKGEN·APMIXED의 다섯 영역을 한 Windows 장치로 소유한다. GPU `_DEP`가 이 장치를 참조한다. `MtkMaliSoc.inf`와 GPU의 Extension INF는 같은 `MtkMaliBoard` 서비스·SYS를 사용한다. OS가 준 원본/변환 자원의 주소·크기·독점 할당을 모두 대조하며 다섯 매핑 중 하나라도 실패하면 이전 매핑을 전부 해제한다. 초기화·START·팩토리 Open은 MMIO를 실행하지 않는다. 순정 DT의 PWRAP `0x10026000`은 Windows 자원에서 제외하고 두 PRM 처리 함수만 변환된 런타임 매핑으로 접근한다.

부모 팩토리는 실제 GPU PDO와 부모 장치 참조를 유지한다. Enter/Leave의 커널 뮤텍스는 레일·SPM·클록 작업 전체를 감싸며, 부분 고장·잘못된 반납·강제 제거 뒤에는 매핑과 사용권을 보존하고 추가 하드웨어 접근을 막는다. 부모 장치 D3는 GPU 전원 정리가 끝났을 때 허용한다. D0는 하위 완료와 현재 조건을 다시 확인한 뒤 접근을 재개하고 `PoSetPowerState`로 OS에 알린다. 시스템 전체의 절전 정책과 Windows 실제 실행은 별도 검증 대상이다.

비일관성 페이지 표는 신뢰할 수 있는 비페이지 CPU 범위를 페이지마다 스택 MDL로 만들고 `MmBuildMdlForNonPagedPool`·`KeFlushIoBuffers`와 완료 장벽으로 GPU에 반영한다. 임시 MDL 메모리 할당 실패를 숨기는 콜백이 없다. 일반 버퍼의 입력/출력 MDL 처리는 기존 Residency/Memory 경로를 유지한다.

`MtkMaliSocSetQualification` 버전 2는 커널 내부에서 기기별 보정 OPP 표를 한 번 설치하는 경로다. 이미 보정한 표를 받거나, 선택적인 `MALI_MFG_CALIBRATION`과 보정 전 순정 45개 항목을 받아 아래의 순정 계산을 적용한다. 세 AVS 값의 읽기 성공 표시가 모두 있어야 하며 명시적인 0과 누락을 구분한다. 성공 후에는 보정한 표만 복사해 유지하고 원래 보정값 포인터는 남기지 않는다. 표를 복사·검사하고, 호출 때마다 펌웨어/다른 사용자의 접근 배제·직접 DMA·유휴·현재 전압/주파수 조건을 확인한다. START/STOP 주기마다 새로 설치해야 하며 실행 중 표 교체는 거부한다. **이 실제 기기 조건을 제공하는 제품 백엔드는 아직 없으므로 기본 전원 확보는 거부된다.** DT의 주소나 `_STA`만으로 허용하는 기본값·레지스트리 스위치를 넣지 않았다.

실제 Soc/Host/Board/MFG 코드의 ARM64 모의 검사 189,064개가 두 IRQ 구조에서 각각 통과했다. 여섯 영역의 12가지 순서·매핑 실패 6위치·잘못된 자원 20가지·조건 입력 오류 16가지, 레일/클록 16조합·48회 D3/D0, 캐시 페이지 경계·이전 사용권·잘못된 스레드 반납·부분 전원 실패 보존, 부모 시작/중지/취소/제거와 지연 부모 조회를 포함한다. OS 객체·I/O·물리 레지스터와 기기 조건의 확인 결과는 모의 서비스다. [Microsoft의 장치 시작 순서](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/starting-a-device)와 [ACPI 장치 의존성](https://learn.microsoft.com/en-us/windows-hardware/drivers/bringup/device-management-namespace-objects)을 기준으로 연결했다.

`MtkMaliPrepareInitialize`가 제공하는 실제 Prepare/Release/Wake를 `MtkMaliDdiInitialize`에 넘긴다. DDI가 성공적으로 지정한 루트 주소를 준비 코드에 전달하므로 사용자 개인 데이터에서 루트 주소를 받아들이지 않는다. 준비 코드는 DMA 주소와 개인 데이터를 별도 공간에 복사하고, 전체 작업 연결과 슬롯 지원 기능을 검사한 뒤 실행용 사본을 만든다. 원래 DMA 주소나 작업의 next/index는 하드웨어에 그대로 넘기지 않는다.

기존 버전 1의 8/16/32/64비트 값 쓰기·시스템 시간·캐시 명령을 유지하고, 버전 2에 arch-9 compute·fragment와 명시적인 버퍼 목록을 추가했다. 최대 64개 작업·16단계·64KiB DMA를 받는다. 전체 작업 연결·자원 목록·범위·권한을 먼저 검사하고 고정한 뒤 사본을 만든다. 순환·중복 작업·잘못된 의존·예약 비트·주소 넘침·정렬 오류·지원하지 않는 슬롯과 쓰기/실행 권한의 동시 요청을 거부한다. compute의 작업 크기·개수·오프셋과 Shader Environment의 자원 표·셰이더 정보·스레드 저장 정보·FAU 범위를 검사한다. fragment는 inclusive 타일 범위·다섯 렌더 순서·MFBD의 1~8개 렌더 대상과 선택적인 깊이/체크섬 확장의 읽기 범위를 검사한다. 슬롯의 `JS_FEATURES` bit 9가 있어야 받으며 읽기 전용 프레임버퍼 정보와 쓰기 가능 출력 버퍼를 기존 전용 MMU에 연결한다. 고정 기능 clear를 위해 실행 권한 버퍼는 필수가 아니다. ZERO, cycle counter, tile-enable map과 scissor array는 거부한다.

arch-9 malloc-vertex 384바이트·tiler 256바이트·fullscreen 128바이트 명령도 버전 2로 받는다. 모든 명령의 실제 길이를 보존하고 128바이트 경계로 연결 주소·번호·의존 관계를 다시 만든다. 인덱스 크기별 전체 범위, primitive/scissor, VS position·선택적 varying·PS 환경, 정점 할당 간격, tiler 쓰기 권한과 DCD·blend·depth·occlusion 범위를 검사한다. 서로 겹치는 원본 작업은 거부한다. 명령 수신 경로에 더해 `MaliCommandsBuildRaster`가 정점/fragment 명령을 만들고 `MtkMaliUmdSubmitRaster`가 실제 KMT 제출로 연결한다. 단일 RGBA8/BGRA8/BGRX8 RT의 제한된 삼각형 그리기에 자원·FAU·타일/프레임 설명 정보와 LOAD_IMAGE preload 생성을 연결했다. 기존 clear·복사·갱신·초기 데이터와 반복 Draw를 허용하며, 일반 셰이더 입력·혼합·깊이·Present 연결은 남아 있다.

버전 2의 개인 데이터는 16바이트 접두부 `(Version=2, Bytes, StepCount, ResourceCount)`, 기존 16바이트 단계 항목, 16바이트 자원 항목 `(GPUVA:uint64, Bytes:uint32, Access:uint32)` 순서다. 자원은 4KiB 단위로 정렬하고 서로 겹치지 않게 한다. `Access`는 `MaliPteRead=2`, `MaliPteWrite=4`, `MaliPteExecute=8` 조합이며 읽기는 필수다. 최대 128개 범위·합계 512MiB이며 한 범위는 한 할당의 연속 오프셋이어야 한다. 조회의 `CommandVersion=2`는 최신 형식이며 버전 1 제출도 받는다. 기존 `MaliCommandsPrepare`는 버전 1 전용이고, 실제 준비 코드는 `MaliCommandsPrepareResources`와 `PinResource`를 연결한다.

`MaliCommandsBuildClear`는 전체 표면을 한 색으로 지우는 실제 arch-9 고정 기능 명령을 만든다. 기존 할당 형식의 RGBA/BGRA 및 X8 UNORM 네 가지, 단일 표본·선형 표면을 지원한다. MFBD와 출력 정보, 새 타일 문맥, 256KiB 힙 정보, 표본 위치를 모두 채우고 버전 2 제출 자료까지 만든다. 타일 문맥·힙의 GPU 쓰기와 전체 버퍼 수명을 기존 MDL·전용 MMU 경로로 연결한다. CPU와 GPU 범위가 겹치거나 크기·주소·지원 슬롯이 잘못되면 출력에 손대기 전에 거부한다.

색상은 RGBA의 IEEE binary32 비트로 받고 커널에서 부동소수점 연산 없이 변환한다. Mesa와 같이 binary32 곱셈을 먼저 반올림한 뒤 가장 가까운 짝수 정수로 변환한다. BGRA 순서는 출력 변환에 적용하며 타일 내부 색상은 RGBA를 유지한다. X 형식 알파는 1로 채우고 음수·NaN·무한대도 처리한다. 원본 Mesa `float_to_fixed`의 네이티브 실행 및 별도의 binary32 계산과 대조한다. 이 함수는 D3D 런타임에 등록한 콜백이 아니며, 실제 GPU 출력 픽셀은 아직 실행하지 않았다.

셰이더 명령어·프레임버퍼 내부 설명 정보·간접 참조 내용은 신뢰하지 않는다. fragment 검사는 작업 본문과 프레임버퍼 정보의 바깥 범위에 해당하며, 픽셀 형식·출력 표면·프레임 셰이더 내부를 해석하거나 고정하지 않는다. 실행용 주소 공간에 유지한 프로세스 소유 버퍼와 명령/상태 사본만 연결하고 OS 표·커널 페이지 표·MMIO는 연결하지 않는다. GPU가 상태를 써야 하는 명령 사본은 GPU 쓰기를 허용하므로, 임의 셰이더가 자신의 연결을 훼손할 수 있다. 명령 사본이나 간접 참조의 불변성을 보장한다고 주장하지 않으며, 잘못된 작업은 오류·리셋과 자원 수명 처리 대상이다. 이 구조는 [Microsoft의 GPU별 주소 공간과 GpuMmu 모델](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/gpummu-model)을 기준으로 한다.

`Residency.c`가 `Prepare.h`의 Begin/CopyDma/ReserveShadow/PinWrite/PinResource/Seal/End/Wake를 구현한다. `Vm.c`는 성공한 OS CPU 페이지 표 갱신을 보관하며, 할당의 고유 번호로 주소가 재사용된 이전 자원을 거부한다. ExplicitPageTableInvalidation을 보고하고 초기 갱신·퇴거·전체 무효화 때 기록을 교체하거나 제거한다. 아직 갱신되지 않은 항목을 실제 표에서 임의로 읽어 허용하지 않는다.

Begin은 해당 프로세스·컨텍스트·OS 루트의 매핑을 유지한다. CopyDma와 PinWrite는 실제 할당의 프로세스/장치 접근 권한과 논리 범위를 확인하고, OS가 준 공유 backing 주소에 **새 MDL**을 만들어 `MmProbeAndLockPages`로 페이지를 고정한다. 고정된 PFN이 OS GPU 주소의 물리 페이지와 일치해야 한다. MDL/시스템 주소 확보 실패와 PFN 불일치 때 이미 얻은 참조를 모두 정리한다. 임의 물리 주소를 CPU로 매핑하는 경로는 없다. [Microsoft의 MDL 사용 절차](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/using-mdls)를 따른다.

ReserveShadow는 24KiB 명령 사본과 제출 전용 페이지 표를 만든다. 완료/오류 상태와 GPU가 기록하는 정점 위치를 위해 여섯 페이지를 읽기·쓰기/NX로 유지하고, 실제로 사용한 명령 길이만 제출한다. 원본 명령의 GPU 주소는 실행 표에 넣지 않는다. `PinResource`는 범위당 MDL과 객체 참조 하나를 유지하고, 모든 페이지의 실제 PFN·OS 권한·할당 번호·연속 오프셋을 대조한다. RO 입력, RX 셰이더, RW 출력 권한을 그대로 구분하며, 유지 중인 다른 제출과 같은 물리 페이지를 가리켜 쓰기/실행 권한이 합쳐지는 경우도 거부한다. 최대 33개 제출을 유지하며 기존 명령 입력/즉시 쓰기 81페이지에 자원 범위 128개·합계 512MiB를 추가한다. 전용 표 상한 1,223페이지는 연속 자원이 여러 표 경계를 가로지르는 경우까지 포함한다. Seal은 모든 자원의 CPU 쓰기를 GPU에 반영하고, End는 쓰기 대상의 GPU 결과를 CPU에 반영한다. 별도 루트는 **완료 통지 전에** GPU에서 분리하며, 분리 실패·GPU 오류·실패한 리셋 때 MDL과 참조를 유지한다. 해제 때 명령/표를 지우고 잠금·참조를 돌려준다.

`MtkMaliRuntimeStart`가 준비 코드·DDI·VM·Residency·기능 조회·Worker를 연결한다. 부모는 새로 0으로 지운 비페이지 메모리에 `MALI_RUNTIME` 한 개를 확보하고, 입증한 주소 모델·이미 시작한 공용 코어·OS 콜백·메모리 예산을 제공한다. 첫 멤버가 실제 DDI 어댑터이므로 그 주소를 hAdapter로 사용한다. 관리형 인터럽트 모드에서는 `Config.Interrupts`/`InterruptCount`로 받은 실제 OS 변환 IRQ 자원 3개를 먼저 검사하고 Worker가 준비된 뒤 IRQ를 활성화한다. `MtkMaliRuntimeStop`은 먼저 새 작업과 IRQ를 막고 연결 해제를 확인한 뒤 스레드와 메모리 관리·준비 코드를 정리한다. 시작이 실패했더라도 `Stages`가 남으면 객체와 코어·콜백을 보존하고 Stop을 재시도해야 한다. 이 API는 보드 전원·IRQ 연결이나 PnP 진입점 자체를 구현하지 않는다. 이 경로는 **OS가 공유한 CPU 접근 가능 backing과 부모가 입증한 직접 물리 주소 모델**을 요구한다. Miniport.c가 이 묶음의 PnP/매핑 수명을 연결한다. 전체 그래픽 자원·셰이더 메모리 관리와 실제 OS/보드 제공자 연결은 남아 있다. [Microsoft의 GpuMmu 설명](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/gpummu-model)에 맞춰 VidMm 소유 페이지와 KMD 전용 MDL 할당을 구분한다.

### D3D10 셰이더 수명과 선택

`MtkMaliUmdShaderFunctions`가 실제 WDK 형식의 크기 계산·VS/PS 생성·해제·선택
6개 콜백을 채운다. 원래 토큰과 전체 입력/출력 목록을 소유 메모리에 복사한다.
생성 때 실제 컴파일러로 검증한 임시 결과는 해제하고, 실패하면 오류 통지 전에
모든 메모리를 정리한다. Windows는 생성 실패 후 해제 콜백을 보내지 않는다.
[Microsoft의 셰이더 생성 규칙](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_createvertexshader)을 따른다.

선택은 단계·장치 소유자·계속 증가하는 객체 번호를 확인한다. null 선택과 선택된
객체의 삭제는 참조를 지우며, 이전 핸들이 새 객체를 가리키지 못한다. 살아 있는
셰이더가 있으면 Close는 정리를 기다린다. `MtkMaliUmdCompileBoundShaders`는
부모가 실제 입력 배치/출력 자원에서 준 정수 형식으로 선택한 쌍을 다시 컴파일한다.
결과는 호출자가 소유하고 `t70_shader_free_pipeline`으로 해제한다. 이 결과를
GPU 메모리 전송은 아래 업로드 API로 연결했다. 생성한 프로그램을 사용하는 명시적 정점/fragment 제출은 아래 API로 연결했고, 제한된 삼각형 Draw와 기존 픽셀 preload 생성은 아래에 연결했다.

사용자 구성 요소를 실제 ARM64 `Windows/Build/MtkMaliUmd.dll`로 링크했다.
컴파일러 DLL의 토큰 API를 가져오고, SDK의 `_DllMainCRTStartupForGS`와
`__security_init_cookie`를 연결해 스택 보호를 초기화한다.
[Microsoft의 스택 보호 초기화 조건](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/security-init-cookie?view=msvc-170)을 따른다.
배포할 때 컴파일러·vkd3d DLL을 같은 패키지에 포함해야 한다.
이 DLL에는 OpenAdapter/CreateDevice 등록이 없다.

실제 UMD ARM64 코드는 321,242개 모의 검사에서 생성 실패·메모리 부족·잘못된
단계·소유자·이전 핸들, 입력 자료의 독립 복사·해제 시 선택 해제·오류 통지 중 재진입,
호출자 소유 결과·컴파일 오류 전달과 기존 자원 처리를 확인했다.
이 검사의 CPU 컴파일러 경계는 대체 함수다. 별도 호스트 검사에서는 실제
vkd3d/Mesa 컴파일을 실행한다. Windows 로더·D3D 런타임·실제 GPU 실행은 남아 있다.

### 컴파일한 VS/PS의 GPU 코드 업로드

`MtkMaliUmdUploadBoundShaders`는 선택한 VS/PS를 입력/출력 형식에 맞춰 함께
컴파일하고, 각각 독립된 KMT 버퍼에 실제 코드 바이트를 복사한다. 할당 전체를
먼저 0으로 지워 페이지 끝을 초기화하고, 캐시 처리·잠금 해제·상주 처리와
주소 준비 fence의 GPU 대기를 거친다. 반환하는 두 `MALI_COMMAND_RESOURCE`는
읽기·실행 권한이며 GPU 쓰기는 허용하지 않는다.
[Microsoft의 주소 보호 형식](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dukmdt/ns-d3dukmdt-_d3dddigpuvirtualaddress_protection_type)에 맞춰 `Write=0`, `Execute=1`을 설정한다.

세 번째 KMT 버퍼에는 arch-9의 실제 32바이트 shader program 정보를 만든다.
정점 위치의 점/비점 진입 위치, 선택적 보간 코드와 픽셀 코드의 주소를 각각 돌려준다.
정확한 코드 범위와 8바이트 정렬을 검사하고, 32/64 레지스터 등급·R48–R63의
미리 읽을 값·부동소수점 FTZ·픽셀 보조 스레드 설정을 컴파일 결과에서 가져온다.
설명 정보의 GPU 주소는 `Write=0`, `Execute=0`이며 쓰기·실행을 허용하지 않는다.
코드와 함께 캐시·상주·주소 준비 대기를 거치고, 사용이 끝날 때까지 함께 보존한다.
보간 코드가 없으면 해당 주소와 예약 영역은 0이다. `MaliCommandsBuildShader`의
바이트 형식은 기존 Mesa `v9.xml`과 별도로 만든 1,024개 입력으로 대조한다.
실제 컴파일러 검사도 생성한 메타데이터와 진입 위치를 이 생산자에 전달한다.

`MALI_UMD_PROGRAM`은 고유 번호·장치 소유자, 컴파일 결과의 읽기 전용 주소와
GPU 코드·설명 정보 범위, 네 종류의 프로그램 주소를 돌려준다. 원래 셰이더를 삭제하거나 다시 선택해도 내용은
유지된다. 부모 그리기 코드는 같은 업로드를 여러 번 사용하고 마지막 사용 뒤
`MtkMaliUmdReleaseProgram`으로 반납한다. 이 함수는 핸들을 소비한 뒤
[D3DKMTWaitForIdle](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtwaitforidle)의 정확한 완료를 확인한다. 대기 실패·단순 pending 결과,
진행 중인 주소 준비·잠금 해제·GPU 주소 반납·할당 해제 실패에는 객체와
컴파일 결과를 유지하고 Flush/Close에서 이어서 정리한다. 살아 있는 업로드는
Close를 막는다. 제출한 프로그램은 완료 이벤트와 명령 정리가 끝날 때까지 참조를 유지한다.
반납 요청 후 실제 코드 해제 직전에는 장치 전체 유휴 상태도 확인한다. 업로드 상한은 32쌍이다.

ARM64 UMD 검사는 321,242개다. 실제 제품의 복사·OS 호출·소유권 코드를
실행하지만 컴파일러와 Win32/KMT, GPU 사용 완료는 대체 함수다. 크기가 다른
두 코드·설명 정보와 페이지 여유 공간, 잘못된 진입 위치/레지스터/단계,
세 할당의 모든 확보/복사 준비 실패, 비동기 주소 준비,
종료 대기 실패·pending, 재바인딩/원본 삭제와 이전 핸들 거부를 확인했다.
별도의 실제 vkd3d/Mesa 컴파일 검사와 DLL 연결을 유지한다. 조립한 정점/fragment 명령을
실제 수신 코드로 검사하며, GPU 셰이더 명령 자체는 실행하지 않는다.

### 정점 처리·화면 그리기 명령 제출

`MtkMaliUmdResourceRange`로 현재 소유 자원의 GPU 주소 범위를 얻는다.
`MtkMaliUmdSubmitRaster`에는 업로드한 프로그램, 명령 상태와 모든 간접 참조
자원 목록을 전달한다. 정점 위치의 점/비점 코드, 선택적 보간 코드·간격과 픽셀
코드는 내부 업로드 객체에서 선택한다. 외부 프로그램 정보의 코드 주소를 믿지
않으며 컴파일 결과의 FAU 개수와 실제 64비트 단위 개수를 대조한다.

명령 생성 함수는 실제 arch-9 malloc-vertex 384바이트와 fragment 128바이트를
만들고 두 작업을 순서가 있는 별도 단계로 제출한다. 주소·접근 권한·코드의
쓰기/실행 분리·인덱스·가위 영역·혼합/깊이/질의 범위는 수신부와 같은 검사로
확인한다. 실패하면 출력은 보존한다. 독립적인 Mesa XML 비교 256개와
실제 수신부를 거치는 왕복 검사를 통과했다.

명령·셰이더·자원 전체의 상주와 주소 준비 대기를 연결하고 완료 이벤트까지
프로그램과 자원 참조를 보존한다. 제출 후 삭제, 동적 버퍼 DISCARD 교체,
오래된 주소, 완료 신호 실패, 잠금/주소/이벤트 해제 실패와 재시도를 검사했다.
사용자 자원과 프로그램 자원의 합계 128개, 동시 제출 32개 한도도 실제 경로로
검사했다. 일반 업로드는 프로그램 자원 3개, 내부 preload를 포함하면 4개를 사용한다. 이 API의 한도 초과는 실패를 돌려주고 이미 제출한 작업을 보존한다.

직접 제출 API의 호출자는 자원·속성·샘플러 표, TLS·FAU, tiler와
프레임버퍼·깊이·혼합 설명 정보를 준비한다. 아래의 제한된 Draw 콜백은
이 중 필요한 상태와 기존 픽셀을 읽는 내부 preload 셰이더·표를 직접 만든다.
일반 그리기·Present·어댑터 등록은 남아 있다. 검사의
Win32/KMT와 컴파일러 호출 경계·GPU 완료는 대체 함수이고 픽셀은 실행하지 않는다.

### D3D10 자원·전송·뷰·clear 연결

Umd.c는 실제 WDK의 자원/RTV 크기 계산·생성·해제, clear/Flush, 자원 복사·업로드·매핑 등 22개 함수 형식을 사용한다. 버퍼는 정점·인덱스·상수·셰이더 입력 용도를 받고, 2D 자원은 mip/배열/표본이 각각 1개인 RGBA8·BGRA8·BGRX8 UNORM을 받는다. 기본·읽기 전용·동적·임시 읽기/쓰기(staging) 사용 방식을 구분한다. 상수 버퍼는 16바이트 배수·최대 64KiB이고 다른 바인딩과 겸용하지 않는다. 2D pitch는 64바이트, 할당 크기는 4KiB로 올려 공용 AllocationAbi.h의 같은 40바이트 형식으로 만든다. 이 구성 요소의 자원 상한은 512MiB에서 clear 설명 정보·힙 공간을 뺀 크기다. mip 묶음·배열·MSAA·깊이/스텐실·다른 형식·sRGB·typeless·외부 공유·primary는 아직 받지 않는다.

현재 커널 구현은 [KMD와 공유한 backing store](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/sharing-backing-store-with-kmd)를 요구한다. 실제 D3DKMTCreateAllocation2에 CreateResource|CreateShared|NtSecuritySharing를 지정하고 전역 공유 핸들이나 외부 NT 핸들을 만들지 않는다. 런타임의 불투명 핸들을 KMT 핸들로 바꾸어 쓰지 않는다. 부모가 실제 어댑터 LUID와 검증된 기능 정보를 주면 별도 KMT 장치·가상 문맥·paging queue를 만든다. 이 장치와 D3D 런타임의 자원 공유·화면 표시·질의 연동은 아직 구현하지 않았다.

초기 데이터와 기본 자원 갱신은 원본 행 간격을 적용해 실제 픽셀/버퍼 바이트만 기록한다. 전체·부분 복사는 GPU와 동기화한 **CPU 복사**이며 셰이더나 GPU 복사로 계산하지 않는다. 버퍼 좌표는 바이트, 2D 좌표는 픽셀이고 음수·범위 초과·덧셈 넘침·잠긴 자원·서로 다른 형식·같은 자원·읽기 전용 목적지를 거부한다. 빈 복사 영역은 메모리에 접근하지 않는다. 행의 남는 공간과 페이지 끝을 보존한다. 초기 업로드가 끝나기 전에는 별도 생성 중 표시로 내부 자원을 유지하고 실패한 핸들은 런타임에 공개하지 않는다.

staging Map은 읽기/쓰기/양방향 접근 권한을 확인한다. GPU 사용 중 DONOTWAIT이면 바쁨을 돌려주고 기다릴 수 있으면 실제 OS idle 호출과 완료 이벤트의 무한 대기를 사용한다. 임의 시간 초과나 반복 조회로 완료를 추정하지 않는다. [WDDM의 사용 추적·캐시 규칙](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/allocation-usage-tracking)에 맞춰 CPU 읽기 전과 CPU 쓰기 후 실제 D3DKMTInvalidateCache를 호출한다. 캐시 처리나 잠금 해제 실패는 장치 제거 상태로 전환하고 잠금/할당 소유권을 유지한다. 런타임이 Unmap을 생략해도 삭제/Close 경로에서 정리할 수 있다.

동적 자원의 discard Map은 새 할당을 만들어 교체하고 이전 할당은 미완료 사용이 남아 있으면 보관한다. 새 할당·주소·잠금·캐시 처리에 실패하면 원래 자원을 유지하고 실패한 새 할당도 추적한다. 정점/인덱스 버퍼의 no-overwrite는 기존 바이트를 유지한다. 이전 할당 보존 검사는 실제 제출과 완료 이벤트에 연결한 수명 검사다. GPU가 셰이더 명령을 실행해 해당 바이트를 읽는 검사는 포함하지 않는다.

GPU 주소 상한은 [배타적·페이지 정렬 규칙](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dukmdt/ns-d3dukmdt-d3dddi_mapgpuvirtualaddress)에 맞춘 48비트 끝 주소다. 각 clear는 명령·설명 정보·256KiB 힙을 따로 확보한다. Lock2로 기록하고 사용자 모드 캐시 처리·Unlock2·MakeResident·paging fence의 GPU 대기를 거쳐 기존 생성기의 실제 버전 2 자료를 제출한다. 커널의 MDL 캐시 처리도 유지한다.

제출 뒤에는 [문맥 처리가 끝날 때 울리는 CPU 이벤트](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dukmdt/ns-d3dukmdt-_d3dddicb_signalflags)를 넣는다. 제출 순간 신호는 사용하지 않는다. 런타임 전용 저장 공간에는 장치 소유자와 되돌아가지 않는 번호만 두고 자원/뷰 객체는 별도로 보관한다. 삭제나 Flush가 먼저 와도 실제 작업 완료 전에는 메모리를 해제하지 않는다. 제출 뒤 신호 실패·비동기 paging·해제 실패도 소유 정보를 보존하고 재시도한다. Close는 기다리지 않고 바쁨을 돌려주므로 성공 전에는 부모가 저장 공간을 버릴 수 없다.

셰이더 수명·업로드 검사를 포함한 현재 ARM64 실행에서 321,242개 항목을 확인했다. 초기 업로드·세 형식의 전체/부분 복사·행/페이지 여유 공간·버퍼 바이트 좌표·상수 버퍼 갱신·staging 읽기/쓰기·동적 교체/no-overwrite·생성 중 수명·이전 할당 보존·잠금/캐시/대기 실패를 포함한다. 기존 clear의 실제 생성 자료는 명령 준비 코드도 통과한다. OS·이벤트·캐시 동작·GPU 완료/기록 데이터는 대체 실행이며 실제 Windows·Direct3D 런타임·GPU 픽셀은 실행하지 않았다. 내부 동시 clear와 보관 중인 동적 교체는 각각 32개로 제한한다. 이를 넘거나 내부 할당에 실패하면 해당 구성 요소를 장치 제거 상태로 전환한다. 실제 제품에는 명령 묶기와 메모리 예산 정책이 더 필요하다.


실제 D3D10 Draw와 IA·RT·raster/blend/depth·viewport/scissor 콜백을 연결했다. SV_VertexID 입력만 받는 VS와 입력 없는 PS, 단일 RGBA8/BGRA8/BGRX8 UNORM RT·전체 viewport·혼합/깊이 기능 없는 삼각형 목록을 받는다. 실제 Mesa arch-9 LOAD_IMAGE 셰이더 생성기와 table 0의 sampler 0/texture 1, FAU 범위·DCD·MFBD를 연결했다. clear·복사·갱신·초기 데이터가 있는 대상과 반복 Draw도 받으며, scissor 밖 기존 픽셀을 보존할 명령을 만든다. 셰이더 코드 네 번째 BO와 설명 정보·대상·임시 자원은 완료 이벤트까지 보존한다. 메타데이터는 힙에 두어 Draw 스택을 800바이트로 제한하고, 메타데이터·프로그램·임시 자원 할당 실패도 정리한다. 이 경로를 포함한 실제 ARM64 UMD 검사 321,242개와 독립 Mesa XML preload 대조 246개가 통과했다. 이 검사는 Windows 서비스·컴파일러 호출·GPU 완료를 대체하며, 실제 픽셀 보존·GPU 실행·일반 Draw·Present·런타임 등록은 아직 검증하지 않았다.

같은 VS·PS 객체와 대상 형식으로 반복 Draw하면 최근 프로그램 한 개를 재사용한다. 매번 셰이더를 다시 컴파일하거나 코드 네 개를 업로드하지 않는다. 새 조합의 컴파일·할당·제출이 실패하면 기존 항목을 보존하고, 새 제출 성공 후에만 교체한다. 셰이더 삭제 시 재사용 항목을 비우며, 이전 제출이 남아 있으면 실제 완료까지 코드와 자원을 보존한다. 반복 호출·대상 형식 변경·셰이더 변경·메모리 부족·컴파일 실패·유휴 대기 실패의 보존과 정리 재시도를 실제 ARM64 검사에 포함했다.

산출물은 커널과 분리한 ARM64 Windows/Build/MtkMaliUmd.lib와 MtkMaliUmd.dll이다. 사용자 모드 플래그와 강한 스택 보호를 적용하며 커널 객체를 섞지 않는다. OpenAdapter/CreateDevice DDI, 나머지 필수 함수 표·기능 조회, 업로드한 셰이더의 작업 연결·일반 그리기·Present가 남아 있어 설치 가능한 D3D UMD로 등록하지 않는다. 설치/하드웨어 DirectX 완료 표시도 유지하지 않는다.

`MALI_ALLOCATION_DESC`는 40바이트 고정 UMD 형식이며 최대 1GiB 버퍼와 선형 32비트 2D 자원을 받는다. 2D는 최대 16,384×16,384, 64바이트 pitch 정렬과 A8/X8 R8G8B8 또는 B8G8R8 형식만 지원한다. 크기는 `Pitch*Height` 또는 그 값을 4KiB로 올린 크기만 받는다. GPU 자원 범위를 페이지 단위로 고정할 때는 뒤쪽 여유 공간을 포함한 크기로 명시해야 하며, 기존의 작은 크기 밖으로 접근 권한을 넓히지 않는다. 요청당 256개, 어댑터당 할당·열기·실행 참조 각각 4,096개로 제한한다. 모든 항목을 검사하고 메모리를 확보한 뒤 결과를 공개하므로 중간 실패가 기존 자원 개수나 출력 핸들을 일부만 바꾸지 않는다. 여러 항목을 닫거나 해제할 때도 전체를 먼저 검사한다.

자원/할당에는 만든 장치나 프로세스를 저장하지 않는다. 만든 쪽이 종료돼도 다른 프로세스의 공유 열기는 유지된다. `DxgkCbGetHandleData`가 돌려준 실제 KMD 객체를 어댑터 목록에서 검증하고, 프로세스·장치별 접근과 OS의 ReadOnly를 적용한다. 별도 D3DKMT 숫자 핸들 표를 만들지 않는다. 실행 참조 토큰은 계속 증가하며, 이전 토큰이나 중복 해제가 새 참조를 해제하지 못한다. 참조가 남은 열기는 닫을 수 없고 열린 할당은 해제할 수 없다. 부모 준비 코드에서 이 API를 부를 때 이미 잡힌 DDI Gate를 유지해야 한다.

공유 CPU 주소는 [Microsoft의 공유 backing store 절차](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/sharing-backing-store-with-kmd)를 따른다. WDDM 3.1은 QueryFeatureSupport 또는 IsFeatureEnabled를, 3.2 이상은 QueryServices의 DXGK_FEATURE_INTERFACE를 사용하고 받은 인터페이스 참조를 반환한다. OS가 정확히 성공하고 지원한 경우에만 ShareBackingStoreWithKmd를 설정한다. 3.2에서는 버전 1과 지원/활성 상태를 확인하며 다른 버전이나 실패를 허용으로 바꾸지 않는다. `MtkMaliDdiQueryInterface`가 공식 GUID와 DISPLAY_ADAPTER_HW_ID, 버전 1을 검사하고 DXGKDDI_FEATURE_INTERFACE를 반환한다. 참조가 남으면 종료를 거부하며, 기능 협상 도중 동기적으로 들어온 조회도 처리한다. 부모는 미니포트 등록에 이 함수를 연결해야 한다.

`MtkMaliDdiConfigureAdapter`는 공유 backing과 VM이 준비된 후 부모가 정한 4KiB 정렬·최대 1GiB 예산을 고정한다. QueryAdapterInfo는 GPU 가상 주소를 쓰는 단일 시스템 메모리 aperture를 1번으로 보고한다. QuerySegment4의 첫 호출은 개수만 쓰며 다음 호출은 OS가 정한 구조체 간격과 뒤의 바이트를 보존한다. 할당은 그 세그먼트를 지원하고 AccessedPhysically를 끈다. GpuMmu에서는 OS가 이를 암시적 시스템 세그먼트 0으로 바꾼다. 실제 물리 페이지는 VidMm 소유이며 할당을 해제할 때 OS의 backing 주소나 PFN을 직접 해제하지 않는다. 공유 backing은 OS에서도 공유·CPU 접근 가능 자원으로 만들어야 한다. 기존 사용자 메모리, 보호 자원, 타일/압축 자원과 표시 모드/primary 자원은 이 형식에 포함하지 않는다.

`MtkMaliAllocationAcquire`는 **객체 수명과 접근 권한**을 보장한다. `Residency.c`는 이에 더해 공유 backing의 MDL 잠금, GPU 매핑 유지, PFN/물리 주소 겹침 검사와 캐시 처리를 수행한다. `CopyBacking`은 OS가 준 주소에서 CPU가 작성한 내용을 비공개 버퍼로 복사하는 경로이며 임의 물리 주소 접근이 아니다. GPU가 쓴 결과를 CPU로 읽으려면 실제 생산자 동기화와 캐시 처리가 먼저 필요하다.

성공한 Prepare의 cookie는 실제 작업 완료를 Windows에 통지한 후에 해제한다. 통지가 실패하면 같은 이벤트와 참조를 보존해 재시도한다. GPU 오류 통지만으로 해제하지 않으며, 실제 RESET_DONE을 관찰해야 한다. 리셋 후 재시작이 실패하더라도 앞서 확인한 DMA 중단과 리셋 실패를 구분한다. ResetEngine의 LastAbortedFenceId에는 실제 실행 중 중단된 제출만 포함하며, 대기 작업의 fence를 제외해 Windows가 재제출할 수 있게 한다. 이 동작은 [Microsoft의 엔진 리셋·재제출 규칙](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/tdr-changes-in-windows-8)을 따른다.

완료·선점·오류는 지정된 인터럽트와 동기화한 루틴 안에서 실제 `DxgkCbNotifyInterrupt`로 알리고 `DxgkCbQueueDpc`를 호출한다. 실제 DPC는 `DxgkCbNotifyDpc`로 이어진다. [Microsoft의 통지 규칙](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkcb_notify_interrupt)에 맞춰 MMU 대기·리셋은 PASSIVE 스레드에 둔다.

`MaliJmSetManagedInterrupts(Core,1)`은 첫 Start 전에 선택한다. GPU/JOB/MMU 마스크는 코어의 시작·리셋 동안 0을 유지한다. `MtkMaliDdiConnectInterrupts`는 살아 있는 비페이지 MMIO·전원을 확인하고, 인터럽트와 동기화한 콜백에서 세 마스크를 설정한 뒤 다시 읽어 확인한다. 이 선택은 리셋 뒤에도 유지된다.

`MtkMaliDdiInterruptRoutine`은 DIRQL에서 각 소스의 마스크가 적용된 상태만 읽는다. 자기 장치의 신호가 아니면 FALSE를 반환한다. 감지한 소스를 가리고 원시 상태를 보존한 뒤 DPC를 요청하며, DPC가 PASSIVE Worker를 깨운다. ISR은 전원 검사·뮤텍스·메모리 해제·fence 통지를 실행하지 않는다. Worker의 실제 Poll이 원시 오류/완료를 처리하고, 동기화한 콜백이 마스크를 복구한다. 유휴 오류도 소비하되 존재하지 않는 제출 fence를 만들지 않는다. [ISR 호출 규칙](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dispmprt/nc-dispmprt-dxgkddi_interrupt_routine)과 [인터럽트 동기화 규칙](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dispmprt/nc-dispmprt-dxgkcb_synchronize_execution)을 따른다.

연결·마스크 복구·연결 해제 실패 때는 실제 연결 상태와 객체를 보존한다. 실패 후 연결 플래그나 Runtime의 Stages가 남으면 원래 메모리·전원·콜백을 유지하고 재시도해야 한다. IRQ 마스크 쓰기를 다시 읽어 확인하기 전에는 연결 해제를 성공으로 보지 않는다. 완료/오류/리셋 때 기존 준비 메모리의 보존 규칙도 유지한다.

순정은 JOB/MMU/GPU 세 물리 IRQ를 요청한다. 부모는 실제 OS 변환 자원의 IRQ 설명 3개를 `MtkMaliDdiConfigureInterruptDomain`에 넘기고 세 소스를 같은 ISR/message 0으로 연결한다. 원본 GSI를 Windows 우선순위 값으로 사용하지 않는다. 구성 함수는 레벨 감지·프로세서 그룹 0·서로 다른 벡터·0이 아닌 CPU 마스크·유효한 DIRQL을 전부 확인한 뒤 가장 높은 DIRQL을 저장한다. 시작 뒤 재구성은 거부한다.

세 IRQ의 OS 잠금이나 우선순위가 달라도, ISR과 마스크 변경 콜백은 가장 높은 DIRQL로 올라간 뒤 같은 비페이지 스핀 잠금을 잡는다. 잠금을 먼저 놓고 원래 DIRQL로 돌아가므로 높은 우선순위 IRQ가 같은 CPU에서 잠금을 다시 잡으며 멈추는 상황을 방지한다. ISR은 잠금 안에서 연결 상태를 다시 확인하고, 중지가 먼저 끝났으면 MMIO에 접근하지 않는다. DPC 요청은 공통 잠금을 놓은 뒤 수행한다. 완료 통지는 기존의 고정된 `DxgkCbSynchronizeExecution(...,0)` 경로를 유지한다. [Windows의 인터럽트 공유 상태 보호 기준](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/synchronizing-access-to-device-data), [우선순위 변경](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-keraiseirql), [DIRQL에서의 스핀 잠금](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-keacquirespinlockatdpclevel)을 바탕으로 구현했다.

이 공통 잠금은 물리 IRQ를 OS에 연결하지 않는다. 실제 PnP 자원과 순정 세 라인의 대응·ISR 등록은 부모에 남아 있다. 종료 때 부모는 OS ISR/DDI/DPC 진입을 정리하고 GPU를 정지/리셋해야 한다. WorkerStop 성공은 CPU 스레드와 Wake 호출 정리의 확인이며 GPU DMA 중단을 대신하지 않는다.

IRQ 경로를 포함한 DDI/Worker PE는 77,311개, Runtime PE는 69,175개 검사를 통과했다. WDK의 프로세서 그룹 필드 유무를 바꾼 별도 DDI PE도 같은 77,311개 검사를 통과했다. 같은/서로 다른 우선순위, 유효한 최저·최고 DIRQL, 높은 IRQ의 지연 도착과 다른 CPU의 중지가 먼저 끝나는 순서를 검사했다. OS 우선순위·잠금 서비스와 병행 실행 순서는 검사 경계에서 모의하며 실제 Windows의 다중 CPU 실행을 포함하지 않는다. 연결 전/리셋 중 마스크 0, 가려진 원시 상태의 보존, 공유·무관한 신호, 마스크 복구 중 새 오류, 유휴 오류, 통지 재시도와 연결 해제 실패의 자원 보존을 검사했다. 순정 `kbase_regmap_v6_0_init`을 미리 확보한 가상 버퍼에서 실행해 세 소스의 RAW/CLEAR/MASK/STATUS 주소 12개를 실제 코어·ISR 상수와 대조했다. 이 실행은 물리 MMIO나 순정 커널 ISR 실행을 포함하지 않는다.

## 보드·메모리 호출 조건

`PowerReady`는 보드 전원·클록·독점 소유권을 실제로 확인해야 한다. GPU 내부 전원 준비가 MT6789 보드 전원 제어를 대신하지 않는다. 고른 `0x90930000`은 G57 9.3 컴파일 대상 모델이며 실기기에서 읽은 ID가 아니다.

물리 주소와 비일관성 캐시 처리는 실제 Windows IOMMU/주소 소유권을 확립한 뒤 사용해야 한다. `MtkMaliMemorySubmit`에는 커널에서 만든 신뢰할 수 있는 작업과 루트·페이지 표·명령·셰이더·데이터의 모든 메모리를 제공한다. CPU는 참조가 고정된 메모리에 접근하지 않는다. Execution의 직접 호출은 호출자가 직렬화하며 DDI 경로는 Gate로 직렬화한다. Prepare/Release는 같은 DDI에 재진입하지 않는다.

`MaliMfgSetDomain`은 SPM의 두 전원 상태와 부모/자식 순서를 확인하고 도메인 4개·MFG1 버스 보호를 처리한다. `MaliMfgClockSet`은 mux 준비/활성화·주 클록 선택·10µs 대기·BG3D 준비/활성화와 MFG 설정 6개를 순정 순서대로 처리한다. 종료는 BG3D 해제·보조 클록 선택·mux 해제 순서다. 주 클록 재시도 한도를 지키며 설정은 다시 읽어 확인한다. 실패하면 확보한 단계를 보존하고 재사용을 차단한다. 원본 abort는 오류 반환으로 바꿨다.

`MaliMfgPowerReference`는 레일 0→1, 도메인 0→1, 기준 클록 준비/활성화, 도메인 2→3, GPU 클록과 DCM 설정을 하나의 시작 순서로 연결한다. 마지막 사용자가 해제할 때 버스 유휴를 확인하고 클록·도메인·레일을 순정 순서대로 종료한다. 참조 수가 남으면 전원을 유지하고, 잘못된 참조 수와 재초기화를 거부한다. 버스 대기에 시간 제한을 두며 실패한 단계 뒤에는 확보한 자원과 객체를 보존한다. `MaliMfgPowerSequenceReady`는 살아 있는 전원·클록·DCM 상태를 재확인한다.

`MaliMfgPmicRead16`/`Write16`은 MT6789 WACS2의 실제 MMIO 명령·데이터·응답 확인을 구현한다. 16비트 짝수 주소를 검사하고, 남은 응답 정리·쓰기 완료 대기·응답 확인 후 출력 확정·10ms 제한과 실패 후 재사용 차단을 처리한다. UEFI Boot Services에 의존하지 않는다. 부모는 동작 중인 PWRAP 클록과 호출 전체의 전역 단독 사용권을 제공해야 한다.

`MaliMfgRailSet`은 순정 PMIC 표의 VGPU·VSRAM_GPU enable/status/selector/DA 레지스터를 사용한다. DT의 200/240µs 켜짐 대기, 실제 전압 읽기, VGPU→VSRAM_GPU 시작과 역순 종료를 구현했다. 켜져 있던 레일은 빌려 쓰고 해제할 때 끄지 않는다. 켜기 명령 전 소유권을 기록하므로 시간 초과나 일부 성공 후에도 전원과 객체를 보존한다. 전압 선택·모드·VS2 부모는 변경하지 않는다.

`MaliMfgHwClockSet`/`Parent`는 실제 MT6789 MFGPLL 전원·격리·활성화와 1/1/20µs 대기, 기준 클록 gate, MFG 부모 mux, BG3D gate를 제어한다. 공유 MAINPLL은 읽기만 한다. 기존 PCW·분주 값과 기준 클록 선택은 유지하고, 전압에 대한 호출자 확인과 1.5~3.8GHz VCO·부모 상태를 검사한다. 주파수 계산은 순정 26MHz 기준의 올림 규칙과 같다. 갱신 레지스터가 없는 MFG mux에는 갱신 쓰기를 하지 않는다. 해제 콜백도 실패를 반환하며, 실패한 자원은 확보 상태로 남겨 둔다.

`MaliMfgRailsVoltage`는 VGPU/VSRAM을 순정 순서로 함께 변경한다. 올릴 때 SRAM→GPU, 내릴 때 GPU→SRAM을 쓰고, 상승 8mV/µs·하강 4mV/µs에 각각 GPU 9µs/SRAM 8µs를 더한 순정 대기를 사용한다. 전압 범위·6250µV 간격·변경 방향을 검사하며 기존·중간·최종 전압 쌍 모두에 보드 허가가 필요하다. 두 레일의 enable/status/selector/DA가 맞아야 저장 전압을 확정하고, 일부 성공 뒤에는 전원을 보존한다.

`MaliMfgHwClockProgramRate`는 기준 클록을 확보하고 BG3D 차단·Sub 선택·MFGPLL 꺼짐/격리를 확인한 뒤 PCW/분주/변경 비트와 20µs 대기를 수행한다. 순정 표의 nearest-PCW 플래그를 반영하며 요청값과 반올림한 실제 주파수를 모두 보드가 승인해야 한다. 켜진 PLL 변경과 공유 MAINPLL 제어는 이 함수의 범위가 아니다. 원본 기본 OPP 45개는 검사 입력이며 기기별 보정이 끝난 동작 표로 사용하지 않는다.

`MaliMfgOppBuild`는 호출자가 제공한 내림차순 보정 OPP 표의 전압 범위·간격·단조성·도달 가능성과 실제 PLL 주파수를 검사하고, 순정의 200mV 전압 차이 한도로 상승·하강 중간 단계를 계산한다. `MaliMfgOppPlan`은 상승 시 전압→주파수, 하강 시 주파수→전압 순서를 만들며 같은 전압 구간도 보존한다. 잘못된 표·겹친 버퍼·진행하지 않는 단계는 출력 변경 전에 거부한다.

`MaliMfgDvfsInitialize`는 현재 실제 전압·클록이 시작 항목과 일치해야 상태를 만든다. `MaliMfgDvfsCommit`은 GPU 유휴·자원 점유·열/배터리 제한에 대한 부모의 명시적 허가를 받은 뒤 기존 PMIC 전압 쌍 코어와 클록 코어를 연결한다. 속도 변경은 BG3D 차단→Sub→PLL 정지/격리→설정→Main→BG3D 복귀 순서다. 모든 단계와 최종 상태를 확인한 뒤 현재 항목을 확정하며 일부 실패 시 목표·완료 단계·레일/클록 소유권을 남기고 재사용을 차단한다. 실제 Windows 보드 제공자와 작업 스케줄러 연결은 별도다.

`MaliMfgControllerInitialize/Set/Ready`는 위의 실제 WACS2·레일·SPM 도메인·기준 gate·PLL/부모/BG3D 코어를 하나의 제품 제어기로 연결한다. 호출 전체에 공유 자원 단독 사용권을 유지하고, 레일을 켜기 전에 실제 PLL 주파수와 두 selector 전압의 기기별 OPP 승인을 요구한다. 켠 뒤 전체 상태와 OPP를 다시 검사하며, 끄기 전 빌린 레일의 원래 전압 복원을 먼저 확인한다. 반복 켜기는 참조를 늘리지 않고 같은 상태를 재확인한다. 꺼짐/불명확 상태의 Ready는 MMIO를 하지 않는다. 부분 실패 후 각 코어의 확보/소유 상태를 유지하고 재사용을 차단한다. OPP 승인이 철회돼도 안전한 종료는 허용하되 자원 소유권 승인은 필요하다. 16개 레일/기준 클록 조합에서 48회 시작/종료와 9가지 부분 실패·정책 거부·빌린 전압 복원 조건을 검사했다. Windows 인터페이스·공유 버스 사용권 공급자는 별도 연결 대상이며 이 제어기를 만들었다는 이유로 보드 제공자 완료를 표시하지 않는다.

부모의 OS 자원 제공자는 구현했다. PRM에 참여하지 않는 펌웨어·보안 영역과의 PWRAP/클록 접근 배제·기기별 보정값 읽기·온도/배터리 제한 정책, Windows에서의 전체 `PowerReady`와 절전 복귀 검증은 남아 있다. 순정 ARM64와 도메인 128가지·클록 155가지·전체 전원 순서 66가지·WACS2 통신 256가지, 실제 PLL/mux/gate 함수의 켜짐·꺼짐 320가지와 주파수 계산 1,024가지, PLL 설정 308가지와 전압 쌍 변경 1,981가지를 대조했다. ASan/UBSan 및 ARM64 PE에서 각각 1,163,691개 검사를 통과했다. 순정의 순수 중간 단계 블록 256바이트와 전환 함수에서 표 6개·항목 121개·전환 계획 4,465개·실패 시 중단 11가지를 대조했다. 이 계획 대조의 전압/주파수 호출은 가상 서비스이며 실제 PMIC·PLL 코어 검사는 앞의 독립 대조에 해당한다. 전원 순서에 실제 PMIC·레일·클록 코어를 함께 연결해 기존 레일 보존과 실패 후 소유권도 검사했다. ARM64 통신·클록 함수의 Linux 폴링·MMIO·regmap·지연·알림 경계는 가상 서비스이며 실제 PMIC와 장치는 실행하지 않았다.

`MaliMfgOppCalibrate`는 순정 MT6789 probe의 칩 등급·MCL50 상한, 세 AVS 기준점, 정수 보간과 6.25mV 올림, 항목별 한 번의 노화 보정을 구현한다. 등급 1이거나 MCL50이면 상위 일곱 항목을 제외한다. 원본 ARM64 블록 세 개와 합성 입력 288조합·정상 항목 6,360개를 대조했다. 보정 후 역전되거나 연결 불가능한 전압 표 128조합과 기준 주파수가 다른 세 입력은 설치 전에 거부한다. 테스트용 퓨즈 기본값을 제품 정책으로 채우지 않으며 실제 기기 보정 영역은 읽지 않았다. 이 순수 계산과 기기의 조건 확인·열 제한은 별도다.

## 검사와 산출물

builder에서 `python3 Windows/MtkMali/build.py`를 실행한다. 공용 빌드 잠금 안에서 공용 코어/스케줄러의 ASan·UBSan 검사, 순정 ARM64 순수 함수와 미리 확보한 가상 메모리의 레지스터 표 초기화 비교, 실제 WDK 형식으로 만든 ARM64 검사 PE 실행을 수행한다. 결과와 원본 해시는 `Windows/Build/mali-verification.json`에 기록한다.

주요 산출물은 `MtkMaliCore.lib`의 ARM64 COFF 18개, 미서명 `MtkMaliMiniport.sys`·`MtkMaliBoard.sys`·장치별 `MtkMaliBoard.inf`·`MtkMaliSoc.inf`와 `MaliWddmTests.exe`, `MaliMemoryTests.exe`, `MaliExecutionTests.exe`, `MaliDdiTests.exe`, `MaliCommandsTests.exe`, `MaliPrepareTests.exe`, `MaliAllocationTests.exe`, `MaliVmTests.exe`, `MaliResidencyTests.exe`, `MaliAdapterTests.exe`, `MaliRuntimeTests.exe`, `MaliMfgTests.exe`, `MaliMiniportTests.exe`, `MaliBoardTests.exe`, `MaliHostTests.exe`, `MaliSocTests.exe`, `MtkMaliUmd.lib`·`MtkMaliUmd.dll`·`MaliUmdTests.exe`다. Ddi PE는 실제 DDI·작업 스레드 코드를 실행하고, Prepare PE는 실제 명령 검사/준비·DDI·스케줄러를 연결해 실행한다. 검사에는 다단계 순서·컨텍스트 수명·잘못된 입력·선점·완료 통지 도중 선점 요청·fence 순환·오류/리셋·통지 실패 재시도·재시작 실패·별도 명령 사본 주소·스레드 생성/대기 실패가 포함된다.

새 명령 검사기는 ASan/UBSan과 ARM64 PE에서 각각 47,638개 검사를 통과했고, 기존 Mesa arch-9 XML의 독립적인 필드 조합 6,689개(기존 192개·compute 256개·fragment 80개·clear 3,857개·shader program 1,024개·정점/타일 혼합 작업 1,024개·정점/fragment 생성 256개)와 사본의 바이트가 일치했다. 준비/제출 연결은 54,175개 검사를 통과했다. 준비 중 사용자 원본 변경, 일부 참조 고정 후 실패, 겹치는 사본 주소, 통지 재시도, 33개 준비 자원 유지, 선점과 리셋 후 재시작 실패를 포함한다. 제품 함수 344개의 고정 스택 프레임 중 최대는 DriverEntry의 1,584바이트이며 명령 검사기는 416바이트, 정점/fragment 생성기는 640바이트, clear 생성기는 112바이트, 준비 함수의 최신 프레임은 빌드 보고서에 기록한다. 함수 이름을 소스별로 구분해 같은 이름이 다른 검사 결과를 덮어쓰지 않게 했다.

할당 PE는 실제 할당/공유/해제와 DDI·스케줄러 연결에서 113,578개 검사를 통과했다. 4,096개 할당·열기·참조의 한도, 각 할당 실패 위치의 정리, 읽기 전용 공유와 생성 프로세스 종료, 실제 완료 통지 실패/리셋 실패 때 참조 보존을 포함한다. 할당 경로의 제품 스택 프레임은 최대 144바이트다.

VM PE 10,345개, Residency PE 234,858개 검사를 통과했다. OS 표 초기화/퇴거/재사용, 실제 MDL 코드의 실패 정리, PFN 불일치·읽기 전용·물리 주소 겹침 거부, 완료 통지 실패·선점 통지 실패·리셋 실패 때 보존을 검사했다. 33개 제출, 17페이지에 걸친 64KiB 입력과 64개 쓰기 대상, 서로 다른 최상위 주소 영역의 전용 표 196개도 실행했다.

기능 조회 PE 29,310개와 시작/종료 PE 69,175개 검사를 통과했다. 부족한 출력 크기와 지원하지 않는 요청의 출력 보존, 단계별 메모리 영역 조회, 기능 참조와 협상 중 재진입, 실제 완료 관찰에 따른 엔진 응답, 시작 실패 정리·참조가 남은 정리 실패·스레드 대기/닫기 재시도를 포함한다. 실행 묶음에서 실제 할당/PTE/MDL/명령 준비/제출/통지 실패 후 보존과 재시도 해제까지 연결했다.

미니포트 PE는 실제 등록 호출·PnP·코어/실행 묶음·전원 전환 코드에서 323,345개 검사를 통과했다. 잘못된 자원의 MMIO 전 거부, 대기 중인 인터페이스 질의, 살아 있는 OS 객체의 중지 차단, IRQ 세 우선순위와 DPC, GPU 오류/실제 리셋 뒤 D0, 실패한 스레드 종료·호출 대기·보드 해제·매핑 해제·잘못된 인터페이스의 자원 보존과 재시도를 포함한다. 같은 실제 할당·MDL·VM 루트·작업 문맥을 유지한 D0/D3 3회와 이후 제출/완료, 보드 전환 부분 실패·정보 불일치·호출 종료 대기·IRQ/스레드/캐시/리셋 실패 재시도, 전원이 꺼지거나 불명확한 상태의 직접 MMIO 차단과 GPU 접근 없는 Stop도 포함한다. `DxgkInitialize`와 보드/OS 경계는 대체하므로 Windows 등록·동시 실행·물리 IRQ 연결 결과가 아니다.

MMIO, 커널 할당·MDL 서비스·캐시·잠금·스레드/이벤트와 Dxgkrnl 콜백은 검사 경계에서 대체한다. Vm/Residency PE는 실제 VM/메모리 관리 코드를 실행한다. Windows 예외 처리는 실제 SEH로 컴파일하지만 검사에서 OS 예외 전달은 실행하지 않는다. Ddi/Allocation PE의 명령 준비 대역과 Prepare PE의 실제 준비 코드 실행을 구분한다. 실제 Windows 스케줄링·동시 실행과 GPU는 실행하지 않는다. assertion 수에는 MMIO 접근·대기 반복과 초기 메모리 바이트 검사도 포함된다. 개수만으로 기능 범위나 하드웨어 성공을 판단하지 않는다.

VM 명령 채널 장애 동안 같은 builder 원본을 macOS에서 읽어 Windows ARM64로 교차 빌드했다. 임시 Zig 링커와 Unicorn을 사용했으며 소스 사본은 만들지 않았다. 일반 builder 경로는 기존 Clang/LLVM·WDK·Python 환경을 사용한다.

Linux PanVK 빌드와 arch-9 셰이더 생성 결과는 [PanvkBuild.md](PanvkBuild.md)에 있다. 이 Linux 라이브러리는 Windows ICD가 아니다.

## 남은 DirectX 구현

실제 Windows 등록·패키지와 보드 전원 제공자/ACPI GPU 열거·구현한 조회/실행 묶음의 실제 OS 연결, tiler/vertex 파이프라인·일반 그리기를 위한 그래픽 자원/설명 정보 변환·실제 셰이더/간접 참조 동작 검증, 물리 IRQ 자원 연결·보드 전원·절전 복귀, 구현한 자원/RTV/clear 구성 요소를 잇는 완전한 D3D 사용자 드라이버 또는 Windows Vulkan ICD/WSI, DXIL 변환·그래픽 자원/파이프라인·화면 연결이 남아 있다. 보드 하위 필터 SYS/INF는 위에서 빌드했으며 설치 가능한 전체 그래픽 패키지와 하드웨어 DirectX 가속은 아직 완성하지 않았다.

compute·fragment 자원 연결을 포함한 실제 ARM64 메모리/제출 코드 234,858개 검사가 통과했다. fragment는 1~8개 대상과 깊이/체크섬 확장 16조합에서 제출·주소 변환·입력 변경 후 사본 보존·캐시·완료 해제를 확인했다. 잘못된 범위·권한·PFN·미지원 작업은 제출 전에 거부하고, GPU 오류와 실패한 리셋 뒤에는 참조를 유지했다. 별도로 만든 17×19 2D 출력 할당과 256KiB 힙을 실제 clear 생성기에서 받아 제출·완료·캐시·오류/실패한 리셋 후 보존까지 네 형식 각각에서 검사했다. 66개 자원 페이지의 물리 주소와 쓰기/실행 권한도 대조했다. 출력 픽셀이나 실제 fragment 셰이더는 실행하지 않았다. 1~64페이지 범위, 2MiB·1GiB·512GiB 경계, 두 번째 페이지의 PFN/권한/오프셋 불일치, 제출 간 물리 쓰기/실행 충돌, 부분 실패 정리, GPU 오류·실패한 리셋 후 보존을 포함한다. 입력/셰이더의 사전 캐시 정리와 출력의 완료 후 정리를 MDL별로 확인했다. 512MiB는 허용 상한과 해시 용량 검사이며, 실제 512MiB GPU 작업 실행을 뜻하지 않는다.

전원 인계 때는 `MaliMfgRailsReleaseReady`로 확보 전부터 켜져 있던 레일의 원래 전압을 확인한다. 확보 시 원래 값을 한 번 보존하고, 두 레일 중 하나라도 원래 전압과 다르면 첫 번째 레일도 놓지 않는다. 실제 selector·DA·켜짐 상태까지 다시 읽어 확인한다. 원래 켜진 레일과 새로 켠 레일이 섞였을 때도 복원에 필요한 두 레일을 유지한다. 복원은 승인된 전압/OPP 경로로 수행해야 하며 이 함수가 임의로 전압이나 주파수를 바꾸지 않는다. 보드의 전체 OFF 승인에서도 이 사전 검사를 사용해야 한다. 상태 불일치는 재시도 가능한 Busy, PMIC 통신 실패는 사용권을 보존하는 오류로 처리한다. Windows D3 절전, 충전/PD, 배터리 계측과 열 제한 정책의 구현을 대신하지 않는다.

`MtkMaliRuntimeSuspend`와 `MtkMaliRuntimeResume`는 PnP 종료와 별도다. 모든 제출·완료 통지·MDL 사용이 끝난 시점에 새 작업을 원자적으로 막고, IRQ를 가린 뒤 연결 해제·작업 스레드 종료·캐시 정리·코어 중지를 수행한다. 프로세스·장치·작업 문맥·공유 메모리·VM 표와 기능 인터페이스 참조는 유지한다. 절전 진입 중 DISPATCH 취소 요청이 먼저 들어오면 요청을 잃지 않고 절전을 거부한다.

부모가 실제 보드 전원을 복구한 뒤 Resume를 호출하면 코어를 다시 시작하고 원래 GPU ID·MMU·주소 공간·작업 슬롯·셰이더/타일러/L2·작업 기능 정보와 일치하는지 확인한다. 작업 스레드와 IRQ 준비가 끝나야 제출을 다시 허용한다. 복귀 실패는 새 작업을 계속 막고 정리를 재시도하며 자원을 보존한다. 전원이 꺼진 동안의 장치 정보/노드 조회는 저장한 정보만 사용하고, 엔진 리셋과 제출은 MMIO 접근 전에 거부한다. 절전·복귀 3회 뒤 동일한 문맥으로 다시 제출·완료하는 경로, 살아 있는 VM/할당/인터페이스 보존과 IRQ·스레드·캐시·코어 재시작·장치 정보 불일치 재시도를 실제 ARM64 코드의 모의 실행으로 확인했다. Runtime 묶음은 69,175개 검사를 통과했다. Miniport D0/D3 콜백과 MMIO 호출 종료 확인은 위 경로로 연결했다. 최종 OS 호출 수명, 실제 보드 전원·깨우기·배터리·충전·열 정책과 실기기 절전은 남아 있다. [Microsoft 전원 상태 콜백 문서](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dispmprt/nc-dispmprt-dxgkddi_set_power_state)를 참고한다.

DDI가 알리는/허용하는 명령 전용 데이터 크기는 v2 자원 목록의 최대 2,320바이트와 일치한다. 실행 단계 표의 옛 크기 272바이트를 넘는 352바이트·20개 자원 목록을 실제 DDI·명령 준비·MDL/전용 MMU·제출·완료 경로로 검사했다. 셰이더 명령어의 GPU 실행을 포함하지 않는다.

2026-11-13 공용 PMIC 연결: 실제 ARM64 PRM 클라이언트 18,249개와 5영역 SoC 131,876개 조건이 통과했다. v1 GPU·TOPSTATUS·읽기 전용 키 snapshot 호환을 유지하고, 별도의 v2 전원 키 enable 사용권·캡처 보존·제한된 W1C·원래 비트 복원 API를 연결했다. v2 원시 키 레지스터 쓰기는 허용하지 않는다. Windows 버튼 드라이버도 같은 실제 클라이언트로 빌드했다. 모듈 잠금 해제 후 호출권을 놓기 전에 사용권·캡처와 출력값을 확정하며, 처리 여부가 불확실하면 재시도를 차단한다. 전체 검사와 SYS 해시는 Windows/Build/mali-verification.json, 펌웨어 코어 1,101개와 최종 PRM 71가지의 근거는 analysis/physical-buttons-edk2-verification.json·analysis/boot-verification.json을 따른다. 실제 Windows 잠금·OS 전원 전환·GPU·물리 PMIC 실행은 미실행이다.
