# HX83132 정상 터치 보고 포팅

원본: `analysis/stock-binaries/vendor_dlkm_a/hxchipset_i2c.ko`, SHA-256
`c1d7d3ae53580d3d3a0b7cfdd99e01785d5d8fd6b5fd42b5d89659c00bd64929`.
원본 백업을 `rb`로 읽었으며 파일·파티션 출처는 `analysis/stock-binaries/manifest.json`에 있다.
바이너리의 HX83132 구현을 확인한 것이며, 실제 장치의 칩 ID를 새로 읽은 결과가 아니다.

## 바이너리 근거

- `himax_report_data_init`, `.text+0x918`: 좌표 4바이트×슬롯 수,
  4바이트 단위로 맞춘 압력 배열, 마지막 4바이트. 10슬롯의 정상 보고는 56바이트다.
  스타일러스가 켜지면 12바이트를 더 쓰므로 이 포팅의 정상 보고와 섞지 않는다.
- `himax_parse_report_points`, `.text+0xf38`, 2156바이트:
  슬롯별 big endian 16비트 X/Y, 압력 배열 `4*slots+i`, 좌표 상한 포함 비교,
  `info_size-4`의 접점 수 하위 4비트와 `0xff` 손 뗌 표시.
- `himax_ts_work`, `.text+0x2278`: 정상 보고의 8비트 합계 검사,
  모두 0인 데이터 거부, 정상/진단 모드 구분.
- `hx83132_read_event_stack`, `.text+0xd458`: `hx_s_ic_setup+0xb9` 명령으로
  `himax_bus_read`를 호출한다. `g_hx83132_setup`는 `.data+0x300`의 432바이트이며
  해당 바이트는 **0x30**이다. `_hx83132_init`의 `.text+0xf1b4..0xf1e4`가 이 설정을 복사한다.
- `himax_bus_read`, `.text+0x1c810`: 같은 주소에 명령 1바이트 쓰기와 읽기를
  `i2c_transfer(..., 2)` 한 묶음으로 전달한다. 중간 STOP을 넣지 않는다.

REA Hopper Evidence:
`ev_2dde5eb4fcac4697826da1960bac85862dc1c6ad675288e364dd19fef464307e` (길이),
`ev_7ad05cb1440ec39bd694270a587a7c195666717a801beae45fa094f6f2fbc998` (좌표),
`ev_117ece5843b1805b57f87f1ca5951009b2c6aa1a76d669623ff17a16fc04d811` (이벤트),
`ev_b2f72f21d3ea081b6bfdc4a1b8ca5f9f5c9123621a7b81f46fcb4c816a4b9af9` (전송),
`ev_e2f6fd3f4f1655fee3b273b6b5c5b17d681b54bc5b76ae128db575f345134c97` (합계).
Hopper의 ET_REL 의사 코드에는 전역 변수·문자열을 잘못 연결한 부분이 있다.
주소·필드 해석은 원본 ELF의 `llvm-objdump-21 -dr`과 재배치 항목으로 대조했다.

## 칩 통신과 시작 확인

원본 HX83132 모듈에서 간접 레지스터 명령 경로도 ELF 재배치와 명령열을 대조했다.
`himax_bus_write`는 명령 바이트 뒤에 주소를 little-endian으로 붙이며, 제조사 전용 sentinel
`0xffffdaf1`이면 주소 없이 데이터를 바로 보낸다. 쓰기 본문은 최대 252바이트다.
`himax_bus_read`는 명령 1바이트 쓰기와 읽기를 반복 START 한 묶음으로 수행하고
1~256바이트 읽기를 받으며, 전송 실패는 3회 재시도와 20,000~20,001 μs 대기를 한다.

`_hx83132_init`은 간접 레지스터 `0x900000d0`에서 4바이트를 읽고
`[3]=0x83`, `[2]=0x13`, `[1]=0x2a..0x2c`인지 확인한다. `HmReadChipId`가
같은 읽기와 검사를 구현한다. `hx83132_sense_on`의 무리셋 분기는 `0x9000005c`에
4바이트 0을 기록하고 10 ms 대기한 뒤 sentinel 명령 `0x31`에 0을 보낸다.
`HmSenseOn`은 이 분기와 앞선 `himax_mcu_interface_on` 호출을 함께 구현한다.

`himax_mcu_interface_on` (`.text+0xfb20`, 372바이트)은 raw `0x08` 4바이트 읽기로 인터페이스를 깨우고,
`0x13/0x31`, `0x0d/0x10`을 기록한 뒤 두 레지스터를 raw 읽기로 다시 확인한다.
불일치하면 1,000~1,100 μs 기다리고 최대 10회 반복한다. `HmInterfaceOn`은 이 순서를 따르며,
실제 읽기·쓰기 오류는 후속 SenseOn을 막는다. `HmInterfaceOn`은 열 번의 값 불일치를
진단할 수 있도록 `HmTimeout`을 반환하며, `HmSenseOn`은 이 경우 순정의 void 함수와 같이
시작 명령을 계속한다. 펌웨어 쓰기처럼 명시적 확인이 필요한 호출자는 시간 초과를 그대로 처리한다.

