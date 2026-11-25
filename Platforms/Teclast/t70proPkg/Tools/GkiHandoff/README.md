# T70 Pro G100: Android 보존·공통 UEFI 진입 준비

최종 OS는 아직 정하지 않았다. 제조사 LK → Android 기반 GKI의 초기 `/init` → 기본 Android 또는 EDK2 → 외장 ARM64 EFI 로더를 목표로 한다. 여기서 GKI는 공통 진입용 커널이며 최종 OS를 정하는 구성은 아니다. 현재 전체 경로는 미완성이며 `installable=false`다. 이 도구들은 기기를 설치하거나 재부팅하지 않는다.

## 고정한 입력

- 원본 커널은 `6.12.38-android16-5-g3c4da6410bcb-ab13872285-4k`다.
- [공개 GKI 릴리스](https://android.googlesource.com/kernel/common/+/refs/tags/android16-6.12-2025-08_r1)의 커밋 `3c4da6410bcbca1c8cb12e7e65d8b531f7873152`, KMI 세대 5를 사용한다.
- Clang r536225/12833971, Rust 1.82.0/12909517, bindgen 0.69.5와 저장소 식별값을 고정했다. 전체 원본 CI manifest를 복원한 것은 아니다.
- 원본 설정, 공개 서명 인증서, 내보낸 함수 8,973개, 출처별 원본 모듈 파일 429개를 사용한다. 개인 키·모뎀 보정·기기 고유 정보는 읽지 않는다.

소스와 실행 환경은 기존 프로젝트의 `gki-handoff/` 한 곳에서 관리한다. 호스트 의존성은 `host/` 안에 풀고 VM 시스템 패키지나 원본 대용량 백업을 변경하지 않는다. x86 Clang/Rust 실행 사본의 ELF 로더를 조정하며 내려받은 원본 도구는 보존한다.

## 빌드와 호환성 확인

`prepare.py --configure`는 원본 설정에 KEXEC를 켜고 원본 공개 서명자·보호 모듈 목록을 유지한 후보를 별도 출력에 만든다. MTE·KASAN·동적 SCS·CFI·Rust·MODVERSIONS를 보존한다. 설정이 이미 있으면 덮어쓰지 않는다.

`build.py --image`로 전체 Image·모듈·동일 출력의 검사 모듈을 빌드했다. 결과 릴리스는 `6.12.38-android16-5-4k-t70-kexec-prep-g3c4da6410bcb`이며 Android16/KMI5 표기를 유지한다. 호스트 C 보조 프로그램은 ARM64, Rust proc macro는 컴파일러와 같은 x86으로 연결한다.

Rust의 v0 이름은 [Kleaf 공식 설정](https://android.googlesource.com/kernel/build/+/9ab2f7613dbe496714ac1d049d169427116bec33/kleaf/impl/kernel_env.bzl)을 따라 `RUSTC_FORCE_RUSTC_VERSION=rustc-android`로 고정한다. 전용 `RUSTC` 래퍼 경로를 make 명령에 넣어 이전 이름의 객체가 재사용되지 않게 한다.

KEXEC가 추가한 헤더 때문에 `ksysfs`에서 세 구조체의 전방 선언이 전체 정의로 바뀌는 차이는 해당 파일에만 `ANDROID_KABI_DECLONLY`를 적용해 처리했다. `check-ksysfs-kabi.py`는 원본 설정 기준과 형식 정의 422개 전체가 일치하고, 실행 시 쓰이는 12개 영역의 바이트·재배치 연결 대상이 그대로임을 실제 컴파일로 확인한다. 이는 [커널의 선언 유지 기능](https://kernel.org/doc/html/v6.16/kbuild/gendwarfksyms.html)에 따른 처리다.

| 검사 | 결과 | 범위 |
| --- | --- | --- |
| 원본 커널·원본 모듈 자체 비교 | 통과 | 파일 429개, 함수 참조 CRC 24,471건 |
| 후보 전체 커널 빌드 | 통과 | Image·모듈·검사 모듈, 실제 모듈 로드는 미실행 |
| 후보와 원본 모듈의 정적 비교 | 차이 1건 | Rust binder가 참조하는 core의 `usize Display::fmt` CRC |
| ksysfs 선언 유지 | 통과 | 원본 형식 전체 일치, 실행 영역과 연결 대상 유지 |

남은 Rust 차이는 같은 주소를 공유하는 `u64`·`usize` 함수 별칭의 형식 선택에 따라 발생함을 재현했다. 동일 후보 객체에서 `usize`를 단독 계산하면 원본 CRC `0xd634776d`, 두 별칭을 함께 계산하면 후보 CRC `0x6d70fa16`가 나온다. 원본 빌드와 같은 전체 처리 결과를 아직 확보하지 못했으므로 후보 모듈 호환성은 통과로 판정하지 않는다. 경로 정규화와 원본 설정으로 별도 컴파일한 진단에서도 차이가 남았다.

원본 비교의 공급자 변형 31건과 서명·보호 KMI 정책, 실제 공급자 선택·드라이버 동작은 별도 확인 대상이다. 정적 CRC가 맞아도 실제 Android 동작을 입증하지 않는다.

## UEFI 진입 코드와 외장 매체

`build-handoff.py`는 기존 T70Pro raw UEFI Image를 `0x40000000`, 40바이트 ARM64 진입 코드를 `0x40401000`, 현재 부팅의 FDT를 `0x47c80000`에 놓는 세그먼트 계획을 만든다. 진입 코드는 x0를 FDT로 지정하고 x1~x3을 0으로 만든 뒤 UEFI로 분기한다. MMU·캐시·CPU 종료는 표준 ARM64 kexec 경로가 담당해야 한다.

`test_handoff.py`는 컴파일된 명령을 EL1·EL2에서 실행하는 4개 경우와 잘못된 이미지 7개 거부, 기존 출력 보존을 확인했다. 커널의 kexec 호출이나 UEFI 본문을 실행한 검사가 아니다. 실행 중 FDT와 전체 메모리 소유권 검사, syscall 로더, 초기 `/init` 연결은 미구현이다. 정적 stock DTB를 실행 중 FDT 대신 쓰지 않는다.

상위 `Tools/prepare-uefi-media.py`는 ARM64 EFI 셸을 공통 이동식 경로 `EFI/BOOT/BOOTAA64.EFI`에 둔다. `UefiExternalStage/esp.fat`는 1GiB FAT32 **파티션 파일시스템 이미지**이며 SD 전체 GPT 이미지가 아니다. 실제 파일 쓰기 후 다시 읽어 내용 일치를 확인했다. 최종 OS·ext4·NTFS를 포함하거나 실제 SD에 기록한 결과는 아니다.

## 다시 실행하기

기존 builder의 `/home/builder/teclast-t70pro`에서 다음 순서를 사용한다. 컴파일은 같은 빌드 잠금을 잡는다.

    gki_tools=Mu-Silicium/Platforms/Teclast/t70proPkg/Tools/GkiHandoff
    python3 "$gki_tools/prepare.py" --check
    Mu-Silicium/Resources/Scripts/with-build-lock T70Pro "Build GKI handoff candidate" -- \
      python3 "$gki_tools/build.py" --image
    python3 "$gki_tools/compare-abi.py" --candidate-out gki-handoff/out/kexec \
      --output analysis/slotless-boot/gki-preparation/candidate-abi.json

첫 준비 순서는 `prepare.py --fetch`, `prepare-host.py`, `prepare.py --extract-all-modules --configure`다. 기존 출력이 있으면 이를 삭제해 다시 준비하지 말고 상태와 로그를 확인한다. 진입 코드나 외장 매체 빌더의 출력도 비어 있어야 한다.

## 결과와 설치 조건

공통 상태는 `analysis/installation-preparation/status.json`, 빌드·ABI·선언 검사는 `analysis/slotless-boot/gki-preparation/`에 기록했다. `full-build-abi-fix-wrapper.log`가 현재 성공한 전체 빌드 로그다. 그 전 실패 결과와 진단 파일도 보존했다.

슬롯 a/b의 `boot`·`init_boot` 네 원본은 기존 복구 키트에서 필요한 블록의 해시를 확인하고 `analysis/installation-preparation/recovery-plan-{a,b}.json`에 계획만 저장했다. 전체 128GB 재검사나 실제 복원 성공을 뜻하지 않는다. 현재 슬롯·버전·바꿀 영역의 최신 원본을 확인하기 전에는 어느 계획도 실제 대상으로 선택하지 않는다.

초기 `/init`, 실행 중 FDT/메모리 검사와 kexec 로더, 부트 파티션·AVB 포장이 남아 있다. 원본의 `kvm-arm.mode=protected`가 활성인 상태에서는 표준 ARM64 kexec가 거부하므로, 부팅 시 모드 제어와 Android 서비스에 미치는 영향도 확인해야 한다. 실제 시험과 설치에는 프로젝트 AGENTS.md의 별도 승인과 순정 Android 복귀 조건이 적용된다. 현재 기기 접근·설치·새 OS 실행·복귀 검증은 미실행이다.
