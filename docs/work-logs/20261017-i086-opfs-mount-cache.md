# 작업 로그: OPFS에 푼 게임 파일 남기기 (issue #86)

설계: [20261017-i086](../design/20261017-i086-opfs-mount-cache.md) ·
작업 지시: [20261017-i086](../work-orders/20261017-i086-opfs-mount-cache.md)

## 2026-10-17 — 구현과 확인

브랜치 `task/i086-opfs-mount-cache`(main `978e267`, v0.0.228에서). 엔진은 바꾸지 않았습니다.

### 구현

| 파일 | 내용 |
|---|---|
| `page/mount_cache.js` (새) | OPFS `rePIU/mounts/<게임>/`. 열쇠는 CHD 이름·크기·수정 시각, `cache.json`이 마지막에 쓰여 완료를 뜻함. 파일 수가 맞지 않으면 쓰지 않음. 저장 전 할당량 확인 |
| `page/worker.js` | 시작 전 같은 열쇠의 완전한 캐시를 WORKERFS로 `/build/runtime_mounts/<게임>`에 마운트. 없으면 `callMain`이 돌아온 뒤(첫 양보) 파일마다 차례를 넘기며 저장 |
| `page/main.js`, `index.html` | 캐시 상태 줄("not saved yet" / "saved, … MB" / 다른 CHD), "Clear saved files" 버튼, Start에서 `navigator.storage.persist()` |
| CMake | 페이지 목록에 `mount_cache.js` |

### 확인 (headless Chrome, 같은 프로필로 두 번, pumpitea, 150초씩)

| 실행 | 로그 | 캐시 줄 | 첫 화면 |
|---|---|---|---|
| 1 | `mount cache reused: false`, `extracted files/bytes: 736/428909991` | 76초에 저장 35%, 127초에 "Saved 737 unpacked files (409 MB)" | 약 92초 |
| 2 | `mount cache reused: true`, `extracted files/bytes: 0/0` | "Using the unpacked files saved earlier (737 files)" | **약 36초** |

* 저장은 게임이 도는 동안 약 50초(파일마다 이벤트 고리에 차례를 넘김). 737개는 736개 + `.chd-identity`.
* 빈 디렉터리(`EMPTY`)는 WORKERFS가 파일 없는 디렉터리를 만들지 않아 두 번째 실행에 없습니다. 게임은 화면까지 정상으로 나옴.
* 웹 빌드(`repiu_browser`)가 `mount_cache.js`를 빌드 디렉터리로 복사함.

### 남은 것

* 사용자 브라우저에서 두 번째 실행 확인.
* 게임이 쓰는 `nvram`·`cfg`는 여전히 메모리(실행마다 처음부터). 영구 저장은 별도.
* GitHub Pages 배포(단위 3의 나머지).

---

# Work log: keep the unpacked game files in OPFS (issue #86)

Design: [20261017-i086](../design/20261017-i086-opfs-mount-cache.md) ·
Work order: [20261017-i086](../work-orders/20261017-i086-opfs-mount-cache.md)

## 2026-10-17 — implemented and checked

Branch `task/i086-opfs-mount-cache` (main `978e267`, from v0.0.228). The engine is unchanged.

### Implementation

| File | Content |
|---|---|
| `page/mount_cache.js` (new) | OPFS `rePIU/mounts/<game>/`; the key is the CHD's name, size and modification time; `cache.json`, written last, means complete; a cache with the wrong file count is not used; the quota is checked before storing |
| `page/worker.js` | before start, a complete cache with the same key is mounted with WORKERFS at `/build/runtime_mounts/<game>`; otherwise, after `callMain` returns (the first yield), the files are stored, one turn of the event loop per file |
| `page/main.js`, `index.html` | the cache line ("not saved yet" / "saved, … MB" / another CHD), a "Clear saved files" button, `navigator.storage.persist()` on Start |
| CMake | `mount_cache.js` in the page list |

### Checked (headless Chrome, twice on one profile, pumpitea, 150 s each)

| Run | Log | Cache line | First picture |
|---|---|---|---|
| 1 | `mount cache reused: false`, `extracted files/bytes: 736/428909991` | 35% saved at 76 s, "Saved 737 unpacked files (409 MB)" at 127 s | about 92 s |
| 2 | `mount cache reused: true`, `extracted files/bytes: 0/0` | "Using the unpacked files saved earlier (737 files)" | **about 36 s** |

* Storing takes about 50 s while the game runs (a turn of the event loop per file). 737 is 736 plus `.chd-identity`.
* The empty directory (`EMPTY`) is absent on the second run, since WORKERFS makes no directory without a file; the game
  still reaches its picture.
* The web build (`repiu_browser`) copies `mount_cache.js` into the build directory.

### Remaining

* The second run checked in the user's browser.
* The `nvram` and `cfg` the game writes are still in memory (fresh every run); keeping them is separate.
* GitHub Pages deployment (the rest of unit 3).
