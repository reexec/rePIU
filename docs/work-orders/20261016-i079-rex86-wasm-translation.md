# 작업 지시: 웹 빌드의 게스트를 rex86의 wasm 번역으로 돌리기, 그리고 재기 (issue #79)

설계: [20261016-i079](../design/20261016-i079-rex86-wasm-translation.md)

브랜치: `task/i079-rex86-wasm-translation`.

## 순서

1. **기준 측정:** 지금의 웹 빌드(인터프리터)로 Node pumpitea 60초: 실행 단계, HLE 경계, 화면 교체.
2. **링크:** 웹 빌드가 `rex86_host_web`을 링크(`repiu_exe`에서 Emscripten일 때만).
3. **켜기:** `emulated` 트램펄린이 `Cpu`를 만든 뒤 `REPIU_REX86_TRANSLATION`·`REPIU_REX86_TRANSLATION_THRESHOLD`에 따라
   `SetTranslation`(결정 1, 2). 실패하면 한 줄을 찍고 인터프리터.
4. **보고:** 실행 끝에 번역 통계 한 줄(결정 3).
5. **확인:** 설계의 검증 1~3. trace 대조는 `off`와 `wasm`(문턱 32, 0).
6. **측정:** `off`·`wasm`을 번갈아 두 번 이상(결정 4).
7. **문서:** 작업 로그, `src/platform/web/README.md`, `ARCHITECTURE.md`, `docs/analysis/web-port-frontier.md`, `docs/TODO.md`.

## 완료 기준

* 웹 빌드가 기본으로 번역을 켜고, `off`로 끌 수 있다.
* trace가 `off`와 같다(첫 벽시계 의존까지). 다르면 그 차이를 rex86 issue로 옮기고 기본값을 `off`로 둔다.
* 번역 전후의 MIPS와 게임 진행, 번역 비율이 기록되어 있다.

---

# Work order: run the web build's guest on rex86's wasm translation, and measure it (issue #79)

Design: [20261016-i079](../design/20261016-i079-rex86-wasm-translation.md)

Branch: `task/i079-rex86-wasm-translation`.

## Order

1. **Baseline:** with today's web build (the interpreter), Node pumpitea for 60 s: steps, HLE boundaries, buffer swaps.
2. **Link:** the web build links `rex86_host_web` (from `repiu_exe`, under Emscripten only).
3. **Turn it on:** after the `emulated` trampoline makes its `Cpu`, `SetTranslation` according to
   `REPIU_REX86_TRANSLATION` and `REPIU_REX86_TRANSLATION_THRESHOLD` (decisions 1, 2); on failure, one line and the
   interpreter.
4. **Report:** the translation statistics in one line at the end of a run (decision 3).
5. **Check:** the design's verification 1 to 3; the trace comparison with `off` and `wasm` (thresholds 32 and 0).
6. **Measure:** `off` and `wasm` alternating, at least twice each (decision 4).
7. **Docs:** the work log, `src/platform/web/README.md`, `ARCHITECTURE.md`, `docs/analysis/web-port-frontier.md`,
   `docs/TODO.md`.

## Done when

* The web build turns translation on by default and `off` turns it off.
* The trace matches `off` (up to the first wall-clock dependence). If it does not, the difference goes to a rex86 issue
  and the default stays `off`.
* MIPS and game progress before and after, and the translated share, are recorded.
