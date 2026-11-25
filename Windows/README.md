# T70Pro Windows ARM64 드라이버

현재 구현은 **MT6983 호환 I²C**, **MT6789 GPIO/EINT 컨트롤러**, 순정 바이너리 기반 **Himax 터치 드라이버**, **UFS Storport 저장장치 드라이버**, **화면 표시 DOD 드라이버**, **MUSB USB2 UCX 호스트**, **UMDF2 GNSS 위치 요청 처리**, Windows Battery Class 미니포트의 초기 경로다. 전체 Windows 포팅과 실제 터치 동작,
충전 정책, USB PD, 실기 배터리 잔량 표시가 완료됐다는 뜻이 아니다.
실제 Windows 로드·기기 부팅·설치는 미실행이다. 기본 빌드 산출물에는 개인 개발용 테스트 서명 절차를 적용할 수 있다.
모든 소스·SDK·빌드·검사는 기존 `builder` 안에 한 벌만 둔다.

## PMIC 배터리 계측

MtkMali/PmicPrm은 게이지 raw 전류, T70 순정 정수 단위 전류, AUXADC raw 전압·thermistor 온도·칩 온도·BIF 값을 제공한다. 순정 parser가 r-fg-value=5, car-tune-value=101을 내부값 50/1010으로 확장하는 규칙을 따른다. 이 전류 값은 mA나 배터리 잔량·충전률이 아니다. BIF 변환은 원래 disable bit를 보존하고 복구한 뒤 다시 읽어 확인한다. 확인에 실패하면 gauge lease를 유지해 후속 PMIC 쓰기를 막는다.

MtkMaliPmicPrmReadBatteryTemperatureDeciC는 thermistor, current latch, BIF ADC를 한 gauge lease에서 읽고 순정 계산으로 0.1°C 단위 온도를 반환한다. MtkMaliPmicPrmReadBatteryVoltageMv는 순정 OTP/eFuse 순서로 보정값을 읽어 캐시하고, BATADC와 CHIP_TEMP를 같은 lease에서 보정해 mV로 반환한다. MtkMaliPmicPrmReadBatterySnapshot은 보정 전압·배터리 온도·전류와 원시 샘플을 한 gauge lease에서 함께 수집한다. OTP read-enable과 클록 게이트를 복구한 뒤 다시 읽어 확인하며, 복구나 lease 반환을 확인할 수 없으면 gauge를 격리한다. 세 API 모두 전체 읽기와 lease 반환이 성공한 뒤에만 출력값을 갱신한다.

python3 Windows/build.py --pmic-prm-test --output-dir Build/PmicPrmBatterySnapshot-20261122의 client/broker/gauge ASan/UBSan 검사는 118,709개, 별도 broker 검사는 2,051개 확인을 통과했다. python3 Windows/build.py --buttons --test --output-dir Build/MtkButtonsBatterySnapshot-20261122는 버튼 드라이버 콜백 검사를 통과했고, 같은 출력 디렉터리의 MtkButtons.sys ARM64 unsigned 링크 크기는 31,744 bytes다. Windows/MtkBattery는 WDK Battery Class 미니포트로 등록된다. HANA679B ACPI 노드가 PnP ID를 제공하며, ACAD _PSR은 어댑터를 알리고, 배터리 드라이버는 같은 판독 경로의 ACST로 어댑터 상태를 배터리 전류와 함께 확인한다. BatteryInformation, QueryStatus, 온도, 예상시간, WMI와 BatteryMiniSetStatusNotify 전환 통지를 연결했다. _BIX/_BST AML은 추가하지 않았다.

### Windows 배터리 미니포트

Windows 배터리 클래스의 표준 인터페이스를 통해 설계 용량, 현재 잔량, 전압, 충·방전률, 온도와 예상 시간을 보고한다. 값은 Battery Class 규격의 mWh/mW로 바꾸며, PMIC 전류의 순정 정수 단위는 0.1mA로 읽어 mA로 환산한다. 배터리 셀의 동작은 고정된 PMIC PRM snapshot 하나에서 수집한다.

SOC는 순정 커널의 기본 battery ID 0, 순정 표 T0~T4의 100점 프로필에서 OCV로 시작 값을 구하고, Windows가 살아 있는 동안 순정 전류값을 소프트웨어로 적산한다. 이 코드는 순정 하드웨어 Coulomb CAR 누적, GM30 학습/노화 보정과 같지 않다. 프로필은 kernel-reference 커밋 a71515dbaf0d539ffb36416f14186dad17eef743의 mtk_battery_table.h에서 재생성하며 SHA-256 5630de3582929dc5a8ac82be6ce36f07855359de6077a04da3874e7085d40489를 검사한다. 커널 빌드의 CONFIG_MTK_ADDITIONAL_BATTERY_TABLE 설정은 체크아웃에 없으므로 실제 stock active table 개수는 확정하지 않았다.

Windows/build.py --battery --test는 실제 프로필과 생산 SOC 코어를 ASan/UBSan으로 478개 확인하고 ACPI HANA679B/INF 연결을 검사한다. Windows/build.py --battery는 ARM64 unsigned MtkBattery.sys와 INF를 생성한다(39,936 bytes). 검사는 Battery Class 인터페이스의 Windows 로드·설치나 기기의 실측값을 확인하지 않는다.

## CPU 주파수 조회

ACPI `CPFM`과 `MtkCpu`는 순정 DTB의 A55 6코어와 A76 2코어가 각각 쓰는 CPU 성능 도메인을 연결한다. 순정 `mediatek-cpufreq-hw.ko`의 `cpufreq_mtk_offsets`, `mtk_cpu_resources_init`, `mtk_cpufreq_hw_get`과 `pwr_tbl_ver` 읽기를 근거로 두 도메인의 OPP 표와 현재 요청 주파수를 읽는다. System-start PEP는 ACPI0007 CPU UID를 A55/A76 도메인에 연결하고 Windows PPM 요청을 표의 주파수 단계로 바꾼다. 요청은 stock scaling mode가 허용할 때만 설정한다.

`IOCTL_T70_CPU_QUERY_DOMAIN`의 입력은 도메인 번호 `ULONG` 0 또는 1이다. 응답에는 원시 요청값, kHz 단위 요청 주파수와 최대 32개 주파수 표가 들어간다. CPU 제어기가 비활성 상태거나 읽는 동안 요청값이나 표 형식이 바뀌면 다음 조회에서 새 값을 읽을 수 있도록 오류를 반환한다. Windows ARM64 빌드는 `Windows/build.py --cpu`, PEP/드라이버 검사와 native core 검사는 `Windows/build.py --cpu --test`를 사용한다. 실측 주파수, 검증된 전압·지속 성능 제한, 절전/복귀와 실제 Windows PPM 전환은 아직 확인하지 않았다.

## 전원·볼륨 버튼과 프레임워크 진입

MtkButtons는 ACPI HANA6790의 읽기 전용 KPD 상태와 공용 PMIC PRM v2 키 이벤트를 함께 처리한다. 일반 KPD IRQ와 20ms 타이머는 PASSIVE_LEVEL 작업을 예약한다. PMIC 전원 키 enable bits0/2의 사용권을 얻고, 보존한 press/release와 TOPSTATUS로 폴링 사이에 끝난 짧은 누름도 전달한다. v2를 지원하지 않는다는 유효한 응답을 받은 경우만 기존 읽기 전용 폴링을 쓴다.

별도 ACPI BTN0의 HANA6791·PNP0C40에 Windows 기본 버튼 알림 인터페이스로 32비트 키 값을 전달한다. 정확한 성공과 4바이트 완료 뒤에만 보존한 PMIC 이벤트 bits0/2를 지운다. 홈 키나 다른 상태가 남아 있으면 캡처를 보존하고 기다린다. 최초·D0 복귀의 눌린 키는 뗄 때까지 억제하고, 정상 D0 종료에는 이벤트 전달·모든 키 해제 뒤 자신이 켠 PMIC 비트만 복원한다. 결과가 불확실한 토글/W1C는 재실행하지 않는다. 종료 실패 시 상태를 남기지만, 이 반환만으로 실제 Windows 전원 전환 중단이나 객체 보존을 보장하지 않는다.

버튼 공통 코드 ASan/UBSan 341개·실제 ARM64 드라이버 콜백 3,221개, 별도의 실제 PMIC 클라이언트 18,249개와 최종 펌웨어 PRM 71가지가 통과했다. 새 미서명 MtkButtons.sys는 28,672바이트다. 커널/WDF/MMIO/알림/PRM 경계는 모의 서비스다. PMIC 상위 GPIO IRQ는 계속 차단하며, 실제 Windows 로드·버튼·절전/제거 복구·OS 깨우기는 미실행이다.

하드웨어 해제 실패에도 버튼의 MMIO 매핑과 인터럽트 핸들을 정리한다. 전달하지 못한 토글이나 보존한 PMIC 이벤트·사용권은 남기고 새 작업을 막는다. 자원 재할당 때는 새 인터럽트를 만들며, 터치의 이전 D0 진입 기록이 있으면 새 HMRD 확인을 요구한다. 터치 콜백 2,356개·I²C 콜백 2,697개도 실제 ARM64 코드로 확인했다. 새 HimaxTouch.sys는 36,864바이트, MtkI2c.sys는 28,160바이트다. Windows 전원 복구와 실기기 동작 확인은 별도다.

