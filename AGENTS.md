# AGENTS.md

## 프로젝트 개요

이 프로젝트는 DOSBox나 전체 시스템 에뮬레이터를 사용하지 않고, 레거시 DOS/4G 게임을 현대 환경에서 실행하는 것을 목표로 한다.

목표는 게임 로직을 재구현하는 것이 아니다.

목표는 원본 32-bit x86 코드를 실행하면서, 주변 DOS 환경만 High Level Emulation(HLE) 계층으로 대체하여 원본 게임 로직을 보존하는 것이다.

모든 구현 결정은 이 원칙을 따른다.

## Project Overview

This project aims to run a legacy DOS/4G game on modern systems without using DOSBox or a full-system emulator.

The goal is not to reimplement the game logic.

The goal is to preserve the original game logic by executing the original 32-bit x86 code while replacing only the surrounding DOS environment with a High Level Emulation (HLE) layer.

All implementation decisions must follow this principle.

---

## 핵심 설계 목표

우선순위:

1. 원본 게임 로직을 보존한다.
2. 가능하면 원본 실행 파일 코드를 수정하지 않는다.
3. DOS/DPMI 서비스를 HLE 구현으로 대체한다.
4. 네이티브 Win32 호스트를 구축한다.
5. DOSBox 소스 코드를 통합하지 않는다.
6. 여러 게임 버전과 런타임 경로를 지원할 수 있는 공용 구조를 우선 설계한다.

## Primary Design Goals

Priority order:

1. Preserve original game logic.
2. Avoid modifying original executable code whenever possible.
3. Replace DOS/DPMI services with HLE implementations.
4. Build a native Win32 host.
5. Do not integrate DOSBox source code.
6. Design shared structures first so multiple game versions and runtime paths can be supported.

---

## 참조 문서

아키텍처 변경 전에는 관련 문서를 먼저 읽고 갱신한다.

* `docs/PROJECT_CHARTER.md`
* `ARCHITECTURE.md`
* `docs/DOS4G_HLE_PORTING_PLAN.md`
* `docs/EXE_DESIGN.ko.md`
* `docs/EXE_DESIGN.en.md`
* `docs/CODING_STYLE.md`
* `docs/design/`
* `docs/work-orders/`
* `docs/work-logs/`
* `docs/analysis/`
* `docs/kb/`

## Reference Documents

Read and update the relevant documents before making architectural changes.

* `docs/PROJECT_CHARTER.md`
* `ARCHITECTURE.md`
* `docs/DOS4G_HLE_PORTING_PLAN.md`
* `docs/EXE_DESIGN.ko.md`
* `docs/EXE_DESIGN.en.md`
* `docs/CODING_STYLE.md`
* `docs/design/`
* `docs/work-orders/`
* `docs/work-logs/`
* `docs/analysis/`
* `docs/kb/`

---

## 핵심 원칙

1. 사용자 요구사항을 받으면 바로 구현하지 말고 먼저 설계를 작성하거나 기존 설계를 갱신한다.
2. 설계가 정리되면 구현 계획서를 작성하거나 갱신한 뒤 구현을 시작한다.
3. 코드 변경이 있는 모든 작업은 작업 지시 문서와 작업 로그 문서를 남긴다.
4. 문서는 기본적으로 한국어를 먼저 쓰고 바로 아래에 영어 번역을 추가한다. 이 규칙은 `docs/` 아래 문서와 `README.md`, `ARCHITECTURE.md` 같은 저장소 문서에만 적용되며, 소스 코드 주석에는 적용하지 않는다.
5. 기존 코드, 자산, 문서의 의도를 확인한 뒤 수정하며, 큰 구조 변경은 근거를 문서에 남긴다.
6. 멀티플랫폼 공용 구조를 우선 설계하고, 플랫폼별 세부 사항은 분리한다.

## Core Principles

1. When a user requirement is received, do not implement immediately. First write a design or update the existing design.
2. After the design is clear, write or update the implementation work order before starting implementation.
3. Every task that changes code must leave a work-order document and a work-log document.
4. Documents should be written in Korean first, followed immediately by an English translation. This applies to documents under `docs/` and repository documents such as `README.md` and `ARCHITECTURE.md`; it does not apply to source-code comments.
5. Confirm the intent of existing code, assets, and documents before modifying them. Document the rationale for large structural changes.
6. Design shared multiplatform structures first, and keep platform-specific details separated.

