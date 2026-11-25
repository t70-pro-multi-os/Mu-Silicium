# T70 Pro: 일반 ARM64 리눅스 외부 모듈

순정 커널 + 순정 .ko 부팅은 진단용으로 남긴다. 배포판의 일반 ARM64
커널에는 순정 .ko를 설치하거나 강제로 로드하지 않는다. 소스에서 대상
커널의 headers/Module.symvers를 사용해서 모듈을 다시 만든다.
커널이 이미 지원하는 gpio-keys 등은 DKMS로 중복 포장하지 않는다.

## HX83132 0.1.2

- 순정에서 추출한 HX83132 보고서 형식과 칩 ID를 확인한 기존 공용 C
  코어를 재사용한다. 소스 아카이브는 명시된 Git 리비전의 코어를 담는다.
- 표준 Linux I²C, threaded IRQ, multitouch slot API만 사용한다. 일반
  커널과 별도 vendor ABI를 요구하지 않는다. 실제 센서 데이터의 유효
  프레임만 입력으로 전달하고, 연속 오류 시 눌린 슬롯을 해제한다.
- 보드 DT에 명시적으로 선언한 teclast,t70pro-hx83132-retained 장치만
  매칭한다. HANA 계열 ACPI ID나 범용 himax,hxcommon은 선점하지 않는다.
  향후 일반 커널의 T70 DT는 I²C 0x48과 실제 GPIO9 IRQ, 올바르게 등록된
  MT6789 I²C/pinctrl/IRQ 공급자 및 전원 상태를 갖춰야 한다. 순정 DT와
  차이가 있으므로 DKMS 빌드만으로 장치가 열거된다는 뜻은 아니다.
- 준비된 컨트롤러에 한함. GPIO152를 토글하거나 컨트롤러 전원을 켜지
  않고, 칩 ID가 일치하지 않으면 등록하지 않는다. 펌웨어 쓰기·복구,
  펜, 전원 손실 뒤 콜드 초기화와 ACPI 연결은 지원하지 않는다.
  S2idle 등에서 복귀할 때는 칩 ID 확인에 성공하면 IRQ를 다시 켠다.

Ubuntu 7.0.0-43-generic ARM64 공식 헤더와 DKMS 3.2.2에서 별도
디렉터리에 add/build를 실행했다. modpost 성공, AArch64, vermagic
7.0.0-43-generic 및 symbol versions 확인. DKMS 상태는 built다.
별도로 기존 6.12.38 ARM64 GKI Kbuild에서도 modpost와 BTF가 통과했다.
이 둘은 각각 빌드 검증이며 모듈 설치·로드나 장비 입력 결과가 아니다.

## 소스 아카이브 생성 및 빌드

    python3 Platforms/Teclast/t70proPkg/Linux/Dkms/package-source.py /path/t70-hx83132-0.1.2.tar.gz

아카이브는 최상위 t70-hx83132-0.1.2 디렉터리에 소스만 담는다.
DKMS와 대상 커널 헤더가 설치된 일반 ARM64 Linux에서, 준비한
디렉터리 안으로 풀고 대상 커널 릴리스를 지정한 뒤 dkms add/build로
컴파일할 수 있다. DKMS를 쓰지 않는 빌드 확인은 풀어낸 디렉터리를
M으로 지정해 make -C /lib/modules/<release>/build M=<absolute-source-path> modules.
배포판의 모듈 서명 정책은 그대로 적용한다. 시스템 설치·자동 로드를
수행하는 스크립트는 포함하지 않는다.

## 다음 장치

덮개는 gpio-keys/SW_LID와 DT 자원, 스피커는 일반 ASoC의 앰프와
MT6789 머신 연결을 우선 확인한다. GPU/디스플레이와 Wi-Fi/BT/LTE는
커널 공통 드라이버와 보드별 공급자·펌웨어를 분리한다. UFS/SD처럼
루트 파일시스템보다 먼저 필요한 드라이버는 DKMS로 빌드하더라도
initramfs 포함이나 커널 내장 설정이 별도다. 그 외 장치를 이 패키지가
지원한다고 보지 않는다.

## 보드 DT: 터치 선언과 공급자 분리

Himax/t70pro-hx83132-retained.dtso는 검증용 오프라인 선언이다.
순정 구조의 I²C 자식 compatible만 바꾼다. 부모 버스·GPIO·IRQ·클록을
추가하거나 활성화하지 않는다. 독립적으로 검증된 공급자가 없으면
이 선언만 적용해도 장치는 동작하지 않는다. 원본 DT에는 적용하지 않는다.

    python3 Platforms/Teclast/t70proPkg/Linux/Dkms/verify-touch-dt.py \
        ../analysis/stock-fdt/effective.dtb

- 순정 부모는 mediatek,mt6983-i2c (MMIO 0x11e00000, DMA MMIO
  0x10217080) 및 main/dma 클록을 참조한다. main은 MT6789 I²C
  wrapper(0x11e02000), dma는 MT6789 infracfg_ao(0x10001000)다.
  vendor 소스의 mt6983 프로필은 v2 레지스터, APDMA 동기화 및
  DMA v1을 명시한다. 단순히 기존 다른 SoC compatible로 치환하지 않는다.
- GPIO9 하강 IRQ의 공급자는 mediatek,mt6789-pinctrl이다. Ubuntu
  7.0.0-43-generic에는 MediaTek Paris 공통 프레임워크와 export된
  probe 심볼이 있지만 MT6789 전용 핀 테이블 드라이버는 없다.
  vendored MT6789 테이블과 일반 커널 프레임워크 ABI를 맞춰야 한다.
- I²C 프레임워크가 CONFIG_I2C_MT65XX=m이라고 해서 vendor의
  compatible이 바인딩되는 것은 아니다.
  참조: https://github.com/torvalds/linux/blob/v7.0/drivers/i2c/busses/i2c-mt65xx.c

## 터치 부모 버스: MT6983 호환 I²C 공급자

package-i2c-source.py는 기존 kernel-reference Git의 고정 리비전에서
MediaTek GPL I²C 소스를 읽어 DKMS 소스 디렉터리로 만든다.
소스 SHA가 다르면 거부한다. MT6983 v2/APDMA 설정과 전송 경로는
유지한다. Linux 7.0의 플랫폼 remove 콜백에 맞췄고, 기존 배포판
i2c-mt65xx와 이름 및 매칭 ID가 겹치지 않는다.

    python3 Platforms/Teclast/t70proPkg/Linux/Dkms/package-i2c-source.py \
        /path/t70-mt6983-i2c-0.1.1

실제 Ubuntu 7.0.0-43-generic ARM64 DKMS add/build와 modpost 통과.
현 단계는 소스 빌드, 단일 OF alias 및 커널 ABI 검증이다. 순정 DT의
mediatek,mt6983-i2c는 자동으로 선점하지 않는다. 별도 검증 뒤 T70
터치 부모 노드 하나에만 teclast,t70pro-mt6983-i2c를 지정한다.
main/dma 클록과 pinctrl/EINT 공급자가 아직 필요하며 다른 컨트롤러를
일괄 전환하지 않는다.

