# T70 Pro Windows 설치 키트

마이크로소프트 공식 Windows 11 ARM64 ISO에 이 키트의 드라이버만 더해 T70 Pro 설치 메모리를 만듭니다.
ISO는 받은 그대로 쓰며, 언어도 받은 ISO를 따릅니다. 한국어 ISO로 만들면 설치 화면과 설치된 Windows 모두 한국어이고, 한글 입력기도 기본으로 들어 있습니다.

## 준비물

- T70 Pro: Mu-Silicium 펌웨어가 설치된 상태
- 16GB 이상 USB 메모리: 전부 지워집니다
- Windows 11 ARM64 ISO: <https://www.microsoft.com/software-download/windows11arm64> 에서 원하는 언어로 받습니다
- 이 키트: 폴더 구조를 바꾸지 마세요. 스크립트가 `SHA256SUMS.txt`로 키트 파일을 확인합니다

## 1. 설치 메모리 만들기

### Windows

1. USB를 꽂습니다.
2. 시작 메뉴에서 "터미널"을 오른쪽 클릭하고 "관리자 권한으로 실행"을 고릅니다.
3. 키트 폴더에서 아래를 실행합니다.

```
.\make-usb.cmd
```

ISO 경로와 USB 디스크 번호를 물어봅니다. 미리 지정하려면 `.\make-usb.cmd -Iso C:\경로\Win11_arm64.iso -Disk 3` 처럼 실행합니다.
따로 설치할 프로그램은 없습니다.

### macOS

```
brew install wimlib
diskutil list                      # USB 디스크 이름 확인 (예: /dev/disk4)
python3 make-usb.py --iso ~/Downloads/Win11_arm64.iso --disk /dev/disk4
```

### Linux

```
sudo apt install wimtools dosfstools   # 배포판 패키지 이름은 다를 수 있음
lsblk                                  # USB 디스크 이름 확인 (예: /dev/sdb)
sudo python3 make-usb.py --iso ~/Downloads/Win11_arm64.iso --disk /dev/sdb
```

세 방법 모두 디스크 이름이나 번호를 다시 입력해야 지우기 시작합니다.
USB나 이동식이 아닌 디스크, 또는 지금 시스템이 쓰는 디스크는 거부합니다.

### 스크립트가 바꾸는 것

모두 USB 메모리 안에서만 바뀌고, ISO 파일 자체는 건드리지 않습니다.

| 바뀌는 것 | 이유 |
|---|---|
| USB를 FAT32 파티션 하나로 다시 만듦 | 태블릿 펌웨어가 FAT32를 읽음 |
| `sources\install.wim`을 `install.swm`, `install2.swm`으로 나눔 | FAT32는 4GB 넘는 파일을 담지 못함. Windows Setup이 나눈 파일을 그대로 읽음 |
| `sources\boot.wim`(Setup 부팅 이미지)에 `\T70` 폴더와 `winpeshl.ini` 추가 | 설치 메모리가 T70의 USB 단자 뒤에 있어서, Setup이 메모리를 읽기 전에 드라이버가 먼저 올라와야 함 |
| `efi\microsoft\boot\bcd`의 Setup 항목에 시험 서명을 켬 | 드라이버가 마이크로소프트 서명이 아닌 시험 서명이라서 |

끝나면 쓴 내용을 전부 다시 읽어 ISO와 비교한 뒤 마칩니다.

## 2. 태블릿에서 설치

1. USB를 꽂고 태블릿을 켭니다. 펌웨어 로고가 뜨는 3초 동안 **볼륨 위** 키를 누르면 부팅 메뉴가 나오고, 여기서 USB를 고릅니다.
2. 드라이버를 올리고 메모리를 찾는 데 몇 초 걸린 뒤 Windows 설치 화면이 뜹니다.
   - TPM이 없다는 이유로 Windows 11 요구 사항 화면에서 막히지 않습니다.
3. 평소처럼 설치합니다.
   - 설치할 위치는 **Windows용으로 비워 둔 공간**을 고릅니다.
   - Android 파티션은 지우거나 포맷하지 마세요.
4. Setup이 파일 복사를 마치면, 첫 재시작 전에 설치된 Windows에 T70 드라이버와 시험 서명을 자동으로 넣고 다시 시작합니다.
   - USB를 뽑을 필요는 없습니다. 펌웨어가 내부 저장소를 먼저 부팅합니다(내부 우선 부팅이 들어간 펌웨어 기준).
5. 첫 설정 화면부터는 일반 Windows와 같습니다.
   - 화면 오른쪽 아래의 "테스트 모드" 표시는 시험 서명 드라이버 때문이며 정상입니다.

## 문제가 생기면

- 중간에 멈추면 화면에 이유가 나오고, 키를 누르면 다시 시작합니다.
- 기록 파일은 USB 메모리 맨 위에 남습니다.
  - `T70-setup.log`: 설치 시작 파일의 기록
  - `T70-dism.log`: 설치된 Windows에 드라이버를 넣은 기록
- 같은 기록이 Setup 실행 중에는 `X:\T70\StartT70Setup.log`에도 있습니다.
- Setup을 설치 전에 닫으면 아무것도 바꾸지 않고 다시 시작합니다.

## 알려진 제한

- **보안 부팅:** 드라이버가 시험 서명이라 켤 수 없습니다.
- **TPM:** 태블릿에 없습니다. BitLocker 자동 장치 암호화, TPM을 요구하는 프로그램, 큰 버전 업데이트의 자동 배포는 쓸 수 없습니다.
- **그래픽:** GPU 드라이버가 아직 없어 화면은 CPU로 그립니다.
