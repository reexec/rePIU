# 작업 로그: 브라우저 호스트 단계 4 단위 1 — Worker의 엔진이 WebGL2 캔버스에 그리기 (issue #72)

설계: [20261016-i072](../design/20261016-i072-browser-page.md) ·
작업 지시: [20261016-i072](../work-orders/20261016-i072-browser-page.md)

## 2026-10-16 — 구현과 검증

브랜치 `task/i072-browser-page`(main `3896d4c`, v0.0.222에서). 결정 세 가지는 시작할 때 사용자가 정했습니다: 단일 Worker 유지,
CHD에서 푼 게임 파일은 우선 메모리, 양보는 Asyncify(범위 제한).

### 구현

| 부분 | 내용 |
|---|---|
| `GlideHostWindow` | 백엔드의 SDL 창 일(열기·닫기, 문맥, drawable 크기, 스왑, 스왑 간격, 제목, 배율, 전체 화면, 주사율)을 인터페이스 뒤로. SDL 구현은 기존 코드를 옮김 |
| 브라우저 캔버스 구현 | OffscreenCanvas에 WebGL2 문맥(깊이·스텐실·AA 없음, 불투명)을 만들어 Emscripten GL에 등록. 스왑 없음, 스왑 간격은 "적용됨" |
| `browser_host.h` | 플랫폼 함수: 캔버스 유무·문맥·크기·GL 함수 조회·양보. Win32·Linux는 `browser_host_none.cpp` |
| GL 함수 조회 | `SetGlProcAddressLoader`: 창 구현이 정함. 브라우저는 `emscripten_webgl_get_proc_address` |
| 양보 | `OneThreadHostLoop::TakeTurn` 끝에서 화면 교체가 있었거나 16 ms가 지나면 `YieldToHostEventLoop()`. 웹은 `MessageChannel` 한 번을 `EM_ASYNC_JS`로 기다림 |
| 링크 타깃 `repiu_browser` | `ENVIRONMENT=worker`, `MODULARIZE`, `INVOKE_RUN=0`, WORKERFS, `ASYNCIFY` + `ASYNCIFY_IGNORE_INDIRECT` + `ASYNCIFY_ADD=['repiu::engine::RunExecutionThread*']`, `ASYNCIFY_STACK_SIZE=1 MB`, `--profiling-funcs`. 페이지 파일을 빌드 디렉터리로 복사 |
| 페이지 | `src/platform/web/page/`: `index.html`(캔버스 1280×960, 파일 선택, 로그), `main.js`(Worker 생성, 캔버스 넘김, 로그 보관 `window.repiuLogHistory`), `worker.js`(WORKERFS 마운트, `callMain`) |
| `node_pre.js` | `window`가 없는 곳(Node, Worker)에서 SDL 비디오 dummy, 오디오 없음 |

### 설계의 "확인 필요"에 대한 답

| 항목 | 결과 |
|---|---|
| `ASYNCIFY_ADD` | binaryen이 한 번만 불리는 `RunGuestOnThisThread`를 호출자에 인라인해, 간접 호출은 `RunExecutionThread` 안에 있음. Release 링크는 함수 이름을 지우므로 `--profiling-funcs`가 필요했고, 패턴에 괄호가 있으면 목록 파싱이 실패함(`RunExecutionThread*`로) |
| OffscreenCanvas 커밋 | `MessageChannel` 양보마다 커밋됨(headless Chrome 캡처에서 화면이 움직임) |
| WORKERFS 하위 디렉터리 | 이름의 `/`로 만들어짐(`pumpitea/010209_1821.chd`) |
| MEMFS에 풀기 | 736개 파일 428,909,991바이트, 약 55초 |
| 처리량 | 브라우저에서 시작 후 약 4분에 타이틀, 약 400초에 데모 플레이(풀기 55초 포함). 네이티브 `emulated`(타이틀 300초, 데모 355초)와 같은 범위 |

### 구현 중 고친 것

1. **GL 함수를 못 찾아 dummy:** 캔버스 문맥은 만들어졌지만 `GlApi`가 `SDL_GL_GetProcAddress`로 찾아 `glCreateShader` 없음 → dummy.
   Worker에는 SDL 비디오가 없기 때문. 조회 함수를 창 구현이 정하게 함.