부모와 자식을 함께 검증하려면 verify-touch-dt.py에 --include-parent를
사용한다. 두 compatible 이외의 전체 노드·속성이 동일해야 통과한다.
순정 pinctrl의 mediatek,eint는 별도 /soc/apirq@1000b000 노드를
참조한다. 4 KiB MMIO, AP GPIO EINT 160개, GIC SPI 244를 확인했다.
Linux v7.0의 mtk_build_eint는 반대로 pinctrl 노드의 뒤쪽 reg-names
리소스와 첫 IRQ를 읽고 SoC EINT 설명을 요구한다.
https://github.com/torvalds/linux/blob/v7.0/drivers/pinctrl/mediatek/pinctrl-mtk-common-v2.c
따라서 vendor pinctrl 파일의 compatible만 맞춰도 EINT는 등록되지
않는다. 두 바인딩을 함께 쓸 때는 이 변환과 공급자 소스를 별도로
검증해야 하며 현재 overlay는 IRQ 소유권이나 pinctrl 자원을 바꾸지 않는다.

이미 검증한 입력과 동일한 소스를 파일 하나로 전달하려면 같은 명령에
--archive를 추가하고 출력 경로를 .tar.gz로 지정한다. 아카이브는
GPL 라이선스와 소스만 포함하며 기존 .ko나 DKMS 상태를 담지 않는다.

0.1.1은 유효한 I²C 속도 조합을 찾지 못하면 등록을 중단한다.
0 또는 범위를 벗어난 부모 클록, 0인 분주값·요청 속도를 거부한다.
기존 소스의 잘못된 성공 반환을 하드웨어 없는 시험에서 재현한 뒤,
호스트 sanitizer 시험과 Ubuntu generic ARM64 DKMS 재빌드를 통과했다.

## 일반 Linux 진단: 모듈 등록과 GPIO IRQ 구분

probe-touch-linux.py는 부팅한 Linux의 sysfs만 읽는다. 대상 OF 노드
하나에 T70 I²C와 HX83132 드라이버가 붙었는지, 그리고 그 I²C
클라이언트 이름으로 요청된 IRQ가 mt-eint hwirq 9/edge인지 각각
보고한다. irq sysfs가 없거나 후보가 둘 이상이면 성공으로 판정하지
않는다. IRQ 등록만으로 에지 극성·카운터 증가·입력 동작을 증명하지
않으며 /dev/input은 열지 않는다.

호스트 fixture 회귀: python3 test-probe-touch-linux.py (이 디렉터리).
ABI 구현: https://github.com/torvalds/linux/blob/v7.0/kernel/irq/irqdesc.c

## 오디오: 표준 AW88261 드라이버 재사용

Ubuntu 7.0.0-43-generic ARM64 설정은 CONFIG_SND_SOC_AW88261=m이다.
앰프 두 개를 DKMS로 다시 구현하기 전에 verify-stock-aw88261.py로
순정 0x34/0x35의 채널 0/1과 정확한 ACF 파일을 검사한다.
헤더 version 1은 [Linux v7.0 ACF 파서]의 64바이트 legacy 포맷으로,
두 채널 모두 기본 레지스터 프로필 0/10을 선택한다. 실제 ACF의
섹션 12개 CRC8을 확인했다. 파일명(aw88261_acf.bin) 및
awinic,aw88261 / awinic,audio-channel 값은 [Linux v7.0 코덱]의
계약이고, 다른 보드의 펌웨어나 핀 설정을 재사용하지 않는다.
아직 T70 앰프 I²C1(0x11e01000), MT6789 AFE/MT6366 머신 라우팅과
클록/전원 공급자 연결이 필요하다. 설정 파일을 수정·설치하지 않는다.

[Linux v7.0 ACF 파서]: https://github.com/torvalds/linux/blob/v7.0/sound/soc/codecs/aw88395/aw88395_lib.c
[Linux v7.0 코덱]: https://github.com/torvalds/linux/blob/v7.0/sound/soc/codecs/aw88261.c

Audio/t70pro-aw88261-staging.dtso는 I²C1의 표준 코덱 속성만 준비한
비활성 템플릿이다. verify-stock-aw88261.py --check-staging은 두 앰프
노드만 변경되고 둘 다 disabled인지 확인한다. 순정 MT6366 머신의
스피커 I²S 출력3/입력0, AFE 0x11210000(8 KiB)/SPI234, I²C1
0x11e01000/SPI145를 확인하며, 컨트롤러·전원이나 사운드 카드는
활성화하지 않는다.

호스트에서 순정 사운드 카드, AFE, MT6366 코덱, AFE 공통 모듈의
강한 import를 Ubuntu 7.0.0-43-generic Module.symvers와 비교했다.
이름이 일치해도 Android 바이너리를 쓸 수 있다고 판단하지 않는다.
특히 AFE 공통 모듈은 Android ADSP/IPI, 예약 메모리, 초음파 보조
경로에 직접 의존한다. 첫 소스 포팅은 순정 음성·초음파·DSP 기능을
무조건 당겨 오는 대신, 일반 ALSA 스피커 PCM에 필요한 AFE memif,
I²S3 및 코덱 연결만 명확히 나누는 것이 우선이다. 아직 구현되지
않은 DSP를 성공으로 반환하는 스텁으로 대체하지 않는다.

snd_soc_set_runtime_hwparams가 Ubuntu 심볼 목록에 없다는 것은
예외다. 대상 include/sound/soc.h에 inline으로 존재하므로 해당
소스 호출을 제거하거나 사설 ABI를 만들 필요가 없다.
Ubuntu MT6358 코덱 설정만으로 T70 MT6366의 vendor accdet나 머신
콜백이 연결된다고 판단하지 않는다.

HX83132 0.1.1은 접점이 활성인 상태에서 잘못된 보고서를 받은
뒤 새 IRQ가 없더라도 120 ms 뒤 접점을 해제한다. 정상 보고서가
먼저 오면 해제를 취소하고, 오류가 연속 3회이면 즉시 해제한다.
정상적으로 누르고 있기만 하는 접점에는 타이머를 걸지 않는다.
호스트에서 상태 전이 시험 및 Ubuntu ARM64 제너릭 DKMS 빌드 통과.

0.1.2는 재개 시 칩 ID 확인 후 IRQ가 아직 꺼진 상태에서 보고서를
한 번 읽는다. 접점 에지가 마스크 중 이미 발생했을 때 유효한 기존
보고서를 복구하고, 빈/오류 스냅샷은 기존 오류 타이머에 포함하지
않는다. IRQ 스레드와 보고서 읽기는 같은 뮤텍스로 직렬화된다.

순정 /sound/mediatek,speaker-codec의 sound-dai도 실제 0x34/0x35
phandle로 해석된다. 별도 ADDA 링크의 MT6366 아날로그 코덱은 Linux
v7.0의 MT6358 드라이버가 이미 해당 compatible과 DAI
mt6358-snd-codec-aif1을 제공한다. 순정 reg-vaud28-supply는
2.8 V VAUD28을 가리키고 일반 드라이버는 정확한 대소문자로
Avdd-supply를 요청하므로, 재사용 시 공급 이름과 PMIC 부모가
연결돼야 한다. I²S3 디지털 스피커가 이 아날로그 DAI를 지난다고
가정하지 않으며, 별도 스피커 전원이나 AFE 클록을 생략하지 않는다.
참조: https://github.com/torvalds/linux/blob/v7.0/sound/soc/codecs/mt6358.c

Audio/stage-i2s-source.py는 공급자 원본 Git에서 I²S 번역 단위의
명시적 파일만 준비한다. Ubuntu 7.0 ASoC에서 사라진 wrapper를
실제 동일 동작의 snd_kcontrol_chip으로 대체했다. 이로써 Ubuntu
7.0.0-43-generic ARM64에서 mt6789-dai-i2s.o 컴파일을 확인했다.
아직 로드 가능한 모듈이 아니고, AFE 구성과 I²S rate transform,
gpio request, MCLK, APLL 및 공용 ASoC 연결이 미해결이다.
원본 AEE 헤더의 monitor_hang_regist_ldt 선언 경고도 전체 AFE
링크 단계에서 따로 해결해야 한다.

