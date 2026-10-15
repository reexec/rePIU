# 데스크톱 OpenGL core와 GLES 3 / WebGL2의 공통 부분집합

관련: [설계 #69](../design/20261016-i069-gles3-renderer.md), [작업 로그 #69](../work-logs/20261016-i069-gles3-renderer.md)

## 세 가지 OpenGL

| | 고정 기능 | 셰이더 언어 | 쓰는 곳 |
|---|---|---|---|
| 데스크톱 OpenGL 2.1 / 3.x **compatibility** 프로필 | 있음(`glBegin`, 행렬 스택, `glAlphaFunc`, `glPushAttrib` …) | GLSL 1.10 ~ (`gl_Color`, `ftransform()` 등 내장 변수) | rePIU v0.0.221까지의 Glide 렌더러 |
| 데스크톱 OpenGL 3.2+ **core** 프로필 | 없음. 부르면 `GL_INVALID_OPERATION` | GLSL 1.50 ~ 4.60 core | issue #69부터 네이티브 |
| **OpenGL ES 3.0** = **WebGL 2.0** | 없음 | GLSL ES 3.00 | 웹(단계 4) |

core 프로필은 3.2에서 생겼고, 3.0~3.1에서 "deprecated"로 표시된 기능이 core에서 빠졌습니다
([OpenGL 3.3 Core Profile Specification, Appendix E](https://registry.khronos.org/OpenGL/specs/gl/glspec33.core.pdf)).
WebGL 2.0은 OpenGL ES 3.0을 거의 그대로 JavaScript에 노출합니다([WebGL 2.0 Specification](https://registry.khronos.org/webgl/specs/latest/2.0/)).

## 둘 다 되는 부분 (rePIU가 쓰는 것)

* 버퍼(`glGenBuffers`, `glBufferData`, `glBufferSubData`)와 VAO(`glGenVertexArrays`). core는 VAO가 묶여 있지 않으면 그리기를
  거절합니다.
* FBO(`glGenFramebuffers`, `glFramebufferTexture2D`, `glFramebufferRenderbuffer`), `GL_READ_FRAMEBUFFER`/
  `GL_DRAW_FRAMEBUFFER`, `GL_RGBA8` 색 텍스처, `GL_DEPTH_COMPONENT24` 렌더버퍼.
* `GL_TRIANGLES`, `GL_TRIANGLE_FAN`, `GL_TRIANGLE_STRIP`, `GL_LINES`, `GL_POINTS`. 선 굵기는 1만 보장됩니다.
* `glReadPixels`의 `GL_RGBA`/`GL_UNSIGNED_BYTE`(ES가 보장하는 유일한 조합).
* `GL_REPEAT`/`GL_CLAMP_TO_EDGE`는 2의 거듭제곱이 아닌 텍스처에도 됩니다(ES 2.0은 아니었음).
* GLSL: `in`/`out`, `texture()`, 사용자 정의 프래그먼트 출력, `gl_PointSize`, `discard`, 정수 uniform과 동적 분기, uniform
  배열의 동적 색인. 머리말만 다릅니다: `#version 330 core` 대 `#version 300 es`(+ 기본 `precision`).

## 한쪽에만 있는 것 (rePIU가 피하는 것)

| 기능 | 데스크톱 core | ES 3.0 / WebGL2 | 대신 |
|---|---|---|---|
| `glClearDepth(double)` | 있음 | 없음(`glClearDepthf`) | `glClearDepthf`가 있으면 그것(데스크톱 4.1+), 없으면 `glClearDepth` |
| `glPointSize` | 있음 | 없음 | 정점 셰이더의 `gl_PointSize`. 데스크톱은 `GL_PROGRAM_POINT_SIZE`를 켜야 셰이더 값을 씀 |
| 기본 프레임버퍼의 front 그리기 `glDrawBuffer(GL_FRONT)` | 있음 | 없음 | 오프스크린 front/back FBO |
| `GL_CLAMP`, `glPolygonMode`, 넓은 선 | 없음/일부 | 없음 | 쓰지 않음 |
| `glMapBufferRange`의 비동기화 쓰기 | 있음 | ES에는 있으나 WebGL2에는 없음 | `glBufferSubData` + orphan(`glBufferData(nullptr)`) |

## WebGL의 기본 프레임버퍼

WebGL 문맥의 그리기 버퍼는 **합성(composite)된 뒤 지워집니다**. 문맥 속성 `preserveDrawingBuffer`가 기본값 `false`이면,
페이지에 표시된 다음 내용은 정의되지 않습니다([WebGL 1.0 Specification §2.2 The Drawing Buffer](https://registry.khronos.org/webgl/specs/latest/1.0/#2.2)).
또 Worker의 OffscreenCanvas는 Worker가 이벤트 고리로 돌아갈 때 그때까지 그린 것을 내보냅니다. Glide처럼 "swap 전까지
back에 그리는" 모델은 게임이 기본 프레임버퍼에 직접 그리면 두 규칙 모두에 걸리므로, rePIU는 게임을 FBO에 그리고 swap 때만
기본 프레임버퍼에 완성된 그림을 옮깁니다.

## GL 조회와 스레드 드라이버

`glGet*`·`glIsEnabled`는 드라이버가 명령을 다른 스레드에서 처리하는 경우(NVIDIA의 threaded optimization 등) 그 스레드가
밀린 명령을 다 처리할 때까지 호출한 스레드를 세웁니다. WebGL에서는 조회가 JavaScript와 GPU 프로세스 사이의 동기 왕복이
될 수 있습니다. 매 프레임 지나는 경로에서 상태를 되돌릴 때는 앱이 이미 아는 값을 쓰는 편이 낫습니다(rePIU #69에서 프레임마다
조회 15번이 vsync OFF 처리량을 절반으로 만든 측정이 있습니다).

---

# The common subset of desktop OpenGL core and GLES 3 / WebGL2

Related: [design #69](../design/20261016-i069-gles3-renderer.md), [work log #69](../work-logs/20261016-i069-gles3-renderer.md)

## Three OpenGLs

| | Fixed function | Shading language | Used by |
|---|---|---|---|
| Desktop OpenGL 2.1 / 3.x **compatibility** profile | yes (`glBegin`, the matrix stack, `glAlphaFunc`, `glPushAttrib` …) | GLSL 1.10 on (built-ins such as `gl_Color`, `ftransform()`) | rePIU's Glide renderer up to v0.0.221 |
| Desktop OpenGL 3.2+ **core** profile | no; calling it raises `GL_INVALID_OPERATION` | GLSL 1.50 to 4.60 core | native since issue #69 |
| **OpenGL ES 3.0** = **WebGL 2.0** | no | GLSL ES 3.00 | the web (phase 4) |

The core profile appeared in 3.2, dropping what 3.0 and 3.1 had marked deprecated
([OpenGL 3.3 Core Profile Specification, Appendix E](https://registry.khronos.org/OpenGL/specs/gl/glspec33.core.pdf)).
WebGL 2.0 exposes OpenGL ES 3.0 to JavaScript almost as it is ([WebGL 2.0 Specification](https://registry.khronos.org/webgl/specs/latest/2.0/)).

## What both have (what rePIU uses)

* Buffers (`glGenBuffers`, `glBufferData`, `glBufferSubData`) and VAOs (`glGenVertexArrays`); core refuses to draw with
  no VAO bound.
* FBOs (`glGenFramebuffers`, `glFramebufferTexture2D`, `glFramebufferRenderbuffer`), `GL_READ_FRAMEBUFFER`/
  `GL_DRAW_FRAMEBUFFER`, `GL_RGBA8` color textures, `GL_DEPTH_COMPONENT24` renderbuffers.
* `GL_TRIANGLES`, `GL_TRIANGLE_FAN`, `GL_TRIANGLE_STRIP`, `GL_LINES`, `GL_POINTS`; only a line width of 1 is guaranteed.
* `glReadPixels` with `GL_RGBA`/`GL_UNSIGNED_BYTE` (the one combination ES guarantees).
* `GL_REPEAT`/`GL_CLAMP_TO_EDGE` on non-power-of-two textures (ES 2.0 lacked this).
* GLSL: `in`/`out`, `texture()`, user-defined fragment outputs, `gl_PointSize`, `discard`, integer uniforms with dynamic
  branching, dynamic indexing of uniform arrays. Only the header differs: `#version 330 core` against `#version 300 es`
  (plus default `precision`).

## What only one side has (what rePIU avoids)

| Feature | Desktop core | ES 3.0 / WebGL2 | Instead |
|---|---|---|---|
| `glClearDepth(double)` | yes | no (`glClearDepthf`) | `glClearDepthf` when present (desktop 4.1+), `glClearDepth` otherwise |
| `glPointSize` | yes | no | `gl_PointSize` in the vertex shader; desktop must enable `GL_PROGRAM_POINT_SIZE` for the shader's value to count |
| Drawing to the default framebuffer's front, `glDrawBuffer(GL_FRONT)` | yes | no | offscreen front/back FBOs |
| `GL_CLAMP`, `glPolygonMode`, wide lines | no / partly | no | not used |
| Unsynchronized writes through `glMapBufferRange` | yes | in ES, not in WebGL2 | `glBufferSubData` plus orphaning (`glBufferData(nullptr)`) |

## WebGL's default framebuffer

A WebGL context's drawing buffer is **cleared after it is composited**: with the context attribute
`preserveDrawingBuffer` at its default `false`, its contents are undefined once presented to the page
([WebGL 1.0 Specification §2.2 The Drawing Buffer](https://registry.khronos.org/webgl/specs/latest/1.0/#2.2)). And a
Worker's OffscreenCanvas presents what has been drawn when the Worker returns to its event loop. A "draw into back until
the swap" model like Glide's runs into both rules if the game draws into the default framebuffer, so rePIU draws the game
into FBOs and moves only finished pictures into the default framebuffer at the swap.

## GL queries and threaded drivers

`glGet*` and `glIsEnabled` stall the calling thread until a driver that processes commands on another thread (NVIDIA's
threaded optimization and the like) has worked through its backlog. In WebGL a query can be a synchronous round trip
between JavaScript and the GPU process. On a path taken every frame, putting state back from values the application
already knows is the better choice (in rePIU #69, 15 queries per frame halved the vsync-OFF throughput in measurement).
