# Windows Himax HX83132 터치 드라이버

## 현재 상태

KMDF/VHF 기반 ARM64 드라이버 소스와 미서명 SYS 파일을 만들었습니다. ARM64 링크와 실제 HID 변환 코드 검사는 통과했습니다. Windows에서 장치 바인딩, 드라이버 적재, 화면 입력은 실행하지 않았습니다.

## 구현한 경로

- HimaxCore가 순정 HX83132 패킷을 검사하고, HID 변환기가 하드웨어 슬롯에 대응하는 접점 ID를 유지합니다.
- INF는 Microsoft VHF가 요구하는 Vhf.sys lower filter를 기존 LowerFilters 목록을 보존하며 추가합니다. InfVerif는 실행하지 않았습니다.
- HID Touch Screen 컬렉션은 10개 접점, Contact Count, Contact Count Maximum, 입력 보고서 ID 1(버퍼 72바이트), 기능 보고서 ID 2(버퍼 2바이트)를 정의합니다.
- 입력 Contact Count는 현재 접점과 올림 상태를 알리는 이전 접점을 모두 셉니다. 활성 접점과 올림 접점을 앞쪽으로 압축하고, 올림 보고서에는 직전 좌표와 압력 0을 넣습니다.
- Resource Hub 연결 경로를 열면 Windows가 이를 MtkI2c의 SPB 타깃으로 다시 연결합니다. MtkI2c `TargetConnect`가 I²C V2 설명자를 확인하므로 Himax는 SPB 타깃에 Resource Hub 전용 조회 IOCTL을 다시 보내지 않습니다. Himax 쪽은 주소 0x48, 400 kHz, raw/translated 자원에 같은 연결 ID, 단독 edge IRQ 하나, DTB의 GPIO9/하강 에지를 확인합니다.
- PassiveHandling ISR은 50 ms 제한으로 0x30 명령을 보내고 56바이트를 읽는 SPB 결합 전송만 수행합니다. 주기적 폴링은 없습니다.
- 연속 3회 읽기·패킷 오류가 발생하면 오래된 대기 보고서를 버리고 접점 올림을 대기한 뒤 Windows IRQ를 비활성화합니다.
- D0 진입은 HMXR 보드 준비 확인 뒤 간접 레지스터로 HX83132 ID를 직접 읽어 검증합니다. ID가 `[무시, 0x2a..0x2c, 0x13, 0x83]`와 맞지 않으면 입력 장치를 켜지 않습니다.
- 최초 D0는 HMXR을 사용합니다. 이후 D0는 HMRD에서 GPIO152 리셋 해제와 GPIO9 입력 설정을 현재 값으로 확인합니다. 응답이 없거나 진입 번호가 일치하지 않으면 I²C에 접근하지 않습니다. 이어서 Resource Hub를 통해 칩 ID와 `0x900000a8`의 순정 실행 상태 `0x05`를 읽은 뒤에 입력을 다시 엽니다.
- 통신 시작은 raw `0x08` 읽기, `0x13/0x31`·`0x0d/0x10` 쓰기와 두 값의 재확인을 거칩니다. 최대 10회 확인에도 값이 맞지 않으면 순정과 같이 입력 시작을 계속합니다. 실제 읽기·쓰기 실패는 입력 시작 전에 오류로 처리합니다.
- 확인된 리셋 없는 SenseOn 경로는 `0x9000005c`에 4바이트 0을 쓰고 10 ms 기다린 뒤 명령 `0x31`에 0을 씁니다. 소프트웨어 리셋 명령은 공통 코어에 별도로 구현·모의 검사했으며 이 경로에서 호출하지 않습니다. 공통 코어에는 검증된 펌웨어 요약과 실행 상태의 bit4가 명시적으로 0이라는 조건을 요구하는 sense-off를 구현했습니다. 순정의 정지 요청 반복 뒤 broadcast 완료를 확인하고 후속 설정까지 수행합니다. 완료를 확인하지 못하면 HmTimeout으로 끝내며 후속 설정을 쓰지 않습니다. 정상 D0에서 자동 호출하지 않으며 bit4가 1이거나 모르는 경우 쓰기 전에 거부합니다. 리셋 뒤 펌웨어 업로드·전원 손실 복구는 구현하지 않았습니다.

순정 Himax_firmware.bin(261,120바이트)의 AP CRC32C와 16바이트 descriptor를 해석·검사합니다. CRC/형식 검사는 정확한 파일 식별을 대신하지 않으며, 원본 manifest의 SHA-256으로 파일을 고정합니다. 상태가 유지된 절전 복귀에는 ID와 펌웨어 상태를 읽는 데 필요한 I²C 주소·버스트 설정만 사용합니다.

## 장치 활성화 조건과 미확인 사항

INF의 ACPI\HANA8313에 대응하는 HXT0와 HMXR을 플랫폼 ACPI에 구현했습니다. `_DEP`는 I2T0와 GPO0이고 `_CRS`는 I²C V2/0x48/400 kHz와 GPIO9 Edge/ActiveLow입니다. HMXR은 두 부모 준비와 터치 준비 여부·주소·핀·버전을 모두 검사합니다. 보드 공급자와 터치 DXE는 현재 FV에 포함됩니다. HMXR은 보드 자원, 칩 펌웨어 실행 상태와 터치 DXE 시작을 확인합니다. 유휴 상태에서도 준비 값을 내보냅니다. Windows는 인터럽트를 열기 전에 대기 중인 보고서를 한 번 읽습니다.