`hx83132_system_reset` (`.text+0xd39c`, 184바이트)의 `0x90000018 ← 0x00000055`도
`HmSystemReset`으로 구현했다. 이는 단일 소프트웨어 리셋 명령이며, 전체 전원·안전 모드·펌웨어
복구 절차가 아니다. 기존 EDK2/Windows 입력 시작 경로에서는 이 함수를 호출하지 않는다.

REA Hopper 근거: 인터페이스 `ev_c6341531a834e9a941bf12029838776b9f5aee20ca83d342fb04d8b36d3c31e3`,
SenseOn `ev_1b68fc56c030f55f52bd74c5221c786d2f631193c91d00f2274bae54f51557f7`.
ELF 재배치로 간접 함수 표 `hx_s_core_fp+0x18`이 인터페이스 함수임을 대조했다.
`himax_mcu_hand_shaking`은 원본에서 단순히 0을 반환하므로 펌웨어 실행 증거로 쓰지 않는다.

EDK2 `Start`와 Windows `D0Entry`는 보드가 순정 펌웨어 실행 중과 독점 버스/핀 소유를
확인한 다음 ID를 직접 읽고, 일치하면 리셋 없는 SenseOn 경로를 실행한다. ID 또는 전송 오류면
입력 장치를 노출하지 않는다. 전원·GPIO 콜드 초기화 순서와 펌웨어 업로드·전원 손실 복구는 아직 구현하지 않았다. 실제 장치에서 ID·시작 명령·정상 보고가 성공했는지는 미확인이다. HmEnter는 최초 인계와 retained resume을 구분한다. 복귀는 새 보드 확인 뒤 ID를 읽으며 reset/SenseOn을 반복하지 않는다. ID 읽기에는 간접 I²C 주소 설정 쓰기가 포함된다.

## 실제 구현과 검사

`HimaxCore.c`는 정상 프레임 해석과 OS 비종속 간접 I²C 명령·ID/sense-on 절차를 제공한다. 접점 수와 유효 좌표 수가
모순되면 거부하는 방어 검사를 더했다. 오류 때 출력은 변경하지 않는다.
`HmPointerUpdate`는 UEFI의 한 포인터에 첫 접점을 유지하며, 해당 손가락을 떼면
해제 상태를 먼저 전달한 뒤 다음 보고에서 다른 접점으로 옮긴다.

`HimaxTouchDxe`는 표준 I2cMaster의 반복 START 읽기·쓰기와 AbsolutePointer를 제공한다.
IRQ가 대기 중일 때만 읽고, 오류 3회 뒤 눌림을 해제한다. 보드 준비부가 실행 중인 순정
펌웨어·독점 핀/버스 소유권과 IRQ 대기 조건을 검증해 `HIMAX_TOUCH_BOARD`를 발행해야
연결된다. 칩 ID는 DXE가 직접 읽고 검증한다.
T70GpioPlatformDxe에 발행부를 구현했다. 정확한 DTB·GPIO9 입력/하강 에지·GPIO152 리셋 해제·모든 EINT mask/AP 도메인을 검사한다. 단독 I²C 사용권을 얻어 ID·정상 보고를 읽고 IRQ가 해제된 뒤 사용권을 반환해야 발행한다. revision 2의 State 콜백은 시작·유효 보고·오류·중단을 전달하며, 시작 뒤 유효 보고가 있어야 EBS 준비 값과 ACPI HMXR이 허용된다. 최대 3회만 통신 검증하며, 콜드 기동·펌웨어 복구·절전 복귀는 구현하지 않았다. 보드 공급자와 HimaxTouchDxe는 현재 FV에 포함되며, 실제 준비 조건을 통과해야 입력을 노출한다.

ASan/UBSan 검사: `Tests/HimaxCoreTest.c`, `Tests/HimaxDxeTest.c`; 최신 181,120개와 6,788개 검사가 통과했다. 보드 공급자 `GpioPlatformTest.c`의 111,124개 검사도 통과했다. DXE 실행 검사는 타이머 해제, 이벤트 닫기, 프로토콜 제거·닫기 실패 때 콜백 문맥을 보존하고 재시도하는 경로도 확인한다. Windows 실제 드라이버의 초기화 경로도 ARM64 실행 검사에 추가했으며 ARM64 실행 검사 결과는 `Windows/Build/HimaxTouch-driver-tests.json`에 기록한다.
`Tests/verify-himax-binary.py`는 원본 함수에 ELF 재배치 135개를 적용해 Unicorn에서 실행하고,
실제 포팅 C와 정상 프레임 1024개(0~10접점)의 활성 상태·좌표·압력을 비교했다.
좌표 함수의 `_printk`는 결과 없는 호출로 가로챘다. 추가로 원본 인터페이스·리셋·SenseOn 함수와
호출되는 burst/register-write/주소 직렬화 함수를 실행해 시작 경로 23가지를 비교했다.
인터페이스 확인 실패 0~10회, 쓰기 바이트·읽기 길이/반환·대기 범위가 일치하며, 소진 뒤
순정과 같은 후속 SenseOn 진행도 확인했다. 이 검사에서는 버스·mutex·대기·로그만 대체하고,
예상하지 않은 함수 실행은 거부한다. 하드웨어나 Windows 커널 실행 결과는 아니다.
보고서: `analysis/himax-binary-tests.json`, `analysis/himax-init-evidence.json`, `analysis/mediatek-driver-tests.json`.

