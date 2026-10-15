# 코딩 스타일

## 기본 원칙

* C++ 코드는 C++20을 기준으로 작성한다.
* 기본 기준은 Google C++ Style Guide를 따른다.
* 프로젝트 예외 규칙은 아래 항목으로 명시한다.
* 원본 실행 파일의 동작 보존을 우선하며, 편의를 위한 게임 로직 재작성은 피한다.
* 플랫폼 공용 코어와 플랫폼 종속 구현을 분리한다.
* 큰 구조 변경은 설계 문서에 근거를 남긴다.
* 새 기능에는 최소 검증 절차를 함께 문서화한다.
* 독립된 책임을 갖는 큰 기능은 전용 header/source pair로 분리한다.
* 공용 data/state와 플랫폼 backend를 섞지 않으며, execution trampoline과 application entry에는 ABI 변환과 orchestration만 둔다.

## Basic Principles

* Write C++ code against C++20.
* Follow the Google C++ Style Guide as the baseline.
* Project-specific exceptions are defined below.
* Prioritize preserving original executable behavior and avoid rewriting game logic for convenience.
* Separate the platform-neutral core from platform-specific implementations.
* Document the rationale for large structural changes in design documents.
* Document a minimum verification procedure with each new feature.
* Give major independently responsible features dedicated header/source pairs.
* Keep shared data/state separate from platform backends, leaving only ABI adaptation and orchestration in execution trampolines and application entry points.

## C++ 기본 기준

* 기본 기준은 Google C++ Style Guide를 따른다.
* 프로젝트 예외 규칙은 아래 항목으로 명시한다.

## C++ Baseline

* Follow the Google C++ Style Guide as the baseline.
* Project-specific exceptions are defined below.

## 프로젝트 예외 규칙

* 여는 중괄호 `{` 는 같은 줄 끝에 두지 않고 다음 줄에 둔다.
* 탭 문자는 사용하지 않는다.
* 들여쓰기는 공백 4칸을 사용한다.
* 함수명은 Google 스타일에 맞춰 `PascalCase`를 사용한다.
* 변수명은 `snake_case`를 사용한다.
* 클래스 멤버 변수는 `snake_case_`를 사용한다.
* C++ 소스 파일 확장자는 `.cpp`를 사용한다.
* C++ 헤더 파일 확장자는 `.h`를 사용한다.

## Project-Specific Exceptions

* Place opening braces `{` on the next line instead of at the end of the current line.
* Do not use tab characters.
* Use four spaces for indentation.
* Use `PascalCase` for function names, following Google style.
* Use `snake_case` for variable names.
* Use `snake_case_` for class member variables.
* Use `.cpp` for C++ source files.
* Use `.h` for C++ header files.

## 주석 언어

* **소스 코드의 주석은 영어로만 작성한다.** 한국어 주석은 남기지 않는다.
* 한국어와 영어를 함께 적은 이중 언어 주석도 두지 않는다. 같은 내용을 두 벌
  유지하면 한쪽만 갱신되어 서로 어긋난다.
* 이 규칙은 **문서 규칙과 다르다.** `docs/` 아래 Markdown 문서는 한국어를 먼저 쓰고
  영어 번역을 덧붙이지만(`AGENTS.md`), 소스 주석은 영어 한 벌만 둔다.
* 커밋 메시지, 코드 식별자, 로그 문자열도 영어를 사용한다.
* 적용 대상은 C++ 소스·헤더와 빌드·측정 스크립트(`scripts/`, `CMakeLists.txt`,
  워크플로 파일)를 포함한 저장소의 모든 코드다. `third_party/` 아래 외부 코드는
  원본 그대로 둔다.
* 기존 파일을 수정할 때 한국어 주석을 발견하면 같은 작업에서 영어로 바꾼다.
  이중 언어 주석이면 한국어 쪽을 지우고, 한국어만 있으면 영어로 옮긴다.

## Comment Language

* **Write source-code comments in English only.** Do not leave Korean comments.
* Do not keep bilingual comments either: maintaining the same explanation twice
  lets one copy be updated while the other drifts out of date.
* This rule **differs from the documentation rule**. Markdown under `docs/` leads
  with Korean and adds an English translation (`AGENTS.md`), while source
  comments carry one English copy only.
* Commit messages, code identifiers, and log strings are English as well.
* The scope is all code in the repository, including C++ sources and headers and
  the build and measurement scripts (`scripts/`, `CMakeLists.txt`, workflow
  files). External code under `third_party/` stays as upstream wrote it.
* When editing an existing file that still has Korean comments, convert them in
  the same task: drop the Korean half of a bilingual comment, and translate a
  Korean-only comment into English.

## 적용 범위

* `include/` 아래의 C++ 헤더
* `src/` 아래의 C++ 소스
* 이후 추가되는 모든 C++ 파일
* 주석 언어 규칙은 `scripts/`와 빌드 파일을 포함한 저장소의 모든 코드에 적용된다

## Scope

* C++ headers under `include/`
* C++ source files under `src/`
* All future C++ files
* The comment-language rule applies to all repository code, including `scripts/`
  and build files

## 예시