T70GpioPlatformDxe는 런타임 DTB·GPIO9 입력·GPIO152 리셋 해제·EINT 소유 상태를 확인하고 I²C master를 단독으로 열어 HX83132 ID와 정상 보고, 읽기 뒤 IRQ 해제를 확인합니다. 주파수는 400 kHz를 요청하며 분주 결과 100~400 kHz만 허용합니다. 부팅당 통신 검증은 최대 3회이고 소유권 반환 실패 때는 보드 프로토콜을 발행하지 않습니다. 입력 드라이버가 시작한 뒤 새 정상 보고를 확인해야 EBS에서 준비 값을 남기며, 오류·중단·반복 EBS 때 취소합니다. 전원을 잃은 경우의 초기화는 별도 경로가 필요합니다.

순정 바이너리에서 이벤트 읽기 함수와 패킷 형식은 확인했지만, 실제 보드의 IRQ provider, 극성·트리거 설정, 읽기 완료 후 IRQ 해제 동작은 장비에서 검증하지 않았습니다. 따라서 ARM64 빌드와 변환기 검사 통과를 실기기 터치 지원 완료로 간주하면 안 됩니다.

## 빌드와 로컬 검사

빌더에서 다음 명령을 실행합니다.

    python3 Windows/build.py --himax
    python3 Windows/build.py --himax --test

첫 명령은 서명하거나 설치하지 않고 Windows/Build/HimaxTouch.sys를 ARM64 native driver로 교차 빌드합니다. 두 번째 명령은 실제 패킷 변환 코드와 Driver.c를 포함한 ARM64 PE harness를 실행합니다. Unicorn에서 실제 HmInterruptIsr, HmReadWork, HmD0Entry, HmD0Exit, HmVhfReady 경로를 실행하고 WDF/VHF/SPB/kernel 호출만 모의합니다. 칩 ID 성공·불일치, 인터페이스 재확인·소진, sense-on 명령 순서, HMXR 실패·재진입 시 무통신 거부, raw/translated 자원 불일치·잘못된 IRQ·누락을 검사합니다. Resource Hub 경로 열기부터 HmPrepare 성공/해제까지 실행하고, SPB 타깃에 Resource Hub 조회 IOCTL을 보내지 않는지 확인합니다. 잘못된 raw/translated 자원과 대상 생성/열기·인터럽트 생성 실패 뒤 정리도 검사합니다. I²C 설명자 검사는 MtkI2c의 `TargetConnect` 콜백 테스트에서 실행합니다. 동기식 VHF 준비 콜백, 제출 실패 뒤 다음 토큰 대기, 세 번 연속 읽기 실패 후 올림·IRQ 중단, 큐 넘침 뒤 재동기화, 준비 토큰 대기 중 FIFO 보존과 작업 반복 실행 방지, lift 제출 중 새 overflow 보존, 50 ms SPB 요청 구성을 확인합니다. 이 검사는 Windows 커널에서 드라이버 적재, 실제 IRQ line, 컨트롤러 전기 동작을 확인하지 않습니다. 태블릿에는 접근하지 않습니다.

## 명시적 플래시 페이지 쓰기와 하드웨어 CRC

공통 코어에 `HmProgramT70Firmware`와 `HmVerifyT70FirmwareFlash`를 구현했다. 순정 HX83132 모듈의 실제 ARM64 함수를 실행해 1,020개 페이지의 16+240바이트 쓰기, 53,041개 버스 요청과 하드웨어 CRC 14개 요청을 C 구현과 대조했다. 준비 지연 두 번, 페이지 busy 101회 제한, CRC busy 제한도 대조했다. 페이지 timeout은 101번 읽고 100번 기다리며, CRC timeout은 원본의 모호한 0 반환 대신 HmTimeout으로 끝낸다. 원본 CRC timeout의 진단 콜백은 검증한 함수 포인터 경계에서 모의 처리했다.

대조에서 페이지 전송 제어값, WIP의 제어/명령/읽기 주소, SPI200 FIFO 예외 주소의 빠진 0을 수정했다. FIFO `0x8000002c`의 긴 전송은 주소 자동 증가를 끄며 일반 주소와 구분한다. CRC 길이 쓰기는 4바이트 `[count_lo,count_hi,0x99,0x00]`다. ASan/UBSan 공통 코어 검사 181,120개가 통과했다. 시작 경로 23가지와 지원하는 sense-off 경로 17가지는 순정 ARM64 실행 결과와 전송·대기 순서를 대조했다.

두 API는 정확한 기종·안정된 전원·정지한 터치·정리된 IRQ·단독 버스·이미지 SHA256 확인을 호출자가 입증했다는 명시적 조건을 요구한다. 페이지 쓰기는 대상이 지워지고 플래시 쓰기 잠금이 풀렸다는 조건도 요구한다. 코어는 이미지 형식·descriptor·AP CRC와 단계별 실제 성공만 확인하며 완료한 페이지 수를 기록한다. 이 조건 입력 자체가 하드웨어 상태를 증명하지는 않는다.

플래시 지우기·잠금 해제·runtime bit4가 1인 sense-off·재시작·전원 손실 복구는 남아 있다. 새 API는 제품 D0/resume/ISR에서 호출하지 않는다. 전체 터치 펌웨어 복구나 실제 업로드가 완료된 상태로 표시하지 않는다.