2. **`function signature mismatch`:** SDL의 Emscripten dummy 오디오가 `setInterval`로 `dynCall('vi', ...)`를 부르는데 Worker에서 이
   호출이 시그니처 불일치로 실패, Worker가 멈춤. 단위 1은 소리가 범위 밖이므로 SDL 오디오 드라이버를 `none`으로 해 열지 않게 함.
   오디오 출력은 장치를 못 열면 무음으로 계속함(Node도 같게).

### 확인 방법(브라우저)

headless Chrome을 DevTools 프로토콜로 몰아 확인했습니다(scratchpad의 Python 스크립트: HTTP 서버, 파일 선택
`DOM.setFileInputFiles`, Start, 5초마다 캡처와 로그). 450초 실행에서 `[worker]` 오류 0, 타이틀과 데모 플레이 화면 캡처.

### 회귀

| 구성 | 결과 |
|---|---|
| Win32 `direct` Release | 장면 probe 4개 화소 일치, `--opengl-post-shader` 통과, core probe 통과, pumpitea 35초 장면 표본이 #69와 같음 |
| Win32 `direct` Debug | 빌드, 장면 probe 4개 화소 일치, `--opengl-post-shader` 통과, core probe 통과 |
| Win32 `emulated` Release | 빌드, core probe 통과, pumpitea 30초 3억 6,419만 단계로 정상 정지, 양보 0회 |
| Linux x64·i386 Release (WSL) | 빌드, core probe 통과 |
| 웹 Node(`repiu`) | core probe 통과, pumpitea 30초 3억 7,121만 단계(이 단위 전 3억 7,307만), 양보 0회 |

### 남은 것

* 단위 2: 입력(키보드·패드 → 메시지), 소리(PCM → AudioWorklet). SDL dummy 오디오의 시그니처 불일치는 단위 2에서 자체 경로로 대체.
* 단위 3: OPFS 영구 저장(풀기 55초를 매번 하지 않게), GitHub Pages 배포.
* 페이지가 보여 주는 처리량(초당 실행 단계) 표시.

## 2026-10-16 — 후속: 롬 고르기 (설계 결정 9)

브랜치 `task/i072-rom-picker`(main `4320931`, v0.0.223에서). 사용자 보고: ZIP과 CHD가 다른 폴더에 있어(`roms/pumpitea.zip`,
`roms/pumpitea/010209_1821.chd`) 파일 대화상자 한 번으로 둘을 함께 고를 수 없었습니다.

### 구현

* `page/rom_selection.js`: 폴더의 파일 목록에서 게임을 찾는 `findGamesInFolder`(롬 폴더 배치, 평평한 배치), 각각 고른 둘을 묶는
  `pairSeparateFiles`, Worker의 마운트 이름을 정하는 `mountBlobs`. 게임 이름은 ZIP 이름을 소문자로. 맨 위의 다른 ZIP도
  함께 마운트(부모 롬셋 대체 경로).
* `index.html`·`main.js`: 폴더 칸(`webkitdirectory`), ZIP 칸, CHD 칸, 게임이 여럿이면 목록. 마지막으로 고른 방법을 따르고,
  다른 방법의 칸은 비웁니다. 짝을 못 찾으면 이유를 붉은 글씨로.
* `worker.js`: 페이지가 넘긴 `{game, blobs}`를 그대로 마운트.
* CMake가 `rom_selection.js`를 함께 복사.

### 확인

| 확인 | 결과 |
|---|---|
| Node 시험 `rom_selection.test.js` | 11/11 (롬 폴더, 여러 게임, 평평한 배치, 평평한 배치에 ZIP 여럿, 게임 하위 폴더만, 짝 없는 ZIP, 대소문자, 너무 깊은 CHD, 각각 고르기, 마운트 이름, 게임 이름) |
| 이 PC의 실제 `roms/` | 16개 게임을 찾음. pumpitea의 마운트는 `pumpitea.zip`, `pumpite.zip`(부모), `pumpitea/010209_1821.chd`. `roms/pumpitea`만 고르면 "ZIP이 없다"는 안내 |
| 웹 빌드 | `repiu_browser`와 페이지 파일(`rom_selection.js` 포함)이 빌드 디렉터리에 나옴 |
| 헤드리스 Edge (DevTools 프로토콜) | 폴더 모드: `roms` 파일 7,907개를 6초에 받아 16개 게임 목록, pumpitea를 골라 시작하면 `pumpitea running`. 각각 모드: ZIP만 고르면 "Now pick the CHD.", 둘 다 고르면 pumpitea, 시작 |

