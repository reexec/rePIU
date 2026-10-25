# 화면 후처리 shader / Post-processing shaders

설계: [20261004-768](../design/20261004-768-post-process-shaders.md) ·
작업 로그: [20261004-768](../work-logs/20261004-768-post-process-shaders.md)

## 한국어

### 1. 고르는 방법

| 방법 | 지속 | 예 |
|---|---|---|
| 런처 Options의 "Screen shader" | `cfg/repiu.ini`의 `[Video] post_shader`에 저장 | `post_shader = crt` |
| 실행 인자 (Task 771) | 그 실행 (환경 변수와 런처 값보다 우선; 롬셋 없이 주면 런처 세션 전체) | `repiu pumpit8 --post-shader crt`, `--post-shader=crt` |
| 환경 변수 | 그 실행 (런처 값보다 우선) | `REPIU_POST_SHADER=scanline` |
| 게임 중 `Tab` OSD | 그 실행만 | 콤보, Reload, 매개변수 슬라이더 |

id는 `none`, 내장 `crt`·`scanline`·`fsr`·`scalefx`·`anime4k`, 또는 `shaders/` 안 파일 이름(`my_crt.glsl`,
다단계 `my_chain.glslp`)입니다. `shaders/`는 작업 디렉터리를 먼저 보고, 없으면 실행 파일 옆을 봅니다(`cfg/`와 같은
규칙).

#### 내장 업스케일러 (issue #90)

세 내장 shader가 게임 그림(640×480)을 창 크기로 키우는 방식을 바꿉니다. 모두 프레임 하나만 보며, 창 배율이
클수록(전체 화면) `none`(선형)과의 차이가 분명합니다. re2DJ #26·#27과 같은 파일입니다. 네이티브와 브라우저 모두에서
돕니다. 브라우저에서 `scalefx`·`anime4k`의 부동소수점 pass 대상은 WebGL2의 `EXT_color_buffer_float`이 필요하며,
없으면 그 프레임은 평범하게 그려지고 shader가 내려가며 이유가 OSD에 나옵니다.

| id | 방식 | 어울리는 그림 | 매개변수 |
|---|---|---|---|
| `fsr` | AMD FidelityFX Super Resolution 1.0: EASU가 경계 방향을 따라 창 크기로 키우고 RCAS가 선명하게 함 | 사진·CG 전반. 글자 둘레가 밝아지면 선명도를 낮춤 | `FSR_SHARPNESS`(스톱, 0이 가장 강함) |
| `scalefx` | ScaleFX(Sp00kyFox): 원본 색만 써서 계단 경계를 매끈한 선으로 바꿔 3배로 그린 뒤 창 크기로 | 픽셀아트. 안티앨리어싱된 CG·글자는 윤곽이 딱딱해질 수 있음 | 임계값·AA·모서리 |
| `anime4k` | Anime4K v3.2 Upscale CNN x2 (S)(bloc97): 합성곱 네 층이 2배에서 윤곽 세부를 더한 뒤 창 크기로 | 그림·CG. 잡티 없이 적당히 선명 | 없음 |

### 2. 직접 shader 쓰기

libretro 단일 pass GLSL 형식의 부분집합입니다. 한 파일에 두 단계를 `VERTEX`/`FRAGMENT`로
나눕니다. 내장 `src/engine/post_shaders/scanline.glsl`이 가장 짧은 예입니다.

```glsl
#pragma parameter STRENGTH "Strength" 0.5 0.0 1.0 0.05

#if defined(VERTEX)
attribute vec4 VertexCoord;
attribute vec4 TexCoord;
uniform mat4 MVPMatrix;
varying vec2 tex_coord;
void main()
{
    gl_Position = MVPMatrix * VertexCoord;
    tex_coord = TexCoord.xy;
}
#elif defined(FRAGMENT)
uniform sampler2D Texture;
uniform vec2 TextureSize;   // 게임 논리 해상도 (예: 640x480)
uniform vec2 OutputSize;    // 창의 픽셀 크기
varying vec2 tex_coord;
#ifdef PARAMETER_UNIFORM
uniform float STRENGTH;
#else
#define STRENGTH 0.5
#endif
void main()
{
    gl_FragColor = texture2D(Texture, tex_coord) * (1.0 - STRENGTH * 0.5);
}
#endif
```

쓸 수 있는 이름: attribute `VertexCoord`, `TexCoord`, `COLOR`; uniform `MVPMatrix`, `Texture`,
`InputSize`, `TextureSize`, `OutputSize`, `FrameCount`, `FrameDirection`; `#pragma parameter`.
`PrevTexture`(이전 프레임)와 LUT는 지원하지 않습니다. 다단계 preset은 아래 절을 보십시오.

