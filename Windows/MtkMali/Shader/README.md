# DXBC에서 Mali-G57 셰이더 생성

`ShaderCompiler.c`는 실제 vkd3d-shader와 Mesa 컴파일러를 연결한다.
완전한 DXBC 컨테이너 또는 Windows DDI 토큰을 SPIR-V, NIR, arch-9 Valhall 기계어로 변환한다.
SM4.0/4.1/5.0의 정점·픽셀 셰이더를 받으며, 고른 대상은 G57
`0x90930000`이다. 이 값은 실기기에서 읽은 GPU ID가 아니다.

## 호출과 출력

`ShaderCompiler.h`의 C ABI는 C++에서도 호출할 수 있다.
`t70_shader_request`에 ABI 버전·대상 GPU·단계·DXBC 주소/크기를 넣고,
0으로 지운 `t70_shader`를 `t70_shader_compile`에 전달한다.
반환값이 0이면 생성된 코드와 부가 정보를 사용하고,
`t70_shader_free`로 같은 DLL에서 해제한다.

정점·픽셀 쌍은 `t70_shader_compile_pipeline`으로 함께 컴파일한다.
0으로 지운 `t70_shader_pipeline`과 두 요청을 전달하고,
`t70_shader_free_pipeline`으로 두 단계의 메모리를 함께 해제한다.
DXBC의 semantic 이름·번호로 대응시키고, 서로 다른 레지스터와 xy/zw 성분
배치도 옮긴다. 소비하지 않는 일반 출력은 제거한다. 형식이 다르거나 정점
출력의 성분 수가 부족하거나 대응 출력이 없으면 결과를 바꾸지 않고 실패한다.

Windows가 넘기는 명령어는 `t70_shader_compile_tokens`와
`t70_shader_compile_token_pipeline`으로 받는다. 버전·명령어 수를 포함한
토큰 길이를 검사하고, 실제 입력/출력 선언에서 ISGN/OSGN을 복원해 vkd3d로
DXBC 체크섬을 만든다. DDI의 전체 입력/출력 목록에 사용하지 않는 항목이 있어도
일반 값은 실제 선언만 컴파일한다. clip/cull 거리 배열은 정점 출력의 전체 배치를
유지해 픽셀 입력이 일부만 읽어도 같은 성분을 가리키게 한다. 이미 정해진
레지스터와 xyzw 위치는 그대로 보존한다.

DDI 목록에는 자료형이 없다. `t70_shader_io_types`의 `vertex_uint/sint`는
입력 배치의 레지스터별 정수 형식이고 `target_uint/sint`는 출력 대상별 형식이다.
각 비트가 모두 0이면 실수/정규화 형식이다. 실제 그리기에는 입력 배치·출력 자원의
형식과 맞춰야 한다. 정수 입력을 실수로 가정하면 GPU attribute 읽기 명령이
달라지는 것을 실제 생성 코드로 확인했고, 형식별로 컴파일하도록 수정했다.
단계 사이의 일반 값은 32비트 성분 위치와 보간 선언으로 연결한다.
깊이·coverage·시스템 값은 일반 정점 전달 값과 구분한다.

픽셀 입력의 레지스터·성분 위치에 맞춘 정점 출력에서 메모리 배치를 한 번 만들고
두 단계에 같은 배치를 사용한다. `varying_stride`는 전달 영역의 크기이며,
`noperspective_mask`는 매 그리기 때 정점 FAU의 10번 값에 넣는다.
반환한 SPIR-V는 NIR 연결 전의 원래 단계 변환본이고, 연결 결과는 Valhall 코드와
배치 정보에 반영된다. 단일 단계 API를 따로 호출한 결과를 연결된 쌍으로 쓰면 안 된다.

출력에는 Valhall 코드와 SPIR-V, 레지스터·TLS·WLS 크기, FAU 상수와
사용 영역, 정점 입력 마스크, varying 형식/배치, IDVS의 보조 코드 위치,
픽셀 discard·깊이·coverage 표시와 blend 반환 위치, FP32/FP16의 FTZ 표시가 포함된다.
FTZ 표시는 기존 `flags`의 새 비트에 넣어 구조체 크기와 ABI 버전 1을 유지한다.
실제 SPIR-V의 FP32 실행 모드와 이 비트를 대조하며, 실제 컴파일 결과의 코드
진입 위치·레지스터·preload를 arch-9 shader program 생성자에도 전달해 검사한다.
코드를 만든 것만으로 GPU 작업이나 화면이 만들어지는 것은 아니다.

설명 정보 표 번호는 다음과 같다.

| 표 | 저장할 자원 |
| --- | --- |
| 0 | D3D 상수 버퍼 cb0..cb13 |
| 1 | 텍스처/버퍼 t0..t127 |
| 2 | 샘플러 s0..s15와 Load/크기 조회용 nearest-clamp 샘플러 16 |
| 3 | 원래 D3D 정점 입력 레지스터 번호의 attribute 설명 정보 |

