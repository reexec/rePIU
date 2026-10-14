# 작업 로그: rex86 위의 `emulated` 실행 모델 (issue #61)

설계: [20261014-i061](../design/20261014-i061-emulated-execution-model.md) ·
작업 지시: [20261014-i061](../work-orders/20261014-i061-emulated-execution-model.md) ·
rex86: [#69](https://github.com/reexec/rex86/issues/69)

## 2026-10-14 — 1차: 연결과 `direct`+`legacy` 대조

브랜치 `work/20261014-i061-emulated-model`(main `814d6c9`, v0.0.218에서). rex86은 로컬 `../rex86`의
`work/i069-user-mode-privilege`(`cbe5cc2`)를 `FETCHCONTENT_SOURCE_DIR_REX86`으로 썼습니다.

### 구현

* **rex86 #69:** `Features::user_mode`. CLI/STI/HLT/IN/OUT/INS/OUTS는 실행 전 특권 폴트, `INT imm8`은 실행 전 #GP,
  POPF/IRET는 IF·IOPL을 바꾸지 않습니다. Windows x86 MSVC 단위 테스트 2,498건, WSL Linux x64 GCC 2,482건 통과.
* **rePIU:** 설계 결정 8대로 `emulated` 모델 파일 넷, 플랫폼의 메모리 변경 관찰자와 호스트 진입 상태, 스레드
  프로시저와 종료 정지, 로더의 `legacy` 고정, HLE 경계 trace(`REPIU_HLE_BOUNDARY_TRACE`).

### 대조로 찾아 고친 것

pumpitea, `legacy` 백엔드에서 두 모델의 HLE 경계 trace를 비교하며 차례로 고쳤습니다. 각 항목은 네이티브
프로세스가 **암묵적으로** 해 주던 일입니다.

| 증상 | 원인 | 조치 |
|---|---|---|
| 첫 단일 스텝에서 `guest-stack-not-entered`로 거절 | "게스트 실행 중" 판단이 스택 전환 어셈블리가 쓰는 `host_esp`에 기댐 | `ThreadContext::emulated_entry_active`를 guard에 더함 |
| 4번째 미러링 뒤 모든 새 페이지가 HLE로 감 | 미러링 재시도 한도가 실행 전체에 걸려 있었음(제 결함) | 명령이 retire되면 한도 초기화 |
| `mov ax, gs`가 0x2B(호스트)를 읽음, 네이티브는 0x20 | `ReadGuestSegmentSelector`가 진입 시 호스트 셀렉터 전역 `g_recovery_host_*`와 비교 — 어셈블리만 채움 | 진입에서 전역을 채움 |
| DS가 0이 됨, 네이티브는 0x2B 유지 | **WOW64는 CONTEXT의 세그먼트 레지스터 쓰기를 반영하지 않음**(네이티브 trace의 셀렉터가 HLE의 쓰기 뒤에도 늘 `0023/002B/002B/002B/0053/002B`) | 재개할 때 컨텍스트의 셀렉터를 무시 |
| native fast path 뒤 단일 스텝으로 돌아오지 않음 | fast path가 **`Dr0`/`Dr7` 실행 breakpoint**로 복귀를 잡음 | DR0~DR3 실행 breakpoint를 게이트로, 명중을 `Dr6` 비트와 함께 단일 스텝 예외로 보고, RF 처리 |
| (동작 차이 없음) 단일 스텝 이벤트의 EFLAGS | Windows는 단일 스텝 예외 CONTEXT에서 TF를 지움 | 같게 지움 |

### 결과

* **처음 2,121개 HLE 디스패치가 완전히 일치합니다**(호스트에서 물려받는 ECX·EBP·ESI·EDI 초기값, 시프트 뒤
  미정의 AF, TF 제외). 첫 차이는 `INT 21h AH=2Ch`가 돌려준 벽시계 시각입니다. 게임은 이 값으로 지연 보정 루프를
  돌기 때문에, 이 뒤부터 두 실행은 시간에 따라 갈라지며 이것은 결함이 아닙니다.
* 이후에도 진행도(`progress=2506`), fast path 통계(`36/36/0`), 마지막 EIP 구역이 같습니다.
* **Release에서 두 모델 모두 `0x040FD010`의 `gs: cmp byte [eax], 6Ah`(EAX=0x9FDD)에서 처리되지 않은 접근 위반으로
  멈춥니다.** `emulated`가 3초, `direct`가 6초에 도달했습니다. `legacy` HLE가 이 명령을 에뮬레이션하지 못하는
  것이고, `emulated`가 `legacy`를 한계까지 그대로 재현한다는 증거입니다. 평소 실행은 `dynamic`(AOT) 백엔드라
  드러나지 않았던 한계입니다.
* 속도: `legacy`는 명령마다 단일 스텝과 HLE 디스패치를 거칩니다. Debug에서 두 모델 모두 초당 약 2.5만 디스패치,
  Release `emulated`는 초당 약 25만 디스패치입니다.

### 발견한 것 (부수)

* `x87_context.cpp`의 `PushX87Float`는 `FloatSave.RegisterArea`를 물리 슬롯 순서로 다룹니다. Windows의 FNSAVE
  형식은 ST(i) 순서입니다. 호출하는 곳이 없어 지금은 영향이 없습니다.

### 남은 것 — 방향 결정 필요

`legacy` 위에서는 타이틀에 닿을 수 없습니다(위 멈춤). 그리고 명령 단위 HLE 디스패치는 너무 느립니다. 다음은
**rex86이 게스트의 가상 셀렉터를 실제 디스크립터로 들고 실행하는 방식**(설계의 "범위 밖" 항목)이 유력합니다.
그러면 세그먼트 override 명령이 HLE 없이 맞게 실행되고 TF 단일 스텝이 필요 없어집니다. 사용자 결정을 받아
설계를 갱신합니다.

### 재현

```bash
cmake -S . -B build/win32_x86_emulated -G "Visual Studio 17 2022" -A Win32 \
  -DREPIU_EXECUTION_MODEL_OVERRIDE=emulated \
  -DFETCHCONTENT_SOURCE_DIR_REX86=E:/MYWORK/Projects/rex86
cmake --build build/win32_x86_emulated --config Release --target repiu -- -m
# cmd: set REPIU_HLE_BOUNDARY_TRACE=emu-trace.txt && build\win32_x86_emulated\Release\repiu.exe pumpitea
# 기준: set REPIU_EXECUTION_BACKEND=legacy && build\Release\repiu.exe pumpitea
```

---

# Work log: the `emulated` execution model on rex86 (issue #61)

Design: [20261014-i061](../design/20261014-i061-emulated-execution-model.md) ·
Work order: [20261014-i061](../work-orders/20261014-i061-emulated-execution-model.md) ·
rex86: [#69](https://github.com/reexec/rex86/issues/69)

## 2026-10-14 — first pass: the connection and the comparison against `direct` + `legacy`

Branch `work/20261014-i061-emulated-model` (from main `814d6c9`, v0.0.218). rex86 was the local `../rex86` on
`work/i069-user-mode-privilege` (`cbe5cc2`) through `FETCHCONTENT_SOURCE_DIR_REX86`.

### Implementation

* **rex86 #69:** `Features::user_mode`. CLI/STI/HLT/IN/OUT/INS/OUTS raise a privileged fault before retiring, `INT imm8`
  raises #GP before retiring, and POPF/IRET leave IF and IOPL alone. 2,498 unit checks pass on Windows x86 MSVC and
  2,482 on WSL Linux x64 GCC.
* **rePIU:** per design decision 8, the four `emulated` model files, the platform's memory change observer and host
  entry state, the thread procedure and the stop on shutdown, the loader pinning `legacy`, and the HLE boundary trace
  (`REPIU_HLE_BOUNDARY_TRACE`).

### What the comparison found and fixed

Comparing the two models' HLE boundary traces on pumpitea with the `legacy` backend led to the fixes below, one after
another. Each is something the native process did **implicitly**.

| Symptom | Cause | Fix |
|---|---|---|
| The first single step declined as `guest-stack-not-entered` | "a guest is running" was judged by `host_esp`, which only the stack switch assembly writes | `ThreadContext::emulated_entry_active` added to the guard |
| Every new page went to the HLE after the fourth mirroring | the mirroring retry bound spanned the whole run (my defect) | the bound resets once an instruction retires |
| `mov ax, gs` read 0x2B (the host's), natively 0x20 | `ReadGuestSegmentSelector` compares against the entry-time host selectors in `g_recovery_host_*`, which only the assembly fills | the entry fills those globals |
| DS became 0, natively it stays 0x2B | **WOW64 does not apply segment registers written into a CONTEXT** (the native trace shows `0023/002B/002B/002B/0053/002B` after every HLE write) | resuming ignores the context's selectors |
| No return to single-stepping after the native fast path | the fast path catches its return with a **`Dr0`/`Dr7` execute breakpoint** | DR0-DR3 execute breakpoints become gates, a hit is reported as a single step with its `Dr6` bit, RF handled |
| (no behavioural difference) EFLAGS of a single-step event | Windows clears TF in a single-step exception's CONTEXT | cleared the same way |

### Results

* **The first 2,121 HLE dispatches match exactly** (excluding the ECX, EBP, ESI and EDI values inherited from the host,
  AF left undefined after a shift, and TF). The first difference is the wall-clock time `INT 21h AH=2Ch` returns. The
  game calibrates a delay loop with it, so from there the two runs diverge with time, which is not a defect.
* Progress (`progress=2506`), fast path statistics (`36/36/0`) and the last EIP's region stay the same afterwards.
* **In Release, both models stop at `0x040FD010` on `gs: cmp byte [eax], 6Ah` (EAX=0x9FDD) with an unhandled access
  violation**, `emulated` after 3 seconds and `direct` after 6. The `legacy` HLE does not emulate that instruction,
  which shows `emulated` reproduces `legacy` up to and including its limits. Ordinary runs use the `dynamic` (AOT)
  backend, which is why the limit had not shown.
* Speed: `legacy` single-steps and dispatches every instruction. Both models make about 25,000 dispatches a second in
  Debug; Release `emulated` makes about 250,000.

### Found in passing

* `PushX87Float` in `x87_context.cpp` treats `FloatSave.RegisterArea` as physical-slot order, while Windows' FNSAVE
  layout is ST(i) order. Nothing calls it, so nothing is affected today.

### Remaining — a decision is needed

On `legacy` the title screen cannot be reached (the stop above), and dispatching every instruction to the HLE is too
slow. The likely next step is **rex86 running with the guest's virtual selectors as real descriptors** (the design's
"out of scope" item): segment-override instructions then execute correctly without the HLE, and TF single-stepping is no
longer needed. The design is updated once the user decides.

### Reproducing

See the commands in the Korean section above.
