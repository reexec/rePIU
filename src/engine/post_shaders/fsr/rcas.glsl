// rePIU built-in post-processing shader fsr, pass 1 (issue #90): AMD FidelityFX
// Super Resolution 1.0's RCAS pass.
//
// Robust Contrast Adaptive Sharpening, FSR 1's second pass, ported to a
// libretro-style GLSL 1.20 pass. It runs at the window's size on what EASU
// drew, sharpening each pixel against its four neighbours by as much as the
// neighbourhood allows without clipping, so edges sharpen without halos.
// FSR_SHARPNESS is in stops: 0 is the strongest, each step up halves it.
//
// Ported from the algorithm in ffx_fsr1.h, which carries this notice:
//
// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#pragma parameter FSR_SHARPNESS "FSR sharpness (stops, 0 = strongest)" 0.2 0.0 2.0 0.1

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

#ifdef GL_ES
precision highp float;
#endif

uniform sampler2D Texture;
uniform vec2 TextureSize;
varying vec2 tex_coord;

#ifdef PARAMETER_UNIFORM
uniform float FSR_SHARPNESS;
#else
#define FSR_SHARPNESS 0.2
#endif

// The most negative lobe RCAS allows, as FSR_RCAS_LIMIT: 0.25 - 1/16.
#define RCAS_LIMIT 0.1875

void main()
{
    vec2 texel = 1.0 / TextureSize;
    //    b
    //  d e f
    //    h
    vec3 b = texture2D(Texture, tex_coord + vec2(0.0, -texel.y)).rgb;
    vec3 d = texture2D(Texture, tex_coord + vec2(-texel.x, 0.0)).rgb;
    vec3 e = texture2D(Texture, tex_coord).rgb;
    vec3 f = texture2D(Texture, tex_coord + vec2(texel.x, 0.0)).rgb;
    vec3 h = texture2D(Texture, tex_coord + vec2(0.0, texel.y)).rgb;

    vec3 mn4 = min(min(b, d), min(f, h));
    vec3 mx4 = max(max(b, d), max(f, h));
    // How far the ring may go before it clips at either end, per channel.
    vec3 hit_min = min(mn4, e) / max(4.0 * mx4, vec3(1.0 / 65536.0));
    vec3 hit_max = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, vec3(-1.0 / 65536.0));
    vec3 lobe_rgb = max(-hit_min, hit_max);
    float lobe = max(-RCAS_LIMIT, min(max(lobe_rgb.r, max(lobe_rgb.g, lobe_rgb.b)), 0.0)) * exp2(-FSR_SHARPNESS);
    vec3 color = (lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0);
    gl_FragColor = vec4(clamp(color, 0.0, 1.0), 1.0);
}

#endif
