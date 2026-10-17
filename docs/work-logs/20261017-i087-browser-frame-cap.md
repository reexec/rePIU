# 작업 로그: 브라우저 빌드의 프레임을 60fps로 묶기 (issue #87)

설계: [20261017-i087](../design/20261017-i087-browser-frame-cap.md) ·
작업 지시: [20261017-i087](../work-orders/20261017-i087-browser-frame-cap.md)

## 2026-10-17 — 구현과 확인

브랜치 `task/i086-opfs-mount-cache`(#86에 이어서).

### 구현

| 파일 | 내용 |
|---|---|
| `glide_host_window.h`, `glide_host_window_browser.cpp` | `DefersSwapWait()`: 기본 거짓, 브라우저 창 참 |
| `glide_opengl_backend.{h,cpp}` | 맡기는 창이면 페이서를 60 Hz로 켬. `PaceSwapAfterPresent`는 잠들지 않고 기한을 기록, `TakeFrameHold`로 꺼냄. 기한 규칙(누적, 늦으면 안 붙잡음, 한 주기 넘게 늦으면 다시 잡음)은 네이티브와 같음 |
| `browser_host.h`, `web/browser_host.cpp`, `browser_host_none.cpp` | `WaitInHostEventLoop(us)`: 타이머(마지막 1 ms 전까지) 뒤 MessageChannel 라운드. 그 밖은 아무것도 안 함 |
| `host_turn.{h,cpp}` | 기한이 있으면 `TurnDue` 참, `TakeTurn`의 `HoldFrameIfDue`가 기다림(페이지 양보를 겸함). 끝 보고에 `frame-holds=횟수/ms` |
| `page/main.js`, `page/worker.js`, `node_pre.js` | 페이지 주소의 `REPIU_*` 매개변수를 엔진 환경 변수로 |

### 확인

headless Chrome, pumpitea 150초, 저장된 푼 파일 사용, `?REPIU_GLIDE_FRAME_RATE_LOG=1`. 비교용 이전 빌드(#86)는 같은 페이지
파일에 환경 변수 전달만 손으로 붙임.

| 장면 | 이전 fps | 지금 fps |
|---|---|---|
| 가벼운 장면(시작 직후, 데모 사이) | 111.9, 326.4, 136.1, 94.9 등 | 60.0 ± 0.3 (1초 창 경계에서 한 번 61.0) |
| 무거운 장면(데모) | 20~45 | 20~45 (같음) |

* 상한 아래의 장면은 느려지지 않았습니다. 로그에 `holding frames every 16667 us`.
* 오류·중단 없음.

| 회귀 | 결과 |
|---|---|
| Win32 `direct` Release | 빌드, core probe·glide render probe 통과 |
| Win32 Debug, `emulated` Release | core probe 통과 |
| Linux x64·i386 Release(WSL) | 빌드, core probe 통과 |
| 웹 Node | core probe 통과(같은 12개 건너뜀) |

네이티브 창은 `DefersSwapWait`가 거짓이라 페이서 경로가 그대로입니다.

### 남은 것

* 사용자 브라우저(실제 화면 주사율)에서 확인. 120 Hz 화면에서도 게임은 60fps.

---

# Work log: hold the browser build's frames to 60 fps (issue #87)

Design: [20261017-i087](../design/20261017-i087-browser-frame-cap.md) ·
Work order: [20261017-i087](../work-orders/20261017-i087-browser-frame-cap.md)

## 2026-10-17 — implementation and checks

Branch `task/i086-opfs-mount-cache` (continuing after #86).

### Implementation

| File | What |
|---|---|
| `glide_host_window.h`, `glide_host_window_browser.cpp` | `DefersSwapWait()`: false by default, true for the browser window |
| `glide_opengl_backend.{h,cpp}` | For a window that defers, the pacer runs at 60 Hz; `PaceSwapAfterPresent` records the deadline instead of sleeping, taken with `TakeFrameHold`. The deadline rules (accumulating, a late frame not held, a frame more than a period late resynchronised) are the native ones |
| `browser_host.h`, `web/browser_host.cpp`, `browser_host_none.cpp` | `WaitInHostEventLoop(us)`: a timer up to the last millisecond, then MessageChannel rounds; nothing elsewhere |
| `host_turn.{h,cpp}` | `TurnDue` is true while a deadline is recorded, and `HoldFrameIfDue` in `TakeTurn` waits (doubling as the yield to the page); `frame-holds=count/ms` in the closing report |
| `page/main.js`, `page/worker.js`, `node_pre.js` | `REPIU_*` parameters in the page's address become the engine's environment variables |

### Checks

Headless Chrome, pumpitea for 150 s, with the saved unpacked files, `?REPIU_GLIDE_FRAME_RATE_LOG=1`. The earlier build (#86)
for comparison ran with the same page files and the variable pass-through added by hand.

| Scene | fps before | fps now |
|---|---|---|
| Light (just after start, between demos) | 111.9, 326.4, 136.1, 94.9 and so on | 60.0 ± 0.3 (61.0 once, at a one-second window edge) |
| Heavy (the demo) | 20–45 | 20–45 (the same) |

* Scenes under the cap did not slow down. The log says `holding frames every 16667 us`.
* No errors or aborts.

| Regression | Result |
|---|---|
| Win32 `direct` Release | build; core probe and glide render probe pass |
| Win32 Debug, `emulated` Release | core probe passes |
| Linux x64 and i386 Release (WSL) | build, core probe passes |
| Web under Node | core probe passes (the same 12 skipped) |

A native window answers `DefersSwapWait` false, so its pacer path is unchanged.

### Remaining

* A check in the user's browser (at the real display rate). On a 120 Hz display the game still runs at 60 fps.
