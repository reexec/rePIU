# 작업 로그: 웹 빌드의 게스트를 rex86의 wasm 번역으로 돌리기 (issue #79)

설계: [20261016-i079](../design/20261016-i079-rex86-wasm-translation.md) ·
작업 지시: [20261016-i079](../work-orders/20261016-i079-rex86-wasm-translation.md)

## 2026-10-16 — 구현, 확인, 측정

브랜치 `task/i079-rex86-wasm-translation`(main `3008163`, v0.0.225에서). rex86 v0.0.34, emsdk 6.0.8, Node 24.19, WSL.
계기는 사용자의 "rex86 성능을 먼저 보는게 좋을거 같아"입니다.

### 구현

| 부분 | 내용 |
|---|---|
| 링크 | `emulated` 모델의 웹 빌드에서 `repiu_exe`가 `rex86_host_web`을 링크(그 JS 라이브러리와 `-sALLOW_TABLE_GROWTH`가 따라옴) |
| 켜기 | `EmulatedRun::Run`이 `Cpu`를 만든 뒤 `ConfigureTranslation`: `REPIU_REX86_TRANSLATION`(`wasm` 기본, `off`), `REPIU_REX86_TRANSLATION_THRESHOLD`(기본 32). 설치기는 프로세스 하나의 동기 `WebWasmModuleServices`. `SetTranslation`이 거절하면 한 줄 찍고 인터프리터 |
| 보고 | 실행 끝 `[repiu-emulated] translation wasm threshold=…: translated-steps=… (…%) blocks=… block-runs=… interpreter-exits=… invalidated=… modules=… evicted=… installed=… bytes=… install-ms=… refused=…` |
| 네이티브 | 바뀌지 않음(`#if defined(__EMSCRIPTEN__)`) |

### 설계의 "확인 필요"에 대한 답

| 항목 | 결과 |
|---|---|
| 게스트 실행 수 | 60초 실행 동안 `EmulatedRun`(=`Cpu`)은 **하나**. 실행마다 번역을 버리는 문제는 없음 |
| 예산과 블록 길이 | 슬라이스 1만 단계, 블록 최대 64명령. 단계의 약 94~95%가 번역 안에서 돎 |
| 링크 옵션 | `repiu`, `repiu_browser`, `repiu_core_probe` 모두 경고·오류 없이 링크 |
| 무효화 | 문턱 32에서 0건, 문턱 0에서 52건(첫 진입에 번역하므로 아직 바뀌는 코드도 번역됨). 설치 거절 0 |
| Asyncify, TF 단일 단계 | Node에는 양보가 없어 이 실행으로는 확인 못 함. 브라우저 확인에서 봄(아래) |

### 같게 도는지(결정 5)

Node, pumpitea, 30초씩, `REPIU_HLE_BOUNDARY_TRACE`:

* 번역 `off`, `wasm` 문턱 32, 문턱 0 세 trace의 **첫 41줄이 바이트까지 같고**, 42번째 줄에서 처음 다릅니다. 같은 EIP의 `INT 21h`
  직전 `EAX`/`EDX`가 `2A2E`/`2E01`, `2A12`/`1201`, `2A34`/`3401` — `INT 21h AH=2Ch`가 돌려준 벽시계 초입니다. #67에서 네이티브와
  Node를 대조했을 때의 첫 벽시계 의존과 같은 줄입니다.
* 레지스터를 빼고 경계의 종류·코드·EIP만 보면, 인터프리터 30초의 **경계 6,240개 전부**가 번역 실행의 첫 6,240개와 순서까지 같습니다
  (문턱 32, 0 모두). 벽시계가 들어간 값만 다르고 게스트가 밟는 HLE 경로는 같습니다.

### 측정(결정 4)

Node, pumpitea, 60초, 번갈아. "입력 폴링 시작"은 부팅이 끝나 게임이 JAMMA 입력을 읽기 시작한 때(`[repiu-input] polling started`,
`glide gate entry #50`과 같은 때)이며, 실행 시작부터 잰 벽시계 초입니다.

