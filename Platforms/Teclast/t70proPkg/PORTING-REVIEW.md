# T70Pro G3D1: DTB·MT6789 커널·EDK2 대조 검토

2026-11-10. 현재 결론은 **EDK2의 저장장치·USB 연결부를 구현할 근거가 있으며, Linux는 순정 커널 조합을 유지하는 경로부터 준비할 수 있다**는 것이다. Windows 실사용에는 별도의 플랫폼 드라이버가 필요하다. 여기서 확인한 코드 결함을 POCO M5의 과거 실패 원인으로 확정하지 않는다. 해당 기기의 실행 로그는 확보하지 않았다.

이 문서는 구현 전 검토와 그 뒤 실제 수정 결과를 함께 기록한다. 주변장치 15개와 표준 부팅 의존 11개를 FV에 포함해 현재 PE와 대조했다. 태블릿 실행은 미실행이다. 실사용 항목과 설치 승인 조건은 [CHECKLIST.md](CHECKLIST.md)를 따른다.

## 현재 EDK2 작업 (2026-11-13)

SD의 빈 슬롯·제거·재삽입 처리와 I²C0/6의 APDMA/main gate·입력 핀 준비를 추가했다. 선택한 실제 C 프로그램 9개의 ASan/UBSan 검사는 통과했고 상위 SD hotplug 연결은 검토 중이다. 순정 LK의 패널 GPIO/지연 코어도 구현·검사했지만 DSI/PHY 초기화나 보드 연결은 포함하지 않는다. GPIO152가 Himax reset과 겹치므로 핀 소유권을 먼저 조정해야 한다. 최신 근거는 `latest_edk2_sd_i2c_gpio_panel`과 `analysis/platform-driver-verification.json`을 따른다.

독립 ID-low 허브/호스트·충전 보존과 인계 revision2의 UEFI·AML·Windows 소비자를 구현했다. 선택 C 12개 프로그램 1,395,406개(보드 1,284,172개 포함), Windows ARM64 콜백·SYS 및 전체 펌웨어 빌드가 통과했다. 실제 AML의 MUSB 조건 61개와 최종 FD의 필수 진입점 13개·인라인 보조 함수 5개·별도 보조 함수 5개 및 호출 관계 대조도 통과했다. 해시는 latest_edk2_id_low_usb를 따른다. 실물 배선·5V·입력과 PD/실행 중 역할/전원 복귀는 남아 있다.

USB source 시작·정리는 POWER_STATUS와 HT_STATUS(0x97)의 실제 safe0V 표시를 함께 확인하도록 보강했다. 마지막 SC RMW 이후 상태 변화도 다시 검사하며, 출력 복원 뒤 실제 전압 표시가 내려가기 전에는 정리를 완료로 표시하지 않는다. 전원 코어 3,365개·보드 193,012개 ASan/UBSan 검사와 순정 HUSB311/TCPM ARM64 8조합 대조를 통과했다. PLL gate/분주 출력/오디오 tuner도 실제 C 969개 검사와 순정 ARM64 14개 비교를 통과했으며, 최종 FD의 연결 코드를 확인했다. 선택한 C 검사 7개 프로그램의 합계 224,216회는 가상 MMIO/UEFI 실행 결과이며, 프로그램별 실행 묶음과 소스 해시는 `latest_edk2_pll_and_vsafe0v` 기록을 따른다.

LK 화면 메모리의 일반 RAM 재사용을 막고, 검증된 GOP 범위를 예약 할당과 비캐시 CPU 매핑으로 연결했다.
고정 예약은 실제 UEFI 할당 HOB로 보호한다. `no-map`은 CPU 매핑에서 제외하고, 다른 고정 예약은
BootServicesData로 표시한다. 중첩된 `no-map`이 우선하며 원본 preloader의 AEE 64KiB도 보호한다.

