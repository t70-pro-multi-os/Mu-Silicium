# T70Pro G3D1 초기 EDK2 포팅

MT6789의 ACPI·SEC/DXE 기반과 주변장치 연결부를 구현 중이다. 기존 RAM 시험에서 UEFI 설정 화면 출력을 확인했다. 포고핀 키보드는 초기화 실패가 남아 있으며 Linux·Windows 부팅은 미실행이다.
런타임 DTB 기반 RAM 구간 계산과 PMIC·클록·MSDC 오류 처리를 구현하고 로컬 검사했다.
화면·입력·UFS·MUSB·OS 부팅과 실제 전체 RAM 사용은 미검증이며 실사용 포팅은 진행 중이다.
순정 ARM64 모듈의 코드를 분석해 Himax 입력과 충전·USB-C 상태 읽기를 옮기고, Linux용 원본 모듈 묶음을 구성했다.
소스와 빌드는 OrbStack `builder`의 기존 저장소 한 벌에서 수행한다.
실사용 기능과 휴대폰·듀얼 부팅 조건은 [CHECKLIST.md](CHECKLIST.md)에서 추적한다.
DTB·같은 칩 커널·기존 EDK2의 재사용 지점과 구현 순서는 [PORTING-REVIEW.md](PORTING-REVIEW.md)에 기록했다.
이번 작업은 실기기 시험을 제외한 오프라인 구현·빌드·검사만 수행한다. 기기 실행·설치는 승인되지 않았다.

2026-11-15 전체 범위 재검수에서 SD 식별 오류, GPS의 빌더 호환성, BTIF·GPU의 엄격한 컴파일 오류, Linux 입력·바탕화면 검사와 ACPI 바이트 해석 오류를 수정했다.
UEFI·공통 드라이버 검사 프로그램 49개와 Windows 드라이버 10종의 빌드·모의 실행을 통과했다. Linux 초기 사용자 공간 검사는 93개 중 92개 통과·macOS 전용 1개 미실행이며, 바탕화면 시작 14개·연결 서비스 24개·원본 모듈 검사 20개도 통과했다.
수정한 최종 FD와 같은 FD의 일반/시험 부팅 ARM64 실행, 원본 LK 전달·해제와 ACPI 검사를 통과했다. 실제 SD·키보드·새 OS에서 주요 기능 대부분이 작동하는 수준과 설치 준비 완료를 입증한 결과는 아니다.
구체적인 수정·재현·실행 범위는 [전체 검수 결과](../../../../analysis/driver-review-20261115T061306Z/review.json)를 따른다.

## 이번 구현과 활성화 상태

