# 설계: 웹 빌드의 게스트를 rex86의 wasm 번역으로 돌리기, 그리고 재기 (issue #79)

관련: [설계 #61 — emulated 실행 모델](20261014-i061-emulated-execution-model.md), [설계 #67 — Node 실행](20261015-i067-emscripten-headless.md),
[설계 #72 — 브라우저 페이지](20261016-i072-browser-page.md), rex86 설계 #44·#45·#49·#65·#67(번역과 wasm 백엔드)

## 배경

웹 빌드는 게스트를 rex86의 **인터프리터로만** 돌립니다. Node에서 약 6.1 MIPS(#67)이고, 브라우저에서 pumpitea 타이틀까지 약
4분입니다. pumpitea가 60fps로 돌려면 약 75 MIPS가 필요하다고 추정했습니다(#61). 다만 이 값에는 게스트의 대기 루프가 섞여 있어
상한으로 봐야 합니다(#64).

조사한 사실(확인됨, rex86 v0.0.34 소스와 문서):

* rex86에는 번역 모드가 셋 있습니다. `kOff`(인터프리터), `kEvaluator`(검증용 IR 평가기, 빠르지 않음), `kWasm`(블록을 wasm 모듈로
  만들어 설치, wasm32 빌드 전용).
* `kWasm`은 호스트의 모듈 설치기 `WasmModuleServices`만 있으면 됩니다(`code_cache`는 네이티브 JIT용이라 필요 없음). rex86의
  `rex86_host_web` 타깃이 동기 설치기 `WebWasmModuleServices`(`new WebAssembly.Module`/`Instance`)와 비동기 설치기를 주며,
  "소비자의 Worker 빌드가 링크한다"고 되어 있습니다. 동기 설치기는 Node와 Worker에 맞습니다(메인 스레드는 큰 모듈의 동기 컴파일이 제한됨).
* 생성 모듈은 코어의 메모리와 함수 테이블만 가져옵니다(`env.memory`, `env.table`).
* 번역기는 게이트(`RegisterGate`)에서 블록을 끊고, 게이트를 등록하면 그 주소를 덮는 번역을 버립니다. 번역하지 못한 명령이나
  실패한 검사는 인터프리터로 넘깁니다(`interpreter_exits`).
* rePIU는 호스트 쪽 메모리 변경을 이미 `Cpu::InvalidateCode`로 알립니다(`PendingMemoryChanges`).
* rex86 자체 벤치마크(`rex86_bench`, 묶음 16): 인터프리터 대비 `alu` 약 9배(Node 19.5 → 202.7 MIPS, Chromium 23.2 → 184.2),
  `call` 약 5배, `mixed` 1.5배 안팎. 블록이 아주 많은 `blocks`는 디스패치 비용 때문에 이득이 작습니다.
* rePIU는 `rex86::Cpu`를 번역 옵션 없이 만듭니다. 즉 **지금까지 한 번도 번역을 켠 적이 없습니다.**

그래서 rex86 성능을 보는 첫 걸음은 rex86을 고치는 것이 아니라, 이미 있는 번역을 rePIU에서 켜고, 무엇이 번역되고 무엇이
인터프리터로 떨어지는지 재는 것입니다. 그 숫자로 다음 rex86 작업(번역 범위: x87, REP 문자열 등)을 고릅니다.

## 결정 1: 웹 빌드의 `emulated` 모델이 rex86 `kWasm`으로 돈다

* 웹 빌드(`repiu`, `repiu_browser`)가 `rex86_host_web`을 링크하고, 게스트 스레드의 `Cpu`를 만든 뒤
  `SetTranslation({kWasm, wasm = &설치기})`를 부릅니다. 설치기는 동기 `WebWasmModuleServices` 하나를 프로세스가 공유합니다
  (한 Worker 안에서 Cpu는 하나씩 돕니다).
* `SetTranslation`이 거짓을 돌려주면(빌드가 wasm 실행을 못 할 때) 한 줄을 찍고 인터프리터로 계속합니다. 흉내 내지 않습니다.
* 네이티브(`emulated` Win32 개발 빌드)는 인터프리터 그대로입니다. 네이티브 호스트용 번역 백엔드는 rex86에 없습니다.

## 결정 2: 끄는 스위치와 번역 문턱

* `REPIU_REX86_TRANSLATION`: `wasm`(웹 기본) 또는 `off`. 대조 측정과 문제가 생겼을 때 되돌리는 데 씁니다. 설정은 소비자가 이미
  읽는 환경 변수로 넘기는 이 저장소의 관례를 따릅니다.
* `REPIU_REX86_TRANSLATION_THRESHOLD`: 블록을 번역하기 전 진입 횟수(rex86 기본 32). `0`은 첫 진입에 번역하며, 검증에서 번역을
  최대한 많이 거치게 할 때 씁니다.
* 나머지 옵션은 rex86 기본값(블록당 최대 64명령, 모듈당 16블록, 대기 4,096, 모듈 예산 4,096)입니다. 예산은 Firefox의 모듈 한도
  (약 16,000)보다 충분히 작습니다.

## 결정 3: 실행 끝에 번역 통계를 한 줄로

실행이 끝날 때 기존 `[repiu-emulated]` 보고 옆에 rex86의 `TranslationStats`와 설치기 수치를 한 줄로 찍습니다:
번역한 블록, 번역 블록 실행 수, 인터프리터로 나간 수, 번역 안에서 돈 단계, 제출·축출된 모듈, 무효화된 블록, 설치한 모듈·바이트,
설치 시간. **번역 비율**(번역 단계 / 전체 단계)이 다음 작업을 고르는 핵심 숫자입니다.

## 결정 4: 재는 방법

Node, pumpitea, 같은 PC, 번역 `off`와 `wasm`을 번갈아:

* 같은 시간(60초) 동안의 실행 단계(→ MIPS), HLE 경계 수, 화면 교체 수. 단계/초에는 대기 루프가 섞이므로 **같은 시간에 게임이
  얼마나 나아갔는지**(경계·화면 교체)를 함께 봅니다.
* 결정 3의 번역 통계.
* 가능하면 브라우저에서 타이틀까지의 시간(이 PC의 브라우저 파일 읽기 문제가 풀렸을 때).

## 결정 5: 같게 도는지 확인하는 방법

* **HLE 경계 trace 대조:** 번역 `off`와 `wasm`(문턱 32, 그리고 0)의 `REPIU_HLE_BOUNDARY_TRACE`가 첫 벽시계 의존까지 같아야 합니다
  (#67의 방법, Node에서는 이미지 위치도 같음). 다르면 첫 차이 지점이 번역 버그의 위치입니다.
* core probe(웹 Node), 네이티브 회귀(번역이 들어가지 않음을 확인).

## 바꾸지 않는 것

* rex86 소스. 이 issue는 소비자 쪽에서 켜고 잽니다. 번역 버그나 부족한 범위가 나오면 rex86 issue로 엽니다.
* HLE 경계, 게이트, 타이머 주입, 단일 스레드 루프의 모양.

## 확인 필요 (구현 중)

| 항목 | 왜 |
|---|---|
| `Cpu::Run(kSliceSteps)`의 예산과 블록 길이 | 블록은 예산이 통째로 덮을 때만 번역으로 돕니다. 슬라이스 끝 무렵은 인터프리터로 돌 수 있음 |
| TF 단일 단계(`Step`)와 RF로 브레이크포인트 넘기기 | 번역과 섞여도 같은 동작인지 |
| Asyncify | 생성 모듈은 함수 테이블로 불립니다(`ASYNCIFY_IGNORE_INDIRECT`). 양보는 `Run` 밖에서만 하므로 문제 없어야 함 |
| `-sALLOW_TABLE_GROWTH`와 기존 링크 옵션 | `rex86_host_web`이 붙이는 옵션이 `repiu_browser`의 것과 충돌하지 않는지 |
| 무효화 | 로더·HLE가 게스트 코드 페이지를 고칠 때 번역이 버려지는지(trace 대조가 잡음) |

## 검증

1. 웹 빌드(`repiu`, `repiu_browser`, `repiu_core_probe`) 빌드, 웹 core probe.
2. Node pumpitea: trace 대조(결정 5), 측정(결정 4) 각 두 번 이상 번갈아.
3. 네이티브 Win32 `direct`·Linux 빌드와 core probe(바뀌지 않음).

---

# Design: run the web build's guest on rex86's wasm translation, and measure it (issue #79)

Related: [design #61 — the emulated execution model](20261014-i061-emulated-execution-model.md), [design #67 — running under
Node](20261015-i067-emscripten-headless.md), [design #72 — the browser page](20261016-i072-browser-page.md), rex86 designs #44,
#45, #49, #65 and #67 (translation and the wasm backend)

## Background

The web build runs the guest on rex86's **interpreter alone**: about 6.1 MIPS under Node (#67), and about 4 minutes to
pumpitea's title in the browser. pumpitea was estimated to need about 75 MIPS for 60 fps (#61), an upper bound because
the guest's wait loops are mixed into it (#64).

What was found (confirmed, from rex86 v0.0.34's source and documents):

* rex86 has three translation modes: `kOff` (the interpreter), `kEvaluator` (an IR evaluator for verification, not fast)
  and `kWasm` (blocks made into wasm modules and installed; wasm32 builds only).
* `kWasm` needs only the host's module installer, `WasmModuleServices` (`code_cache` is for a native JIT and not
  needed). rex86's `rex86_host_web` target provides a synchronous installer, `WebWasmModuleServices`
  (`new WebAssembly.Module`/`Instance`), and an asynchronous one, and is "linked by consumers' Worker builds". The
  synchronous one suits Node and a Worker (the main thread limits synchronous compilation of large modules).
* Generated modules import only the core's memory and function table (`env.memory`, `env.table`).
* The translator ends blocks at gates (`RegisterGate`) and drops the translations covering an address when a gate is
  registered there. An instruction it cannot translate, or a failed check, goes to the interpreter
  (`interpreter_exits`).
* rePIU already reports host-side memory changes through `Cpu::InvalidateCode` (`PendingMemoryChanges`).
* rex86's own benchmark (`rex86_bench`, batches of 16): against the interpreter, `alu` about 9× (Node 19.5 → 202.7 MIPS,
  Chromium 23.2 → 184.2), `call` about 5×, `mixed` around 1.5×. `blocks`, with a great many blocks, gains little
  because of the per-block dispatch cost.
* rePIU makes its `rex86::Cpu` with no translation options; **translation has never been on.**

So the first step in looking at rex86's performance is not changing rex86 but turning on the translation it already has
in rePIU, and measuring what runs translated and what falls back to the interpreter. Those numbers choose the next rex86
work (translation coverage: the x87, REP strings and so on).

## Decision 1: the web build's `emulated` model runs on rex86 `kWasm`

* The web build (`repiu`, `repiu_browser`) links `rex86_host_web`, and after making the guest thread's `Cpu` calls
  `SetTranslation({kWasm, wasm = &installer})`. One synchronous `WebWasmModuleServices` is shared by the process (one
  Cpu runs at a time in a Worker).
* If `SetTranslation` returns false (the build cannot run wasm), one line is printed and the interpreter carries on;
  nothing is imitated.
* Native builds (the `emulated` Win32 development build) keep the interpreter; rex86 has no translation backend for a
  native host.

## Decision 2: a switch to turn it off, and the translation threshold

* `REPIU_REX86_TRANSLATION`: `wasm` (the web default) or `off`, for comparisons and for going back if something breaks.
  It follows this repository's custom of passing settings through environment variables their consumers already read.
* `REPIU_REX86_TRANSLATION_THRESHOLD`: entries into a block before it is translated (rex86's default 32). `0`
  translates at the first entry, to push as much as possible through translation when verifying.
* The other options are rex86's defaults (at most 64 instructions a block, 16 blocks a module, a wait of 4,096, a
  budget of 4,096 modules). The budget is well below Firefox's module limit (about 16,000).

## Decision 3: the translation statistics in one line at the end of a run

When a run ends, next to the existing `[repiu-emulated]` report, one line gives rex86's `TranslationStats` and the
installer's numbers: blocks translated, translated block runs, exits to the interpreter, steps run translated, modules
submitted and evicted, blocks invalidated, modules and bytes installed, install time. **The translated share** (translated
steps / all steps) is the key number for choosing the next work.

## Decision 4: how it is measured

Under Node, pumpitea, on the same PC, alternating translation `off` and `wasm`:

* Over the same time (60 s): steps (→ MIPS), HLE boundaries and buffer swaps. Steps per second include the wait loops,
  so **how far the game got in the same time** (boundaries, swaps) is read alongside.
* Decision 3's statistics.
* If possible, the time to the title in a browser (once this PC's browser file-read problem is gone).

## Decision 5: how it is checked to run the same

* **The HLE boundary trace:** `REPIU_HLE_BOUNDARY_TRACE` with translation `off` and `wasm` (threshold 32, and 0) must
  match up to the first wall-clock dependence (#67's method; under Node the image lands at the same address too). If
  they differ, the first difference is where a translation bug is.
* The core probes (web, Node), and the native regression (translation stays out).

## What does not change

* rex86's source. This issue turns translation on and measures from the consumer's side; a translation bug or a coverage
  gap becomes a rex86 issue.
* The HLE boundaries, gates, timer injection and the single-threaded loop's shape.

## To be confirmed (during implementation)

| Item | Why |
|---|---|
| The `Cpu::Run(kSliceSteps)` budget and block length | A block runs translated only when the budget covers it whole; near a slice's end it may run on the interpreter |
| TF single steps (`Step`) and passing a breakpoint with RF | Do they behave the same mixed with translation |
| Asyncify | Generated modules are called through the function table (`ASYNCIFY_IGNORE_INDIRECT`); yields happen only outside `Run`, so there should be no problem |
| `-sALLOW_TABLE_GROWTH` and the existing link options | Do the options `rex86_host_web` adds clash with `repiu_browser`'s |
| Invalidation | Are translations dropped when the loader or the HLE changes guest code pages (the trace comparison catches it) |

## Verification

1. The web build (`repiu`, `repiu_browser`, `repiu_core_probe`) builds; the web core probe.
2. Node pumpitea: the trace comparison (decision 5) and the measurement (decision 4), each at least twice, alternating.
3. Native Win32 `direct` and Linux builds and core probes (unchanged).
