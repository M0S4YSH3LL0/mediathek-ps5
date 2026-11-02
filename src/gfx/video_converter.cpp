// Mediathek - Turns a video picture's YUV planes into the player's RGBA texture on the GPU.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The tone mapping follows what VLC's PS5 port does (GPL-2.0-or-later,
// https://github.com/ZiZc3/VLC): PQ and HLG to light, BT.2390's curve on the brightest
// channel with HDR's reference white as the screen's white, then BT.2020 into BT.709.

#include "gfx/video_converter.hpp"

#include "gfx/gl_program.hpp"
#include "platform/ps5/system.hpp"

namespace hui::gfx
{

namespace
{

constexpr const char *kVertex = R"(
out vec2 v_uv;
void main()
{
    // One triangle over the whole target; rows are written top first, as the planes are.
    vec2 corner = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    v_uv = corner;
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)";

constexpr const char *kFragment = R"(
in vec2 v_uv;
out vec4 frag_color;
layout(location = 0) uniform sampler2D u_luma;
layout(location = 1) uniform sampler2D u_chroma;
layout(location = 2) uniform vec4 u_range;  // luma scale, luma offset, chroma scale, offset
layout(location = 3) uniform mat3 u_matrix; // Y'CbCr to R'G'B'
layout(location = 6) uniform int u_transfer; // 0 SDR, 1 PQ, 2 HLG
layout(location = 7) uniform float u_peak;   // nits
layout(location = 8) uniform int u_wide_gamut;

// BT.2408's reference white: what HDR calls paper white becomes the screen's white.
const float kWhite = 203.0;
const float kM1 = 0.1593017578125;
const float kM2 = 78.84375;
const float kC1 = 0.8359375;
const float kC2 = 18.8515625;
const float kC3 = 18.6875;
const mat3 kBt2020ToBt709 = mat3(1.6605, -0.1246, -0.0182, -0.5876, 1.1329, -0.1006,
                                 -0.0728, -0.0083, 1.1187);

vec3 pq_to_nits(vec3 e)
{
    vec3 p = pow(max(e, vec3(0.0)), vec3(1.0 / kM2));
    return 10000.0 * pow(max(p - kC1, vec3(0.0)) / (kC2 - kC3 * p), vec3(1.0 / kM1));
}

float pq_to_nits(float e)
{
    float p = pow(max(e, 0.0), 1.0 / kM2);
    return 10000.0 * pow(max(p - kC1, 0.0) / (kC2 - kC3 * p), 1.0 / kM1);
}

float nits_to_pq(float nits)
{
    float y = pow(max(nits, 0.0) / 10000.0, kM1);
    return pow((kC1 + kC2 * y) / (1.0 + kC3 * y), kM2);
}

// HLG is scene light; a 1000-nit screen shows it through a system gamma of 1.2.
vec3 hlg_to_nits(vec3 e)
{
    const float a = 0.17883277;
    const float b = 0.28466892;
    const float c = 0.55991073;
    vec3 low = e * e / 3.0;
    vec3 high = (exp((e - c) / a) + b) / 12.0;
    vec3 scene = mix(low, high, step(vec3(0.5), e));
    float level = dot(scene, vec3(0.2627, 0.6780, 0.0593));
    return 1000.0 * pow(max(level, 1e-6), 0.2) * scene;
}

// BT.2390's curve, in PQ's steps: unchanged up to a knee, then bending so the film's
// peak lands on the screen's white.
float tone_map(float nits)
{
    float source = nits_to_pq(u_peak);
    float top = nits_to_pq(kWhite) / source;
    float knee = 1.5 * top - 0.5;
    float e = min(nits_to_pq(nits) / source, 1.0);
    if (e > knee)
    {
        float t = (e - knee) / (1.0 - knee);
        float t2 = t * t;
        float t3 = t2 * t;
        e = (2.0 * t3 - 3.0 * t2 + 1.0) * knee + (t3 - 2.0 * t2 + t) * (1.0 - knee) +
            (-2.0 * t3 + 3.0 * t2) * top;
    }
    return pq_to_nits(e * source);
}

void main()
{
    float y = texture(u_luma, v_uv).r * u_range.x + u_range.y;
    vec2 c = texture(u_chroma, v_uv).rg * u_range.z + u_range.w;
    vec3 rgb = clamp(u_matrix * vec3(y, c), 0.0, 1.0);
    if (u_transfer == 0 && u_wide_gamut == 0)
    {
        frag_color = vec4(rgb, 1.0);
        return;
    }
    vec3 light;
    if (u_transfer == 1)
        light = pq_to_nits(rgb) / kWhite;
    else if (u_transfer == 2)
        light = hlg_to_nits(rgb) / kWhite;
    else
        light = pow(rgb, vec3(2.4));
    if (u_wide_gamut != 0)
        light = max(kBt2020ToBt709 * light, vec3(0.0));
    if (u_transfer != 0)
    {
        // On the brightest channel, so a colour keeps its hue as it is brought down.
        float brightest = max(max(light.r, light.g), light.b);
        if (brightest > 0.0)
            light *= tone_map(brightest * kWhite) / (brightest * kWhite);
    }
    frag_color = vec4(pow(clamp(light, 0.0, 1.0), vec3(1.0 / 2.4)), 1.0);
}
)";

// Y'CbCr to R'G'B' for a matrix's two weights, by column as GL wants it.
void colour_matrix(media::YuvMatrix matrix, float out[9])
{
    float kr = 0.2126f;
    float kb = 0.0722f;
    if (matrix == media::YuvMatrix::bt601)
    {
        kr = 0.299f;
        kb = 0.114f;
    }
    else if (matrix == media::YuvMatrix::bt2020)
    {
        kr = 0.2627f;
        kb = 0.0593f;
    }
    const float kg = 1.0f - kr - kb;
    const float values[9] = {1.0f,
                             1.0f,
                             1.0f,
                             0.0f,
                             -2.0f * kb * (1.0f - kb) / kg,
                             2.0f * (1.0f - kb),
                             2.0f * (1.0f - kr),
                             -2.0f * kr * (1.0f - kr) / kg,
                             0.0f};
    for (int i = 0; i < 9; ++i)
        out[i] = values[i];
}

void plane(GLuint texture, int width, int height, bool wide, bool pairs, const void *data)
{
    glBindTexture(GL_TEXTURE_2D, texture);
    const GLenum format = pairs ? GL_RG : GL_RED;
    const GLenum type = wide ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE;
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format, type, data);
}

