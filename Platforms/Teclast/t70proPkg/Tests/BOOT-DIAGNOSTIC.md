# T70 temporary boot diagnostic

The board boot policy prints a compact status on the **existing UEFI text
console** when it reaches `DeviceBootManagerUnableToBoot`, before trying the
recovery Shell. No graphics driver, filesystem search algorithm, persistent
storage, EFI variable, USB path, or device power configuration is changed.

Example:

```
T70 D1 I=0003 O=0001 F=0001 P=00000004 E=00010001
```

All values are hexadecimal:

- `D1`: diagnostic format version 1.
- `I`, `O`: last registered input/output console path counts. Registration does
  not prove physical input or visible output works.
- `F`: current SimpleFileSystem protocol handle count, saturated to `FFFF`.
  `0000` also covers a failed handle query. This does not mean a bootable EFI
  file exists, nor identify USB versus SD/UFS.
- `P`: setup application's last launch phase: 0 not attempted, 1 lookup begun,
  2 lookup failed, 3 image load begun, 4 image load failed, 5 image loaded,
  6 setup preparation failed, 7 application started, 8 application returned.
- `E`: existing packed setup status: low 16 bits of EFI_STATUS plus bit 16 set
  for an EFI error. It is **not the full EFI_STATUS**, and does not describe
  the subsequently attempted recovery Shell result.

## 사진용 빠른 해독표

모든 숫자는 **16진수**입니다. 사진에는 `T70 D1`부터 마지막 `E=...`까지
이어지는 줄을 전부 포함하세요.

| 코드 | 의미 | 해석 주의 |
|---|---|---|
| I / O | 마지막 입력 / 출력 콘솔 등록 수 | 실제 키·화면 동작 성공 여부가 아님 |
| F | 현재 파일시스템 프로토콜 수 | 0은 없음 또는 조회 실패, 부팅 파일 발견 여부가 아님 |
| P | 설정 앱 시작 단계 | 0 미시도, 2 조회 실패, 4 로드 실패, 6 준비 실패, 7 실행 중, 8 반환 |
| E | 설정 앱 상태 하위 16비트 + 오류 표시 비트 16 | `00010001`은 오류 표시 + 상태 1; Shell 결과가 아님 |

표시는 **BDS의 부팅 실패 복구 진입 후, 복구 Shell 탐색 전**입니다.
20 / 40 / 80열 모형에서 초기 빈 줄 포함 각각 최대 5 / 3 / 2줄입니다.
사진 한 장에 담을 수 있는 짧은 형식이지만, 실제 화면 높이·폰트·스크롤은
검증하지 않았습니다. 이후 Shell 출력이 이를 밀어낼 수 있습니다.

## Collection

The current console width is read with `QueryMode`. Text is split at spaces
where possible and uses at most columns minus one characters per line, avoiding
automatic wrapping at the last column. Missing mode/query support or a failed
query uses a conservative 20-column layout. A successful report of fewer than
two columns suppresses the diagnostic. Output failure stops the diagnostic,
not the boot recovery path. No console mode is changed.

After an authorized, separately prepared device boot, photograph the complete
`T70 D1` status (including its continuation lines) and send that image in the conversation. A photo avoids manually
copying long logs; it is not automatic remote collection. If it scrolls away,
this implementation does not retain a separate copy on disk.

The line requires BDS recovery to be reached and a functioning text console.
A blank screen, early boot failure, missing console, or OutputString failure
can prevent it appearing. Its absence alone does not identify the failure.
There is no QR reader, network endpoint, USB dependency, or tablet-side file
upload in this implementation. No persistent storage path is assumed available;
the existing UFS adapter remains read-only.

## Validation boundary

`DeviceBootManagerTest` exercises the actual output function with a host console
mock, including absent console, formatting, recursive output callback, output
failure, post-ExitBootServices suppression, and failed Shell lookup entry.
The static text does not contain user filenames, keys, serials, addresses, or
framebuffer contents. Only protocol counts and numeric setup state are shown.

This source change has **not** been included in a newly built firmware image or
verified on the tablet. It cannot change the behavior of an existing image.

## Normal setup menu (HII)

The T70 FrontPage also inserts six **read-only text** entries before the
existing menu choices: `T70 D1 (snapshot)` and one entry each for I/O/F/P/E.
These are rendered by the existing Form Browser after its screen clearing;
there is no console overlay, added input callback, wait, timer, or boot target
change. Each field is at most ten characters. HII controls wrapping/scrolling;
actual screen placement and narrow-mode rendering have not been device-tested.

The values are a snapshot of the existing validated RAM BDS record when the
form is updated, not live polling. In the normal menu, P=00000007 commonly means
setup was started and E=00000000 that its launch had no recorded error; this is
not proof the OS boots. If the record is absent/inconsistent, no diagnostic
entries are added. Resource exhaustion can omit the diagnostic or leave only
an initial subset of its text; photograph all five fields, and report missing
fields rather than infer zeros. Existing menu choices remain unchanged.

