# 작업 로그: 브라우저 호스트 단계 2 — `emulated` 엔진의 Emscripten 빌드와 Node 헤드리스 실행 (issue #67)

설계: [20261015-i067](../design/20261015-i067-emscripten-headless.md) ·
작업 지시: [20261015-i067](../work-orders/20261015-i067-emscripten-headless.md)

## 2026-10-15 — 구현과 검증

브랜치 `web-emscripten-headless`(main `fe0d31e`, v0.0.220에서). 툴체인: WSL Ubuntu 24.04의 emsdk, Emscripten 6.0.8,
Node 24.19.0. rex86은 로컬 `../rex86` v0.0.34. 빌드 트리는 WSL 홈(`~/repiu-web`)에 두었습니다(`/mnt/c`보다 빠름).

### 설계의 "확인 필요"에 대한 답

| 항목 | 결과 |
|---|---|
| 메모리 배치 | `-sGLOBAL_BASE=0x10000000`이면 데이터 `0x10000260`, 스택 `0x1080095c`(8 MB 시험), 힙 `0x108013c0`. Emscripten은 `--no-stack-first`로 링크해 **스택이 데이터 뒤**에 옵니다. `0x04000000`에 바로 쓸 수 있음(확인됨, 작은 시험 프로그램) |
| 기본 스택 | **64 KB**(`STACK_SIZE`). 단일 스레드 루프에는 너무 작아 16 MB로 링크 |
| `getenv`와 Node 환경 | 기본은 Node의 `process.env`를 보지 않음. `-sNODE_HOST_ENV=1`이면 그대로 보임(확인됨). `--pre-js`는 SDL 드라이버 기본값에만 씀 |
| wasm32에서 컴파일되지 않는 엔진 코드 | **없었습니다.** 엔진 목록 전체가 첫 시도에 컴파일됐습니다. 고친 것은 `_mm_pause`를 x86에서만 쓰게 한 것 하나 |
| SDL3와 Node | Emscripten 비디오 드라이버가 초기화 중 `window.matchMedia`를 읽어 `ReferenceError: window is not defined`로 멈춤. Node에서는 dummy 비디오·오디오 드라이버를 기본으로 함(`src/platform/web/node_pre.js`). 그러면 Glide는 dummy, 오디오는 무음 |
| 게스트가 FS(TEB)를 읽는지 | 30초 실행에서 문제 없음(가짜 TEB는 0과 `Self`만) |
| libchdr와 `NODERAWFS` | 롬 ZIP과 CHD를 읽고 게임 파일을 준비함(확인됨) |
| 로더의 spdlog | 번들 fmt 대신 `SPDLOG_USE_STD_FORMAT`로 Emscripten에서 컴파일됨 |

### 구현

* **CMake:** 웹의 실행 모델을 `emulated`로. 엔진 목록과 모델 파일, ImGui 링크의 `NOT EMSCRIPTEN` 가드를 뗐습니다. 웹
  로더 타깃 `repiu`(Node용 `NODERAWFS`, `NODE_HOST_ENV`, `EXIT_RUNTIME`, `--pre-js`). 엔진을 쓰는 웹 바이너리의 메모리
  배치는 `repiu_link_web_engine`(`GLOBAL_BASE`, 처음 메모리 768 MB, 증가 허용, 스택 16 MB)으로 로더와 core probe가 함께
  씁니다. `execution_model_none.cpp`는 쓰는 곳이 없어 지웠습니다.
