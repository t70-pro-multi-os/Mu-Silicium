# T70 Pro XNU 초기 부팅 개발

OrbStack `builder`의 `/home/builder/teclast-t70pro`에서 개발한다.
순정 펌웨어의 CPU·GIC·PSCI·타이머 설명을 바탕으로 별도 XNU 포크와
EDK2 실행기를 구현했다. iOS용과 macOS용 전체 ARM64 커널을 각각 빌드했으며,
QEMU에서 실제 DarwinBoot로 실행해 초기 메모리 설정과 소스 corecrypto의
전체 자체 검사(POST), 암호화 공급자 등록, 난수 생성기 준비까지 확인했다.

기본 배치 커널 두 개는 신뢰 캐시 초기화에서 `image4 interface not available`로
멈춘다. 별도 iOS 시험 커널에 실제 Apple corecrypto·Image4·AMFI와 의존 모듈을
연결한 경우에는 원본 모듈 초기화, 플랫폼·CPU·GIC 초기화와 타이머 인터럽트를
확인했다. 저장장치 시험 커널에서는 실제 가상 디스크 읽기와 `IOFindBSDRoot`의
`disk0s1` 선택·성공 반환을 확인했고, 별도 HFS+ 시험에서는 루트 볼륨의 읽기 전용
마운트, 루트 디렉터리 설정과 PID 1의 `load_init_program` 진입을 확인했다.
빈 시험 볼륨에는 실행 파일이 없어 사용자 프로그램 실행·화면·터치와 실기기
저장장치 동작은 미확인이다. 태블릿에서 새 커널을 실행하거나 설치하지 않았다.

부팅 경로는 `EDK2 → DarwinBoot → T70PRO XNU`다. DarwinBoot가 iBoot와 XNU 사이에서
쓰이는 부팅 인자·Apple 기기 트리·메모리 설명을 제공한다. iBoot 바이너리 실행이나
Apple의 전체 보안 부팅 체계 재현은 현재 구현 범위가 아니다. Image4·AMFI 등록은
기록한 초기화 단계의 실행 근거이며 루트 볼륨·프로그램의 인증 성공을 뜻하지 않는다.

## MacBook Neo 목표

[targets/macbook-neo.toml](targets/macbook-neo.toml)은 macOS 제품 목표를
`MacBook Neo / Mac17,5 / J700AP`로 고정한다. [prepare-neo.py](prepare-neo.py)는 Apple
공개 카탈로그에서 확인한 macOS `27.2.1 (26B131)` 복원 이미지의 필요한 파일만 범위
요청으로 읽고, BuildManifest와 기기 트리·신뢰 캐시·커널의 SHA-256 및 Apple
Manifest의 SHA-384를 검사한다. 실제 기기 트리의 제품 이름·모델·보드 식별자와
커널 버전·명령 형식도 확인한다. 전체 IPSW 다운로드는 하지 않는다.

원본 Neo 커널은 `arm64e`, Darwin `27.2.0`, XNU `13432.41.4`다. 현재 실행하는 초기
포트는 Darwin `23.6.0`, XNU `10063.141.1`, 일반 `arm64`이며 macOS 컴파일 대상은
14.0이다. Neo 원본 모듈을 현재 커널에 연결하거나 macOS 27.2.1을 실행한 상태가
아니다. 해당 XNU 버전의 정확한 공개 태그 조회는 성공했으나 일치 태그는 없었다.
T70 Pro의 실제 CPU·GIC·RAM 설정은 MT6789를 유지한다.

```sh
python3 Platforms/Teclast/t70proPkg/Xnu/prepare-neo.py
```

검사 보고서는 `analysis/xnu-macos-neo-target.json`에 있다. macOS 배치 폴더에는
제품 목표 설정을 `os-target.toml`로 함께 넣으며, 커널 검사에서 현재 설정과 내용 및
해시가 일치하는지 확인한다. `target_os_runtime_qualified`는 `false`다.

## 소스 관리

