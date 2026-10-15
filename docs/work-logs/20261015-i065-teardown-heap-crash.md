# 작업 로그: `emulated` 단일 스레드 빌드의 종료 중 힙 손상 크래시 (issue #65)

## 2026-10-15 — 조사 (미해결)

사용자 실행 1회(`emulated`, pumpitea, 음악 뒤 종료)가 `ThreadContext` 파괴 중 `ntdll!RtlFreeHeap`에서 쓰기 접근
위반(`target=0xFFFFFFF8`)으로 끝났습니다. 코드 변경은 없었고, 임시 진단은 모두 되돌렸습니다.

### 확인됨

* 크래시 지점: `RunExecutionThread`가 반환한 뒤 `ThreadContext`의 멤버가 파괴되는 중. `Piu10Mp3AudioOut::Close`의
  통계 줄 바로 뒤이고, 로더 요약은 찍히지 않았습니다.
* 사용자 실행의 MP3 통계(`229378/0/548/631296/1/226148/2144/0/3584`)가 `emulated` + 음악 뒤 창 닫기 재현
  (`229378/0/548/631296/1/226154/2144/0/3584`)과 사실상 같아, 사용자 실행은 `emulated`였습니다.

### 재현 시도 (모두 크래시 없음)

| 방법 | 횟수 |
|---|---|
| `emulated` 시간 제한 30/40/60/360/900초 | 5 |
| `emulated` 음악 뒤 창 닫기(`WM_CLOSE`) | 1 |
| `emulated` 콘솔 Ctrl+C 한 번, 두 번 | 2 (게임은 Ctrl+C에 반응하지 않음) |
| `emulated` + 128번째 호스트 차례마다 `HeapValidate`(모든 프로세스 힙), 420초·900초 | 2 (검사 수천 번, 손상 0) |
| `direct` 시간 제한 70초 | 4 |
| `direct` 음악 뒤 창 닫기 | 2 |

### 기각한 것

* 게스트가 1 MB 아래 호스트 페이지(단일 스레드에서 메인 스레드 스택이 `0x000D0000` 근처)를 건드린다: 60초 동안
  미러링 2,050번 중 16 MB 아래 0번.
* 스택 크기 차이: 메인 스레드와 이전 게스트 스레드 모두 1 MB.

### 쓰지 못한 도구

* MSVC ASan(x86): ASan이 낮은 주소를 차지해 게스트 재배치 후보 `0x01000000`~`0x09000000`이 모두 막혀 게스트를 올리지
  못함.
* page heap(Application Verifier): 관리자 권한 필요.

### 함께 본 것

* `direct` 실행이 Task 736 모양(타이머 ISR 중첩으로 게스트 스택이 넘침, `0x0402AB87` 프레임 반복)의 게스트 접근
  위반으로 끝난 적이 두 번 있습니다(56초, 58초). 둘 다 다른 무거운 작업 직후였습니다.
* #64가 원인인지 보려고 변경 전(v0.0.219, `0b613c8`) `direct` 빌드와 지금 빌드를 70초씩 번갈아 돌렸습니다.

  | 조건 | 변경 전 | 변경 후 |
  |---|---|---|
  | 한가한 PC | 0/6 | 0/6 |
  | WSL 바쁜 루프 3개로 부하 | 0/5 | 0/5 |
  | 이 작업 전체의 다른 `direct` 실행 | — | 2/15 |

  드문 현상이라 이 횟수로는 통계적으로 가를 수 없습니다. #64는 `direct`의 tick 주입 경로를 바꾸지 않았고(규칙을 옮긴
  `HostTurn`), 모양이 기존 Task 736 기록과 같으므로 **기존 문제로 봅니다(추정)**. 별도 issue로 다룰 후보입니다.

### 다음

* 사용자 실행의 종료 방법과 그 사이의 조작(OSD, 키, 창 크기, 전체 화면, 포커스 전환)을 확인합니다.

---

# Work log: a heap-corruption crash in the teardown of the `emulated` one-thread build (issue #65)

## 2026-10-15 — investigation (unresolved)

One user run (`emulated`, pumpitea, ended after the music) ended in a write access violation (`target=0xFFFFFFF8`) in
`ntdll!RtlFreeHeap` while `ThreadContext` was destroyed. No code changed; every temporary diagnostic was reverted.

### Confirmed

* Where: after `RunExecutionThread` returned, while `ThreadContext`'s members were destroyed, just after
  `Piu10Mp3AudioOut::Close`'s statistics line and before the loader's summary.
* The user's MP3 statistics (`229378/0/548/631296/1/226148/2144/0/3584`) match an `emulated` close-after-music
  reproduction (`229378/0/548/631296/1/226154/2144/0/3584`), so the user's run was `emulated`.

### Reproduction attempts (none crashed)

| How | Runs |
|---|---|
| `emulated`, timeouts of 30/40/60/360/900 s | 5 |
| `emulated`, closing the window after the music (`WM_CLOSE`) | 1 |
| `emulated`, console Ctrl+C once and twice | 2 (the game does not react to Ctrl+C) |
| `emulated` with `HeapValidate` on every process heap every 128th host turn, 420 s and 900 s | 2 (thousands of checks, no corruption) |
| `direct`, 70 s timeout | 4 |
| `direct`, closing the window after the music | 2 |

### Ruled out

* The guest touching host pages below 1 MB (where the main thread's stack sits on one thread, near `0x000D0000`): of
  2,050 mirrored runs in 60 s, none below 16 MB.
* A stack size difference: both the main thread and the former guest thread have 1 MB.

### Tools that could not be used

* MSVC ASan (x86): ASan takes low addresses, every relocation candidate `0x01000000` to `0x09000000` was occupied, and
  the guest could not be loaded.
* Page heap (Application Verifier): needs administrator rights.

### Seen alongside

* `direct` runs ended twice (at 56 s and 58 s) with a guest access violation shaped like Task 736 (the timer ISR
  nesting until the guest stack overflows, `0x0402AB87` frames repeating), both just after another heavy job.
* To see whether #64 caused it, the `direct` build from before (v0.0.219, `0b613c8`) and today's were alternated, 70 s
  each.

  | Condition | Before | After |
  |---|---|---|
  | Idle PC | 0/6 | 0/6 |
  | Loaded with three busy loops in WSL | 0/5 | 0/5 |
  | Other `direct` runs during this work | — | 2/15 |

  Too rare to separate statistically at these counts. #64 did not change `direct`'s tick injection path (the
  `HostTurn` moved its rules), and the shape matches the existing Task 736 record, so it is **taken to be an existing
  problem (inferred)**; a candidate for an issue of its own.

### Next

* Find out how the user's run ended and what happened in it (the OSD, keys, window size, fullscreen, focus changes).
