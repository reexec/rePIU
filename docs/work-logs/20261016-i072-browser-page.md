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