EDK2는 기존 `wwwhana/Mu-Silicium` 포크에서 관리한다.
커널은 별도 [wwwhana/xnu-mtk](https://github.com/wwwhana/xnu-mtk)의 `t70pro`
브랜치이며 `xnu-sources/xnu-mtk` 한 벌을 두 OS 빌드가 공유한다.
기준은 `woaphone/xnu-mtk`의 `yunluo` 커밋
`53bc420e5acda41c1689f6690a85841393a347cf`다. 기존 ARM 포팅 변경이 포함된
기반이며 Apple 원본에서 새로 시작한 포크는 아니다. 구현 변경은 builder의
로컬 작업 트리에 있고 커밋·푸시는 미실행이다.

공통 소스는 [sources.json](sources.json)에 고정한다.
UniXNU `dff276535db3ac394f628cec58a718cd46c20ac2`, DarwinBoot
`5243c68ec332a227fe3680cd5da6d5a0e127035c`를 사용하며 이 체크아웃은 수정하지 않는다.
ArmCpu·ArmDTPlatform·libpthread·소스 corecrypto의 고정 버전과 커널 소스 해시는
각 프로필의 빌드 보고서에 기록한다. 버전이나 변경 상태가 다르면 자동으로
reset/checkout하지 않고 기존 내용을 보존한 채 중단한다.

## 순정 펌웨어와 메모리

[analyze-stock.py](analyze-stock.py)는 기존 `analysis/stock-fdt`의 base DTB·오버레이·
합성 DTB 해시를 검사하고 오버레이 재적용 결과를 비교한다. CPU·GIC·타이머·PSCI
항목만 [t70pro-hardware.dts](t70pro-hardware.dts)에 옮긴다.

| 항목 | 순정 펌웨어에 선언된 값 |
| --- | --- |
| CPU | Cortex-A55 6개, Cortex-A76 2개; MPIDR `0x000..0x700`, 간격 `0x100` |
| CPU 시작 | PSCI 1.0, SMC |
| GIC | GICv3, distributor `0x0c000000/0x40000`, redistributor `0x0c040000/0x200000` |
| 타이머 | 13 MHz, 가상 타이머 INTID 27 |

정적 DTB의 1 GiB 메모리 항목은 실제 설치 RAM이나 부팅 중 예약 영역의 근거로
사용하지 않는다. [prepare-adt.py](prepare-adt.py)는 하드웨어 설명과 새 난수 시드,
비어 있는 휘발성 CHRP NVRAM, `darwinboot-memory-size` 요청으로 실제 ADT를 만든다.
DarwinBoot가 현재 UEFI의 사용 가능한 메모리에서 연속된 256 MiB 전체를 확보한 뒤
그 범위만 커널에 넘기는 방식이다. 영역의 앞부분만 확보하거나 예약 영역을
커널이 처리한다고 가정하는 방식은 사용하지 않는다. 이 크기는 초기 실행용
할당 요청이며 태블릿의 RAM 용량을 뜻하지 않는다.

실기기용 `T70XnuBoot.efi`는 부팅 중 전달된 실제 FDT와 기존 SEC 메모리 파서를
검사한다. MPIDR·CPU·GIC·PSCI·CNTFRQ와 BGRA GOP를 확인하고, 프레임버퍼 전체가
SEC의 런타임 화면 예약 영역 안에 들어가는지도 확인한다. 정적 DTB만 넣어서 이
검사를 통과시킬 수 없다. 이 실기기 검사는 아직 태블릿에서 실행하지 않았다.

DarwinBoot 프로토콜 v2의 `DeviceTree`는 NULL로 요청한다. 실제 ADT는 같은 볼륨의
`xnu/kernel.macho.adt`에서 읽는다. 실행기는 자신이 실행된 이동식 매체만 사용하며
X로 커널 진입을 요청하고 Esc로 펌웨어에 돌아간다. 로더 실패·종료 이벤트 이후의
정리와 재호출 방지는 호스트 검사로 확인했다.

## 두 OS의 빌드

아래 명령은 기존 Mu-Silicium 디렉터리에서 실행한다. 빌드·생성·검사는 공통
`Resources/Scripts/with-build-lock`으로 직렬 실행한다.

```sh
python3 Platforms/Teclast/t70proPkg/Xnu/prepare-sources.py --check
bash Platforms/Teclast/t70proPkg/build.sh xnuboot
bash Platforms/Teclast/t70proPkg/build.sh xnuboot-macos
```

각 명령은 EFI·전체 커널·ADT를 만들고 로더/커널 검사를 실행한다. 두 OS는 다음과
같이 생성물과 설정을 분리한다. macOS 설정은 실제 OS 대상·MASTER·라이브러리·
Mach-O 배포 명령을 바꾸며, 파일 이름만 바꾸는 방식이 아니다.

| 프로필 | 컴파일 대상 | 생성 빌드 폴더 | 배치 폴더 |
| --- | --- | --- | --- |
| iOS | `arm64-apple-ios17.0` | `tools/xnu-build` | `Build/t70proPkg/RELEASE_CLANGPDB/XnuStage` |
| macOS | `arm64-apple-macos14.0` | `tools/xnu-build-macos` | `Build/t70proPkg/RELEASE_CLANGPDB/XnuStage-macos` |

각 배치 폴더에는 다음 파일이 들어간다.

```text
sources.json
EFI/Teclast/T70XnuBoot.efi
EFI/Teclast/DarwinBoot.efi
xnu/kernel.macho
xnu/kernel.macho.adt
```

커널과 연결 라이브러리는 `-march=armv8.2-a -mcpu=cortex-a55`를 사용한다.
Cortex-A55는 두 CPU 종류가 함께 실행할 공통 명령 기준이며 CPU 구성을 바꾸지 않는다.
macOS의 암묵적인 Apple CPU 선택은 `-march`만으로 남아 있었고, 실제 실행에서
XPACI와 STLURB 예외를 일으켰다. CPU를 명시해 두 문제를 해결했으며 최종 커널도
해당 PAC/RCpc-immediate 명령이 없는지 역어셈블해 검사한다.

현재 포트는 일반 `arm64` ABI와 4 KiB/38비트 변환표를 유지한다. 사용자 주소 상한은
commpage와 겹치지 않는 63 GiB이며, macOS의 zone 영역은 이 커널 주소 공간에 맞춰
24 GiB 가상 공간/8 GiB 물리 상한을 사용한다. 실제 물리 할당량은 확보한 RAM에 따라
더 작아진다. 새로운 macOS 사용자 공간의 고정 xzone 주소와 arm64e/PAC 요구는 별도
구현이 필요하다. 이 초기 설정으로 전체 macOS 앱 호환성이 확보된 것은 아니다.

빌드 래퍼는 고정한 공통 스크립트의 생성 사본에만 보드·OS·CPU 설정을 적용한다.
MASTER·CPU·생성 레시피·선택 설정이 바뀌면 해당 프로필의 생성 커널 오브젝트와
존재 여부만으로 재사용되는 두 라이브러리를 다시 만든다. 공통 소스와 다른 OS의
생성물은 보존한다. Linux 도구 모음은 프로젝트의 `tools/unixnu-linux`이며 LLVM
`58c46bae21181fb4240cf964461bea738d96d50e`와 ld64·MIG·IIG를 사용한다.

## 실제 실행 검사

QEMU 전용 EFI는 별도로 빌드한다.

```sh
bash Platforms/Teclast/t70proPkg/build.sh xnuqemu
python3 Platforms/Teclast/t70proPkg/Xnu/qemu-check.py --os ios
python3 Platforms/Teclast/t70proPkg/Xnu/qemu-check.py --os ios --negative-post
python3 Platforms/Teclast/t70proPkg/Xnu/qemu-check.py --os ios --checkpoint bsd
```

macOS는 `--os macos`로 같은 세 경우를 실행한다. 원본 모듈을 추가하지 않은 두 기본
커널의 BSD 검사는 Image4 부재로 실패가 기록되며 0이 아닌 종료값을 반환한다. VM 실행기는 `linux,dummy-virt` FDT만 허용한다.
실제 빌드한 커널과 DarwinBoot를 사용하고 VM의 CPU·GIC·타이머 주소를 담은 별도 ADT를
만든다. 이 ADT를 실기기용 ADT나 T70 Pro 하드웨어 검증 결과로 취급하지 않는다.
VM ADT는 실제 가상 실행 환경에 맞춰 `vmm-present=1`을 선언한다. T70PRO의 VMM
확인 함수는 이 기기 트리 값을 읽으며, 값이 없으면 0을 반환한다. 기존 기반 코드의
모든 ARM64를 VM으로 처리하던 부분은 T70PRO에서 제외했다.

| 검사 | iOS | macOS |
| --- | --- | --- |
| 전체 커널 링크·EFI·ADT 파서·해시·연결 심볼 | 통과 | 통과 |
| QEMU `kmem_crypto_init`, 전체 POST 상태 0, 공급자 등록, PRNG 준비 | 통과 | 통과 |
| 4 KiB MMU가 실제 활성화된 상태 | 확인 | 확인 |
| 검사값 한 바이트를 바꾼 VM 복사본의 POST 거부 | 통과 | 통과 |
| BSD 진입 | 실패: Image4 공급자 없음 | 실패: Image4 공급자 없음 |

훼손 검사는 VM 복사본만 바꾸고 원래 배치 커널의 해시가 그대로인지 재확인한다.
사용한 소스 corecrypto는 공개된 제3자 소스 스냅샷이며, 검사 통과는 Apple의 원본
배포물 인증이나 공급업체 인증을 뜻하지 않는다.

### 실제 Apple 모듈을 연결한 별도 시험

[prelink-ios.py](prelink-ios.py)는 고정한 iPadOS `17.7.11 (21H461)`, `iPad7,1 /
J120AP` 원본에서 실제 의존 모듈을 선택하고 포인터·주소·ABI·파서를 검사한다.
이는 현재 Darwin 23.6과 맞는 모듈 참조 대상이며 macOS 제품 목표는 Neo를 유지한다.
시험 커널은 `tools/xnu-prelink-ios`에 별도로 생성하고 기본 배치 커널은 보존한다.

```sh
python3 Platforms/Teclast/t70proPkg/Xnu/prelink-ios.py --kind image4
python3 Platforms/Teclast/t70proPkg/Xnu/prelink-ios.py --kind security
python3 Platforms/Teclast/t70proPkg/Xnu/qemu-check.py --prelink security
python3 Platforms/Teclast/t70proPkg/Xnu/qemu-check.py --prelink security --checkpoint bsd
python3 Platforms/Teclast/t70proPkg/Xnu/qemu-check.py --prelink security --checkpoint root-entry
```

| 시험 커널 | 실제 실행 결과 |
| --- | --- |
| Image4 및 의존 모듈 | 원본 corecrypto·PRNG·Image4 초기화 확인; 다음 AMFI 부재로 중단 |
| Image4·AMFI 및 의존 모듈 | 원본 corecrypto·PRNG·Image4·AMFI, 플랫폼 설정, BSD 함수 진입 확인 |
| 보안 모듈 커널의 루트 선택 진입 | CPU 등록·GIC 초기화, 실제 가상 타이머 인터럽트, `IOFindBSDRoot` 진입 확인 |

원본 corecrypto의 자체 검사 통과 로그를 실제 커널 메시지 버퍼에서 수집한다.
이미 등록된 원본 공급자는 유지하며 소스 corecrypto의 POST 상태는 5(미실행)로
기록한다. 앞의 훼손 거부 검사는 소스 공급자 검사이며 원본 모듈의 훼손 거부 시험과
구분한다. VM의 `/chosen/manifest-properties`, `crypto-hash-method=sha2-384`,
`amfi-only-platform-code=1`은 입력 구조와 제한적인 실행 정책을 지정한다.
인증 성공값이나 Apple CPU 정보를 꾸며 넣지 않는다.

T70PRO의 플랫폼 등록표에 빠진 `IONameMatch=device-tree`를 추가해 실제
`ArmDTPlatform` 선택과 NVRAM·CPU·주변장치 항목 게시를 확인했다. 게시 계수와
CPU/GIC 드라이버 시작 계수는 별개다. `bsd_init` 진입은 드라이버의 비동기 선택이
끝나기 전이었고, 루트 선택 함수에 도달했을 때 CPU/GIC 계수가 각각 1이 됐다.
루트 선택 함수 진입은 실제 루트 볼륨 발견·인증·연결 성공을 뜻하지 않는다.

보고서는 `analysis/xnu-ios-{image4,security}-prelink-build.json`과
`xnu-qemu-prelink-{image4,security}[-bsd|-root-entry]-execution.json`에 있다.
`root-entry`는 함수 진입만 확인하며, 아래 저장장치 시험의 `root`는 실제 반환까지
확인한다.

`verify.py`는 실제 실행기 31개 경우와 실제 libfdt를 연결한 플랫폼 검사 35개 경우를
GCC의 ASan·UBSan·경고 오류 처리로 확인한다. 고정한 DarwinBoot의 실제 Mach-O·ADT·
메모리·입력 버퍼·압축 ramdisk 검사와 무작위 메모리 표 2,200개도 통과했다.
[run-upstream-check.py](run-upstream-check.py)는 호스트 검사 보조 함수의 인자 형식만
실제 EDK2 BaseLib에 맞춘다. 상위 소스나 검사 기대값은 수정하지 않는다.

`verify-profile.py`는 실제 헤더·make·RAM 초기화·주소 제한·zone 상수·컴파일러 명령과
8개 MPIDR 분기를 확인한다. 수정 전 CPU 중복과 지원하지 않는 컴파일러 명령도 재현한다.
`verify-kernel.py`는 최종 커널의 소스/배치 해시·미해결 심볼·OS 명령·POST HMAC·
실제 파서를 검사하고, 커널·OS·실행 스크립트·두 EFI 해시가 현재 파일과 일치하는
QEMU 결과만 연결한다.

보고서는 프로젝트 `analysis`의 `xnu-kernel[-macos]-build.json`,
`xnu-loader[-macos]-verification.json`, `xnu-kernel[-macos]-verification.json`,
`xnu-qemu[-macos][-negative-post|-bsd]-execution.json`과
`xnu-kernel-build-readiness.json`에 있다. 정적 검사 통과와 BSD 실행 실패를 구분한다.

## 저장장치와 부팅 기반의 병렬 개발

가상 저장장치와 실제 T70 Pro UFS 코드를 나눠 개발하고 부팅 시험에 통합했다.
`Drivers/build_virtio_mmio.py`는 고정한 VirtioMmioBlock의 생성 사본에 읽기 계수와
데이터 해시 검사를 추가한다. 원본 IOStorageFamily의 190개 가상 함수 항목,
클래스 크기와 실제 VM 메타데이터를 먼저 대조한 뒤 전체 커널에 연결한다.

`build-kernel.py --virtio`는 `tools/xnu-build-virtio`와 별도 커널을 사용한다.
`prelink-ios.py --kind security-virtio`가 해당 커널에 실제 Image4·AMFI 및 의존
모듈을 연결한다. 기본 iOS·macOS 배치 커널과 실기기 ADT는 보존한다. VM의 실제
QEMU 장치 트리에서 활성 VirtIO 주소와 GIC 연결을 읽으며, Darwin의 인터럽트
부모 조회에 필요한 `AAPL,phandle`도 실제 GIC 식별자로 게시한다.

기본 iOS 빌드와 `security` 시험 커널을 준비한 뒤 Mu-Silicium에서 실행한다.
아래 생성·빌드·검사 스크립트도 공통 빌드 잠금을 사용한다.

```sh
python3 Platforms/Teclast/t70proPkg/Xnu/qemu-check.py --prelink security --checkpoint root-entry
python3 Platforms/Teclast/t70proPkg/Xnu/qualify-storage-abi.py
python3 Platforms/Teclast/t70proPkg/Xnu/build-kernel.py --virtio
python3 Platforms/Teclast/t70proPkg/Xnu/prelink-ios.py --kind security-virtio
python3 Platforms/Teclast/t70proPkg/Xnu/qemu-check.py --prelink security-virtio --checkpoint block
python3 Platforms/Teclast/t70proPkg/Xnu/qemu-check.py --prelink security-virtio --checkpoint root
```

실행기는 4 MiB 합성 GPT 디스크를 읽기 전용으로 연결한다. 파일시스템은 없다.
`block` 시험에서 첫 512바이트의 SHA-256과 실제 DMA 결과를 비교했다. `root`
시험에서는 7회, 19,456바이트를 읽고 UUID
`70D07E70-5EAB-4AE4-9D58-F96C785CDA72`의 `disk0s1`을 선택했다.
`IOFindBSDRoot` 반환값 0, 장치 번호 1:1과 실제 커널 로그가 일치한다.
읽기 오류와 쓰기 요청은 0이며 시험 전후 디스크 해시가 같다. 쓰기 거부 경로는
소스 검사로 확인했고, VM에서 쓰기 요청을 넣는 시험은 실행하지 않았다.
루트 파티션 선택은 확인했지만 볼륨 인증·파일시스템 연결·사용자 공간은 미실행이다.

`Drivers/T70Ufs`의 `T70UfsControllerProvider`와 `NativeT70UfsBlock`을 별도 전체
XNU 커널에 연결했다. 원본 IOStorageFamily의 190개 가상 함수 항목을 대조하고,
VirtIO와 같은 원본 저장장치 연결 심볼을 재사용한다. `--t70-ufs`는
`tools/xnu-build-t70-ufs`와 `tools/xnu-prelink-ios/t70-ufs-native`만 사용한다.

공급자는 `t70-ufs=1`과 실제 MT6789 기기 트리가 모두 확인돼야 시작한다. UEFI가
남긴 ready 표식·실제 런타임 FDT·UFS 주소를 검사하고, handoff/FDT 구간이 커널이
할당받은 RAM과 겹치면 거부한다. 펌웨어 소스의 예약된 handoff 페이지,
ACPI 메모리의 FDT 사본, GCD RAM 검사와 커널의 할당 영역만 쓰는 경로를 대조했다.
이후 비캐시 연속 DMA 메모리를 할당하고 실제 NOP·용량 조회 응답을 검사한다.
LU2 읽기만 허용하며 용량 제한값은 기존 EDK2 정책이다. 새 실기기 조회 결과는 없다.

요청 뒤 DMA 소유권이 불명확해지면 읽기를 중단한다. 정지 실패 시 메모리·매핑·
상태 참조를 격리해 보관하고 재사용하지 않는다. 정상 시작, 잘못된 전달 정보·용량,
시간 초과·정지 실패는 호스트 ASan·UBSan으로 검사했다. 실제 블록 읽기 메서드도
호스트에서 실행해 정상 4 KiB 읽기, 128 KiB 경계 분할, 다음 청크 실패 때 정확히
131,072바이트만 완료되는 동작을 확인했다. 쓰기·범위 초과·짧은 버퍼 거부와 DMA
오류 시 중단도 포함해 ASan·UBSan 검사 29개가 통과했다.
전체 순정 FDT와 10개 잘못된 입력, 메모리 경계 12개 경우도 통과했다.

전체 순정 `analysis/stock-fdt/effective.dtb`에는
`/soc/ufshci@11270000`, `mediatek,mt8183-ufshci`, 주소 `0x11270000/0x2300`,
GIC 인터럽트 `[0,137,4,0]`, 클럭 6개·리셋 3개·VEMC 전원이 있다.
`t70pro-hardware.dts`는 CPU·GIC·타이머·PSCI만 옮긴 파일이다. 현재 UFS 구현은
UEFI가 초기화한 컨트롤러 상태를 이어받는다. 전원·클럭·리셋의 독립 제어,
절전 복귀와 실제 저장장치 읽기는 추가 구현·확인 대상이다.

가상환경에서는 IOKit의 실제 공급자 검사가 실행됐고, UFS 시작과 물리 MMIO 접근은
0회였다. 이 거부 결과는 가상환경에서 T70 Pro 물리 주소를 건드리지 않는다는 근거다.
T70 Pro UFS에서 데이터를 읽은 결과로 해석하지 않는다.

HFS+ 시험은 별도의 빈 32 MiB 볼륨을 GPT에 넣고 원본
`com.apple.filesystems.hfs.kext`와 보안 모듈을 연결한다. 포맷·검사 도구는
프로젝트 안에만 추출했으며 파일시스템 표준 검사와 GPT/HFS 변조 입력 7개를 확인했다.
실제 가상 디스크의 UUID `915C4AD8-28EB-5825-A65E-631DAB379D8A`를 선택하고
`vfs_mountroot` 성공 반환, `hfs` 마운트 항목, 읽기 전용 상태와 `disk0s1` 장치 번호를
확인했다. 커널 로그의 UUID·IOMedia·BSD 장치 이름/번호도 결과와 일치한다.
15회, 41,984바이트를 읽었고 오류·쓰기 요청은 0이며 디스크 해시는 그대로다.
추가 `root-vnode` 검사는 `bsd_init`의 `/dev` 마운트 호출 진입에서 실제 루트
vnode의 HFS 마운트 연결, `VDIR`·`VROOT`·`MNT_ROOTFS`, 장치 vnode와 커널 프로세스의
현재 디렉터리를 확인한다. `init` 검사는 이어서 `load_init_program`에 진입한
PID 1의 함수 인자·프로세스 포인터·현재 디렉터리 상속·chroot 미설정을 확인한다.
구조체 위치와 필드 크기는 같은 커널 설정으로 컴파일해 측정한다. 잘못된 루트
상태 14개와 PID 1 상태 8개도 검사에서 거부된다.

현재 iOS 빌드는 포크에 이미 있던 정책에 따라 루트 볼륨 인증을 요청하지 않는다.
이번 변경은 인증 정책을 수정하지 않았다. `init` 검사는 첫 실행 파일 로딩 함수
진입에서 멈추며, 빈 볼륨에는 `/sbin/launchd`와 운영체제 파일이 없어 사용자 프로그램
실행은 미실행이다. `/dev` 관찰점은 루트 디렉터리 설정을 확인하기 위한 지점이다.

```sh
python3 Platforms/Teclast/t70proPkg/Xnu/prepare-hfs-root-fixture.py
python3 Platforms/Teclast/t70proPkg/Xnu/build-kernel.py --t70-ufs
python3 Platforms/Teclast/t70proPkg/Xnu/prelink-ios.py --kind security-t70-ufs
python3 Platforms/Teclast/t70proPkg/Xnu/prelink-ios.py --kind security-t70-ufs-hfs
python3 Platforms/Teclast/t70proPkg/Xnu/qemu-check.py --prelink security-t70-ufs --checkpoint block
python3 Platforms/Teclast/t70proPkg/Xnu/qemu-check.py --prelink security-t70-ufs --checkpoint root
python3 Platforms/Teclast/t70proPkg/Xnu/qemu-check.py --prelink security-t70-ufs-hfs --checkpoint root
python3 Platforms/Teclast/t70proPkg/Xnu/qemu-check.py --prelink security-t70-ufs-hfs --checkpoint mount
python3 Platforms/Teclast/t70proPkg/Xnu/qemu-check.py --prelink security-t70-ufs-hfs --checkpoint root-vnode
python3 Platforms/Teclast/t70proPkg/Xnu/qemu-check.py --prelink security-t70-ufs-hfs --checkpoint init
python3 Platforms/Teclast/t70proPkg/Xnu/verify-storage.py
```

실행 근거는 `analysis/xnu-qemu-prelink-security-t70-ufs[-hfs]-{block,root,mount,root-vnode,init}-execution.json`의
해당 시험에 있다. `analysis/xnu-t70-ufs-{host,device-tree,memory,block-method}-verification.json`은
각 코드 실행 결과와 현재 소스 해시를 기록한다. `analysis/xnu-storage-verification.json`은
전체 커널·IOKit 등록표·원본 연결 심볼·EFI·시험 디스크·실행 스크립트 해시와 실제
마운트 결과를 함께 확인한다. 기존 부팅 경로도 VM 11개 경우와 커널 검사 2개로
재확인했다. 커밋·푸시·실기기 부팅·설치는 미실행이다.

## 다음 구현 경계

기본 배치 커널과 실제 모듈을 연결한 시험 커널의 상태는 구분한다. 저장장치 시험
커널은 QEMU에서 CPU·GIC·타이머 초기화, 디스크 읽기와 루트 파티션 선택을 확인했다.
읽기 전용 HFS+ 마운트, 루트 디렉터리 설정과 PID 1 실행 파일 로딩 진입을 확인했다.
운영체제 파일이 있는 루트 볼륨, 실행 파일·시스템 호출 호환성, 볼륨 인증과
실제 T70 Pro UFS 동작은 남아 있다.
Neo 원본의 새 커널·모듈 ABI,
arm64e/PAC 및 더 넓은 사용자 주소 공간도 별도 구현 과제다. SMP 시작(`sheng-smp=1`),
실기기 인터럽트·저장장치·표시/입력은 각각 추가 구현과 확인이 필요하다.

기본 펌웨어 FV나 부팅 옵션은 이 빌드 대상으로 바꾸지 않는다. 실기기 시험과 설치는
상위 [AGENTS.md](../../../../../AGENTS.md)의 별도 승인·원복 조건을 따른다.
