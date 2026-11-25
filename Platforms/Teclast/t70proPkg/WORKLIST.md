# T70 Pro: 검증 근거 기반 작업 목록

갱신: 2026-11-23. 최초 장치 목록 기준: 메인 `497acab`, ACPI gitlink `896190a`.
아래 최신 VM 검증·추가 커밋은 별도 명시하며, 최초 목록 전체를 최신 판정으로 간주하지 않는다.
진행 중인 작업트리 변경은 커밋/검증 결과가 확인된 뒤 이 파일에 반영한다.
이 파일이 보드 작업 목록의 단일 원본이다. 완료 보고 사본을 따로 만들지 않는다.

## 목표와 검증 규칙

최종 목표는 **완전한 2-in-1 태블릿과 듀얼 부팅**이다. 기본 stock Android를 보존하고 Windows ARM64를 선택할 수 있어야 한다. ARM64 Linux live는 진단 경로, XNU는 후속 대상이다. 부팅 성공은 시작점이며 아래 사용 시나리오가 완료 기준이다.

1. USB 입력·외장 저장장치 경로를 우선 확인하고 WinPE 및 ARM64 Linux 진입 기반을 갖춘다.
2. 이후 통신 → 그래픽 가속 → 센서 → 소리 순으로 실제 장치 연결을 완성한다.
3. XNU는 장기 후속 대상이며 현재 부팅 지원을 주장하지 않는다.

소스 존재, DSC/FDF 포함, 호스트 모의 테스트, ARM64 컴파일, 최종 FD 포함,
실기 동작은 서로 다른 단계다. `_STA=0x0F`도 Windows 드라이버 성공의 증거가 아니다.
실기 RAM 부팅·재부팅·플래시는 사용자가 따로 요청할 때까지 수행하지 않는다.
기존 안전 검토에서 거절된 소스 조사/호출을 재시도하거나 다른 경로로 우회하지 않는다.

## 2-in-1 사용성 수용 기준 (미완료 항목)

Windows와 ARM64 Linux를 각각 판정하며, mock/컴파일 통과만으로 체크하지 않는다.

- [ ] stock Android 기본 부팅을 보존하면서 Windows ARM64를 선택하는 실제 지속 가능한 native 듀얼 부팅을 구성한다. 임시 Boot entry·QEMU EFI Shell은 이 완료 기준과 다르다. 현재 파티션 크기 변경·플래시는 요청되지 않았고 이 문서가 OS 진입 구현을 대신하지 않는다.
- [ ] 키보드·트랙패드와 터치가 함께 동작하며 분리/재연결 시 입력을 잃거나 눌린 상태로 남지 않는다.
- [ ] 태블릿/키보드·도크 전환, 화면 회전과 터치 좌표 변환, 덮개 이벤트가 일관된다.
  자동 회전은 실제 IMU 실장·버스 증거 확인 후 구현한다.
- [ ] 절전/복귀 후 화면·입력·저장장치·통신이 회복되고 stale readiness로 장치를 재사용하지 않는다.
- [ ] Wi-Fi 재연결, Bluetooth 주변기기, LTE 데이터/SIM 및 GNSS/A-GPS가 실제 공급자와 연결된다.
  A-GPS 보조 데이터와 위치 계산 엔진은 NMEA 파싱·UMDF 요청 처리와 별도다.
- [ ] 충전 상태·배터리 잔량/경고가 실제 측정 공급자로 연결되고 도크 연결·해제와 모순되지 않는다.
- [ ] 디스플레이·소리·외장 저장장치 등 일상 사용 경로를 OS별로 검증한다.
- [ ] 펜: 디지타이저/프로토콜 등 **실제 지원 하드웨어 근거부터 확인**한다.
  일반 정전식 터치만으로 능동펜·필압 지원을 주장하지 않는다.

## 우선순위와 다음 작업