동일 스테이징의 t70_afe_i2s_slice.o 대상은 I²S, AFE control 및
AFE GPIO의 실제 세 소스를 함께 컴파일하고 연결한다. 대상 Ubuntu
헤더에서 성공했으며, mt6789_rate_transform, mt6789_dai_set_priv,
mt6789_afe_gpio_request가 더 이상 외부 의존성이 아님을 확인했다.
Android 진단 기능이 꺼진 기존 빈 함수는 본문과 조건을 바꾸지 않고
로컬 심볼로 한정해 번역 단위 간 중복 정의를 없앴다. MCLK/APLL과
전체 AFE/ASoC 등록·PCM 경로는 아직 없으며, 이 산출물은 .ko가 아니다.

--exact-clock-check는 원본 mt6789-afe-clk.c를 변경 없이 목적 파일
링크 검사에 추가한다. 재현 확인에서 네 개 ARM64 목적 파일이
연결됐고, MT6789 내부 미해결 심볼이 사라졌다. clk_get/clk_set_rate
등 일반 Linux API를 실제로 참조하므로 FPGA 빈 구현의 빌드가 아니다.
이는 클록의 호출 코드를 링크한 결과일 뿐, T70에서 APLL·MCLK를
등록하거나 동작시키는 보드 클록 공급자, 전원, 전체 AFE/PCM
등록을 검증하지 않는다. 실행하거나 모듈로 설치하지 않는다.

--pcm-check는 같은 목적 파일 묶음에 MT6789 AFE PCM 등록 코드를
추가한다. Linux 7.0의 ASoC 컨트롤 조회와 플랫폼 remove 선언을
맞추고, 일반 드라이버에 필요 없는 vendor 원시 sysfs를 제외한다.
실제 AFE probe와 I²S 등록이 동일 ARM64 오브젝트에 링크됐다.
아직 미해결 AFE 공통 memif/PCM 및 다른 DAI 공급자가 있고,
이 검사에는 .ko 생성, 로드, ALSA 카드 열거 또는 재생이 없다.

--dai-check는 위 PCM 검사에 순정 MT6789 PCM, SRC, HW gain, hostless
DAI 네 구현을 변경 없이 추가한다. Ubuntu 7.0.0-43-generic ARM64
Kbuild에서 9개 소스의 단일 목적 파일 링크를 확인했다. 기존 AFE의 네
등록 함수 참조가 실제 구현으로 해소됐고, 같은 Module.symvers 기준으로
새 미충족 외부 심볼은 없었다. 공통 PCM/IRQ, 버퍼·SRAM, vendor DSP 및
misc/debug 의존성은 남는다. 이 검사는 modpost나 DKMS 패키지가 아니다.

--platform-check extends --dai-check with the actual vendor common ALSA
platform object built against the same private AFE struct. It keeps the helper
names local to the T70 link rather than importing/exporting the generic MediaTek
names with an incompatible struct layout. Seven referenced helpers now resolve
locally. The plain PCM copy route now reports a short iterator as -EFAULT
instead of claiming success for uncopied data (original fails the fixture;
the port passes for playback and capture). This remains an object-only test:
the remaining dependencies include the vendor front-end/memif and IRQ, firmware-managed
resources, and vendor-specific addon dependencies. Matching export names in
generic Module.symvers do not by themselves validate private AFE ABI.

--fe-check extends --platform-check with the genuine vendor AFE front-end
and memif/IRQ helpers, under the same private namespace. ARM64 Kbuild links
11 objects. Eleven remaining AFE calls now resolve locally (including seven
otherwise picked up from the incompatible generic common export set); the
object no longer imports generic snd-soc-mtk-common symbols. The genuine
front-end adds an unresolved notify_3way_semaphore_control reference; the
semaphore is not replaced with a no-op. The PCM runtime, DMA providers, and
complete .ko/modpost are still separate from this link check.

The DAI set now also includes the real MT6789 ADDA path. The only source
adaptation in that DAI is four ASoC component control lookups. The combined
Ubuntu ARM64 --fe-check object contains 12 source objects and defines
mt6789_dai_adda_register called by the actual PCM probe, with no new unmet
generic imports. This closes the AFE-side registration, not the separate
MT6366 PMIC/regmap/Avdd link or analog playback/capture.

--sram-check adds the actual vendor audio SRAM manager (13 linked ARM64
objects). The effective stock DT and the stock snd-soc-mtk-common.ko both
use prefer-mode, mode-size, and block-size. The pinned source instead used
underscores; fdtget confirms all three old lookups fail on the real board DT.
The source port uses the stock names, checks geometry before dividing or
indexing, and fails cleanly if block allocation fails. Stock SRAM is
0x11212000 + 0xd000, normal/compact mode sizes 0x9c00/0xd000, block 0x1000.
Both referenced SRAM helpers resolve inside the staged object. This is not
an SRAM mapping/runtime test, and the separate DRAM provider still matters.

The staged platform PCM pointer also checks the complete 64-bit hardware
offset against the active memif ring size before narrowing/alignment. A
successfully read position before base, at/after the exclusive end, or
with no configured ring returns ALSA's XRUN position. The host fixture
exercises the exact staged calculation: valid offsets including one that
crosses the 4 GiB low-word boundary, underflow, exact end, a delta that
could alias after narrowing, and an empty ring. This does not qualify
split-register sampling or prove physical DMA across a 4 GiB boundary.

--ack-check adds the actual MediaTek playback acknowledge/clear callback,
private to this AFE. It uses the active ring instead of wrapping at the end
of ALSA's preallocation and refuses an undersized backing allocation.
A host fixture using the actual callback detects the old write beyond the
active ring; the port wraps within the ring and does not clear on invalid
available-frame counts. Ubuntu ARM64 links 14 source objects, with the
callback resolved and no new unmet exports from this stage.

The MT6789 START path now validates rate before enabling DMA, reports
IRQ counter/rate/enable failures, and rolls back only a memif that this
START enabled. Common IRQ helpers preserve the existing arbitration and
release on a failed register write while returning the actual error.
Ten mocked cases exercise the actual staged code, including busy
arbitration, register failure and release, and START rollback. This does
not add the SCP/ADSP semaphore provider or assume an AP-only owner.

The staged PCM drops the vendor raw-register debugfs node as well as the
already-excluded sysfs command surface. The Ubuntu ARM64 link retains the
ASoC probe, START callback and PCM pointer, but no longer imports the three
vendor debug helpers and does not contain the raw node. Eight non-generic
imports remain: misc ALSA controls (one), DSP link (one), external semaphore
notification (one), and the reserved mmap exporter/fd/address helpers (five).

--tone-check adds the actual MT6789 sine-generator controls (seven ALSA
controls) with Linux 7.0 component/enum APIs, and propagates the registration
result. Its scope is test tone only. It deliberately does not register the
vendor USB-call echo loop, modem speech controls, or SCP voice trigger
controls that require their own resource owners. The Ubuntu ARM64 link now
contains 15 objects and has seven unresolved nongeneric imports: the five
reserved mmap helpers, external semaphore notification, and the DSP link.

--alsa-buffer-check removes only the Android HAL's private AAudio
dma-buf exporter/descriptor controls and the corresponding alternate
allocation/free branches. It does not substitute dummy callbacks.
Ubuntu ARM64 still imports standard ALSA mmap, PCM allocation/free,
and the existing SRAM map helper. The vendor DRAM resource callbacks
were left intact. The former five vendor mmap imports and their controls
are absent: DSP attachment and semaphore notification are the only
remaining non-generic symbols in the combined ELF, not proof of hardware
resource ownership. The current generic headers disable vendor SRAM
allocation configuration, so retaining SRAM code is not evidence of
using it for a generic playback stream.

