# 작업 지시: 브라우저 호스트 단계 4 단위 2 — 페이지의 키보드·게임패드 입력 (issue #76)

설계: [20261016-i076](../design/20261016-i076-browser-input.md)

브랜치: `task/i076-browser-input`.

## 순서

1. **키 이름 표:** `src/input/browser_key_codes.{h,cpp}`(`KeyboardEvent.code` → SDL scancode)와 core probe.
2. **플랫폼:** `browser_host.h`에 `BrowserInputEvent`와 `TakeBrowserInputEvent`. 웹은 EM_JS로 `Module.repiuInput`에서 꺼냄,
   `browser_host_none.cpp`는 "없음".
3. **엔진:** `src/engine/input/browser_input_bridge.{h,cpp}`: 키 → `SDL_PushEvent`(시각 보정), 포커스 잃음 →
   `SDL_EVENT_WINDOW_FOCUS_LOST`, 패드 → SDL 가상 게임패드. `PumpEvents`에서 부름.
4. **Worker:** `{type: "input"}` 메시지를 `Module.repiuInput`에 쌓음.
5. **페이지:** 키 받기와 `preventDefault` 규칙, 포커스 잃음, Gamepad API 표본. 페이지에 짧은 키 안내.
6. **확인:** 설계의 검증 1~4.
7. **문서:** 작업 로그, `src/platform/web/README.md`, `ARCHITECTURE.md`, `docs/analysis/web-port-frontier.md`, `docs/TODO.md`.

## 완료 기준

* 브라우저에서 기존 키 배치로 조작되고 F5가 새로고침되지 않는다. 게임패드로 조작된다(사용자 확인).
* Win32·Linux 회귀가 없다.

---

# Work order: browser host phase 4 unit 2 — keyboard and gamepad input on the page (issue #76)

Design: [20261016-i076](../design/20261016-i076-browser-input.md)

Branch: `task/i076-browser-input`.

## Order

1. **The key name table:** `src/input/browser_key_codes.{h,cpp}` (`KeyboardEvent.code` → SDL scancode) and a core probe.
2. **Platform:** `BrowserInputEvent` and `TakeBrowserInputEvent` in `browser_host.h`; the web takes them from
   `Module.repiuInput` through EM_JS, and `browser_host_none.cpp` answers "none".
3. **Engine:** `src/engine/input/browser_input_bridge.{h,cpp}`: keys → `SDL_PushEvent` (with the time corrected), focus
   lost → `SDL_EVENT_WINDOW_FOCUS_LOST`, pads → SDL virtual gamepads; called from `PumpEvents`.
4. **Worker:** queues `{type: "input"}` messages in `Module.repiuInput`.
5. **Page:** taking keys and the `preventDefault` rule, focus lost, Gamepad API sampling; a short key guide on the page.
6. **Checks:** the design's verification 1 to 4.
7. **Documents:** the work log, `src/platform/web/README.md`, `ARCHITECTURE.md`, `docs/analysis/web-port-frontier.md`,
   `docs/TODO.md`.

## Done when

* In the browser the existing layout plays the game and F5 does not reload; a gamepad plays it (confirmed by the user).
* No Win32 or Linux regression.