| 우선 | 장치/기능과 확인된 식별자 | EFI 상태 | Windows 상태 | 다음 구현·검증 / 담당 |
|---|---|---|---|---|
| P0 | USB 키보드·외장 저장장치·부팅 파일 | EFI QEMU의 초기/유지 전원 부팅, 키 입력, 같은 부팅의 저장장치·키보드 3회 재연결, 파일 실행과 EBS 인계 통과. PC 모드는 호스트와 온보드 허브를 유지하며 GPIO10 변화 뒤에도 HCD·USB_IO·파일시스템 핸들이 유지된다. | MtkUsb는 VBUS 손실을 UCX root-hub disconnect/change로 전달하고 URB를 막는다. VBUS 복귀 후 hub reset까지 전송을 거부해 재열거한다. 보존한 TPHY ACR 튜닝과 새 ACPI clock/route/Type-C source 증거가 유효한 D0 복귀에서는 컨트롤러와 TPHY가 함께 끊긴 경우도 비동기 cold host 절차로 다시 시작한다. 시작 전 IRQ 마스크, 역할·경로·클록 재확인, 실패 시 소유 자원 정리를 ARM64 모의로 검사하고 unsigned SYS 빌드 통과 | 실제 OTG·키보드·저장장치·원격 깨우기와 Windows 로드는 미확인. PD source/sink 협상·device-role 전환은 지원하지 않는다. cold D0 복구는 이전에 저장한 아날로그 튜닝과 매번 읽은 유효 경로/클록 증거가 필요하다. |
| P0 | SD, stock `mmc@11240000`, MSDC1, ACPI `HANA6792` | MSDC+SdDxe, 읽기 전용·25 MHz. QEMU에서 카드 제거·재삽입 후 EFI 파일 원본 일치와 쓰기 거부 확인 | SDPORT 읽기·쓰기, GPIO7 both-edge 알림, 제거 중 요청 종료와 재삽입 후 I/O 전 전체 초기화. ARM64 링크 및 모의 검사 142,750개 통과 | OS가 실제 GPIO 알림을 받고 카드를 재검색하는지, 전원 손실 복귀는 미확인. |
| P0 | UFS, `0x11270000`, 내부 LU2, ACPI `HANA678B` | handoff→PassThru→SCSI→읽기 전용 BlockIo 포함. 실기 파티션 열거는 사용자가 확인했다. 가상 LU2의 EFI 파일 원본 일치 및 쓰기 거부 확인 | Storport·PRM 전원 필터 ARM64 빌드 및 콜백 모의 통과. 실제 OS 동작 미확인 | 저장장치 담당: 다음 허가된 실기에서 파일 내용 읽기와 오류 처리 확인. 현재 관찰을 오류/고장으로 해석하지 않음 |
| P0 | 화면 GOP | LK/DT framebuffer 인계 검증 후 GOP 게시. 냉간 화면 초기화의 증거 아님 | GOP와 Windows DOD/GPU 가속은 별개 | EFI 담당: 현재 산출물 포함/콘솔 연결 증거와 WinPE·Linux 화면 인계 조건 정리. 금지된 GPU 조사 재개 금지 |
| P0 | MT6366 내부 MT6358 호환 RTC, PWRAP, base `0x0588` | 순정 근거 기반 부팅 중 GetTime/SetTime 및 조건부 EBS 이후 달력 GetTime/SetTime 구현. 전용 호스트 검사에서 boot read 1,115,477개·write 1,351,655개, runtime 21,688개 통과. Alarm 미지원; 정확도는 보정되지 않아 MAX_UINT32 반환 | 실제 Windows 및 RTC 실기 미실행 | 실제 날짜/요일 쓰기·재부팅 유지와 정확도 근거 확인 |
| P0 | GPIO/EINT, ACPI `HANA678A` | GPIO 프로토콜·보드 공급자와 GRDY 인계 | GpioClx가 절전 전에 소유 IRQ를 가리고, 핀 설정과 AP IRQ 도메인이 유지된 경우 원래 마스크를 복원한다. 대기 중인 인터럽트 보존과 복귀 실패 뒤 재시도 모의 확인 | 실제 덮개·PMIC IRQ와 완전한 전원 손실 복구, Windows 깨우기 핀 연결 남음 |
| P0 | I²C, ACPI `HANA6789`, I2T0/I2T1/I2C6 | I2cDxe와 보드 공급자가 3개 버스의 준비 상태와 클록을 인계 | SpbCx 드라이버가 IRDY·IRCK를 확인하며 정지한 버스는 IRRS로 해당 클록 게이트만 복구한다. 복귀 뒤 전송까지 ARM64 모의 확인 | 부모 PLL/APDMA/핀 상태까지 사라진 뒤 복구와 물리적으로 걸린 선 복구 남음 |
| P0 | Himax HX83132 터치, I²C `0x48`, IRQ GPIO9, ACPI `HANA8313` | AbsolutePointer와 HMXR 인계. release 전달 수정 `497acab` | D0 복귀에서 HMRD가 현재 리셋·IRQ 핀을 읽고 VHF 드라이버가 SPB 칩 ID 및 펌웨어 실행 상태를 확인한다. ARM64 모의 확인 | 전원 손실 뒤 전체 sense-off, 펌웨어 업로드와 실제 터치·IRQ 검증 남음 |
| P0 | 배터리·충전, MT6366 게이지, SC8989x, ACPI ACPI0003 | 순정 환산으로 순간 전류·전압·온도를 읽고 AC 전원 상태를 인계 | ACAD _PSR와 GPIO12 통지. PWRAP broker v5는 FG_CTRL/FG_CURRENT와 고정 AUXADC 전압·배터리 온도·칩 온도·BIF 채널을 gauge lease 안에서 처리한다. BIF raw 경로는 disable bit를 보존·복구한다. MtkMaliPmicPrmReadBatteryVoltageMv는 순정 OTP/eFuse 행 0x50·0x52·0x54·0x56과 BATADC/CHIP_TEMP 환산을 한 gauge lease에서 처리하며, OTP read-enable·클록 게이트 복구를 다시 읽어 확인한다. MtkMaliPmicPrmReadBatterySnapshot은 보정 전압·온도·전류와 원시 샘플을 한 lease에서 함께 수집한다. 배터리 계측 ASan/UBSan 검사 118,709개, broker 검사 2,051개와 ARM64 unsigned MtkButtons.sys 링크(31,744 bytes)를 통과했다. 공유 PMIC broker를 포함한 RELEASE EDK2 펌웨어도 4 MiB FD와 부팅 이미지를 생성했다. 실제 배터리 값은 미실행이다. 순정 DTB mtk-gauge에는 온도별 6개 배터리 프로필(각각 100×3 셀), 게이지 저항 70, 튜닝값 101, 보정 기준 전압 3400이 있다. Windows/MtkBattery는 ACPI HANA679B의 KMDF Battery Class 미니포트로 등록되며 표준 정보·상태·온도·예상시간·WMI 질의와 상태 알림 콜백을 제공한다. 정적 곡선 초기 SOC 후 순정 전류 단위로 소프트웨어 적산한다. 아직 WinPE 패키지 포함·Windows 로드·충전/잔량 실기 확인은 없다. | Windows Battery Class 조회/알림 경로는 구현·ARM64 링크됨. PMIC 현재 단위와 순정 ID0 5×100 프로필을 반영했으며 소프트웨어 적산은 순정 하드웨어 CAR 기반 알고리즘과 같지 않음. 커널 빌드의 CONFIG_MTK_ADDITIONAL_BATTERY_TABLE 설정이 체크아웃에 없어 active table 개수는 미확인. 배터리 부재 판별, 장기 보정/학습, WinPE·설치 이미지 패키징과 Windows 로드도 남음 |
| P0 | 온도, AP NTC 2개와 LVTS 13개, ACPI `HANA6794` | `\_TZ.SOC0`를 NTC0 온도 공급자에 연결. 순정 SoC 위험 온도 113.5°C | 보드/LTE 온도 및 설정이 유지된 LVTS를 읽어 Windows 온도 요청에 응답한다. ARM64 모의 통과 | LVTS 전원 손실 뒤 초기화·보정·HW 보호 설정과 실제 Windows 열 정책 미확인 |
| P0 | CPU, MT6789 A55 6개·A76 2개 | MADT/PPTT와 순정 DT의 주파수 도메인 2개 대조. CPU ACPI 노드는 CPFM 준비를 _DEP로 기다린다. | HANA6799 system-start PEP가 ACPI0007 CPU UID를 A55/A76 두 PPM 도메인에 연결하고, 검증된 OPP 요청을 SRAM index에 반영한다. DPM 시작·D0 복귀 통지와 system-resume 뒤 OPP 캐시 무효화도 처리한다. 1,638개 ARM64 PEP/OPP 콜백 검사와 18개 ASan/UBSan 코어 검사 통과, unsigned 21,504-byte SYS 링크 | 순정 전압·지속 성능 제한의 근거는 미확인. 실제 Windows PPM 로드·클록 전환은 실기 작업 범위에서 제외 |
| P1 | LTE/WWAN, DPMAIF·CCCI CCIF HANA6797·HANA6798 | 순정 DT의 AP MMIO·IRQ에 맞춰 ACPI 열거 자원 포함 | NDIS miniport INF/SYS package targets ACPI HANA6797. MiniportInitializeEx validates raw/translated _CRS, maps four MMIO windows plus the fixed 64-byte CCCI handoff, initializes the 36-bit WDM DMA adapter, and allocates/zeros separate 64-byte-aligned PIT/BAT/DRB buffers totaling 851,968 bytes and builds the source-ordered combined startup plan only when LK reports no MD1 image-load error; this does not prove modem power, clocks or runtime boot are ready. WWAN driver-model and device-capabilities OIDs return NOT_SUPPORTED until the full modem path is implemented; transmit remains disconnected. ASan/UBSan checks: CCCI/CCMNI 236, descriptor 73, ring 84, BAT 71, TX 66, SG normalization 15, ACPI resource-map checks 22, CCCI handoff bounds 13, separate-table layout 32, and source-locked DPMAIF startup-plan/fake-MMIO executor checks 139, CCCI CCIF send/ack checks 17, and HANA6798 CCIF resource-map checks 16. NDIS package check passed; unsigned ARM64 MtkWwan.sys 28,672 bytes. Build records: Windows/Build/wwan-rx-bat-vectors-20261123/mtkwwan-verification.json; Windows/Build/wwan-separate-buffers-20261123/CcciRing-verification.json (ARM64 objects: DPMAIF planner/executor 18,120 bytes and table layout 2,652 bytes, CCCI CCIF core 968 bytes, HANA6798 resource mapper 1,928 bytes). No Windows load or device execution. | Source mapping follows kernel-reference commit a71515dbaf0d539ffb36416f14186dad17eef743 and the source-locked DPMAIF v3 register order. Remaining: HANA6798 FDO/IRQ binding; the NDIS miniport allocates/zeros separate ring-table common buffers and precomputes the full startup plan when LK reports no MD1 image-load error; this flag does not attest to a live modem. It then allocates request/fragment common-buffer vectors, validates their 128-byte alignment and 36-bit DMA range, initializes the BAT ownership slots, and posts count-minus-one entries to each BAT table. It does not apply the MMIO plan, register DPMAIF/CCIF interrupts, refill or reclaim buffers, or indicate packets.  NDIS interrupt/DPC integration, modem power/boot, CCCI control-plane connection, TX/RX indications, WWAN OIDs and packet connections also remain. The WWAN model and device-capabilities OIDs are deliberately unsupported until the complete modem path exists; LTE data/IMS/voice are not implemented. |
| P1 | Wi-Fi, `/soc/wifi@18000000`, `HANA6796` | ACPI에 WLAN/conninfra 공용 영역을 한 번만 선언하고, WCN0가 DT의 BTCVSD GSI 439와 WLAN GSI 441을 각각 소유 | MtkConn WCN0 펌웨어/ROM 초기화 소유자와 ACPI HANA6796 INF 구현. WCN bootstrap 검사 65,707개 및 unsigned ARM64 SYS 링크 통과 | WLAN 전송/NDIS 클라이언트, 라디오 정책, 네트워크 연결 미구현 |
| P1 | Bluetooth / 공통 BTIF·STP | EFI Bluetooth 구성 없음 | HANA6795 FDO/INF·BTHX 자식 PDO, HCI event/ACL 큐, full-mode WMT Bluetooth function-on과 응답 대기 구현. Owner 40,967·HCI 2,824 검사 및 서명 전 ARM64 SYS 링크 통과. WCN0 SCO 패킷 코어는 캡처한 IRQ control word와 SRAM 포인터를 검증해 RX/TX 배치를 안전하게 복사한다. MtkConn은 기존 SoC CONN power lease에서 stock INFRACFG 0xF00 bit 11을 읽고, WCN 시작 전 마스크·MCU 정지 후 원래 상태 복원을 읽기로 확인한다. SoC writer는 저장된 bit와 반대 상태만 허용하고, 복원 실패면 lease를 유지한다. Bootstrap 검사 65,707개, SoC 검사 133,310개, SCO 주소 검사 393,216개 및 6 profile 왕복이 통과했고 ARM64 WCN0 SYS가 링크됐다. WCN0는 level-high GSI 439를 WDF interrupt로 연결하고, 오디오 consumer가 없을 때 참조 BTCVSD 드라이버의 control 상태 비트만 ISR에서 ACK한다. T70 모듈의 ACK mask는 binary 대조 전이다. ScoPacket object는 ARM64 object로 컴파일됐지만 MtkConn.sys에 링크되지 않았고 packet transfer/HFP audio는 없음 | 현재 BTHX 응답은 `ScoSupportNone`/0채널이라 Microsoft 요구 조건을 충족하지 못해 스택 로드를 막는다. HFP bypass 오디오 드라이버는 GUID_DEVINTERFACE_BLUETOOTH_HFP_SCO_HCIBYPASS의 동적 도착/제거를 추적하고, wave/topology 하위 장치를 등록하며, KS 핀이 ACQUIRE로 갈 때 IOCTL_BTHHFP_STREAM_OPEN을 완료해야 한다. WCN0 SCO SRAM/IRQ 서비스와 MtkAudio WaveRT/HFP 경로가 연결되기 전에는 BTHX capability를 올리지 않는다. SCO packet transfer/HFP audio endpoint, T70 module ACK mask의 binary parity, 서명·OS 로드와 실제 RF 연결도 남음 |
| P1 | GNSS, 요청된 자식 ID `MTK\MT6789_GNSS` | EFI 위치 서비스 구성 확인 없음 | Windows UMDF2 위치 API 소비자와 요청·취소·세션 변환 구현. ARM64 DLL 및 콜백 검사 통과 | 자식 장치 열거, 실제 수신기 공급자, 위치 계산 엔진, A-GPS 공급자 미구현 |
| P2 | Windows 그래픽 가속 | GOP 상태와 분리 | 이번 범위에서 완성 여부 재판정하지 않음 | 그래픽 담당 지정 필요: 허용된 기존 결과에서 DOD/가속/절전 지원 범위를 분리. 과거 거절된 작업 재시도 금지 |
| P3 | MH248 Hall 덮개, GPIO0, ACPI `PNP0C0D` | 보드 검증 후 EBS readiness 공급 | 표준 ACPI lid + MtkGpio OpRegion/양쪽 에지 Notify 연결 구현 | 입력 담당: 실제 덮개 이벤트 확인은 별도 실기 승인 후. 전용 Hall INF가 없다는 이유로 누락 판정하지 않음. S3/S4 wake 완료 아님 |
| P3 | IMU·조도 등 기타 센서 | 순정 Linux 적재 계획에 sensorhub·SCP·IPI·hf_manager 포함. 원본 sensorhub가 SCP 공유 메모리로 센서 목록을 받고 hf_manager에 등록 | SCP 전송 및 Windows 센서 공급자 미구현 | 순정에서 받은 실제 센서 목록과 지원 기능을 확인하고 회전·조도 경로 연결 |
| P3 | 카메라, 순정 imgsensor·CP2155 | DT에 I²C 버스 `0x11eb0000/0x11eb1000/0x11eb2000`의 카메라 3개 경로, EEPROM `0x50/0x51`, AF `0x72` 및 CP2155 플래시가 선언됨. 원본 모듈에 7개 센서 후보 있음 | 실장 칩과 연결되는 Windows 센서/ISP/캡처 드라이버 미구현 | 실제 센서 ID 확인, 전원·클록/CSI·ISP 및 프레임 캡처 남음 |
| P4 | 오디오, AFE `0x11210000`, AW88261 두 개 `0x34/0x35` | 스피커 SSDT에 I2T1 연결과 AFE IRQ 선언 | PortCls StartDevice는 두 Resource Hub 타깃을 열고 AW88261 ID를 확인한다. 93개 생산 번역 단위를 공식 WDK 10.0.28000.2526/KMDF 1.35로 컴파일·링크하고 unsigned INF/SYS 패키지를 임시 경로에 스테이징했다 | 물리 자원 허가 게이트가 `STATUS_DEVICE_NOT_READY`를 반환한다. AFE/코덱 전원과 클록, DMA·IRQ 소유권, 설치·재생·녹음은 미완료 |
| 미래 | XNU | 현재 지원 주장 없음 | 해당 없음 | WinPE·ARM64 Linux 기반과 장치 계약 확립 후 별도 계획 |