- `MemoryMapLib`: 런타임 RAM bank, FDT 예약 표, `/reserved-memory`, CCCI LK 정보 버퍼 64KiB를 해석한다. 고정 예약은 실제 UEFI 할당 HOB로 보호하고 `no-map`은 CPU 매핑에서 제외한다. 주소 없는 동적 요청은 검증 뒤 원본 FDT에 보존해 OS가 배치하게 한다. 잘못된 DTB·예약 정보·모뎀 정보 누락 때 추가 RAM 등록을 취소한다. 정적 8GB 값은 넣지 않았다.
- `PmicWrapperDxe`/`MT6358PmicDxe`: 10ms 통신 제한, 오류 반환, LDO 선택 번호 수정, VMCH_EINT_LOW 전원 순서와 실패 시 이전 상태 복원. 반환 형식 변경에 맞춰 PWRAP protocol GUID도 변경했다.
- `GpioDxe`: 방향 비트와 실제 DI 입력 읽기, write-one SET/CLR, MT6789 원자적 mux 갱신을 수정했다. 실제 GPIO 0~187과 가상 EINT 핀을 구분하며 다른 핀의 값을 보존한다.
- `ClockDxe`: bit31 gate의 부호 있는 시프트를 제거했다. 하위 gate를 끌 때 공유 부모/PLL을 끄지 않는다. 켜져 있던 PLL은 다시 초기화하지 않으며 gate 극성도 일치시켰다. MT6789의 실제 gate와 추가 분주 출력 비트를 분리하고 USBPLL의 PCW bit0를 켜짐으로 읽던 오류를 수정했다. 순정의 별도 gate/분주 출력 종료 순서와 APLL1/2 tuner-enable도 연결했다. 실제 C 969개 검사와 순정 ARM64 PLL 14개 실행을 통과했으며, 같은 초기값의 켜기·끄기 쓰기 값과 순서가 모두 일치했다. 최종 FD의 ClockDxe 연결도 확인했다. 세부 결과는 `latest_edk2_pll_and_vsafe0v` 기록을 따른다.
- `MsdcDxe`/MT6789·MT6768 연결부: 호스트별 선택, T70Pro는 MSDC1만·읽기 전용·최대 25MHz·GPIO7 active-high·VMCH_EINT_LOW. 응답/리셋/FIFO 시간 제한, CRC 우선 처리, 64-byte ADTC, FIFO 데이터/카운트, R1b busy 처리와 SD/eMMC 분기를 수정했다. 빈 슬롯도 게시하고, 20ms 간격의 안정된 감지 3회 뒤 식별한다. 제거·감지 오류 때 I/O를 차단하고 기존 카드의 프로토콜 철회가 끝나야 재식별하며, EBS는 정지 표시만 남긴다. CMD8의 전압·확인값, CMD55의 APP_CMD 응답과 RCA 오류를 검사하며, CMD8 응답이 없는 구형 SD 카드는 HCS/XPC 없이 식별한다. 삽입·제거·읽기와 오류 처리를 포함한 실제 C 검사 38,947개가 통과했다. 실물 카드 인식·읽기와 Linux의 SD 실행은 미확인이며 Windows SD 호스트 드라이버는 미구현이다.
- `MemoryMapLib`에 순정 DTB와 대조한 주변장치 MMIO 26개를 추가했다. 클럭·GPIO·PWRAP·APDMA·I²C·MSDC·UFS·MUSB/PHY를 device/uncached 속성으로 매핑한다.
- `T70I2cPlatformDxe`: 런타임 DTB·MMIO 범위와 상위 클록을 확인한 뒤 필요한 APDMA·I²C0/6 wrapper gate를 켜고, 입력으로 유지되며 두 번 높게 읽힌 GPIO만 I²C 기능으로 바꾼다. 준비 전에는 컨트롤러/APDMA를 읽지 않고, busy·부분 실패로 안전한 복원이 불가능하면 클록을 유지하고 게시를 거부한다. I²C6의 USB ID/VBUS 기능 핀은 전환하지 않는다. DTB 게시 알림으로 초기화 순서를 처리하고 EBS에서 실제 준비 비트/클록을 ACPI에 넘긴다. 꺼진 상위 PLL·전원 자체의 기동과 전기적 버스 복구는 남아 있다.
- `T70GpioPlatformDxe`: 게시된 런타임 DTB·GPIO/EINT 연결과 GCD MMIO 속성, 160개 EINT mask·GPIO IRQ AP 도메인을 읽기만 해 검사한다. EBS 때 별도 예약 RAM에 188/126 핀 수와 준비 상태를 기록한다. Windows GpioClx와 조건부 ACPI 장치를 연결하고 FV에 포함했다. GPIO9 입력·GPIO152 리셋 해제와 정상 터치 보고를 검증하는 Himax 보드 공급자도 연결했다. Windows의 188핀 IES·풀·공유 Schmitt 소유권·원시 DRV/RSEL/EH 저장/복원과 여섯 IOCFG 자원을 구현했다. 414,184개 네이티브 검사와 실제 ARM64 콜백의 모의 실행을 확인했다. 전류값과 EH 원시 필드의 대응 근거가 없어 nonzero DriveStrength는 MMIO 전에 거부한다. Windows 복귀에서는 핀 설정과 EINT 도메인이 유지된 경우 소유 IRQ 마스크를 복원한다. 전원 손실 뒤 핀 복원과 깨우기 경로는 남아 있다.
- UFS·GPU 공용 PMIC: 2026-11-13. UFS·GPU의 WACS2 통신을 공용 PRM 처리부에 연결했다. 두 처리 함수가 같은 잠금과 지속 상태 기록을 사용하며, 상대가 응답을 처리 중이거나 실패한 명령이 남아 있으면 새 명령을 차단한다. GPU는 허용된 레일 레지스터만 읽고 쓰며, 쓴 값을 다시 확인하고 응답을 확정한 뒤 결과를 넘긴다. Windows GPU의 PWRAP 직접 매핑을 없애 MFG0 자원을 5영역으로 맞췄다. UFS 전원 필터는 첫 호출부터 OS 재시작까지 PRM 모듈을 고정하고, GPU도 처리 여부가 불명확하면 모듈 고정과 실패 상태를 보존한다. 이 경로의 PRM 모듈 실행 중 교체는 지원하지 않는다. UFS의 공유 VEMC·TOP_UFS 유지형 정지·복귀는 계속 연결되어 있다. C 검사 프로그램 11개의 보고된 확인 항목 590,580개, Windows GPU ARM64 검사 18묶음, 새 PMIC 연결 5,364개·SoC 131,876개와 UFS 전원 필터 586개 조건이 통과했다. 최종 4MiB 펌웨어의 PRM 함수 36개 전체 명령어와 공용 통신 실행 27가지, 두 처리 함수의 332바이트 MEDT 및 잘못된 PE 메타데이터 20가지 거부도 확인했다. UFS 처리 코어의 연결부 실행 검사는 모의 호출이며 별도 C 검사로 실제 코어를 확인했다. 보안 영역·다른 펌웨어와의 접근 배제, 공유 부모 전원·기기별 OPP/DMA 입증, 전체 전원 손실 복구와 전체 DXE·OS·실기기 실행은 남아 있다. 근거는 project-state.json의 latest_edk2_shared_pmic_broker와 연결된 검사 기록이다.
- MUSB는 PIO control/bulk/interrupt 전송과 EFI USB2 호스트를 작성했다. 게시 전 Operational 전환과 ExitBootServices 시 타이머·비동기 포인터 정리를 검사했다. T70 U2 TPHY의 eFuse·초기화·slew 보정·전원 켜기/끄기·종료 코어를 추가해 11,668개 ASan/UBSan 검사를 통과했다. MMIO 실패 때 클록 소유권을 보존하고 시간 제한과 정수 범위를 검사한다. 허브 TT 라우팅과 비동기 인터럽트 파이프, 순정 FIFO·DMA/QMU 유휴 상태·이벤트 정리 코어를 연결했다. 순정 런타임 atag,devinfo의 읽기 전용 보정값 공급자와 고정 26MHz 참조를 연결했다. EndOfDxe 이후 다른 호스트 조건이 검증되면 순정 PHY 초기화·보정·호스트 조정과 FIFO를 준비한다. 초기 고정 Type-C source/DFP cold 경로를 연결했고, PD·실행 중 USB-C/VBUS 역할 변경·전체 전원 복귀와 isochronous는 남아 있다.
- `MusbHandoffDxe`는 정확한 DTB·클록·GPIO·PHY·MUSB·TCPC 전원/CC 상태를 검사해 USB 호스트를 게시한다. PC 모드에서는 초기 장치 연결을 한 번 정리한 뒤 기본 역할을 호스트로 고정하고 온보드 허브를 계속 켠다. GPIO10 변화는 허브 아래 포고 접점의 상태로만 취급해 HCD·USB_IO·파일시스템을 다시 만들지 않으며, 역할 변경 권한은 ExitBootServices 이후 운영체제로 넘어간다. 이미 준비된 source/host와 꺼진 허브에서는 GPIO99→4→104 순서로 허브를 켠다. source 연결·POWER_STATUS[2] 부재·HT_STATUS(0x97)[1] 안전 전압 표시를 200ms 반복 확인하고 순정 전압/전류 selector가 맞으면 꺼진 SC8989x boost도 시작한다. 버스 전체 점유와 각 단계 결과 확인, 변경한 부분만 역순으로 복원하는 경로를 연결했다. 최종 boost 쓰기 직전에도 SC RMW 뒤 CC/역할/두 전압 상태를 다시 읽는다. boost enable 뒤 실제 TCPC/MUSB VBUS 확인이 끝나야 진행한다. 정리 뒤에는 두 원시 상태로 전압 하강을 기다리고 전체 출력 상태를 다시 확인한다. 시간 초과·읽기 실패 때는 문맥과 코드를 유지하며 재시도에서 복원 쓰기를 반복하지 않는다. I²C 정지 직전 관찰자가 마지막 전원을 읽으므로 EBS 이벤트 순서가 바뀌어도 공개 동기 I²C를 정지 후 호출하지 않는다. PLL 미설정 상태·PD·운영체제 요청 기반 역할 변경과 실제 열거는 남아 있다. 자세한 근거는 `analysis/musb-platform-readiness.md`와 [Windows USB 호스트](../../../Windows/README.md#musb-usb2-호스트와-순정-키보드-경로)를 따른다.
- 독립 ID-low 허브/호스트·충전 보존과 인계 revision2의 UEFI·AML·Windows 소비자를 구현했다. 선택 C 12개 프로그램 1,395,406개(보드 1,284,172개 포함), Windows ARM64 콜백·SYS 및 전체 펌웨어 빌드가 통과했다. 실제 AML의 MUSB 조건 61개와 최종 FD의 필수 진입점 13개·인라인 보조 함수 5개·별도 보조 함수 5개 및 호출 관계 대조도 통과했다. 해시는 latest_edk2_id_low_usb를 따른다. 실제 배선·전압·입력/열거, PD·실행 중 역할/전원 복귀와 PLL 미설정 상태는 남아 있다.
- GOP는 표준 런타임 simple-framebuffer에 더해 LK가 유지한 OVL 화면을 직접 검사하는 공급자를 연결했다. 정확한 DT 자원·RAM/VRAM·DISP 전원/버스·클록·RDMA/DSI 경로·SMI 물리 주소 모드와 두 번 읽은 레이어 설정이 일치해야 한다. SEC는 기존 레이어의 픽셀 알파 무시 비트만 설정·재확인하고 실제 화면 영역을 예약/UC 매핑한다. GOP는 쓰기 없이 이를 다시 검사한 뒤 기존 EFI 메모리 보호·장치 경로·EBS 절차를 따른다. CCCI·memreserve·다른 고정 예약 풀과 겹치면 화면 매핑과 추가 RAM 등록을 중단한다. 순정 오프라인 DT의 미초기화 화면 값으로는 MMIO에 접근하지 않는다. 기존 RAM 시험에서 UEFI 화면 출력을 확인했다. 꺼진 화면의 초기화·새 타이밍 설정·OS 화면과 절전 복귀는 남아 있다.
- 패널 전원 코어는 순정 LK의 along_cyx init/suspend GPIO·지연 순서를 구현하고 쓰기 후 확인·실패 위치 유지·재시도·재진입 거부를 검사했다. GPIO152는 Himax reset과 겹치므로 보드별 핀 소유권 확인이 끝나기 전에는 첫 쓰기부터 거부해야 한다. 이 코어는 아직 보드에 연결되지 않았고 DSI/PHY 초기화·완전한 전원 끄기/복귀를 구현한 상태도 아니다. 근거는 `analysis/display-lk-handoff.json`, 선택 검사는 `analysis/platform-driver-verification.json`이다.
- GOP의 OS 인계 기록은 등록된 GOP 콜백이 맡는다. EBS 알림 순서가 뒤바뀌어도 정상 ready 값이 지워지지 않으며, 화면이 없는 경우와 반복 EBS는 계속 ready를 취소한다. 인계 직전에 쓰기 금지(WP)가 생긴 프레임버퍼도 거부한다. 수정 전 실제 C에서 두 오류를 각각 재현했고 GOP DXE 검사 2,774개를 통과했다.
- PSCI 재부팅·종료 라이브러리는 `ResetSystemLib`와 실제 소비자인 `HwResetSystemLib`를 함께 선언한다. 기존 호출과 매핑은 유지하며 빌드 경고를 없앴다. 최종 FV의 runtime PE와 ARM64 재부팅·종료·웜 재부팅 대체 경로 7가지를 검사했다. 보안 모니터 호출은 가짜 응답으로 대체하며 실제 전원 차단 결과는 아니다.
- `t70pro.fdf`에는 보드 주변장치 15개와 표준 부팅 의존 드라이버 11개를 포함한다. 압축을 푼 실제 FV의 PE를 현재 빌드와 대조하며 일반 ScsiDiskDxe는 포함하지 않는다. 보드 BDS가 입력·화면·저장장치 연결, 표준 SimpleTextIn의 ConIn 등록과 SimpleTextOut의 ConOut/ErrOut 등록, 파일 시스템 재검색을 수행한다. UsbKbDxe가 만든 일반 키보드 입력도 등록하며, 잘못된 장치 경로와 중복 진입을 처리한다. 최종 FV의 BdsDxe PE와 연결 심볼도 검사한다. 실제 태블릿의 핀 전기 설정·전원 역할·콜드 초기화와 장치 인계 검증이 남아 있다.
- 실제 C + 가짜 MMIO/UEFI 서비스로 검사 프로그램을 실행하며 AddressSanitizer/UndefinedBehaviorSanitizer를 사용한다. 전체 결과는 `analysis/mediatek-driver-tests.json`, 최근 선택한 프로그램·통과 결과·소스 해시는 `analysis/mediatek-driver-selected-tests.json`에 있다. 프로그램별 실행 범위는 최신 EDK2 상태 기록과 함께 확인한다. 검사 횟수에는 반복 MMIO 접근 확인이 포함되며 기능 지원 수가 아니다.

```sh
with-build-lock T70Pro "Check MediaTek drivers and runtime RAM" -- \
  python Mu-Silicium/Platforms/Teclast/t70proPkg/verify-mediatek.py
```

실제 RAM/주변장치 동작, SD hotplug·장시간 I/O·절전, Windows 드라이버 로드와 듀얼 부팅은 미실행이다.

I²C 공통 엔진·EDK2 비동기 master·Windows KMDF/SpbCx와 GPIO/EINT KMDF/GpioClx `.sys`·INF를 구현했다.
세 버스의 ACPI 자원과 준비 상태 발행을 구현했다. Windows 복귀용 IRRS는 검증한 상위 클록과 핀이 유지될 때 해당 I²C 게이트만 복구한다. I²C 공급자와 master는 FV에 포함하며, ACPI는 EBS 때 새로 확인한 준비 값에 따라 장치를 노출한다. GPIO/Himax 전용 공급자도 FV에 포함하며 새로 검증한 준비 상태를 요구한다.
지원 범위·빌드·검사·미완 항목은 [Windows/README.md](../../../Windows/README.md)에 기록했다.

I²C는 최대 32개 읽기/쓰기·지연과 repeated START를 처리한다. 모든 버퍼·DMA 범위·128KiB·10초 한도를 첫 접근 전에 검사하며 MT6983 read handshake와 마지막 DMA 정지도 확인한다. Windows는 SpbCx의 완료 바이트 규칙을 따른다. 요청 사이 버스 잠금과 전기적 버스 복구는 미완이다.

## 순정 바이너리에서 옮긴 코드

- `hxchipset_i2c.ko`의 HX83132 이벤트 읽기 명령은 `0x30`이며, 10점 일반 프레임은 56바이트다. 좌표·압력·체크섬·접촉 수를 해석하는 `HimaxCoreLib`와 `EFI_ABSOLUTE_POINTER_PROTOCOL` 연결부를 구현했다. 실제 순정 ARM64 좌표 함수를 Unicorn에서 실행해 새 C 코드와 유효 프레임 1,024개를 대조했다. 간접 레지스터 접근·칩 ID 확인·인터페이스 재확인과 리셋 없는 SenseOn을 EDK2와 Windows 양쪽에 구현했다. 원본 시작 함수 23가지의 명령·대기를 비교했다. 소프트웨어 리셋 명령도 별도로 구현했으나 전체 복구 절차는 아니다. T70GpioPlatformDxe가 정확한 DTB·핀 상태를 검사하고 I²C 단독 사용권을 얻어 칩 ID·정상 보고·IRQ 해제를 확인한 뒤 revision 2 보드 프로토콜을 발행한다. 입력 드라이버 시작 후 유효 보고가 있어야 EBS에서 HMXR 준비 값을 남긴다. 펌웨어 CRC32C/descriptor 검사와 bit4가 알려진 0일 때의 제한된 sense-off 앞부분을 별도 API로 추가했다. 이 API는 정지를 확인하지 못하면 HmTimeout으로 끝나며 전체 정지/복구 절차가 아니다. 최초 인계/retained resume을 구분하고 오래된 HMRD 응답은 거부한다. bit4-set·펌웨어 업로드·전원 손실 복구·실행 중 전원 공급자와 실제 IRQ 동작은 남아 있다.
- `StockPowerCoreLib`는 SC8989x 칩 식별·충전 상태와 ADC 단위 변환, HUSB311 식별·CC·VBUS·alert/mask·raw fault 읽기를 구현한다. 준비 상태로 지정한 I²C master에만 연결하는 `StockPowerReadOnlyDxe`를 추가했고, register-pointer 1바이트 + repeated START 읽기와 오류 전파를 모의 검사했다. 순정 ARM64 충전 상태 함수의 charge/VBUS 32조합과 boost getter의 제어 레지스터 256조합을 실행해 새 C의 결과와 일치하는 것을 확인했다. 순정 OTG 켜기·끄기 두 경로도 실행해 regmap/GPIO 호출 순서와 물리 출력 인자를 확인했다. Boost는 칩 식별 뒤 원본 필드 표의 reg0x03 bit5만 읽으며 프로토콜 revision 2의 끝에 getter를 추가했다. USB source의 제한된 boost 켜기/복원은 별도 코어와 보드 연결부에 구현했다. 충전 설정·PD 협상은 남아 있다. 상태 레지스터 읽기가 latch를 지우는지는 미확인이며, 보드 제공자가 이를 허용해야 한다.
- Windows 터치는 같은 `HimaxCoreLib` 코드를 사용한다. ARM64 KMDF/VHF 터치 드라이버의 빌드·로컬 검사와 실제 Windows 로드는 별도 상태다. 상세 상태는 [HimaxTouch/README.md](../../../Windows/HimaxTouch/README.md)를 따른다.
- Linux는 순정 모듈 429개와 활성 compatible DT 노드 537개 전부를 대조했으며, 원본 모듈은 DT·필수 심볼/CRC 공급자를 포함한 이름 219종·파일 220개를 기존 백업에서 출처·해시와 함께 추출했다. 정확한 순정 Image의 심볼 CRC·필수 의존 관계·DT 공급자 순서와 모듈 무결성을 검사한다. rfkill 두 변형은 보존하고 정확한 Image 릴리스와 유일하게 일치하는 파일만 적재 계획에서 선택한다. Wi-Fi/BT 73종 등 11개 분야 순서를 준비했다. PID1의 별도 작업자가 선택한 분야를 시작하고 제한 시간·실패·장치 연결 결과를 메모리에 기록한다. 외장 전용 EFI 앱에는 자동 적재 없는 진단 항목과 입력·화면·저장장치·USB 시작 항목이 있다. 원본 vendor 펌웨어 32개를 해시 검증 뒤 초기 CPIO의 /lib/firmware에 포함하고 정확한 커널의 firmware_class.path 매개변수도 확인했다. 정확한 Image의 보호 심볼 590개·가져오기 허용 심볼 9,176개와 공개 인증서 기반 PKCS#7 서명 7개를 확인했고, 11개 적재 계획의 정적 정책 검사를 통과했다. 미확정 정책이나 바뀐 서명은 적재 전에 차단한다. 실제 모듈 적재·화면 서비스·RIL은 미실행 또는 미구현이며, [Linux/README.md](Linux/README.md)에 검사와 남은 조건을 기록한다.

`inspect-stock-kernel.py`는 순정 boot_a의 커널을 메모리에서만 풀고 kallsyms 125,572개로 export 위치를 찾는다. export 8,973개의 실제 PREL32 이름·대상·CRC 표 길이와 값을 교차 확인했다. 주소 판별 도구의 휴리스틱 경고도 결과에 남겼으며, 이를 무시하고 주소만 신뢰하는 방식은 쓰지 않았다. 커널 export 증거는 `analysis/stock-kernel-exports.json`이다. 선택한 모듈의 import 13,246건을 이 커널과 전체 순정 모듈 공급자에 대조해 CRC 불일치·미해결 항목·없는 공급자는 0건이었다. `.modinfo depends`에 정확히 적히지 않은 공급자 참조 32건과 `rfkill` 변형 참조 12건은 따로 남겼다. 이 검사는 새 커널이나 실제 적재의 호환성을 보증하지 않는다.

검사된 함수의 동작과 전체 장치 구동을 구분한다. HX83132 코드가 들어 있다는 사실만으로 실기기의 칩 ID나 바인딩을 확정하지 않는다. Linux ELF 모듈은 Windows PE 드라이버로 직접 적재할 수 없으며, Android 전용 GPU·RIL·카메라 사용자 라이브러리도 일반 Linux 사용자 공간과 바로 연결되지 않는다.

## 빌드와 검사

작업 경로는 `/home/builder/teclast-t70pro`이며 `env.sh`에 기존 가상환경과 도구 경로가 있다.
ARM64 네이티브 BaseTools를 사용한다. 필수 도구는 clang/lld/llvm, GNU AArch64 도구,
ACPICA의 iasl/acpiexec, dtc/fdtget, lz4이다. 검사 전용 Python 의존성은
이 폴더의 `requirements-test.txt`에 고정했다. 백업 모듈 분석에는 erofs-utils와 e2fsprogs도 사용한다. 순정 커널 심볼 분석 도구의 minilzo를 빌드하려면 가상환경 Python 버전과 맞는 개발 헤더가 필요하다.

```sh
cd /home/builder/teclast-t70pro
source env.sh
# BaseTools가 없을 때만 실행한다.
with-build-lock T70Pro "Build native BaseTools" -- \
  make -C Mu-Silicium/Mu_Basecore/BaseTools/Source/C -j6 HOST_ARCH=AARCH64

# ACPI만 빌드하거나, 초기 펌웨어 전체를 빌드한다.
bash Mu-Silicium/Platforms/Teclast/t70proPkg/build.sh acpi DEBUG
bash Mu-Silicium/Platforms/Teclast/t70proPkg/build.sh firmware
# 기존 FV를 보존하며 같은 소스에서 별도 출력 디렉터리를 선택한다.
T70_BUILD_OUTPUT=Build/t70pro-verify bash Mu-Silicium/Platforms/Teclast/t70proPkg/build.sh firmware
# 링크 도구 없이 전체 DSC/FDF/INF 의존성·AutoGen만 검사한다 (FD 생성 아님).
T70_BUILD_OUTPUT=Build/t70pro-metadata BUILD_ACTION=genc \
  bash Mu-Silicium/Platforms/Teclast/t70proPkg/build.sh firmware

python Mu-Silicium/Platforms/Teclast/t70proPkg/verify-acpi.py \
  --dtb analysis/stock-fdt/effective.dtb \
  --build-dir Mu-Silicium/Build/t70proPkg/RELEASE_CLANGPDB

python Mu-Silicium/Platforms/Teclast/t70proPkg/verify-boot.py \
  --image Mu-Silicium/Build/t70proPkg/RELEASE_CLANGPDB/T70Pro-UEFI.Image \
  --fd Mu-Silicium/Build/t70proPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd \
  --boot Mu-Silicium/Build/t70proPkg/RELEASE_CLANGPDB/T70Pro-UEFI-boot.img \
  --dtb analysis/stock-fdt/effective.dtb \
  --preloader /mnt/mac/Users/you/.tmp/teclast-t70pro/device-backup/images/ufs-lu0.img
```

빌드 스크립트는 공용 `build.lock`을 잡고 최대 6개 작업으로 실행한다.
`BUILD_ACTION=genc`로 먼저 설정과 의존성만 검사할 수 있다.
출력의 `FV/SILICIUM_UEFI.fd`는 4MiB UEFI 본체이며, `T70Pro-UEFI.Image`는 시작 코드를
앞에 붙인 Linux Image이다. `T70Pro-UEFI-boot.img`는 이를 순정과 같은 legacy LZ4로
압축해 Android boot header v4에 담은 서명 없는 시험 이미지다. AVB 서명은 포함하지 않는다.

DTB 전달 코드의 실제 C와 기존 libfdt를 네이티브로 검사한다. 이 검사는 UEFI 서비스와 캐시를 대체하며 기기를 실행하지 않는다.

```sh
with-build-lock T70Pro "Check runtime DTB handoff" -- \
  "$PYTHON_COMMAND" Mu-Silicium/Platforms/Teclast/t70proPkg/verify-dtb.py \
  --dtb analysis/stock-fdt/effective.dtb

# 원본을 읽기 전용으로 열고, 논리 파티션은 VM 메모리 안에서만 읽는다.
"$PYTHON_COMMAND" Mu-Silicium/Platforms/Teclast/t70proPkg/inspect-stock-modules.py \
  --backup /mnt/mac/Users/you/.tmp/teclast-t70pro/device-backup \
  --hardware analysis/runtime-hardware.json --output analysis/stock-modules.json
```

macOS에서 builder의 원본 트리를 직접 사용할 때도 같은 `build.sh`를 쓴다. `tools/native-macos`의 별도 호스트 실행 파일과 기존 ACPICA 도구를 사용하며 Linux용 실행 파일은 보존한다. `T70_EDK_ZIG`에 기존 Zig 경로를 지정하고 GNU make를 설치해야 한다. Apple Clang의 LTO 형식이 대체 링커와 맞지 않아 이 경로에서만 `T70_NO_LTO`를 적용한다. 전체 RELEASE 빌드와 생성 이미지 검사를 통과했으며 소스·출력 경로는 같은 한 벌이다. 호스트 도구의 해시는 `analysis/native-macos-edk-tools.json`, 펌웨어 결과는 `analysis/build-summary.json`에 기록한다. `verify-boot.py`와 `verify-acpi.py`의 `--python-dtb`는 기존 순정 DT 파서를 사용하므로 fdtget 없이 같은 DT를 검사할 수 있다.

MH248 자석식 덮개는 GPIO0 원시 입력 0이 열림·1이 닫힘이다. Linux의 별도 런타임 DTB에 `gpio-keys`의 `SW_LID`·열림 쪽 깨우기를 연결하고, Windows용 ACPI 덮개 상태·변화 통지도 구현·오프라인 검사했다. Linux 입력·센서 적재에서는 중복 GPIO 요청을 막도록 기존 hall 모듈을 제외한다. 같은 GPIO의 입력과 IRQ 공유, 덮개 인계 초기화·제공자·DSDT를 새 펌웨어에 포함했다. 절전 중 전원과 깨우기 경로는 [CHECKLIST.md](CHECKLIST.md#자석식-덮개-구현)에서 별도로 추적한다.

## 메모리 근거

백업의 UFS 부트 영역 `ufs-lu0.img`에서 GFH와 프리로더 예약 표를 읽었다.
파일의 `0x553b0`에 `mb_kernel = [0x40000000, 0x47c80000)`, `0x553d8`에
`mb_dtb = [0x47c80000, 0x48080000)` 설정이 있다. 예약 루프는 `0x29fb4`에서 시작하며,
`0x29ff8`에서 예약 함수를 호출하고 반환 주소가 요청 주소와 다르면 오류로 처리한다.
이는 저장된 프리로더의 정적 분석 결과이며 실제 부팅 시 읽은 메모리 표는 아니다.
원본 해시와 파일 위치는 `analysis/lk/preloader-memory.json`에 보관한다.

| 용도 | 시작 주소 | 크기 |
| --- | --- | --- |
| 입력 커널 이미지 보존 | `0x40000000` | 8MiB |
| 초기 사용 가능 RAM | `0x40800000` | `0x0377f000` |
| 부팅 전달 정보 | `0x43f7f000` | 4KiB |
| UEFI 스택 | `0x43f80000` | 512KiB |
| UEFI 본체 | `0x44000000` | 4MiB |
| DXE 작업 메모리 | `0x44400000` | `0x03880000` |
| 부트로더 DTB 보존 | `0x47c80000` | 4MiB |

초기 메모리 표에는 `mb_kernel` 밖의 RAM을 사용 가능 영역으로 추가하지 않는다.
GIC·watchdog과 주변장치 MMIO 22개의 주소·크기는 순정 DTB와 대조한다. watchdog의 256바이트 레지스터 영역은
MMU 페이지 크기에 맞춰 4KiB로 매핑한다. 보드용 SEC는 기존 MT6789 코드와 같은 키로
watchdog을 끄고 부팅 단계 기록을 갱신한다.

## 시작 코드와 확인 범위

`BootShim.S`는 `0x40000000` 진입, EL1/EL2, MMU와 데이터 캐시 꺼짐,
DTB 주소·정렬·매직·크기를 확인한다. 조건이 다르면 쓰기 전에 대기한다.
검사를 통과하면 UEFI 본체를 겹치지 않는 영역에 복사하고 캐시를 처리한 뒤 진입한다.
`x0`의 DTB 주소를 보존한다. SEC/DXE가 실행될 때의 실기기 캐시 상태와 보안 펌웨어의
실제 인자 전달 방식은 아직 확인하지 못했다.

`0x43f7f000`에는 UINT64 8개를 기록한다: 매직/버전, DTB 주소, DTB 크기,
입력 이미지 주소, CurrentEL, MPIDR, SCTLR, 단계. 매직/버전은 `0x0000000148303754`이다.
단계 1은 검사 통과, 2는 복사 완료, 3은 SEC C 진입, 4는 DXE 드라이버 진입을 뜻한다.
DXE는 추가로 `0x43f7f040`에 EFI_SYSTEM_TABLE 주소를 기록하고 이 페이지의 캐시를 비운다.
DXE는 원래 DTB의 주소·길이·전체 구조를 libfdt로 검사하고 ACPI reclaim 메모리에 그대로 복사해
`gFdtTableGuid` EFI 구성 표에 게시한다. `0x43f7f048`에는 복사본 주소, `0x43f7f050`에는 크기,
`0x43f7f058`에는 게시 결과 EFI_STATUS를 기록한다. 실패 시 주소와 크기는 0이며 실패한 할당은 반환한다.
`0x43f7f080`·`0x43f7f090`·`0x43f7f0a0`·`0x43f7f0b0`에 I²C·GPIO·터치·UFS 준비 값 16바이트씩을 두고 `0x43f7f0c0`에 화면 정보 48바이트를 둔다. USB 인계는 별도의 `0x43f7f100`에 64바이트를 둔다. BootShim이 기존 112바이트와 이 64바이트를 지우고 중간 16바이트는 보존하며, 터치 인계는 준비 여부·주소 0x48·IRQ 핀 9·형식 버전 1이다. 반복 EBS에서는 이전 터치 준비 값을 취소한다. 기존 8개 필드와 단계 4의 뜻은 유지한다. 정적 DTB로 대체하거나 예약 영역·모뎀 속성을 수정하지 않는다.

`verify-dtb.py`는 이 DXE의 실제 C와 수정한 BaseFdtLib를 사용해 순정 DTB·합성 런타임 DTB에서 각각 18개 경우를 확인한다.
합성 자료에는 8GB RAM과 4GB 위 모뎀 예약 영역을 넣어 바이트 단위 보존을 검사한다.
기본 libfdt에 이미 있는 `fdt_check_full()`을 EDK2에서 쓰도록 Mu_Basecore의 FdtLib API 두 파일만 확장했다.
이 기록과 진짜 런타임 DTB를 실기기에서 회수하는 경로는 아직 연결하지 않았다.
DTB 게시로 전체 UEFI RAM 표나 화면·통신 드라이버가 구현되는 것은 아니다.

검사는 실제 생성한 Image의 ARM64 명령을 Unicorn에서 실행한다.
EL1/EL2의 본체 복사·DTB 보존과 잘못된 주소·DTB·캐시 상태 등 14개 경우를 확인한다.
정상 경로는 FD와 전달 정보 영역만 수정하고, 거부 경로는 쓰기 0바이트를 확인한다.
EL0 시험은 CurrentEL 읽기에서 CPU 예외로 중단됨을 확인한다.
EL3와 MMU가 켜진 실행, 실제 캐시 동작, SEC/DXE 실행과 주변 장치는 이 검사의 범위 밖이다.

## ACPI와 남은 연결

`Silicium-ACPI`의 MT6789 MADT/GTDT/FADT/PPTT 및 T70Pro DSDT를 사용한다.
CPU 8개, MPIDR `0x000..0x700`, 6+2 클러스터, GICv3, 타이머 IRQ와 PSCI SMC를
순정 DTB와 대조하고 DSDT의 CPU UID도 ACPICA에서 실행한다.
GICv2 호환 인터페이스는 기술하지 않으며 CPU Interface Number는 0이다.
CPU의 동일한 구현 리비전, 캐시 용량 등 확인하지 못한 값은 표에 주장하지 않는다.
규격 근거: [ACPI 6.6 MADT/PPTT](https://uefi.org/specs/ACPI/6.6/05_ACPI_Software_Programming_Model.html).

기기 실행 전에 남은 구현과 근거 확인은 CHECKLIST에 기록한다. 이후 별도 승인이 있어야
이 시작 코드의 실기기 진입과 부팅 시 전달된 DTB 회수를 확인할 수 있다.
그 DTB의 실제 RAM 예약 영역·프레임버퍼 정보를 확인한 뒤 전체 메모리 표와 화면을 연결한다.
UFS·USB의 초기화·전원 역할 연결과 실제 장치 검증이 더 필요하다. 현재 이미지는 Linux/Windows를 사용할 수 있는
완성된 펌웨어가 아니다. 2026-11-09 ADB 확인값은 `flash.locked=1`,
`vbmeta.device_state=locked`, `verifiedbootstate=green`이다. 잠금 해제나 기기 쓰기는 수행하지 않았다.

Windows `MtkDisplay` 표시 전용 드라이버를 추가했다. GOP→ACPI→POST 소유권을 대조하고
네 방향 회전·모드 적용·화면 복사·종료 인계를 수행한다. GOP 코어는 줄 끝 패딩도
지원한다. 소스·ARM64 모의 실행과 실제 패널/Windows 실행은 구분하며,
패널/DSI 전원·백라이트·절전·Mali 가속은 아직 완성되지 않았다.
자세한 범위는 [드라이버 설명](../../../Windows/MtkDisplay/README.md)을 따른다.

공통 C 검사 33개와 실제 FV의 주변장치 15개·표준 부팅 의존 11개·ACPI 5개 대조를 통과했다. GOP/UFS는 런타임 FDT·EndOfDxe 이후 재시도하며 UFS 읽기 전용 자식의 표준 UFS 장치 경로를 처리한다. Linux의 명시적 DRM/KMS 출력 프로그램과 Windows의 D3D11/12 WARP 검사·표시 프로그램도 추가했다. WARP는 CPU 실행이며 Mali WDDM 가속은 구현하지 않았다.

순정 LK OVL 레이어 설정 함수와 해석 코드를 `verify-mediatek.py --tests DisplayCoreTest RuntimeMemoryTest FramebufferGopDxeTest --display-lk`로 대조한다. 복사한 레지스터 페이지에서 DMA 주소·크기·pitch·알파 제어를 얻는 단계이며, 현재 활성 출력·CPU 물리 주소·불투명 출력을 입증하는 GOP 공급자는 아직 연결되지 않았다. 고정 예약의 다중 `reg`는 GOP 파서에서도 모두 검사하며, 한 범위가 전체 화면 영역을 포함할 때만 허용한다.

### ACPI 오프라인 빌드와 실제 FV 연결 검사

전체 EDK2 빌드는 `build.sh firmware`를 사용한다. Mac에서는 기존 Zig 실행 파일을
`T70_EDK_ZIG`로 지정해야 하며, 아래 호스트 검사로 링커나 전체 펌웨어 빌드를 대체하지 않는다.

```sh
python3 Platforms/Teclast/t70proPkg/build-acpi-host.py \
  --output-dir ../analysis/acpi-host-NEW \
  --autogen Build/t70proPkg/RELEASE_CLANGPDB/AARCH64/t70pro/AcpiTables/DEBUG/AutoGen.h \
  --dtb ../analysis/stock-fdt/effective.dtb \
  --acpica-bin ../tools/acpica/generate/unix/bin
python3 Platforms/Teclast/t70proPkg/verify-acpi-fv.py \
  --build-dir Build/t70proPkg/RELEASE_CLANGPDB \
  --tables-dir ../analysis/acpi-host-NEW
```

첫 명령은 원본 ASL/C 표를 새 출력 폴더에 생성하고 stock DTB 및 가상 AML 회귀를
검사한다. 명시한 기존 AutoGen.h의 PCD를 사용하므로 전체 DSC 재생성 검사는 아니다.
두 번째 명령은 실제 FD의 압축 FV에서 오디오·Bluetooth·Wi-Fi·셀룰러 SSDT를 포함한 ACPI 표 9개와 설치 모듈의 실행 파일·의존성을 읽는다.
표는 새 생성물과 대조하되 설치 시 재계산되는 checksum과 IASL compiler revision만
제외한다. AcpiTableDxe의 PCD 의존성과 AcpiPlatform의 ACPI+PCD 의존성을 확인한다.
이 검사는 기존 FV의 포장·바이트 연결을 증명하지만 DXE 실제 디스패치나 Windows/기기
부팅을 증명하지 않는다. 설치 후 RSDP/XSDT/FADT→DSDT 연결은 별도 실행 검증이 필요하다.

### 배터리·thermal 공급자 경계

순정 DT의 배터리 게이지는 PMIC 하위 `mediatek,mt6358-gauge`이다. SC89890H
충전기 상태만으로 잔량·방전 전류를 만들지 않는다. 순정 `mt6358_battery.ko`의
전류 읽기는 `0x0d0a` latch 설정/해제와 `0x0d8a` 읽기, sense 저항 및 car-tune
보정을 사용한다. DT `r-fg-value=5`, `car-tune-value=101`은 순정 파서에서 각각
10배로 저장된다. 이 초기값과 실제 런타임 보정 상태를 혼동하지 않는다.

PMIC AUXADC battery channel 0은 trigger `0x1108` bit 0/output `0x1088`,
15bit·저항비 3:1이다. 채널 3의 배터리 온도 ADC는 trigger bit 3/output `0x108e`,
12bit·저항비 2:1이다. 변환 trigger, ready poll, channel callback 및 efuse 보정이
필요하므로 단순 read-only 레지스터 노출이 아니다. 별도의 AP NTC는 AUXADC
`0x11001000`의 채널 0/1이고 SoC 센서는 LVTS `0x1100b000`이다.

보드·LTE NTC는 `MtkThermal` Windows 드라이버에서 요청마다 현재 ADC 값을 읽는다.
순정 DT의 변환표와 eFuse 보정값을 적용한다. [NTC 드라이버](../../../Windows/README.md#보드·lte·soc-온도-센서)에 사용 방법을 정리했다.

이미 설정된 SoC LVTS의 CPU·GPU·모뎀 센서도 Windows에서 읽을 수 있게 연결했다.
UEFI Device Check에서 `G`를 누르면 MT6366 배터리 게이지의 순간 전류와 PMIC ADC의 배터리 전압과 온도를 읽는다. 전압은 순정 DT가 지정한 MT6358 AUXADC eFuse 네 행과 PMIC 온도 보정값을 사용해 mV로 표시한다. 보정값 읽기나 변환에 실패하면 전압 원시 코드를 표시한다. ADC는 채널별 대기 후 준비 비트를 확인한다. 공유 PMIC 버스는 샘플 하나의 요청과 응답을 처리하는 동안 해당 요청이 소유한다. 전류 래치 해제를 확인할 수 없으면 다음 전류 요청을 막고 나머지 레지스터를 통한 요청은 계속 받는다. 배터리 온도는 순정 게이지의 21점 NTC 표에 배터리 온도·VBIF ADC와 순정 DT 초기값으로 환산한 순간 전류를 적용해 표시한다. VBIF 변환 뒤 제어 레지스터를 복원했는지 확인한다. 온도 계산에 실패하면 해당 ADC 원시 코드를 표시한다. Windows 열 구역은 순정 DT의 SoC 위험 온도 113.5°C와 NTC0의 최고 온도 응답에 연결했다. 배터리 용량 산출과 Windows의 `_BST`, LVTS 초기화·전원 손실 복구는 남아 있다.
PMIC 경로에는 공유 버스 소유권, 변환·정리 동작, 보정값 전달과 알림이 필요하다.

## XNU 초기 부팅 연결

`bash Platforms/Teclast/t70proPkg/build.sh xnuboot`로
T70 Pro 실행기와 고정 버전 DarwinBoot의 AARCH64 EFI 두 개를 만든다.
기존 펌웨어에 자동 부팅 항목을 추가하지 않고 외장 매체의 명시적 실행 경로를 준비한다.
소스 준비·검사·입력 경로와 남은 커널/ADT 작업은 [XNU 설명](Xnu/README.md)을 따른다.
실제 XNU 커널 빌드와 태블릿 부팅은 미실행이다.
