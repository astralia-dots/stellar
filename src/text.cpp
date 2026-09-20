#include "text.h"

#include <fontconfig/fontconfig.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace {

constexpr int ATLAS = 1024;
constexpr int BASE_PX = 14;

const char* const VERT = R"(#version 100
attribute vec2 a_pos;
attribute vec2 a_uv;
uniform vec2 u_size;
varying vec2 v_uv;
void main() {
    vec2 c = a_pos / u_size * 2.0 - 1.0;
    gl_Position = vec4(c.x, -c.y, 0.0, 1.0);
    v_uv = a_uv;
}
)";

// premultiplied output, matching how the compositor blends the surface
const char* const FRAG = R"(#version 100
precision mediump float;
varying vec2 v_uv;
uniform sampler2D u_tex;
uniform vec3 u_color;
void main() {
    float a = texture2D(u_tex, v_uv).a;
    gl_FragColor = vec4(u_color * a, a);
}
)";

GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        throw std::runtime_error(std::string("shader compile failed: ") + log);
    }
    return s;
}

/// Font file for the `monospace` family, via `fontconfig`.
std::string monospace_path(int& index) {
    std::string path;
    FcPattern* pat = FcPatternCreate();
    FcPatternAddString(pat, FC_FAMILY, reinterpret_cast<const FcChar8*>("monospace"));
    FcConfigSubstitute(nullptr, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult res;
    if (FcPattern* match = FcFontMatch(nullptr, pat, &res)) {
        FcChar8* file = nullptr;
        if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch)
            path = reinterpret_cast<const char*>(file);
        FcPatternGetInteger(match, FC_INDEX, 0, &index);
        FcPatternDestroy(match);
    }
    FcPatternDestroy(pat);
    return path;
}

/// Decodes the code point at `i` and advances `i`; malformed input gives U+FFFD.
char32_t decode(std::string_view s, size_t& i) {
    auto c = static_cast<unsigned char>(s[i++]);
    if (c < 0x80)
        return c;
    int n = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
    if (!n)
        return U'�'; // stray continuation byte
    char32_t cp = c & (0x3f >> n);
    for (; n && i < s.size() && (static_cast<unsigned char>(s[i]) & 0xc0) == 0x80; --n)
        cp = cp << 6 | (static_cast<unsigned char>(s[i++]) & 0x3f);
    return n ? U'�' : cp;
}

} // namespace

Text::Text() {
    if (FT_Init_FreeType(&ft_))
        throw std::runtime_error("FreeType init failed");
    int index = 0;
    std::string path = monospace_path(index);
    if (path.empty() || FT_New_Face(ft_, path.c_str(), index, &face_))
        throw std::runtime_error("no usable `monospace` font found via fontconfig");

    program_ = glCreateProgram();
    glAttachShader(program_, compile(GL_VERTEX_SHADER, VERT));
    glAttachShader(program_, compile(GL_FRAGMENT_SHADER, FRAG));
    glBindAttribLocation(program_, 0, "a_pos");
    glBindAttribLocation(program_, 1, "a_uv");
    glLinkProgram(program_);
    GLint ok = 0;
    glGetProgramiv(program_, GL_LINK_STATUS, &ok);
    if (!ok)
        throw std::runtime_error("shader link failed");
    u_size_ = glGetUniformLocation(program_, "u_size");
    u_color_ = glGetUniformLocation(program_, "u_color");
    u_tex_ = glGetUniformLocation(program_, "u_tex");

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glGenTextures(1, &tex_);
    glBindTexture(GL_TEXTURE_2D, tex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    const std::vector<unsigned char> zeros(ATLAS * ATLAS);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, ATLAS, ATLAS, 0, GL_ALPHA, GL_UNSIGNED_BYTE,
                 zeros.data());

    set_scale(1);
}

Text::~Text() {
    glDeleteTextures(1, &tex_);
    glDeleteProgram(program_);
    FT_Done_Face(face_);
    FT_Done_FreeType(ft_);
}