GLuint make_plane(int width, int height, bool wide, bool pairs)
{
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    GLint inner = wide ? GL_R16 : GL_R8;
    if (pairs)
        inner = wide ? GL_RG16 : GL_RG8;
    glTexImage2D(GL_TEXTURE_2D, 0, inner, width, height, 0, pairs ? GL_RG : GL_RED,
                 wide ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return texture;
}

bool same(const media::YuvLayout &a, const media::YuvLayout &b)
{
    return a.luma_width == b.luma_width && a.luma_height == b.luma_height &&
           a.chroma_width == b.chroma_width && a.chroma_height == b.chroma_height &&
           a.wide == b.wide;
}

// What the converter changes, put back afterwards: it runs between the screens' own
// drawing, which relies on what it set.
struct SavedState
{
    GLint framebuffer = 0, program = 0, vertex_array = 0, active = 0, unit0 = 0, unit1 = 0;
    GLint alignment = 4;
    GLint viewport[4] = {};
    GLboolean blend = GL_FALSE, scissor = GL_FALSE;

    SavedState()
    {
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vertex_array);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
        glGetIntegerv(GL_VIEWPORT, viewport);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &unit0);
        glActiveTexture(GL_TEXTURE1);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &unit1);
        blend = glIsEnabled(GL_BLEND);
        scissor = glIsEnabled(GL_SCISSOR_TEST);
    }

    ~SavedState()
    {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(framebuffer));
        glUseProgram(static_cast<GLuint>(program));
        glBindVertexArray(static_cast<GLuint>(vertex_array));
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(unit1));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(unit0));
        glActiveTexture(static_cast<GLenum>(active));
        glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        if (blend == GL_TRUE)
            glEnable(GL_BLEND);
        if (scissor == GL_TRUE)
            glEnable(GL_SCISSOR_TEST);
    }

    SavedState(const SavedState &) = delete;
    SavedState &operator=(const SavedState &) = delete;
};

} // namespace