#### 다단계 preset(`.glslp`) (issue #90)

`shaders/` 안의 `.glslp` 파일은 여러 pass를 잇습니다. libretro preset 형식의 부분집합입니다.

```ini
shaders = 2
shader0 = first.glsl         # preset 파일 기준 상대 경로
filter_linear0 = false       # 이 pass가 Texture를 읽는 필터(기본 linear)
scale_type0 = source         # source(입력의 배수)·viewport(화면 그림의 배수)·absolute(픽셀)
scale0 = 2.0                 # scale_type_x0/scale_x0처럼 축마다 따로 둘 수도 있음
float_framebuffer0 = true    # 색이 아닌 값을 쓰는 pass는 부동소수점 대상으로
shader1 = second.glsl        # 배율이 없는 마지막 pass는 창에 바로 그림
```

각 pass는 앞 pass의 출력을 `Texture`로, 게임 그림을 `OrigTexture`로, N개 앞 pass의 출력을 `PassPrevNTexture`로
읽을 수 있고, 이름마다 `...TextureSize`·`...InputSize`가 있습니다(모두 그 텍스처의 픽셀 크기; pass 0의 입력은 게임
논리 해상도). 마지막 pass에 배율이 있으면 그 결과를 선형으로 창에 그립니다. `#pragma parameter`는 pass들이 이름으로
공유합니다. `srgb_framebuffer`, `wrap_mode`, `mipmap_input`, `alias`, LUT 텍스처는 지원하지 않습니다. mpv 훅
형식(Anime4K 원본)이나 slang은 직접 변환해야 합니다. 내장 preset(`src/engine/post_shaders/fsr/`, `scalefx/`,
`anime4k/`)이 예입니다. libretro의 `#version 130` 스타일(`COMPAT_*` 매크로, `out vec4 FragColor`)도 그대로
읽힙니다(`scalefx`가 그렇습니다).

작성 중에는 게임을 켠 채 파일을 고치고 OSD의 **Reload**를 누르면 됩니다. 컴파일 오류는 OSD에
빨간 글씨로, 로그에 `[repiu-post] shader '…' not applied: …`로 나오며 그동안 화면은 `none`으로
그려집니다.

### 3. 확인 절차

1. `REPIU_POST_SHADER=crt`로 롬셋을 실행합니다. 로그에 `[repiu-post] shader: crt (6 parameters)`.
2. 화면이 휘고 주사선과 마스크가 보이는지 봅니다. 창 배율 2x 이상(기본 2x, `Alt+1`~`Alt+4`)에서
   주사선이 뚜렷합니다.
3. `Tab` → Screen shader에서 `scanline`, `none`으로 바꿔 즉시 바뀌는지 확인합니다. `none`이면
   후처리 없는 원래 화면입니다.
4. `shaders/test.glsl`에 위 예제를 넣고 Reload → 목록에 `test.glsl`이 나오고 고를 수 있는지
   확인합니다. 파일에 일부러 오타를 넣고 Reload하면 오류가 보이고 화면은 `none`이어야 합니다.