## 확인된 완료 단위

- [x] `f801e49`: SD caller buffer wrap 거부 회귀 및 Mu_Basecore 갱신.
  Windows SD 드라이버 구현 완료나 최종 FD/실기 검증을 뜻하지 않는다.
- [x] `497acab`: Himax 소비된 press의 release가 다음 접촉에 덮이는 문제 수정.
  원본에서 신규 회귀 테스트 실패 확인, 수정 후 전용 ASan/UBSan 7,308 검사 통과.
  여러 Poll·오류 후 회복·WaitForInput·reset/stop/EBS 정리 포함. 전체 빌드/FD/실기 미수행.
- [x] `b9984e8`: Windows 오디오 cyclic start의 KMDF parent 소유권 관리.
  실제 SPB·오디오 출력 완료로 확대 해석하지 않는다.
- [x] USB 기존 QEMU EFI Shell 및 Windows 모의 결과 대조(위 P0 근거).
  QEMU EFI 실행은 OS/kernel 부팅이나 실제 보드 USB 성공을 의미하지 않는다.
- [x] 시험용 UEFI 재빌드 및 boot 이미지의 LZ4 내용과 4 MiB FD 일치 확인. ACPI 표 9개를 FD에서 추출해 대조하고 QEMU에서 실행 중 설치된 ACPI 표 10개를 확인했다.
- [ ] 사용자 별도 요청 이후에만 장치 RAM 부팅/실기 점검.

## 이전 소스 VM 검증 (2026-11-19)

기존 Ubuntu builder에서 공식 WDK/LLVM과 기존 Unicorn으로 개별 타깃만 재빌드했다.
결과의 입력 해시와 현재 소스를 대조했고 공통 build lock은 정상 해제됐다.
실제 ARM64 production callback을 실행하지만 WDF·장치 접근은 mock이다.
결과 루트는 Mac `~/.tmp/teclast-t70pro/`이며 소스 사본을 만들지 않는다.