잘못된 예:

```cpp
int main() {
    if (ready) {
        Run();
    }
}
```

올바른 예:

```cpp
int main()
{
    if (ready)
    {
        Run();
    }
}
```

## Example

Incorrect:

```cpp
int main() {
    if (ready) {
        Run();
    }
}
```

Correct:

```cpp
int main()
{
    if (ready)
    {
        Run();
    }
}
```

## 디렉터리 정책

* 플랫폼 공용 로더와 런타임 코어는 `src/` 아래의 공용 영역에 둔다.
* Win32 전용 코드는 `src/platform/win32/` 아래에 둔다.
* Linux 전용 코드는 `src/platform/linux/` 아래에 둔다. i386과 x86-64가 함께 쓰는 코드만 이 디렉터리에 두고, 아키텍처에 따라 갈리는 코드는 `src/platform/linux/x86/`, `src/platform/linux/x64/`에 둔다. 아키텍처 파일은 첫머리에서 `#error`로 자기 아키텍처를 확인한다. 공개 헤더도 같은 규칙으로 `include/repiu/platform/linux/`, `include/repiu/platform/linux/x86/`, `include/repiu/platform/linux/x64/`에 둔다.
* Web 전용 코드는 `src/platform/web/` 아래에 둔다.
* 엔진·런타임·HLE·도구 디렉터리에는 플랫폼·아키텍처 하위 디렉터리를 두지 않는다. OS에 의존하는 부분은 플랫폼 계층의 함수로 떼어 내고(`include/repiu/platform/`의 계약, `src/platform/<OS>/`의 구현), 플랫폼 계층은 엔진의 타입을 쓰지 않는다.
* 엔진에서 실행 모델(`direct`, `cache`, `emulated`)에 따라 다른 구현은 공용 파일 옆에 `<이름>_direct.cpp`, `<이름>_cache.cpp`, `<이름>_emulated.cpp`로 둔다. `direct`·`cache` 파일은 첫머리에서 `#error`로 자기 아키텍처를 확인하고, `emulated` 파일은 호스트 CPU를 가리지 않으므로 확인하지 않는다. 공용 코드는 `include/repiu/runtime/execution_model.h`의 함수로 묻는다.
* 헤더에서 플랫폼을 고르는 `#if`는 플랫폼 헤더(`include/repiu/platform/`)의 선택 지점에만 두며, 그 안에는 `#include`만 둔다.
* 예외: Win32의 예외 진입과 종료 복구의 vectored handler(`execution_trampoline.cpp`의 `#if defined(_WIN32)` 블록, `exception_rescue_win32.cpp`)는 엔진의 상태를 쥐고 있어 엔진에 남아 있다. 플랫폼 계층의 콜백으로 바꾸는 별도 작업 전까지 같은 모양의 Win32 코드를 엔진에 더하지 않는다.

## Directory Policy

* Put platform-neutral loader and runtime core code in shared areas under `src/`.
* Put Win32-specific code under `src/platform/win32/`.
* Put Linux-specific code under `src/platform/linux/`. Only code shared by i386 and x86-64 stays in that directory; code that differs by architecture goes in `src/platform/linux/x86/` or `src/platform/linux/x64/`. Architecture files check their own architecture with `#error` at the top. Public headers follow the same rule under `include/repiu/platform/linux/`, `include/repiu/platform/linux/x86/` and `include/repiu/platform/linux/x64/`.
* Put Web-specific code under `src/platform/web/`.
* The engine, runtime, HLE and tool directories have no platform or architecture subdirectories. What depends on the OS is taken out into functions of the platform layer (contracts in `include/repiu/platform/`, implementations in `src/platform/<OS>/`), and the platform layer uses none of the engine's types.
* In the engine, implementations that differ by execution model (`direct`, `cache`, `emulated`) sit next to their shared file as `<name>_direct.cpp`, `<name>_cache.cpp` and `<name>_emulated.cpp`. The `direct` and `cache` files check their own architecture with `#error` at the top; the `emulated` files run on any host CPU and do not. Shared code asks through the functions of `include/repiu/runtime/execution_model.h`.
* A `#if` that chooses a platform in a header exists only at the selection points of platform headers (`include/repiu/platform/`), with nothing but `#include` inside.
* The exception: Win32's exception entry and the shutdown recovery's vectored handlers (the `#if defined(_WIN32)` blocks of `execution_trampoline.cpp`, and `exception_rescue_win32.cpp`) hold engine state and remain in the engine. Until the separate task that turns them into callbacks of the platform layer, add no more Win32 code of that shape to the engine.

## 라이선스 정책

* 프로젝트 기본 라이선스는 `BSD 3-Clause License`를 기준으로 한다.
* GPL, LGPL, AGPL 등 전염성 라이선스의 서드파티 코드는 도입하지 않는다.
* 서드파티 의존성을 추가하기 전에 라이선스를 확인하고 문서화한다.

## License Policy

* Use the `BSD 3-Clause License` as the project license baseline.
* Do not introduce third-party code under copyleft licenses such as GPL, LGPL, or AGPL.
* Check and document third-party dependency licenses before adding them.