**확인하지 못한 것:** 헤드리스 Edge에서는 시작 뒤 엔진 로그가 한 줄도 오지 않고 화면이 검었습니다. 이번 변경 전의 페이지
(`main`의 `index.html`·`main.js`·`worker.js`)도 같은 환경에서 똑같았으므로 이 실행 환경 탓이고 이번 변경 때문은 아닙니다.
게임 화면이 나오는지는 사용자 브라우저에서 확인할 일로 남깁니다.

## 2026-10-16 — 후속: 진행이 보이는 페이지 (설계 결정 10)

사용자 요청: 진행 상황을 알기 어려우니 출력 로그를 같은 페이지에서 보이게. 원인을 먼저 확인했습니다.

### 원인 (확인됨)

* Worker의 메시지는 페이지에 옵니다: 시험용 표시(`[diag] creating module`, `mounted [pumpitea.zip, pumpitea]`, `callMain
  begins`)가 모두 도착했습니다.
* 그런데 `callMain` 뒤 40~120초 동안 엔진 출력이 한 줄도 없었습니다. 로더는 `PreparePiuChdMount`가 CHD에서 게임 파일(736개,
  409 MB)을 다 푼 **뒤에야** 첫 줄을 찍기 때문입니다. 브라우저에서는 실행마다 풀고(결정 2), 헤드리스 Edge에서는 이것이 약
  100초 걸렸습니다. 앞 절의 "헤드리스 Edge에서 로그 0줄"도 이것이었습니다(엔진이 멈춘 것이 아님).

### 구현

* `PreparePiuChdMount`의 선택 인자 `PiuChdUnpackObserver`: 디렉터리 기록으로 전체를 먼저 세고(`CountTree`), 풀기 시작과 약
  16 MB마다 알립니다. 로더는 준비 시작 한 줄과, 풀기 시작(파일 수·MB)과 진행(%·파일·MB)을 로그로 찍습니다.
* 페이지: 넓은 화면(1100 px 이상)에서는 캔버스 오른쪽 열에 롬 선택·상태·진행 줄·엔진 출력, 좁은 화면에서는 아래로. 진행 줄은
  페이지 스레드에서 0.5초마다 "경과 시간, 줄 수, 마지막 줄". 엔진 출력은 끝에 있을 때만 따라 내려가고, 복사 버튼이 있습니다.

### 확인

| 확인 | 결과 |
|---|---|
| Win32 `direct` Release (로더, `repiu_exe_analyzer` 빌드) | 빈 캐시에서: 준비 줄, 풀기 시작(736개, 409 MB), 약 0.7초마다 진행 26줄, 21초 뒤 완료 |
| 웹 (`repiu`, `repiu_browser`) | 빌드. clang이 `string_view` → `const std::string&`를 거절해 값 복사로 고침(MSVC는 통과시켰음) |
| 헤드리스 Edge, 폴더 모드 | 시작 3초 안에 준비·풀기 시작 줄, 그 뒤 약 4초마다 진행 줄. 87초에 82% |
| 넓은 화면(1600×1000) 캡처 | 캔버스 오른쪽에 조작부, "0:52 elapsed ... Latest: ...", 엔진 출력 칸, 복사 버튼 |
| Node 시험 `rom_selection.test.js` | 11/11 |

### 사용자 확인 (2026-10-16)

사용자가 자신의 브라우저에서 새 페이지로 게임이 실행되는 것을 확인했습니다. 키보드 입력은 아직 게임에 가지 않습니다(단위 2).
F5(기본 배치의 COIN1)는 브라우저 새로고침이 됐습니다. 단위 2에서 페이지가 게임 키의 기본 동작을 막고 Worker로 넘깁니다.

---

# Work log: browser host phase 4 unit 1 — the engine in a Worker drawing to a WebGL2 canvas (issue #72)

Design: [20261016-i072](../design/20261016-i072-browser-page.md) ·
Work order: [20261016-i072](../work-orders/20261016-i072-browser-page.md)

## 2026-10-16 — implemented and verified

Branch `task/i072-browser-page` (main `3896d4c`, from v0.0.222). The user settled three decisions at the start: keep the
single Worker, unpacked game files in memory for now, yield through Asyncify with a limited scope.

### Implementation