* **웹 플랫폼 계층:**
  * `virtual_memory.cpp`(stub → 구현): 게스트 영역 `[0x00010000, 0x10000000)`의 예약기와 보호 표, 메모리 변경 알림.
    `GLOBAL_BASE` 없이 링크된 바이너리에서는 예약을 거절하고 이유를 찍습니다(아래 core probe에서 실제로 걸림).
  * `host_cpu_entry_state.cpp`: WOW64 값과 가짜 TEB, 고정 CPUID.
  * `host_thread.cpp`: 스레드 없음(실패를 돌려줌). Linux 것은 `pthread_timedjoin_np`를 써서 링크되지 않았습니다.
  * `host_reports.cpp`: 크래시 보고·심볼·텔레메트리 "없음". `host_trace_descriptor`는 Linux 것(POSIX `write`) 그대로.
  * `guest_stack_switch.cpp`: 두 복구 thunk. 실행되지 않고 주소만 게이트로 쓰입니다(wasm에서 함수 주소는 테이블 색인).
  * `legacy_gl_stubs.cpp`: Glide 백엔드가 아직 부르는 고정 기능 GL 11개. 불리면 이유를 찍고 멈춥니다. 단계 3에서 지웁니다.
  * `fault_handler.cpp`: `ArmLinuxDataWatchFromEnvironment`(항상 false).
* **core probe:** 웹에서 `launcher_update`를 뺐습니다(스레드에서 릴리스를 내려받는 데스크톱 기능).
* **빌드 스크립트:** `--build-dir`, `--rex86-source`.

### 결과

| 확인 | 결과 |
|---|---|
| 웹 `repiu` | 링크, `repiu.wasm` 3.6 MB |
| 웹 `repiu_core_probe` (Node) | **36/36 통과**, 웹에서 성립하지 않는 12개는 이름을 찍고 건너뜀 |
| Node에서 pumpitea 30초 | `stop requested`로 정상 정지, Glide dummy, 무음 |
| HLE 경계 trace (Node 대 네이티브 Win32 `emulated`, 30초) | 아래 |

| 30초 | Node (wasm) | 네이티브 Win32 `emulated` |
|---|---|---|
| 실행 단계 | 183,959,733 (약 6.1 MIPS) | 181,247,027 |
| HLE 디스패치 | 6,094 | 6,094 |
| 호스트 차례 | 12,414 | 13,380 |

**trace 대조:** Node에서는 게스트 이미지가 첫 재배치 후보 `0x01000000`에 놓였습니다(네이티브는 그 범위를 Windows가 써서
`0x04000000`). 게스트 영역의 32비트 값을 `0x03000000` 옮기고 재배치된 주소가 박힌 명령 바이트 칸을 빼면:

* 41번째 줄까지의 차이는 모두 레지스터 일부에 담긴 이미지 기준 주소의 최상위 바이트(`0x04` 대 `0x01`)입니다.
* **42번째 줄이 처음 다른 종류의 차이**입니다: `DH`(`0x0D` 대 `0x1D`), 그 사이 `INT 21h AH=2Ch`가 돌려준 벽시계 초입니다.
  #64에서 네이티브끼리 대조했을 때의 첫 벽시계 의존과 같은 줄입니다.
* 그 뒤 차이도 주소 바이트 꼴(재배치)입니다. 6,095줄 중 5,925줄이 정규화 후 그대로 같습니다.

**즉 Node 실행은 네이티브 `emulated`와 첫 벽시계 의존까지 일치합니다(완료 기준).**

### 회귀

| 구성 | 결과 |
|---|---|
| Win32 `direct` Release | 빌드, core probe 44/44, pumpitea 70초 13번(아래) |
| Win32 `emulated` Release | 빌드, core probe 44/44, pumpitea 30초 1억 8,840만 단계, 정상 정지 |
| Linux i386 (WSL) | 빌드, core probe 통과 |
| Linux x64 (WSL) | 빌드, core probe 통과 |

