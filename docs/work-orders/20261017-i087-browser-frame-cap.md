# 작업 지시: 브라우저 빌드의 프레임을 60fps로 묶기 (issue #87)

설계: [20261017-i087](../design/20261017-i087-browser-frame-cap.md)

1. `GlideHostWindow::DefersSwapWait()`(기본 거짓, 브라우저 창 참).
2. 백엔드: 맡기는 창이면 페이서를 켜고, `PaceSwapAfterPresent`는 잠들지 않고 기한을 기록(`TakeFrameHold`).
3. 플랫폼 `WaitInHostEventLoop(us)`: 웹은 타이머 후 MessageChannel 라운드, 그 밖은 아무것도 안 함.
4. `OneThreadHostLoop`: 기한이 있으면 `TurnDue` 참, `TakeTurn`에서 기다림(페이지 양보를 겸함). 끝 보고에 `frame-holds`.
5. 진단: 페이지 주소의 `REPIU_*` 매개변수를 엔진 환경 변수로(worker.js → node_pre.js).
6. 검증: headless Chrome `REPIU_GLIDE_FRAME_RATE_LOG=1`, Win32·Linux·Node 빌드와 core probe.
7. 문서: 작업 로그, 웹 README.

---

# Work order: hold the browser build's frames to 60 fps (issue #87)

Design: [20261017-i087](../design/20261017-i087-browser-frame-cap.md)

1. `GlideHostWindow::DefersSwapWait()` (false by default, true for the browser window).
2. The backend: for a window that defers, turn the pacer on; `PaceSwapAfterPresent` records the deadline instead of sleeping
   (`TakeFrameHold`).
3. The platform's `WaitInHostEventLoop(us)`: on the web a timer and then MessageChannel rounds; elsewhere nothing.
4. `OneThreadHostLoop`: `TurnDue` is true while a deadline is recorded, and `TakeTurn` waits (the wait doubles as the yield
   to the page). `frame-holds` in the closing report.
5. Diagnosis: `REPIU_*` parameters in the page's address become the engine's environment variables (worker.js →
   node_pre.js).
6. Verification: headless Chrome with `REPIU_GLIDE_FRAME_RATE_LOG=1`; Win32, Linux and Node builds and the core probe.
7. Documents: the work log, the web README.