---

## 응답 태도 규칙

* 모든 질문에 대한 답변은 존대말을 사용하고 예의 있는 태도를 유지한다.

## Response Tone Rules

* Use polite Korean speech and maintain a respectful attitude for all answers.

---

## 요구사항 처리 절차

1. 요구사항 접수
2. 관련 코드, 문서, 자산 맥락 확인
3. 설계 문서 작성 또는 갱신
4. 구현 계획서 작성 또는 갱신
5. 사용자 승인 또는 현재 진행 맥락 확인
6. 구현
7. 빌드 또는 범위에 맞는 검증
8. 작업 로그 작성

단, 요구사항이 단순 질문이거나 확인 요청이라면 해당 내용에 바로 응답하고 문서화 및 코드 구현은 하지 않는다.

## Requirement Handling Procedure

1. Receive the requirement.
2. Inspect the relevant code, documentation, and asset context.
3. Write or update the design document.
4. Write or update the implementation work order.
5. Confirm user approval or the current active context.
6. Implement.
7. Run a build or verification appropriate to the scope.
8. Write the work log.

If the requirement is a simple question or confirmation request, answer it directly without documentation or code implementation.

---

## 문서 작성 규칙

* 설계 문서는 `docs/design/` 아래에 둔다.
* 계획 문서는 `docs/work-orders/` 아래에 둔다.
* 작업 결과와 회고는 `docs/work-logs/` 아래에 둔다.
* 사용자가 직접 수행하는 검증·측정·운영 절차는 `docs/guides/` 아래에 둔다. 특정 작업의
  일회성 증거가 아니라 반복 수행 가능한 절차만 두고, 근거가 되는 작업 로그와 설계를
  링크한다.