`direct` 70초 실행 1번이 58초에 Task 736 모양(타이머 ISR 중첩으로 게스트 스택이 넘침)의 게스트 접근 위반으로
끝났습니다. 이 작업의 `direct` 쪽 변경은 `_mm_pause` 분기뿐이므로 원인으로 보기 어렵습니다. 같은 모양은 #64 코드에서도
한 번 나왔습니다. 변경 전(v0.0.219) 빌드와 번갈아 돌린 비교는 [#65 로그](20261015-i065-teardown-heap-crash.md)에
있습니다.

### 남은 것

* 단계 3(Glide 렌더러를 GLES 3 / WebGL2로)과 단계 4(브라우저 페이지).
* 런처는 웹에서 아직 쓸 수 없습니다(업데이트 확인이 스레드를 씀). 브라우저 페이지에서는 런처 대신 페이지가 게임을 고릅니다.
* 웹 빌드를 CI에 넣는 일.

### 재현

```bash
source ~/emsdk/emsdk_env.sh
./scripts/build_web_wasm.sh --build-dir ~/repiu-web --rex86-source /mnt/c/workspace/git/rex86 \
  --target repiu --target repiu_core_probe
node ~/repiu-web/repiu_core_probe.js
# 저장소 루트에서:
REPIU_EXECUTION_TIMEOUT_MS=30000 node ~/repiu-web/repiu.js pumpitea
REPIU_HLE_BOUNDARY_TRACE=trace_node.txt REPIU_EXECUTION_TIMEOUT_MS=30000 node ~/repiu-web/repiu.js pumpitea
```

---

# Work log: browser host phase 2 — the Emscripten build of the `emulated` engine, run headless under Node (issue #67)

Design: [20261015-i067](../design/20261015-i067-emscripten-headless.md) ·
Work order: [20261015-i067](../work-orders/20261015-i067-emscripten-headless.md)

## 2026-10-15 — implemented and verified

Branch `web-emscripten-headless` (main `fe0d31e`, from v0.0.220). Toolchain: emsdk on WSL Ubuntu 24.04, Emscripten
6.0.8, Node 24.19.0. rex86 is the local `../rex86` at v0.0.34. The build tree is in the WSL home (`~/repiu-web`), which
builds faster than under `/mnt/c`.

### Answers to the design's "to be confirmed"

| Item | Result |
|---|---|
| Memory layout | with `-sGLOBAL_BASE=0x10000000`: data at `0x10000260`, stack at `0x1080095c` (8 MB in the test), heap at `0x108013c0`. Emscripten links with `--no-stack-first`, so **the stack follows the data**; `0x04000000` can be written at once (confirmed, a small test program) |
| Default stack | **64 KB** (`STACK_SIZE`), far too small for the single-threaded loop, so linked at 16 MB |
| `getenv` and Node's environment | by default Node's `process.env` is not seen; with `-sNODE_HOST_ENV=1` it is (confirmed). The `--pre-js` is used only for the SDL driver defaults |
| Engine code that does not compile for wasm32 | **none.** The whole engine list compiled on the first attempt; the one fix was using `_mm_pause` on x86 only |
| SDL3 under Node | the Emscripten video driver reads `window.matchMedia` while initialising and stops with `ReferenceError: window is not defined`. Under Node the dummy video and audio drivers are the default (`src/platform/web/node_pre.js`), so Glide runs as its dummy and the audio goes silent |
| Whether the guest reads FS (the TEB) | no problem in a 30 s run (the stand-in TEB holds zeros and `Self`) |
| libchdr under `NODERAWFS` | the ROM ZIP and the CHD are read and the game's files prepared (confirmed) |
| The loader's spdlog | compiles for Emscripten with `SPDLOG_USE_STD_FORMAT` in place of the bundled fmt |

### Implementation

* **CMake:** the web's execution model is `emulated`. The `NOT EMSCRIPTEN` guards came off the engine list, the model
  files and the ImGui link. A web loader target `repiu` (for Node: `NODERAWFS`, `NODE_HOST_ENV`, `EXIT_RUNTIME`, the
  `--pre-js`). The memory layout of web binaries that run the engine is `repiu_link_web_engine` (`GLOBAL_BASE`, 768 MB
  initial memory, growth, a 16 MB stack), shared by the loader and the core probe. `execution_model_none.cpp`, used by
  nothing now, is deleted.
* **The web platform layer:**
  * `virtual_memory.cpp` (stub → implementation): the reserver and protection record over the guest range
    `[0x00010000, 0x10000000)`, with memory-change notifications. In a binary linked without `GLOBAL_BASE` it refuses
    reservations and says why (which the core probe below actually hit).
  * `host_cpu_entry_state.cpp`: the WOW64 values, the stand-in TEB, the fixed CPUID.
  * `host_thread.cpp`: no threads (it returns failure). The Linux one did not link: it uses `pthread_timedjoin_np`.
  * `host_reports.cpp`: "none" for the crash reporter, symbols and telemetry. `host_trace_descriptor` is the Linux one
    (a POSIX `write`) as it is.
  * `guest_stack_switch.cpp`: the two recovery thunks, never run, their addresses used as gates only (in wasm a
    function's address is a table index).
  * `legacy_gl_stubs.cpp`: the 11 fixed-function GL calls the Glide backend still makes; reaching one prints why and
    stops. Phase 3 deletes it.
  * `fault_handler.cpp`: `ArmLinuxDataWatchFromEnvironment` (always false).
* **Core probe:** `launcher_update` is left out on the web (a desktop feature that downloads releases on a thread).
* **Build script:** `--build-dir` and `--rex86-source`.

### Results

| Check | Result |
|---|---|
| Web `repiu` | links; `repiu.wasm` 3.6 MB |
| Web `repiu_core_probe` (Node) | **36/36 pass**; the 12 that do not hold on the web are named and skipped |
| pumpitea for 30 s under Node | a clean `stop requested`, Glide dummy, silent |
| HLE boundary trace (Node against native Win32 `emulated`, 30 s) | below |

| 30 s | Node (wasm) | native Win32 `emulated` |
|---|---|---|
| Steps | 183,959,733 (about 6.1 MIPS) | 181,247,027 |
| HLE dispatches | 6,094 | 6,094 |
| Host turns | 12,414 | 13,380 |

**The trace comparison:** under Node the guest image was placed at the first relocation candidate, `0x01000000` (natively
Windows uses that range, so `0x04000000`). With the guest range's 32-bit values moved by `0x03000000` and the instruction
byte field, which carries relocated addresses, left out:

* Every difference up to line 41 is the top byte of the image base held in part of a register (`0x04` against `0x01`).
* **Line 42 is the first difference of another kind:** `DH` (`0x0D` against `0x1D`), the wall-clock seconds `INT 21h
  AH=2Ch` returned in between -- the same line as the first wall-clock dependence when #64 compared native runs.
* The differences after it have the shape of address bytes too (relocation). 5,925 of 6,095 lines are identical after
  the normalisation.

**So the Node run matches native `emulated` up to the first wall-clock dependence (the done criterion).**

### Regressions

| Configuration | Result |
|---|---|
| Win32 `direct` Release | build, core probe 44/44, pumpitea for 70 s thirteen times (below) |
| Win32 `emulated` Release | build, core probe 44/44, pumpitea 188.4 million steps in 30 s, a clean stop |
| Linux i386 (WSL) | build, core probe passes |
| Linux x64 (WSL) | build, core probe passes |

One `direct` 70 s run ended at 58 s with a guest access violation in the shape of Task 736 (the timer ISR nesting until
the guest stack overflows). This work's only change on the `direct` side is the `_mm_pause` branch, so it is hard to see
as the cause; the same shape appeared once on #64's code too. The comparison alternating with the build from before
(v0.0.219) is in the [#65 log](20261015-i065-teardown-heap-crash.md).

### Remaining

* Phase 3 (the Glide renderer on GLES 3 / WebGL2) and phase 4 (the browser page).
* The launcher is not usable on the web yet (its update check uses a thread); in the browser the page picks the game
  instead.
* The web build in CI.

### Reproducing

```bash
source ~/emsdk/emsdk_env.sh
./scripts/build_web_wasm.sh --build-dir ~/repiu-web --rex86-source /mnt/c/workspace/git/rex86 \
  --target repiu --target repiu_core_probe
node ~/repiu-web/repiu_core_probe.js
# from the repository root:
REPIU_EXECUTION_TIMEOUT_MS=30000 node ~/repiu-web/repiu.js pumpitea
REPIU_HLE_BOUNDARY_TRACE=trace_node.txt REPIU_EXECUTION_TIMEOUT_MS=30000 node ~/repiu-web/repiu.js pumpitea
```
