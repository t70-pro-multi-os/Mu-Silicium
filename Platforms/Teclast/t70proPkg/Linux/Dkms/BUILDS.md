# T70 Pro 제너릭 Linux DKMS 결과

2026-11-24 기준. 경로는 Mac의 ~/.tmp/teclast-t70pro/ 아래 상대 경로다.
모두 Ubuntu 7.0.0-43-generic, AArch64, modversions용으로 빌드된 미서명 바이너리다.
다른 ABI에서 재사용하거나 장치에 로드한 결과가 아니다.
외장 SD 0.1.7은 DKMS가 사용하는 외부 Kbuild를 직접 실행해 빌드했다. DKMS 등록·설치는 하지 않았다.

| 구성 | 버전 | 파일 | 남아 있는 연결 |
| --- | --- | --- | --- |
| Himax HX83132 터치 | 0.1.3 | linux-himax-0.1.3.aJeX67K5/t70_hx83132.ko | 실제 I²C0와 MT6789 GPIO9 EINT, 기존 컨트롤러 상태 |
| T70 I²C 부모 | 0.1.4 | linux-i2c-0.1.4.NIaLCwP8/t70_mt6983_i2c.ko | 기존 DT의 클럭·DMA·IRQ 공급자 |
| 외장 SD | 0.1.7 | linux-sd-0.1.7.hQrN3PU9/t70_mt6789_ext_sd.ko | VMMC/VQMMC, 카드 감지 GPIO, 클럭/IRQ |
| CCMNI 네트워크 코어 | 0.1.4 | linux-ccmni-0.1.4.wAwSZnVY/t70-ccmni.ko | CCCI/DPMAIF와 모뎀 연결. 이 모듈 단독으로 인터페이스를 등록하지 않음 |
| MT6789 AFE | 0.1.3 | linux-afe-0.1.3.GCay2s4r/t70_afe_i2s_slice.ko | 명시적 오디오 자원 소유자와 ASoC 연결 |
| AW88261 스피커 카드 | 0.1.2 | linux-speaker-0.1.2.He7lp9wX/t70_mt6789_aw88261.ko | AFE, 앰프 두 개, I²S3 MCLK. UCM2 소스는 Audio/Speaker/ucm2 |
| SCP 가속도 IIO 소비자 | 0.1.1 | linux-scp-accel-0.1.1.4gXa6ZTh/t70-scp-accel.ko | 독점 SCP motion provider. 전체 센서허브 모듈 아님 |
| STP Bluetooth HCI 소비자 | 0.1.3 | linux-stp-hci-0.1.3.pL96SFsu/t70-stp-hci.ko | STP/WMT와 BTIF. BTIF HAL 자체는 배포 대상 아님 |

MT6789 pinctrl, 전체 SCP 센서허브, BTIF HAL, Wi-Fi 및
CCCI/DPMAIF 전송 계층은 검증용 소스나 오브젝트가 있어도 위 DKMS 결과에
포함하지 않는다. 이 목록은 설치 지침이 아니며 stock DT를 변경하지 않는다.

## 오프라인 검증

OrbStack Linux VM에서 결과 디렉터리와 대상 Ubuntu 헤더의 Module.symvers를
넘겨 확인한다. ELF·서명 여부·vermagic·OF alias·import CRC를 검사하며
장비에 모듈을 로드하거나 설치하지 않는다. 필요한 DT 공급자나 모듈 런타임
연결은 이 검증 범위가 아니다.

    python3 verify-builds.py /path/to/teclast-t70pro /path/to/Module.symvers

## SHA-256

    b59be897d6173697b56c5fb7d21a1eee03007e6486e3d7a2aae808761aee5b1c  linux-himax-0.1.3.aJeX67K5/t70_hx83132.ko
    eb6abf543c3172820c2413da6485cace445a5e43746097f0c7ee6a568ecc8532  linux-i2c-0.1.4.NIaLCwP8/t70_mt6983_i2c.ko
    679431d96f4f666e0e147710d4719e045f564b3453ffc56adc93bc2ac8d52ae1  linux-sd-0.1.7.hQrN3PU9/t70_mt6789_ext_sd.ko
    23e05f909621928bbe14bdb2272ee75fef393a56e30e7039ff43ad15c693ec0f  linux-ccmni-0.1.4.wAwSZnVY/t70-ccmni.ko
    ae8cceed33a57a69895c73ce549d3d8290b53c89210de390f3ab7bcc97197b87  linux-afe-0.1.3.GCay2s4r/t70_afe_i2s_slice.ko
    acb4ae865e9de96a2f37f32928f9a512ec1c0da813a9ec4397ddbc437ddd622a  linux-speaker-0.1.2.He7lp9wX/t70_mt6789_aw88261.ko
    426808d37ed03a06436ced98787585e2473b72066d86510aeef316808f2c958d  linux-scp-accel-0.1.1.4gXa6ZTh/t70-scp-accel.ko
    376649410f1c2396495e851d4afd1d16ea3e8252dc92ae5589e9735a25d0daa8  linux-stp-hci-0.1.3.pL96SFsu/t70-stp-hci.ko
