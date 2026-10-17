# 작업 지시: OPFS에 푼 게임 파일 남기기 (issue #86)

설계: [20261017-i086](../design/20261017-i086-opfs-mount-cache.md)

1. `src/platform/web/page/mount_cache.js`: 상태, 지우기, WORKERFS용 파일 목록, 저장(동기 접근 핸들, `cache.json` 마지막).
2. worker.js: 시작 전 열쇠가 같으면 마운트, 없으면 `callMain`이 돌아온 뒤 저장. 진행을 페이지에.
3. main.js·index.html: 캐시 상태 줄, "Clear saved files", `navigator.storage.persist()`. CMake 페이지 목록.
4. 검증: headless Chrome 같은 프로필 두 번, 웹 빌드.
5. 문서: 작업 로그, 웹 README, frontier.

---

# Work order: keep the unpacked game files in OPFS (issue #86)

Design: [20261017-i086](../design/20261017-i086-opfs-mount-cache.md)

1. `src/platform/web/page/mount_cache.js`: state, clear, the file list for WORKERFS, store (synchronous access handles,
   `cache.json` last).
2. worker.js: mount before start when the key matches; otherwise store after `callMain` returns, with progress to the page.
3. main.js and index.html: the cache line, "Clear saved files", `navigator.storage.persist()`; the CMake page list.
4. Verification: headless Chrome twice on one profile, the web build.
5. Documents: the work log, the web README, the frontier.