--notifier-check links the original atomic 3-way semaphore notifier
dispatch locally (16 ARM64 objects). It does not replace hardware
arbitration: there is no success when the chain is empty, and the
shared-owner IRQ path still rejects a missing grant. A mocked chain
checks actual callback dispatch for no listener, rejected grant,
accepted grant and unregister. Neither SCP nor ADSP listener is
included or assumed redundant. Only audio_set_dsp_afe remains an
undefined non-generic symbol in the combined object; symbol closure is
not equivalent to safe resource ownership or a working module.

--module-check builds t70-mt6789-afe 0.1.0 with Kbuild and isolated DKMS
against Ubuntu 7.0.0-43-generic ARM64. The actual module passes modpost
and carries symbol versions; DKMS status is built, not installed
(AUTOINSTALL=no). It matches only the new teclast,t70pro-mt6789-afe
compatible, not any stock DT. The probe must acquire a versioned T70
audio resource-owner provider before setting DMA or touching hardware.
The explicit contract has claim, attach, release, and a held module
reference for the entire device lifetime. Missing provider defers,
failed claim returns the provider error, and failed attach unwinds.
No resource-owner provider or DT enablement is shipped. This is a
buildable, deliberately nonbinding development module, not playback
readiness; clock/pin, firmware/DSP ownership and machine routing remain.

Audio/t70pro-audio-providers-staging.dtso pairs the exact stock speaker
I2C1 parent and AFE node with the new generic module compatibles, keeping
both disabled. --check-providers applies this with the disabled AW88261
pair to an offline stock DT and compares every node/property. I2C1 retains
SPI 145, DMA at 0x10217100, West I2C clock index 1, and infracfg DMA index
61; AFE retains the exact SPI 234 and all inherited resources. The stock
machine linkage is untouched. No provider, machine driver or DT enablement
is implied. Five altered IRQ/clock fixtures are rejected.

Audio/test-audio-owner.py compiles the exact staged AFE probe prefix into
a host fixture with mocked driver-core, supplier, devres, and DMA APIs.
It distinguishes supplier deferral before driver entry from the T70
consumer's own missing-owner deferral. Missing owner, busy claim,
allocation failure, and failed devres setup never reach DMA setup.
A guarded retry reaches the mocked DMA call once; failure and success
both release the owner/module reference during cleanup. This does not
execute a kernel probe or exercise the attach/runtime path. The packaged
I2C and AFE ARM64 modules' OF aliases were also compared with the two
disabled provider DT entries.

Audio/Speaker implements a separate playback-only machine card. The AFE
provider is selected by phandle for both DL1 FE and I2S3 BE; the BE uses
the upstream aw88261-aif DAI on both I2C addresses 0x34 and 0x35, checks
their channel order, selects CPU clock/frame provider and gated I2S, and
forces the stock S32_LE backend format. No voice/capture graph is exposed.
Ubuntu 7.0.0-43-generic ARM64 Kbuild/modpost passed. The card has a new
compatible and no enabled DT node. Before enabling it, verify real AFE
ownership, the I2S3 MCLK requirements (the reference machine requests
128x sample rate), DAPM mixer route selection, and amp rails/profile.

The speaker card is now a separate t70-aw88261-speaker 0.1.0 DKMS package.
Isolated Ubuntu 7.0.0-43-generic ARM64 DKMS add/build passed (built,
not installed; AUTOINSTALL=no). The --check-card offline verifier applies
the disabled card DT along with the four disabled provider nodes and checks
the exact AFE and ordered amplifier phandles. Mislabeling an amp is refused.
The stock /sound node and its properties remain unchanged; the new card is
not enabled or selected.

Speaker DKMS 0.1.1 sets a stable ASoC driver identity, t70pro-aw88261,
with a matching ready-to-package userspace UCM2 profile. It opens PCM 0
and toggles only the two verified DL1→I2S3 DAPM mixer controls on device
enable/disable. It does not change amp gain, rails or clock rates. The
existing VM libasound parsed both files and the exact control names/routes;
the module rebuilt in isolated Ubuntu ARM64 DKMS. No profile or module
was installed, and this is not a live playback test.

### Generic storage

verify-storage-dt.py checks the actual T70 source DT without touching media.
The UFS node already advertises the standard mt8183-ufshci alias, and Ubuntu
7.0 enables SCSI_UFS_MEDIATEK=m. The alias alone is not readiness: T70 has
six target clocks rather than the one in the MT8183 schema, named reset
IDs 1–3 and MT6366 VEMC. Stock has no phys property; the v7.0 schema
requires it, but ufs_mtk_bind_mphy can accept -ENODEV, so absence alone
does not prove the runtime blocker. Do not invent a PHY phandle or replace
the known clocks. See Documentation/devicetree/bindings/ufs/mediatek,ufs.yaml
and drivers/ufs/host/ufs-mediatek.c in Linux v7.0.

External SD is MMC1 at 0x11240000, SPI 135, with GPIO7 card detect,
three distinct pinctrl states and MT6366 VMCH/VMC. Both MMC descriptions
use mediatek,mt6789-mmc, which is not in Linux v7.0 drivers/mmc/host/mtk-sd.c
even though CONFIG_MMC_MTK=m. The separate non-removable MMC0 description
is not evidence of installed eMMC. The exact vendor MSDC variant is a
candidate for source porting, pending supplier and timing compatibility;
do not blindly alias another SoC. Six corrupted DT fixtures were rejected.

Do not select generic mt6779-mmc for external SD based on the reference tree.
The pinned reference mtk_mmc.c aliases MT6789 to MT6779, but the actual
T70 stock vendor_boot mtk-mmc.ko has distinct OF relocations to separate
named 40-byte mt6789_compat and mt6779_compat objects. Their only byte
difference is offset 39 (MT6789=1, MT6779=0); its semantics are not inferred.
verify-stock-msdc.py reproduces that result from the exact original ARM64
module and rejects an unrelated input. Use the exact variant source before
offering a generic compatible or timing configuration.

Follow-up: the offset-39 flag above is consumed by stock msdc_cqe_enable
to decide whether to call secure firmware. Only MMC0 has supports-cqe in
the T70 effective DT; removable SD (MMC1) does not. Fixtures reject adding
CQE to MMC1. A separate stock probe check shows MMC always tries the
second MMIO register bank when provided, including MMC1 at 0x11ef0000.
Linux v7.0 mtk-sd.c only maps this bank if needs_top_base is set, which
its MT6779 profile does not set. Therefore external SD neither requires
the observed CQE firmware call nor can be modeled by an unmodified
MT6779 alias: its real top bank and upstream tuning path need a dedicated
candidate. No firmware call or storage probe was run.

Sd/stage-sd-source.py builds the first external-only candidate from the
existing pinned 6.12/common MediaTek source (not the vendor_boot binary
and not a claim of exact Ubuntu driver source). The probe accepts only
teclast,t70pro-mt6789-ext-sd with no-mmc and refuses non-removable/CQE DT
configurations. The actual top resource is mandatory and must map.
The Ubuntu 7.0 ARM64 object built using the renamed GPIO-detect API; its
OF table has just the dedicated compatible, and there are no secure
firmware calls. This is object-only: the baseline private CQHCI code
has not been eliminated from the linkage, and the rail/pinctrl/clock
contracts and UHS tuning remain unverified. No overlay was enabled.