| 실행 | 번역 | 단계 | MIPS | 입력 폴링 시작 | 번역 단계 비율 |
|---|---|---:|---:|---:|---:|
| base1 | off | 331,779,145 | 5.5 | | |
| off2 | off | 366,401,154 | 6.1 | 55.7초 | |
| off3 | off | 359,366,851 | 5.9 | 56.4초 | |
| wasm1 | wasm | 1,428,812,990 | 23.8 | | 93.8% |
| wasm2 | wasm | 1,296,847,590 | 21.6 | 18.3초 | 94.4% |
| wasm3 | wasm | 1,237,068,628 | 20.6 | 18.4초 | 94.6% |

* **MIPS 약 3.5배(6 → 21), 부팅 끝까지 약 3배 빠름(56초 → 18초).**
* 같은 60초에 HLE 경계 처리는 213만에서 599만으로(wasm1 대 base1), 게임이 그만큼 더 나아갔습니다.
* wasm1의 번역 통계: 블록 3,440, 번역 블록 실행 2억 1,353만, 인터프리터로 나감 1,820만, 모듈 768(축출 0), 설치 3.19 MB에 334 ms.
* rex86 벤치마크의 `alu` 9배보다 작은 것은, 블록 실행당 약 6.3단계로 블록이 짧고(디스패치 비용), 블록 실행 12번에 한 번꼴로
  인터프리터로 나가기 때문으로 봅니다(추정).

### 회귀

| 구성 | 결과 |
|---|---|
| 웹 `repiu`, `repiu_browser`, `repiu_core_probe` | 빌드, Node core probe 37/37 |
| Win32 `emulated` Release | 빌드, core probe 45/45, pumpitea 30초 1억 7,823만 단계로 정상 정지, 번역 줄 없음(인터프리터 그대로) |
| Win32 `direct` Release | 빌드, core probe 45/45 |

**브라우저:** headless Edge로 다시 시도했으나 #76 작업 로그의 문제(브라우저의 파일 읽기가 느려 CHD 풀기가 약 20%에서 멈춤)가 그대로라
게임까지 가지 못했습니다. 브라우저에서 번역과 Asyncify 양보가 함께 도는지, 타이틀까지의 시간은 사용자 브라우저에서 확인할 것으로 남깁니다.

### 남은 것

* **브라우저 확인**(위).
* **인터프리터로 나가는 곳의 내역.** rex86의 통계는 합계만 줍니다. 어느 명령(x87, REP 문자열, 세그먼트 접두 등)이 번역 밖인지
  명령별로 세어야 다음 rex86 작업을 고를 수 있습니다. rex86 쪽 진단 추가가 필요합니다.
* **블록 길이.** 블록 실행당 6.3단계. 블록을 잇는 연결(chaining)이나 더 긴 블록이 디스패치 비용을 줄일 수 있는지.
* 60fps 목표(추정 75 MIPS 상한)까지는 아직 3배 남짓.

---

# Work log: run the web build's guest on rex86's wasm translation (issue #79)

Design: [20261016-i079](../design/20261016-i079-rex86-wasm-translation.md) ·
Work order: [20261016-i079](../work-orders/20261016-i079-rex86-wasm-translation.md)

## 2026-10-16 — implementation, checks, measurements

Branch `task/i079-rex86-wasm-translation` (from main `3008163`, v0.0.225). rex86 v0.0.34, emsdk 6.0.8, Node 24.19, WSL.
It started from the user's "I think we should look at rex86's performance first".

### Implementation

| Part | What |
|---|---|
| Link | in the `emulated` model's web build, `repiu_exe` links `rex86_host_web` (its JS library and `-sALLOW_TABLE_GROWTH` come along) |
| Turning it on | after `EmulatedRun::Run` makes its `Cpu`, `ConfigureTranslation`: `REPIU_REX86_TRANSLATION` (`wasm` by default, `off`), `REPIU_REX86_TRANSLATION_THRESHOLD` (default 32). The installer is one synchronous `WebWasmModuleServices` for the process. If `SetTranslation` refuses, one line and the interpreter |
| Report | at the end of a run, `[repiu-emulated] translation wasm threshold=…: translated-steps=… (…%) blocks=… block-runs=… interpreter-exits=… invalidated=… modules=… evicted=… installed=… bytes=… install-ms=… refused=…` |
| Native | unchanged (`#if defined(__EMSCRIPTEN__)`) |

