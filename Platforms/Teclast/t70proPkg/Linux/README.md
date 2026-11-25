# Linux stock-module reuse

현재 보관된 부팅 파일과 새 콘솔 소스의 차이는 [화면 콘솔 작업 상태](#화면-콘솔-작업-상태)에 기록했다.

`prepare-stock-modules.py` checks the extracted stock `.ko` files against the original partition catalog, then compares their imported symbol CRCs with the exact stock kernel exports and the full 429-binary stock module export catalog. It validates extended and legacy module version sections, ARM64 ELF structure, module dependencies, export table sections/ordinals, source hashes and duplicate source variants, and verifies appended PKCS#7 signatures against the exact Image's built-in public certificates. The exact stock loading policy is checked statically; runtime module acceptance is untested.

Run the DT supplier scan below first, then audit all features under the shared build lock. Pass its payload_extra_roots as additional module roots:

```sh
# Run in bash from /home/builder/teclast-t70pro after sourcing env.sh.
mapfile -t supplier_roots < <("$PYTHON_COMMAND" -c 'import json; print(*json.load(open("analysis/linux-platform-modules.json"))["payload_extra_roots"],sep=chr(10))')
supplier_args=()
for supplier in "${supplier_roots[@]}"; do supplier_args+=(--extra-module "$supplier"); done
Mu-Silicium/Resources/Scripts/with-build-lock T70Pro 'audit stock Linux module reuse' -- \
  python3 Mu-Silicium/Platforms/Teclast/t70proPkg/Linux/prepare-stock-modules.py \
    --workspace /home/builder/teclast-t70pro \
    --manifest /home/builder/teclast-t70pro/analysis/stock-binaries/manifest.json \
    --stock-modules /home/builder/teclast-t70pro/analysis/stock-modules.json \
    --hardware /home/builder/teclast-t70pro/analysis/runtime-hardware.json \
    --stock-config /home/builder/teclast-t70pro/analysis/stock-kernel.config \
    --stock-kernel-exports /home/builder/teclast-t70pro/analysis/stock-kernel-exports.json \
    --kernel-info /home/builder/teclast-t70pro/analysis/kernel-info.json \
    --stock-kernel-image /home/builder/teclast-t70pro/Mu-Silicium/Build/t70proPkg/Linux/Image \
    --feature input --feature storage --feature cellular --feature display \
    --feature usb --feature power --feature connectivity --feature audio \
    --feature sensors --feature camera --feature sim_radio \
    --extra-module trusted_mem --extra-module device_apc_common "${supplier_args[@]}" \
    --package-output /home/builder/teclast-t70pro/Mu-Silicium/Build/t70proPkg/linux-stock-module-payload.cpio \
    --report /home/builder/teclast-t70pro/analysis/linux-stock-module-audit.json
```

The CPIO is a verified static module fragment, not a bootable initramfs. It contains the exact extracted stock module binaries and an inventory only. It does not contain `/init`, a Linux root filesystem, kernel, DTB, boot command line, module autoload list, firmware payloads, UFS setup, or cellular userspace. Its `firmware_references` are `.modinfo` declarations and do not prove unconditional runtime requirements; the two declared names were not found in the searched stock locations, so availability remains unknown. The archive write and full read-back validate file hashes; no module loading, device access, or boot test occurs.

The stock binary CRC audit is separate from candidate-kernel ABI readiness. The source stock kernel image and its 8,973 exports are hash-linked; all observed imported CRCs match the original stock kernel or an exact stock module export in the full catalog. The audit still records 32 `.modinfo depends` gaps and 12 imports with the two `rfkill` binary variants as possible providers. The latter have matching CRCs but different source hashes, so a raw archive catalog retains both variants. The exact-Image load planner separately selects the system_dlkm variant by unique full kernel-release equality and still checks every required import CRC and dependency. This does not establish Android load priority or candidate-kernel compatibility. The exact candidate kernel config, vermagic, and `Module.symvers` are not established; `candidate_kernel_abi_ready` remains false. A different stock vermagic release prefix alone is not treated as an incompatibility when module CRCs are present.

The stock config has `CONFIG_DEVTMPFS=n` and `CONFIG_ACPI=n`; Linux early userspace must create `/dev` explicitly, and ACPI discovery cannot be assumed. Static symbol compatibility does not demonstrate screen, input, storage, radio, or other hardware operation.

Run the offline parser and archive checks under the same build lock:

```sh
Mu-Silicium/Resources/Scripts/with-build-lock T70Pro 'check Linux stock module audit' -- \
  python3 Mu-Silicium/Platforms/Teclast/t70proPkg/Linux/Tests/test_stock_modules.py
```

## 모듈 라이선스·namespace·적재 정책

.modinfo의 반복 license/import_ns 태그를 보존한다. 모듈 export의 namespace는 __ksymtab의
세 번째 32비트 필드에 대한 실제 AArch64 RELA에서 해석한다. 잘못된 relocation 종류·범위·문자열과
누락된 namespace를 빈 namespace로 바꿔 통과시키지 않는다. 정확한 CRC 공급자를 고른 뒤
GPL-only export의 license와 MODULE_IMPORT_NS를 대조한다. 예약 module: namespace의
대상 이름/지원 문법과 MODULE_SIG_FORCE의 서명 없는 모듈 거부도 검사한다.

현재 220개 파일의 export 3,441개에는 메타데이터 오류가 없고 11개 분야의 GPL/namespace를
포함한 정적 적재 계획 오류는 0개다. license 태그가 호환·비호환으로 섞이면 정확한 vendor
loader 근거 없이 확정 거부로 표시하지 않고 정책 미확인으로 남긴다. 현재 파일의 모든 license
태그는 GPL 호환이다. 원본 파일이나 license 문자열은 변경하지 않는다.

정확한 순정 Image에서 보호 심볼 590개와 서명 없는 모듈용 가져오기 허용 심볼 9,176개를 추출하고,
실제로 이 표를 검사하는 명령과 분기를 확인했다. 원본 빌드 입력인 모듈 이름 목록과
abi_symbollist.raw 파일이 없어도 현재 Image의 내보내기/CRC 및 실행 시 사용하는 두 표를 검사할 수 있다.

서명 꼬리표 8개 중 7개는 같은 Image의 공개 인증서로 실제 PKCS#7 내용/서명을 검증했다.
검증은 RSA/SHA-256, 정확한 모듈 바이트와 커널에 내장된 공개 서명자만 사용하며 개인 키를 읽지 않는다.
외부 CA나 서명 파일 안의 인증서를 신뢰하지 않는다. 이전 서명 꼬리표가 남아 있는 파일도 커널이 검사하는
바깥쪽 서명과 그 내용 범위를 그대로 검증한다. 나머지 vendor_dlkm의 rfkill 변형은 이 인증서로 검증되지 않아
보존만 하며, 정확한 커널 릴리스와 일치하는 system_dlkm 파일만 적재 계획에 넣는다.

11개 기능 묶음 모두 실제 CPIO 바이트를 다시 검증해 load_policy_ready=true이고, 정책 미확정·내용/서명 변경 시
생성된 적재 스크립트가 insmod 전에 중단한다. 이는 정확한 순정 커널의 정적 정책 검사 결과다.
실제 커널 키링 초기화·모듈 적재와 장치 동작은 미실행이며, 다른 커널의 ABI 호환성을 뜻하지 않는다.
검증 옵션은 [OpenSSL CMS 공식 문서](https://docs.openssl.org/3.5/man1/openssl-cms/)에 따른다.
이 상태와 확정된 GPL/namespace 오류를 섞어 보고하지 않는다. 모든 계획의 런타임 적재는 미실행이다.
최신 묶음·펌웨어·정책·시작 경로 78개와 ELF/CPIO·서명 감사 15개가 builder 검사에서 통과했다.

## 순정 커널용 초기 사용자 공간과 외장 EFI 로더

`build-early-userspace.py`는 기존 모듈 CPIO를 그대로 참조하면서 `/init`과 정적 ARM64 BusyBox를 담은 작은 gzip CPIO를 별도로 만든다. 모듈 220개를 다시 넣은 큰 중복 묶음은 만들지 않는다. 순정 boot_a에서 검증한 EFI-stub `Image`는 현재 부팅 산출물 한 벌로만 둔다. 새 커널의 ABI가 맞는다는 주장은 하지 않는다.

`Init.c`는 PID1에서 `/proc`, 읽기 전용 `/sys`, 메모리의 `/dev`·`/run`·`/tmp`와 devpts를 구성한다. 순정 커널에 devtmpfs가 없어 console/null/zero/tty/random/urandom과 ptmx를 직접 만들고, 정상 실행 경로에서는 BusyBox mdev의 netlink 장치 감시와 자식 회수를 수행한다. 내부 디스크나 Android userdata는 마운트하지 않는다. PID1은 선택한 모듈 작업을 별도 프로세스로 시작하고 진단 셸을 함께 연다. 기본 진단 항목은 자동 적재를 끄며, 장치 시작 항목은 입력·화면·저장장치·USB 분야를 차례로 요청한다. 통신 연결을 요청하는 사용자 서비스는 포함하지 않는다. builder의 별도 PID·마운트 공간에서 실제 PID1과 파일 시스템·장치 노드·`/dev/null`을 검사했다. 검사 모드는 장치 탐색, mdev, 콘솔 열기와 셸 실행을 건너뛴다.

수동 `/sbin/teclast-modules <분야>`는 Image 배너에서 추출한 정확한 커널 릴리스와 전체 모듈 해시를 먼저 검사한다. ELF의 필수 import CRC와 원본의 선언된 의존 관계를 합쳐 적재 순서를 정한다. 약한 import는 선택 의존으로 취급한다. 옛 DEVAPC/DRM/GPU/TEE 이름 여섯 종류는 고유한 실제 심볼·CRC 연결이 있을 때만 대응시킨다. 이름만 비슷하다는 이유로 연결하지 않는다. 순환, 없는 공급자와 중복 변형은 해당 분야의 실행을 막는다. `rfkill` 두 변형은 원본 묶음에 모두 보존한다. 정확한 Image 릴리스
`6.12.38-android16-5-g3c4da6410bcb-ab13872285-4k`와 일치하는 변형이 하나뿐일 때만
적재 계획에서 선택한다. 현재 선택은 system_dlkm 파일 SHA-256
`fcd7fff4f61ec7de2164972283cafcd9d76d02d9a06542a5a401f6ec80820d76`이다.
고유 일치가 없거나 둘 이상이면 계속 차단하고, 선택된 변형의 필수 심볼·CRC·의존성 오류도 차단한다.
다른 변형으로 오류를 덮거나 경로 순서로 고르지 않는다. WLAN/Bluetooth 분야는 73종의
순서가 준비됐으며 실제 모듈 적재·키링 실행·펌웨어·통신 실행은 미실행이다. 셀룰러 분야는 68종의 순서가 준비됐지만 모뎀 구동·SIM·RIL·IP 연결은 검증하지 않았다.

`Applications/LinuxBoot`는 자기 자신이 실행된 **이동식** 볼륨만 열고 다음 세 파일의 크기와 SHA-256을 생성된 헤더와 대조한다. 내부 볼륨 검색, 파일 쓰기, NVRAM 부팅 항목이나 슬롯 변경은 하지 않는다.

| 이동식 볼륨 안의 경로 | builder의 현재 산출물 |
| --- | --- |
| `EFI/Teclast/Image` | `Build/t70proPkg/Linux/Image` |
| `EFI/Teclast/early-userspace.cpio.gz` | `Build/t70proPkg/Linux/early-userspace.cpio.gz` |
| `EFI/Teclast/linux-stock-module-payload.cpio` | 기존 `Build/t70proPkg/linux-stock-module-payload.cpio` |

BootStatusDxe가 게시한 런타임 DTB의 출처·전체 구조와 RAM/모뎀 예약 해석이 통과해야 로더가 진행한다. 정적 DTB를 대신 넣지 않는다. 두 CPIO 조각은 Linux initrd LoadFile2 방식으로 전달하며, 기본 `L` 항목의 커널 옵션은 `acpi=off rdinit=/init loglevel=7 firmware_class.path=/lib/firmware`이다. `D` 항목은 여기에 `teclast.modules=usb,input,display,storage`를 추가한다. 다른 장치의 적재 실패 전에 USB 키보드 경로를 준비하도록 USB를 먼저 요청한다. `F` 항목은 이어서 전원·오디오·센서·카메라·Wi-Fi/BT·셀룰러·SIM/무선의 순정 모듈까지 요청한다. 분야별 결과는 `/run/teclast-startup/results.tsv`에서 확인한다. 모듈 적재 오류나 시간 초과가 발생하면 시작 작업자가 종료하며, 앞서 기록한 분야별 결과는 남는다. 순정 Image의 실제 kernel_param 이름·권한·내장 소유자·setter/getter 포인터까지 확인한 뒤 이 검색 경로를 넘기며, PID1에서 읽기 전용 sysfs를 바꾸지 않는다. 앱의 `A` 항목은 재시작 요청이며 Android 직접 진입이 입증된 선택 항목이 아니다. 앱은 FV에 자동 실행 항목으로 넣지 않았다.

Linux 진입 직전에 런타임 DTB의 별도 메모리 복사본에 자석식 덮개 입력을 추가하고 EFI 구성 표에 게시한다. 원본 DTB와 RAM·모뎀 예약은 보존하며, 자식 이미지가 돌아오면 원래 구성 표를 복원한다. 덮개 모델·핀·극성이 순정 근거와 다르면 진입을 거부한다.

로더의 실제 C를 대체 UEFI 서비스로 실행해 두 부팅 항목을 포함한 20가지 경우의 검사·정리 경로를 확인했다. 구성 표 복원이나 initrd 인터페이스·이벤트 해제가 실패하면 참조 중인 코드와 메모리를 남기고 정지한다. ARM64 PE의 실제 파일 읽기·연결된 OpenSSL SHA-256은 길이 경계·다른 해시 10가지, 덮개 변환·연결된 libfdt는 순정 DTB 보존과 잘못된 입력 8가지로 실행했다. 이 검사는 커널이나 태블릿을 부팅하지 않는다.

```sh
# 프로젝트 env.sh를 읽은 뒤 기존 가상환경 Python으로 실행한다.
"$PYTHON_COMMAND" Mu-Silicium/Platforms/Teclast/t70proPkg/Linux/build-early-userspace.py
bash Mu-Silicium/Platforms/Teclast/t70proPkg/build.sh linuxboot
Mu-Silicium/Resources/Scripts/with-build-lock T70Pro 'check early archive and module order' -- \
  "$PYTHON_COMMAND" Mu-Silicium/Platforms/Teclast/t70proPkg/Tests/test-early-userspace.py
Mu-Silicium/Resources/Scripts/with-build-lock T70Pro 'check built Linux EFI reader and SHA-256' -- \
  "$PYTHON_COMMAND" Mu-Silicium/Platforms/Teclast/t70proPkg/verify-linux-boot.py
```

산출물 정보는 `analysis/linux-early-userspace.json`, PID1 검사는 `linux-early-userspace-tests.json`, 묶음·선택·DT 공급자·펌웨어·셸 실행 검사는 `linux-early-package-tests.json`, EFI 코드 검사는 `linux-boot-tests.json`에 기록한다. GPIO·전원·UFS/USB·디스플레이 초기화와 실제 드라이버 적재, SELinux/보안 펌웨어의 허용, 일반 Linux 사용자 공간과 셀룰러 제어는 미검증이다. stock `ttynull` 콘솔과 VT 부재 때문에 `/dev/console` 생성만으로 화면에 셸이 보이지 않는다. 그래픽 콘솔·Weston 데스크톱은 아래의 오프라인 빌드·포장 단계까지 진행됐고, 기기에서 동작하는 배포판과 Android를 보존하는 실제 듀얼 부팅은 아직 제공하지 않는다. 이 자료는 언락·시험 부팅·설치 승인을 의미하지 않는다.


## 순정 펌웨어 연결

prepare-stock-firmware.py는 원본 UFS 백업의 LP geometry/header/table 체크섬을 확인하고
vendor_a를 메모리의 익명 파일로만 읽는다. /firmware의 일반 파일 32개·3,975,858바이트를
기존 stock-binaries/vendor_a_firmware 한 곳에서 재사용하며 각 파일의 출처·길이·SHA-256을
firmware-manifest.json에 기록한다. 논리 파티션 이미지나 모듈 묶음의 사본은 만들지 않는다.
sensor 심볼릭 링크는 따라가지 않으며 보정값·사용자 데이터·모뎀 파티션을 읽지 않는다.

모든 펌웨어 파일의 해시·크기·경로를 다시 확인한 뒤 초기 CPIO의 lib/firmware에 포함한다.
빈 connfem.cfg는 허용하고 빈 실행 바이너리, 경로 이탈·중복·심볼릭 링크와 다른 원본 덮어쓰기는
거부한다. EFI의 firmware_class.path 설정은 일반적인 커널 펌웨어 요청의 검색 위치를
연결하는 것이며, 펌웨어 업데이트·실제 드라이버 요청·무선 통신을 실행한 결과는 아니다.
Android 서비스가 직접 여는 경로나 별도 파일은 구현 근거 없이 추가하지 않는다.

```sh
Mu-Silicium/Resources/Scripts/with-build-lock T70Pro 'prepare shipped firmware' -- \
  "$PYTHON_COMMAND" Mu-Silicium/Platforms/Teclast/t70proPkg/Linux/prepare-stock-firmware.py \
  --backup /mnt/mac/Users/you/.tmp/teclast-t70pro/device-backup
```

## 내장 공급자 확인

고정 클록과 fixed regulator는 정확한 Image의 driver 객체·probe·OF 표 포인터,
실제 AArch64 등록 인자/호출 대상과 PREL32 initcall을 교차 확인했다. 고정 클록의
초기 OF 선언도 확인했다. 이 두 가지는 모듈 파일이 없는 이유를 설명하는 정적 근거다.
실제 장치 등록·연결은 실행하지 않았으며, auxadc와 PMIC 하위 공급자를 같은 근거 없이
내장 드라이버로 분류하지 않는다. 출처는 stock-kernel-exports.json의
built_in_provider_evidence이며 변조된 포인터·등록 명령·initcall·끝나지 않는 OF 표는 거부한다.

## 부팅 시 분야별 모듈 시작

Init.c는 /proc/cmdline의 teclast.modules= 한 항목만 읽는다. 없으면 none이며, 중복·빈 값·길이 초과는 거부한다. 생성한 /sbin/teclast-start는 모든 분야 이름과 중복을 먼저 검사한 뒤 적재를 시작한다. none과 다른 분야를 섞을 수 없다.

작업자는 진단 셸과 다른 세션에서 실행되며 PID1이 한 번만 회수한다. 분야마다 30초 후 종료 신호, 2초 뒤 강제 종료를 요청한다. 적재 실패나 제한 시간 초과 시 다음 분야로 진행하거나 재시도하지 않는다. 장치 연결 미완료는 별도 종료 코드 2로 남기고 다음 분야를 검사한다. 모든 분야가 끝나면 제한 시간 10초의 mdev 장치 파일 검사를 수행한다. 커널 안에서 중단할 수 없는 드라이버 작업까지 이 제한 시간으로 끝낼 수 있다고 보장하지 않는다.

상태는 메모리의 /run/teclast-startup/state, 분야별 결과는 results.tsv, 세부 출력은 <분야>.log와 devices.log, 시작 프로그램 출력은 /run/teclast-startup.log에 남긴다. 기존 실행 기록을 덮어쓰지 않으며 각 파일은 512KiB로 제한한다. bindings-observed는 sysfs 연결을 확인했다는 뜻이며 기능 동작 완료가 아니다. 내부 파티션이나 userdata는 마운트하지 않는다.

실제 정적 Init 바이너리의 PID1·마운트 검사를 통과했다. 같은 Init의 시작 함수도 격리 환경에서 실행해 fork/exec, 독립 세션, 파일 크기 제한, 기존 로그 보존과 종료 중 새 작업 거부를 확인했다. 이때 실행 파일은 아무 장치 작업도 하지 않는 검사 대체물이다. 실제 모듈·mdev 실행이나 태블릿 부팅 결과로 계산하지 않는다.

## DT 공급자와 전체 바이너리 누락 점검

platform-modules.py는 vendor_boot 161개, vendor_dlkm 186개, system_dlkm 82개로
순정 모듈 429개 전체를 원본 해시와 대조한다. 논리 이미지와 소스 사본을 추가로 저장하지 않으며
기존 읽기 전용 DLKM 검사 경로를 재사용한다. 전체 1,095개 DT 노드 중 활성 compatible 노드
537개를 빠짐없이 목록에 남긴다. 모듈 OF 표와 연결되지 않은 244개에는 CPU·설정·하위 구성 요소도
섞여 있으므로 이를 누락 드라이버 244개라고 계산하지 않는다.

표 이름의 match/ids 문자열로 고르던 방식을 실제 ARM64 driver 객체의 이름·OF 포인터 재배치와
등록 함수·정확한 크기 검사로 바꿨다. 순정 RANDSTRUCT_NONE 설정과 device_driver/SPI/platform/I²C
배치를 확인한다. 이로써 단수형 device_id 표도 읽고, 드라이버에 연결되지 않은 내부 참고 표는
결합 근거에서 제외한다. 원본 IT6151 표의 끝 표시 누락 1건은 그대로 기록하며 장치 일치를 만들지 않는다.
지원 범위 밖 배치와 다른 버스의 동적 장치는 별도 근거가 필요하다.

클록·핀 상태·GPIO·IOMMU·전원 영역·PHY·버스 부모와 선언 의존성, 실제 필수 심볼·CRC 공급자를
변화가 없을 때까지 반복해 연결한다. 공급자의 공급자가 빠지는 문제를 수정했다.
SMI 디버그 장치의 두 vendor 공급 목록은 실제 소비자와 타깃 compatible이 맞을 때만
phandle 벡터로 처리한다. 인자 셀을 핸들로 오해하거나 일반 regulator 속성에 이 규칙을 적용하지 않는다.

분야별 시작점에 키패드·PMIC 전원 키·홀·진동, 사운드카드·Bluetooth 통화 오디오,
카메라 플래시, PD 관리자와 열 보호 모듈을 포함했다. 센서 분야의 imgsensor_isp6s는 제거했다.
PMIC regulator는 정확한 mt6366 부모와 platform:mt6358-regulator 별칭이 모두 확인될 때만
미리 적재한다. OF 결합 확인 대상으로 바꾸지 않으며 실제 MFD 하위 장치 연결은 남아 있다.
fhctl의 mcupm_v3는 get_mcupm_ipidev의 고유한 실제 공급자·CRC가 있을 때만 mcupm으로 연결한다.

현재 묶음은 219종·220개 바이너리다. 원본 30개를 새로 넣고 참고 표의 잘못된 연결로 들어간
1개를 제외해 기존 191개에서 220개가 됐다.
이름이 중복된 rfkill의 두 변형은 원본 묶음에 보존한다. 보관한 원본 추출본 221개 중 과거 참고 표로
선택됐던 mtk_emi는 현재 묶음에 불필요해 포함하지 않는다.
분야별 오프라인 적재 순서는 입력 20종·화면 78종·저장장치 24종·USB 91종·
셀룰러 68종·연결 73종·전원 109종·오디오 88종·센서 19종·카메라 92종·SIM/무선 68종이다.
이는 실제 장치 동작 수가 아니다.

```sh
# 원본은 rb로 읽는다. --extract는 필요한 원본 모듈만 기존 manifest에 추가한다.
Mu-Silicium/Resources/Scripts/with-build-lock T70Pro 'prepare DT supplier modules' -- \
  "$PYTHON_COMMAND" Mu-Silicium/Platforms/Teclast/t70proPkg/Linux/platform-modules.py --extract
# 위 명령으로 생성한 payload_extra_roots를 앞부분 supplier_args로 전달해 audit를 재생성한다.
# 기존 생성 CPIO를 바꿀 때는 기록된 해시와 먼저 대조하고 검증된 재생성 부산물만 교체한다.
```

`/sbin/teclast-modules <분야>`는 해시·커널 릴리스 확인, 공급자/필수 심볼 의존 순서의 insmod 뒤
`/sbin/teclast-devices <분야>`를 호출한다. 이 명령은 sysfs의 실제 `of_node`와 `driver` 링크를 읽는다.
모듈만 등록되고 장치가 없거나 probe가 미뤄진 경우, DT 노드가 없는 경우는 종료 코드 2를 반환한다.
모듈로 해결되지 않은 공급자·여러 일치 표·DT 검사 대상이 없는 분야도 성공으로 처리하지 않는다.
추후 `teclast-devices`만 다시 실행해 현재 상태를 읽을 수 있다. sysfs에 bind/unbind나 재탐색 명령을 쓰지 않는다.

현재 모든 분야의 정적 의존 순서는 준비됐지만 고정 클록·auxadc·PMIC regulator와 오디오 하위 전원은
실제 내장/다른 버스의 장치 연결 근거가 남아 있다. 모르는 노드를 내장 드라이버로 단정하거나,
드라이버 링크가 있다는 이유로 데이터 전송·충전·통신이 동작한다고 보고하지 않는다.
실제 태블릿에서 insmod와 이 상태 검사는 미실행이다.

실행 검사는 DT/ELF 파싱, 독립 fdtget 대조, 실제 임시 sysfs 링크, 생성한 BusyBox 셸의 커널/해시/insmod 실패와
순서·상태 반환을 포함한다. 검사 중 uname/해시/insmod는 셸 함수로 대체한다. BusyBox standalone applet은
PATH만으로 대체되지 않을 수 있어 실제 모듈 적재 함수에 도달하지 못하도록 했다.
결과는 `analysis/linux-platform-modules.json`과 `analysis/linux-early-package-tests.json`에 기록한다.

## Wi-Fi 서비스 연결과 Bluetooth·GNSS 상태 확인

초기 CPIO에 /sbin/teclast-connectivity를 넣습니다. probe는 가짜 ready 값을 만들지 않고 순정 WMT/WLAN 모듈, cfg80211 wiphy·무선 netdev, wpa_supplicant, control socket을 각각 표시합니다. Bluetooth는 HCI·D-Bus·bluetoothd 존재를 분리하고, GNSS는 class/장치 노드와 사용자 서비스 유무를 구분합니다. probe는 통신을 시작하지 않고 모듈·인터페이스·실행 파일·제어 소켓 상태를 분리해 보여 줍니다. 포함된 실행 파일의 버전, nl80211 지원, wpa_supplicant 제어 소켓 PONG, 격리된 D-Bus 소켓 시작과 종료를 builder에서 확인했습니다. 실제 무선 인터페이스는 연결하지 않았습니다.

초기 CPIO에 ARM64 wpa_supplicant, wpa_cli, BlueZ, D-Bus와 필요한 동적 라이브러리를 넣었습니다. Wi-Fi 설정에는 사용자가 제공한 별도 CONFIG를 받습니다. 명시적 wifi-run IFACE CONFIG는 순정 WLAN 모듈과 무선 인터페이스를 확인한 뒤 포함된 wpa_supplicant를 nl80211 전경 서비스로 실행합니다. wifi-status IFACE는 wpa_state만 출력합니다. 명시적 `wifi-dhcp-run IFACE`가 BusyBox udhcpc로 IPv4 주소·경로·DNS를 설정합니다. 이는 서비스 바이너리 포장과 호스트 가상 검증 결과입니다. 실제 WMT/RF 기동과 네트워크 연결은 검증하지 않았습니다.

`wifi-dhcp-run`은 연결 완료를 확인한 뒤 `udhcpc -f -R`을 실행하며 임대를 갱신할 때까지 계속 살아 있습니다. 주소·연속된 서브넷 마스크·게이트웨이·DNS와 RFC 3442 경로를 적용 전에 모두 검사합니다. 옵션 121이 있으면 기본 router 옵션보다 우선하며, 게이트웨이 0의 직접 경로도 처리합니다. 경로에는 DHCP 프로토콜 번호 16과 600부터의 metric을 붙입니다. MTU·IPv6·Wi-Fi 재연결 정책은 이 IPv4 서비스의 범위에 넣지 않았습니다.

주소·경로는 add 성공 뒤에만 소유 목록에 기록합니다. 같은 주소/경로의 갱신은 DNS만 바꾸고, 바뀐 임대와 deconfig/nak/leasefail·종료는 자기 목록의 항목만 제거합니다. 다른 주소/경로를 flush하거나 replace하지 않습니다. 삭제 실패 시 소유 목록과 상태를 보존하고 재실행을 거부합니다. 살아 있는 서비스 중복 실행과 남아 있는 실패 기록도 자동으로 덮어쓰지 않습니다.

초기 CPIO에는 udhcpc·rm·rmdir·chmod·cmp 앱렛과 `/etc/resolv.conf -> /run/teclast-connectivity/resolv.conf` 연결을 포함합니다. 인터페이스별 DNS를 원자적으로 합쳐 게시하고 한 인터페이스를 종료해도 다른 인터페이스 DNS는 보존합니다. 별도 Linux rootfs에서도 이 연결로 DNS 관리를 맡긴 경우에만 실행하며 임의의 기존 resolv.conf를 바꾸지 않습니다. D-Bus·bluetoothd·bluetoothctl도 포함하고 별도 명령으로 시작합니다. PID1은 무선·Bluetooth 서비스를 자동 시작하지 않습니다. GNSS 위치 서비스에는 gpsd와 수신기·위치 계산 경로가 아직 없습니다.

`build-early-userspace.py --repack-connectivity`는 기존 산출물·계획·펌웨어·컴파일 입력과 현재 셸 검사를 먼저 확인한 뒤 같은 CPIO의 헬퍼/앱렛/DNS 연결만 갱신합니다. init·그래픽·BusyBox 실행 파일과 나머지 모듈/펌웨어 내용은 보존하고 BootPayload.h의 크기·SHA-256을 다시 생성합니다. 이후 EFI 앱과 펌웨어를 빌드해야 새 이미지 해시가 반영됩니다.

PID1과 teclast-start는 이 헬퍼나 무선 서비스를 자동으로 시작하지 않습니다. 특히 순정 wmt_drv 의존성에는 CCCI/CCMNI와 throttling 모듈이 포함되어 있어 connectivity 기능의 자동 적재를 추가하지 않았습니다. 원본 gps_drv_stp는 문자 장치 등록과 펌웨어 요청을 사용하지만 표준 gnss_register_device 사용 근거는 찾지 못했습니다. 순정 이진 프레임은 처리하지만 본문의 위치 정보 변환은 남아 있어 gpsd에 직접 연결하지 않습니다. 순정 `GPS_read`/`GPS_write`는 WMT/STP 채널 2를 사용합니다. 문자 장치의 복사 제한은 16KiB지만 내부 STP 송신 본문은 최대 1,996바이트라 그보다 큰 명령을 보낼 수 있다는 뜻은 아닙니다. `GPS_open`은 WMT GPS 기능과 LNA·수신 콜백·깨움 잠금을 켜므로 단순 상태 확인을 위해 장치를 열지 않습니다. DT에는 L1/L5 이름의 LNA 상태(GPIO25/26, GPIO 출력 또는 DSP 기능 2)가 있지만 이 이름은 실제 L5 장착·수신을 입증하지 않습니다. 위치 계산 엔진, 명령별 본문 해석과 gpsd 또는 OS 위치 서비스 연결은 별도로 구현해야 합니다.

공통 `MtkStpCoreLib`는 mandatory STP 송수신, GPS 수신 버퍼·초기화 세대 구분, WMT 기능 제어·MCU opcode `0x32`·GPS 절전/복귀 요청과 실제 응답 길이/상태 검사를 구현합니다. Linux용 AArch64 ELF 정적 라이브러리는 `Windows/Build/libMtkStpCore.a`에 있습니다. 같은 코드를 메모리 오류 검사와 ARM64 PE 모의 실행에서 각각 5,799,629개 확인으로 검사했고, 순정 ARM64 코드와 16,199가지 조건을 대조했습니다. 기존 `verify-mediatek.py --tests MtkStpCoreTest`에도 연결했습니다. 이 라이브러리는 커널 모듈이나 위치 서비스가 아니며 초기 CPIO에 적재하지 않았습니다. BTIF OS 자원·인터럽트·DMA와 WMT/conninfra/펌웨어 기동·위치 계산 엔진·GPS 본문 프로토콜을 연결해야 합니다. 수신 바이트를 NMEA로 가정하거나 알 수 없는 GPS 명령을 자동 분할하지 않습니다. MCU 응답 수신을 임의 명령의 의미상 성공으로 해석하지 않습니다. GPIO25·26 LNA의 세 상태 전환과 원래 설정 복원 코드는 기존 GPIO 코어에서 오프라인 검사했으며 Linux 수신기/전원 서비스에는 연결하지 않았습니다. Linux에서는 정확한 순정 DT의 pinctrl 상태와 실제 RF 전원 소유권을 함께 연결해야 합니다.

순정 `/soc/consys@18002000`에는 WMT의 `gps_lna_state_init/oh/ol` pinctrl이 없습니다. GPS 노드의 `gps_l1_lna_*`·`gps_l5_lna_*`가 실제 GPIO25·26 상태입니다. WMT 공용 처리의 반환값을 GPS 핀 전환 근거로 사용하지 않습니다. 제공된 `WMT_SOC.cfg`와 GPS 기동 코드에는 공유 PALDO 제어가 있으므로, 수신기 서비스는 공유 전원·다른 무선 기능의 점유와 GPS 전용 pinctrl을 함께 연결해야 합니다. Windows용 WDF 점유 인터페이스를 구현했지만 Linux 수신기 연결 상태는 바뀌지 않았습니다.

`MtkVcn28Core`에 GPS/FM이 함께 쓰는 MT6358 VCN28 전원 순서를 추가했다. 순정 PALDO 선택 2·3이 같은 콜백으로 연결되는 것을 실제 ARM64 코드로 대조했고, ADIE가 `0x6635`이면 이 전원을 건드리지 않는다. 실행 중 칩 식별 결과를 받은 공용 RF 제어 주체만 사용한다. 정확한 DT의 2.8V 고정 전압·270µs 지연·`always-on`을 적용해 기본 전원을 유지한다. GPS/FM 사용권을 따로 관리하고 마지막 반납 때 기존 동작 설정을 복원한다. 쓰기와 전원 상태를 다시 읽어 확인하며 실패하면 원래 설정과 사용권을 보존한다. 두 OS의 ARM64 정적 라이브러리에 포함했다. 메모리 오류 검사와 ARM64 가상 실행에서 각각 20,157개 확인, 순정 ARM64 전원 함수와 49가지 대조를 통과했다. 실제 PMIC 공급자 연결, GPS 준비·펌웨어 기동과 기능 종료 확인을 묶는 전체 RF 제어는 남아 있다.

`MtkWmtRegisterBegin`에 GPS 준비와 펌웨어 기동에 필요한 WMT opcode `8` 레지스터 읽기·마스크 쓰기를 추가했다. 쓰기 응답 8바이트, 읽기 응답 16바이트의 종류·명령·선언 길이·상태를 검사하고, 읽기는 요청 주소가 일치해야 값을 공개한다. 시간 초과·불완전 전송 뒤에는 명령을 재전송하지 않는다. `MtkWmtGpsSyncBegin`은 순정 SoC 지원 비트가 확인된 경우에만 GPS 동기화 설정을 전송한다. 이 기기의 실행 중 지원 비트는 아직 확정하지 않았으므로 임의로 켜지 않는다. 순정 레지스터 제어 2,832가지와 GPS 동기화 514가지 조건을 대조했고, STP→BTIF 대기열·FIFO 송수신과 잘린 응답을 함께 검사했다. 동기화 명령의 성공 응답만으로 전체 GPS 기동이 끝났다고 처리하지 않는다.

공통 `MtkGnssCore`도 Linux AArch64 정적 라이브러리에 포함했습니다. 순정 `libmnl.so`의 기본 입력 프레임 `AA F0`/`AA 0F`, `DE DF`/`DE E0` 이스케이프, 하위 12비트 길이와 합계 검사값을 검증합니다. 상위 4비트와 명령·본문은 원래 값으로 보존하며 최대 본문은 502바이트입니다. STP 수신·대기열과 연결한 메모리 오류 검사 및 실제 ARM64 코드 가상 실행에서 각각 2,559,428개 확인, 순정 ARM64 함수와 901가지 대조를 통과했습니다. 전체 데몬·위치 계산 엔진은 실행하지 않았고 자동 서비스·장치 열기도 추가하지 않았습니다. `ReadOnlyErofs`로 원본 백업을 복사하거나 마운트하지 않고 GNSS 파일 일곱 개를 꺼냈으며, 같은 읽기로 기존 펌웨어 32개와 일곱 파일의 해시를 대조했습니다. 파일·소스 근거는 `analysis/stock-binaries/gnss-userspace.json`에 있습니다.

`MtkGnssOffloadInit/Receive/Reset`에 순정 보조 프로세서의 분할 데이터 조립을 추가했다. 기본 `AA F0` 입력과 별도인 포트 형식이며, 길이·전체 조각 수·1부터 시작하는 순번을 검사한다. 조각 본문 1,532바이트·완성 데이터 65,535바이트로 제한하고, 누락·중복·순서 오류·넘침은 내용을 비운 뒤 복구까지 차단한다. 복구 전 콜백은 세대 번호로 거른다. 순정 `nmea_recv → pack_data → 출력 함수 → 등록 콜백`의 실제 ARM64 코드와 가상 데이터 1,976가지 조건을 대조했고, 새 조립 검사를 포함한 GNSS 검사 2,559,428개를 메모리 오류 검사와 ARM64 가상 실행에서 각각 통과했다. 두 OS 라이브러리의 공용 함수는 동일한 38개다. NMEA 출력 포트의 전달 경로는 확인했으며, 실제 포트·동작 모드 선택과 수신 엔진·위치 서비스 연결은 구현 대상이다. 위치·시간·위성 기록과 RMC/GGA 출력 해석은 아래 공용 처리기에 연결했으며, OS 소비자 연결을 이어간다.

`MtkGnssDecodeLocation`·`MtkGnssDecodeSvStatus`는 순정 Android 구현이 받는 104바이트 위치 기록과 48바이트 위성 기록을 해석한다. 좌표·WGS84 고도·속도·방향·정확도·UTC/경과 시간, 위성 ID·위성군·신호 세기·방위·고각·주파수를 구분한다. 실제 입력 길이·개수·유효 비트·숫자 범위를 검사하고, 잘못된 기록이 하나라도 있으면 결과를 일부 쓰지 않는다. 순정 위치 변환과 위성 콜백 전체를 가상 서비스에서 934가지 조건으로 대조했다. `MtkGnssMcuPosition`은 종류 2·본문 3,168바이트인 별도 MCU 메시지를 검사하며 순정 복사·조회 코드와 194가지 조건으로 대조했다. MCU 본문을 104바이트 위치 기록으로 취급하지 않는다.

`MtkGnssNmeaParse`는 확인된 NMEA 출력의 RMC·GGA 문장을 체크섬·CRLF·256바이트 상한으로 검사한다. 날짜의 세기는 호출자가 명시하며 이전 문장의 날짜·고도를 빌려 쓰지 않는다. 수신 무효는 위치 비트 없이 전달하고, GGA는 해발 고도와 지오이드 높이가 모두 있을 때만 WGS84 고도를 만든다. HDOP를 미터 단위 정확도로 바꾸지 않는다. 윤초는 POSIX 자정 정규화와 별도 표시를 함께 전달한다. 순정 출력 콜백에서 해석기로 이어지는 메시지 6개, 별도 정밀 수치·달력 계산 1,024개를 확인했다. GNSS 전체 검사 2,559,428개를 메모리 오류 검사와 ARM64 PE 실행에서 각각 통과했고 공용 함수 38개를 두 OS 라이브러리에 포함했다.

`MtkGnssNmeaStreamInit/Receive/Reset`으로 조각난 출력 줄과 연속 문장을 처리한다. 한 호출은 최대 256바이트와 문장 하나까지 소비하며, 남은 입력 위치를 돌려준다. 정확한 나노초 시각이 같고 두 좌표의 표시 정밀도 범위가 겹칠 때만 RMC의 날짜·속도·방향과 GGA의 고도·위성 수·HDOP를 결합한다. 결합한 두 기록은 즉시 비워 재사용하지 않는다. 수신 무효·만료·재설정도 대기 기록을 비우며, 연결 세대가 지난 콜백은 새 상태를 바꾸지 못한다. 부분 문장의 시간 초과·잘못된 체크섬·길이 초과·수신 시계 역행은 복구 전까지 차단한다. 호출자가 정한 수신 시간 제한은 실제 위치의 절대적인 최신성을 증명하지 않으므로 OS 연결 단계에서 별도 정책이 필요하다.

문장 결합·입력 분할·오래된 콜백 등을 다룬 가상 조건 520개, 순정 출력과 공용 전송 결과를 각기 연결한 결합 결과 6개를 확인했다. 보고 구조는 208바이트, 스트림 상태는 760바이트이며, 공용 함수는 동일한 38개다. 전체 검사는 줄 경계·별칭 인수·윤초·날짜 순서·시간 초과도 포함한다. 실제 포트·동작 모드 선택, 수신 엔진·RF 기동과 Linux 위치 서비스 연결은 계속 구현할 부분이다. Windows는 별도의 `Windows/MtkGnss/Driver.c` UMDF2 위치 요청 어댑터와 ARM64 콜백 검사 2,373개를 추가했으며 하위 수신기·엔진 연결은 남아 있다. 이 Windows 코드가 Linux 수신기나 gpsd 연결을 제공하지는 않는다.

공통 라이브러리의 `MtkBtifCore`에 4KiB 송신 대기열·8/16바이트 FIFO 송신·한 번에 최대 256바이트 수신·128..160µs 깨우기 신호·원래 제어 상태 복원을 추가했습니다. STP/WMT/GPS 경로를 함께 실행하는 메모리 오류 검사와 실제 ARM64 PE 검사에서 각각 169,966개, 순정 BTIF ARM64 코드와 2,478가지 대조를 통과했습니다. 실제 MMIO 명령은 가상 레지스터에서 실행하며, 실제 칩·클록·RF 동작은 검사하지 않았습니다. 이 코드는 Linux 커널의 IRQ 등록/클록·DMA 공급자나 위치 서비스에 연결되지 않았으며 초기 CPIO에 자동 적재하지 않습니다.

Linux/Tests/test_connectivity_services.py의 37개 합성 검사는 외부 서비스를 실제로 실행하는 경로를 가짜 실행 파일로 확인합니다. 가짜 sysfs·UNIX socket과 실제 셸/가짜 DHCP·IP 실행 파일로 연결 상태 구분, nl80211 인자, 종료 코드, 출력 정보 제한, 주소·경로·DNS 쓰기·갱신·해제, 두 인터페이스의 소유권 분리, 중복 실행·일부 적용/정리 실패를 검사했습니다. 태블릿의 무선·SIM·GNSS 장치에는 접근하지 않았습니다. 원본 firmware manifest의 WMT/BT 및 soc2_2 Wi-Fi 변형과 실제 런타임 펌웨어 요청의 정확한 연결은 여전히 확인해야 합니다.


## DRM/KMS 출력 프로그램

`Graphics.c`는 `/sbin/teclast-graphics` 정적 ARM64 프로그램으로 초기 CPIO에 포함한다.
명시적인 `--probe /dev/dri/cardN`은 내부 패널·경로·모드를 읽고,
`--pattern /dev/dri/cardN`은 DRM master와 dumb-buffer 지원을 확인한 뒤
XRGB8888 화면 버퍼·FB2를 만들고 실제 KMS CRTC에 10초 동안 CPU 색상 패턴을 출력한다.
원래 CRTC를 복원한 뒤 버퍼·매핑·master를 해제하며, 복원 실패 시 사용 중인 버퍼를
해제하지 않고 신호를 통한 복원 재시도를 기다린다. 모드·버퍼 크기·pitch·장치 번호를 제한하고
복제 출력이나 여러 내부 패널처럼 대상을 고를 수 없는 상태는 거부한다.

자동 실행·원시 MMIO·GPU 명령은 추가하지 않는다. 순정 DRM 드라이버의 표준
`dumb_create/map_offset/mmap`과 KMS를 사용하며, Mali kbase ABI나 EGL/GLES,
Wayland/X11·그래픽 콘솔·일반 배포판 연결은 구현하지 않는다. ASan/UBSan의 실제 C
검사 1,390개는 가짜 ioctl·매핑으로 생성·출력·복원·실패 시 버퍼 보존을 확인했다.
`analysis/linux-graphics-tests.json`에 기록하며 실제 DRM 장치는 열지 않았다.

## GPU 공유 메모리

PanVK의 내보내기용 메모리 요청을 `/dev/dma_heap/system`의 공유 버퍼 할당과
Mali kbase 가져오기에 연결했다. 가져온 파일 번호는 드라이버가 별도로 보관하고,
내보낼 때마다 독립된 CLOEXEC 파일 번호를 만든다. 입력이나 이전 내보내기 번호를
닫아도 다른 사용자의 버퍼 수명이 유지된다. 할당·가져오기·내보내기 실패의 정리와
두 가지 가져오기 명령 형식도 실제 ARM64 코드의 가상 실행으로 확인했다.

공유 버퍼의 CPU 캐시 처리가 연결될 때까지 해당 메모리는 GPU 전용으로 제한한다.
Vulkan의 외부 이미지·버퍼 메모리 요구 사항에서도 CPU 접근 가능한 종류를 제외하고,
호환되지 않는 종류나 실제 버퍼보다 큰 할당 요청은 거부한다. 공유 할당이 실패했을 때
일반 GPU 전용 할당으로 바꿔 성공 처리하지 않는다.

검사 364개와 Vulkan 호출부 세 파일의 ARM64 Linux 컴파일을 통과했고,
현재 PanVK 전체 라이브러리를 객체 956개·라이브러리 62개로 다시 빌드·링크했다.
기존 DRM 직접 화면 출력 코드에 실제 Wayland 화면 생성·프레임 표시 코드를 추가했다.
컴포지터·데스크톱 묶음, X11과 실제 GPU·화면 실행은 남아 있다.

기존 Mesa 화면 전환 경로를 사용해 획득한 화면의 대기 신호와 그리기 완료 신호를
공유 버퍼에 연결한다. kbase의 신호 검사에서 오류·끊김·잘못된 파일 번호를 완료로
처리하던 결함을 고쳤고, 대기 실패 시 GPU 제출을 중단한다. 신호 파일은 모두
독립된 CLOEXEC 번호로 복사하며, 복사 실패 시 기존 대상 신호를 보존한다.
이미 완료된 Vulkan 신호(-1)는 디스플레이의 커널 명령이나 close에 넘기지 않는다.
외부에서 받은 화면 반환 신호는 단순히 제출된 상태가 아니라 실제 완료까지 기다린다.
같은 GPU 안의 작업은 기존 순서 보장으로 처리해 불필요한 CPU 대기를 추가하지 않는다.

실제 신호 처리·제출·완료 코드와 Mesa 화면 전환 코드를 함께 실행한 ARM64 가상 검사 124개,
실제 WSI·T70용 제출 코드의 ARM64 컴파일과 전체 ICD 재링크를 통과했다.
재사용된 작업 번호에 이전 제출의 완료·고장 알림이 도착해도 현재 버퍼를 해제하지 않도록
두 원본 식별 값을 모두 대조한다. 별도 완료 신호 작업의 회차도 보존하며, 중복·역순 알림과
정상 고장 전달을 실제 제출·완료 코드로 검사했다. 수정 전 코드는 새 검사에서 실패했다.
파일·read·poll·스레드·할당·커널 명령은 명시적으로 모의하며 실제 화면 전환 검사는 아니다.
Wayland 개발 패키지는 builder에 설치했다. VM 명령이 응답하지 않아 Linux에서 직접
실행하는 빌드는 대기 중이지만, 같은 소스와 실제 Linux 라이브러리를 쓰는 맥의 교차
빌드에서 Wayland 코드를 포함한 결과물을 만들었다.
GPU 작업의 실패도 Vulkan 오류 상태로 전달하도록 실제 JM 제출 경로를 수정했다.
첫·중간·마지막 묶음의 제출 거부 시 남은 작업을 보내지 않고, 이미 보낸 작업을
모두 기다린 뒤 메모리와 화면 갱신 잠금을 정리한다. 각 묶음과 제출 중인 스레드가
같은 완료 기록을 잡고 있으므로, 완료 통지가 역순으로 오거나 제출 함수가 반환하기
전에 도착해도 응용 프로그램에 완료를 알리거나 메모리를 먼저 해제하지 않는다.
실패한 작업의 이벤트는 반영하지 않으며, 동기 제출 실패와 고장 상태의 추가 제출도
차단한다. 실제 JM 코드의 ARM64 검사 40개로 이 경로들을 확인했다. GPU 제출·완료,
메모리·신호·캐시 경계는 모의하며 실제 GPU 동작을 검사한 것은 아니다.

빌더가 잠시 `running`으로 돌아와 일반 명령은 실행됐지만, Wayland 빌드 요청은
잠금 파일·로그를 만들지 못해 취소했다. builder 한 대의 정상 종료는 성공했고,
다시 시작하는 명령은 25초 뒤 시간 초과가 났다. 전역 OrbStack 재시작은 하지 않았다.
근거는 `analysis/panvk-kbase-abi-verification.json`, `analysis/panvk-build.json`이다.

## Wayland 화면 출력 연결

Android용으로 줄여진 PanVK 소스에서 빠져 있던 Mesa의 Wayland 보조 C 소스·헤더와
Vulkan Wayland 헤더를 원본 해시와 함께 복원했다. 공식 Wayland 1.24.0 생성 도구는
builder 작업 공간에서 맥용으로 한 번 빌드했다. Linux용 client 라이브러리와 프로토콜
XML은 builder에 있는 원본을 그대로 사용하며 소스 사본은 만들지 않았다.

화면 공유 버퍼(linux-dmabuf), 표시 시각, 프레임 간격·예약 시각, 화면 찢김 제어,
DRM 완료 신호, 색상 관리의 7종 프로토콜을 실제 Wayland 코드에 연결했다. 생성된
데이터를 별도 XML 해석 결과와 비교하는 ASan/UBSan 실행 검사 1,006개가 통과했다.
46개 인터페이스·228개 메시지의 이름·버전·호출 형식·객체 종류와 잘못된 XML 거부를
확인했다. 각 기능의 기기 지원 여부는 실행 중인 컴포지터와 커널 기능에 따라 달라진다.

최종 Linux ELF에서 실제 Wayland 화면 생성·초기화·보조 함수, 7종 프로토콜 데이터,
`libwayland-client.so.0` 연결과 실제 공개 함수 호출을 확인했다. 기존 DRM 출력 코드도
유지한다. 결과물은 `mesa/build-t70pro-panvk/src/panfrost/vulkan/libvulkan_panfrost.so`의
23,962,048바이트 한 벌이며, SHA-256은
`8861883092b41b57132dbbda25afe08ff9c361d1f9f2c5c82aa6fcfc03a98c3f`다.
원래 native Meson 구성과 이번 교차 빌드의 실제 Wayland 설정을 구분해 기록했다.

다음 작업은 컴포지터·창 관리·글꼴·한글 입력기를 담은 사용자 공간 연결이다.
Vulkan 프로그램·컴포지터 실행, 화면 전환과 실제 GPU는 아직 실행하지 않았다.
부팅 CPIO에는 이 라이브러리를 넣지 않았으며 이번 작업으로 태블릿 상태를 바꾸지 않았다.

## 자석식 덮개

순정 MH248의 GPIO0 원시 입력은 0이 열림, 1이 닫힘이다. 원본 `hall_sensor_p.ko`는 닫힘 `KEY_SLEEP`·열림 `KEY_WAKEUP`만 보내며, 이 동작은 원본 ARM64 코드 35가지로 확인했다.

Linux용 EFI 로더는 별도 런타임 DTB 복사본의 `/teclast-cover-lid/lid-switch`를 순정 커널에 내장된 `gpio-keys`에 연결한다(`CONFIG_KEYBOARD_GPIO=y`). GPIO0을 active-high `EV_SW/SW_LID`로 전달해 0=열림·1=닫힘을 유지한다. 안정화 시간은 10ms이며 깨우기는 열림 쪽 하강 에지로 지정한다. GPIO/EINT 모듈이 준비되어야 장치가 연결된다.

같은 GPIO의 중복 요청을 막기 위해 기존 hall 매개변수 노드는 이 복사본에서만 비활성화하고, `input`·`sensors` 적재 목록 모두에서 `hall_sensor_p`를 제외했다. 두 그룹의 상태 확인에는 실제 `/teclast-cover-lid` 드라이버 연결도 요구한다. 커널·실행 파일·펌웨어를 유지한 초기 CPIO에 바뀐 스크립트와 모듈 계획을 반영하고 해시를 EFI 앱에 다시 고정했다.

실제 C와 libfdt 보존 검사는 20,923개, 로더 수명 검사는 20가지, 빌드된 ARM64 덮개 변환은 8가지, 적재 제외·장치 연결 셸 검사는 2가지 통과했다. 초기 셸에는 덮개 정책 서비스가 없으므로 실제 화면 끄기·절전·복귀를 자동 처리하지 않는다. Weston 모드의 화면 끄기 정책은 아래에 구현했으며, 실제 절전 전원·깨우기는 별도로 검증해야 한다. 근거는 `analysis/linux-boot-tests.json`, `analysis/mediatek-driver-selected-tests.json`, `analysis/linux-early-package-tests.json`, 통합 결과는 `analysis/hall-cover-verification.json`이다.

## 화면 콘솔 작업 상태

`console.py`는 builder의 단일 `kmscon` 소스(v9.3.2,
9ce1dc0dc639e54e243a21d93e13f502a746f0a8)를 사용한다. DRM/KMS의 CPU 버퍼,
가상 터미널이 없는 커널의 fake VT, evdev/XKB 키보드, 실제 PTY 셸,
UTF-8·한글 글꼴을 연결하는 초기 환경을 구현 중이다. GPU 가속이나 데스크톱은
이 콘솔의 기능에 포함되지 않는다. 영문 키보드 배치이며 한글 입력기는 별도로 필요하다.

이 초기 환경은 mdev를 사용하므로 kmscon의 `early_userspace` 옵션에서 커널 이벤트를
직접 받는다. 올바른 번호의 문자 장치 파일이 준비된 뒤에만 장치를 등록하며,
mdev보다 먼저 도착한 이벤트는 1초 간격 검색으로 다시 확인한다. 원본의 udev 방식은
해당 빌드 옵션을 끄면 유지된다. 그래픽 가속·fbdev·logind는 이 구성에서 사용하지 않는다.

PID1은 `teclast.console=drm`이 지정됐을 때만 콘솔 프로세스를 시작한다.
기본값은 `none`이고 중복·알 수 없는 값은 거부한다. 모듈 시작 작업과 별개로
실행하며 프로세스 상태는 `/run/teclast-console.state`, 출력은
`/run/teclast-console.log`에 남긴다. `process-starting`은 프로세스 시작 기록으로,
화면이나 키보드 동작 성공을 뜻하지 않는다. 종료된 콘솔을 반복 실행하지 않으며
PID1 종료 요청은 콘솔 프로세스에 전달한다. 로그와 임시 파일은 기존 RAM 영역을 쓴다.
콘솔 셸은 `dmesg`와 장치 시작 로그를 확인할 명령을 안내한다.

EFI 진단 항목 `D`에 이 옵션을 연결했고, 새 ARM64 초기 실행 프로그램과 콘솔·
라이브러리·설정·이용 허락 문구 19개 파일을 기존 초기 CPIO에 넣었다. CPIO는
6,004,980바이트이며 기존 커널과 원본 드라이버 묶음의 내용은 보존했다.
`BootPayload.h`의 크기·해시를 갱신하고 외장 `T70LinuxBoot.efi`를 다시 빌드했다.
ARM64 EFI의 파일·SHA 검사 10가지와 실제 libfdt 덮개 변환 8가지,
LinuxBoot의 실제 C 동작 20가지를 확인했다. C 동작 검사는 Mac의 ASan/UBSan을 사용한다.

`build-early-userspace.py --repack-console`은 Mac에서 builder의 기존 헤더·
라이브러리를 직접 참조해 PID1을 다시 컴파일한다. 이미 만든 콘솔은 고정 커밋,
빌드 옵션, 소스·구성·실행 파일·라이브러리 해시가 모두 일치할 때만 사용한다.
기존 CPIO를 먼저 검증하고 새 묶음을 다시 읽어 모든 파일의 내용·권한을 대조한 뒤
이미지·헤더·보고서를 갱신한다. 변경된 근거 6가지의 거부와 실패 시 기존 파일 보존,
실제 CPIO 쓰기·재읽기, 콘솔·펌웨어·그래픽 실행 파일 대조를 포함한 포장 검사
7개 메서드를 통과했다. 교차 빌드 결과의 Linux 실행 검사는 별도로 대기 상태로 기록한다.

builder가 재시작 요청 뒤에도 `starting`에 머물러 Linux 실행 검사가 대기 중이다.
콘솔 본체, Unifont 모듈, 상류 검사 프로그램 5개와 프로젝트 검사 프로그램 2개는
ARM64 Linux ELF로 컴파일하고 링크했다. 필요한 실행 파일·라이브러리 9개에서
강한 함수·데이터 참조 536개의 이름이 모두 해결됨을 파일 분석으로 확인했다.
실제 동적 로더 실행이나 심볼 버전 검사를 대신하지는 않는다.

Mac에서 실제 글꼴 코드와 생성된 글꼴 데이터를 실행해 영문·한글 그리기 11개 검사를
ASan/UBSan으로 통과했다. 사용하지 않는 Linux 이벤트 형식은 전방 선언만 제공했으며
Linux 이벤트 루프·DRM·키보드·PTY는 이 실행에 포함되지 않는다. 세 C 파일의 ARM64
문법, Python·셸 문법, 실제 PID1 선택 함수의 ASan/UBSan 실행과 ARM64 ELF 형식·
인터프리터 검사 20개 경우도 확인했다. 반복 키·VT·PTY·장치 검색 프로그램은
링크했지만 Linux에서 실행하지 않았다. 기록은 `analysis/linux-console-build.json`이다.

교차 빌드의 중간 파일·생성기·Meson 캐시·전문 로그 81개(17,147,522바이트)는
검사 기록을 남긴 뒤 정리했으며, 최신 실행 파일·모듈 9개는 builder에 보존했다.
빌드 환경 복구 뒤에는 기존 `build-early-userspace.py`로 Linux용 Meson 구성을
다시 생성하고 XKB 반복 키·fake VT·PTY·커널 장치 검색 검사를 실행한다.
기본 Linux 빌드 경로는 이 소프트웨어 검사를 통과한 뒤 CPIO를 갱신한다.
교차 포장 경로의 소프트웨어 검사 기록은 `pending`으로 유지한다.
전체 `Tests/test-early-userspace.py`는 태블릿 장치가 없는 별도 루트에서 실제
ARM64 콘솔의 동적 라이브러리 로딩도 확인한다. 실제 화면·키보드·USB 전원·
드라이버 연결과 순정 복귀 검증, 사용자 승인 조건은 남아 있다.

## 자석식 덮개와 데스크톱 화면 전원

Weston의 libinput 입력에서 덮개 닫힘·열림을 받아 화면 전원을 제어하도록 연결했다.
덮개가 여러 개로 잡히면 하나라도 닫혀 있는 동안 화면을 켜지 않는다. 중복 이벤트와
장치 제거를 처리하며, 세션이 비활성일 때는 화면 장치에 쓰지 않는다. 세션 복귀 때
입력 상태를 먼저 갱신하고 화면을 켜도록 순서도 바꿨다.

덮개를 열었다는 이유로 잠금 화면을 풀거나 잠든 시스템을 깨우지 않는다.
화면이 꺼진 상태에서는 다음 실제 입력을 기다리며, 강제로 꺼 둔 출력의 상태도 보존한다.
이 구현 범위는 Weston의 화면 전원 처리다. 기기 전체 절전·충전·전원 레일과 물리적 깨우기는
별도로 남아 있다.

수정한 Weston을 ARM64로 다시 빌드했다. 기존 `lid-power-test.c`의 검사 본문과 제품
헤더를 사용해 설치된 `libweston-16`·DRM backend의 실제 반복 타이머 콜백과
`libwayland-server`의 ARM64 명령을 Unicorn에서 실행했다. 전원·덮개 90개, 음량 정책
30개, 일반 ALSA 제어 경계 48개, T70 출력 경로 194개, 반복 콜백 29개로 합계 391개 검사를 통과했다.
타이머 서비스 호출은 11회이며 ALSA 제어 I/O·타이머·로그 서비스는 대체한다.
Linux 운영체제·ALSA 라이브러리·실제 libinput/DRM 세션·실물 입력은 실행하지 않는다.
T70의 정확한 사운드카드 ID와 mux 메뉴·현재 경로·외장 앰프 상태를 대조해
Headset/Lineout/Handset 음량을 선택한다. 출력 경로를 바꾸지 않고 한 음량 제어만 쓰며,
쓰기 전 경로를 다시 읽는다. T70 카드가 존재하지만 경로가 모호하면 다른 카드의 Master로
넘기지 않고 일반 Wayland 입력을 유지한다. 외장 앰프 음량과 실제 PCM·오디오 서비스는 남아 있다.
T70 카드가 없을 때의 단일 Master Playback Volume 처리도 보존했다. ALSA 제어를 열 때 필요한
alsa.conf·cards/aliases.conf·ctl/default.conf도 원본 패키지에서 묶음에 넣었다.
기록은 `analysis/linux-desktop-lid-tests.json`이다. 재실행 명령은 기존 빌드 잠금 안에서
`Linux/desktop.py check-lid`를 쓰며, Weston의 전체 Meson 검사 실행을 뜻하지 않는다.

교차 빌드한 데스크톱을 당시 `early-userspace.cpio.gz`에 통합했다.
순정 커널과 원본 모듈 묶음은 보존했고, 실제 CPIO 내용·전체 ELF 의존성·입력/글꼴 데이터와
실패 시 이전 묶음 보존을 포함한 포장 검사 7개를 통과했다.
현재 파일 크기·해시는 `analysis/linux-early-userspace.json`이 기준이다.

## Linux 데스크톱 빌드

`desktop.py`로 Weston 16.0.2의 DRM/KMS·Vulkan·Pixman 화면 처리, libinput/libseat 입력,
데스크톱 창 관리, 터미널·화면 키보드와 Cairo/Pango 글자 표시를 ARM64 Linux용으로 빌드했다.
실행 프로그램·모듈 18개와 GBM의 동적 모듈을 포함한 실행 의존성 105개의 ELF·검색 경로·연결을
검사했다. Vulkan 셰이더 세 개는 실제 glslc로 컴파일했다. 잘못된 GLSL 거부와 패키지 경계·버전
처리도 실행해 확인했다. Linux에서 프로그램을 실행하거나 기기 화면을 띄운 결과는 아니다.

누락된 라이브러리와 Noto CJK 글꼴은 `tools/linux-desktop-deps` 한 곳에 두며, builder에 이미
설치된 43개 패키지는 원래 파일을 참조한다. 추가 패키지는 기존 Ubuntu 인덱스의 정확한 버전·길이·
SHA-256으로 검사했고 설치 스크립트는 실행하지 않았다. 소스는 `weston` 한 벌이다.
`Build/t70proPkg/LinuxDesktop/stage`에는 Linux 실행 경로를 반영한 최신 실행 파일만 보관한다.
전체 실행 라이브러리를 이곳에 복사하지 않으며, 파일별 원본 경로와 해시는
`analysis/linux-desktop-build.json`의 `runtime`에서 관리한다.

macOS에서 `desktop.py build`와 `qualify`로 ARM64 데스크톱 실행 파일과 의존성 검사를 만들고,
builder에서 `build-early-userspace.py`가 그 결과를 묶음에 연결한다. 패키지 파일과 검사된 Weston
시작 스크립트의 해시를 대조한 뒤 initramfs를 다시 만든다.

현재 초기 CPIO에는 데스크톱·PanVK·입력 규칙/hwdb·XKB·DMZ 커서·CJK 글꼴을 포함했다.
EFI 메뉴의 G는 Vulkan, P는 Pixman을 명시적으로 선택한다. PID1은 udev를 시작하고 실제 장치 노드
생성을 mdev와 연결하며, 시작 셸은 장치·Wayland 소켓·Weston 프로세스를 제한 시간 안에서 감시한다.
셸의 모의 의존성을 둔 프로세스·Unix 소켓 검사가 15개로 늘었다. SIGTERM을 받을 때 Weston과 자식 프로세스를 함께 종료하고 회수한다. 실제 CPIO 내용·의존성 검사 7개도 통과했다.
builder에서 `python3 Platforms/Teclast/t70proPkg/Linux/build-early-userspace.py`를 실행하면
검사된 Weston과 PanVK를 기본 CPIO에 포함한다. Weston 의존 파일 경로는 builder의 루트를 기준으로
기록한다. 맥에서 만든 검사 결과를 Linux의 기본 빌드에서 사용할 수 있다.
현재 117,102,306바이트 CPIO의 SHA-256은
`6cc1c5019679311aa5cffcedd6af2d312e5ab4c2e439ae5670d4c4654b304fb3`이다.
kmscon은 builder에서 다시 빌드했다. 콘솔 종료 시 포인터 타이머와 훅을 해제하며
키 입력, PTY, 모니터 검사 1,393개를 통과했다.
Weston 프로세스 그룹 정리는 구현했다. PTY 정책, 캐시·로그 정리, 한글 입력기는 남아 있다.

kbase 장치 번호를 DRM render 장치로 게시하던 코드는 실제 PanVK 소스에서 수정하고 ICD를 다시 빌드했다.
EFI 로더도 현재 이미지 해시로 빌드하고 실제 C 23가지·ARM64 읽기/해시/덮개 처리 18가지를 확인했다.
Linux의 실제 udev·Weston·GPU·화면·입력 실행 결과는 아니다. 실제 기기 검증에는 별도 승인이 적용된다.

현재 개발 우선순위는 EDK2의 화면 정보 전달·메모리 예약·부팅 매체·OS 진입 경로 완성이다.
Linux 데스크톱의 추가 확장은 이 기반 작업 다음에 진행한다.
