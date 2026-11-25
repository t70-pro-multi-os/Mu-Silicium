# Android/UEFI 부팅 선택기와 설치 묶음

2026-11-16. Android를 보존하는 UEFI 첫 시험용 설치 묶음을 만들었다. Mac에서 계획 확인·복구 모듈 검사를 통과했다. 실제 기기 쓰기·부팅·원본 복구는 미실행이다.

## 바로 사용할 파일

Mac: /Users/you/.tmp/teclast-t70pro/install-ready/
Builder: Mu-Silicium/Build/t70proPkg/InstallBundle/
자세한 순서는 묶음의 설치안내.txt에 있다.

~~~sh
cd /Users/you/.tmp/teclast-t70pro/install-ready
./install.sh plan
./install.sh doctor
./install.sh install --execute --reboot
~~~

설치는 Android 정상 부팅·USB 디버깅 허용 상태에서 시작한다. 연결 대기 문구가 나오면 전원을 끄고 USB를 연결한다. 원본 복구는 Android가 켜지지 않아도 다음 명령으로 실행한다.

~~~sh
./install.sh restore --execute --reboot
~~~

새 OS는 포함하지 않는다. esp/EFI/BOOT/BOOTAA64.EFI는 ARM64 EFI Shell이다. 선택한 OS와 저장 매체를 확인한 뒤 해당 파일과 드라이버를 준비한다.

## 최종 구성

제조사 LK → boot_a 초기 선택기 → 원래 Android 커널 또는 UEFI. 루트 권한이나 LK 교체 없이, LK가 /chosen/atag,boot에 넣는 모드 2로 UEFI를 선택한다. 일반·재부팅·알람·충전 모드는 Android로 이어진다. 잘못된 기동 정보·예상하지 않은 메모리 위치·진단 모드는 커널 실행 전에 멈춘다.

원본 LK 0x10380은 유효한 A/B suffix가 있으면 복구 모드에서도 boot_a/b를 읽는다. suffix가 비었을 때만 별도 recovery를 읽는다. 이 분기는 원본 LK 코드와 실제 백업의 비밀값 없는 32바이트 부팅 정보로 모의 실행했다. 설치 도구는 현재 misc의 형식·CRC·슬롯 수·A 우선도·정상 부팅 표시와 비어 있는 부팅 명령을 확인하며 수정하지 않는다.

USB 데이터 케이블을 빼고 전원을 끈 뒤 음량−와 전원으로 LK 복구 경로를 고른다. 메뉴가 나오면 Recovery Mode를 선택한다. 메뉴의 실제 안내는 VOLUME_UP으로 선택, VOLUME_DOWN으로 확정이다. 버튼 조합은 LK 코드와 키 맵에서 추론했으며 실기기 확인 전이다. 기본 Android 부팅과 일회성 UEFI 선택을 각각 시험해야 한다.

Recovery 경로를 UEFI로 사용하므로 원래 Android 복구 화면·업데이트·초기화·리락 전에는 원본 boot를 복구한다. adb reboot recovery, misc 직접 쓰기, 슬롯 전환을 부팅 선택 수단으로 사용하지 않는다.

## 이미지와 보존 범위

최종 빌드: Mu-Silicium/Build/t70proPkg/LkBootSelect-AB/
- 선택기와 두 커널 본문: 44,908,544바이트, SHA-256 a60050c19aa8149807b7dd81c078bb81004cabac8cf6e5326a3e024115241d0c.
- legacy LZ4: 23,780,665바이트.
- Android boot v4 + AVB hash footer: 67,108,864바이트.
- 최종 boot.img SHA-256: e9ea13616663ed06595e360c4bf3ec00bb9829365205765f1286f2cf76541a46.

원래 Android 커널은 본문 전체와 선언된 0 채움 영역을 그대로 복사한다. FDT와 재부팅 요청 메모리를 수정하지 않으며 키 회로에도 접근하지 않는다. 새 GKI·초기 /init·kexec·pKVM 변경은 이 후보에 사용하지 않는다. 기존 GKI 재빌드 후보의 Rust CRC 차이 1건은 별도 미해결 후보로 남긴다.

