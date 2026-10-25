// rePIU built-in post-processing shader anime4k, pass 4 (issue #90): the
// depth-to-space step of Anime4K v3.2 Upscale CNN x2 (S) by bloc97, converted
// from its mpv hook file (glsl/Upscale/Anime4K_Upscale_CNN_x2_S.glsl) to a
// libretro-style pass. At twice the game's size it picks, for each output
// pixel, the one of the last layer's four channels that belongs to its
// quarter of the source pixel, and adds that luma detail to the game's
// picture scaled up linearly (OrigTexture).
//
// MIT License

// Copyright (c) 2019-2021 bloc97
// All rights reserved.

// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

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
uniform sampler2D OrigTexture;
varying vec2 tex_coord;

void main()
{
    // Where this pixel sits inside its source pixel, and that pixel's centre.
    // The channels run left to right, then top to bottom, as mpv's y runs
    // down; rePIU's y runs up, so the top half is f0.y >= 0.5.
    vec2 f0 = fract(tex_coord * TextureSize);
    vec4 channels = texture2D(Texture, (vec2(0.5) - f0) / TextureSize + tex_coord);
    float detail = f0.y >= 0.5 ? (f0.x < 0.5 ? channels.r : channels.g)
                               : (f0.x < 0.5 ? channels.b : channels.a);
    vec3 color = texture2D(OrigTexture, tex_coord).rgb + vec3(detail);
    gl_FragColor = vec4(clamp(color, 0.0, 1.0), 1.0);
}

#endif