* 프로젝트의 큰 방향성은 `docs/PROJECT_CHARTER.md`에 반영한다.
* 현재 구현되는 코드의 설계와 구조는 `ARCHITECTURE.md`에 지속적으로 반영한다.
* 원본 파일 분석으로 확인한 구조와 설계는 `docs/EXE_DESIGN.ko.md`, `docs/EXE_DESIGN.en.md`에 누적 반영한다.
* 규칙이 바뀌면 `AGENTS.md`와 관련 문서를 함께 갱신한다.
* 작업 단위 문서 파일명은 가능하면 `YYYYMMDD-i###-slug.md` 형식을 사용한다. `###`은 task의 GitHub issue 번호를 세 자리로 맞춘 값이다(예: issue #6 → `20261005-i006-slug.md`). issue로 바꾸기 전의 문서(Task 773까지)는 `YYYYMMDD-###-slug.md`로 남아 있으며, 이 번호는 저장소 안에서 매긴 순번이다.
* Markdown 문서에서 구조, 관계, 흐름, 호출 순서, 상태 전이, 주소 변환을 도식화할 수 있으면 이해를 돕기 위해 Mermaid를 적극적으로 사용한다.
* 세 개 이상의 구성요소·단계·분기·계층이 있으면 Mermaid 적용 가능성을 검토하고, flowchart, sequenceDiagram, stateDiagram, classDiagram 등 내용에 맞는 형식을 선택한다.
* 도식이 본문 이해를 실질적으로 개선하는 경우 최대한 포함하되, 단순 사실 하나나 한 단계 설명에는 불필요한 도식을 강제하지 않는다.
* Mermaid 도식은 본문과 같은 사실 및 확인 상태를 표현해야 하며, 본문 설명을 완전히 대체하지 않는다.

## Documentation Rules

* Put design documents under `docs/design/`.
* Put work-order documents under `docs/work-orders/`.
* Put work results and retrospectives under `docs/work-logs/`.
* Put procedures the user runs themselves — verification, measurement, operations —
  under `docs/guides/`. Keep only repeatable procedures there rather than one-off
  evidence, and link the design and work log they rest on.
* Put project-specific binary findings, confirmed behavior, hypotheses, and unresolved questions under `docs/analysis/`, organized by topic.
* Put generally applicable technical definitions and background knowledge under `docs/kb/`, organized by topic and linked to external sources when derived from them.
* Reflect the project's broad direction in `docs/PROJECT_CHARTER.md`.
* Continuously reflect the design and structure of currently implemented code in `ARCHITECTURE.md`.
* Accumulate original executable analysis findings and design notes in `docs/EXE_DESIGN.ko.md` and `docs/EXE_DESIGN.en.md`.
* When rules change, update `AGENTS.md` and the related documents together.
* Use the `YYYYMMDD-i###-slug.md` filename format for task documents whenever possible, where `###` is the task's GitHub issue number padded to three digits (for example issue #6 → `20261005-i006-slug.md`). Documents from before the switch to issues (up to Task 773) keep `YYYYMMDD-###-slug.md`, whose number was a sequence kept in the repository.
* Actively use Mermaid in Markdown documents whenever structure, relationships, flows, call sequences, state transitions, or address translation can be visualized to improve understanding.
* Evaluate Mermaid whenever content has three or more components, steps, branches, or layers, choosing an appropriate form such as flowchart, sequenceDiagram, stateDiagram, or classDiagram.
* Include useful diagrams as broadly as practical when they materially improve the prose, but do not force them into single-fact or one-step explanations.
* Mermaid diagrams must express the same facts and confirmation status as the prose and must not completely replace the written explanation.

## 분석 및 지식 기반 유지 규칙

* 바이너리 분석이나 구현 검증으로 새로운 사실이 확인되거나 기존 결론이 바뀌면 같은 작업에서 관련 `docs/analysis/` 문서를 갱신한다.
* 작업 중 프로젝트 이해에 중요한 새로운 용어, 표준, interrupt, 실행 형식, CPU 동작이 등장하면 관련 `docs/kb/` 문서를 작성하거나 갱신한다.
* 외부 자료에서 얻은 기술 지식에는 가능한 한 원 사양, CPU 제조사 문서, 공식 프로젝트 문서 같은 권위 있는 출처 링크를 가까운 위치에 남긴다.
* `docs/analysis/`는 확인됨, 추정, 미확정 내용을 구분한다.
* analysis 또는 kb 파일을 추가하거나 이름을 바꾸면 해당 디렉터리의 `README.md` 링크 색인을 같은 작업에서 갱신한다.
* `docs/work-logs/`는 시간순 작업 증거로 유지하고, `docs/analysis/`와 `docs/kb/`는 주제별 누적 문서로 유지한다.
* 요구사항 맥락을 확인한 뒤 설계 전에 관련 analysis와 kb 문서를 함께 확인하고, 구현·검증 후 새로 얻은 내용을 반영한다.

## Analysis and Knowledge Base Maintenance Rules

* When binary analysis or implementation verification confirms a new fact or changes an earlier conclusion, update the relevant `docs/analysis/` topic in the same task.
* When important new terminology, standards, interrupts, executable formats, or CPU behavior arise, create or update the relevant `docs/kb/` topic.
* Place authoritative external links near derived technical knowledge, preferring original specifications, CPU-vendor manuals, and official project documentation.
* Distinguish confirmed, inferred, and unresolved statements in `docs/analysis/`.
* When adding or renaming an analysis or knowledge file, update the linked index in that directory's `README.md` in the same task.
* Keep `docs/work-logs/` as chronological evidence, while maintaining `docs/analysis/` and `docs/kb/` as cumulative topic references.
* After inspecting requirement context, review relevant analysis and knowledge topics before design, then incorporate new findings after implementation and verification.

---

## 구현 규칙

* 플랫폼(OS) 종속 코드는 `src/platform/win32/`, `src/platform/linux/`, `src/platform/web/` 아래에 둔다. 엔진·런타임·HLE·도구 디렉터리에는 플랫폼·아키텍처 하위 디렉터리를 두지 않는다. 플랫폼 계층은 엔진의 타입을 알지 못한다: 엔진은 주소·버퍼·콜백 같은 기본 값으로 플랫폼 함수를 부르고, 엔진의 흐름과 엔진 타입을 다루는 코드는 엔진에 남긴다.
* 엔진의 CPU 아키텍처 차이는 실행 모델로 표현한다. 실행 모델은 `direct`(게스트의 바이트를 그대로 실행: Win32, Linux i386), `cache`(번역한 long mode 코드 캐시를 실행: Linux x64), `emulated`(rex86 CPU 코어가 게스트를 실행: 웹과 ARM 호스트용, Win32에서 개발용으로 빌드) 셋이다. 게스트를 실행하지 않는 빌드는 `none`이다. 엔진은 `include/repiu/runtime/execution_model.h`로 묻고, 모델에 따라 다른 구현은 공용 파일 옆에 `<이름>_direct.cpp`, `<이름>_cache.cpp`, `<이름>_emulated.cpp`로 두며 CMake가 `REPIU_EXECUTION_MODEL`로 고른다. 기능을 켤지 여부나 한 호스트에서만 만든 진단을 아키텍처 디렉터리로 나누지 않는다.
* 예외: Win32의 예외 진입(`execution_trampoline.cpp`의 Win32 스레드 프로시저와 `DispatchGuestException`, `exception_rescue_win32.cpp`)과 종료 복구의 vectored handler는 엔진의 상태를 쥐고 있어 엔진에 남아 있다. 이것을 플랫폼 계층의 콜백으로 바꾸는 일은 별도 작업이며, 그 전에 같은 모양의 Win32 코드를 엔진에 더하지 않는다.
* 한 플랫폼 안에서 CPU 아키텍처에 따라 갈리는 코드는 그 플랫폼 디렉터리의 아키텍처 하위 디렉터리(예: `src/platform/linux/x86/`, `src/platform/linux/x64/`)에 두고, 플랫폼 디렉터리 자체에는 아키텍처 공용 코드만 둔다. 한 파일 안에 아키텍처 분기가 섞이면 공용 부분과 아키텍처 부분으로 나눈다. 공개 헤더도 같은 규칙을 따라 `include/repiu/platform/<플랫폼>/`과 그 아키텍처 하위 디렉터리(예: `include/repiu/platform/linux/x64/`)에 두고, 모든 호스트가 쓰는 플랫폼 중립 헤더만 `include/repiu/platform/`에 둔다.
* 새 기능을 추가할 때는 테스트 전략 또는 최소 검증 절차를 문서에 함께 남긴다.
* 코드 수정이 있는 작업은 영향 범위에 맞는 빌드 검증을 수행하고, 불가능하면 이유를 작업 로그에 남긴다.
* 자산, 런타임 경로, 플랫폼 분기 정책은 코드와 문서에서 함께 관리한다.
* 코딩 스타일 세부 규칙은 `docs/CODING_STYLE.md`에서 관리한다.
* 소스 코드 주석은 영어로만 작성한다. 한국어 주석과 한국어·영어 이중 언어 주석을 남기지 않으며, 기존 파일을 수정하다 발견하면 같은 작업에서 영어로 정리한다. 세부 규칙은 `docs/CODING_STYLE.md`의 주석 언어 항목을 따른다.
* 큰 기능은 하나의 거대 파일에 누적하지 않는다. 플랫폼 공용 상태/정책, 플랫폼 backend, guest ABI 연결처럼 책임이 다른 부분을 별도 header/source로 분리한다.
* 기존 파일에 기능을 추가할 때 독립적으로 이름 붙일 수 있는 하위 시스템이면 전용 파일로 추출하고, 통합 지점에는 orchestration과 adapter 코드만 남긴다.
* 프로젝트 기본 라이선스는 `BSD 3-Clause License`를 기준으로 한다.
* 서드파티 오픈소스 도입 시 GPL, LGPL, AGPL 같은 전염성 라이선스는 사용하지 않는다.
* 서드파티 라이선스는 도입 전에 확인하고, 예외가 필요하면 먼저 문서화와 사용자 확인을 거친다.

## Implementation Rules

* Put platform (OS) specific code under `src/platform/win32/`, `src/platform/linux/`, or `src/platform/web/`. The engine, runtime, HLE and tool directories have no platform or architecture subdirectories. The platform layer knows none of the engine's types: the engine calls platform functions with plain values (addresses, buffers, callbacks), and code that holds the engine's flow or its types stays in the engine.
* Express the engine's difference by CPU architecture as an execution model. There are three: `direct` (the guest's bytes run as they are: Win32, Linux i386), `cache` (the translated long-mode code cache runs: Linux x64) and `emulated` (the rex86 CPU core runs the guest: for the web and ARM hosts, built on Win32 for development). A build that runs no guest is `none`. The engine asks through `include/repiu/runtime/execution_model.h`; implementations that differ by model sit next to their shared file as `<name>_direct.cpp`, `<name>_cache.cpp` and `<name>_emulated.cpp`, picked by CMake with `REPIU_EXECUTION_MODEL`. Do not turn whether a feature is on, or a diagnostic built on one host, into architecture directories.
* The exception: Win32's exception entry (the Win32 thread procedure and `DispatchGuestException` in `execution_trampoline.cpp`, and `exception_rescue_win32.cpp`) and the shutdown recovery's vectored handlers hold engine state and remain in the engine. Turning them into callbacks of the platform layer is a task of its own; until then, add no more Win32 code of that shape to the engine.
* Within one platform, put code that differs by CPU architecture in that platform directory's architecture subdirectory (for example `src/platform/linux/x86/` and `src/platform/linux/x64/`), and keep only architecture-shared code in the platform directory itself. When one file mixes architecture branches, split it into a shared part and per-architecture parts. Public headers follow the same rule under `include/repiu/platform/<platform>/` and its architecture subdirectories (for example `include/repiu/platform/linux/x64/`); only platform-neutral headers every host uses stay in `include/repiu/platform/`.
* When adding a feature, document the test strategy or minimum verification procedure.
* For tasks that modify code, run build verification appropriate to the impact. If verification is impossible, record the reason in the work log.
* Manage assets, runtime paths, and platform branching policy in both code and documentation.
* Maintain detailed coding style rules in `docs/CODING_STYLE.md`.
* Write source-code comments in English only. Leave no Korean or bilingual comments, and convert any found while editing an existing file in the same task. The detailed rule is the comment-language section of `docs/CODING_STYLE.md`.
* Do not accumulate major features in one monolithic file. Separate platform-neutral state/policy, platform backends, and guest ABI integration into dedicated headers and sources.
* When adding to an existing file, extract any independently named subsystem and leave only orchestration or adapter code at the integration point.
* Use the `BSD 3-Clause License` as the default project license baseline.
* Do not introduce third-party open source with copyleft licenses such as GPL, LGPL, or AGPL.
* Check third-party licenses before adoption. If an exception is needed, document it and confirm with the user first.