External SD 0.1.0 now passes isolated DKMS add/build and real modpost on
Ubuntu 7.0.0-43-generic ARM64. The staged sources no longer include the
private CQHCI header or initialize, suspend, or dispatch CQE; the ordinary
SD IRQ, tuning, and runtime power-management paths remain. The .ko has
only the exact external-slot OF alias, normal MMC-core dependencies, and
symbol versions, with no CQHCI or secure firmware symbols. AUTOINSTALL=no.
The firmware/kernel pinctrl, regulators, clocks and UHS timing have not
been enabled or exercised by the build.

External SD DKMS 0.1.1 validates both stock MMIO ranges and requires VMMC
and VQMMC regulator handles before mapping the controller. Absence no
longer passes the generic optional-regulator API silently. The new
Sd/t70pro-external-sd-staging.dtso changes only MMC1's binding and status;
verify-storage-dt.py --check-staging compares the complete tree. UFS,
MMC0, GPIO7 card detect, clocks, IRQ and rail references are unchanged.
The node remains disabled; the DKMS build passed but no voltage switch
or SD tuning was exercised.

External SD 0.1.2 also requires the intended four-bit data bus and an
acquired GPIO card-detect handle; a missing GPIO no longer proceeds to
controller mapping. The existing GPIO-provider deferral is preserved.
Sd/test-sd-probe.py exercises the exact staged checks with mocked MMC APIs
and eight cases for parsing, bus width, card detect and rail failures.
Isolated Ubuntu ARM64 DKMS add/build passed. No polarity changes or
hardware observations are implied.

Sd/test-sd-top-bank.py executes the actual staged command/data delay, save,
restore, and runtime suspend/resume callbacks in a two-bank host fixture.
It checks delay steps 0/31/32/63 and a decrease after the high range, leaves
unrelated fields and the primary tuning register alone, restores the top
bank after simulated power loss, and performs no read/write when clock
bring-up reports an error. This uses fake register and clock APIs, not
a live SD/UHS or suspend test.

UFS reset-provider verification decodes the actual seven-cell ti,reset-bits
table and confirms HCI, UniPro and crypto IDs 1–3. Both operations are
set-bit writes and the stock tuples do not expose reset status. Ubuntu 7.0
has RESET_TI_SYSCON=m, using the parent syscon; no new reset driver is
needed for this DT contract. --kernel-config reports the relevant drivers
without conflating config with binding. In particular MTK_PMIC_WRAP=m and
MT6358 regulator support do not by themselves support the stock
mediatek,mt6789-pwrap transport to MT6366 VEMC. Three corrupted reset-table
fixtures were refused. No hardware action was performed.

audit-storage-initramfs.py checks an exact target-release initramfs
file listing against target .config, modules.dep, modules.alias, and
built-in metadata. It separately requires the UFS or SD host, reset or GPIO
provider, both MT6789 clock providers, PMIC wrapper transport, and the
MT6366 MFD/regulator modules. Host module dependencies alone are not enough.
The stock DT is validated first, including UFS reset IDs. The tool
distinguishes built-ins from packaged modules and refuses absent aliases,
missing dependency paths, ambiguous modules and previous kernel releases.
For SD, a clean audit also requires --effective-dtb with the actual
candidate tree. The verifier checks the slot and GPIO7 supplier and reports
if an ancestor prevents normal OF population. An archive-only SD audit
cannot pass. It does not establish binary authenticity or successful binding. Ten fixtures pass, including missing
TI syscon even though the UFS module itself is present. No target Ubuntu
initramfs is implied; the existing stock diagnostics archive lists zero
modules under 7.0.0-43-generic.

The actual Ubuntu arm64 linux-modules-7.0.0-43-generic package (7.0.0-43.43)
was also extracted and audited using read-only depmod -n, including its
built-in alias index. The full package is not the live initramfs. It does
supply ufs-mediatek (mt8183-ufshci) and reset-ti-syscon, including the UFS
core/platform and RPMB dependencies, but has no exact OF match for the
stock MT6789 topckgen, infracfg_ao or PMIC wrapper. The shipped mt6358
regulator and generic mtk-pmic-wrap files do not resolve the missing
MT6789 transport. The stock MT6789 pinctrl alias is absent too.
Copying more files from this same package is not a fix for these
supplier bindings. The test does not inspect a device's partitions or
claim that ID substitution is safe.

Stock PMIC-wrapper evidence: the exact Android vendor_boot mtk-pmic-wrap.ko
(SHA-256 f0a232edcf1b00ed991b61f719b09e94d1fff4f0b0a7a1508c656bbae3d37ee7)
does publish MT6789 and MT6366 aliases. Its vendor source at
mindone/modules/mtk_pmic_wrap/mtk_pmic_wrap.c in a71515dbaf0d539ffb36416f14186dad17eef743
defines a separate MT6789 wrapper description and MT6366 child data; the
MT6765 description differs in register table, capabilities and IRQ masks.
Offline comparison against Ubuntu 7.0.0-43-generic found 2 matching stock
import CRCs, 32 mismatches (including module_layout) and 1 missing export
(aee_kernel_warning_api_func). Thus neither reusing the Android binary nor
substituting an MT6765 alias is an integration path. This identifies the
source for a future native port; no transport operation was performed.

The storage clock source split is likewise explicit in the pinned vendor
tree: mindone/modules/clk_mt6789/clk-mt6789.c owns topckgen/apmixedsys, and
mindone/modules/clk_mt6789_bus/clk_mt6789_bus.c owns infracfg_ao. Their
vendor_boot binaries contain match strings but publish no modinfo OF
aliases. The Android top-clock binary has 21 incompatible symbol CRCs
and one absent Ubuntu export (mtk_mux_clr_set_ops); the infra-clock
binary has seven incompatible CRCs. For the stock external SD slot,
source uses topckgen index 21 and hclk/source_cg use infra indices 21/28;
UFS uses topckgen index 39 (ufs_ck) and infra indices 47/43/42/41/48.
These are DT selectors, not regulator/clock programming values.
A native port must retain the two providers and address their API
dependencies; an ID-only alias is not enough.

verify-stock-connectivity.py ties the four exact stock ARM64 module hashes
to the effective DT and expected symbols. Wi-Fi is the MT6789 vendor AXI
platform HIF at 0x18000000, with WMT and cfg80211 dependencies; an enabled
generic mt76 PCI/SDIO module alone does not match that path. Bluetooth
talks vendor WMT/STP; WMT has a BTIF backend at 0x1100c000 and also compiles
unrelated SDIO match entries, so those entries cannot prove this tablet
runs Bluetooth over SDIO. BTCVSD is a separate voice PCM device, not HCI.
The audit rejects three wrong DT layouts. No RF operations were used.

The stock Bluetooth module exports a character device (cdev), not a
registered BlueZ HCI controller. Its source selects vendor mode and
feeds STP task 0; the injected Hardware Error event uses an H4 prefix.
Bluetooth/t70-h4-rx.h implements a separate bounded stream parser for
classic HCI events, ACL and SCO. It reassembles across chunk boundaries
and reports malformed frames or downstream errors without silently
consuming later frames. ASan/UBSan checks passed at all ten split points
and for maximum ACL, oversize, invalid type and backpressure. This is an
offline receive building block; it has no transport claimant, hardware
operations or HCI device registration yet.

Bluetooth/t70-stp-hci is a generic kernel HCI consumer with a versioned
STP provider contract. It uses standard HCI registration/delivery and
the tested classic H4 parser, and only registers after an exclusive
channel-0 transport provider successfully attaches. No provider, physical
power or register access, pairing, or DT enablement is included. DKMS
0.1.0 built on Ubuntu 7.0.0-43-generic ARM64 with symbol versions. The
actual module imports the generic Bluetooth core, not stock Android
symbols, SDIO/UART, or BTIF directly. The provider remains responsible
for WMT lifecycle/reset and ordered TX/RX with no partial frame acceptance.