### Answers to the design's "to confirm"

| Item | Result |
|---|---|
| Guest runs | one `EmulatedRun` (= `Cpu`) over a 60 s run, so translations are not thrown away per run |
| Budget and block length | slices of 10,000 steps, blocks of at most 64 instructions; about 94–95% of steps run translated |
| Link options | `repiu`, `repiu_browser` and `repiu_core_probe` all link without warnings or errors |
| Invalidation | 0 at threshold 32, 52 at threshold 0 (translating at the first entry also translates code that is still changing); 0 installs refused |
| Asyncify, TF single steps | Node never yields, so these runs cannot show it; see the browser check below |

### Does it run the same (decision 5)

Node, pumpitea, 30 s each, `REPIU_HLE_BOUNDARY_TRACE`:

* The three traces, translation `off`, `wasm` at threshold 32 and at 0, **match byte for byte for the first 41 lines** and
  first differ at line 42: `EAX`/`EDX` before the `INT 21h` at the same EIP are `2A2E`/`2E01`, `2A12`/`1201` and
  `2A34`/`3401` — the wall-clock seconds `INT 21h AH=2Ch` returned. It is the same line as the first wall-clock dependence
  when #67 compared native and Node.
* Leaving the registers out and keeping each boundary's kind, code and EIP, **all 6,240 boundaries** of the interpreter's
  30 s match the translated runs' first 6,240 in order (threshold 32 and 0 alike). Only values carrying the wall clock
  differ; the HLE path the guest takes is the same.

### Measurements (decision 4)

Node, pumpitea, 60 s, alternating. "Input polling starts" is when the boot is over and the game starts reading the JAMMA
inputs (`[repiu-input] polling started`, at the same time as `glide gate entry #50`), in wall-clock seconds from the run's
start.

| Run | Translation | Steps | MIPS | Input polling starts | Translated share |
|---|---|---:|---:|---:|---:|
| base1 | off | 331,779,145 | 5.5 | | |
| off2 | off | 366,401,154 | 6.1 | 55.7 s | |
| off3 | off | 359,366,851 | 5.9 | 56.4 s | |
| wasm1 | wasm | 1,428,812,990 | 23.8 | | 93.8% |
| wasm2 | wasm | 1,296,847,590 | 21.6 | 18.3 s | 94.4% |
| wasm3 | wasm | 1,237,068,628 | 20.6 | 18.4 s | 94.6% |

* **About 3.5× the MIPS (6 → 21), and the boot about 3× faster (56 s → 18 s).**
* In the same 60 s the HLE served 5.99 million boundaries instead of 2.13 million (wasm1 against base1); the game got that
  much further.
* wasm1's translation statistics: 3,440 blocks, 213.53 million translated block runs, 18.20 million exits to the
  interpreter, 768 modules (0 evicted), 3.19 MB installed in 334 ms.
* That this is below the benchmark's 9× for `alu` is put down to short blocks, about 6.3 steps per block run (the
  dispatch cost), and an exit to the interpreter about once every 12 block runs (inferred).

### Regression

| Configuration | Result |
|---|---|
| Web `repiu`, `repiu_browser`, `repiu_core_probe` | build; Node core probe 37/37 |
| Win32 `emulated` Release | builds, core probe 45/45, pumpitea stops normally after 178.23 million steps in 30 s, no translation line (the interpreter as before) |
| Win32 `direct` Release | builds, core probe 45/45 |

**Browser:** a retry in headless Edge hit the problem in #76's work log again (the browser's file reads are slow and the CHD
unpack stalls at about 20%), so it did not reach the game. Whether translation and the Asyncify yield run together in a
browser, and the time to the title, are left to check in the user's browser.

### Left

* **The browser check** (above).
* **What the interpreter exits are.** rex86's statistics give totals only. Which instructions fall outside translation
  (the x87, REP strings, segment prefixes and so on) has to be counted per instruction to choose the next rex86 work; that
  needs a diagnostic on rex86's side.
* **Block length.** 6.3 steps per block run. Whether chaining blocks, or longer blocks, would cut the dispatch cost.
* About 3× more is still needed for the 60 fps target (the estimated upper bound of 75 MIPS).