원본 vbmeta_a는 boot chain descriptor를 가진다. 새 크기와 해시는 boot 자체에 기록하며 원본 vbmeta를 보존한다. 새 boot는 OEM 서명 이미지가 아니므로 언락 상태에서만 시험한다. OEM 서명이나 DRM·RPMB·퓨즈 복구를 제공하는 묶음이 아니다.

## 설치·복구 구현

기존 제조사 DA 복구 키트를 사용한다. 설치 실행 때 ADB 속성 여섯 개로 슬롯 A·언락·정상 Android 부팅·정확한 순정 버전을 확인한다. 같은 기기, 원래 UFS 크기와 GPT, LK/init_boot/vendor_boot/dtbo/vbmeta 해시를 확인한 뒤 정해진 주소의 boot_a 64MiB만 쓴다.

16MiB 이하 블록마다 기록 의도를 저장하고, 쓴 내용을 즉시 다시 읽으며 마지막에는 전체 boot 해시를 확인한다. 설치 오류가 나면 같은 연결에서 원본 boot 복구를 시도한다. 연결이 끊겼다면 restore를 다시 실행한다. 복구는 ADB와 정상 A/B 정보에 의존하지 않지만 기기·GPT·부팅 펌웨어 보호 검사는 유지한다. 실제 DA 쓰기와 재연결 원복은 미확인이다.

원본 대용량 백업을 중복 복사하지 않았다. 묶음은 기존 /Users/you/.tmp/teclast-t70pro/device-backup/recovery-kit과 원본 백업을 함께 필요로 한다.

## 확인 결과

| 검사 | 통과 | 실제 확인한 범위 |
| --- | --- | --- |
| 최종 초기 선택기 | 34개 | 컴파일된 ARM64 코드의 EL1/EL2 분기, 잘못된 입력에서 정지, FDT 보존, 실제 Android·UEFI 전체 복사 |
| 설치·복구 도구 | 16개 | 일반 파일에 실제 쓰기·전체 재확인·원본 복원, 중단·잘못된 읽기 후 자동 복구, 쓰기 범위와 부팅 정보 보호 |
| boot/AVB | 6개 | 공식 avbtool와 별도 검사기의 전체 이미지 검사, 손상·잘림·잘못된 메타데이터 거부 |
| 원본 LK A/B 복구 분기 | 4개 | 유효 A/B suffix와 빈 suffix의 실제 코드 분기 |
| 원본 LK LZ4 | 2개 | 순정 커널과 최종 선택기 전체 압축 해제·바이트 일치·출력 뒤 보호 영역 유지 |
| 묶음 보호 | 2개 | 증명 없는 진입 경로 거부, 기존 출력 보존 |
| 이전 RAM 요청 선택기 | 24개 | 공유 코드 변경 후 기존 동작 유지 |

최종 묶음 15개 파일의 해시와 Mac install.sh plan·doctor·셸 문법 검사를 통과했다. 모의 실행은 선택한 커널의 진입 직전에 멈추므로 Android·UEFI 자체 부팅이나 실기기 쓰기 성공으로 표시하지 않는다.

## 실제 기기에 남은 항목

수정 boot의 LK/AVB 수용, 실제 적재 주소·압축 해제, 버튼으로 UEFI 선택, 기본 Android 부팅, 화면·입력·외장 EFI, 원본 boot 복구 후 화면·터치·파일/앱·Wi-Fi·SIM 인식·충전을 확인해야 한다. 무인 원복은 미확인이다. 내장 GPT나 새 OS 파티션 구성은 이번 시험에 포함하지 않는다.

근거: analysis/installation-preparation/의 lk-bootmode-lz4-avb-evidence.md/.json, lk-lz4-tests.json, avb-chain.json, 각 검사 보고서와 status.json.