5. 런처에서 Screen shader를 고르고 실행한 뒤 다시 런처로 돌아와 값이 유지되는지 확인합니다.
6. `repiu pumpitea --post-shader anime4k`: 로그에 `[repiu-post] shader: anime4k (5 passes, 0 parameters)`,
   전체 화면에서 `none`과 비교해 윤곽이 또렷한지 봅니다. `fsr`은 `(2 passes, 1 parameters)`, `scalefx`는
   `(5 passes, 3 parameters)`입니다(issue #90).

## English

### 1. Choosing a shader

| Way | Lasts | Example |
|---|---|---|
| "Screen shader" under the launcher's Options | stored as `[Video] post_shader` in `cfg/repiu.ini` | `post_shader = crt` |
| Command-line option (Task 771) | that run (wins over the environment variable and the launcher's value; with no ROM set, the whole launcher session) | `repiu pumpit8 --post-shader crt`, `--post-shader=crt` |
| Environment variable | that run (wins over the launcher's value) | `REPIU_POST_SHADER=scanline` |
| The in-game `Tab` OSD | that run only | combo, Reload, parameter sliders |

Ids are `none`, the built-in `crt`, `scanline`, `fsr`, `scalefx` and `anime4k`, or a file name in
`shaders/` (`my_crt.glsl`, or a multi-pass `my_chain.glslp`). `shaders/` is looked up in the working
directory first and next to the executable otherwise, the rule `cfg/` follows.

#### The built-in upscalers (issue #90)

Three built-ins change how the game's picture (640×480) is scaled to the window. All look at one
frame, and the larger the window scale (fullscreen) the clearer the difference from `none` (linear).
The files are re2DJ #26 and #27's. They run natively and in the browser; in the browser the
floating-point pass targets of `scalefx` and `anime4k` need WebGL2's `EXT_color_buffer_float`, and
without it that frame is drawn plainly, the shader is dropped and the reason shows in the OSD.
`fsr` is AMD FidelityFX Super Resolution 1.0, EASU scaling along edge directions to the window and
RCAS sharpening it: for photos and CG in general (lower the sharpness if lettering gains a light rim),
with `FSR_SHARPNESS` in stops, 0 the strongest. `scalefx` is ScaleFX by Sp00kyFox, turning
stair-stepped edges into smooth lines using only the picture's own colours at 3x, then scaling to the
window: for pixel art, while anti-aliased CG and lettering may get hard outlines; threshold, AA and
corner parameters. `anime4k` is Anime4K v3.2 Upscale CNN x2 (S) by bloc97, four convolution layers
adding edge detail at 2x before scaling to the window: for drawn art and CG, moderately sharper
without artifacts, no parameters.

### 2. Writing a shader

The format is a subset of libretro single-pass GLSL: one file, two stages guarded by `VERTEX` and
`FRAGMENT`. The built-in `src/engine/post_shaders/scanline.glsl` is the shortest example, and the
Korean section above has a minimal one. Available names: attributes `VertexCoord`, `TexCoord`,
`COLOR`; uniforms `MVPMatrix`, `Texture`, `InputSize`, `TextureSize` (both the game's logical
resolution, such as 640x480), `OutputSize` (the window's pixel size), `FrameCount`,
`FrameDirection`; and `#pragma parameter`. `PrevTexture` (the previous frame) and LUTs are not
supported; multi-pass presets are described below.

#### Multi-pass presets (`.glslp`) (issue #90)

A `.glslp` file in `shaders/` chains passes, in a subset of the libretro preset format (the Korean
section above has an example): `shaders` (the count), and per pass `shaderN` (a path relative to the
preset), `filter_linearN` (how the pass samples `Texture`, linear by default), `scale_typeN`
(`source`, a multiple of the input; `viewport`, of the picture in the window; `absolute`, pixels) with
`scaleN` (or `_x`/`_y` each), and `float_framebufferN` for a pass writing values rather than colours;
a last pass with no scale draws straight into the window. Each pass reads the one before as
`Texture`, the game's picture as `OrigTexture` and the output N passes back as `PassPrevNTexture`,
each with `...TextureSize` and `...InputSize` beside it (the texture's pixel size; pass 0's input is
the game's logical resolution); a scaled last pass is drawn to the window linearly, and
`#pragma parameter`s are shared by name across passes. `srgb_framebuffer`, `wrap_mode`,
`mipmap_input`, `alias` and lookup textures are not supported, and mpv hook files (Anime4K's own) or
slang need converting by hand. The built-in presets under `src/engine/post_shaders/fsr/`, `scalefx/`
and `anime4k/` are the examples. libretro's `#version 130` style (the `COMPAT_*` macros,
`out vec4 FragColor`) is read as it is, as `scalefx` shows.

While writing, keep the game running, edit the file and press **Reload** in the OSD. A compile
error appears in red in the OSD and in the log as `[repiu-post] shader '…' not applied: …`, and the
screen is drawn as `none` meanwhile.

### 3. Check procedure

1. Run a ROM set with `REPIU_POST_SHADER=crt`. The log shows `[repiu-post] shader: crt (6 parameters)`.
2. Check for curvature, scanlines and the mask. Scanlines are clear at a 2x window scale or more
   (2x by default, `Alt+1` to `Alt+4`).
3. `Tab` → Screen shader: switch to `scanline` and `none` and check the change is immediate. `none`
   is the unprocessed picture.
4. Put the example in `shaders/test.glsl` and press Reload: `test.glsl` appears and can be chosen.
   Introduce a typo and Reload: the error shows and the screen is `none`.
5. Pick a Screen shader in the launcher, play, return to the launcher, and check the value stayed.
6. `repiu pumpitea --post-shader anime4k`: the log shows `[repiu-post] shader: anime4k (5 passes, 0
   parameters)`; fullscreen, compare the edges with `none`. `fsr` logs `(2 passes, 1 parameters)` and
   `scalefx` `(5 passes, 3 parameters)` (issue #90).