| Part | Content |
|---|---|
| `GlideHostWindow` | the backend's SDL window work (open, close, context, drawable size, swap, swap interval, title, scale, fullscreen, refresh rate) behind an interface; the SDL implementation is the old code moved |
| Browser canvas implementation | a WebGL2 context on the OffscreenCanvas (no depth, stencil or AA, opaque) registered with Emscripten's GL; nothing to swap; the swap interval answers "applied" |
| `browser_host.h` | platform functions: whether there is a canvas, the context, the size, the GL lookup, the yield; Win32 and Linux use `browser_host_none.cpp` |
| GL lookup | `SetGlProcAddressLoader`, set by the window implementation; the browser uses `emscripten_webgl_get_proc_address` |
| Yield | at the end of `OneThreadHostLoop::TakeTurn`, once a picture was swapped or 16 ms have passed, `YieldToHostEventLoop()`; the web waits one `MessageChannel` round through `EM_ASYNC_JS` |
| Link target `repiu_browser` | `ENVIRONMENT=worker`, `MODULARIZE`, `INVOKE_RUN=0`, WORKERFS, `ASYNCIFY` + `ASYNCIFY_IGNORE_INDIRECT` + `ASYNCIFY_ADD=['repiu::engine::RunExecutionThread*']`, `ASYNCIFY_STACK_SIZE=1 MB`, `--profiling-funcs`; the page files are copied into the build directory |
| Page | `src/platform/web/page/`: `index.html` (a 1280×960 canvas, file picking, log), `main.js` (creates the Worker, hands over the canvas, keeps the log in `window.repiuLogHistory`), `worker.js` (WORKERFS mount, `callMain`) |
| `node_pre.js` | where there is no `window` (Node, a Worker): SDL video dummy, audio none |

### Answers to the design's "to be confirmed"

| Item | Result |
|---|---|
| `ASYNCIFY_ADD` | binaryen inlines `RunGuestOnThisThread`, called once, into its caller, so the indirect call sits in `RunExecutionThread`. A release link strips function names, so `--profiling-funcs` was needed, and a pattern with an unbalanced parenthesis fails list parsing (hence `RunExecutionThread*`) |
| OffscreenCanvas commits | committed at every `MessageChannel` yield (the picture moves across headless Chrome captures) |
| WORKERFS subdirectories | made from a `/` in a name (`pumpitea/010209_1821.chd`) |
| Unpacking into MEMFS | 736 files, 428,909,991 bytes, about 55 s |
| Throughput | in the browser the title appears about 4 minutes after start and demo play at about 400 s (the 55 s unpacking included), in the same range as native `emulated` (title at 300 s, demo at 355 s) |

### Fixed during implementation

1. **The GL functions were not found, so the backend fell back to its dummy:** the canvas context was made, but `GlApi`
   looked functions up with `SDL_GL_GetProcAddress` and found no `glCreateShader`, because a Worker has no SDL video. The
   window implementation now sets the lookup.
2. **`function signature mismatch`:** SDL's Emscripten dummy audio calls `dynCall('vi', ...)` from `setInterval`, and in a
   Worker that call failed with a signature mismatch and stopped the Worker. Sound is outside unit 1, so SDL's audio
   driver is `none` and never opens; the audio outputs carry on silent when they cannot open a device (Node likewise).

### How the browser was checked

Headless Chrome driven over the DevTools protocol (a Python script in the scratchpad: an HTTP server, file picking through
`DOM.setFileInputFiles`, Start, a capture and the log every 5 s). In a 450 s run there were no `[worker]` errors, and the
captures show the title and demo play.

### Regressions

| Configuration | Result |
|---|---|
| Win32 `direct` Release | the four probe scenes match pixel for pixel, `--opengl-post-shader` passes, the core probe passes, pumpitea's 35 s scene samples match #69's |
| Win32 `direct` Debug | build, the four probe scenes match pixel for pixel, `--opengl-post-shader` passes, the core probe passes |
| Win32 `emulated` Release | build, the core probe passes, pumpitea stops cleanly after 30 s at 364.19 million steps, no yields |
| Linux x64 and i386 Release (WSL) | build, core probe passes |
| Web under Node (`repiu`) | core probe passes, pumpitea for 30 s runs 371.21 million steps (373.07 million before this unit), no yields |

### Remaining

* Unit 2: input (keyboard and pad → messages) and sound (PCM → AudioWorklet), which also replaces SDL's dummy audio and its
  signature mismatch with a path of our own.
* Unit 3: OPFS persistence (so the 55 s unpack is not repeated every time) and GitHub Pages deployment.
* The page showing throughput (steps per second).

## 2026-10-16 — follow-up: picking the ROMs (design decision 9)