---

## 작업 단위 규칙

* task는 GitHub issue로 만든다. task 번호는 GitHub issue 번호를 쓴다. Task 773까지의 번호는 저장소 안에서 매긴 순번이었고, issue #4부터는 issue 번호다.
* 기존 task의 후속 작업(남은 검증, 후속 수정)은 새 issue를 만들지 않고 그 task의 issue에 붙인다.
* 커밋 제목 끝에 관련 issue 번호를 `(#6)`처럼 붙인다.
* 의미 있는 작업마다 하나의 작업 지시 문서를 만든다.
* 작업이 끝나면 대응되는 작업 로그를 남긴다.
* 설계 없이 바로 코드만 추가하지 않는다.
* 초기 구조 작업이라도 디렉터리 목적과 향후 확장 방향을 문서로 남긴다.

## Task Unit Rules

* A task is created as a GitHub issue, and the task number is the GitHub issue number. Numbers up to Task 773 were a sequence kept in the repository; from issue #4 on they are issue numbers.
* Follow-up work on an existing task (verification left open, a follow-up fix) goes to that task's issue rather than a new one.
* End commit titles with the related issue number, as in `(#6)`.
* Create one work-order document for each meaningful task.
* When the task is complete, leave the corresponding work log.
* Do not add code directly without a design.
* Even for initial structure work, document the directory purpose and future extension direction.