정점 셰이더의 FAU 앞 12개 32비트 값은 매 그리기 때 채워야 한다.
0..2는 viewport scale, 4..6은 offset, 8은 first vertex,
9는 base instance, 10은 noperspective varying 마스크이며 나머지는 여유 공간이다.
viewport나 정점 시작 값을 컴파일 때 고정하지 않는다.

## 프레임버퍼 LOAD_IMAGE 셰이더

`t70_shader_compile_framebuffer_preload`는 Mesa의 `pan_get_fb_shader_v9`로
단일 RGBA8/BGRA8/BGRX8 UNORM 대상의 단일 샘플 `LOAD_IMAGE` 픽셀 셰이더를
만든다. `array_layers=1`은 2D, 2..256은 2D 배열 대상이다. 반환값은 같은
`t70_shader`이며 SPIR-V 포인터는 비어 있고, 코드는 `t70_shader_free`로 해제한다.
실패하면 전달한 출력 구조체를 바꾸지 않는다.

셰이더는 자원 표 0의 텍스처 1과 샘플러 0을 읽는다. UMD는 이 슬롯에 해당
프레임버퍼 뷰를 바인딩해야 한다. FAU의 첫 두 32비트 단어에
`t70_shader_fb_preload_sysvals`를 넣는다. 좌표는 Mesa와 같이 최대 x/y를 포함한다.
반환된 기존 D3D `texture_mask`/`sampler_mask`는 이 내부 고정 슬롯을 나타내지 않는다.
배열 대상에서는 현재 레이어 인덱스(0..array_layers-1)를 frame argument의
하위 8비트로 전달한다. 색상 형식, 샘플 수, 배열 길이를 호출 전에 확인하며
다른 형식과 다중 샘플은 거부한다.

이 API는 셰이더 코드 생성만 제공한다. 프레임버퍼 디스크립터 생성, RX 코드
할당·수명, 자원 바인딩, 제출은 UMD 책임이다.

잘못된 컨테이너/단계/ABI, 1MiB 초과 입력, 지원하지 않는 GPU,
자원 배열·register space·UAV·중복/범위 밖 자원, FP64 연산은 거부한다.
현재 범위는 아래 단일 셰이더 20종, DXBC 단계 연결 8종과 DDI 단계 연결 10종의 실제 변환으로 확인했다.

## 빌드와 검사

소스는 builder의 기존 `mesa`·`vkd3d` 한 벌을 사용한다.
중간 산출물을 정리할 때 `Build/MaliShader/zig-cache`와 `zig-cache-local`은 도구 캐시로 보존한다. 캐시 안의 객체만 지우면 기록이 남아 다음 컴파일·링크가 실패할 수 있다.
Mesa는 `58f2c55854bd48b18df756edb11f187af9cd2fde`,
vkd3d 2.1은 `fb7f9ff3c1f8d972ee6f1b8c6a92caf6c8ba1e10`에 고정한다.
vkd3d 릴리스의 생성 입력 26개는
`vkd3d/build-t70pro-shader/generated-inputs.json`의 해시로 확인한다.
새 소스 사본이나 전역 설치를 만들지 않는다.

Mu-Silicium 루트에서 실행한다. 두 도구 모두 공용 빌드 잠금과 작업 6개
상한을 적용한다. 기존 호스트 Python의 Mako/Ply, Meson/Ninja/GNU make,
프로젝트의 Bison 3.8.2와 Zig 0.15.2, 기존 Windows SDK를 사용한다.

```sh
PYTHONDONTWRITEBYTECODE=1 python3 Windows/MtkMali/Shader/build.py
PYTHONDONTWRITEBYTECODE=1 python3 Windows/MtkMali/Shader/build-windows.py
```