| 단위 | 확인된 결과 | 결과 하위 폴더 |
|---|---|---|
| UFS | callback/handoff pass; assertion 수 미기록. 사용자의 EFI 파티션 열거 관찰은 정상 관찰로 유지 | `ufs-callback-vm-20261119-current-1/` |
| I²C | 2,697 callback assertions pass | `i2c-callback-vm-20261119-current-1/` |
| GPIO/Hall 의존성 | ARM64 callback pass + native GPIO/EINT 424,151 checks pass | `gpio-callback-vm-20261119-current-1/` |
| Himax | 2,417 callback assertions + native parser/mapper pass. HMRD 실제 공급자 완성을 뜻하지 않음 | `himax-callback-vm-20261119-current-1/` |
| Bluetooth 자원 owner | `409fb1c` VM SYS 링크·FxDriverEntry/import/checksum 검증. INF/기능 노출 없음 | `btif-owner-vm-409fb1c/MtkBtif.sys` |
| 오디오 | 담당 검증: DriverTest 843 checks 및 공식 WDK 93 units ARM64 SYS 링크, 현재 입력 해시 일치 | 별도 audio 담당 결과 참조; 실제 재생과 구분 |

Windows SDPORT 호스트에 읽기·쓰기와 GPIO7 카드 변경 알림을 연결했다. ARM64 모의 검사 142,750개와 SYS 링크를 확인했다. ACPI의 GPIO7 양쪽 에지 자원도 다시 검사했다. OS에서 GPIO 알림이 라우팅돼 카드가 재열거되는지는 확인하지 않았다.
`bd2c8f7`은 STP→BTIF zero-accepted Full을 정상 backpressure로 보존한다.
BTIF 223,944/STP 5,799,629 native ASan/UBSan 통과이며 실제 RF 연결이나 DMA 구현이 아니다.

## 이번 구현과 검사 결과 (2026-11-19)

시험 이미지는 `Build/t70pro-remaining-20261119-final/RELEASE_CLANGPDB/T70Pro-UEFI-Trial-boot.img`이다.
SHA-256은 `e7890b4b4e611a4d2cf4c2d33f02104af4b2702c8d9e7a80115e7b4fa52e7cc7`이다.
검사 경로는 Mac `~/.tmp/teclast-t70pro/remaining-yxan3Xnd/` 기준이다.

| 단위 | 확인한 내용 | 검사 경로 |
|---|---|---|
| ACPI | 실제 AML에서 USB 준비 82개·클록 복귀 42개, I²C 유지 상태 61개·클록 게이트 쓰기 및 거부 30개, HMRD 13개와 열 구역 연결 확인 | `acpi-i2c-resume-r2/verification.json`, `fv-i2c-final.json` |
| USB UEFI | 두 전원 조건의 키보드, 저장장치 재연결과 정확한 파일 읽기, 외부 EFI Shell 실행, EBS 인계. 유지 조건에서 GPIO 연결 경로 복구 뒤 파일을 다시 읽고, OS 인계 직전 ID가 바뀐 경우 인계를 지움 | `qemu-usb-retained-final/execution.json`, `qemu-usb-cold-final/execution.json`, `qemu-usb-id-changed-final/execution.json` |
| USB Windows | VBUS 이탈 시 pending endpoint 요청 종료, root-port disconnect/change 전달, 복귀 뒤 hub reset까지 I/O 차단, Type-C route recheck와 D0 retained-session resume. Root-port suspend/resume도 검증. ARM64 PE 콜백 모의 실행 통과; Windows/기기 load 미실행 | usb-worker-test/MtkUsb-driver-tests.json, usb-worker-package/ |
| I²C Windows | 3개 컨트롤러의 복귀 뒤 전송, 잘못된 응답과 새 자원에서 게이트 복구 거부. ARM64 콜백 모의 검사 3,357개 통과 및 SYS 링크 | `i2c-test-r3/request-tests.json`, `i2c-package/` |
| GPIO·터치 Windows | 유지된 GPIO/EINT 마스크 복원, 터치 펌웨어가 멈춰 있으면 VHF 시작 거부. ARM64 SYS 링크 및 콜백 모의 실행 | `gpio-test/`, `himax-test/` |
| 오디오 Windows | 스피커 두 채널의 리소스·SPB 연결·칩 ID 확인. WDK 93개 소스 단위 ARM64 링크 | `audio-host-macos-r2.log`, `audio-package/` |
| 온도·CPU Windows | 온도 임계값 대기·취소와 두 주파수 도메인 판독의 ARM64 콜백 모의 실행과 SYS 링크 | `thermal-test/`, `thermal-package/`, `cpu-test/`, `cpu-package/` |
| SD·UFS UEFI | SD 재삽입과 UFS LU2 각각에서 478,720바이트 원본 일치, 정상 EFI 파일 실행과 쓰기 보호 | `qemu-sd-final/execution.json`, `qemu-ufs-final/execution.json` |
| UFS·버튼 Windows | Storport, UFS PRM 전원 필터, 전원·음량 버튼 SYS의 ARM64 링크와 모의 콜백 통과 | `windows-storage-buttons-final-r2.log`, `ufs-test-final/`, `ufs-power-test-final/`, `buttons-test-final/` |
| Bluetooth·LTE·Wi-Fi | BTIF 자원 소유 코드 모의 검사 및 WDK ARM64 링크. Linux Wi-Fi 연결 보조 35개와 읽기 전용 LTE 상태 검사의 모의 실행 통과 | `btif-owner-final/`, `btif-native-macos-final.log`, `linux-connectivity-final.log`, `linux-lte-final.log` |
| RTC | 부팅 중 달력 읽기·쓰기, 래퍼의 EBS 이후 차단, 잘못된 시간 및 실패 뒤 상태 보존을 네이티브 모의 실행 | `rtc-native-macos-final.log` |
| 보드 전체 코드 | ARM64 Linux에서 ASan·UBSan을 켠 57개 검사 통과. 실행 전후 검사 대상 소스 해시 일치 | OrbStack 프로젝트의 `analysis/verify-remaining-board-20261119-final.json` |

Windows/WinPE 로드, 서명, 설치와 새 이미지의 태블릿 실기 실행은 미실행이다.
Linux는 순정 모듈 공급자 429개를 대조했고, 선택한 이름 219종의 원본 파일 220개와 펌웨어 32개를 사용할 수 있다. 실제 적재·카메라·센서·통신·화면 서비스 동작은 미확인이다.

## 2026-11-22 오프라인 포트 갱신

- Windows boot storage 설정을 source INF에 연결했다. UFS Storport와 PRM 상위 필터는 StartType=0, SD는 demand-start+BootFlags=0x88(SD/WinPE), USB는 demand-start+BootFlags=0x80(WinPE)다. Windows/test-storage-boot-inf.py가 네 패키지의 ACPI ID/service 설정/필터를 확인하며, Windows/build.py의 --usb/--sd/--ufs/--ufs-power 대상도 이를 자동 실행해 검증 보고서에 포함한다. 네 ARM64 unsigned 링크가 Build/MtkUfsBootStartChecked-20261122 (35,840), MtkUfsPowerBootStartChecked-20261122 (15,872), MtkSdBootFlagChecked-20261122 (29,696), MtkUsbBootFlagChecked-20261122 (84,480 bytes)에서 통과했다. 기존 boot.wim은 이전 INF 패키지를 담고 있고 install.wim은 없어 새 WinPE/설치 이미지·서명·설치 후 시작은 아직 확인하지 않았다.