---

## Git 작업 규칙

* 사용자가 작업을 요청하면 먼저 현재 Git 브랜치명을 확인한다.
* 현재 브랜치가 `main`이면 사용자가 요청한 작업 내용을 바탕으로 작업용 브랜치를 새로 만든 뒤 작업한다.
* 현재 브랜치가 `main`이 아니면 새 브랜치를 만들지 않고 그 브랜치에서 이어서 작업한다. 다른 issue의 작업이어도 같다 — 한 작업 브랜치에 여러 issue의 커밋이 쌓일 수 있고, 커밋마다 제목 끝에 해당 issue 번호를 붙여 구분한다. 브랜치는 머지할 때 정리된다.
* 작업 단위가 하나 끝날 때마다 관련 변경을 Git 커밋으로 남긴다.
* 프로젝트 버전은 저장소 루트의 `VERSION` 파일에서 `major.minor.patch` 형식으로 관리한다.
* 사용자가 머지를 요청하면 `main`에 머지하기 전에 patch 버전을 1 증가시킨다.
* 사용자가 minor 버전 증가를 요청하면 minor 버전을 1 증가시키고 patch 버전은 0으로 리셋한다.
* 사용자가 major 버전 증가를 요청하면 major 버전을 1 증가시키고 minor와 patch 버전은 0으로 리셋한다.
* 사용자가 머지를 요청하면 작업 브랜치를 원격에 push하고 GitHub pull request를 만든 뒤(머지 요청이 이 push와 PR 생성의 승인이다), 그 PR을 squash merge해 `main`에 넣는다. 머지한 뒤 `git pull`로 로컬 `main`을 원격과 같게 갱신한다.
* PR 본문에는 해결하는 issue를 `Closes #6`처럼 적는다. 여러 issue를 해결하면 `Closes #6, closes #8`처럼 issue마다 적는다.
* squash merge의 커밋 제목은 작업 브랜치 안의 커밋 제목들을 확인하고, 전체 변경 내용을 잘 표현하는 제목으로 만든다.
* `main`에 머지한 뒤에는 그 머지 커밋에 `VERSION`과 같은 값의 annotated tag를 `vmajor.minor.patch` 형식으로 붙인다. 예: `VERSION`이 `0.0.81`이면 `v0.0.81`.
* tag 메시지에는 해당 버전의 핵심 변경을 한 줄로 남긴다.
* tag는 로컬까지만 만들고 원격 push는 사용자가 직접 수행한다.
* 머지가 완료되면 현재 작업 브랜치를 삭제한다.
* 릴리스 노트(`docs/release-notes/v<version>.md`)에는 해결된 issue 번호와 그 커밋 ID를 함께 적는다.