Bluetooth HCI DKMS 0.1.1 keeps up to 64 accepted TX frames in FIFO order.
A temporary -EAGAIN/-ENOSPC leaves the head intact and schedules a bounded
20 ms retry interval rather than returning that frame to HCI core for
freeing. Hard errors update the TX error count; flush/close drain pending
frames and close cancels work before the provider is released. The exact
callbacks pass a host fixture covering retry order, queue limit, flush
and close, and the Ubuntu 7.0.0-43-generic ARM64 DKMS rebuild passes.
This does not add or load the actual WMT/STP provider.

Bluetooth/stage-btif-hal.py stages the actual MediaTek BTIF platform and
DMA constituents (fixed stock source revision and explicit header link).
Both sources are unchanged and link together as an Ubuntu 7.0 ARM64
object using the existing non-legacy Linux OF/clock declarations; no fake
hardware functions. It has no platform registration or HCI exposure.
The normal diagnostic compiler notices two legacy false→mode enum
conversions. The controller/lifetime owner, WMT/STP layer, supplier
resources and module link remain separate; no live GPIO/clock/DMA access.

BTIF --core-check adds the vendor controller and export objects. The
staged controller registers without waiting for its device. Probe prepares
the clocks and initializes the singleton before publishing it. An unavailable
clock can defer probe, and a later attempt binds the same registered driver.
The staging driver removes the vendor writable flag and suppresses manual
bind/unbind for the fixed platform device. This remains an object build for
the controller and transport; the WMT/STP provider and device execution
are separate.

WMT/STP provider follow-up (pinned reference a71515db): the normal fullset
path queues the whole frame and reports its length, or returns zero on
queue/reset backpressure. Mandatory-mode compares the entire framed
write count and returns zero on a short underlying write. However the
legacy closing and firmware-coredump branches also return the requested
length without transmitting; PSM-hold has a separate return path.
Therefore do not implement the generic adapter by translating any
reported full byte count to success. It must use an owned state/queue
contract and synchronize against close/reset; no positive-partial policy
has been established for the exact stock device. Legacy callback removal
assigns the function table slot and clears the RX queue, but there is
no drain in that API itself; an adapter's close must independently wait
for in-flight callbacks before freeing the generic HCI state. The standard
Linux 7 HCI TX core owns skb on send error, RX core consumes on error, and
RX must remain available during HCI_INIT as well as HCI_UP.

--probe-lifetime selects the same controller build as --core-check.
Existing scripts can still pass --fail-unbound as an alias. The host
fixture registers with a deferred clock, binds after the clock comes up,
and checks balanced removal and failed probe cleanup.

BTIF --software-alloc-check adds the RX, DMA and interrupt ownership
changes. The singleton buffers belong to the bound device and are freed
on partial setup failure or after the workers stop during removal.

BTIF --rx-overrun-check removes raw register/memory dumps from the software
RX overflow path (including the old unconditional RX DMA context access,
which is not valid in PIO). It no longer reports a positive accepted count
when that write itself resets and discards the ring. An exact-callback
host fixture reproduces the original false count and passes normal writes,
reset generation and PIO with no DMA context. Ubuntu ARM64 object linking
passed. Existing raw IRQ/DMA and downstream reset propagation/lifetime
have not been validated or enabled.

BTIF --tx-fifo-check makes the software FIFO's enqueue all-or-none.
The pinned producer previously inserted a prefix when space was short
but returned zero (retry the entire chunk), which corrupted byte order.
A separate producer spinlock and capacity check avoid partial admission;
the sole worker discards on fatal error via its own output index. The
exact-callback fixture reproduces the old prefix and passes rejection
without mutation and a later complete retry. Ubuntu ARM64 object link
passes. The underlying worker's no-progress retry policy and the
probe/remove ownership remain unqualified; this is not a .ko.

BTIF --tx-cleanup-check clears the queue descriptor after a backing FIFO
allocation fails, so the caller's second teardown cannot use the freed
descriptor. The exact staged TX init/deinit fixture reproduces that use
in the pinned source and passes all three allocation faults, repeated
teardown and user-thread TX. The normal exit now drains the software TX
workqueue before DMA and state teardown. Ubuntu ARM64 object linking
passed; the legacy worker still needs a no-progress bound and the probe/
remove lifetime remains incomplete, so this is not packaged as a .ko.

BTIF --pio-progress-check preserves direct TX's signed count contract.
The pinned source defaults to BTIF_TX_USER_CTX, not BTIF_TX_SINGLE_CTX;
the earlier software-queue fixes did not change that default. The PIO
HAL can accept a prefix. A following negative result used to be accumulated
as if it were bytes, hiding the accepted prefix from the caller. The staged
callback returns the actual prefix after any later error, or a negative
result if no bytes were accepted. The extracted original callback fails
the scripted partial-plus-error case; the fixed callback passes that,
initial error, zero progress, suffix retry and overreported-count tests.
Ubuntu ARM64 object linking passed. The caller must still preserve that
suffix, and optional queued mode still needs bounded retries and lifetime
synchronization. No TX mode, HAL register logic, or provider selection changed.

BTIF staging now checks both required clock prepares before publishing probe
success, unwinds a partial prepare, and releases the prepare references at
platform remove. Mock clock and ARM64 object checks cover this lifecycle.
The IRQ request now precedes DMA start; this does not prove that a running
DMA engine has stopped. The staged FIFO is manually coherent-allocated;
some provider resources (including the clock handles) are device-managed.
The void platform remove callback cannot veto removal or stop devres cleanup.
Do not package or deploy this HAL as an unload-safe module until an owner
can quiesce DMA (including STOP failure) before freeing the FIFO or losing
the provider references. No recovery reset or MMIO behavior is assumed here.

The pinned vendor STP/BTIF adapter already retries a short direct BTIF
write with the suffix (up to ten 5 ms waits). However, timeout or a later
error after accepting bytes reports *written_len=0; returning -wr_count
does not repair that because BTIF mandatory STP checks the output count
and discards the callback status. The exact pinned adapter fixture proves
suffix recovery, the two partial-frame loss cases, the difference from
zero progress, and that a later whole-frame retry would replay the prefix.
A closed adapter can also leave written_len unchanged. Do not wire this
callback into the HCI provider as an all-or-none offer: retain the complete
serialized frame and offset until drained, or surface a fatal transport
state if the link cannot be recovered; do not advertise generic retry
after an irrevocable partial send. This is a host regression fixture,
not a replacement STP provider.

t70-stp-hci 0.1.2 scopes RX allocation or HCI-core rejection to the
one already framed packet and increments err_rx. Those faults no longer
make the STP producer discard a valid following H4 packet. Unknown H4
type or invalid length still reports a framing error; the provider must
handle that stream boundary. Host tests reproduce original loss and pass
allocation failure, HCI consumed-on-error, split callback, malformed type
and next transaction recovery. The existing TX regression and isolated
Ubuntu 7.0.0-43-generic aarch64 DKMS add/build both pass.

Wi-Fi's stock 6789 object defines and references the SOC2_1X1 chip
implementation; the firmware prefix is nevertheless WIFI_RAM_CODE_soc2_2.
The pinned SOC2_1X1 source declares that same prefix and gets the flavor
from its platform data/helper. Do not infer SOC2_2X2 from the file names.
The connectivity audit's optional --stock-firmware-dir checks eight
exact shipped hashes: 1d and 1e each have a WLAN executable plus distinct
BT, MCU and Wi-Fi ROM patches. The original vendor files pass; a swapped
patch and a missing executable both fail. No file is installed and the
presence of both families does not identify the active flavor. The stock
WMT ROM patch selection invokes the userspace command srh_rom_patch with
chip version context, separate from WLAN's runtime firmware lookup. A
generic port must replace that selection contract or explicitly discover
the board flavor; do not mix families or reuse Android modules.