명령은 Mu-Silicium 루트에서 실행한다.

```sh
Resources/Scripts/with-build-lock T70Pro 'Himax binary comparison' -- \
 /home/builder/teclast-t70pro/.venv/bin/python \
 Platforms/Teclast/t70proPkg/Tests/verify-himax-binary.py
```

## 미확인

실제 칩 ID/정상 보고 설정, GPIO9 극성·전원/리셋152 순서, 펌웨어 재사용/업로드,
진단·제스처·펜, 절전 복귀, IRQ/DMA/화면 회전과 실기기 입력은 미확인이다.
이 검사는 커널 함수의 보고 해석을 비교하며 터치 하드웨어 구동을 증명하지 않는다.

## 펌웨어 검사와 제한된 sense-off

원본 Himax_firmware.bin은 261,120바이트이며 SHA-256은
49852ca3bcc829247f319372003a7c12fac2b3b9a517d61a50c066a24df26add이다.
HmCrc32c는 원본의 초기값 ffffffff·반사 다항식 82f63b78·마지막 xor 없는
4바이트 정렬 CRC를 구현한다. HmInspectFirmware는 16바이트 descriptor의 합계와
끝 범위를 검사하며 중복된 유효 tag를 거부한다. HmValidateT70Firmware는 길이·CRC residue 0과
네 tag/value/offset을 확인한다. 이는 형식·변형 검사이며 암호학적 파일 식별은 manifest의 SHA-256이다.

HmSenseOff는 검증된 T70 펌웨어 요약과 hx_s_ts+0x78의 bit4가 알려진 0이라는
호출자 확인을 요구한다. 확인되지 않거나 1이면 I²C 접근 전에 HmUnsupported를 반환한다.
해당 필드를 쓰는 원본 함수는 himax_debug_level_write이며 dbg_lvl/debug_level 문자열도 확인했다. bit4를 펌웨어 overlay 옵션으로 부를 근거는 없으므로 API는 runtime bit4로만 이름 붙였다.

시작 단계는 0x9000005c에 0xa5를 쓰고 0x900000a8 상태를 읽는다. 상태가 0x05면
10 ms 기다린 뒤 0x9000005c를 읽고, raw 0x31/0x27/0x95 명령 뒤에는 0x900000a8이
0x0c인지 확인한다. 여섯 번 안에 확인되지 않으면 재시도 중 단일 reset을 보내며 HmTimeout으로 끝낸다.

원본 stop helper는 상태가 0x05일 때 0x9000005c 읽기, 0x5a 쓰기, 20~21 ms 대기를
51회 수행한다. 0x6a를 읽어도 같은 요청을 계속한다. 코어는 이 순서를 그대로 수행하되,
요청 완료를 정지 성공으로 간주하지 않는다. 뒤의 broadcast 단계에서 완료를 확인한다.

HmWaitBroadcastLine은 0x8007008c를 1/0으로 쓰고 각 단계에서 10~11 ms 대기 후
0x80070034를 읽는다. 두 단계는 원본과 같은 busy 횟수 제한을 공유한다. 완료되면
0x80070084에 0x7e를 쓰고, 0x80070004를 조정한 뒤 0xc00f3100/0xc00f3200에
각각 0x27/0x95를 쓴다. 0x80070004의 후속 조정과 마지막 상태 확인까지 수행한다.
완료 제한에 도달했는데도 true를 반환하는 원본과 달리, 코어는 HmTimeout을 반환하며
완료 뒤 설정을 쓰지 않는다. 실제 통신 오류도 그대로 오류로 반환하고 대기 중 오류에는
0x8007008c 정리를 시도한다.

원본 ARM64와 성공·경계·대기 소진·재시도 17가지의 전체 전송·읽기 결과·대기 순서를
대조했다. 근거는 analysis/lk-ram-boot/Himax-compatibility-REA.json과
analysis/himax-binary-tests.json에 있다. bit4가 1인 추가 slave scan, 펌웨어 지우기·
잠금 해제, 전원 손실 복구, GPIO reset/power sequence는 구현하지 않았다.
이 API를 정상 입력/D0/resume에 연결하지 않았으며 실기기에서는 실행하지 않았다.