- SDPORT GPIO7 카드 변경 경로와 재삽입 뒤 호스트 재초기화를 연결했다. ARM64 콜백 모의 검사 142,750개와 `MtkSd.sys` 링크를 확인했다.
- Linux 초기 CPIO에 Weston/PanVK와 Wi-Fi·Bluetooth 사용자 공간을 묶었다. Weston 종료 시 자식 프로세스 그룹도 회수한다. Linux 시작 검사 15개, 초기 CPIO 검사 106개, EFI 로더 검사 10개가 통과했다. CPIO SHA-256은 `6cc1c5019679311aa5cffcedd6af2d312e5ab4c2e439ae5670d4c4654b304fb3`이다.
- Linux EFI가 압축 initramfs와 RAW 모듈 CPIO를 결합할 때 두 번째 archive를 4바이트 경계로 정렬하고 사이를 0으로 채우도록 수정했다. `LinuxBootTest` 실제 C 함수 26개 시나리오가 ASan/UBSan으로 통과했다. ARM64 Unicorn 검사에서는 파일·SHA 10건과 실제 lid/libfdt 처리 8건이 통과했다. RELEASE ARM64 EFI 앱은 `Build/t70proPkg/RELEASE_CLANGPDB/AARCH64/T70LinuxBoot.efi`에 빌드됐다(44,544바이트, SHA-256 `874717f333d678e07e46bc2d4cfafcc8d41f149e3656d88a58df68e43ba90bbe`). 커널 실행·태블릿 부팅은 미실행이다.
- 전체 ACPI 호스트 검사가 새로 빌드한 표 9개(DSDT/SSDT 5개와 고정 표 4개)를 대상으로 통과했다. 가상 AML 평가에서 USB 82건, root-port 복귀 42건, I²C 유지 61건·wrapper 복구 30건, 터치 복귀 13건을 포함해 GPIO·Hall·충전기·화면 자원 검사를 통과했다. 이는 AML/DT 모의 검사이며 Windows 설치 부팅이나 0xA5 실기 재현은 아니다. 결과는 `Build/t70proPkg-linux-alignment-20261122/acpi-host-pathfix2/verification.json`에 있다.
- 0xA5 조사에서 위 ACPI host 검사 입력은 최신 DSDT가 아니었던 것을 확인했다: 기존 DSDT AML 10,019 bytes였고 현재는 10,115 bytes다. 현재 ASL/SSDT 5개와 고정 표 4개를 다시 생성해 전체 가상 AML 검사를 통과했다. 현재 결과는 `Build/acpi-host-0xa5-current-20261122/verification.json`; 최신 trial FD가 같은 표를 포함하는 것도 `Build/battery-adc-gauge-fw-20261122/acpi-fv-package-verification.json`에서 확인했다. 사용자가 보낸 사진에는 `ACPI_BIOS_ERROR (0xA5)`만 있고 첫 bugcheck 매개변수와 사용 이미지/USB 식별 정보가 없어 실제 중지 원인은 아직 특정하지 않았다. 상세는 `ACPI-A5-HANDOFF.md`에 기록했다.
- `Build/winpe-acpi-a5-20261122`의 기존 4 MiB FD에서 ACPI FV 포장을 다시 대조했다. DSDT/SSDT 5개와 FADT/MADT/GTDT/PPTT 4개, AcpiPlatform/AcpiTableDxe 이미지·DEPEX가 현재 `acpi-host-pathfix2` 표와 일치한다. FD SHA-256 `368f206002cb22977b1f07f3119127d7310eb858cb06cf7ab6cf9f2bf551d6a4`; 결과 `Build/winpe-acpi-a5-20261122/acpi-fv-package-verification.json`. AML/포장 일치 검사이며 Windows 부팅이나 0xA5 원인 확인은 아니다.
- Windows 표준 드라이버 10종의 시험 빌드·모의 검사와 ARM64 SYS 링크 20단계를 통과했다. WCN0, BTIF, GNSS도 별도 테스트·링크에서 통과했다. 결과는 `Build/windows-offline-audit-20261122/`에 있다.
- QEMU WinPE에서 16개 드라이버 패키지를 두 인덱스에 적용했다. 13개 패키지는 이번 소스에서 빌드한 버전이며 `MtkAudio`, `MtkMaliBoard`, `MtkMaliSoc`은 기존 패키지를 유지했다. 새 시험 이미지 SHA-256은 `87b54bd2af846a3db091fe3ac8459f444431db3f2fb4e7421712f27968d1fa2d`이다. WinPE BCD에서 testsigning을 켰고 MtkSd 서명 체인을 확인했다. 원본 USB 볼륨은 쓰지 않았다.
- 새 WinPE 이미지는 `boot.wim` 설치 환경이다. `install.wim`, `install.esd`, `install*.swm`은 포함하지 않아 이 매체만으로 Windows 설치를 끝낼 수 없다. 실제 태블릿 부팅, Windows 드라이버 로드, GPIO IRQ 전달과 장치 재열거는 미실행이다.
- EFI 수정 항목의 실제 C 호스트 검사 57종이 통과했다. RTC 5종은 별도 stock-DT 검증에서 Boot RTC 1,115,477개, Set RTC 1,351,655개, Runtime RTC 21,688개 검사도 통과했다. Windows MtkUfsPower·MtkSd·MtkUfs·MtkI2c·MtkGpio·MtkUsb·HimaxTouch의 ARM64 모의 검사와 임시 출력 폴더 링크가 통과했다. 링크 결과는 unsigned이며 OS 로드와 기기 동작을 뜻하지 않는다.
- 수정된 전체 T70Pro 펌웨어의 firmware-trial 빌드가 별도 출력 폴더 Build/t70proPkg-audit-offline-20261122에서 완료됐다. 4 MiB FD와 Android boot-v4 헤더, FD 삽입, LZ4 압축 해제 결과를 확인했다. FD SHA-256 368f206002cb22977b1f07f3119127d7310eb858cb06cf7ab6cf9f2bf551d6a4, boot image SHA-256 626d81f238c60999e7d3d870bfc2d3be4823dd0fca34b2933e69250eff47b7ed. 태블릿에는 쓰지 않았다.
- Windows MtkConn WCN0 owner의 firmware/ROM load, provider ordering, PnP/전원/rundown host tests 및 unsigned ARM64 SYS link 통과. GNSS UMDF2 consumer DLL과 ARM64 callback harness도 builder 가상환경에서 통과했다. GNSS receiver child/provider는 여전히 없음.
- MtkButtons 코어 검사 341개, ARM64 콜백 모의 검사와 28,672바이트 서명 전 SYS 링크도 통과했다. 소프트웨어 알림 경로만 확인했으며 실제 버튼·전원 신호는 미확인이다.
- Windows PmicPrm에 순정 게이지 raw 전류와 stock 정수 단위 변환 API를 연결했다. 변환에는 T70 stock parser가 r-fg-value=5, car-tune-value=101을 내부값 50/1010으로 확장하는 규칙을 쓴다. 결과는 ACPI mA가 아니며 SOC나 잔량으로 해석하지 않는다. Build/PmicPrmStockUnits-20261122/의 생산 코드 ASan/UBSan 검사 40,308개와 broker 검사 2,051개가 통과했다. Build/MtkButtonsStockCurrent-20261122/MtkButtons.sys는 ARM64 unsigned 링크(31,232 bytes)다. 표준 Windows battery device, 보정 추적과 _BIX/_BST 소비자는 아직 없다.
- Windows PMIC PRM은 AUXADC 전압·thermistor 온도·칩 온도·BIF raw 샘플을 고정 channel API로 제공하고, 하나의 gauge lease 안에서 thermistor/current latch/BIF를 수집해 stock signed deci-Celsius를 계산한다. BIF가 disabled였으면 control bit를 복구해 다시 읽고, 복구 실패 시 WACS lease를 유지해 추가 접근을 막는다. Build/PmicPrmBatteryTemperature-20261122/의 production C ASan/UBSan 검사 73,851개와 broker 검사 2,051개가 통과했고, Build/MtkButtonsBatteryTemperature-20261122/MtkButtons.sys는 ARM64 unsigned 링크(31,232 bytes)다. 이는 temperature API만 추가하며 calibrated voltage/eFuse trim, SOC, Windows battery device와 _BIX/_BST 공급은 남음.
- Windows CPU 및 thermal provider의 ARM64 callback harness와 unsigned SYS link도 각각 통과했다. CPU OPP 전환/PPM 제어, thermal power-loss reinitialization은 미구현 또는 미확인이다.
- EFI/MediaTek 생산 C 전체 57개 호스트 프로그램을 ASan/UBSan으로 다시 실행해 모두 통과했다. 입력 소스와 순정 DTB 해시가 실행 중 바뀌지 않았고 결과는 ../analysis/mediatek-offline-all-20261122.json에 있다. 이 검사는 가짜 MMIO/이벤트 기반이며 Windows PPM 통합과 실기 동작을 증명하지 않는다.
- 11/22 전수 감사에서 추가된 USB 인터럽트 패킷 분할, Himax 복귀 lease/timer, RTC 동시 읽기, UFS 전원 DEVICE_USAGE_NOTIFICATION 네 건을 수정했다. MusbHostCoreTest 7,313개, HimaxDxeTest 13,002개, RTC Boot/Set/Runtime 1,115,477/1,351,700/21,688개, UFS power 1,166개를 통과했다. Windows/Build/interrupt-multipacket-prod-final-20261122/와 Windows/Build/usage-notify-prod-final-20261122/에서 unsigned ARM64 SYS 링크도 확인했다. 상세는 DRIVER-AUDIT-20261122.md에 있다.
- 새 firmware-trial ARM64 빌드가 Build/driver-audit-fix-20261122/RELEASE_CLANGPDB에 완료됐다. 4 MiB FD SHA-256 dc3c457d31c996bbb2ab1eb1a74ca56fa2e26b21ba6c7abc15e938804214f754, 시험 부팅 이미지 SHA-256 cbff4fa23c020d7bbd976caed63b6add79ef6b8a53056ea71971e1da4d3fbeb4. EFI 호스트 검사 57개 모두 통과; 태블릿에는 쓰지 않았다.

