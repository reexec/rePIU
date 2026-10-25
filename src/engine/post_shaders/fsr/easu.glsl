// rePIU built-in post-processing shader fsr, pass 0 (issue #90): AMD
// FidelityFX Super Resolution 1.0's EASU pass.
//
// Edge Adaptive Spatial Upsampling, the upscaling pass of FSR 1, ported to a
// single libretro-style GLSL 1.20 pass. It looks at the 12 source pixels
// around each output pixel, estimates the local edge direction and length
// from their luma, and weighs them with a Lanczos-like kernel stretched along
// the edge, then clamps the result to the nearest four pixels' range so it
// does not ring. It draws at the window's size; rcas.glsl sharpens that.
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

vec3 Fetch(vec2 texel)
{
    return texture2D(Texture, (texel + 0.5) / TextureSize).rgb;
}

// Luma times two, as FSR approximates it.
float Luma(vec3 c)
{
    return c.b * 0.5 + (c.r * 0.5 + c.g);
}

// One of the four bilinear quadrants' contribution to the edge direction and
// length: a above, b left, c centre, d right, e below.
void EdgeSet(inout vec2 dir, inout float len, float w, float a, float b, float c, float d, float e)
{
    float dc = d - c;
    float cb = c - b;
    float len_x = max(abs(dc), abs(cb));
    len_x = len_x > 0.0 ? 1.0 / len_x : 0.0;
    float dir_x = d - b;
    dir.x += dir_x * w;
    len_x = clamp(abs(dir_x) * len_x, 0.0, 1.0);
    len_x *= len_x;
    len += len_x * w;

    float ec = e - c;
    float ca = c - a;
    float len_y = max(abs(ec), abs(ca));
    len_y = len_y > 0.0 ? 1.0 / len_y : 0.0;
    float dir_y = e - a;
    dir.y += dir_y * w;
    len_y = clamp(abs(dir_y) * len_y, 0.0, 1.0);
    len_y *= len_y;
    len += len_y * w;
}

void Tap(inout vec3 sum, inout float weight, vec2 offset, vec2 dir, vec2 len2, float lob, float clp, vec3 c)
{
    // Rotate the offset onto the edge, then stretch it.
    vec2 v = vec2(offset.x * dir.x + offset.y * dir.y, offset.x * -dir.y + offset.y * dir.x) * len2;
    float d2 = min(dot(v, v), clp);
    // Lanczos2 approximated as (25/16 * (2/5 * x^2 - 1)^2 - (25/16 - 1)) * (lob * x^2 - 1)^2.
    float wb = 2.0 / 5.0 * d2 - 1.0;
    float wa = lob * d2 - 1.0;
    wb *= wb;
    wa *= wa;
    wb = 25.0 / 16.0 * wb - (25.0 / 16.0 - 1.0);
    float w = wb * wa;
    sum += c * w;
    weight += w;
}

void main()
{
    // The source position of this output pixel's centre, and the source
    // pixel f at the top left of its 2x2 neighbourhood.
    vec2 pp = tex_coord * TextureSize - 0.5;
    vec2 fp = floor(pp);
    pp -= fp;

    //    b c
    //  e f g h
    //  i j k l
    //    n o
    vec3 b = Fetch(fp + vec2(0.0, -1.0));
    vec3 c = Fetch(fp + vec2(1.0, -1.0));
    vec3 e = Fetch(fp + vec2(-1.0, 0.0));
    vec3 f = Fetch(fp);
    vec3 g = Fetch(fp + vec2(1.0, 0.0));
    vec3 h = Fetch(fp + vec2(2.0, 0.0));
    vec3 i = Fetch(fp + vec2(-1.0, 1.0));
    vec3 j = Fetch(fp + vec2(0.0, 1.0));
    vec3 k = Fetch(fp + vec2(1.0, 1.0));
    vec3 l = Fetch(fp + vec2(2.0, 1.0));
    vec3 n = Fetch(fp + vec2(0.0, 2.0));
    vec3 o = Fetch(fp + vec2(1.0, 2.0));

    float bl = Luma(b);
    float cl = Luma(c);
    float el = Luma(e);
    float fl = Luma(f);
    float gl = Luma(g);
    float hl = Luma(h);
    float il = Luma(i);
    float jl = Luma(j);
    float kl = Luma(k);
    float ll = Luma(l);
    float nl = Luma(n);
    float ol = Luma(o);

    vec2 dir = vec2(0.0);
    float len = 0.0;
    EdgeSet(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bl, el, fl, gl, jl);
    EdgeSet(dir, len, pp.x * (1.0 - pp.y), cl, fl, gl, hl, kl);
    EdgeSet(dir, len, (1.0 - pp.x) * pp.y, fl, il, jl, kl, nl);
    EdgeSet(dir, len, pp.x * pp.y, gl, jl, kl, ll, ol);

    // Normalize the direction; a flat area takes the x axis.
    float dir_r = dot(dir, dir);
    bool flat_area = dir_r < 1.0 / 32768.0;
    dir = flat_area ? vec2(1.0, 0.0) : dir * inversesqrt(dir_r);

    // How strong the edge is shapes the kernel: stretched along a strong
    // edge, closer to round where there is none.
    len = len * 0.5;
    len *= len;
    float stretch = dot(dir, dir) / max(abs(dir.x), abs(dir.y));
    vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = 1.0 / lob;

    vec3 sum = vec3(0.0);
    float weight = 0.0;
    Tap(sum, weight, vec2(0.0, -1.0) - pp, dir, len2, lob, clp, b);
    Tap(sum, weight, vec2(1.0, -1.0) - pp, dir, len2, lob, clp, c);
    Tap(sum, weight, vec2(-1.0, 1.0) - pp, dir, len2, lob, clp, i);
    Tap(sum, weight, vec2(0.0, 1.0) - pp, dir, len2, lob, clp, j);
    Tap(sum, weight, vec2(0.0, 0.0) - pp, dir, len2, lob, clp, f);
    Tap(sum, weight, vec2(-1.0, 0.0) - pp, dir, len2, lob, clp, e);
    Tap(sum, weight, vec2(1.0, 1.0) - pp, dir, len2, lob, clp, k);
    Tap(sum, weight, vec2(2.0, 1.0) - pp, dir, len2, lob, clp, l);
    Tap(sum, weight, vec2(2.0, 0.0) - pp, dir, len2, lob, clp, h);
    Tap(sum, weight, vec2(1.0, 0.0) - pp, dir, len2, lob, clp, g);
    Tap(sum, weight, vec2(1.0, 2.0) - pp, dir, len2, lob, clp, o);
    Tap(sum, weight, vec2(0.0, 2.0) - pp, dir, len2, lob, clp, n);

    // Deringing: stay within the nearest four pixels' range.
    vec3 lo = min(min(f, g), min(j, k));
    vec3 hi = max(max(f, g), max(j, k));
    vec3 color = weight > 0.0 ? sum / weight : f;
    gl_FragColor = vec4(min(hi, max(lo, color)), 1.0);
}

#endif
