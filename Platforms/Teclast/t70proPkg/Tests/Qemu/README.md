# T70Pro QEMU 구현 실행

현재 제품 UEFI Image를 CPU가 실제 실행하고, 독립 QEMU USB HID/SCSI/SD 카드가 제품 드라이버에 응답한다. 커스텀 t70-test 보드는 CPU/GIC/타이머와 T70 컨트롤러 주소를 연결한다. 제품 펌웨어의 함수나 성공값을 대체하지 않는다.

## 실행 경로

- ARM64 Image → SEC → DXE → UiApp. EL1/EL2, 실제 MMU/캐시 및 타이머 IRQ를 확인한다.
- USB HID → QEMU MUSB → MusbHostDxe → UsbBus/UsbKb → 콘솔/메뉴.
- USB SCSI → MUSB PIO → USBMassStorage/BOT → BlockIO/FAT → 외장 EFI.
- QEMU SDState → MSDC PIO 어댑터 → MsdcDxe/SdDxe/DiskIo/FAT → SD EFI.
- SD 제거/재삽입, SDSC 2MiB/SDHC 4GiB, 끝 섹터/범위 초과, 쓰기 거부와 매체 해시 보존을 확인한다.

## 실행 명령

builder 안에서 기존 작업 잠금을 잡은 상태로 실행한다. 동일 잠금 안에 다시 with-build-lock을 넣지 않는다. 실제 기기 명령은 없다. 기존 결과가 있는 출력 폴더는 덮어쓰지 않는다.

다음은 builder에서 프로젝트 잠금을 이미 잡은 상태의 명령이다. 출력 폴더 이름은 실행마다 새로 지정한다.

```sh
T70_ROOT=/home/builder/teclast-t70pro
T70_PKG="$T70_ROOT/Mu-Silicium/Platforms/Teclast/t70proPkg"
"$T70_ROOT/.venv/bin/python" "$T70_PKG/Tests/Qemu/prepare-qemu.py"
bash "$T70_PKG/build.sh" --locked firmware-trial
"$T70_ROOT/.venv/bin/python" "$T70_PKG/Tests/Qemu/verify-qemu.py" --output "$T70_ROOT/analysis/QEMU-new-sd" --el 2 --seconds 6 --sd
"$T70_ROOT/.venv/bin/python" "$T70_PKG/Tests/Qemu/verify-qemu.py" --output "$T70_ROOT/analysis/QEMU-new-sdhc" --el 1 --seconds 6 --sdhc
"$T70_ROOT/.venv/bin/python" "$T70_PKG/Tests/Qemu/verify-qemu.py" --output "$T70_ROOT/analysis/QEMU-new-keyboard" --el 2 --seconds 6 --usb keyboard
"$T70_ROOT/.venv/bin/python" "$T70_PKG/Tests/Qemu/verify-qemu.py" --output "$T70_ROOT/analysis/QEMU-new-storage" --el 1 --seconds 6 --usb storage
```

맥에서 잠금을 잡고 SD 실행 한 회를 호출하는 예:

```sh
T70_HOST=/Users/you/OrbStack/builder/home/builder/teclast-t70pro
T70_ROOT=/home/builder/teclast-t70pro
"$T70_HOST/Mu-Silicium/Resources/Scripts/with-build-lock" T70Pro "QEMU SD 검사" -- /opt/homebrew/bin/orb run -m builder -u builder "$T70_ROOT/.venv/bin/python" "$T70_ROOT/Mu-Silicium/Platforms/Teclast/t70proPkg/Tests/Qemu/verify-qemu.py" --output "$T70_ROOT/analysis/QEMU-new-sd" --el 2 --seconds 6 --sd
```

prepare-qemu.py는 sources.json의 다운로드 해시, musb-v8.2.0.patch를 확인하고 프로젝트 안의 QEMU를 빌드한다. 시스템 패키지를 설치하지 않는다. 최초 다운로드/설정이 필요하면 해당 스크립트의 --help 옵션을 따른다. 일반 빌드 명령은 전용 프로젝트 잠금과 builder 사용자로 실행한다.

## 대체 응답과 한계

전원·클록·PWRAP/PMIC·DTB는 명시된 대체 응답이다. CPU/MMU/GIC/타이머는 QEMU 구현이며 USB HID/SCSI와 SDState는 독립 원본 장치 모델이다. MSDC 어댑터는 PIO와 표준 응답을 구현하며 전기적 지연·CRC/튜닝·DMA·물리 접점은 재현하지 않는다.

외장 EFI는 GDB로 실제 CoreLoadImage(전체 장치 경로, SourceBuffer=NULL)와 CoreStartImage를 실행한다. BDS 메뉴에서 파일을 고르는 경로는 확인하지 않는다. Shell 진입을 OS 커널·ExitBootServices·WinPE/Linux 사용자 공간 성공으로 표시하지 않는다.

공유 트레이의 SIM은 별도 모뎀 EINT 경로다. 현재 QEMU에는 모뎀/SIM 모델이 없으므로 SD 결과로 SIM 분리·재삽입을 통과 처리하지 않는다. SD는 Media.ReadOnly=true를 표시하고 실제 쓰기 호출도 거부한다. QEMU 저장장치 자체는 쓰기 가능한 SD로 설정한 뒤 제품 UEFI의 거부와 전체 디스크 해시 보존을 확인한다.

현재 새 Image의 실행 로그와 장치 모델 근거는 analysis/Hardware-core8-20261116T022237Z 에 있다. 사용자에게 제출하는 [전체 체크리스트](/Users/you/.tmp/teclast-t70pro/docs/CHECKLIST.md), [검수 보고서](/Users/you/.tmp/teclast-t70pro/docs/HARDWARE-REVIEW.md), [설치 매뉴얼](/Users/you/.tmp/teclast-t70pro/docs/INSTALLATION-MANUAL.md)은 맥의 /Users/you/.tmp/teclast-t70pro/docs 에 둔다. 이전 8회 QEMU 결과는 당시 Image의 기록으로 analysis/QEMU-implementation-20261116T002244Z 에 보존한다.

## SIM 설정 소비자 실행

순정 CCCI의 실제 ARM64 함수만 Unicorn에서 실행한다. OF·문자열·로그 응답은 대체하며 모뎀 기동·SIM 이벤트·IMEI 조회는 실행하지 않는다. 동일 프로젝트 잠금 아래 builder에서 Tests/verify-sim-eint-binary.py --output <새 결과 파일>을 프로젝트 Python으로 실행한다. 원본 모듈과 유효 DTB의 해시가 고정되어 있다.