Branch `task/i072-rom-picker` (main `4320931`, from v0.0.223). Reported by the user: with the ZIP and the CHD in different
folders (`roms/pumpitea.zip`, `roms/pumpitea/010209_1821.chd`), one file dialog could not pick both.

### Implementation

* `page/rom_selection.js`: `findGamesInFolder` finds games in a folder's file list (the ROM-folder layout and the flat
  one), `pairSeparateFiles` pairs the two picked separately, and `mountBlobs` names the Worker's mount. The game is the
  ZIP's name in lower case. The other top-level ZIPs are mounted too (the parent-set fallback).
* `index.html` and `main.js`: a folder field (`webkitdirectory`), a ZIP field, a CHD field, and a list when there are
  several games. The way picked last is followed and the other way's fields are cleared; when no pair is found the reason
  shows in red.
* `worker.js`: mounts the page's `{game, blobs}` as given.
* CMake copies `rom_selection.js` too.

### Checked

| Check | Result |
|---|---|
| Node test `rom_selection.test.js` | 11/11 (ROM folder, several games, flat, flat with several ZIPs, the game's subfolder alone, an unpaired ZIP, letter case, a CHD too deep, separate picks, mount names, game names) |
| This PC's real `roms/` | 16 games found; pumpitea mounts `pumpitea.zip`, `pumpite.zip` (its parent) and `pumpitea/010209_1821.chd`; picking `roms/pumpitea` alone explains that there is no ZIP |
| Web build | `repiu_browser` and the page files (`rom_selection.js` included) come out in the build directory |
| Headless Edge (DevTools protocol) | folder mode: the 7,907 files of `roms` taken in 6 s, a list of 16 games, pumpitea picked and started gives `pumpitea running`. Separate mode: the ZIP alone gives "Now pick the CHD.", both give pumpitea, which starts |

**Not checked:** in headless Edge no engine log line arrived after starting and the canvas stayed black. The page from
before this change (`main`'s `index.html`, `main.js` and `worker.js`) did exactly the same there, so it is the
environment, not this change. Whether the game appears is left for the user's browser.

## 2026-10-16 — follow-up: a page that shows its progress (design decision 10)

Requested by the user: progress was hard to follow, so show the output log on the same page. The cause came first.

### Cause (confirmed)

* The Worker's messages do reach the page: test markers (`[diag] creating module`, `mounted [pumpitea.zip, pumpitea]`,
  `callMain begins`) all arrived.
* Yet for 40 to 120 s after `callMain` the engine printed nothing: the loader prints its first line only **after**
  `PreparePiuChdMount` has unpacked the game's files from the CHD (736 files, 409 MB). A browser unpacks on every run
  (decision 2), and in headless Edge that took about 100 s. The "no log line in headless Edge" of the section before was
  this too, not a stopped engine.

### Implementation

* `PreparePiuChdMount`'s optional `PiuChdUnpackObserver`: the total is counted from the directory records first
  (`CountTree`), then the unpack's start and about every 16 MB are reported. The loader logs a line when preparation
  starts, the unpack's start (files, MB) and its progress (percent, files, MB).
* The page: on a wide screen (1100 px and up) the ROM picking, the status, the activity line and the engine output sit
  in a column right of the canvas; on a narrow one, below it. The page's own thread updates the activity line every
  0.5 s with the elapsed time, the line count and the latest line. The output follows its end only while the reader is
  there, and a button copies it.

### Checked

| Check | Result |
|---|---|
| Win32 `direct` Release (the loader and `repiu_exe_analyzer` built) | from an empty cache: the preparing line, the unpack's start (736 files, 409 MB), 26 progress lines about 0.7 s apart, done after 21 s |
| Web (`repiu`, `repiu_browser`) | builds; clang refused `string_view` to `const std::string&`, fixed with a copy (MSVC had let it through) |
| Headless Edge, folder mode | the preparing and unpack-start lines within 3 s of starting, then progress lines about 4 s apart; 82% at 87 s |
| A wide (1600×1000) capture | the controls right of the canvas, "0:52 elapsed ... Latest: ...", the output box, the copy button |
| Node test `rom_selection.test.js` | 11/11 |

### Confirmed by the user (2026-10-16)

The user ran the game with the new page in their own browser. Keyboard input does not reach the game yet (unit 2): F5
(COIN1 in the default layout) reloaded the page. In unit 2 the page stops the browser's default for game keys and hands
them to the Worker.