공용 빌더는 KMDF인 I²C·GPIO·터치·USB·버튼 SYS를 [프레임워크 진입점 `FxDriverEntry`](https://learn.microsoft.com/en-us/windows-hardware/drivers/wdf/building-and-loading-a-kmdf-driver)로 연결한다. 최종 PE의 진입 주소와 WDF 로더 가져오기, WDM 플래그·체크섬을 검사한다. 다섯 최종 SYS의 진입 코드를 ARM64 명령으로 실행해 원본 보안 쿠키 초기화와 WdfVersionBind 연결 실패 반환을 확인했다. MtkButtons는 실제 Worker·DriverEntry까지 실행해 WdfDriverCreate 성공과 실패 후 진단·프레임워크 해제도 확인했다. Windows 로더의 쿠키와 레지스트리 경로 복사·WDF 응답은 대체 함수다. 다른 네 드라이버의 정상 연결·PnP 장치 추가·실제 Windows 로드는 미실행이다. 결과는 `analysis/windows-framework-entry-execution.json`에 기록한다. Storport·순수 WDM·DOD 드라이버는 각 기존 진입 경로를 유지한다. 생성된 SYS/INF는 서명·설치하지 않았다.

## 구현

`MtkI2c/Driver.c`는 KMDF 1.33/SpbCx 컨트롤러 드라이버이며, EDK2 `I2cDxe`와
`Silicon/MediaTek/MediaTekPkg/Library/MtkI2cCoreLib`의 실제 전송 코드를 공유한다.

- 7비트 주소, 10~400kHz, 메시지당 최대 65535바이트·한 요청당 최대 32개 메시지의 임의 읽기/쓰기 순서를 지원한다. 중간 메시지에는 repeated START를 유지하고 마지막에 버스를 놓는다. 기존 단일 전송·write→read 경로도 유지한다.
- 16바이트 이하 FIFO, 그보다 큰 전송 APDMA v1/36비트. Windows DMA 매핑 주소와 WDF common buffer를 쓴다.
- 모든 메시지의 MDL 체인·읽기 매핑·주소·길이·최대 1초 지연과 전체 대기 예산을 첫 하드웨어 접근 전에 검사한다. 공통 DMA 버퍼의 정렬·총 128KiB 한도를 넘는 요청은 전송하지 않는다.
- NACK·중재 상실·시간 초과·예상하지 않은 버스 완료·취소를 구분한다. 오류 때 전송을 자동 재시도하지 않는다. SPB 규칙에 따라 대상 버스 오류는 실제 끝난 메시지의 바이트 수와 성공 상태를 반환하며, 취소·검증·안전하지 않은 DMA 상태는 별도 오류로 남긴다.
- 취소 콜백과 완료 처리를 나누고, DMA 정지 확인 후 요청을 한 번만 완료한다.
- DMA가 멈추지 않으면 버퍼 참조를 보존하고 다음 전송·자원 재배치를 막는다.
- 매 D0 진입에서 ACPI IRCK로 현재 클록 선택·PLL·게이트·핀·버스 선·정지한 DMA를 검사하고 현재 소스 주파수로 초기화한다. IRCK가 0을 반환하면 새로 준비했거나 안전하게 정지한 컨트롤러는 IRRS로 전용 클록 게이트를 복구할 수 있다. 상위 클록과 핀이 유지됐고 DMA가 정지한 경우에만 쓰며, 현재 주파수와 컨트롤러를 다시 확인한다. 3개 버스의 복귀 후 전송과 잘못된 응답의 거부를 ARM64 모의 검사에서 확인했다. 공유 전원·클록 및 핀 상태가 사라진 뒤 복구와 실제 절전은 미확인 또는 미구현이다.
- Windows는 수동 수준의 ISR과 동기화 이벤트로 전송 대기를 깨운다. ISR은 완료/오류를 확인해 IRQ를 가리고 상태는 전송 담당자가 해석한다. 최대 5ms마다 원시 상태를 다시 확인하므로 인터럽트 누락에도 전체 제한 시간이 적용된다. EDK2는 같은 코어의 기존 폴링 경로를 유지한다.
- IRQ 잠금은 마스크/상태 접근에만 사용하며 대기 중에는 잡지 않는다. 취소 콜백은 대기 이벤트를 먼저 깨우고, 담당자가 콜백 종료와 DMA 정지를 확인한 뒤 요청을 완료한다.
- 두 컨트롤러의 정확한 MMIO·APDMA·원본 GSI와 변환 자원 형식을 전부 검사한 뒤 매핑한다. 매핑·DMA·버퍼·IRQ 생성 실패는 이미 확보한 자원을 정리한다. DMA가 멈추지 않으면 초기 D0 실패 때도 버퍼를 보존한다.
- PrepareHardware에서 만든 인터럽트 객체는 ReleaseHardware 반환 뒤 WDF가 삭제한다. 버튼·터치·I²C는 해제 때 보관한 핸들을 비우고, 다시 준비할 때 새 객체를 만든다. 터치는 SPB 연결을 닫기 전에 작업을 비운다. I²C는 정지한 자원을 다시 할당하고 현재 상태를 재검사할 수 있다. 안전하지 않은 DMA 버퍼 재사용은 거부하며, D0 종료가 누락된 자원 반납에서는 MMIO를 건드리지 않고 버퍼를 보존한다. [실제 WDF 삭제 처리](https://github.com/microsoft/Windows-Driver-Frameworks/blob/main/src/framework/shared/irphandlers/pnp/interruptobject.cpp).
- I²C V2 Resource Hub의 버전·전체 길이·전용 연결·7비트 주소·속도·원본 경로 문자열 범위를 검사한다. 실제 FIFO/DMA·인터럽트·캐시·타이밍은 실기기에서 확인해야 한다.

컨트롤러 레지스터는 폭 16비트, FIFO는 8비트, APDMA는 32비트로 접근한다.
전송과 전체 시퀀스에 10초 대기 예산과 반복 횟수 제한을 적용하며, 메시지별 DMA 정지 확인도 별도로 제한한다. 타이머가 멈춰도 무한 반복하지 않는다.
느린 속도의 대용량 전송은 시작 전에 예산 초과로 거부될 수 있다. 10비트 주소,
여러 요청에 걸친 FIRST/CONTINUE/LAST 버스 유지, SMBus/PEC, 버스 선 복구와 전원 손실 뒤 클록·핀·전원 복구는 아직 지원하지 않는다.
순정 i2c-mt65xx.ko의 자동 재시작 비트와 MT6983 APDMA 동기화 순서를 대조했으며, 중간 RESTART와 최종 COMPLETE를 구분한다. 마지막 DMA 전송도 완전히 멈췄는지 확인한 뒤 완료 처리한다.

## ACPI 연결과 활성화 조건

DSDT의 `I2T0`와 `I2C6`은 사설 시험 HID `HANA6789`를 쓴다.
주소·크기·IRQ는 현재 T70Pro DTB의 두 I²C 노드와 대조한다.

| 장치 | 컨트롤러 | APDMA | GSIV |
| --- | --- | --- | --- |
| I2T0 (터치 버스) | 0x11e00000 / 0x1000 | 0x10217080 / 0x80 | 176 |
| I2C6 (충전기·TCPC 버스) | 0x1101a000 / 0x1000 | 0x10217480 / 0x80 | 182 |

`_CCA=0`으로 보수적인 비일관성 DMA 버퍼를 요청한다. 실제 일관성 경로가 확인됐다는 표시가 아니다.
`IRDY`는 준비 비트·형식 버전·실측 소스 클록 범위를 검사하고, `ICLK`는 그 클록을 반환한다.
부팅 전달 영역 `0x43f7f080`에는 준비 비트(32비트), I2T0 클록(32비트), I2C6 클록(32비트),
형식 버전(32비트)을 둔다. BootShim은 16바이트 모두 0으로 지운다.

Windows의 실제 초기화 주파수는 매번 IRCK에서 받는다. 이 메서드는 TOP I²C/AXI 선택, 두 pseudo gate와 APDMA/main gate, GPIO130/131 또는 142/143의 mode1·입력-high, 컨트롤러 START와 채널 DMA의 정지를 확인한다. PLL은 전원·격리·리셋·출력 게이트와 현재 PCW/post-divider를 검사해 26MHz·MAINPLL/32·UNIVPLL/20 중 현재 경로의 주파수를 계산한다. 지원 근거가 없는 AXI OSC_D4 선택은 거부한다. 두 번의 버스/DMA 표본과 클록 재확인을 사용하며 하드웨어에 쓰지 않는다. 실제 AML 54가지와 추가 접근 기록 5가지를 실행했고, 클록 거부 때 컨트롤러/DMA를 읽지 않는 것과 호출 사이에 사라진 준비 상태를 다시 검사하는 것을 확인했다. 공용 클록 소유자와의 사용권 연결, 전원 손실 뒤 복원과 실기기 복귀는 남아 있다.

**T70I2cPlatformDxe의 보드 준비 상태 발행을 구현했다.** 런타임 DTB·클럭/gate·핀·직접 DMA 경로·버스 idle을 확인하고 ExitBootServices 때 재검사해 ACPI에 넘긴다. 현재 FV에 이 드라이버와 I²C 드라이버를 포함한다. 두 장치의 `_STA`는 EBS에서 새로 확인한 준비 값이 있을 때만 활성화된다. 실제 클록 gate·핀·단독 소유권·DMA 경로,
레지스터 메모리 속성을 확인한 뒤에만 상태를 발행할 수 있다. 임의 26/136MHz 값을 넣지 않는다.
터치 자식 HXT0는 보드의 정상 보고 확인과 I²C/GPIO 준비 값이 모두 있어야 노출한다. 충전기·TCPC 자식 연결은 남아 있다.

EDK2 쪽은 같은 조건을 `MTK_I2C_CONTROLLER` 프로토콜로 받는다. 컨트롤러 MMIO가 GCD에
등록돼 있는지 검사하고 NonCoherentDmaLib로 캐시하지 않는 common buffer를 할당한다.
버퍼는 RuntimeServicesData로 보존해 OS 인계 때 DMA 정지가 실패해도 일반 RAM으로 재사용되지 않게 한다.
EDK2 I²C 드라이버는 같은 FV에서 보드가 게시한 두 컨트롤러에만 연결한다.

## 재현

`Mu-Silicium` 루트에서 실행한다. 스크립트가 기존 공용 빌드 잠금을 사용한다.

```sh
python3 Windows/prepare-toolchain.py  # 이미 있으면 재사용
python3 Windows/build.py             # ARM64 MtkI2c.sys, INF와 해시/PE 정보
python3 Windows/build.py --test      # 준비/해제·IRQ·취소·요청의 실제 ARM64 코드를 Unicorn에서 실행
```

공식 NuGet WDK/SDK `10.0.26100.6584`, KMDF 1.33, SpbCx 1.1과 Visual Studio
`Microsoft.VC.14.44.17.14.CRT.Headers.base`의 고정 헤더를 사용한다.
SDK 헤더를 복사·이름 변경하지 않고 Clang VFS로 Windows의 대소문자 규칙을 적용한다.
실제 드라이버는 GS 보호·ASLR·NX를 켜서 clang-cl/lld-link로 빌드한다.
빌드, 카탈로그 작성·서명, Windows 로드, 실제 장치 동작은 서로 다른 단계다.
현재 `.cat` 생성·서명·Windows 분석 도구 검사는 미실행이다.

`Build/mtki2c-verification.json`은 실제 `.sys`의 ARM64/native PE와 import·소스/산출물 해시를 기록한다.
`Build/request-tests.json`은 실제 Windows ARM64 코드의 요청 경로를 실행한 결과다.
MDL·WDF·SpbCx·커널 함수와 I²C 엔진은 이 검사에서 대체한다. 버퍼 분할·불완전한 버퍼·취소 시점·
오류 후 완료 바이트 수·DMA 실패 격리를 검사하며, 실제 Windows 커널 검증은 아니다.
공통 전송 엔진과 EDK2 연결부는 별도로 `verify-mediatek.py`의 가짜 MMIO/UEFI 검사에서 실행한다.

## Himax 터치 연결부

`HimaxTouch/Driver.c`는 순정 HX83132 일반 이벤트(명령 0x30, 56바이트)를 공통 `HimaxCoreLib`로 해석해 VHF 다중 접점 보고로 변환한다. D0 진입은 ACPI `HMXR` 준비 응답을 확인한 뒤 간접 레지스터 `0x900000d0`에서 칩 ID를 직접 읽는다. ID가 맞으면 인터페이스를 깨우고 `0x13=0x31`·`0x0d=0x10`을 최대 10회 다시 읽어 확인한다. 확인됐을 때만 `0x9000005c`에 네 바이트 0을 쓰고 10 ms 대기한 뒤 명령 `0x31`에 0을 보내는 바이너리 확인 리셋 없는 SenseOn 경로를 실행한다. ID·전송 오류면 터치 장치를 활성화하지 않는다.

조건부 ACPI `HXT0`/`HANA8313`와 `HMXR`를 구현했다. T70GpioPlatformDxe가 정확한 DTB, GPIO9 입력·GPIO152 리셋 해제, 단독 I²C 사용권, HX83132 ID·정상 보고와 IRQ 해제를 확인해 보드 프로토콜을 발행한다. 입력 드라이버 시작 후 정상 보고가 있어야 EBS에서 터치 준비 값을 내보낸다. Windows는 ACPI I²C V2 주소 0x48/400kHz/소스 `\_SB.I2T0`와 단독 하강 에지 IRQ를 검사한다. 이 공급자는 FV에 포함하며 EBS에서 새로 검증한 인계가 있어야 HMXR과 `_STA`가 활성화된다. D0 재진입은 HMRD의 요청 번호와 현재 GPIO152 리셋·GPIO9 IRQ 설정을 검사한다. VHF 보고를 다시 열기 전에 SPB로 칩 ID와 순정 실행 상태 레지스터를 읽는다. 전체 순정 `_hx83132_init`의 sense-off, 전원 손실 후 리셋 및 펌웨어 업로드·복구와 실기 IRQ는 미구현 또는 미확인이다. 공통 코어의 소프트웨어 리셋 명령은 별도로 구현했으나 입력 시작에서 호출하지 않는다. 자원 양쪽의 동일한 I²C ID와 IRQ 형식을 먼저 검증한다. Windows D0/인터럽트/VHF 실제 코드의 ARM64 모의 검사는 통과했지만 Windows 로드·서명·실기기 터치는 미실행이다. 상세 명령과 산출물은 [HimaxTouch/README.md](HimaxTouch/README.md)에 기록한다.

## GPIO/EINT 연결부

`MtkGpio/Driver.c`는 KMDF 1.33/GpioClx 컨트롤러이며, `Hardware.c`가 실제 레지스터 처리를 맡는다.
물리 GPIO는 0~187이고 GPIO 인터럽트로 연결되는 범위는 0~125다.
188번 이후의 가상 EINT를 물리 핀으로 열거하지 않는다. 원본 pinctrl-mt6789.ko에서 핀의 실제 필드 표를 읽고 비교 커널과 대조했으며, Teclast 커널 소스가 공개됐다는 뜻은 아니다.

- 32개 핀씩 6개 논리 bank, 마지막 bank의 유효 핀은 28개다. GPIO DIR/DOUT SET·CLR와 MODE masked-write alias를 쓴다.
- 입력·출력과 단일 edge/level의 극성 설정, mask/unmask·pending 조회·ACK를 구현했다.
  클래스 확장의 양쪽 edge·debounce 모의를 요청하며, 하드웨어 양쪽 edge/debounce를 구현했다고 표시하지 않는다.
- 같은 핀의 읽기 전용 입력과 IRQ를 풀 설정이 같을 때 함께 연결한다. 연결·해제 순서가 바뀌어도 남은 사용자의 입력/IRQ를 유지하며 패드 상태는 마지막 해제 때 복원한다. 출력 공유와 다른 풀 설정은 거부한다.
- 연결 전 mux/방향/출력값을 저장하고, IRQ 연결 때 감지 방식·극성도 저장한다. 해제·정지 때 소유한 핀만 원래 값으로 복원한다.
  소유하지 않은 핀·가상 EINT의 상태를 ACK하거나 바꾸지 않는다.
- PASSIVE 연결·해제는 구성 잠금과 해당 GpioClx bank 인터럽트 잠금 안에서 실행한다. DIRQL 콜백은 클래스 확장이 잡은 잠금을 다시 잡지 않는다. 전원 전환은 새 DIRQL 진입을 막고 진행 중 콜백이 끝난 뒤 실행한다.
- 전체 자원 목록을 확인한 뒤 MMIO를 매핑한다. 첫 D0에서 `GRDY`와 160개 EINT의 mask, GPIO IRQ의 AP 도메인을 다시 검사한다.
  사용 중인 인터럽트가 있으면 인계를 거부한다. 복귀 전원 제공자가 없어 D3 뒤 재시작은 지원하지 않는다.
- 순정 핀 표의 IES와 기본/풀업/풀다운/풀 해제를 I/O·IRQ 연결에 적용한다. 160개 PU/PD 핀과 28개 PUPD/R0/R1 핀을 복원하며, 188핀의 Schmitt 입력 설정은 실제 108개 필드와 공유 18그룹의 소유권·원래 값을 추적한다. 같은 필드를 쓰는 다른 기능 핀을 바꾸지 않는다.
- 전원 저장·복원 코어는 DRV/RSEL/EH의 원시 필드와 IRQ 마스크를 보존·검사한다. GRDY는 부팅 시 값이므로 실제 D3 후 준비를 보증하지 않으며, Windows 재개 콜백은 전원 공급자가 생기기 전까지 하드웨어에 쓰지 않고 거부한다. 저장된 상태와 매핑도 유지한다.
- GpioClx의 전류 단위는 10µA지만 MT6789의 EH 5비트 원시값과 연결되는 전류표는 확보하지 못했다. 상류 Paris의 EN/E0/E1 변환을 EH에 적용한 후보는 근거가 맞지 않아 채택하지 않는다. nonzero DriveStrength는 MMIO 전에 거부하며, DRV/RSEL/EH의 원래 값 보존과 복원은 유지한다. 이 후보의 소프트웨어 검사 통과는 전기적 전류 변환의 입증으로 사용하지 않는다. 실제 절전 깨우기·전원 복귀와 완전히 꺼진 상태의 기동도 남아 있다.

DSDT `GPO0`의 사설 HID는 `HANA678A`다. `_CRS`는 GPIO `0x10005000/0x1000`, EINT
`0x1000b000/0x1000`, 여섯 IOCFG 블록과 논리 bank마다 하나씩 공유 Level/ActiveHigh GSIV 276을 기술한다.
`T70GpioPlatformDxe`는 BootStatus가 게시한 런타임 DTB, GPIO 범위/연결, EINT 160개/인스턴스 1개,
GIC 부모·IRQ, GCD uncached 매핑, 모든 EINT의 mask와 AP 도메인을 확인한다.
GPIO/EINT에 쓰지 않고 EBS 때 예약 RAM `0x43f7f090`에 준비 여부·188/126 핀 수·버전만 기록한다.
BootShim은 I²C·GPIO·터치·UFS·화면과 MH248 덮개 인계 영역 128바이트, USB 인계 영역 64바이트를 지운다. 덮개 제공자·DSDT·BootShim 변경을 포함한 전체 펌웨어를 다시 빌드했고, 실제 FD에서 보드 드라이버 15개·표준 부팅 드라이버 11개·ACPI 표 5개를 대조했다. 생성한 부팅 이미지의 CPU 모의 실행 14가지도 통과했다.

```sh
python3 Windows/build.py --gpio
python3 Windows/build.py --gpio --test
```

`Build/mtkgpio-verification.json`은 unsigned ARM64 SYS의 소스/산출물·import를 기록한다.
`Build/MtkGpio-tests.json`과 `MtkGpio-driver-tests.json`은 전체 핀·감지 방식·실패 주입·복원 검사와
실제 ARM64 `Driver.c` 콜백 실행 결과다. 레지스터·WDF·GpioClx·ACPI 서비스는 모의 구현이다.
EDK2 보드 인계와 실제 AML의 준비 조건은 별도로 `verify-mediatek.py`/`verify-acpi.py`에서 검사한다.
Himax 자식은 I²C/GPIO에 `_DEP`를 선언하고 DTB의 GPIO9 Edge/ActiveLow 자원을 사용한다. 실제 AML에서 부모 준비와 터치 주소·핀·버전/준비 값 11조합을 검사했다. 터치 입력 오류·중단·반복 EBS는 준비 값을 취소하며, EBS에서는 추가 I²C 통신을 하지 않는다. 실제 IRQ·전원/복귀 동작은 미실행이다.

공식 연결 규칙: [GPIO bank와 공유 인터럽트](https://learn.microsoft.com/en-us/windows-hardware/drivers/gpio/partitioning-a-gpio-controller-into-banks-of-pins),
[GPIO 인터럽트 동기화](https://learn.microsoft.com/en-us/windows-hardware/drivers/gpio/interrupt-synchronization-for-gpio-controller-drivers).

## UFS 저장장치 연결부

`MtkUfs/Driver.c`는 실제 Windows ARM64 Storport 드라이버다. ACPI `UFS0/HANA678B`의
MMIO `0x11270000/0x2300`, 준비 정보 `0x43f7f0b0/16`, GSIV 169를 연결한다.
UEFI는 실행 중이던 요청을 비운 뒤 두 목록을 멈추고 인터럽트를 막아 기존 DMA 주소 네 개를
0으로 지운다. 정확한 DTB·GIC·직접 DMA 범위와 켜진 클록/VEMC, HCE/HCS/버전을 다시
확인한 경우에만 링크를 유지하고 준비 값을 발행한다. 실패하면 HCE를 끄며 준비 값은 0이다.
준비 정보는 캐시에서 실제 RAM으로 반영한다. 반복 EBS는 오래된 준비 값을 취소한다.
현재 UFS 공급자도 FV에 포함한다. 새 인계 검사가 실패하면 `_STA=0`을 유지한다.

Windows는 별도의 연속·비캐시 공통 버퍼와 36비트 주소/정렬/길이를 검사한다. 단일 UFSHCI
전송 슬롯, UPIU/PRD, NOP, READ CAPACITY(16)를 구현했다. 실제 응답이 블록 4096바이트,
LU2 용량 127,959,826,432바이트와 맞아야 디스크 요청을 받는다. 초기 UNIT ATTENTION과
일시적 NOT READY는 최대 세 번의 용량 읽기로 제한한다. 부트 LU와 RPMB는 노출하지 않으며
LU2만 Windows BTL `0:0:0`으로 연결한다.

READ/WRITE(6·10·12·16), DPO/FUA 읽기·쓰기, INQUIRY·REQUEST SENSE·MODE SENSE·용량 읽기와
단일 LU의 REPORT LUNS를 처리한다. SRB FLUSH/SHUTDOWN은 실제 비동기 SYNCHRONIZE CACHE를
보내 응답을 기다리고, SHUTDOWN은 그 뒤 HCE를 끈다. 쓰기는 호출자의 데이터를 사유 DMA
버퍼에 복사한 뒤 시작하며 호출자 버퍼를 직접 DMA 주소로 쓰지 않는다. 범위·방향·길이가 맞지
않는 명령과 UNMAP·FORMAT·보안·펌웨어·제조사 명령은 거부한다. **UEFI Block I/O는 계속 읽기 전용이다.**

전체 양방향 요청 모델을 쓰고 `StorPortSynchronizeAccess`로 요청·타이머와 IRQ의 공유 상태를
보호한다. 완료·sense·짧은 전송·중복 완료 방지·최대 30초 시간 제한을 구현했다. 오류 때 DMA
정지가 확인되지 않으면 공통 버퍼를 재사용하지 않는다. 유휴 PnP 중단은 링크를 유지하며 DMA
목록을 분리하고, 재시작은 모든 하드웨어 조건과 NOP·용량을 다시 검사한다. 공유 VEMC·TOP_UFS를 유지하는 전원 정지·복귀는 아래의 PRM 코어와 상위 필터에 연결했다. 전체 공급 전원 손실 뒤의 PHY 복귀와 실제 Windows 전원 전환은 확인하지 않았다.

2026-11-13. UFS·GPU의 WACS2 통신을 공용 PRM 처리부에 연결했다. 두 처리 함수가 같은 잠금과 지속 상태 기록을 사용하며, 상대가 응답을 처리 중이거나 실패한 명령이 남아 있으면 새 명령을 차단한다. GPU는 허용된 레일 레지스터만 읽고 쓰며, 쓴 값을 다시 확인하고 응답을 확정한 뒤 결과를 넘긴다. Windows GPU의 PWRAP 직접 매핑을 없애 MFG0 자원을 5영역으로 맞췄다. UFS 전원 필터는 첫 호출부터 OS 재시작까지 PRM 모듈을 고정하고, GPU도 처리 여부가 불명확하면 모듈 고정과 실패 상태를 보존한다. 이 경로의 PRM 모듈 실행 중 교체는 지원하지 않는다. UFS의 공유 VEMC·TOP_UFS 유지형 정지·복귀는 계속 연결되어 있다. C 검사 프로그램 11개의 보고된 확인 항목 590,580개, Windows GPU ARM64 검사 18묶음, 새 PMIC 연결 5,364개·SoC 131,876개와 UFS 전원 필터 586개 조건이 통과했다. 최종 4MiB 펌웨어의 PRM 함수 36개 전체 명령어와 공용 통신 실행 27가지, 두 처리 함수의 332바이트 MEDT 및 잘못된 PE 메타데이터 20가지 거부도 확인했다. UFS 처리 코어의 연결부 실행 검사는 모의 호출이며 별도 C 검사로 실제 코어를 확인했다. 보안 영역·다른 펌웨어와의 접근 배제, 공유 부모 전원·기기별 OPP/DMA 입증, 전체 전원 손실 복구와 전체 DXE·OS·실기기 실행은 남아 있다. 근거는 project-state.json의 latest_edk2_shared_pmic_broker와 연결된 검사 기록이다.

```sh
"$PYTHON_COMMAND" Mu-Silicium/Windows/build.py --ufs --test
"$PYTHON_COMMAND" Mu-Silicium/Windows/build.py --ufs
"$PYTHON_COMMAND" Mu-Silicium/Windows/build.py --ufs-power --test
"$PYTHON_COMMAND" Mu-Silicium/Windows/build.py --ufs-power
```

산출물은 `Windows/Build/MtkUfs.sys`, `MtkUfsPower.sys`와 각각의 `.inf`다. MtkUfsPower는 Windows 11 빌드 26100 이상에서 ACPI UFS 장치에 연결하는 상위 필터다. 실제 Driver.c ARM64 실행으로 자원 오류,
검색·NOP·용량·읽기·쓰기·flush·shutdown·sense·시간초과·DMA 정지 실패·중단/재시작을
검사했다. 정상 인계 값+HCE=0과 13개 전원 손실/오래된 재시작 조건을 MMIO/DMA 쓰기 전에 거부하고, 진행 중 Stop의 단일 중단 완료·호출자 버퍼 보존·HCE 정지 실패의 DMA 격리도 확인했다. 실제 테스트 ARM64 PE는 UFS 전용 100M 명령/20초 한도 안에서 2.271초에 복귀했다. 가짜 RAM/레지스터에 쓴 결과이며 태블릿 저장장치에는 쓰지 않았다.
서명·InfVerif·Windows 로드·실제 저장 내용 대조·전원 복귀는 미실행이다.

사용한 호출 규칙은 Microsoft의 [Storport 초기화](https://learn.microsoft.com/en-us/windows-hardware/drivers/storage/hardware-initialization-with-storport),
[동기화](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/storport/nf-storport-storportsynchronizeaccess),
[공통 버퍼](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/storport/nf-storport-storportgetuncachedextension),
[물리 주소](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/storport/nf-storport-storportgetphysicaladdress),
[중단/재시작](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/storport/nc-storport-hw_adapter_control) 문서와 WDK 26100 헤더를 대조했다.

## 보드·LTE·SoC 온도 센서

`MtkThermal`은 ACPI `HANA6794`의 AUXADC 채널 0과 1을 읽는다. ACPI 전원 리소스가 26 MHz 클록을 제어하고, 드라이버가 클록 상태를 확인한 뒤 지정된 보정 워드 하나를 읽는다. 각 요청에서 이전 변환 완료 표시를 지운 뒤 새 변환을 시작한다. 두 센서가 공유하는 순정 변환표를 사용하며 시간 초과와 범위 밖 샘플은 오류로 반환한다. 보정과 변환 순서는 [MediaTek AUXADC 소스](https://nest-open-source.googlesource.com/manifest_repos/kernel_device_modules-5.15/+/refs/heads/main/drivers/iio/adc/mt6577_auxadc.c)에서 확인했다.

`ThermalIo.h`의 인터페이스는 채널 번호 0 또는 1을 받아 원시 코드, 0.001°C, 0.1 K 값을 반환한다. `IOCTL_T70_READ_SOC_TEMP`는 순정 LVTS 센서 번호 1~13 또는 전체 최고 온도(0)를 읽는다. 기존에 설정된 LVTS의 클록, 센서 선택과 보정 레지스터, 유효한 측정 값을 확인한다. 순정 DT의 두 eFuse 영역에서 공장 온도 보정값을 읽고, 사용하는 보정 비트가 모두 비어 있으면 순정 모듈의 기본값을 적용한다. 이전 ACPI에서는 제공된 공장 온도 값만 쓴다. 이 경로는 기존 설정의 LVTS를 읽는다. 표준 `IOCTL_THERMAL_READ_TEMPERATURE`도 SoC 최고 온도를 0.1 K 단위로 반환한다. 즉시 읽기와 임계값·기한 대기를 함께 처리하며, 대기 요청의 취소와 D0 종료 시 정리를 확인했다. ACPI `\_TZ.SOC0`가 NTC0 센서 공급자를 참조하고 순정 DT의 위험 온도 113.5°C를 사용한다. LVTS 초기화·전원 손실 복구와 실제 Windows 열 정책은 미구현 또는 미확인이다. [Windows 온도 읽기 규격](https://learn.microsoft.com/en-us/windows-hardware/design/device-experiences/ioctl-thermal-read-temperature)

```sh
python3 Windows/build.py --thermal --test
python3 Windows/build.py --thermal
```

## MUSB USB2 호스트와 순정 키보드 경로

`MtkUsb/Driver.c`는 KMDF 1.33/UCX 1.6 호스트 드라이버다. ACPI
`USB0/HANA678D`의 컨트롤러 `0x11200000/0x10000`, 원본 GSIV 160과
PHY `0x11f40000/0x700`, 읽기 전용 인계 영역 `0x43f7f100/64`를 검사한다. 호스트 장치 인터페이스
`GUID_DEVINTERFACE_USB_HOST_CONTROLLER`를 등록하고 사용자 IOCTL을 UCX로 전달한다.
루트 허브·USB 장치·기본/일반 endpoint를 실제 UCX 객체로 만든다.
장치의 설명자와 HID 자식은 Windows USB 계층이 열거한다.

인계 revision2의 Type-C source flags0x7f/path1·3과 ID-low flags0xef/path2를 받는다. revision1의 flags0x7f/path0도 허용하며 누락/추가 비트·경로·버전 불일치는 거부한다. 수락한 flags를 공통 코어에 전달한다. 초기 D0는 FIFO/유휴 확인 전후에 실제 DEVCTL의 HOSTMODE·SESSION·VBUSVALID와 BDEVICE 해제를 새로 읽고 첫 코어 쓰기 전에 상태 변화를 거른다. 실제 Driver.c ARM64 콜백 실행과 SYS 빌드, AML의 MUSB 상태 77가지와 클록 경계값 20가지 검사를 통과했다. Windows 로드·열거는 미실행이다.

EDK2의 독립 ID-low 경로는 충전 중 GPIO48과 TCPC 상태를 보존하고 SC boost를 켜지 않는다. 실제 포고핀 배선·전압은 모의 실행으로 확정하지 않으며 MUSB VBUS가 유효하지 않으면 준비 기록을 게시하지 않는다.

- 공통 `MusbHostCore`로 제어·bulk·interrupt PIO 전송, 저속/전속/고속과 허브 TT 주소를 처리한다.
- endpoint마다 요청 큐와 수동 대기 큐를 두고 하나의 수동 작업자가 인덱스 레지스터를 소유한다. 요청 완료는 별도 DPC에서 처리한다.
- MDL 체인 전체와 방향·길이를 먼저 확인하고 비페이지 버퍼로 옮긴다. NAK 인터럽트 요청은 주기에 맞춰 다시 대기하며 취소·중단·분리와 중복 완료를 구분한다.
- SET_ADDRESS는 주소 0으로 실제 제어 전송한 뒤 성공할 때만 새 주소를 저장한다. endpoint 구성·비활성화는 대기 요청과 진행 요청의 수명을 함께 관리한다.
- 관리 요청·전송·타이머는 `EndpointLock` 아래에서 작업을 한 번만 예약한다. 코어 초기화가 끝나면 요청을 받고, 전원 종료는 새 예약을 막은 뒤 실행 중인 작업을 기다린다.
- 초기 D0에서 ACPI `MHRD`와 64바이트 인계를 확인한다. `MCLK`가 있는 펌웨어에서는 실제 클록을 확인한 뒤 DEVCTL·PHY·FIFO와 DMA/QMU 유휴 상태를 읽는다. 시작 실패는 `UcxControllerSetFailed`로 USB 계층에 알린다.
- D1/D2/D3 복귀 때 `MCLK`가 0을 반환하면 `MGON`으로 USB 클록을 복구한다. 부모 PLL과 mux가 종료 직전과 같을 때 꺼진 USBPLL을 켜고 TOP과 IFRAO_SSUSB 게이트를 연다. USBPLL이 격리돼 있고 주파수 설정이 지워졌으면 순정 `clk-common.ko` 순서로 마지막 주파수를 되쓴다. 각 단계의 결과를 장치에서 읽어 확인한다.
- Windows retained resume는 DEVCTL·PHY의 호스트 상태와 DMA/QMU 유휴를 확인한 뒤 유실된 고속 통신·절전 허용 설정과 FIFO 배치를 복구한다. PHY 전기 보정값은 호스트 신호가 살아 있고 보정이 진행 중이지 않을 때 달라진 필드만 되쓴다. 새 D0 복귀 경로는 보존 상태를 잃었을 때에도 저장해 둔 ACR1/ACR5/ACR6 튜닝값, 현재 MCLK와 ACPI MROU/MTCV 경로·역할 확인이 모두 맞으면 TPHY와 MUSB를 비동기 worker 순서로 다시 시작한다. USB 상위 IRQ는 재설정 전에 마스크하고 Host/VBUS 준비 상태가 확인된 뒤 다시 연다. 각 단계가 실패하면 소유한 PHY/SESSION/POWER 변경을 정리한 뒤 UCX 실패로 보고한다. 이 경로는 PE 코드 모의 검사와 SYS 링크까지 확인했으며, Windows 로드와 실제 전원 손실 복귀는 미실행이다.
- PHY 보정 복구에는 새 ACPI와 이번 USB 드라이버가 함께 필요하다. 새 드라이버는 이전 ACPI의 읽기 전용 PHY 자원도 받으며 그 구성에서는 PHY 보정값을 읽기만 한다.
- 데이터 전송은 USB IRQ를 마스킹한 PIO 폴링 경로다. isochronous·USB3·고속 대역폭 추가 거래와 새 시스템 부팅에서의 USB 단독 host 전원 기동은 남아 있다.

`MusbHandoffDxe`는 DTB의 USB·전원·I²C 연결과 실제 클록/GPIO/PHY,
SC8989x boost와 HUSB311 VBUS 값을 대조한다. 충전기·TCPC 버스 마커는 정확한
I²C 컨트롤러와 master가 있는 동일 핸들에 게시한다. USB 준비 프로토콜은 별도
USB 핸들에 게시하고, UEFI USB2HC는 그 USB 핸들에 연결한다. EBS에서는 I²C 드라이버가 정지하기 전에 전원 상태를 새로 읽고,
호스트가 멈춘 뒤 얻은 실제 포트 상태와 최종 MMIO 관측을 결합한다.
관측한 TX/RX/DMA 완료 비트만 순정과 같은 폭으로 정리하고, 연결·분리 이벤트는
호스트 코어가 처리하도록 보존한다. 모든 조건을 통과한 64바이트 기록만 준비 비트를 마지막에 쓴다.

FV에는 Clock/GPIO/I²C/StockPower/MusbHandoff/MusbHost와 표준
UsbBus/UsbKb/UsbMouse 드라이버를 함께 넣는다. 포고핀 실물의 USB 배선과 정확한
터치패드 보고 형식을 확인한 것으로 간주하지 않는다. OEM 키보드의 실제 입력과
다중 손가락 동작은 별도 실물 검증 조건을 유지한다.

`Windows/Build/usb-cold-resume-final-test/MtkUsb-tests.json`과
`MtkUsb-driver-tests.json`은 실제 ARM64 드라이버 콜백의 장치 등록·IOCTL 전달·요청·취소·허브·자원·인계 및 cold D0 재시작 검사를 기록한다. unsigned SYS 빌드 정보는
`Windows/Build/usb-cold-resume-final-arm64/mtkusb-verification.json`에 있다.
이 실행에서는 WDF/UCX/커널/MMIO와 모든 전송·플랫폼 코어를 대체한다.
공통 코어와 UEFI 인계 코드는 별도 native ASan/UBSan 검사로 확인한다.

```sh
python3 Windows/build.py --usb
python3 Windows/build.py --usb --test
```

## 개인 개발용 테스트 서명

Windows WDK의 `Inf2Cat.exe`와 `SignTool.exe`를 사용할 수 있는 PowerShell에서 실행한다.
입력 폴더는 드라이버별 하위 폴더로 구성한다. 각 폴더에 INF와 필요한 SYS/DLL을 둔다.

```powershell
.\Windows\Sign-TestDrivers.ps1 -InputPath .\Packages -OutputPath .\Packages-Signed
```

무료로 만든 로컬 테스트 인증서로 실행 파일을 SHA-256 서명하고, 서명된 파일을 기준으로 PnP용 CAT을 만들어 서명한다.
출력 폴더에는 공개 인증서만 남긴다. 기본값은 Windows 11 24H2 ARM64이고 25H2용 WDK에서는 `-WindowsVersion 10_25H2_ARM64`를 지정할 수 있다.
[Microsoft 테스트 서명 문서](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/test-signing-driver-packages)

## 근거와 남은 범위

레지스터·변형 근거는 기존 커널 참조의 고정 커밋
`a71515dbaf0d539ffb36416f14186dad17eef743`,
`mindone/modules/i2c_mt65xx/i2c_mt65xx.c`다. Teclast의 정확한 소스라는 근거는 없다.
원본 알고리즘 전체를 옮기지 않고, 확인한 레지스터와 시간 조건에 맞춰 공통 엔진을 작성했다.

Windows 측 남은 범위는 [실사용 체크리스트](../Platforms/Teclast/t70proPkg/CHECKLIST.md)에서 추적한다.
특히 UFS/PHY·GOP/패널·WDDM GPU·MUSB 콜드 기동/역할 전환·GPIO 구동 전류/절전 복귀·Himax 전체 보드 경로·배터리/충전/열 보호·
Wi-Fi/BT·오디오·카메라·센서·CCCI/DPMAIF WWAN·SIM/IMS/통화·GNSS는 완료되지 않았다. 듀얼 부팅 개발은 최신 사용자 지시로 현재 범위에서 제외한다.
내부 모뎀을 USB MBIM 장치로 가정하거나 EFI만으로 Windows/macOS 드라이버가 생긴다고 보지 않는다.

공식 문서: [SpbCx](https://learn.microsoft.com/en-us/windows-hardware/drivers/spb/spb-controller-drivers),
[WDK NuGet](https://learn.microsoft.com/en-us/windows-hardware/drivers/install-the-wdk-using-nuget),
[ARM64 드라이버 빌드](https://learn.microsoft.com/en-us/windows-hardware/drivers/develop/building-arm64-drivers).

## T70Pro inherited display

`MtkDisplay/` implements a real Windows display-only miniport using Microsoft's
pinned KMDOD mode/DDI base and a checked synchronous renderer. It validates the
GOP/ACPI 48-byte DFBI handoff against POST ownership, creates native/transposed
source modes, supports four software rotations, serializes presents, pins the
pitched source, and handles visibility/crash-display/POST release. It uses UC
framebuffer mappings matching GOP and never simulates a hardware interrupt.
See [MtkDisplay/README.md](MtkDisplay/README.md) for the supported paths and tests.

Build/test with `Windows/build.py --display [--test]`. It remains unsigned and
hardware-unexecuted. Cold panel/DSI, backlight, physical power-loss resume and Mali rendering
are separate unfinished work; no GPU acceleration is claimed. The producer is
included in the FV, but only validated live geometry enables the ACPI device.
D1–D3 blank and unmap the framebuffer. D0 validates POST and ACPI before remapping and keeps the previous visibility state.
`MtkDisplay/DirectX.cxx` adds explicit D3D11/12 WARP shader/readback and DXGI presentation;
see [MtkDisplay/README.md](MtkDisplay/README.md) for builds and exact test scope.
WARP runs on the CPU; an installable Mali hardware-acceleration driver remains incomplete.

`MtkMali` supplies the stock-backed AArch64-4K MMU/JM core, WDK paging/MDL
memory, ordered multi-stage scheduler, actual typed lifecycle/submission/reset/
notification DDIs, a PASSIVE kernel worker, and bounded arch-9 write/timestamp/cache
command validation with private command rebuilding and preparation leases.
Adapter-owned allocation/resource lifetime, process/device-specific opens, read-only
rights, atomic batch cleanup and OS-gated shared-backing CPU access are implemented.
OS CPU page-table tracking and PFN-verified fresh MDL pinning now prepare private
per-submission roots containing only validated targets and command shadows.
Private roots detach before completion; notification/reset failures retain leases.
Native sanitizers, stock ARM64 comparisons and actual ARM64 WDK test-code
emulation pass. See [MtkMali/README.md](MtkMali/README.md). Adapter/MMU/segment/node/engine queries, a referenced feature interface and a
retryable software start/stop bundle also pass ARM64 emulation. MT6789 SPM/bus-protection, mux/BG3D and counted rail/reference/domain/clock/DCM power sequencing pass 128 original ARM64 domain traces, 155 clock traces, 66 composite power traces, 256 WACS2 command traces, 320 hardware clock traces, 1,024 PLL rate comparisons, 308 stopped-PLL programming and 1,981 voltage-pair comparisons, and 864,808 native/ARM64 checks. Exact stock springboard block and commit planning match 121 entries across six test tables and 4,465 transition plans, including equal-voltage plateaus and 11 failing-service stop prefixes. Real WACS2 MMIO transport and VGPU/VSRAM_GPU enable/status/voltage reads preserve inherited rails and failed acquisitions. Actual MFGPLL/ref/mux/BG3D MMIO control is implemented with stopped-PLL PCW/postdivider programming, exact stock voltage-pair order/settle timing, no absent mux-update write and retained failure ownership. Board authorization must approve transient rail pairs and the requested/rounded rate. Adjusted-table validation, exact 200mV springboards and a bounded voltage/frequency planner are implemented. The explicitly authorized idle-only executor connects actual PMIC pair and stopped-PLL cores, gates BG3D and moves through Sub/Main, verifies the final hardware state before committing the stable index, and retains partial failures. Shared-bus/resource/OS binding, device-qualified adjusted OPP inputs, AVS/aging/segment/thermal/battery governor and suspend/resume remain. `Miniport.c` now supplies a typed WDDM 3.1 DriverEntry/DxgkInitialize table,
Add/Start/Stop/Remove, validated resources before MMIO, referenced board-interface
query, GPU mapping and Core/runtime lifetime. Failed teardown retains live stages
for retry; D0 checks actual readiness and D3 is explicitly unsupported. The actual
ARM64 lifecycle code passes 186,631 checks with OS/board/MMIO boundaries mocked.
The real SoC board provider, ACPI GPU enumeration, Windows registration/package
and OS integration of the bounded shared-backing backend,
shader and compute/tiler/fragment command validation,
physical IRQ resource binding/power, and a D3D user driver remain. Managed GPU/JOB/MMU masks, the real ISR/DPC-to-worker path, idle-fault polling and synchronized start/reset/stop gating are implemented. Failed connect/rearm/disconnect operations retain ownership. The three actual translated line-IRQ descriptors are validated before startup. A common nonpaged spin lock at the highest assigned DIRQL now protects the ISR and mask changes even when their OS interrupt locks differ. The parent must associate/connect the physical lines and drain OS ISR/DPC calls before freeing resources. No installable accelerated
display driver is produced.

### GPIO 풀 설정과 입력 버퍼

`MtkGpio/generate-pin-map.py`는 원본 백업을 읽기 전용으로 열고 순정
`pinctrl-mt6789.ko`의 일곱 표를 메모리에서 해석한다. 188개 IES와 PU/PD 또는
PUPD/R0/R1 위치를 비교 커널과 전부 대조해 `PinMap.h`를 만든다. 모듈 사본은 남기지 않는다.
`analysis/gpio-pin-map.json`에 원본 SHA-256·표의 파일 위치·해시·대조 결과를 기록한다.

Windows는 여덟 GPIO/EINT/IOCFG 자원의 주소·크기·순서를 확인한다. 새 풀을 켜기 전에
기존 풀을 해제하며 PUPD 핀은 R0/R1을 끈 상태에서 방향을 바꾼다. 기존 저항 선택이 켜져 있으면
그 선택을 유지하고, 꺼져 있었다면 순정의 기본 활성 선택 R1:R0=01을 사용한다. 저항값이나 전류를
임의로 정하지 않는다. 모든 설정은 핀별 SET/CLR로 처리하며 다른 bank의 핀을 건드리지 않는다.
중간 오류가 나면 새 요청을 막고 저장한 상태로 복원한다. 방향을 입력으로 만들거나 풀 복원이
실패하면 출력을 다시 켜지 않고 복원 재시도에 필요한 상태를 보존한다.

보드 공급자는 여섯 IOCFG의 UC 매핑과 GPIO9·GPIO152 입력 버퍼까지 읽어 확인한다.
전기 설정을 실제 기기에 쓰거나 새 OS를 실행한 검사는 하지 않았다.

GPIO 방향·mux·출력 래치·IES·풀 변경 뒤에는 실제 읽을 수 있는 원래 레지스터를 다시 읽어
변경이 반영됐는지 확인한다. 쓰기가 무시되거나 읽기가 실패하면 성공으로 보고하지 않는다.
표준 `DriveStrength`의 단위는 10µA지만 순정 MT6789 설정은 `FLAG_DRIVE_SET_RAW`와
`MTK_PIN_NODRV`를 사용한다. 원본의 3비트 코드를 전류값으로 해석할 근거가 없어
이 값에 임의 배율을 곱해 지원한다고 처리하지 않는다. 공유 구동 비트의 제어와 전류 대조는
후속 구현 조건으로 기록한다.

### 자석식 덮개

MH248 GPIO0을 ACPI `PNP0C0D` 덮개로 연결했다. `_LID`는 닫힘 0·열림 1을 반환하고 양쪽 변화에서 `Notify(0x80)`를 보낸다. 새 인계와 GPIO OpRegion 연결 전에는 입력을 읽지 않는다. Windows의 양쪽 에지 모의를 위한 `_DSM`에 GPIO0의 높은 입력이 닫힘이라는 극성을 명시했다. 실제 AML의 준비 조건 39가지·극성 7가지와 읽기 전용 입력/IRQ 공유의 실제 ARM64 콜백 모의 검사를 통과했다.

공통 GPIO 코어의 `MgSuspendWake`는 호출자가 지정한 깨우기 핀만 열고, 나머지 소유 IRQ를 막는다. 순정 EINT의 SET→CLR 순서를 따르며 다른 소유자의 핀과 대기 중인 변화는 건드리지 않는다. 원본 `pinctrl-mtk-v2.ko`의 절전·복귀 함수와 레지스터 표도 96가지 ARM64 가상 실행으로 확인했다. 마스크를 다시 읽어 확인하고, 실패하면 원래 마스크로 되돌린다. 복구도 실패하면 저장 상태를 보존하고 입력을 막아 재시도를 기다린다. ASan/UBSan 검사 414,184개(GPIO/LNA 포함)와 전원이 공급된 가상 MMIO를 사용한 실제 ARM64 코드 실행을 통과했다.

GPIO 레지스터 복원은 하드웨어가 동작하는 CLIENT_StopController에서 끝낸다. CLIENT_ReleaseController에서는 새 접근을 차단하고 진행 중 콜백을 비운 뒤 매핑만 해제한다. 정지가 누락되거나 핀·PMIC IRQ 복원이 실패했어도 늦은 MMIO 재시도를 하지 않는다. 복원 기록은 남겨 새 자원 할당을 차단하며, 연결된 LNA 핀 사용권은 소프트웨어에서 무효화하고 그 참조만 회수한다. 인터페이스 참조는 별도로 유지한다. 정상 종료·반복 반납·중간 실패·정지한 컨텍스트·LNA 두 사용권을 실제 ARM64 코드로 검사했고, 공통 코어 424,151개 메모리 오류 검사를 통과했다. 실제 장치 제거와 전원 복귀 검증은 남아 있다. [공식 반납 콜백 규칙](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/gpioclx/nc-gpioclx-gpio_client_release_controller).

Windows의 `CLIENT_StopController(SaveContext=TRUE)`는 소유 IRQ를 마스크하고 대기 중인 신호를 보존한다. 재시작에서 GRDY, GPIO/IOCFG, EINT 도메인과 인터럽트 종류가 유지됐는지 확인한 뒤 해당 소유 IRQ의 이전 마스크를 복원한다. PMIC 가상 IRQ 137도 같은 검사에 포함한다. 마스크 복원 실패 뒤 차단 상태와 재시도를 ARM64 모의 검사로 확인했다. OS 깨우기 핀 연결, 실제 Wake 및 전원이 끊긴 뒤 핀 초기화는 미구현 또는 미확인이다. `analysis/hall-cover-verification.json`에 소스·SYS·AML 해시와 범위를 기록한다.

### GPS 공통 통신·레지스터·MCU·LNA

`MtkGnssCore.h`의 `MtkGnssDecoderInit/Receive/Encode`를 같은 ARM64 COFF 라이브러리에 포함했다. 이 기기의 순정 `libmnl.so`에서 확인한 `AA F0`/`AA 0F` 구분·이스케이프·길이·검사값을 처리하며, 명령 종류와 상위 길이 비트는 원래 값으로 보존한다. 전체 프레임 512바이트·본문 502바이트 한도, 분할 입력·연속 프레임·잘못된 입력 차단과 비우기를 구현했다. STP→수신 대기열→GNSS 복원·이전 연결 차단을 함께 검사했고 메모리 오류 검사와 실제 ARM64 PE에서 각각 2,559,428개, 순정 ARM64와 901가지 대조를 통과했다. 선택한 원본 프레임 함수만 가상 실행했다. Windows UMDF2의 위치 요청 처리는 구현했으며, 실제 수신기·위치 계산 엔진 연결이 남아 있다. 이진 본문을 NMEA로 가정하지 않는다.

`MtkGnssOffloadInit/Receive/Reset`에 순정 보조 프로세서의 분할 데이터 조립을 추가했다. 기본 `AA F0` 입력과 별도인 포트 형식이며, 길이·전체 조각 수·1부터 시작하는 순번을 검사한다. 조각 본문 1,532바이트·완성 데이터 65,535바이트로 제한하고, 누락·중복·순서 오류·넘침은 내용을 비운 뒤 복구까지 차단한다. 복구 전 콜백은 세대 번호로 거른다. 순정 `nmea_recv → pack_data → 출력 함수 → 등록 콜백`의 실제 ARM64 코드와 가상 데이터 1,976가지 조건을 대조했고, 새 조립 검사를 포함한 GNSS 검사 2,559,428개를 메모리 오류 검사와 ARM64 가상 실행에서 각각 통과했다. 두 OS 라이브러리의 공용 함수는 동일한 38개다. NMEA 출력 포트의 전달 경로는 확인했으며, 실제 포트·동작 모드 선택과 수신 엔진·위치 서비스 연결은 구현 대상이다. 위치·시간·위성 기록과 RMC/GGA 출력 해석은 아래 처리기에 연결했으며, OS 소비자 연결을 이어간다.

`MtkGnssDecodeLocation`·`MtkGnssDecodeSvStatus`는 순정 Android 구현이 받는 104바이트 위치 기록과 48바이트 위성 기록을 해석한다. 좌표·WGS84 고도·속도·방향·정확도·UTC/경과 시간, 위성 ID·위성군·신호 세기·방위·고각·주파수를 구분한다. 실제 입력 길이·개수·유효 비트·숫자 범위를 검사하고, 잘못된 기록이 하나라도 있으면 결과를 일부 쓰지 않는다. 순정 위치 변환과 위성 콜백 전체를 가상 서비스에서 934가지 조건으로 대조했다. `MtkGnssMcuPosition`은 종류 2·본문 3,168바이트인 별도 MCU 메시지를 검사하며 순정 복사·조회 코드와 194가지 조건으로 대조했다. MCU 본문을 104바이트 위치 기록으로 취급하지 않는다.

`MtkGnssNmeaParse`는 확인된 NMEA 출력의 RMC·GGA 문장을 체크섬·CRLF·256바이트 상한으로 검사한다. 날짜의 세기는 호출자가 명시하며 이전 문장의 날짜·고도를 빌려 쓰지 않는다. 수신 무효는 위치 비트 없이 전달하고, GGA는 해발 고도와 지오이드 높이가 모두 있을 때만 WGS84 고도를 만든다. HDOP를 미터 단위 정확도로 바꾸지 않는다. 윤초는 POSIX 자정 정규화와 별도 표시를 함께 전달한다. 순정 출력 콜백에서 해석기로 이어지는 메시지 6개, 별도 정밀 수치·달력 계산 1,024개를 확인했다. GNSS 전체 검사 2,559,428개를 메모리 오류 검사와 ARM64 PE 실행에서 각각 통과했고 공용 함수 38개를 두 OS 라이브러리에 포함했다.

`MtkGnssNmeaStreamInit/Receive/Reset`으로 조각난 출력 줄과 연속 문장을 처리한다. 한 호출은 최대 256바이트와 문장 하나까지 소비하며, 남은 입력 위치를 돌려준다. 정확한 나노초 시각이 같고 두 좌표의 표시 정밀도 범위가 겹칠 때만 RMC의 날짜·속도·방향과 GGA의 고도·위성 수·HDOP를 결합한다. 결합한 두 기록은 즉시 비워 재사용하지 않는다. 수신 무효·만료·재설정도 대기 기록을 비우며, 연결 세대가 지난 콜백은 새 상태를 바꾸지 못한다. 부분 문장의 시간 초과·잘못된 체크섬·길이 초과·수신 시계 역행은 복구 전까지 차단한다. 호출자가 정한 수신 시간 제한은 실제 위치의 절대적인 최신성을 증명하지 않으므로 OS 연결 단계에서 별도 정책이 필요하다.

문장 결합·입력 분할·오래된 콜백 등을 다룬 가상 조건 520개, 순정 출력과 공용 전송 결과를 각기 연결한 결합 결과 6개를 확인했다. 보고 구조는 208바이트, 스트림 상태는 760바이트이며, 공용 함수는 동일한 38개다. 전체 검사는 줄 경계·별칭 인수·윤초·날짜 순서·시간 초과도 포함한다. 실제 포트·동작 모드 선택, 수신 엔진·RF 기동과 Linux 위치 서비스·Windows 하위 수신기 연결은 계속 구현할 부분이다.

순정 `gps_drv_stp.ko`의 GPS는 WMT/STP 채널 2를 사용한다. `MtkStpCoreLib`에 mandatory STP 송수신·수신 버퍼·이전 연결 콜백 차단, WMT 기능 제어와 MCU opcode `0x32`의 요청/응답·시간 제한을 구현했다. `MtkWmtGpsPowerSaveBegin`은 순정 절전/복귀 요청을 만들고 실제 응답 길이와 상태를 검사한다. 일반 MCU의 `Confirmed`는 완전한 응답 수신을 뜻하며, 임의 명령의 의미상 성공을 보장하지 않는다. `Windows/Build/MtkStpCore.lib`는 ARM64 COFF 정적 라이브러리다. 메모리 오류 검사와 ARM64 PE 모의 실행에서 각각 5,799,629개 확인, 순정 ARM64 함수와 16,199가지 대조를 통과했다. 결과와 해시는 `analysis/gnss-stp-verification.json`에 있다.

`MtkWmtRegisterBegin`에 GPS 준비와 펌웨어 기동에 필요한 WMT opcode `8` 레지스터 읽기·마스크 쓰기를 추가했다. 쓰기 응답 8바이트, 읽기 응답 16바이트의 종류·명령·선언 길이·상태를 검사하고, 읽기는 요청 주소가 일치해야 값을 공개한다. 시간 초과·불완전 전송 뒤에는 명령을 재전송하지 않는다. `MtkWmtGpsSyncBegin`은 순정 SoC 지원 비트가 확인된 경우에만 GPS 동기화 설정을 전송한다. 이 기기의 실행 중 지원 비트는 아직 확정하지 않았으므로 임의로 켜지 않는다. 순정 레지스터 제어 2,832가지와 GPS 동기화 514가지 조건을 대조했고, STP→BTIF 대기열·FIFO 송수신과 잘린 응답을 함께 검사했다. 동기화 명령의 성공 응답만으로 전체 GPS 기동이 끝났다고 처리하지 않는다.

기존 `MtkGpio/Hardware.c`의 `MgGpsLnaAcquire`·`MgGpsLnaSet`은 GPIO25·26을 독점 점유해 출력 끄기·DSP 제어·출력 켜기를 전환한다. 출력 값을 먼저 준비하고 모드를 전환하며 매 쓰기를 다시 읽어 확인한다. 기존 설정은 해제/중지 시 복원하고, 오류가 나면 저장 상태를 보존해 복구 전에 다시 쓰지 못하게 한다. 일반 GPIO 읽기/쓰기에서 이 핀을 제외하고, 점유한 채 GPIO 절전에 들어가거나 현재 상태를 남긴 채 해제하는 요청을 거부한다. GPIO/LNA의 ASan/UBSan 414,184개 검사와 실제 ARM64 코어 실행을 통과했다.

`MtkGpio/GpsLnaInterface.h`의 `GUID_MTK_GPS_LNA_INTERFACE_V1`을 실제 GpioClx 드라이버에서 `WdfDeviceAddQueryInterface`로 공개한다. 커널의 GNSS 전송 드라이버는 GPIO 원격 I/O 대상을 열고 `WdfIoTargetQueryForInterface`로 얻은 `Header.Context`와 `Acquire/Set/Release`를 호출한다. `Acquire`는 25·26만 허용하고 핀을 바꾸지 않는다. 점유마다 재사용하지 않는 토큰을 발급하며, `Set/Release`는 그 토큰을 검사한다. 세 함수는 PASSIVE_LEVEL에서 호출한다. 인터페이스 참조·핀 점유 각각이 장치 참조를 유지하고, 참조가 남으면 정상 중지·제거를 잠시 막는다. 핀 복원 실패 시 토큰과 원래 설정을 유지해 같은 토큰으로 다시 반납할 수 있다. RF를 먼저 멈춘 뒤 모든 핀을 반납하고 인터페이스를 해제해야 한다. 원격 대상의 query-remove/remove-complete/remove-canceled 처리도 호출자에게 필요하다. [Microsoft 인터페이스 참조 관리 설명](https://learn.microsoft.com/en-us/windows-hardware/drivers/wdf/using-driver-defined-interfaces), [정상 중지·제거 제어](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfdevice/nf-wdfdevice-wdfdevicesetstaticstopremove).

`IRP_MN_SURPRISE_REMOVAL`은 WDF 전처리에서 진행 중 접근을 끝내고 새 접근을 차단한다. 이때 핀 복원이나 다른 MMIO를 실행하지 않고 점유 참조를 회수하며, 원래 IRP는 WDF에 전달한다. 장치 준비 전·시작 전·핀 두 개를 빌린 상태의 제거, 반복 제거, 종료 후 호출, 이전 토큰과 토큰 소진, 참조 균형, IRQ 잠금 밖 WDF 호출, 복원 실패와 재시도를 실제 ARM64 코드에서 검사했다. 최종 미서명 `MtkGpio.sys`를 다시 빌드했다. 실제 GpioClx 등록·장치 제거·D3 복귀는 Windows 실행으로 확인해야 하며, 이 인터페이스를 호출하는 GNSS 드라이버와 RF 전원 공급자는 구현 중이다.

순정 `/soc/consys@18002000`에는 WMT가 찾는 `gps_lna_state_init/oh/ol` 설정이 없다. `/gps@18c00000`의 별도 L1/L5 pinctrl이 GPIO25·26에 연결된다. WMT의 LNA 처리 함수가 성공을 반환한다고 이 두 핀이 바뀌었다고 해석하지 않는다. 제공된 `WMT_SOC.cfg`의 `wmt_gps_lna_enable=0`, `co_clock_flag=1`과 순정 GPS 기동 코드의 공유 PALDO 제어도 구분해 분석했다. 전체 기동에는 이 전원 경로와 다른 무선 기능의 점유 조정이 필요하다.

`MtkVcn28Core`에 GPS/FM이 함께 쓰는 MT6358 VCN28 전원 순서를 추가했다. 순정 PALDO 선택 2·3이 같은 콜백으로 연결되는 것을 실제 ARM64 코드로 대조했고, ADIE가 `0x6635`이면 이 전원을 건드리지 않는다. 실행 중 칩 식별 결과를 받은 공용 RF 제어 주체만 사용한다. 정확한 DT의 2.8V 고정 전압·270µs 지연·`always-on`을 적용해 기본 전원을 유지한다. GPS/FM 사용권을 따로 관리하고 마지막 반납 때 기존 동작 설정을 복원한다. 쓰기와 전원 상태를 다시 읽어 확인하며 실패하면 원래 설정과 사용권을 보존한다. 두 OS의 ARM64 정적 라이브러리에 포함했다. 메모리 오류 검사와 ARM64 가상 실행에서 각각 20,157개 확인, 순정 ARM64 전원 함수와 49가지 대조를 통과했다. 실제 PMIC 공급자 연결, GPS 준비·펌웨어 기동과 기능 종료 확인을 묶는 전체 RF 제어는 남아 있다.

STP 송신은 최대 1,996바이트, MCU 본문은 송신 1,992·수신 1,995바이트다. 위치 본문을 NMEA로 가정하거나 알 수 없는 명령을 분할하지 않는다. WMT에는 요청 번호가 없어 공용 제어 주체와 순서·중복을 보장하는 물리 통신 공급자가 필요하다. conninfra·펌웨어·BTIF OS 자원/인터럽트·DMA·SDIO 전송, RF 전원·전체 절전·공존과 위치 계산 엔진은 남아 있다. Windows의 위치 요청·취소·시간/위성 보고 코드는 아래 UMDF2 어댑터에 구현했으며 하위 수신기 공급자에 연결해야 한다. 태블릿 GPS 장치 열기·ioctl·전원 변경·위치 수집은 미실행이다.

`MtkBtifCore`를 같은 라이브러리에 연결했다. 4KiB 대기열, 8/16바이트 FIFO 송신, 제한된 인터럽트 수신, 순정 128..160µs 깨우기 신호와 제어 상태 복원을 구현했다. 전송 도중 오류가 나면 중복 전송을 차단하고 상태·점유를 보존한다. BTIF의 메모리 오류 검사와 실제 ARM64 PE 실행에서 각각 169,966개 확인, 순정 ARM64 코드와 2,478가지 대조를 통과했다. `MtkBtifTests.exe`는 이 코드의 검사 프로그램이며 Windows 드라이버가 아니다. WDF 자원 매핑·클록·인터럽트 등록과 conninfra/펌웨어/RF 전원은 아직 연결해야 한다. 검사는 가상 FIFO와 시간 대기에서 수행했다.

### Windows GNSS 위치 요청 처리

`MtkGnss/Driver.c`에 UMDF 2.33·GNSS 버전 6의 실제 위치 IOCTL 처리를 구현했다. 플랫폼 협의, 단일 위치 요청의 시작·변경·중지, 대기 중 위치 조회, 파일 정리·절전·제거를 처리한다. 수신기 준비 확인 전에는 장치를 열지 못하며, 위치 사용 허가와 수신 시작을 나눈다. 수신기 호출은 500ms로 제한하고, 시작·중지 결과가 불확실하면 위치를 비우고 확인된 재설정 전까지 새 요청을 막는다. WDF 수동 대기열이 조회 취소를 맡는다. 재조회 실패 뒤에도 마지막 세션 번호를 기억하고, 새 수신기 세대가 확인됐을 때만 번호 기록을 초기화한다.

위치의 UTC가 현재 시각보다 5초 넘게 오래되거나 1초 넘게 앞서면 전달하지 않는다. 저장한 결과는 경과 시간으로도 5초 뒤 만료시키므로 시스템 시각 변경으로 오래 살아남지 못한다. 해발 고도와 타원체 기준 고도를 나눠 보고하고, 없는 고도·속도·방향·정확도는 NaN으로 전달한다. Android의 68% 정확도를 Windows의 95% 정확도로 표시하지 않으며, 하위 수신기가 별도로 제공한 95% 값만 사용한다. 위성군과 위성 ID를 함께 구분하고, 같은 위성의 여러 주파수 기록은 가장 강한 신호와 사용 여부로 합친다. 실제 공용 NMEA 스트림 출력에서 Windows 이벤트로 이어지는 경로도 검사한다.

`Receiver.h`는 실제 conninfra/수신기 드라이버가 내줄 하위 자식 장치와의 Windows 내부 호출 규칙이다. 펌웨어나 Android의 통신 형식이 아니다. 해당 공급자는 세션별 원자적 위치, 증가하는 사용 번호와 세대, 수신 종료·RF 정지 확인을 제공해야 한다. 그 공급자와 위치 계산 엔진은 아직 없으며, 이를 대신하는 Root 장치·가짜 위치·준비 완료 ACPI 값은 만들지 않았다. 전화형 기기의 SUPL·제어 평면·보조 위치 데이터 연결도 남아 있고 지원 플래그를 켜지 않았다.

`MtkGnss/build.py`는 기존 SDK/WDK와 Zig, 공용 빌드 잠금을 재사용한다. ARM64 `MtkGnss.dll`과 미서명 INF를 만들고, 실제 생산 콜백 코드를 ARM64 PE로 실행하는 검사 2,373개를 통과했다. WDF·수신기·시각은 가상 구현이며 Windows 로드·실제 RF/위치 수집은 실행하지 않았다. 산출물은 `Windows/Build/MtkGnss.dll`, `MtkGnss.inf`, `MtkGnssDriverTests.exe`, `MtkGnss-driver-tests.json`, `mtkgnss-verification.json`이다. 기존 KMDF 빌드에는 섞지 않는다. [Microsoft GNSS 요구 사항](https://learn.microsoft.com/en-us/windows-hardware/drivers/gnss/gnss-driver-requirements), [위치 조회와 중지](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/gnssdriver/ni-gnssdriver-ioctl_gnss_get_fixdata), [고도 항목](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/gnssdriver/ns-gnssdriver-gnss_fixdata_basic_2).


### BTIF BTHX data path update

The HANA6795 KMDF owner exposes an MS_BTHX_BTHMINI child and forwards static queries plus event/ACL reads and HCI writes through non-power-managed queues. The owner test routes HCI traffic through STP, the adapter and WDF queues, and exercises full-mode WMT Bluetooth function-on with firmware response gating. D0 exit and removal fail queued requests, and same-session resume binds a new HCI epoch. Owner tests passed 40,967 assertions and the HCI adapter passed 2,824. An unsigned 46,592-byte ARM64 SYS linked with its INF in `Windows/Build/btif-radio-wmt-host-20261122-r7/`; SHA-256 `b018137fc25c97cee52b73825b5ea892912e5bf2520bf3f39b7f36badba7242f`. Function ID 0 matches WMTDRV_TYPE_BT=0 in the T70 kernel-reference wmt_exp.h at commit a71515dbaf0d539ffb36416f14186dad17eef743; the WMT response path still has host coverage only. SCO sideband/HFP bypass, signing, OS loading and wireless exchange remain incomplete.

Windows HFP bypass is an audio-driver path: it watches `GUID_DEVINTERFACE_BLUETOOTH_HFP_SCO_HCIBYPASS` instances as paired devices arrive and leave, creates the corresponding PortCls wave/topology subdevices, and opens the SCO stream when a KS pin enters ACQUIRE. The WCN0 SCO SRAM/IRQ owner and this audio path must be connected before advertising `ScoSupportHCIBypass` and its single supported channel. [Microsoft HFP bypass streaming](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/bluetooth-hfp-bypass-audio-streaming), [HFP audio driver startup](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/startup), [stream-open contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/bluetooth-hfp-ddi-ioctls).

`MtkConn/ScoPacket.c` processes a captured BTCVSD interrupt batch: it validates the packet profile and both SRAM pointers, stages RX before TX, preserves per-slot padding, and leaves caller outputs untouched on validation failure. The WCN0 build runs its ASan/UBSan cases and compiles a 4,798-byte ARM64 object. `MtkConn` also captures and masks the stock INFRACFG_AO 0xf00 bit 11 under the existing SoC lease, then restores the inherited state after the WCN MCU stops; failed restoration retains the lease. Bootstrap checks passed 65,707 assertions and the SoC lease checks 133,310. The latest offline package and test report are in `Windows/Build/mtkconn-sco-irq-ack4-20261122/`. WCN0 validates and binds the distinct level-high GSIV 439, then its WDF ISR acknowledges only the BTCVSD control status bits from the referenced Linux driver while the SCO source remains masked; the exact T70 module mask is not binary-matched. The packet object is not linked into MtkConn.sys, no IRQ path transfers audio packets, and no HFP audio endpoint exists yet.
