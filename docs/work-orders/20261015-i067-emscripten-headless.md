# 작업 지시: 브라우저 호스트 단계 2 — `emulated` 엔진의 Emscripten 빌드와 Node 헤드리스 실행 (issue #67)

설계: [20261015-i067](../design/20261015-i067-emscripten-headless.md)

브랜치: `web-emscripten-headless`. 빌드 트리: `build/web_wasm`(WSL, `~/emsdk`).

## 순서

1. **툴체인 확인:** emsdk 버전, Node 버전, 메모리 배치(`GLOBAL_BASE`, 스택 위치), `getenv`와 Node 환경 변수.
2. **CMake:** Emscripten의 실행 모델을 `emulated`로, 엔진 목록과 모델 파일을 웹에서도 빌드, rex86 FetchContent를 웹에서도,
   링크 옵션(`GLOBAL_BASE`, 메모리, `NODERAWFS`, `EXIT_RUNTIME`, 필요하면 `--pre-js`).
3. **웹 플랫폼 계층:** `virtual_memory.cpp`(예약기와 보호 표, 메모리 변경 알림), `host_cpu_entry_state.cpp`(WOW64 값,
   가짜 TEB, CPUID 표), `host_thread`(실패), `host_time` 등 Linux 구현을 그대로 쓸 수 있는 것은 그대로. README 갱신.
4. **엔진 컴파일:** wasm32에서 깨지는 곳을 고친다. x86 전용은 x86에서만(`_mm_pause` 등), 그 밖은 플랫폼 계층으로.
5. **빌드 스크립트:** `scripts/build_web_wasm.sh`가 `repiu`도 빌드.
6. **Node 실행:** `repiu_core_probe`, pumpitea 30초(dummy, 무음), 경계 trace를 네이티브 Win32 `emulated`와 대조.
7. **회귀:** Win32 `direct`·`emulated`, Linux i386·x64 빌드와 core probe.
8. **문서:** 작업 로그, `docs/analysis/web-port-frontier.md`, `ARCHITECTURE.md`(웹 실행 모델), `docs/TODO.md`,
   필요하면 `docs/kb/`(Emscripten 메모리 배치).

## 완료 기준

* Node에서 pumpitea가 `emulated`로 30초 돌고 정상 정지하며, 경계 trace가 네이티브와 첫 벽시계 의존까지 같다.
* Win32·Linux 회귀가 없다.

---

# Work order: browser host phase 2 — the Emscripten build of the `emulated` engine, run headless under Node (issue #67)

Design: [20261015-i067](../design/20261015-i067-emscripten-headless.md)

Branch: `web-emscripten-headless`. Build tree: `build/web_wasm` (WSL, `~/emsdk`).

## Order

1. **Toolchain check:** the emsdk and Node versions, the memory layout (`GLOBAL_BASE`, where the stack goes), `getenv`
   and Node's environment.
2. **CMake:** the Emscripten model becomes `emulated`; the engine list and the model files are built for the web; rex86
   through FetchContent on the web too; link options (`GLOBAL_BASE`, memory, `NODERAWFS`, `EXIT_RUNTIME`, a `--pre-js`
   if needed).
3. **The web platform layer:** `virtual_memory.cpp` (the reserver, the protection table, memory-change notifications),
   `host_cpu_entry_state.cpp` (WOW64 values, the stand-in TEB, the CPUID table), `host_thread` (failure), and the Linux
   implementations as they are where they hold, such as `host_time`. Update the README.
4. **Compiling the engine:** fix what breaks on wasm32: x86-only code stays on x86 (`_mm_pause` and the like), anything
   else moves to the platform layer.
5. **Build script:** `scripts/build_web_wasm.sh` builds `repiu` too.
6. **Node runs:** `repiu_core_probe`; pumpitea for 30 s (dummy, silent); the boundary trace compared against native
   Win32 `emulated`.
7. **Regressions:** Win32 `direct` and `emulated`, Linux i386 and x64: build and core probe.
8. **Documents:** the work log, `docs/analysis/web-port-frontier.md`, `ARCHITECTURE.md` (the web's execution model),
   `docs/TODO.md`, and `docs/kb/` if needed (Emscripten's memory layout).

## Done when

* pumpitea runs 30 s on `emulated` under Node, stops cleanly, and its boundary trace matches native up to the first
  wall-clock dependence.
* No Win32 or Linux regression.