VideoConverter::~VideoConverter()
{
    release();
}

void VideoConverter::release()
{
    if (program_ != 0)
        glDeleteProgram(program_);
    if (vao_ != 0)
        glDeleteVertexArrays(1, &vao_);
    if (framebuffer_ != 0)
        glDeleteFramebuffers(1, &framebuffer_);
    const GLuint planes[2] = {luma_, chroma_};
    if (luma_ != 0 || chroma_ != 0)
        glDeleteTextures(2, planes);
    program_ = vao_ = framebuffer_ = luma_ = chroma_ = 0;
    layout_ = {};
}

bool VideoConverter::init()
{
    program_ = build_program("video", kVertex, kFragment);
    if (program_ == 0)
    {
        failed_ = true;
        return false;
    }
    glGenVertexArrays(1, &vao_);
    glGenFramebuffers(1, &framebuffer_);
    return true;
}

void VideoConverter::upload(const media::YuvFrame &frame)
{
    const media::YuvLayout &layout = frame.layout;
    if (!same(layout, layout_))
    {
        const GLuint old[2] = {luma_, chroma_};
        if (luma_ != 0)
            glDeleteTextures(2, old);
        luma_ = make_plane(layout.luma_width, layout.luma_height, layout.wide, false);
        chroma_ = make_plane(layout.chroma_width, layout.chroma_height, layout.wide, true);
        layout_ = layout;
    }
    // Rows have no gaps, and an odd-width 8-bit plane's rows are an odd number of bytes.
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    plane(luma_, layout.luma_width, layout.luma_height, layout.wide, false, frame.planes);
    plane(chroma_, layout.chroma_width, layout.chroma_height, layout.wide, true,
          frame.planes + media::yuv_luma_bytes(layout));
}

bool VideoConverter::convert(const media::YuvFrame &frame, std::uint32_t target, int width,
                             int height)
{
    if (failed_ || frame.planes == nullptr || target == 0 || width <= 0 || height <= 0 ||
        frame.layout.luma_width <= 0)
        return false;
    const SavedState saved;
    if (program_ == 0 && !init())
        return false;
    // Attached for this picture only. A texture deleted while an unbound framebuffer holds
    // it lives on there, and the next film's texture can get its freed name: a framebuffer
    // that kept its attachment drew every later film into a texture nobody showed (black,
    // with sound, from the second film on), and kept each old one's memory.
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer_);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
    if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    {
        sys::log("[HUI] video converter: the %dx%d target cannot be drawn into", width, height);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        failed_ = true;
        return false;
    }
    glActiveTexture(GL_TEXTURE0);
    upload(frame);

    // Samples come as 0..1 of their word; this brings them to 8-bit steps, then to the
    // range the film uses (16-235 and 16-240 unless it says full).
    const float steps = frame.layout.wide ? 65535.0f / 256.0f : 255.0f;
    const bool full = frame.colour.full_range;
    const float luma_scale = steps / (full ? 255.0f : 219.0f);
    const float luma_offset = full ? 0.0f : -16.0f / 219.0f;
    const float chroma_scale = steps / (full ? 255.0f : 224.0f);
    const float chroma_offset = -128.0f / (full ? 255.0f : 224.0f);
    float matrix[9];
    colour_matrix(frame.colour.matrix, matrix);
    int transfer = 0;
    if (frame.colour.transfer == media::Transfer::pq)
        transfer = 1;
    else if (frame.colour.transfer == media::Transfer::hlg)
        transfer = 2;

    glViewport(0, 0, width, height);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(program_);
    glUniform1i(0, 0);
    glUniform1i(1, 1);
    glUniform4f(2, luma_scale, luma_offset, chroma_scale, chroma_offset);
    glUniformMatrix3fv(3, 1, GL_FALSE, matrix);
    glUniform1i(6, transfer);
    glUniform1f(7, frame.colour.peak_nits);
    glUniform1i(8, frame.colour.wide_gamut ? 1 : 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, luma_);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, chroma_);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    return true;
}

} // namespace hui::gfx