## Git Workflow Rules

* When the user requests work, first check the current Git branch name.
* If the current branch is `main`, create a task branch based on the user's requested work before making changes.
* If the current branch is not `main`, do not create a new branch; keep working on the current one, even for a different issue. One task branch may carry commits for several issues, each commit title ending with its own issue number; the branch is settled when it is merged.
* Leave a Git commit for the related changes whenever one task unit is complete.
* Manage the project version in the repository-root `VERSION` file using `major.minor.patch`.
* When the user requests a merge, increment the patch version by 1 before merging into `main`.
* When the user requests a minor version bump, increment the minor version by 1 and reset the patch version to 0.
* When the user requests a major version bump, increment the major version by 1 and reset the minor and patch versions to 0.
* When the user requests a merge, push the task branch to the remote, open a GitHub pull request from it (the merge request approves this push and the pull request), and squash-merge it into `main`. After the merge, update local `main` to match the remote with `git pull`.
* Name the issues the pull request resolves in its description, as in `Closes #6`. When it resolves several, name each one, as in `Closes #6, closes #8`.
* For the squash merge's commit title, inspect the commit titles in the task branch and write one that best describes the complete change.
* After merging into `main`, tag that merge commit with an annotated tag matching `VERSION`, in the form `vmajor.minor.patch`. For example, tag `v0.0.81` when `VERSION` reads `0.0.81`.
* Put a one-line summary of the version's key change in the tag message.
* Create tags locally only; the user pushes them to the remote.
* Delete the task branch after the merge is complete.
* Release notes (`docs/release-notes/v<version>.md`) record the resolved issue numbers together with their commit IDs.

---

## 아키텍처 규칙

* 원본 게임 코드는 항상 주 실행 경로로 남아야 한다.
* 실용적으로 가능하면 전체 CPU 에뮬레이션보다 HLE를 우선한다.
* 운영체제와 하드웨어 인터페이스만 대체한다.
* 절대적으로 필요하지 않으면 게임플레이 로직을 C++로 다시 작성하지 않는다.
* 모든 하위 시스템은 독립적으로 교체 가능해야 한다.

## Architecture Rules

* Original game code must remain the primary execution path.
* Prefer HLE over full CPU emulation whenever practical.
* Replace operating-system and hardware interfaces only.
* Do not rewrite gameplay logic into C++ unless absolutely unavoidable.
* Every subsystem should be replaceable independently.

---

## 개발 철학

여러 구현이 가능할 때는 원본 실행 파일과의 호환성을 보존하면서 리버스 엔지니어링 부담을 최소화하는 방법을 선택한다.

최적화보다 정확성을 우선한다.

## Development Philosophy

When multiple implementations are possible, choose the one that preserves compatibility with the original executable while minimizing reverse engineering effort.

Accuracy is preferred over optimization.