String tokens belong to the current HII package, are reused on form refresh,
and are forgotten when that package is removed. The helper only reads the RAM
record. It does not change stored diagnostics or configuration variables.
`test-hii-diagnostic.py` runs the actual helper with RAM/HII mocks and checks
its FrontPage wiring statically; it does not build or run the entire browser.

### Freshness boundary

The normal platform BDS entry calls `DeviceBootManagerBdsEntry` before console
connection and setup launch. It now invalidates the previous BDS RAM signature
first (when the T70 handoff region is identified), then attempts to publish
this boot's stage-1 record. Thus a failed current update cannot leave the prior
valid snapshot visible through this normal path. Subsequent stage updates
publish a new signature only after their fields are written.

The HII display is still a form-update snapshot, not a continuously refreshed
status or timestamp. Its P/E fields describe the setup launch, not an OS result.
There is no persistent boot identifier: launching UiApp outside the normal BDS
path or mixing older/newer firmware components is outside this freshness
assurance. No successful real-device boot is implied by a valid RAM signature.

## Larger console text

For T70 builds carrying `T70PRO_RAM_DIAGNOSTICS`, GraphicsConsole now uses
**16×38 pixel cells**, a nearest-neighbor 2× rendering of the existing 8×19
font, when the current GOP mode is at least 1280×950. This enlarges the
existing console and text-based HII menu, not just diagnostic line wrapping.
The font is rendered into an offscreen HII bitmap and copied to the existing
GOP Blt interface. No GPU driver, framebuffer mapping, hardware display mode,
power setting, or input callback is changed.

Cell counts, centering, line scroll, line-buffer allocation, and cursor pixels
use the same scale. Outputs smaller than that threshold retain the original
1× font to preserve an 80×25 console. In particular a 1200×2000 portrait mode
does **not** enlarge; the actual active orientation must be checked on device.
Existing stock-panel evidence (`display-lk-handoff.json`,
`stock_panel_and_drm_binary_review.mode`) records 2240×1400. Both that mode
and its 1400×2240 rotation qualify for 2× and are covered by the UI mock.
This is not a new reading of the currently running firmware GOP.
Other builds without the T70 define retain 1× rendering. Allocation, HII, or
GOP failures can omit text; no new retry or blocking wait is added.

`test-console-scale.py` compiles extracted actual mode-generation, scaled
drawing, cursor, OutputString, and SetMode functions with ASan/UBSan mocks. It checks
threshold modes, exact doubled pixels, wide-cell allocation, cursor erase,
real newline scrolling, allocation/HII/GOP failures, and builds with and
without the T70 define. SetMode failure injection verifies that the old buffer/mode survives failed
allocation or GOP transition, and that successful 2×/1× switches replace the
buffer exactly once. Initial failures, cursor read failure, best-effort cursor
restoration, and clear failures are covered. A GOP failure after partial
hardware changes cannot be repaired by this mock or guaranteed by the console.
The full browser, real HII glyphs, firmware build, and tablet visibility
have not been validated by this test. This still requires a separately
prepared firmware image and device verification; existing images do not change.

### Text and cursor boundary regression

The host test also executes the actual scaled/unscaled draw dispatch and
SetCursorPosition. Narrow, wide-marker and mixed strings at columns 77–79,
first/last rows, repeated full-line wrapping, and the largest valid text mode
are exercised. Mock GOP validates dimensions and pitch; ASan checks accesses
to allocated bitmap buffers. HII/GOP draw failures are injected through the
real OutputString path. Real glyph shapes and wide-Unicode font availability
remain untested.

A cursor-erasure failure now returns its error without changing the logical
position. If drawing the cursor at its new location fails after the move, the
new logical location remains set and the error is returned. Neither behavior
claims restoration of pixels after a partially failing GOP operation.

### QueryMode and HII layout contract

The host suite executes actual QueryMode and the selected CustomizedDisplayLib
`ScreenDimensionInfoValidate`, connected through the mocked ConOut. Every
generated valid mode is selected, queried, checked for at least 80×25 cells,
passed through HII dimension validation, and scrolled at its bottom row.
Reserved unsupported modes, out-of-range mode numbers, and requested HII
rectangles exceeding queried bounds are rejected. Footer fixtures with
0/3/6/9 hotkeys fit the tested layouts. This is not a full interactive menu test.

At 2×, stock landscape 2240×1400 has a full-width 140×36 text mode; portrait
1400×2240 has 87×58. Both retain mode 0 at 80×25. Portrait also supports mode 1
at 80×50; landscape does not have enough pixel height for that mode and reports
it unsupported. QueryMode, the selected mode, HII dimensions, and scrolling
remain consistent in these host checks. No new production defect was found.
