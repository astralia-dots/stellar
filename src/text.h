#pragma once

#include <GLES2/gl2.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include <string_view>
#include <unordered_map>
#include <vector>

/// Draws UTF-8 text using `FreeType` glyphs cached in a `GL_ALPHA` atlas, with
/// `monospace` resolved through `fontconfig`. Needs a current GL context and
/// throws `std::runtime_error` if no font or shader is usable.
///
/// ponytail: one font, no fallback, shaping or bidi; missing glyphs draw as `?`.
/// The 1024x1024 atlas never grows, so new glyphs are skipped once it is full.
class Text {
public:
    Text();
    ~Text();
    Text(const Text&) = delete;
    Text& operator=(const Text&) = delete;

    /// Rebuilds the glyph cache at 14px times the buffer scale.
    void set_scale(int scale);
    int line_height() const { return line_height_; }

    /// Starts a batch for a `width` x `height` px framebuffer.
    void begin(int width, int height);
    /// Queues `utf8` with the line box's top-left at (`x`, `y`) px.
    void add(std::string_view utf8, float x, float y);
    /// Draws the batch in one opaque colour.
    void flush(float r, float g, float b);

private:
    struct Glyph {
        float advance = 0;
        int left = 0, top = 0, w = 0, h = 0; // quad offset from the pen and baseline, y down
        float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    };
    const Glyph& glyph(char32_t cp);

    FT_Library ft_ = nullptr;
    FT_Face face_ = nullptr;
    GLuint program_ = 0, tex_ = 0;
    GLint u_size_ = 0, u_color_ = 0, u_tex_ = 0;
    std::unordered_map<char32_t, Glyph> glyphs_;
    int pen_x_ = 0, pen_y_ = 0, row_h_ = 0; // shelf packer cursor
    int ascent_ = 0, line_height_ = 0;
    int width_ = 0, height_ = 0;
    std::vector<float> verts_; // x, y, u, v per vertex, 6 per glyph
};