- BTIF HANA6795 FDO/INF는 MS_BTHX_BTHMINI 자식 PDO와 HCI event/ACL 큐를 연결한다. full-mode WMT Bluetooth function-on 전송·firmware status 확인을 추가했다. Owner 40,967개, HCI 2,824개, WDK ARM64 SYS 링크는 호스트 모의 검사 결과다. WMT function ID 0은 T70 kernel-reference commit a71515dbaf0d539ffb36416f14186dad17eef743의 wmt_exp.h에서 WMTDRV_TYPE_BT=0으로 확인했다. 현재 BTHX의 SCO capability는 Windows 요구 조건을 충족하지 않아 스택 로드를 막는다. SCO sideband·HFP bypass, 서명·OS 로드와 실제 RF 연결도 남아 있다.
- Windows 기본 I2C, SD, CPU, thermal, buttons, USB, GPIO, Himax, UFS, UFS 전원, display 드라이버 11종과 SD 전원 검사 경로를 새 출력 폴더에서 다시 빌드·검사했다. 21개 JSON 콜백 검사 보고서가 모두 `pass`; 결과는 `Windows/Build/ports-host-pass-20261122-a/`에 있다. unsigned 호스트 빌드이며 Windows 로드나 실기 동작은 아니다.
- USB-C Type-C source probe는 role timer에서 Begin/Poll/StepCleanup으로 한 단계씩 진행하며 동기 `DelayMs` loop를 거치지 않는다. `CheckTimerDrivenProbeDoesNotDelay`는 Begin/Poll/StepCleanup의 no-attach timeout과 attached-source cleanup에서 `DelayMs` callback 호출 수가 0인지 검사한다. PC 호스트가 확정되기 전 미완성 probe는 정리하고, 확정 뒤에는 `CheckLockedHostSkipsRoleProbe`가 펌웨어의 자동 역할 탐색이 다시 시작되지 않는지 검사한다. 현재 `MtkHusb311TypecCoreTest` 167,264개와 `MusbHandoffDxeTest` 228,263개가 통과했다.
- WCN0 firmware/ROM owner를 현재 소스와 stock ROM 6개로 새 출력 폴더 `Windows/Build/mtkconn-wifi-foundation-20261122-a/`에 다시 빌드했다. Bootstrap 51,708개와 PnP/power/BTIF lease callback 검사 통과, unsigned ARM64 KMDF SYS 링크 완료. 이 산출물은 WLAN HIF/NDIS client를 포함하지 않는다.
- WCN0의 공유 HANA6796 `_CRS`에 순정 BTCVSD level-high GSIV 439와 WLAN GSIV 441을 각각 한 번씩 선언했다. SCO control/SRAM 주소 두 구간은 기존 0x18000000/1MiB WCN 창 안에 들어가 별도 MMIO 자원을 중복 추가하지 않는다. `build-acpi-host.py` 전체 호스트 AML 평가에서 통과했고 `verification.json`의 WCN0 자원은 `[439, 441]`로 기록됐다.
- `MtkConn`은 raw/translated GSI 439만 선택해 WDF interrupt를 만들고, ISR은 stock BTCVSD control의 인터럽트 상태가 올라온 경우에만 상태 비트를 지운다. SCO 소스는 기존 SoC lease에서 계속 마스크된 상태이며 이 ISR은 오디오 패킷을 옮기지 않는다. 자원 누락·중복·공유 모드·interrupt 생성 실패와 ACK/비대상 interrupt를 `DriverTest`에서 확인했다. WCN bootstrap 65,707개, SCO pointer/copy 393,216개와 6개 packet profile 검사 및 unsigned ARM64 SYS 링크가 `Windows/Build/mtkconn-sco-irq-ack4-20261122/verification.json` 산출과 함께 통과했다. packet object link, audio consumer와 `ScoSupportHCIBypass` 연결은 남음.
- 이 ACPI 변경을 포함한 RELEASE 펌웨어가 `Build/t70pro-wcn-sco-irq-ack-20261122/RELEASE_CLANGPDB`에 빌드됐다. 4 MiB FD SHA-256 `dab756ceb3d97d8696430c2815d44a7a72f88db5e7d2572fe145e4538491b797`, Android boot-v4 이미지(696,320 bytes) SHA-256 `29f8720095f5b6c657141ccb6b1ba69ea7ceeac99e849a9025ca13114b906123`. 새 AML과 FD 내 ACPI FV 표 9개가 바이트 대조를 통과했다. 이미지는 태블릿에 쓰지 않았다.
- WWAN에 stock CCMNI의 정방향 채널 표와 RX/TX 채널 역조회를 포팅했다. T70 DT의 모뎀 세대 6295, 22개 인덱스, index 8 비활성 LAN 복제 항목, ACK 번호와 MD_SYS1 다중 큐 flag를 port_net/ccci_core와 대조했다. Build/wwan-ccci-rx-route-20261122에서 링과 CCMNI 분류를 합친 ASan/UBSan 검사 236개와 ARM64 객체(링 6,653 bytes, map 2,339 bytes)가 통과했다. 이는 DPMAIF DMA나 NDIS/MBIM 장치 연결을 구현하지 않는다.
- WWAN의 stock CCCI/CCIF shared-memory ring을 독립 C core로 포팅했다. CCCI header/footer, 16-byte message header, 15-bit receive sequence, 길이 정렬, backpressure와 순환 복사를 처리한다. 같은 검사에서 CCMNI RX/TX 채널을 모두 22개 인덱스와 대조하고, 미등록 채널 및 실패 시 출력 보존도 확인했다. 순정 CCCI ring source의 GPL-2.0-only를 유지한다. DPMAIF/DMA와 WWAN PnP 연결은 남음.
- HUSB311 passive VBUS sensing에서 일부 레지스터 쓰기 실패 또는 샘플 timeout이 나도 `DeviceSettled` 전 원래 세 감지 레지스터를 복구하고, I²C lease를 놓은 뒤 다음 tick에서 재시도한다. `CheckEarlySenseRollback`의 부분실패/timeout 두 경우가 `MusbHandoffDxeTest`에 포함돼 위 255,596개 검사에 통과했다.
- `Windows/build.py --help`가 I2C 빌드로 빠지던 동작을 도움말 종료로 수정했다. `Windows/test-build-output.py` 9개 검사 통과; 검사에서 출력 디렉터리가 생성되지 않는 것도 확인했다. 수정 전 도움말 호출 과정에서 기본 `Windows/Build/MtkI2c.sys`가 재생성됐다(28,160 bytes, SHA-256 `7f63aef001a0674535cb858d96830a7d4f4e6231075f37a7d5c4987e2e04cdda`).