첫 명령은 의존 라이브러리를 필요한 만큼 다시 만들고 호스트 검사 프로그램을 실행한다.
브리지와 검사 C/C++에는 ASan/UBSan을 적용한다. Mesa/vkd3d 의존 라이브러리
전체에 해당 검사를 적용한 것은 아니다. 정점 입력·행렬/상수 버퍼·보간 입력,
discard/미분, Sample/Load/SampleLevel, 동적 반복, 크기 조회, cube,
VertexID/InstanceID, 깊이·coverage 출력, clip 거리, 정수 입력/출력 등 20종을 검사한다. 반복 생성의 코드/SPIR-V/FAU가
같은지 확인하고, 실제 호스트 스레드 4개에서 32회 동시에 컴파일한다.
단계 연결은 남는 출력·순서 변경·한 레지스터 안의 성분 이동·정수 보간·
centroid/noperspective/sample 보간·위치/앞면 시스템 값을 포함한 8종을 검사한다.
실제 변환된 NIR 상수의 값과 목적 성분을 독립된 값과 대조하고, 기준 픽셀 코드와
공통 배치를 확인한다. 별도 스레드 4개에서 연결 쌍도 32회 동시에 컴파일한다.
동일한 20종을 DDI 토큰으로도 컴파일해 실제 Mali 코드·FAU·출력 형식이
원래 DXBC 결과와 같은지 확인했다. DDI 연결 10종에는 정수/실수 보간, 시스템 값,
정점 전체 출력의 zw 성분만 읽는 픽셀 입력을 포함한다. 마지막 경우는 실제 NIR의
상수 값과 목적 성분을 확인한다. clip/cull 배열의 뒤쪽 4개 성분만 읽는
두 경우도 실제 SPIR-V 전체 배치와 NIR의 살아 있는 입력 성분으로 확인한다.
DDI 쌍도 32회 동시에 컴파일한다.
총 3,710개 셰이더 검사와 비트 위치 검사 300개가 통과했다.
고정한 HLSL 생성기가 sample 수식어를 받지 못해, 해당 검사는 SM4.1 선언 토큰을
구성하고 vkd3d의 DXBC 직렬화로 체크섬을 만든 실제 입력을 사용한다.

vkd3d의 `LINEAR_NOPERSPECTIVE_CENTROID` 변환 누락도 수정했다.
NoPerspective와 Centroid 두 SPIR-V 지정이 함께 남는지 검사하며,
수정 파일 해시는 두 검사 기록의 `vkd3d_tracked_change_sha256`에 보존한다.

둘째 명령의 산출물은 `Build/MaliShader/windows/MtkMaliShader.dll`과
`vkd3d-shader.dll`이다. 두 DLL의 ARM64 PE, 공개 함수·가져오는 함수,
ASLR·NX·high-entropy VA와 쓰기/실행 겸용 구역이 없는 것을 검사한다.
실제 ARM64 PE의 명령으로 텍스처 플래그와 내부 64비트 인덱스의 비트 위치
300개를 Unicorn에서 검사한다. 이 검사는 DLL 본문이나 Windows를 실행하지 않는다.
vkd3d의 자동 LTO는 Zig의 Windows 링크와 맞지 않아 끄고 실제 COFF 객체로 연결한다.

Windows에서 bool/enum 비트필드가 나뉘어 저장되던 Mesa 내부 형식은
Windows 조건에서 같은 32비트 저장 단위를 사용하도록 수정했다.
공통 OS 구조체의 배치 규칙은 바꾸지 않는다. Linux PanVK는 같은 Mesa 소스에서
별도로 전체 재빌드·연결하며 `analysis/panvk-build.json`에 기록한다.

검증 뒤 객체·정적 라이브러리·configure 전문 로그는 지우고 현재 실행 파일/DLL과 생성 설정을 보존한다. 다음 빌드는 필요한 중간 파일을 다시 만든다.

검사 기록은 프로젝트의 `analysis/mali-shader-verification.json`과
`analysis/mali-shader-windows.json`이다. 입력 소스·의존 라이브러리·산출물의
해시와 실행 범위를 따로 기록한다.

## 다음 연결 대상

`Umd.c`에서 셰이더 생성·선택·해제와 선택한 VS/PS의 CPU 컴파일을 연결했다.
실제 ARM64 `MtkMaliUmd.dll`이 이 컴파일러의 새 API를 가져오는 것도 확인한다.
`MtkMaliUmdUploadBoundShaders`가 실제 코드 복사·읽기/실행 GPU 주소와 정점/보간/픽셀 프로그램의 읽기 전용·실행 금지 설명 정보를 만든다. 캐시/상주/주소 준비 대기와 안전한 반납도 연결했다.
`MtkMaliUmdSubmitRaster`로 내부 프로그램의 점/비점 정점·보간·픽셀 주소와 FAU 개수를 검증하고 정점/fragment 두 단계 명령을 조립·제출한다. 제출한 프로그램은 완료 이벤트까지 보존한다.
자원·샘플러 설명 정보 생성, 동적 FAU 공급과 tiler/프레임 설명 정보 변환, D3D Draw 상태·Present와
OpenAdapter/CreateDevice·필수 DDI 등록이 남아 있다.
DXIL/D3D12·geometry/tessellation/compute 컴파일도 현재 API 범위에 포함하지 않는다.
Windows DLL 실행, 실제 GPU 실행과 픽셀 대조는 미실행이다.

vkd3d-shader는 LGPL-2.1-or-later이고 별도 DLL로 연결한다.
원본의 `vkd3d/COPYING`·`vkd3d/LICENSE`와 생성 입력 출처 기록을 유지한다.
Mesa의 라이선스·출처도 기존 소스에서 보존한다.
