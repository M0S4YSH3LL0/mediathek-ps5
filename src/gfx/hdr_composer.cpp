// Mediathek - An HDR10 frame: the interface drawn over the film's picture in PQ.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The blend of the interface over a PQ picture follows VLC's PS5 port (GPL-2.0-or-later,
// https://github.com/ZiZc3/VLC, shaders/ui_pq.frag).

#include "gfx/hdr_composer.hpp"

#include "gfx/gl_program.hpp"
#include "platform/ps5/system.hpp"

namespace hui::gfx
{

namespace
{

constexpr const char *kVertex = R"(
void main()
{
    vec2 corner = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)";

constexpr const char *kFragment = R"(
out vec4 frag_color;
layout(location = 0) uniform sampler2D u_layer;
layout(location = 1) uniform sampler2D u_picture;
layout(location = 2) uniform vec4 u_rect;    // the picture in target pixels: left, top, w, h
layout(location = 3) uniform float u_height; // the target's height in pixels
layout(location = 4) uniform float u_opacity; // 0: no picture
layout(location = 5) uniform int u_pq;

const float kWhite = 203.0;
const float kM1 = 0.1593017578125;
const float kM2 = 78.84375;
const float kC1 = 0.8359375;
const float kC2 = 18.8515625;
const float kC3 = 18.6875;
const mat3 kBt709ToBt2020 = mat3(0.6274, 0.0691, 0.0164, 0.3293, 0.9195, 0.0880,
                                 0.0433, 0.0114, 0.8956);

vec3 pq_to_nits(vec3 e)
{
    vec3 p = pow(clamp(e, 0.0, 1.0), vec3(1.0 / kM2));
    return 10000.0 * pow(max(p - kC1, vec3(0.0)) / (kC2 - kC3 * p), vec3(1.0 / kM1));
}

vec3 nits_to_pq(vec3 nits)
{
    vec3 y = pow(clamp(nits / 10000.0, 0.0, 1.0), vec3(kM1));
    return pow((kC1 + kC2 * y) / (1.0 + kC3 * y), vec3(kM2));
}

void main()
{
    vec3 film = vec3(0.0); // light in nits, BT.2020
    vec2 at = vec2(gl_FragCoord.x, u_height - gl_FragCoord.y);
    vec2 inside = (at - u_rect.xy) / u_rect.zw;
    if (u_opacity > 0.0 && all(greaterThanEqual(inside, vec2(0.0))) &&
        all(lessThan(inside, vec2(1.0))))
    {
        vec3 code = texture(u_picture, inside).rgb;
        film = u_pq != 0 ? pq_to_nits(code) : kBt709ToBt2020 * (pow(code, vec3(2.4)) * kWhite);
        film *= u_opacity;
    }
    vec3 result = nits_to_pq(film);

    // The layer is premultiplied: drawn with straight alpha into a clear target.
    vec4 layer = texelFetch(u_layer, ivec2(gl_FragCoord.xy), 0);
    if (layer.a > 0.0)
    {
        vec3 straight = clamp(layer.rgb / layer.a, 0.0, 1.0);
        vec3 light = kBt709ToBt2020 * (pow(straight, vec3(2.2)) * kWhite);
        // Blended on PQ codes, half-transparent black halved the code and darkened the film
        // far more than on an SDR screen, where (1 - a)^2.2 of the light is left: the
        // coverage that leaves that much of a 100-nit picture is used instead.
        float cover = layer.a;
        if (cover < 1.0)
            cover = 1.0 - nits_to_pq(vec3(100.0 * pow(1.0 - cover, 2.2))).x /
                              nits_to_pq(vec3(100.0)).x;
        result = nits_to_pq(light) * cover + result * (1.0 - cover);
    }

    // A2R10G10B10, blue in the low bits; the screen keeps its bytes as B, G, R, A.
    uvec3 c = uvec3(clamp(result, 0.0, 1.0) * 1023.0 + 0.5);
    uint word = c.b | (c.g << 10) | (c.r << 20) | (3u << 30);
    frag_color = vec4(float((word >> 16) & 255u), float((word >> 8) & 255u), float(word & 255u),
                      float(word >> 24)) / 255.0;
}
)";

} // namespace

HdrComposer::~HdrComposer()
{
    release();
}

void HdrComposer::release()
{
    if (program_ != 0)
        glDeleteProgram(program_);
    if (vao_ != 0)
        glDeleteVertexArrays(1, &vao_);
    if (framebuffer_ != 0)
        glDeleteFramebuffers(1, &framebuffer_);
    if (texture_ != 0)
        glDeleteTextures(1, &texture_);
    program_ = vao_ = framebuffer_ = texture_ = 0;
    width_ = height_ = 0;
}

bool HdrComposer::begin(int width, int height)
{
    if (failed_ || width <= 0 || height <= 0)
        return false;
    if (program_ == 0)
    {
        program_ = build_program("hdr10", kVertex, kFragment);
        if (program_ == 0)
        {
            failed_ = true;
            return false;
        }
        glGenVertexArrays(1, &vao_);
    }
    if (width != width_ || height != height_)
    {
        if (texture_ == 0)
            glGenTextures(1, &texture_);
        if (framebuffer_ == 0)
            glGenFramebuffers(1, &framebuffer_);
        glBindTexture(GL_TEXTURE_2D, texture_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture_, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        {
            sys::log("[HUI] hdr10: the %dx%d layer cannot be drawn into", width, height);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            failed_ = true;
            return false;
        }
        width_ = width;
        height_ = height;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    glViewport(0, 0, width, height);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    return true;
}

void HdrComposer::finish(GLuint target, const HdrPicture &picture)
{
    if (program_ == 0 || texture_ == 0)
        return;
    GLint program = 0, vertex_array = 0, active = 0, unit0 = 0, unit1 = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vertex_array);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &unit0);
    glActiveTexture(GL_TEXTURE1);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &unit1);
    const GLboolean blend = glIsEnabled(GL_BLEND);
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    const GLboolean dither = glIsEnabled(GL_DITHER);

    // Each word must reach the screen bit for bit.
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DITHER);
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glViewport(0, 0, width_, height_);
    glUseProgram(program_);
    glUniform1i(0, 0);
    glUniform1i(1, 1);
    const float sx = static_cast<float>(width_) / 1920.0f;
    const float sy = static_cast<float>(height_) / 1080.0f;
    const bool shown = picture.texture != 0 && picture.w > 0.0f && picture.h > 0.0f;
    glUniform4f(2, picture.x * sx, picture.y * sy, shown ? picture.w * sx : 1.0f,
                shown ? picture.h * sy : 1.0f);
    glUniform1f(3, static_cast<float>(height_));
    glUniform1f(4, shown ? picture.opacity : 0.0f);
    glUniform1i(5, picture.pq ? 1 : 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, shown ? picture.texture : 0);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glBindVertexArray(static_cast<GLuint>(vertex_array));
    glUseProgram(static_cast<GLuint>(program));
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(unit1));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(unit0));
    glActiveTexture(static_cast<GLenum>(active));
    if (blend == GL_TRUE)
        glEnable(GL_BLEND);
    if (scissor == GL_TRUE)
        glEnable(GL_SCISSOR_TEST);
    if (dither == GL_TRUE)
        glEnable(GL_DITHER);
}

} // namespace hui::gfx