- Windows/MtkWwan has source-locked CCCI/CCIF ring and CCMNI cores, DPMAIF v3 descriptor decode/encode, RX PIT packet walking, RX/TX ring lifecycle, BAT token/slot ownership, a TX packet descriptor planner with the kernel-derived 36-bit DMA mask and HANA6797 ACPI resource-map validation. Resource order follows the stock ccci_hif_dpmaif_v3.c reg[0..3] mapping and is matched against Cellular.asl SHA-256 f67d26f8d2b178b030d405c91903ba3b689f16ea845693cb1467bd06da2d9a91. The ring/BAT rules and descriptor formats are derived from kernel-reference commit a71515dbaf0d539ffb36416f14186dad17eef743; required kernel source hashes are checked by Windows/build.py. ASan/UBSan CCCI/CCMNI checks 236, descriptor/PIT/TX/BAT encode checks 73, ring lifecycle checks 84, BAT ownership checks 71, TX packet-plan checks 66, WDM SG-list normalization checks 15 and ACPI resource-map checks 22, CCCI memory-handoff checks 13, and separate-table layout checks 32 pass. The low-level descriptor codec preserves the 40-bit wire fields; WDM adapter setup requests the stock 36-bit mask, validates common buffers and keeps async mappings alive until PutScatterGatherList. This WDM DMA bridge and the SG normalizer are ARM64 objects 8,033 and 1,055 bytes; the current ring/map/descriptor/ring/BAT/TX/resource/layout objects are 6,653/2,339/6,236/3,648/4,866/1,679/3,412/2,652 bytes in Windows/Build/windows-port-matrix-20261123/wwan-cores/CcciRing-verification.json. The PnP NDIS miniport and INF are linked but not loaded. The miniport maps the validated handoff, initializes WDM DMA, allocates/zeros separate source-sized ring-table buffers, and precomputes the full startup plan when LK reports no MD1 image-load error; this flag does not attest to a live modem. It also allocates request/fragment common-buffer vectors, validates returned DMA addresses, initializes BAT slot ownership, and posts count-minus-one initial BAT entries. The production WDM common-buffer-vector callbacks now have 86 host checks for allocation/free ownership, indexed address/alignment validation, range failures, and cleanup. It does not apply MMIO plans, register interrupts, refill/reclaim BAT buffers, boot the modem, connect CCCI control, complete packets, or provide MBIM/WWAN data flow.
- The BTIF build verifies its Bluetooth function ID against the same T70 kernel-reference wmt_exp.h and the exact BT target compile define; Windows/Build/btif-wmt-source-parity-20261122/verification.json records commit, enum value and source hash. This confirms the function identifier, not hardware acknowledgement or radio operation.

- Windows 배터리 경로를 추가했다. Windows/MtkBattery는 BATTERY_MINIPORT_INFO_V1_1/BatteryClassInitializeDevice 기반이며, ACPI HANA679B PnP ID와 WDF 전처리 IOCTL/WMI 경로를 연결한다. 커널 a71515dbaf0d539ffb36416f14186dad17eef743의 mtk_battery_table.h에서 기본 battery ID 0의 T0~T4 500개 profile points를 다시 생성·해시 대조한다. OCV로 시작 SOC를 보정하고 PRM의 stock 0.1mA 단위 전류를 적산해 Battery Class capacity/rate/status로 변환한다. 실제 하드웨어 Coulomb CAR와 stock GM30 aging/learn 알고리즘은 이 포트에 노출되지 않아 동일 동작을 주장하지 않는다. ASan/UBSan 호스트 검사는 478개, ARM64 unsigned MtkBattery.sys 링크는 39,936 bytes, INF/driver import 검사, DSDT 독립 IASL compile(AML 10,195 bytes), 전체 AML 호스트 검증(Build/acpi-host-battery-v2-20261123/verification.json), RELEASE FD 안 ACPI FV 표 9개 바이트 대조가 통과했다. 새 4 MiB FD SHA-256은 6692e1020994f6a5e4bead06ca833f65e7d6b19e5aff9bb64aec419d73b7b989이며 결과는 Windows/Build/battery-miniport-20261123/와 Build/t70pro-battery-acpi-20261123/RELEASE_CLANGPDB/에 있다. Windows load/서명/WinPE 포함과 기기 실행은 하지 않았다.

## 2026-11-23 CCCI handoff readiness semantics

- The LK `hdr_count`/`hdr_tbl_inf` result only establishes the MD1 reserved-memory range and whether the image/error fields report success. It does not prove modem boot, DPMAIF clock availability or CCIF readiness. Renamed the handoff bit to `MD1_IMAGE_LOAD_OK` and the parser result to `ImageLoadReportedOk`; the wire layout and bit value remain unchanged. The NDIS miniport still only stores a startup plan; no MMIO startup is implied by this handoff bit.
- Added a host check that the range handoff remains valid when no successful image-load flag is present. This keeps address reservation independent from device readiness.
- The EDK2 RELEASE firmware-trial build completed after the field/flag rename, producing `Build/goal-ccci-handoff/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd` and `Build/goal-ccci-handoff/RELEASE_CLANGPDB/T70Pro-UEFI-Trial-boot.img`. This was a builder compile/link only; no image was loaded or written to a tablet.
- The checked kernel source calls `MTK_SIP_KERNEL_CCCI_CONTROL/MD_BOOT_STATUS` to read live MD boot status, but the defining secure-monitor ABI header/constants are absent from the available checkout. No guessed SMC function ID or runtime-ready bit is used. A source-backed provider is still required before enabling DPMAIF MMIO/IRQ startup.

## 2026-11-23 Windows USB cold resume

- Windows `MtkUsb`는 retained D0 상태가 사라져도 저장된 ACR 튜닝이 있고, 새 MCLK·MROU·MTCV 표본이 유효하며, 컨트롤러가 idle인 경우만 cold PHY/host 시작으로 넘어간다. 첫 레지스터 접근 전에 상위 USB IRQ를 마스크하고 각 cold 단계의 외부 역할·경로·클록 증거를 다시 확인한다. 동기 지연 대신 1 ms worker polling을 사용한다. 재시작 실패는 소유했던 PHY/SESSION/POWER를 정리한 뒤 UCX 실패로 전달한다.
- `MtkMusbPlatformCore`는 Windows가 실제 쓰기 권한을 받은 TPHY 0x700-byte aperture도 cold evidence로 허용한다. guard에서 실제 접근 가능한 모든 TPHY 레지스터는 이 구간 안에 있다. 경계 통과/거부는 `MtkMusbPlatformCoreTest`에서 확인했다.
- 검증: `Windows/build.py --usb --test`의 ARM64 코드 모의 검사와 INF 검사 통과, `Windows/build.py --usb` ARM64 unsigned SYS 링크 통과. `MtkMusbPlatformCoreTest` 3,183 checks와 `MtkUsbPhyCoreTest` 23,123 checks 통과. 보고서는 `Windows/Build/usb-cold-resume-final-test/`와 `Windows/Build/usb-cold-resume-final-arm64/`에 있다. Windows 로드와 실기기는 미실행이다.

## 2026-11-23 WinPE setup entry

`Windows/Setup/StartT70Setup.cmd` calls `wpeinit` once before waiting for removable storage, retries only media discovery, and rejects ambiguous installation media. `Windows/Setup/test-start-t70-setup.py` checks the order in both setup scripts. The script was not executed in WinPE and no installer was launched.

