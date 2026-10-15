# `src/platform/web` — WebAssembly 플랫폼 계층 (Task 513 Stage 1)

설계: [20260828-513](../../../docs/design/20260828-513-web-wasm-execution.md) ·
작업 지시: [20260828-513](../../../docs/work-orders/20260828-513-web-wasm-build.md)

이 디렉터리는 13개 플랫폼 헤더 중 **wasm에서 성립하는 것만** 구현합니다. 성립하지 않는
다섯은 같은 자리에 **실패를 반환하는 stub**으로 둡니다.

**issue #67부터 웹 빌드는 엔진을 `emulated` 모델로 빌드합니다.** 게스트는 rex86 CPU 코어가 실행하므로,
게스트 메모리는 선형 메모리의 같은 주소에 두고(identity 매핑) Emscripten의 데이터·스택·힙은 링크 옵션
`-sGLOBAL_BASE=0x10000000`으로 그 위에 둡니다. 그래서 가상 메모리가 stub에서 실제 구현이 되었습니다
([설계 #67](../../../docs/design/20261015-i067-emscripten-headless.md)).

| 파일 | 성격 |
|---|---|
| `safe_memory_copy.cpp` | 실제 구현 — 선형 메모리 범위 검사. wasm은 범위 밖 접근이 **복구 불가능한 트랩**이므로 검사가 유일한 안전망입니다 |
| `web_unsupported.h` | stub이 이유를 **한 번만** 찍는 자리 |
| `guest_cpu_context.cpp` | **stub** — 호스트에 x86 레지스터 컨텍스트가 없습니다 |
| `fault_handler.cpp` | **stub** — 하드웨어 폴트를 사용자 핸들러로 전달하는 개념이 없습니다 |
| `virtual_memory.cpp` | **실제 구현 (issue #67)** — 게스트 영역 `[0x00010000, 0x10000000)`의 예약기와 페이지 보호 표. 보호는 rex86의 페이지 속성표가 지킵니다 |
| `host_cpu_entry_state.cpp` | **실제 구현 (issue #67)** — WOW64 스레드가 보는 셀렉터·FPU 값, 가짜 TEB 한 페이지, 고정 CPUID 표 |
| `host_reports.cpp` | **"없음" (issue #67)** — 크래시 보고, 폴트 보고의 호스트 정보, 모듈·심볼 이름, 프로세스 간 텔레메트리. 진단만 잃습니다. 렌더러가 요청할 GL 문맥은 ES 3.0(WebGL2)입니다(issue #69; 고정 기능 GL stub `legacy_gl_stubs.cpp`는 렌더러가 GLES 3 부분집합으로 옮겨 지워졌습니다) |
| `host_process.cpp` | **stub** — 브라우저에 자식 프로세스가 없습니다 |

## 여기 없는 것 — `host_environment`, `worker_signal`, `host_trace_descriptor`

이들은 웹 사본을 만들지 않고 `src/platform/linux/`의 것을 **그대로 씁니다**(`host_trace_descriptor`는
issue #67부터, POSIX `write` 하나뿐입니다). 두 파일 모두
`#if !defined(_WIN32)`로 열려 있고 내용이 POSIX입니다 — `environ`·`setenv`와
`std::mutex`·`std::condition_variable`뿐이며, Linux 고유 호출이 하나도 없습니다.

같은 코드를 두 벌 두면 한쪽만 고쳐지는 날이 옵니다. 경로 이름이 `linux`인 것은 정확하지
않지만, Task 503d-17이 `src/host/win32/main.cpp`를 Linux에서 쓰기로 한 것과 같은 판단입니다
— 이름의 부정확함보다 사본의 발산이 비쌉니다.

`safe_memory_copy`는 이 목록에 들어가지 못합니다. Linux 구현이 `process_vm_readv`를 쓰는데
Emscripten에 없고, 애초에 wasm에서는 다른 방법이 필요합니다.

## stub이 조용히 성공하지 않는 이유

2026-08-27 세션은 **성공 신호 하나를 성공으로 읽어** 세 번 걸렸습니다 — `exit 0`이 정상
완주가 아니었고, `opened=1`이 창이 열린 것이 아니었으며(더미 폴백도 같은 값을 냈습니다),
`dispatch_entry` 폭증이 전진이 아니었습니다.

그래서 여기 stub은 전부 **거짓을 반환하고, 이유를 표준 오류로 한 번 찍습니다.** 성공을
흉내내는 더미를 두면 Stage 3에서 "실행이 되는 것 같은데 아무 일도 일어나지 않는" 상태를
디버깅하게 되고, 그것이 이 저장소가 이미 비싸게 배운 종류의 함정입니다.

`RemoveFaultHandler`가 참이 아니라 거짓을 돌려주는 것도 같은 이유입니다. 설치된 적이 없는
것을 "떼어냈다"고 보고하면 종료 경로가 정리를 마쳤다고 기록하게 되고, Task 508의 코어 덤프가
정확히 그렇게 **올바르게 보이는 순서** 뒤에 숨어 있었습니다.

---

# `src/platform/web` — the WebAssembly platform layer (Task 513 Stage 1)

Design: [20260828-513](../../../docs/design/20260828-513-web-wasm-execution.md) ·
Work order: [20260828-513](../../../docs/work-orders/20260828-513-web-wasm-build.md)

This directory implements **only the platform headers that hold on wasm**. The five that do not
stay here as **stubs that return failure**.

**Since issue #67 the web build builds the engine, on the `emulated` model.** The guest runs on the
rex86 CPU core, so guest memory sits at the same addresses in linear memory (identity mapping) and
Emscripten's data, stack and heap sit above it through the link option `-sGLOBAL_BASE=0x10000000`.
That is why the virtual memory went from a stub to an implementation
([design #67](../../../docs/design/20261015-i067-emscripten-headless.md)).

| File | Nature |
|---|---|
| `safe_memory_copy.cpp` | Real — a linear-memory range check. Out-of-bounds access in wasm is an **unrecoverable trap**, so the check is the only safety net |
| `web_unsupported.h` | Where a stub says its reason, **once** |
| `guest_cpu_context.cpp` | **stub** — the host has no x86 register context |
| `fault_handler.cpp` | **stub** — there is no notion of delivering a hardware fault to a user handler |
| `virtual_memory.cpp` | **real (issue #67)** — a reserver and page-protection record over the guest range `[0x00010000, 0x10000000)`; rex86's page attribute table enforces the protection |
| `host_cpu_entry_state.cpp` | **real (issue #67)** — the selector and FPU values a WOW64 thread sees, a stand-in TEB page, a fixed CPUID table |
| `host_reports.cpp` | **"none" (issue #67)** — the crash reporter, the fault report's host details, module and symbol names, cross-process telemetry; only diagnostics are lost. The GL context the renderer asks for is ES 3.0 (WebGL2) (issue #69; the fixed-function GL stubs, `legacy_gl_stubs.cpp`, went when the renderer moved to the GLES 3 subset) |
| `host_process.cpp` | **stub** — a browser has no child processes |

## What is not here — `host_environment`, `worker_signal`, `host_trace_descriptor`

None gets a web copy; the Emscripten build uses the ones in `src/platform/linux/` **as they
are** (`host_trace_descriptor` since issue #67: a single POSIX `write`). Both open with `#if !defined(_WIN32)` and both are POSIX inside — `environ` and `setenv`,
`std::mutex` and `std::condition_variable`, and not one Linux-specific call.

Two copies of the same code means a day when only one is fixed. The path saying `linux` is
inaccurate, but it is the same judgement Task 503d-17 made when it used `src/host/win32/main.cpp`
on Linux: a divergent copy costs more than an inaccurate name.

`safe_memory_copy` cannot join that list. The Linux implementation uses `process_vm_readv`, which
Emscripten does not have, and wasm needs a different approach regardless.

## Why the stubs do not quietly succeed

The 2026-08-27 session was caught three times by **reading one success signal as success**:
`exit 0` was not a clean finish, `opened=1` was not an opened window (the dummy fallback returned
the same value), and a surge in `dispatch_entry` was not progress.

So every stub here **returns false and prints its reason to standard error once**. A dummy that
imitates success would turn Stage 3 into debugging a state where "execution seems to work but
nothing happens", and that is a trap this repository has already paid for.

`RemoveFaultHandler` returning false rather than true is the same reasoning. Reporting that
something never installed was "removed" lets a teardown path record that it finished tidying up,
and Task 508's core dump hid behind exactly that kind of **sequence that looked correct**.