Wifi/stage-wlan-chip.py stages the pinned SOC2_1X1 AXI chip translation
unit and genuine declaration headers for target Kbuild object checking.
Ubuntu 7.0.0-43-generic ARM64 compiled the actual chip descriptor and
firmware callbacks. The staging configuration excludes private QA,
its TX BF dependency, and the WEXT-only calibration debug reset hook;
it is not the shipping feature configuration. The firmware name/loader
hooks still reference kalGetFwFlavor and wlanConnacFormatDownload, and
the object still needs the AXI/PDMA, firmware, and platform connectivity
providers. This stage does not generate or claim a DKMS Wi-Fi .ko.

Wifi/stage-wlan-chip.py --with-loader also compiles the pinned Connac
firmware parsing/download translation unit and links it with SOC2_1X1
as t70_wlan_chip_fw.o. Its chip callback resolves to the genuine
wlanConnacFormatDownload, wlanGetConnacFwInfo and tailer implementation
in the Ubuntu ARM64 object. Remaining firmware request, AXI DMA and
connectivity-provider operations are unresolved in this partial build.

For the two available WLAN executable families, the pinned platform
code and exact stock ELF both query WMTCHIN_ADIE: 0x6631 maps to 1d
and 0x6635 maps to 1e. The audit's --adie-id accepts an already observed
A-die ID; it does not interrogate hardware or infer it from the MT6789
SoC ID. Offline tests cover both values, no observation, and rejection
of 0x6789 as an A-die ID. The selection report does not replace the
separate WMT ROM-patch lookup or assert which revision is physically fitted.

Wifi/stage-wlan-chip.py --with-pdma adds the pinned HAL, kernel glue
and debug PDMA units to the same ARM64 object. The staging adjusts only
the Linux timer API spelling at two synchronous cancellation sites and
the corresponding timer owner lookup. The Ubuntu 7.0.0-43-generic object
links the real ring allocator, recovery callback and register-access
glue; no test substitutes are linked. AXI platform probe is still
outside this target: its generic compile reaches the missing EMI
protection provider and the changed platform .remove callback. The
EMI protection call must not be stubbed or dropped. The composite is
still not an installable WLAN module.

With the Android EMI backend absent, the original firmware loader
reported success for an EMI section that had not been written. Generic
staging now returns NOT_SUPPORTED for that path and stops at the first
non-success section; the Connac entry point releases the input image
without calling the firmware-start routine. The callback fixture
compares the two versions and confirms the ordinary non-EMI path
still works. The full staged ARM64 object still links.

The exact stock WLAN ELF's kalSetEmiMpuProtection is a single return
instruction. This is distinct from kalSetDrvEmiMpuProtection, which
references the vendor mtk_emimpu_* API. Neither path is proof of an
acknowledged generic protection provider. Stock WLAN DT supplies EMI
size 0x600000, alignment 0x1000000, max address 0xc0000000 and base
zero, with no memory-region phandle on the WLAN node. These are not
evidence for any particular owned physical range. No EMI MMIO, mapping,
protection or reservation path is added or enabled by this stage.

### Motion / environmental sensor path

verify-stock-sensorhub.py checks the exact T70 stock SCP/HF four-module
chain and effective DT. The sensor hub consumes a runtime enumeration
from SCP, not CPU-side I2C/SPI probe for the unnamed motion sensors.
It uses mailbox 2 at 0x107fd000, GIC SPI 470, TX IPI 29/31 and RX IPI
30/32, and six named SCP memory-table entries. The declared sizes are
not physical addresses. Positive stock and negative mismatched
mailbox/list-buffer fixtures pass. This does not claim which sensors
the SCP will enumerate.

