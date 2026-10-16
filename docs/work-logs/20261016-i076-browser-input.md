# 작업 로그: 브라우저 호스트 단계 4 단위 2 — 페이지의 키보드와 게임패드 (issue #76)

설계: [20261016-i076](../design/20261016-i076-browser-input.md) ·
작업 지시: [20261016-i076](../work-orders/20261016-i076-browser-input.md)

## 2026-10-16 — 구현과 확인

브랜치 `task/i076-browser-input`(main `33b6dc9`, v0.0.224에서). 계기는 사용자의 "웹에서도 기존 키배치가 그대로 작동할까? F5 는
누르니까 리프레시 됐어"입니다.

### 구현

| 부분 | 내용 |
|---|---|
| 이름 표 | `ScancodeForBrowserKeyCode`(`include/repiu/input/browser_key_codes.h`): `KeyboardEvent.code` → SDL scancode. 글자·숫자·F1~F24·키패드 숫자는 계산, 나머지 66개는 표. 모르는 이름은 `SDL_SCANCODE_UNKNOWN` |
| 플랫폼 경계 | `BrowserInputEvent`와 `TakeBrowserInputEvent`(`browser_host.h`). 웹은 `Module.repiuInput`에서 하나씩 꺼냄(`EM_JS`), 네이티브는 늘 거짓 |
| 다리 | `BrowserInputBridge`(`src/engine/input/`): 키 → `SDL_PushEvent`(시각은 페이지가 본 때로 되돌림), 초점 잃음 → `SDL_EVENT_WINDOW_FOCUS_LOST`, 패드 → SDL 가상 게임패드(표준 매핑 표, 트리거는 축). 백엔드가 이벤트 폴링 직전에 `Pump()` |
| 페이지 | `page_input.js`: keydown/keyup에서 Ctrl/Meta 조합·F11·F12를 뺀 키의 기본 동작을 막고 보냄, blur·숨김에 focus-lost, 표준 매핑 패드를 프레임마다 보고 바뀐 것만 보냄. `worker.js`는 최대 1024개 큐. `main.js`는 시작 뒤 Start 버튼 초점을 빼고 입력을 시작, 끝나면 멈춤. `index.html`에 키 안내 |
| probe | `browser_key_code`: 범위, 이름 붙은 키, 기본 배치의 모든 키, 모르는 이름 |

### 설계의 "확인 필요"에 대한 답

| 항목 | 결과 |
|---|---|
| F5·Tab 기본 동작을 막을 수 있는지 | headless Edge에서 F5 두 번 뒤에도 페이지가 그대로(`page kept: True`) |
| 밀어 넣은 키가 JAMMA 입력까지 가는지 | 코드로 확인. 백엔드는 키 이벤트의 keycode로 JAMMA 입력 시간표에 edge를 기록하고, 포트 읽기는 시간표를 먼저 봅니다(Task 753, 입력 스크립트가 같은 길로 크레딧·곡 선택까지 감). `SDL_GetKeyboardState`만 보는 곳은 시간표가 없을 때의 대체 길과, 패드를 놓을 때 키보드가 같은 입력을 쥐었는지 보는 곳뿐 |
| Worker에서 `SDL_INIT_GAMEPAD`, 가상 게임패드의 버튼 번호 | **확인 못 함.** 아래 브라우저 확인이 막혀 게임까지 가지 못했습니다. 확인할 probe를 써 두었으나 넣지 않았습니다(아래 "남은 것") |

### 브라우저 확인이 막힌 것

headless Edge(DevTools 프로토콜로 키 입력)로 게임까지 가서 F5·Tab을 확인하려 했으나, 이 PC에서 **브라우저의 파일 읽기가 매우
느려져** CHD 풀기가 4~15%에서 멈췄습니다(오전 10:13에는 같은 하네스로 74초에 82%).

* main의 페이지 파일, 오전 09:30의 #72 빌드(그때 4분에 타이틀까지 확인한 것)로도 같게 멈춤 → 이 변경과 무관.
* Worker를 디버거로 멈추면 매번 `ChdCdImage::ReadRawSector` → `fread` → WORKERFS `read`(FileReaderSync) 안이고, 멈추기까지 4~11초.
  같은 파일을 네이티브로 읽으면 165 MB에 0.48초.