## 2026-11-23 WWAN host callback follow-up

`DpmaifWdmDmaTest.c` now calls the production common-buffer-vector allocation, element lookup and free callbacks with fake WDM operations. The 86 checks cover allocation failures, missing callbacks, 36-bit range and 128-byte alignment rejection, retained DMA-adapter ownership, and release after vector free. `WwSetOid` stores a validated `OID_GEN_CURRENT_PACKET_FILTER` value under the NDIS spin lock so a subsequent query returns the requested mask; invalid values leave the prior filter unchanged. NDIS callback checks and package checks pass, and the unsigned ARM64 miniport links at 31,744 bytes. No device I/O or NDIS load was performed.

## 근거 위치와 확실성

- 소스상 포함: `t70pro.dsc` / `t70pro.fdf`.
  GOP DSC:129/FDF:47; RTC DSC:61/FDF:43;
  SD DSC:153·공통 `SiliciumPkg.dsc.inc:445`/FDF:73–74;
  UFS DSC:122–123,135–136/FDF:57,61–63. 행 번호는 변경 시 달라질 수 있다.
- ACPI 기준: `Silicium-ACPI/Platforms/Teclast/t70pro/DSDT.asl`의 커밋 `896190a`.
  UFS0/SDC1/HXT0/GPO0/LID0/I2T0/I2C6 및 각 `_STA` 조건을 확인했다.
  현재 미커밋 추가 장치·전원/열 변경은 이 기준과 섞지 않는다.
- Windows: `MtkGpio`, `MtkI2c`, `HimaxTouch`, `MtkUfs`의 INF와 생산 Driver.c를 대조했다.
  GPIO `ReadyMethod`, I²C `D0Entry`, 터치 `HmFirmwareReady/HmResumeReady`,
  UFS `UfsFind/DriverEntry`가 실제 준비/연결 경계다.
- Windows SD의 INF, 클록·카드 전원, 전송 지점과 남은 카드 교체 통지 범위는 [MtkSd 설명](../../../Windows/MtkSd/README.md)에 있다.
- 통신 한계: 기준 커밋의 `Windows/README.md` 통신·GNSS 절,
  `Windows/MtkGnss/MtkGnss.inf`, `Driver.c`, `Receiver.h`.
- Hall: 보드 `GpioPlatformDxe.c`의 `ValidateHallTree/LiveHallReady/ExportToAcpi`와
  DSDT의 GPIO0 OpRegion, `_AEI`, `_EVT`, `LID0`.
- USB: 11/18 실기 화면은 USB 장치 미연결 상태의 호스트/인터페이스 0 및 Not Ready다. 위 QEMU/ARM64 결과의 실제 태블릿 동작은 미확인이다.
  USB 소스 교차 검토는 기존 차단 범위를 유지한다.
- 담당은 실제 작업 배정 단위다. 미지정 항목은 자동으로 실행 중이라고 간주하지 않는다.
- 2026-11-23: RuntimeMemoryTest now checks LK tag parsing, MD_SYS1 reservation and signature-last publication of the 64-byte CCCI handoff (5,269 host checks). The NDIS package check confirms the firmware reservation, ACPI resource and Windows-side bounds validation. HANA6797 resource-map checks: 22; CCCI handoff malformed-record checks: 13; unsigned ARM64 MtkWwan.sys: 13,312 bytes. These checks do not start the modem or execute MMIO/IRQ operations.
- 2026-11-23 Windows verification update: ARM64 build and mock checks passed for I2C, SD, UFS, UFS power, USB, GPIO, Himax, display, thermal, CPU, buttons, battery, PMIC and SD power. MtkWwan linked an unsigned 13,312-byte SYS; CCCI handoff and DPMAIF core checks passed. BTIF HCI 2,824, owner 40,967 and PMIC checks passed; GNSS production source compiled for ARM64. A duplicate WDF interrupt type in the BTIF test fixture was moved into the shared mock header. All 57 board host checks are recorded in analysis/mediatek-driver-ports-20261123.json. RELEASE AARCH64 firmware was built under Build/port-complete-20261123-windows-first/; FD SHA-256 248ec7565db370222c8711258be705fcbb3c790a29e100181cac4ae412200312. verify-acpi.py and the FD ACPI table/package comparison passed. This work did not refresh the WinPE WIM or load drivers in Windows or on the tablet.
- 2026-11-23: Added DpmaifLayout from the stock v3 dimensions: DT default BAT count 16,384, PIT count 32,768, four 2,048-entry DRBs, and 64-byte table alignment. The 851,968-byte layout passes 32 separate-range/alignment/boundary checks and ARM64 object compile; MtkWwan now allocates and zeroes those separate table buffers during NDIS initialization, with common-buffer release before DMA-adapter teardown. Register startup, BAT refill/reclaim, interrupts and packet indication remain unconnected.
- 2026-11-23: Combined source-ordered startup-plan builder covers common SRAM/remap, interrupt masks, RX/TX queue setup, and init-done. Its 139 host checks verify stage order, total size, invalid modem memory, and unchanged output on failure. NDIS stores the plan only when the validated CCCI handoff reports no MD1 image-load error; it does not execute MMIO callbacks. The unsigned ARM64 miniport linked at 25,600 bytes in Windows/Build/wwan-startup-plan-separate-buffers-20261123-r2/.
- 2026-11-23: MtkWwan now prepares the initial request/fragment BAT buffer pools when the validated handoff reports no MD1 image-load error. Separate common-buffer vectors provide 16,383 buffers per table (3,584-byte request, 128-byte fragment); the portable initializer writes 8-byte wire descriptors, leaves one slot empty, and is covered by 71 ASan/UBSan BAT ownership checks. The unsigned ARM64 NDIS miniport linked at 28,672 bytes in Windows/Build/wwan-rx-bat-vectors-20261123/. No MMIO plan, IRQ, modem boot or packet path was executed.
- 2026-11-23 Windows build pass: 16 Windows/build.py targets passed in Windows/Build/windows-port-matrix-20261123/matrix.json (I2C, SD and SD-power, PMIC PRM, CPU, thermal, buttons, battery, USB, GPIO, Himax, UFS, UFS power, display, WWAN driver and WWAN cores). Separate BTIF, WCN0, GNSS UMDF2, DirectX WARP, and Mali source builds also passed; details are in their verification reports. GNSS and DirectX use the builder project virtualenv for Unicorn-based ARM64 callback emulation. All artifacts are unsigned and were not loaded. The checkout contains no WinPE WIM/ESD/ISO, so its image was not refreshed; no tablet was connected or exercised.
- 2026-11-23 USB resume audit: Windows/MtkUsb handles retained-context resume but does not invoke MtkMusbPlatformAdvanceColdHost after PHY/host power loss. That core requires fresh owner/role tokens plus clock, PHY, GPIO, boost and TCPC evidence. Current ACPI exposes MTCV's HUSB311 role-control/CC/VBUS sample and MROU's route-ready boolean; it does not expose the live PD data-role swap state handled by Android's TCPC notifier. A cold restart must not infer DFP authority from CC attachment alone. The next software step is to port a live Windows Type-C data-role provider and the MTK USB2 PHY clock/calibration owner, expose current route/clock/PHY readback through an ACPI contract, then integrate the asynchronous cold-start/cleanup state machine and test rejection, retry and resume paths. MtkUsb currently does not link MtkUsbPhyCore; its D0 resume only restores the retained USBPLL identity and cached PHY fields. The generic PHY initializer requires a 26 MHz reference-clock provider and ReadNvmemCell(intr_cal), which the EDK2 provider validates from the boot FDT. Windows cold recovery must replay the already-captured ResumeTuning values and obtain a verified clock contract; it must not fetch or log raw calibration data. No device was accessed.