void Text::set_scale(int scale) {
    FT_Set_Pixel_Sizes(face_, 0, static_cast<FT_UInt>(BASE_PX * scale));
    glyphs_.clear();
    pen_x_ = pen_y_ = row_h_ = 0;
    ascent_ = static_cast<int>(face_->size->metrics.ascender >> 6);
    line_height_ = static_cast<int>(face_->size->metrics.height >> 6);
}

const Text::Glyph& Text::glyph(char32_t cp) {
    if (auto it = glyphs_.find(cp); it != glyphs_.end())
        return it->second;

    FT_UInt idx = FT_Get_Char_Index(face_, cp);
    if (!idx && cp != U'?')
        return glyphs_[cp] = glyph(U'?');

    Glyph g;
    if (FT_Load_Glyph(face_, idx, FT_LOAD_RENDER) == 0) {
        FT_GlyphSlot slot = face_->glyph;
        const FT_Bitmap& bm = slot->bitmap;
        g.advance = static_cast<float>(slot->advance.x) / 64.f;
        const int w = static_cast<int>(bm.width), h = static_cast<int>(bm.rows);
        if (w && h && bm.pixel_mode == FT_PIXEL_MODE_GRAY) {
            if (pen_x_ + w + 1 > ATLAS) { // start a new shelf
                pen_x_ = 0;
                pen_y_ += row_h_ + 1;
                row_h_ = 0;
            }
            if (pen_y_ + h <= ATLAS) {
                std::vector<unsigned char> px(static_cast<size_t>(w * h));
                for (int y = 0; y < h; ++y) {
                    const unsigned char* src = bm.buffer + y * bm.pitch;
                    std::copy_n(src, w, px.begin() + y * w);
                }
                glBindTexture(GL_TEXTURE_2D, tex_);
                glTexSubImage2D(GL_TEXTURE_2D, 0, pen_x_, pen_y_, w, h, GL_ALPHA,
                                GL_UNSIGNED_BYTE, px.data());
                g.left = slot->bitmap_left;
                g.top = -slot->bitmap_top;
                g.w = w;
                g.h = h;
                g.u0 = static_cast<float>(pen_x_) / ATLAS;
                g.v0 = static_cast<float>(pen_y_) / ATLAS;
                g.u1 = static_cast<float>(pen_x_ + w) / ATLAS;
                g.v1 = static_cast<float>(pen_y_ + h) / ATLAS;
                pen_x_ += w + 1;
                row_h_ = std::max(row_h_, h);
            }
        }
    }
    return glyphs_.emplace(cp, g).first->second;
}

void Text::begin(int width, int height) {
    width_ = width;
    height_ = height;
    verts_.clear();
}

void Text::add(std::string_view s, float x, float y) {
    const float base = y + static_cast<float>(ascent_);
    for (size_t i = 0; i < s.size();) {
        const Glyph& g = glyph(decode(s, i));
        if (g.w) {
            const float x0 = std::round(x) + static_cast<float>(g.left);
            const float y0 = base + static_cast<float>(g.top);
            const float x1 = x0 + static_cast<float>(g.w), y1 = y0 + static_cast<float>(g.h);
            const float q[] = {
                x0, y0, g.u0, g.v0, x1, y0, g.u1, g.v0, x0, y1, g.u0, g.v1,
                x1, y0, g.u1, g.v0, x1, y1, g.u1, g.v1, x0, y1, g.u0, g.v1,
            };
            verts_.insert(verts_.end(), std::begin(q), std::end(q));
        }
        x += g.advance;
    }
}

void Text::flush(float r, float g, float b) {
    if (verts_.empty())
        return;
    glUseProgram(program_);
    glUniform2f(u_size_, static_cast<float>(width_), static_cast<float>(height_));
    glUniform3f(u_color_, r, g, b);
    glUniform1i(u_tex_, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), verts_.data());
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), verts_.data() + 2);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(verts_.size() / 4));
    verts_.clear();
}