* WSL의 Playwright Chromium(`/mnt/c` 경유)도 같게 느림. 메모리(WSL 종료 뒤 여유 2.5 GB)나 남은 Edge 프로세스 탓도 아님.
* 같은 시간에 다른 프로젝트의 MSBuild 병렬 빌드가 CPU를 100% 쓰고 있었습니다. 원인일 수 있으나 확정하지 못했습니다.

그래서 게임 안의 동전·OSD 반응과 게임패드는 **사용자 브라우저에서 확인할 것**으로 남깁니다.

### 회귀

| 구성 | 결과 |
|---|---|
| Win32 `direct` Release | 빌드, core probe 45/45(`browser_key_code_all=true`), pumpitea 40초 정상 정지 |
| Linux x64 Release (WSL) | 빌드, core probe 47/47 |
| Linux i386 Release (WSL) | 빌드, core probe 45/45 |
| 웹 `repiu_browser`, `repiu_core_probe`(Node) | 빌드, core probe 37/37 |
| 웹 `repiu`(Node) | 빌드 |
| 페이지 JS | `node --check` 통과 |

### 따로 찾은 것 — Win32의 첫 텍스처 업로드 GL 오류

Win32 `direct`에서 pumpitea의 첫 `grTexDownloadMipMapLevel`(glide gate #51)이 `GLIDE_BACKEND_FAILURE ...
texture-download-backend-failure`로 기록됩니다(`glTexImage2D` 뒤 `glGetError()`가 오류). 40초 실행에서 이 브랜치 3번 중 2번,
**이 변경 없는 HEAD(main 코드) 3번 중 2번**, v0.0.219 2번 중 0번. #76과 무관하며, `glGetError`가 앞선 다른 호출의 오류를 돌려주는
것일 수 있습니다. 별도 issue로 다룹니다.

### 남은 것

* 사용자 브라우저에서 동전(F5)·OSD(Tab)·P1/P2 키와 게임패드 확인. 완료 기준의 "사용자 확인"이 아직입니다.
* 다리 probe: 키 이벤트(scancode·keycode·수정 키·시각), 모르는 이름, 초점 잃음, 가상 게임패드의 버튼·축 읽기와 떼기, 기본 JAMMA
  배치의 모든 키가 keycode로 자기 입력을 찾는지. Node(창 없는 곳, Worker와 같음)에서 게임패드 하위 시스템이 뜨는지도 이것으로
  봅니다. 초안만 있고 빌드·실행하지 않아 넣지 않았습니다.
* OSD가 열린 동안 ImGui가 키를 받으려면 창 ID가 맞아야 하는데, 브라우저에는 SDL 창이 없어 `windowID` 0으로 보냅니다. OSD 토글(Tab)은
  그 전에 처리되어 상관없습니다.
* 단계 4 단위 2의 소리(AudioWorklet).

## 2026-10-17 — 후속: 브라우저의 OSD와 마우스

브랜치 `task/i076-browser-osd`(main `fca6380`, v0.0.227에서). 사용자 확인에서 Tab이 OSD를 열지 않았습니다. 원인은 입력이 아니라 OSD가
만들어지지 않은 것이었습니다(SDL 창이 없어 "OSD needs a window"). 설계의 후속 결정 6·7.

| 부분 | 내용 |
|---|---|
| `GlideOsd` | SDL 창이 없으면 ImGui SDL3 백엔드를 창 없이 초기화(SDL 비디오가 없으면 dummy로 올림), 매 프레임 화면 크기를 캔버스 drawable 크기로(`SetDisplaySize`, 배율 1) |
| 백엔드 | `ApplyDrawableViewport`에서 OSD에 drawable 크기를 알림 |
| 플랫폼·브리지 | 입력 종류에 마우스 이동·버튼·휠, 브리지가 창 ID 0의 SDL 마우스 이벤트로 넣음 |
| 페이지 | 캔버스 위의 마우스를 그리기 화소 좌표로 보냄(오른쪽 클릭 메뉴와 휠 스크롤은 막음) |
| 페이지(소리) | `AudioContext.resume()`을 기다리지 않음. 브라우저가 소리를 허락하지 않으면 끝나지 않아 게임 시작까지 막혔음(headless에서 자동 재생 플래그 없이 확인). 다음 클릭·키에 다시 시도 |

**확인(headless Chrome):** Tab에 `[repiu-pad] OSD open`, 캡처에 OSD(렌더러 WebGL 2.0, 옵션, 셰이더). OSD가 열린 채 "Keep aspect ratio"
체크박스를 클릭하자 `[repiu-glide] keep aspect ratio off`. 자동 재생 플래그 없이도 게임이 시작됨.

**회귀:** Win32 Release core probe·`--opengl-post-shader`(네이티브 OSD 경로는 창이 있어 바뀌지 않음), 웹 core probe. Win32 Debug·`emulated`,
Linux x64·i386 모두 빌드와 core probe 통과.

---

# Work log: browser host phase 4 unit 2 — keyboard and gamepads on the page (issue #76)

Design: [20261016-i076](../design/20261016-i076-browser-input.md) ·
Work order: [20261016-i076](../work-orders/20261016-i076-browser-input.md)

## 2026-10-16 — implementation and checks

Branch `task/i076-browser-input` (from main `33b6dc9`, v0.0.224). It started from the user's "Will the existing key layout
work on the web too? Pressing F5 reloaded the page".

### Implementation

| Part | What |
|---|---|
| Name table | `ScancodeForBrowserKeyCode` (`include/repiu/input/browser_key_codes.h`): `KeyboardEvent.code` → SDL scancode. Letters, digits, F1–F24 and keypad digits are computed, the other 66 come from a table. An unknown name gives `SDL_SCANCODE_UNKNOWN` |
| Platform boundary | `BrowserInputEvent` and `TakeBrowserInputEvent` (`browser_host.h`). The web takes one at a time from `Module.repiuInput` (`EM_JS`); natively it is always false |
| Bridge | `BrowserInputBridge` (`src/engine/input/`): a key → `SDL_PushEvent` (its time put back to when the page saw it), lost focus → `SDL_EVENT_WINDOW_FOCUS_LOST`, a pad → an SDL virtual gamepad (the standard-mapping table, triggers as axes). The backend calls `Pump()` right before it polls events |
| Page | `page_input.js`: on keydown/keyup it stops the default of every key but Ctrl/Meta combinations, F11 and F12 and sends it; blur and hiding send focus-lost; standard-mapping pads are read every frame and sent when they change. `worker.js` keeps a queue of at most 1024. `main.js` takes the focus off Start once started, starts the input and stops it at the end. `index.html` shows the keys |
| Probe | `browser_key_code`: the ranges, the named keys, every key of the default layout, unknown names |

### Answers to the design's "to confirm"

| Item | Result |
|---|---|
| Can the page stop F5's and Tab's defaults | In headless Edge the page stays after F5 twice (`page kept: True`) |
| Do pushed keys reach the JAMMA input | Confirmed in the code. The backend records edges in the JAMMA input timeline by the key event's keycode, and port reads look at the timeline first (Task 753; the input script reaches credits and a song this way). Only the timeline-less fallback, and the check whether the keyboard holds an input a pad lets go of, read `SDL_GetKeyboardState` alone |
| `SDL_INIT_GAMEPAD` in a Worker, the virtual gamepad's button numbers | **Not confirmed.** The browser check below was blocked before the game. A probe for it is drafted but not added ("Left" below) |

### What blocked the browser check

The plan was to reach the game in headless Edge (keys over the DevTools protocol) and check F5 and Tab, but on this PC **the
browser's file reads had become very slow**, and the CHD unpack stalled at 4–15% (at 10:13 the same harness reached 82% in 74 s).

* main's page files, and the #72 build from 09:30 (which reached its title in 4 minutes then), stall the same way → not this change.
* Pausing the Worker in the debugger lands every time in `ChdCdImage::ReadRawSector` → `fread` → WORKERFS `read`
  (FileReaderSync), and takes 4–11 s to land. Reading the same file natively takes 0.48 s for 165 MB.
* Playwright's Chromium in WSL (through `/mnt/c`) is as slow. Neither memory (2.5 GB free with WSL stopped) nor leftover Edge
  processes explain it.
* Meanwhile another project's parallel MSBuild build held the CPU at 100%. It may be the cause; that was not settled.

So the coin and OSD reacting in the game, and the gamepads, are **left to check in the user's browser**.

### Regression

| Configuration | Result |
|---|---|
| Win32 `direct` Release | builds, core probe 45/45 (`browser_key_code_all=true`), pumpitea stops normally after 40 s |
| Linux x64 Release (WSL) | builds, core probe 47/47 |
| Linux i386 Release (WSL) | builds, core probe 45/45 |
| Web `repiu_browser`, `repiu_core_probe` (Node) | build, core probe 37/37 |
| Web `repiu` (Node) | builds |
| Page JS | `node --check` passes |

### Found on the way — a GL error at Win32's first texture upload

On Win32 `direct`, pumpitea's first `grTexDownloadMipMapLevel` (glide gate #51) is recorded as `GLIDE_BACKEND_FAILURE ...
texture-download-backend-failure` (`glGetError()` reports an error after `glTexImage2D`). In 40 s runs: 2 of 3 on this branch,
**2 of 3 on HEAD without this change (main's code)**, 0 of 2 on v0.0.219. It is unrelated to #76, and `glGetError` may be
returning an error left by an earlier call. It gets its own issue.

### Left

* Checking the coin (F5), the OSD (Tab), the P1/P2 keys and gamepads in the user's browser. The completion criterion's "user
  check" is still open.
* A bridge probe: the key event (scancode, keycode, modifiers, time), an unknown name, lost focus, a virtual gamepad's buttons
  and axes read back and detached, and every key of the default JAMMA layout finding its own input by keycode. Under Node (no
  window, as in the Worker) it also shows whether the gamepad subsystem starts. Only drafted, never built or run, so not added.
* For ImGui to take keys while the OSD is open the window ID has to match, but the browser has no SDL window, so keys go with
  `windowID` 0. The OSD toggle (Tab) is handled before that and is unaffected.
* Phase 4 unit 2's sound (AudioWorklet).

## 2026-10-17 — follow-up: the OSD and the mouse in the browser

Branch `task/i076-browser-osd` (main `fca6380`, from v0.0.227). In the user's check Tab did not open the OSD. The cause
was not the input: the OSD was never made (no SDL window, "OSD needs a window"). The design's follow-up decisions 6 and 7.

| Part | Content |
|---|---|
| `GlideOsd` | with no SDL window, ImGui's SDL3 backend is initialised with none (SDL's dummy video brought up if absent), and every frame the display size is the canvas's drawable size (`SetDisplaySize`, scale 1) |
| Backend | `ApplyDrawableViewport` tells the OSD the drawable size |
| Platform and bridge | mouse move, button and wheel input kinds; the bridge pushes them as SDL mouse events with window id 0 |
| Page | the mouse over the canvas goes in drawing pixels (the context menu and wheel scrolling are stopped) |
| Page (sound) | `AudioContext.resume()` is no longer awaited: when the browser holds sound back it never settles, which kept the game from starting (seen in headless without the autoplay flag). The next click or key tries again |

**Checked (headless Chrome):** Tab gives `[repiu-pad] OSD open` and the capture shows the OSD (renderer WebGL 2.0, options,
shaders). With the OSD open, clicking the "Keep aspect ratio" checkbox gives `[repiu-glide] keep aspect ratio off`. The
game starts without the autoplay flag.

**Regressions:** Win32 Release core probe and `--opengl-post-shader` (the native OSD has a window and does not change), web
core probe. Win32 Debug and `emulated`, and Linux x64 and i386: build and core probe pass.