주소가 없는 `size` 예약은 OS가 ExitBootServices 뒤 배치해야 한다는
[Devicetree 규격](https://raw.githubusercontent.com/devicetree-org/devicetree-specification/main/source/chapter3-devicenodes.rst)에 맞췄다.
순정의 네 동적 요청을 검증하되 임의 주소를 배정하거나 FDT를 바꾸지 않는다. `reg`가 있으면 우선한다.
따라서 동적 요청 자체가 전체 RAM 공개를 막는다는 이전 판단은 정정한다. 원본 오프라인 DTB의 현재
중단 이유는 실행 중 LK가 채우는 CCCI 정보가 없기 때문이다.

주소 없는 동적 예약이 고정 화면 예약보다 앞에 있으면 GOP 파서가 중단되던 오류도 수정했다.
실제 C와 수정 전 FD에서 재현했으며, 요청 크기만으로 물리 화면 주소를 인정하지 않는다.
최근 선택 실행의 실제 C 프로그램별 ASan/UBSan 결과는 `analysis/mediatek-driver-selected-tests.json`에 기록한다. 이전 화면·메모리·USB·UFS·부팅 콘솔 검사와 이번 변경의 검사 범위는 최신 EDK2 상태 기록에서 구분한다.
최종 FD의 SEC·MemoryPeim·할당기·정렬·HOB·페이지 표 실행 결과는 `analysis/boot-verification.json`에 기록한다.
ArmMmuLib의 실제 페이지 표 생성 코드를 EL1·EL2 조건에서 실행하고, 전체 주소·캐시 속성·
비매핑 영역을 대조했다. CPU 기능·레지스터·캐시 정리·MMU 켜기 동작은 모의 처리했다.
검사기의 구조체 필드 위치 오류도 수정했고 실제 C의 크기·필드 위치 검사로 확인했다.
현재 결과가 이전 ARM64 메모리 형식 판독을 대체한다. 실제 MMU 설정·전체 DXE·기기 부팅 결과는 아니다.

순정 LK 초기 MMU 표와 초기 진입 코드에서 `0xffff000050f00000`에 대응하는 물리 시작 주소
`0x50f00000`을 확인했다. 화면 예약 검색·물리 주소 저장과 별도의 가상 주소 저장, 패널의
크기 getter 및 32픽셀 정렬 계산도 추적했다. 다만 현재 활성 버퍼와 하드웨어 픽셀 순서는
확정하지 못했으며, 이 정적 결과만으로 LK 전역값을 읽거나 GOP를 발행하지 않는다.

LK 유지 화면 공급자를 SEC와 GOP에 연결했다. 런타임 영상 태그 다섯 개와 정확한 DT 자원을 먼저 검사한 뒤,
DISP 전원·버스 보호·MMSYS 클록·OVL/RDMA/DSI 경로·SMI 물리 주소 모드와 안정된 레이어를 읽는다.
단일 비압축·비회전 format-2 화면만 받고, SEC는 기존 레이어의 픽셀 알파 무시 비트를 설정·재확인한다.
이때 전체 LK VRAM은 계속 보호하고 실제 화면 영역만 예약·UC 매핑한다. GOP에서는 쓰기 없이 재검사한다.
예약의 출처도 유지한다. 화면과 CCCI·memreserve·다른 고정 예약 풀이 겹치면 추가 RAM 등록을 중단하며,
LK 경로에서는 레지스터 접근 전에 검사한다. 표준 화면이 memory-region으로 직접 지정한 풀만 예외로 허용한다.
수정 전 합성 CCCI 중첩에서 잘못된 성공·알파 쓰기를 재현했고 새 검사는 이를 거부한다.
순정 오프라인 DT의 islcm_inited=0은 MMIO 전에 거부한다. 순정 LK의 경로 연결·시작 코드를 별도로 모의 실행했고,
순정 clock 모듈에서 상태 주소와 순정 DRM에서 선택 레이어 pitch bit28 설정도 대조했다.

USB는 HUSB311의 현재 source 역할·단일 Rd 연결과 VBUS 상태를 함께 요구한다. 준비된 호스트에서
모두 LOW인 순정 허브 핀 GPIO99→4→104를 켜며 각 단계의 전체 핀 값과 역할·컨트롤러 유휴를 다시 확인한다.
실패하면 직접 쓰기를 시도한 핀만 역순으로 정리하고, 외부 변경이나 재상승을 발견하면 소유 상태를 보존한다.
BDS는 SimpleTextIn 키보드를 ConIn에, SimpleTextOut을 ConOut/ErrOut에 등록한다.
최종 FV에 들어간 BdsDxe의 PE가 현재 빌드와 일치하고 보드 콘솔 함수들이 연결됐는지도 검사한다.

원본 TEE의 실제 ARM64 보안 처리기 31가지 분기로 C2000276 리셋 명령과 명령 8/32의 미지원 응답을 확인했다.
명령 32의 기본 정책은 엄격하다. T70 보드만 AllowSramRetention=1을 명시해 VEMC/SRAM 전원을 유지하는 얕은 Hibern8에서 callback이 정상 완료한 정확한 -1을 허용한다. off/on을 모두 보내며, off가 0으로 성공했던 경우 on의 -1은 거부한다. 전송 오류·-3·양수는 계속 실패하고 복구 실패 시 Fault와 소유권을 유지한다. 전원이 끊긴 뒤 SRAM이 복원됐다는 뜻이 아니다. 명령 16의 secure memory 경로는 의미를 확정할 근거가 없어 사용하거나 실행하지 않는다.
2026-11-13. UFS·GPU의 WACS2 통신을 공용 PRM 처리부에 연결했다. 두 처리 함수가 같은 잠금과 지속 상태 기록을 사용하며, 상대가 응답을 처리 중이거나 실패한 명령이 남아 있으면 새 명령을 차단한다. GPU는 허용된 레일 레지스터만 읽고 쓰며, 쓴 값을 다시 확인하고 응답을 확정한 뒤 결과를 넘긴다. Windows GPU의 PWRAP 직접 매핑을 없애 MFG0 자원을 5영역으로 맞췄다. UFS 전원 필터는 첫 호출부터 OS 재시작까지 PRM 모듈을 고정하고, GPU도 처리 여부가 불명확하면 모듈 고정과 실패 상태를 보존한다. 이 경로의 PRM 모듈 실행 중 교체는 지원하지 않는다. UFS의 공유 VEMC·TOP_UFS 유지형 정지·복귀는 계속 연결되어 있다. C 검사 프로그램 11개의 보고된 확인 항목 590,580개, Windows GPU ARM64 검사 18묶음, 새 PMIC 연결 5,364개·SoC 131,876개와 UFS 전원 필터 586개 조건이 통과했다. 최종 4MiB 펌웨어의 PRM 함수 36개 전체 명령어와 공용 통신 실행 27가지, 두 처리 함수의 332바이트 MEDT 및 잘못된 PE 메타데이터 20가지 거부도 확인했다. UFS 처리 코어의 연결부 실행 검사는 모의 호출이며 별도 C 검사로 실제 코어를 확인했다. 보안 영역·다른 펌웨어와의 접근 배제, 공유 부모 전원·기기별 OPP/DMA 입증, 전체 전원 손실 복구와 전체 DXE·OS·실기기 실행은 남아 있다. 근거는 project-state.json의 latest_edk2_shared_pmic_broker와 연결된 검사 기록이다.

GOP의 두 EBS 콜백 순서에 따라 정상 ready 기록이 지워지는 오류와, WP로 쓰기 금지된 화면을 ready로 표시하는 오류를 실제 C에서 재현하고 수정했다. 등록된 GOP가 인계 기록을 맡으며 화면이 없거나 다시 EBS를 시도하면 계속 준비 값을 취소한다. GOP DXE 2,774개 검사를 통과했다.
PSCI 라이브러리는 실제 소비자인 HwResetSystemLib도 선언해 ABI와 구현을 그대로 유지하면서 메타데이터 경고를 없앴다. 최종 FV의 runtime PE를 대조하고 ARM64 재부팅·종료·웜 재부팅 대체 경로 7가지를 실행했다. 보안 모니터 응답은 대체했으며 실제 전원 동작은 아니다.

이전 latest_edk2_usb_cold_start의 검사 이력: 초기 고정 Type-C source/DFP cold 경로의 클록·핀·PHY 보정 사전 확인·전원·SESSION/POWER와 실패 복구를 구현했다. 전체 보드 그룹 835,872개를 포함한 선택 C 프로그램 9개에서 ASan/UBSan 검사 940,086개를 통과했다. 전체 펌웨어 빌드와 최종 FD의 cold 함수 진입점 13개·인라인 보조 함수 5개의 포함 코드도 대조했다. 별도 cold 그룹 632,268개는 전체 수치와 합산하지 않는다. 실제 보드·Windows USB 열거는 미실행이다.
남은 EDK2 작업은 실제 화면/알파 반영과 LK RAM/CCCI 인계 대조, 꺼진 패널 초기화,
PLL 미설정 상태·USB-C PD/실행 중 역할 변경·독립 ID-low 포고핀/충전 경로, UFS PHY/전원 인계와 부팅 매체·PE/커널 진입이다.
상세 순서는 [CHECKLIST.md](CHECKLIST.md)를 따른다.

## 검토 뒤 구현한 부분

`MemoryMapLib`의 런타임 RAM·예약 구간 계산, PWRAP 오류 반환/10ms 제한,
PMIC LDO 선택 번호·VMCH_EINT_LOW·상태 복원, 공유 PLL 보존, MSDC1 읽기 전용 경로를 실제 C로 수정했다.
MSDC의 FIFO 32-bit 데이터/카운트, 64-byte SD SWITCH 전송, CRC/시간 제한/R1b busy,
25MHz SD에서 eMMC SWITCH를 보내던 분기도 고쳤다.
가짜 MMIO/UEFI 서비스와 실제 소스로 공통 검사 프로그램을 실행했고 ASan/UBSan 검사를 통과했다. ARM64 UEFI 컴파일도 통과했다.
검사 기록은 `analysis/mediatek-driver-tests.json`이며 소스 해시를 포함한다.

RAM 계산은 검증된 LK 유지 화면이 있으면 그 부분의 framebuffer 정보를 구성하고 예약·UC 매핑한다. CCCI 속성은 비교 커널처럼 little-endian native 구조로 읽고
커널이 매핑하는 64KiB 전체를 UEFI 가용 RAM에서 제외한다. 속성이 없거나 size가 잘못되면 bootstrap만 유지한다.
정확한 T70 런타임 구조와 모뎀 복귀는 미검증이다. 이 변경은 모뎀 드라이버 구현/통신 성공을 뜻하지 않는다.

GPIO 방향/입력·write-one alias·원자적 mux와 bit31 클럭 gate를 수정했다. 순정 DTB 기준 MMIO 15개도 추가했다.
UFS 기존 링크 인계와 LU2 읽기 전용 Block I/O, MUSB PIO 호스트/Operational/EBS 연결, 예약 프레임버퍼 GOP/장치 경로,
I²C 런타임 DT·클럭·핀·직접 DMA 확인/ACPI 전달과 Himax 간접 레지스터·칩 ID·리셋 없는 SenseOn을 구현했다. 이벤트 정리 실패 때 콜백이 참조하는 메모리와 드라이버 코드를 유지하도록 실패 경로도 보완했다. 해당 주변장치 DXE와 표준 USB/콘솔 코드는 DSC/FDF에 연결했고 최종 FV의 실제 PE 포함도 별도로 대조한다.
UFS 컨트롤러의 리셋·SMC 참조 클록·HCE/UniPro 초기화와 EBS 정지, v4 기기 POWERDOWN·Hibern8·HCE 정지 소비자를 연결했다. 다섯 소유 gate의 확보·실패 정리와 공유 VEMC 유지, 정지 뒤 재시작을 현재 코드에서 검사했다. 최신 실행 수와 최종 펌웨어 근거는 [ufs-platform.md](../../../../analysis/ufs-platform.md)를 따른다. OS 공용 전원 공급자와 전체 전원 손실 복귀, 실제 DMA·PHY 동작은 남아 있다. 다른 미완 항목은 아래 장치별 표를 따른다. 듀얼 부팅 개발은 현재 범위에서 제외한다. I²C Windows KMDF/SpbCx 코드와 Himax KMDF/VHF 코드는 작성했으며 실제 로드는 미실행이다.
아래 결함 설명과 `porting-review.json` 소스 해시는 **수정 전 검토 시점**의 기록이다.
현재 구현 범위와 실사용 조건은 README/CHECKLIST 및 `project-state.json`에서 구분한다.

Windows UFS Storport와 조건부 ACPI 인계도 구현했다. NOP·실제 LU2 용량·읽기·쓰기·캐시 비우기·중단/재시작과 오류 경로를 ARM64 모의 실행했다. 실제 Windows 로드·저장장치 쓰기와 완전히 꺼진 PHY/전원 복귀는 미실행이며, 구체적인 호출/한계는 [Windows/README.md](../../../Windows/README.md)를 따른다.

## 근거와 한계

- T70Pro: 원본 백업에서 추출한 vendor_boot DTB + 보드 DTBO를 오프라인으로 합친 `effective.dtb`. SHA-256 `a3dd45e28ee284f1995374091eca9634f68c88dfddc5ecd27a37cdcc877dd391`. 실행 중인 부트로더가 보정한 DTB는 아니다.
- 순정 커널: `6.12.38-android16-5-g3c4da6410bcb-ab13872285-4k`, 설정·EFI 진입점·원본 모듈 메타데이터를 확보했다. 405개 로드 모듈 이름이 백업에서 모두 발견됐다는 기존 결과는 실행 중인 바이너리의 해시 일치를 뜻하지 않는다.
- 비교 소스: [iKKO MindOne MT6789/MT8781 커널](https://github.com/iasnezhkov/android_kernel_ikko_mindone/tree/a71515dbaf0d539ffb36416f14186dad17eef743), `android16-6.12`, 커밋 `a71515dbaf0d539ffb36416f14186dad17eef743`. builder의 `kernel-reference` 한 벌에서 관련 경로만 꺼냈다. **Teclast 제조사 소스로 확인된 것은 아니다.** 이 소스의 다른 기기 동작 보고는 T70Pro 검증에 포함하지 않는다.
- EDK2: Mu-Silicium `20b82927b64f4d601d4f01e1bb3312071e437ba5`, Mu_Basecore `64f478939955e45faa632c160ae2a75a5ff7ef7f`, ACPI `8c4cdc43abf23cffdeb588621126253bb2c559da` 및 보존된 로컬 변경.
- 기존 [Status.md](../../../Status.md)의 Helio G99 / POCO M5 stone 항목은 화면·버튼만 지원으로 기록한다. 저장장치·USB·Windows/Linux 부팅은 미지원이다. 이는 해당 저장소의 지원 표이며, SoC 자체가 기능을 수행할 수 없다는 증거가 아니다.
- 추출한 17개 장치 노드·클록/전원 참조·소스 해시는 [porting-review.json](../../../../analysis/porting-review.json)에 기록했다. 정확한 T70Pro 커널 및 패널 이름으로 한 공개 검색에서는 제조사 소스를 찾지 못했다. 공개 소스의 부재를 확정하는 결과는 아니다.

## 보드 차이: 같은 칩이어도 가져오면 안 되는 값

| 항목 | T70Pro에서 확인한 값 | 참고 보드와의 차이·의미 |
| --- | --- | --- |
| UFS | `11270000`, 길이 `2300`, SPI 137 → GSIV 169. 클록 이름 `ufs, ufs_mp, unipro_tick, unipro_sys, ufs_aes, ufs_ck`. VCC는 PMIC VEMC. | MindOne는 8개 클록 이름과 별도 UFS Vcore 보조 장치를 추가했다. T70의 6개 이름을 그대로 새 코드에 넘기면 추가 이름을 요구하는 초기화와 맞지 않을 수 있다. 임의의 650mV/192MHz 정책을 옮기지 않는다. |
| UFS PHY | 정적 UFS 노드에 `phys`가 없다. 순정에서는 `phy_mtk_ufs` 모듈도 로드돼 있다. | 참고 `ufs_mtk_bind_mphy()`도 `-ENODEV`이면 PHY 없이 진행한다. 런타임 DT 보정·실제 연결을 추가 확인하고 T70에 확인되지 않은 M-PHY 주소·전원을 필수 자원으로 만들지 않는다. |
| microSD | MSDC1 `11240000`, TOP `11ef0000`, SPI 135 → GSIV 167. `cd-gpios`는 GPIO7/flags0. VMCH는 `VMCH_EINT_LOW`, VMC는 `ldo-vmc`. | MindOne는 GPIO14/active-low, `VMCH_EINT_HIGH`. 카드 감지 핀·극성·레귤레이터를 그대로 복사하면 안 된다. stone의 MSDC TOP 주소 자체는 T70 DTB와 일치했다. |
| 터치 I²C | `11e00000` + APDMA `10217080`, 400kHz. `mediatek,mt6983-i2c`. 자식 `himax_ts@48`, `himax,hxcommon`, IRQ GPIO9, reset GPIO152. 좌표 1400×2240. | MT6789라는 칩 이름으로 I²C 레지스터 변형을 선택하면 안 된다. 비교 커널에는 `mt6983_compat`가 명시돼 있다. 순정 바이너리의 HX83132 코드·이벤트 명령 0x30·56바이트 일반 보고 해석을 확인했다. 실제 칩 ID와 드라이버 연결은 미확인이다. |
| 충전·USB-C I²C | `1101a000` + APDMA `10217480`, 400kHz. SC89890H `0x6a`, HUSB311 `0x4e`. 각각 인터럽트 GPIO12/13. | MindOne의 ETA6965 및 RT1711H 드라이버를 T70 드라이버로 취급하지 않는다. TCPCI 계층 일부를 재사용해도 칩별 레지스터·확인 절차는 별개다. |
| 패널 | `/soc/dsi@14013000/panel1@0`의 `cyx,cv101wu_lh1038b`가 순정 패널 모듈 alias와 일치한다. | `chosen`에는 다른 nt36672e 패널 문자열이 남아 있다. 패널 노드+모듈을 우선 근거로 삼되, 실제 바인딩·프레임버퍼 주소/stride는 추가 확인한다. |
| 모뎀 DPMAIF | 버전1, capability `0x42`, reg 4개: `10014000, 1022d000, 1022c000, 1022e000`. SPI241 → GSIV273. | MindOne DT는 reg 7개와 capability `0x06`. 참고 코드에도 4개/7개 인덱스 경로가 공존한다. DT만 교체해서 올리지 말고 선택되는 코드·빌드 옵션부터 맞춘다. |

## EDK2에서 먼저 고쳐야 할 부분

### 1. RAM과 원래 부팅 상태 보존

[RamManagerLib](../../../Silicon/MediaTek/MediaTekPkg/Library/RamManagerLib/RamManager.c)의 `GetUsableMemoryRanges()`(111행)는 EMI CONA/CONH로 크기를 계산해 연속 범위 하나를 반환한다. 공용 RamManagerDxe는 **이미 UEFI 메모리 표에 있는 범위**를 피해 나머지를 추가한다. 런타임 DTB의 예약 영역을 먼저 표에 반영하지 않으면 모뎀·SCP·화면·보안 펌웨어 메모리가 일반 RAM에 포함될 수 있다.

검토 당시 작은 부트스트랩 메모리 표만 사용하던 부분은 이제 런타임 FDT 기반 RAM 계산으로 확장했다. 고정 예약·모뎀·AEE·펌웨어·DTB·화면 범위를 보호하고, `no-map`과 CPU 매핑을 구분한다. 주소가 없는 동적 `size` 요청은 OS 배치 대상으로 보존하며 잘못된 요청은 거부한다. 런타임 CCCI 정보가 없거나 범위를 해석할 수 없으면 부트스트랩 표를 유지한다. 실제 LK 인계 자료를 이용한 검증은 남아 있다. EMI 값은 대조 자료이며 런타임 예약 표의 대체물이 아니다.

이미 구현한 FDT 복사·게시 검사 36건은 이 RAM 계산의 검증이 아니다. `/chosen`의 모뎀 부팅 태그가 가리키는 메모리의 수명도 보존해야 한다. 포인터만 복사하고 그 대상 RAM을 재사용하면 셀룰러 부팅 정보가 깨질 수 있다.

### 2. PMIC·클록·GPIO: 실패가 멈춤으로 이어지는 경로 제거

재사용 근거가 있다. [MT6789 커널 PWRAP](../../../../kernel-reference/mindone/modules/mtk_pmic_wrap/mtk_pmic_wrap.c)의 `mt6789_regs`(1044행)는 EDK2 [PmicWrapperImplLib](../../../Silicon/MediaTek/MT6789Pkg/Library/PmicWrapperImplLib/PmicWrapperImplLib.c)의 `0x880/884/894/8a4/8a8`과 일치하고, 중재 방식의 FSM 비트 처리도 대응한다. 주소를 전부 새로 추측할 필요는 없다.

다만 다음 결함을 먼저 고쳐야 한다.

- [PmicWrapperDxe.c](../../../Silicon/MediaTek/MediaTekPkg/Drivers/PmicWrapperDxe/PmicWrapperDxe.c) `WacsWaitFor()`(56행)는 무한 대기다. 커널의 `pwrap_wait_for_state()`는 약 10ms 한도로 오류를 반환한다. UEFI에서도 타이머 기반 한도·유효값 비우기·읽기/쓰기 완료 확인이 필요하다.
- 같은 파일 `InitPmicWrapper()`(144행)는 초기화되지 않았어도 ASSERT 다음에 프로토콜을 등록할 수 있다. RELEASE의 DebugPropertyMask는 0이다. 실패 시 즉시 반환해야 한다.
- [MtkPmicWrapper.h](../../../Silicon/MediaTek/MediaTekPkg/Include/Protocol/MtkPmicWrapper.h)의 Read/Write 형식이 `VOID`라서 실패를 상위에 전달할 수 없다. 해당 프로토콜 및 모든 소비자를 함께 `EFI_STATUS`로 바꾸고 PMIC read-modify-write가 실패하면 뒤따르는 쓰기를 중단해야 한다. MT6358PmicDxe의 성공 반환도 이 변경과 함께 고친다.
- [MsdcImplLib.c](../../../Silicon/MediaTek/MT6789Pkg/Library/MsdcImplLib/MsdcImplLib.c) 생성자(169행)는 GPIO/Clock/PMIC 탐색 실패를 ASSERT로만 처리한다. 호출 순서용 APRIORI 외에도 실제 Depex와 실패 반환이 필요하다. 전압 설정을 라이브러리 생성자에서 수행하지 않고 해당 장치의 명시적 초기화 단계로 옮긴다.

클록 이름·부모·게이트는 기존 MT6789 ClockImplLib를 재사용 후보로 삼는다. 실제 T70 phandle/클록 이름에 맞게 연결하고, 필요 장치의 클록만 켠다. 다른 보드의 전체 클록 덤프를 읽거나 모든 전원 영역을 켜서 해결하지 않는다.

### 3. microSD: 기존 드라이버를 고쳐 사용할 수 있는 경로

[MsdcDxe.c](../../../Silicon/MediaTek/MediaTekPkg/Drivers/MsdcDxe/MsdcDxe.c) `InitMsdc()`(1370행)는 0부터 `NumberOfHosts=2`까지 순회하고 각 호스트를 초기화한 뒤 eMMC/SD 식별을 시도한다. 그런데 UFS 설정에서 [MsdcImplLib](../../../Silicon/MediaTek/MT6789Pkg/Library/MsdcImplLib/MsdcImplLib.c)의 Index0도 SD용 GPIO·VMCH/VMC 분기를 탄다. 실제 없는 eMMC 경로를 탐색하면서 엉뚱한 컨트롤러와 전원을 만질 수 있다.

`NumberOfHosts=1`만 바꾸면 인덱스0을 남기므로 해결되지 않는다. **사용할 컨트롤러 ID를 명시하는 목록**을 두고 T70는 MSDC1만 열어야 한다. `no-mmc`, GPIO7 카드 감지, VMCH_EINT_LOW와 VMC, SD 핀의 mode/pull/drive를 T70 노드에서 연결한다. 초기는 3.3V·400kHz 식별 후 25MHz 등 보수적인 단계로 진행하고, 1.8V 전환·고속 튜닝은 별도 단계로 둔다.

기존 코드의 reset(198행), FIFO clear(211행), clock stable(329행), OCR busy(1262/1334행) 대기에 한도를 추가해야 한다. 실패 시 카드 없음/시간 초과를 구분하고 풀·프로토콜·클록/전원 상태를 정리한다. 비교 커널의 일반 `drivers/mmc/host/mtk-sd.c`에는 MT6789 match가 없으므로 이것을 완성된 T70 SD 드라이버로 오인하지 않는다.

초기 디스크 인터페이스는 읽기 전용으로 설계한다. 이 코드의 실제 카드 접근과 쓰기 검증은 현재 권한에 포함되지 않는다.

### 4. UFS: 범용 코어 앞에 MT6789 연결부가 빠져 있다

현재 MediaTek DXE 경로에서 UFS 호스트 프로토콜 생산자를 찾지 못했다. MSDC 드라이버로 UFS를 제어할 수 없다. 재사용할 범용 코어는 이미 있다.

`MT6789 UFS 호스트 연결부 → UfsPassThruDxe → ScsiBusDxe → ScsiDiskDxe → Block I/O`

[UfsHostController.h](../../../Mu_Basecore/MdeModulePkg/Include/Protocol/UfsHostController.h)의 MMIO/Allocate/Free/Map/Unmap/Flush/Read/Write를 비 PCI 플랫폼 장치로 구현한다. DMA 주소 폭은 컨트롤러 capability를 따르고, 캐시 정리와 장치 주소 변환을 처리한다. IOMMU나 secure DMA 경로가 확인되지 않았으면 단순 포인터 변환으로 대신하지 않는다.

[UfsHostControllerPlatform.h](../../../Mu_Basecore/MdeModulePkg/Include/Protocol/UfsHostControllerPlatform.h)의 Pre/Post HCE·LinkStartup 콜백이 커널의 `ufs_mtk_hce_enable_notify()`(262행), `ufs_mtk_link_startup_notify()`(1482행)를 연결할 자리다. [ufs-mediatek.c](../../../../kernel-reference/mindone/modules/ufs_mediatek/ufs-mediatek.c)에서 필요한 클록·VCC·reset 순서·XOUFS 클록 우회·UniPro 설정을 추려 옮긴다. `ufs_mtk_setup_ref_clk()`와 [SiP 헤더](../../../../kernel-reference/mindone/modules/ufs_mediatek/ufs-mediatek-sip.h)의 secure firmware 호출은 T70 펌웨어 지원·반환값을 검증해야 한다. 확인되지 않은 SMC 실패를 무시하고 다음 단계로 진행하지 않는다.

초기 목표는 최소 전송 모드의 읽기다. Linux용 DVFS·runtime PM·WriteBooster·inline crypto 전체를 EDK2로 옮길 필요는 없다. 기본 UfsPassThruDxe도 초기화 중 `fDeviceInit`을 설정하며 RefClkFreq 불일치 시 UFS attribute를 쓸 수 있다(937행). **Block I/O의 ReadOnly만 켠다고 초기화 전체가 저장장치 설정을 변경하지 않는 것은 아니다.** SCSI 쓰기/UNMAP·UFS 설정 쓰기·RPMB 접근을 포함한 진입점을 검토하고, 원본 보존 정책을 적용한 뒤에만 실기기 시험안을 만든다.

ExitBootServices에서는 진행 중 DMA·이벤트를 정리하고 OS가 컨트롤러를 다시 초기화할 수 있게 넘긴다. Linux에서 UFS가 됐다는 결과와 UEFI의 UFS, Windows의 UFS는 각각 기록한다.

### 5. USB·PD: MUSB용 호스트 코드와 전원 역할 제어 필요

T70는 MUSB USB2 `11200000` + PHY `11f40000`, SPI128 → GSIV160이다. xHCI 드라이버를 연결하는 방법으로 해결되지 않는다. 현재 MediaTek DXE 경로에는 이 MUSB를 구동하는 호스트 드라이버가 없다.

참고 [musb_main](../../../../kernel-reference/mindone/modules/musb_main/musb.c), [usb20_host.c](../../../../kernel-reference/mindone/modules/musb_hdrc_recon/usb20_host.c), [PHY](../../../../kernel-reference/mindone/modules/phy_mtk_tphy/phy_mtk_tphy.c)는 sys/ref/src 클록, PHY 모드, FIFO, endpoint, VBUS의 순서를 보여준다. EDK2에서는 `EFI_USB2_HC_PROTOCOL`을 생산하는 MUSB 호스트 연결부를 만들고 기존 UsbBus/USB 키보드·대용량 저장장치 코드를 재사용하는 방향이 맞다. 처음에는 PIO와 필수 control/bulk/interrupt 전송부터 검토하고, QMU DMA는 캐시·경계·취소·오류 복구를 별도로 검증한다.

호스트·장치·PD 세 상태는 각각 필요하다. HUSB311/SC89890H의 실제 전원 역할과 연결 상태를 확인하기 전에는 VBUS를 켜거나 역할을 강제하지 않는다. OS 이전에는 기존 충전 상태 보존을 우선하고, 충전기·OTG 액세서리·호스트 PC가 연결된 경우의 차이를 설계한다. 참고 `musb_glue_mt6789.c`의 PHY context save/restore는 빈 함수이므로 이를 절전 복귀 구현의 근거로 삼지 않는다.

Linux는 순정 MUSB/TCPC/충전 모듈을 유지하는 쪽이 먼저다. Windows 장치 모드는 UFX와 실제 컨트롤러 드라이버가 필요하고, USB 호스트도 별도의 MUSB 지원을 확인해야 한다. Microsoft의 [UFX 설명](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/developing-windows-drivers-for-usb-function-controllers)은 ACPI를 포함한 플랫폼 작업·검증이 필요함을 명시한다.

### 6. 화면·입력: 켜져 있는 화면을 인계받는 경로부터

기존 [SimpleFbDxe](../../../Silicon/Silicium/SiliciumPkg/Drivers/SimpleFbDxe/SimpleFb.c) `GetFrameBufferInfos()`(88행)는 메모리 영역 이름과 고정 PCD를 쓰며, `PixelsPerScanLine=Width`로 둔다(177행). stone 값은 1088×2408이다. T70의 1400×2240 수치만 대입해도 실제 stride/format/할당 크기·표시 방향은 해결되지 않는다. 영역 검색 두 번의 결과를 확인하지 않고 초기화되지 않은 descriptor를 검사하는 경로도 함께 정리해야 한다.

런타임 부트로더가 넘긴 framebuffer base/size/width/height/stride/format을 서로 대조하고 RAM 표에서 예약한 뒤 GOP를 게시하는 보드 연결부가 우선이다. 순정 LK 0x50d0c 레이어 설정 함수의 실제 ARM64 실행 결과와 새 OVL 해석 코드를 336가지 입력에서 대조했다. 이 코드는 DMA 주소·기하 정보·pitch·알파 제어를 해석할 뿐이며, 장치의 활성 경로·IOMMU 우회·픽셀 알파 무시 여부를 입증하지 않는다. 이 조건을 검증하는 공급자를 SEC와 GOP에 연결했고 실제 장치의 출력·인계 확인은 남아 있다. 고정 예약 노드의 다중 `reg`와 `memory-region` phandle의 여러 주소 범위도 처리하도록 공유 GOP 파서를 수정했다. 수정 전 실제 C와 FD의 거부를 재현했고, 수정 후 최종 FD에서 RAM 확장·예약 할당 HOB·UC 페이지 표 생성까지 대조했다. 범위·곱셈 넘침·실제 pitch·캐시 속성과 패널 유지 여부를 확인한다. 정적 `chosen`의 주소나 패널 문자열을 확정값으로 하드코딩하지 않는다.

panel1 노드에는 eDP enable/reset, backlight 전원, panel reset, AVDD/AVEE 및 1.2/1.8/3.3V 제어 GPIO가 함께 있다. 이것만으로 브리지 칩과 전원 투입 순서·DSI 명령을 알 수는 없다. 콜드 초기화는 정확한 패널 모듈/소스의 순서·지연을 추가 분석한 뒤 구현한다.

터치는 먼저 MT6983 변형 I²C master와 GPIO interrupt/reset이 필요하다. 그 위의 Himax 프로토콜은 순정 `hxchipset_i2c` 기준으로 확인한다. Himax 노드가 있다는 이유로 표준 I²C-HID로 등록하지 않는다. 물리 버튼도 PMIC/GPIO 읽기 실패 처리와 반복 입력을 검증한다. 순정 포고핀 키보드는 사용자가 순정 OS에서 USB 키보드로 열거됨을 확인했다. 터치패드 보고 형식과 포고핀의 실제 배선·전원 경로는 별도 확인이 필요하다.

## 초기 OS별 검토 당시의 기능 배치와 난점

아래 표는 초기 검토 당시의 판단이다. 이후 구현·빌드·실제 실행 상태는 현재 README/CHECKLIST와 `project-state.json`을 따른다.

| 기능 | Linux 준비 경로 | Windows 준비 경로 |
| --- | --- | --- |
| 부팅·CPU·RAM | 순정 EFI stub, 런타임 DTB, 4KiB 커널, 정확한 initramfs/모듈/펌웨어 조합을 먼저 유지. 8코어 SMP/PSCI/열 제한·절전은 각각 확인. | ACPI CPU 표 외에 전체 RAM, GOP, 디스크, 타이머/인터럽트, 실제 Runtime Services가 필요. core ACPI 5개와 비활성 I²C 두 버스의 DSDT 자원이 있으며, 보드 준비 상태 발행은 구현했고 현재 FV에 포함한다. |
| UFS | 순정 ufs_mediatek + reset/PMIC/클록 의존성을 유지. | [StorUFS](https://learn.microsoft.com/en-us/windows-hardware/drivers/storage/storufs-feature-support)는 존재한다. MTK의 clock/PHY/SMC/전원 인계를 표준 경로와 연결할 수 있는지 먼저 확인하고, 부족한 플랫폼 연결부 또는 드라이버를 결정한다. 기본 제공 드라이버의 T70 지원은 미확인. |
| USB·SD·입력 | 순정 컨트롤러/PHY/TCPC·충전·Himax 모듈과 보드 자원을 함께 유지. | MUSB, GPIO interrupt, I²C/SPB, Himax, SD 및 USB-C 전원 역할을 처리할 실제 ARM64 드라이버가 필요. 장치 ID나 `_CRS`만 만들고 지원 완료로 표시하지 않는다. |
| GPU·표시 | 초기 GOP/simple framebuffer와 3D를 분리. Mali kbase와 Android 사용자 라이브러리 조합을 보존하는 경로, Panfrost로 옮기는 경로를 따로 평가. | GOP는 초기 출력용이다. 지속적인 표시·가속에는 WDDM 및 플랫폼 전원/표시 드라이버가 필요. 확보한 자료로 지원을 입증하지 못했다. |
| LTE·SIM·문자·통화 | CCCI util/LK 태그 → modem FSM/전원·CCIF/SCP → DPMAIF → CCMNI → 제어 데몬/RIL·IMS·통화 오디오. 데이터·SMS·IMS·음성·수신 대기를 별도 통과 조건으로 둔다. | 내부 CCCI를 USB MBIM 장치처럼 열거해 해결할 수 없다. 실제 WWAN 데이터/제어 드라이버 및 SIM·SMS·전원 경로가 필요. 현재 확보된 Windows 드라이버는 없다. |
| Wi-Fi·BT·GNSS | conninfra/WMT·펌웨어·SCP·전원 의존성을 유지. Android 전용 사용자 서비스와 일반 Linux 연결 계층을 나눈다. | 실제 버스와 펌웨어 제어, NDIS/BT/GNSS 연결을 담당할 드라이버를 확인해야 한다. EFI 구현 대상이 아니다. |
| 오디오·3.5mm·카메라·센서 | MT6789 AFE/MT6366, 실제 앰프, DSP/SCP, ISP·센서 허브와 사용자 서비스 필요. 현재 여러 앰프 모듈의 로드만으로 장착 부품을 확정하지 않는다. | 오디오/ISP/센서·GPIO/I²C·전원과 플랫폼 드라이버가 필요. 범용 ACPI 표로 대체되지 않는다. |
| 배터리·충전·절전 | MT6358 계열 gauge·SC89890H·HUSB311, CPU/GPU DVFS·열 제한, suspend/resume·수신 대기까지 묶어 확인. | 배터리 보고와 실제 충전 제어, 열 제한·대기 전원 관리가 모두 필요. ACPI 잔량 숫자만 만드는 구현으로 완료 처리하지 않는다. |

Linux의 첫 목표를 최신 mainline으로 바로 정하면 기존 모듈 ABI와 플랫폼 의존성까지 동시에 바뀐다. 우선 순정 조합의 로더·초기 파일 시스템·장치 연결을 준비하고, 소스 기반 교체는 기능별로 나눈다. 참고 커널의 6.12.92 모듈을 T70의 6.12.38 GKI에 섞지 않는다. 이름·vermagic 외에 symbol CRC/KMI·provider·서명 정책·펌웨어/사용자 API까지 맞아야 한다. stock kbase와 Panfrost의 사용자 API도 호환으로 가정하지 않는다.

Mesa 공식 [Panfrost 표](https://docs.mesa3d.org/drivers/panfrost.html)는 Mali-G57의 OpenGL ES/OpenGL 경로를 제공한다. 이는 MT6789의 클록·전원·MMU·표시 장치 연결이 구현됐다는 뜻이 아니며, 해당 표의 G57 Vulkan 칸은 비어 있다. GPU 모델 지원과 이 보드의 전체 지원을 분리한다.

순정 설정에서 `CONFIG_ACPI`, `CONFIG_DEVTMPFS`, `CONFIG_VT`, `CONFIG_FB`, `CONFIG_DRM_SIMPLEDRM`, `CONFIG_DRM_PANFROST`, `CONFIG_DRM_PANTHOR`가 꺼져 있다. 따라서 현재 순정 커널은 ACPI로 장치를 찾지 않으며, GOP를 게시해도 커널 화면 콘솔이나 simpledrm이 자동으로 이어지지 않는다. initramfs에 필수 장치 노드를 준비하고 사용자 공간의 uevent 처리기로 `/dev`를 관리하는 구성이 필요하다. 화면은 순정 DRM과 사용자 라이브러리의 연결을 먼저 검토한다. simpledrm/VT/Panfrost 추가를 택하면 커널 변경·모듈 ABI 검증이 별도 작업으로 발생한다. UART 콘솔 설정은 있지만 보드에서 관찰할 경로는 미확인이다.

나중에 ACPI 지원 커널을 사용할 때는 [arm64 ACPI 문서](https://docs.kernel.org/arch/arm64/arm-acpi.html)의 DT/ACPI 선택 정책도 적용한다. Windows용 DSDT를 채우기 위해 Linux까지 ACPI 열거로 바꾸지 않는다.

현재 공통 설정은 emulated NV variables와 Null RTC를 사용한다. 재부팅 후 BootOrder/BootNext 유지와 실제 시간 서비스를 제공한다는 근거가 아니다. Windows는 [Microsoft SoC UEFI 요구 사항](https://learn.microsoft.com/en-us/windows-hardware/drivers/bringup/uefi-requirements-that-apply-to-all-windows-platforms)의 GOP·Block I/O·시간/변수·SMBIOS 등도 검토해야 한다. 이 문서는 Windows 10을 대상으로 한 설명이므로 설치할 Windows 버전의 추가 요구 사항은 별도 확인한다.

macOS/iPadOS는 별도 미해결 목표다. 이번 커널 대조에서 Apple 플랫폼 부팅·드라이버를 대체할 경로는 찾지 못했다. Linux 셀룰러 준비 결과를 macOS 셀룰러 지원으로 표시하지 않는다.

## 구현 순서와 통과 조건

| 순서 | 오프라인에서 할 구체 작업 | 실기기 시험 전 필요한 증거 |
| --- | --- | --- |
| 1 | 런타임 FDT 예약 해석·RAM 구간 계산과 framebuffer 정보 검증. 기존 BootStatusDxe 전달부를 재사용. | 합성/원본 자료의 범위·겹침·넘침·4GiB 이상·동적 예약 실패 처리 검사. 실제 런타임 FDT 수집은 별도 실행 승인 전까지 미실행. |
| 2 | PWRAP 반환 형식·시간 제한·실패 중단, PMIC/Clock/GPIO 및 MSDC의 실패 처리·Depex 정리. | 대체 MMIO로 FSM 고착·초기화 안 됨·전원/클록 실패를 주입했을 때 정해진 시간에 끝나고 후속 쓰기를 하지 않는 검사. |
| 3 | MSDC1 명시 목록, T70 카드 감지/전원/핀, 보수적인 읽기 경로. | MSDC0 접근 없음, 시간 초과·카드 없음·자원 정리·쓰기 거부 검사. 실제 SD 읽기는 새 OS/UEFI 실행 승인 뒤에만. |
| 4 | UFS 비 PCI 호스트 프로토콜 + MTK HCE/LinkStartup 연결부, DMA·저장장치 명령 정책. | MMIO 범위·DMA 폭/정렬/캐시·SMC 오류·UIC timeout·쓰기 거부·ExitBootServices 정리 검사. 실제 UFS 읽기/내용 대조는 미실행. |
| 5 | MUSB 호스트와 GPIO/I²C 기반 입력. TCPC/충전 상태 보존·역할 전환 정책은 보드별 구현. | 가짜 장치/전송 오류 검사, DMA 정리와 VBUS 이중 공급 방지 설계. 실제 USB·충전·터치·GOP 시험은 미실행. |
| 6 | 순정 커널 Linux initramfs 설계·모듈 의존성/KMI·펌웨어/사용자 서비스 연결. Windows는 확보된 드라이버가 있는 기능부터 ACPI 계약 작성. | 기능별 구현/로컬 검사/OS 실행/장시간 실사용 증거를 분리. LTE·SMS·통화 오디오·절전·열 보호를 포함한 CHECKLIST 조건 충족. |

2026-11-09 지시로 듀얼 부팅 개발은 현재 범위에서 제외한다. 아래는 기존 설치 조건의 기록이다. virtual A/B의 비활성 슬롯을 독립 OS용 공간으로 가정하지 않는다. 현재 GPT/슬롯/AVB를 유지하면서 외부 저장 장치의 OS와 순정 Android 복귀를 설계하되, 부트로더가 허용하는 진입 방식·외부 저장장치 경로·업데이트/복구·사용자 데이터 보존이 모두 미검증이다. EDK2 성공이나 전체 백업만으로 듀얼 부팅을 완료했다고 보고하지 않는다.

## 이번 작업 상태

- 완료: 원본 DTB 자원·phandle 대조, 같은 칩 커널의 UFS/PWRAP/I²C/MUSB/모뎀 코드 검토, EDK2 결함·재사용 지점·보드 차이·구현 순서 기록.
- 로컬 확인: 비교 커널 커밋 고정·작업 트리 변경 없음, 추출 자원과 문서의 수치·소스 경로 확인. 기존 EFI/모듈 검사 결과는 이전 작업 증거로만 인용했다.
- 구현 뒤 상태: 위 RAM·전원·MSDC·I²C 및 바이너리 기반 Himax 코드를 구현·컴파일하고 로컬 검사했다. 자세한 최신 수치와 바이너리 해시는 README와 `analysis/`의 검사 결과를 따른다.
- 미실행: 태블릿 실행·부팅·설치, 실제 주변 장치/통신/절전·듀얼 부팅 시험. 아래 검토 당시 결함 목록은 수정 전 근거로 남긴다.

## Windows 표시 연결 갱신

`MtkDisplay` DOD는 48바이트 GOP/ACPI 상태와 `DxgkCbAcquirePostDisplayOwnership`의
화면 정보를 대조한다. native/transpose 소스·고정 타깃 모드, 네 방향 회전,
동기 복사·동시 요청 잠금·MDL 정리·오류 화면·검은 화면 인계를 구현했다.
GOP는 줄 끝 패딩을 처리하고 EBS에서 UC 속성과 소유 모드를 다시 확인한다.
이는 소프트웨어 표시 경로이며 패널/DSI 기동·백라이트·절전·Mali GPU 가속은 별도다.
실제 Windows 로드와 기기 출력은 실행하지 않았다.

### 순정 핀 표를 이용한 Windows 전기 설정

순정 `pinctrl-mt6789.ko` SHA-256 `5ba5cca4ceb724767b8484de321b4b026b6d024aa47fe7627509e6d71553a2fb`의
PU/PD/PUPD/R0/R1/IES/pull_type을 ELF 심볼의 실제 파일 위치에서 읽었다.
188핀 전체를 비교 커널의 표와 대조했고 각 사용 비트가 다른 핀과 공유되지 않음을 확인했다.
원본 바이너리는 저장하지 않았으며 생성 도구·필드 출처는 `Windows/MtkGpio/generate-pin-map.py`와
`analysis/gpio-pin-map.json`에 있다. Windows 풀·IES와 복원, 여섯 IOCFG CPU/GCD/ACPI 매핑을
연결했다. 공유 Schmitt 설정과 원시 DRV/RSEL/EH 저장·복원 및 공유 필드 충돌 방지를 추가했다. 구동 전류 단위 변환·실제 전원 공급자와 기기 복귀는 남아 있다.


### Linux 플랫폼 공급자 연결

심볼 의존성에 나오지 않는 I²C 버스·핀 제어·클록 공급자 누락을 DT의 실제 연결로 보완했다.
`Linux/platform-modules.py`가 원본의 명명된 OF 표와 DT 인자 셀 수를 대조하며,
터치 적재 계획에 pinctrl/I²C/기본 클록이 Himax보다 먼저 포함된다.
기존 보관 위치에 원본 21개를 추가했고 새 모듈 CPIO는 190종·191개다.
생성한 적재 명령은 실제 sysfs의 장치/드라이버 연결도 확인한다. 없는 노드를 내장 공급자로
단정하지 않으며, 공급자 미해결·여러 일치 후보·연결 대기는 성공을 반환하지 않는다.
태블릿 적재와 기능 실행은 미실행이다. 상세 근거는 `analysis/linux-platform-modules.json`을 따른다.

## Windows I²C 인터럽트와 자원 수명 보강 (2026-11-10)

공통 엔진에 선택적인 이벤트 대기 경로를 넣었다. Windows 수동 수준 ISR은 완료/오류 IRQ를 가리고 원시 상태를 보존해 담당자를 깨운다. 담당자는 최대 5ms 대기 후 상태와 취소·전체 제한 시간을 다시 확인한다. EDK2는 기존 폴링을 유지한다. 타이머 정지·잘못된 깨우기에도 반복 제한이 있다.

두 버스의 정확한 MMIO/APDMA/GSI·원본/변환 자원과 Resource Hub V2 연결 정보를 검사한다. 준비 실패는 확보한 매핑·버퍼·DMA를 정리하며, 초기 D0 실패 때도 멈추지 않은 DMA 메모리를 보존한다. WDF 소유 인터럽트를 직접 삭제하지 않도록 I²C와 Himax 수명 처리를 맞췄다. 재준비·절전 복귀는 지원하지 않는다.

공통 C 28개 프로그램 ASan/UBSan 검사, I²C 코어 19,701개 확인과 실제 ARM64 Driver.c 콜백 모의 실행이 통과했다. 실제 Windows 로드·기기 IRQ·DMA·버스 전기적 동작은 미실행이다.

## Linux 시작 경로 연결 (2026-11-10)

정확한 순정 Image와 191개 모듈의 기존 순서를 PID1의 별도 작업자에 연결했다. EFI 앱은 기본 진단 항목과 입력·화면·저장장치·USB 시작 항목을 제공한다. 분야 이름 전체 검사 후 순서대로 실행하며, 적재 실패·제한 시간 초과·장치 연결 미완료·mdev 실패를 구분한다. 상태와 로그는 메모리에만 남기며 기존 기록을 덮어쓰지 않는다. 실제 커널 드라이버가 중단 불가능한 상태에 빠진 경우까지 사용자 공간 제한 시간으로 해결되지는 않는다.

묶음·파서·셸 검사 41개, 실제 EFI C 17가지 경로를 포함한 공통 C 28개 프로그램, 갱신한 ARM64 EFI 파일 읽기/OpenSSL SHA-256 10가지가 통과했다. 실제 Init의 격리 PID1/마운트 실행과 별도 시작 작업자 fork/exec·세션·파일 크기 제한·로그 보존도 통과했다. 시작 작업자 실행 파일은 장치에 접근하지 않는 대체물이었다. 태블릿 부팅·모듈 적재·새 OS 기능 실행은 미실행이다.

## 전체 모듈·장치 목록의 누락 보완 (2026-11-10)

이전의 표 이름 추측과 vendor_boot/추출본 중심 검색은 단수형 ID 표를 놓치고 내부 참고 표를
실제 드라이버로 세었다. 원본 모듈 429개 전부를 해시로 대조하고 정확한 driver 객체·이름·OF 포인터
재배치·등록 함수를 따라가도록 수정했다. 활성 compatible DT 노드 537개를 모두 기록했으며
모듈 OF 표와 직접 연결되지 않는 244개는 별도 증거 대상으로 유지했다.
IT6151 원본 표의 끝 표시 누락은 명시하며 일치를 만들어 보충하지 않는다.

키패드·전원 키·사운드카드·플래시·PD·열 보호의 누락 시작점을 보완했다. DT 공급자,
선언 의존성, 실제 필수 심볼/CRC 공급자를 함께 반복 계산하며 SMI 목록은 보드의 정확한
소비자·타깃에서만 벡터로 읽는다. PMIC regulator의 MFD 별칭 미리 적재는 실제 OF 결합으로
집계하지 않는다. fhctl이 필요로 하는 mcupm도 포함하고, 옛 mcupm_v3 이름은 고유 CRC
연결이 있을 때만 대응시킨다. 일반 센서 시작점의 카메라 imgsensor_isp6s를 제거했다.

최신 묶음은 219종·220개 바이너리로 이전보다 29개 늘었다. 13,246개 import CRC의 원본
커널/모듈 대조에서 불일치·공급자 파일 누락·미해결은 0건이며, 선언된 이름에 없는 연결 32건과
rfkill 변형 참조 12건은 기록을 유지한다. 정확한 Image에 대해 11개 분야의 정적 적재 오류가 없다.
묶음·정책·시작 경로 78개, 모듈·서명 감사 15개, EFI 실제 C 17가지와 ARM64 파일 읽기/해시 10가지가
통과했다. PID1 코드는 변경하지 않았고 이전 격리 실행 결과의 소스/바이너리 해시가 그대로 맞는다.
EFI와 초기 묶음을 다시 빌드했다. 기기 기능 실행·부팅·설치·복구는 미실행이다.
잠들기 전 복구 시작·기상 때 완료 조건과 현재 기기 시험 보류는 유지한다.

현재 소스 추가 결과: 보드 BDS와 GOP/UFS 지연 재시도, 표준 UFS 장치 경로의 읽기 전용 BlockIo 연결, 런타임 atag,devinfo·고정 클록·조건부 PHY/FIFO 준비가 구현됐다. Linux는 표준 KMS의 명시적 CPU 패턴 출력·원래 화면 복원 경로를 CPIO에 넣었다. Windows DOD는 D1-D3에서 표시를 차단하고 D0에서 POST/ACPI를 다시 검증하며, D3D11/12 WARP 앱은 셰이더 그리기·픽셀 검사와 DXGI 표시를 수행하도록 작성했다. CPU WARP 앱과 Mali 하드웨어 가속용 WDDM 드라이버는 별개이며 후자는 아직 없다. 최신 검사 수와 소스 해시는 각 JSON 보고서를 기준으로 한다.