Ubuntu's config has MTK_SCP, RPMSG_MTK_SCP and IIO, but the
[upstream Linux 7.0 match and probe](https://github.com/torvalds/linux/blob/v7.0/drivers/remoteproc/mtk_scp.c)
do not implement the stock mediatek,scp node with its vendor resource
names or the HF-manager data contract. Do not alias to a Chromebook
SCP or create a direct I2C node from IMU model guesses. The missing
pieces are the MT6789 SCP transport and its firmware/memory contract,
followed by a standard IIO consumer of its actual sensor data.

For the complete wire/sample composite, compare the output of
`nm -u t70_sensorhub_wire.o` with the target build's `Module.symvers`.
Ubuntu 7.0.0-43 exports `scp_ipi_register` / `scp_ipi_send`, not the
vendor tiny-IPI interface `mtk_ipi_*` and `scp_ipidev`, the SCP-A
readiness/sensor/reserved-memory interfaces, the HF-manager hooks, or the
vendor custom/debug and Awinic hooks. The protocols and ownership are not
interchangeable by symbol renaming. The staged normal-notify callback
drops its diagnostic-only `printk_deferred` timing trace because Ubuntu
does not export it, without changing the sample timestamp/filter logic.

Sensors/stage-sensorhub-wire.py compiles the genuine sensor communication
callbacks and CRC-8 into t70_sensorhub_wire.o for the generic ARM64
kernel. Compile-time checks fix the control/ack headers at 8 bytes and
notifications at 28 bytes (payload offset 8). The staged receive checks
length before reading its CRC; offline control/notify returns EAGAIN
instead of reporting delivery, and communications init propagates an
IPI initialization error before registering callbacks. The host fixture
uses the actual callbacks and stock CRC with successful, not-ready and
failed-IPI test shims; Ubuntu 7.0.0-43-generic object linking passes.
The real SCP IPI transport and readiness callbacks are still undefined
and no .ko or sensors were registered.

Sensors/stage-sensorhub-wire.py --with-ipi links the pinned transaction
and queue implementation, using the actual tiny-IPI/SCP declarations.
It propagates either channel registration failure and unbinds an already
registered control channel; failed sends no longer leave a transaction
open, and the host copy requires the exact expected ACK size. The staged
callback fixtures pass allocation/registration failure, send error,
timeout, and normal/bad-sized ACK cases; Ubuntu ARM64 linking passes.
The device-side mtk_ipi_* backend, SCP readiness generation and actual
SCP/HF/IIO consumers are not supplied, so there is still no sensor .ko.

Sensors/stage-sensorhub-wire.py --with-ready adds the real two-sided
SCP platform/sensor readiness callbacks. It initializes all bytes of
the host's fixed-size READY notification, including unused payload and
padding, and excludes the stock automatic watchdog reset for a missing
ACK. Host fixtures verify offline until both sides are ready, an ACK
during send, and clearing readiness on STOP. Ubuntu ARM64 links all
five units; SCP notification ownership/unregister synchronization and
the actual device IPI backend are still external. Not a sensor .ko.

Sensors/stage-sensorhub-wire.py --with-list links the original runtime
sensor enumeration code. An ACK wakes its request only when command,
sensor type and sequence all match the outstanding query; late and
duplicate replies cannot overwrite its saved write position. The host
fixture exercises correlation and completion, while the Ubuntu ARM64
build links all six units. Sequence wrap remains the vendor's 8-bit
protocol; this is not authentication and does not replace transport
lifetime or generation fencing. Sensor list shared memory and the
SCP backend are not yet supplied; no physical enumeration is claimed.

Sensors/stage-sensorhub-wire.py --with-shm links the pinned SCP shared
ring implementation. The host derives capacity from the supplied map,
aligns it to complete records and rejects space for fewer than two.
Reads/writes require a matching ring header and indices within the
record-aligned host span; failed initialization leaves the map alone.
Memory-backed fixtures pass empty, wrap, full and mismatched-size
cases; Ubuntu ARM64 links all seven units. The SCP-owned physical map,
coherency, configuration ownership, provider stop/drain, and cross-session
stale-event behavior are still to be established. This is not an IIO
device, a reusable physical reservation or an installable .ko.

Sensors/stage-sensorhub-wire.py --with-config validates all six SCP
shared-memory handoffs before touching any ring headers. Missing
handlers/maps, overlaps, and extents that cannot fit the stock 32-bit
address protocol fail without publishing. Initializer/send errors
are propagated. The staging uses the same validated physical addresses
for the message, rather than querying them twice. Host fixtures pass
missing, oversized, overlapping, failed init/send and valid cases;
Ubuntu ARM64 links. Firmware rollback after a partially sent pair of
messages and provider mapping lifetime still require an owned
SCP session. No device-side mappings or firmware were changed.

Sensors/stage-sensorhub-wire.py --with-samples links the actual stock
sample transceiver and its clock conversion/filter code. The staged
initial publication is serialized; leave first-boot pending if memory
handoff or sensor enumeration/manager creation fails, and start
time sync only once first registration succeeds. A callback fixture
passes failed handoff, failed registration, retry and subsequent
restore. Ubuntu ARM64 links nine units. Stock HF publication, private
debug/custom paths and the real SCP backend are not linked into an
installable module; a native IIO consumer still needs real provider
and sensor-unit evidence.

The --with-samples stage now uses controllable Linux delayed work for
sensor time sync instead of an unconditionally self-rearming timer
plus a separate work item. Suspend stops/drains work while preserving
a real prior start; resume cannot start an unregistered sensor clock.
Stop/exit disable requeue, and exit drains before releasing its wakeup
source. A callback fixture exercises a requeue during drain, suspend/
resume, explicit stop, and no restart after exit. The Ubuntu ARM64
composite links. This does not change the SCP clock reference or establish
device-side stop/re-ready session delivery.

The staged sensor sample delivery no longer retries every consumer error
indefinitely. The stock HF consumer reports EAGAIN on a full buffer and
can recover after one second; the staged callback allows up to roughly
1.1 seconds for that case, stops on a permanent error or thread stop,
and decrements pending flushes only on acceptance. A host fixture verifies
recoverable pressure, deadline, thread stop, permanent error and flush.
The Ubuntu ARM64 composite links; HF/IIO publication is still external.

The --with-samples stage also closes the sample gate when the host
receives an SCP STOP. It clears both position queues, stops the clock
work and serializes host ring reads against shared-memory setup. The
gate stays closed after a failed setup. One READY notification starts
up to five setup attempts, 100 ms apart after a failure. A successful
attempt reopens the gate. The host fixture checks recovery without a
second READY, the retry limit, and cancellation on STOP. This only rejects retained host queue
entries. It does not authenticate late firmware messages, fix a STOP
lost by the device notifier, or establish provider-side epoch/drain.

Stock T70 /timer clock-frequency is 13,000,000 Hz and the vendor
sensor time sync compares CNTVCT with SCP's matching counter in host
CLOCK_BOOTTIME. The staged conversion uses exact quotient/remainder
arithmetic instead of a signed multiply that overflows on long deltas.
It resets the host filter before reopening after SCP readiness. Offline
fixtures check a two-hour delta, delivery delay, a boottime gap and a
new SCP offset; now/adjusted comparisons remain host-side tests.
A native IIO consumer must retain BOOTTIME or convert to the clock it
publishes in current_timestamp_clock; no IIO device or physical time
source has been registered or changed.

Sensors/IioMotion is the native buffered three-axis accel consumer
t70-scp-accel 0.1.0. It requires a real, exclusive provider exporting
t70_scp_motion_attach_v1 with an independently established scale,
serialized boottime-stamped samples and synchronous callback drain.
The module provides no fake provider or enabled DT node. It will
defer probing without that provider. Initial buffers support
CLOCK_BOOTTIME only: the callback passes through the acquisition
timestamp, not delivery time, and refuses another IIO clock rather than
mislabeling it. Exact callback fixtures pass XYZ/scan alignment, time
and start/stop checks. Isolated Ubuntu 7.0.0-43-generic aarch64 DKMS
add/build passed; no installation, SCP provider, or desktop orientation
is claimed.

### Integrated LTE modem

verify-stock-wwan.py verifies four pinned T70 modules plus the effective
DT resources for the MT6789 integrated LTE path. Control is vendor CCIF
via CCCI; raw IP network data is DPMAIF v1 via CCMNI. The DT has two
dynamic DPMAIF memory reservations (cache/no-cache), not resolved
physical addresses. The native stock data module contains the MT6789
v1-specific queue path. The exact T70 files pass, and a substituted
DPMAIF v3 fails. Ubuntu's CONFIG_WWAN is the generic interface framework;
[upstream MediaTek t7xx](https://github.com/torvalds/linux/blob/v7.0/drivers/net/wwan/t7xx/t7xx_pci.c)
binds PCIe and is not the built-in T70 modem transport. This check
does not boot/start the modem, or assert active SIM/data service.

Wwan/stage-ccmni.py stages the genuine stock CCMNI netdev code and its
actual declaration headers. Ubuntu 7.0.0-43-generic moved the GRO lists
under napi->gro; use the kernel's gro_flush_normal() instead of
accessing obsolete fields and importing an unused Android trace
header. The stock configuration's original object failed at those
fields; the staged ARM64 object builds with real GRO, TX and netdev
registration symbols. Vendor RPS and modem transport still need
providers. This is an object-only probe, not an installable LTE driver.

Wwan/stage-ccmni.py --module builds t70-ccmni 0.1.0 with isolated
Ubuntu 7.0.0-43-generic aarch64 DKMS add/build/modpost. It keeps the
genuine callback-driven CCMNI TX/RX/network core, using the stock
plain receive path with exported netif_rx/netif_receive_skb. It
does not install the Android workqueue GRO, vendor RPS API or private
global pacing sysctl: the stock batching option needs an unexported
Linux symbol. The result exports ccmni_ops; there is no automatic
registration, hardware probe or modem start. The CCCI transport and
its final lifecycle/ABI matching are separate, and this is not yet
a working LTE interface. No device install or network tuning applied.

t70-ccmni 0.1.1 makes RX ownership explicit. The stock CCCI port
hands off an skb without inspecting the callback result. The
generic module therefore consumes it exactly once even if the modem
or channel instance is unavailable, and validates the linear IP
header before assigning skb protocol. Offline callback tests pass
absent transport and normal IPv4/IPv6 delivery; Ubuntu ARM64 DKMS
build/modpost pass. Transport teardown still needs to quiesce the
provider's callbacks; this does not activate the modem.

Wwan/stage-ccmni.py --with-port links the stock CCCI network-port
translation unit and generic CCMNI together. Both use the same
CCMNI header and feature configuration. The port uses the original
CCCI channel table and TX/RX callbacks. TX checks that a channel and
port exist before prepending a zero-initialized header, and a busy
send restores the original skb view. Exact callbacks pass the host
port/ACK-channel/missing-endpoint/busy tests, and Ubuntu ARM64 linking
resolves ccmni_ops. port_net_send_skb_to_md, modem state and the
device-side DPMAIF backend are still external; this combined target
is only an intermediate object, not a new DKMS release.
