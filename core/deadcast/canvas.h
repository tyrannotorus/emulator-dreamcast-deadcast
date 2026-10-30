// CPU drawing for the menu: BGRA8 pixels as fb0 stores them, anti-aliased shapes and FreeType text.
// Colors are 0xRRGGBB.
#pragma once
#include "types.h"

#include <string>
#include <unordered_map>
#include <vector>

typedef struct FT_LibraryRec_ *FT_Library;
typedef struct FT_FaceRec_ *FT_Face;

namespace deadcast
{

class Font
{
public:
	~Font();
	// px: the em size in pixels
	bool init(FT_Library library, const u8 *data, size_t size, int px);

	int ascent() const { return ascender; }
	int capHeight() const { return caps; }
	int lineHeight() const { return line; }
	int measure(const std::string& text);

private:
	struct Glyph
	{
		int left = 0;		// from the pen
		int top = 0;		// above the baseline
		int width = 0;
		int height = 0;
		int advance = 0;
		std::vector<u8> alpha;
	};
	const Glyph& glyph(u32 codepoint);

	FT_Face face = nullptr;
	int ascender = 0;
	int caps = 0;
	int line = 0;
	std::unordered_map<u32, Glyph> glyphs;

	friend class Canvas;
};

class Canvas
{
public:
	void resize(int width, int height);
	int width() const { return w; }
	int height() const { return h; }
	const u32 *data() const { return pixels.data(); }

	void clear(u32 rgb);
	void fill(int x, int y, int width, int height, u32 rgb);
	// rounded rectangle, radius r; a pill is r = height / 2
	void roundRect(int x, int y, int width, int height, int r, u32 rgb);
	void pill(int x, int y, int width, int height, u32 rgb) { roundRect(x, y, width, height, height / 2, rgb); }
	void circle(int cx, int cy, int r, u32 rgb) { roundRect(cx - r, cy - r, 2 * r, 2 * r, r, rgb); }
	// a small polygon (icons): count points, x then y each
	void polygon(const float *xy, int count, u32 rgb);
	// an RGB24 picture scaled into the rectangle, its channels multiplied by brightness
	void picture(const u8 *rgb, int width, int height, int x, int y, int toWidth, int toHeight, float brightness = 1.f);
	// text with its baseline at y; returns the x after it
	int text(Font& font, const std::string& text, int x, int baseline, u32 rgb);
	// text shortened with "..." to fit maxWidth
	static std::string fit(Font& font, const std::string& text, int maxWidth);

private:
	void blend(int x, int y, u32 rgb, u32 alpha);	// alpha 0-255

	int w = 0;
	int h = 0;
	std::vector<u32> pixels;
};

// the next codepoint of UTF-8 text, from pos
u32 nextCodepoint(const std::string& text, size_t& pos);

// a heavily blurred, downscaled copy of an RGB24 picture (shrink, then three box blurs), to be drawn
// scaled up with Canvas::picture
std::vector<u8> blur(const std::vector<u8>& rgb, int width, int height, int shrink, int radius, int& outWidth, int& outHeight);

}
