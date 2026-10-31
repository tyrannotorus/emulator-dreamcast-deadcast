#include "canvas.h"
#include "log/Log.h"

#include <algorithm>
#include <cmath>

#include <ft2build.h>
#include FT_FREETYPE_H

namespace deadcast
{

u32 nextCodepoint(const std::string& text, size_t& pos)
{
	const u8 c = (u8)text[pos++];
	int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
	u32 cp = more == 3 ? c & 7 : more == 2 ? c & 0xf : more == 1 ? c & 0x1f : c;
	for (; more > 0 && pos < text.size() && ((u8)text[pos] & 0xc0) == 0x80; more--)
		cp = (cp << 6) | ((u8)text[pos++] & 0x3f);
	return cp;
}

Font::~Font()
{
	if (face != nullptr)
		FT_Done_Face(face);
}

bool Font::init(FT_Library library, const u8 *data, size_t size, int px)
{
	if (FT_New_Memory_Face(library, data, (FT_Long)size, 0, &face) != 0)
	{
		face = nullptr;
		return false;
	}
	FT_Set_Pixel_Sizes(face, 0, px);
	ascender = (int)((face->size->metrics.ascender + 32) >> 6);
	line = (int)((face->size->metrics.height + 32) >> 6);
	caps = glyph('H').top;
	return true;
}

const Font::Glyph& Font::glyph(u32 codepoint)
{
	auto it = glyphs.find(codepoint);
	if (it != glyphs.end())
		return it->second;
	Glyph& g = glyphs[codepoint];
	if (FT_Load_Char(face, codepoint, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) != 0)
		return g;
	const FT_GlyphSlot slot = face->glyph;
	g.left = slot->bitmap_left;
	g.top = slot->bitmap_top;
	g.width = (int)slot->bitmap.width;
	g.height = (int)slot->bitmap.rows;
	g.advance = (int)((slot->advance.x + 32) >> 6);
	g.alpha.resize((size_t)g.width * g.height);
	for (int y = 0; y < g.height; y++)
		std::copy_n(slot->bitmap.buffer + y * slot->bitmap.pitch, g.width, &g.alpha[(size_t)y * g.width]);
	return g;
}

int Font::measure(const std::string& text)
{
	int width = 0;
	for (size_t pos = 0; pos < text.size(); )
		width += glyph(nextCodepoint(text, pos)).advance;
	return width;
}

void Canvas::resize(int width, int height)
{
	w = width;
	h = height;
	pixels.assign((size_t)w * h, 0xff000000);
}

void Canvas::clear(u32 rgb)
{
	std::fill(pixels.begin(), pixels.end(), 0xff000000 | rgb);
}

void Canvas::copy(const Canvas& from)
{
	if (from.w == w && from.h == h)
		std::copy(from.pixels.begin(), from.pixels.end(), pixels.begin());
}

void Canvas::fill(int x, int y, int width, int height, u32 rgb)
{
	const int x0 = std::max(x, 0), x1 = std::min(x + width, w);
	const int y0 = std::max(y, 0), y1 = std::min(y + height, h);
	for (int yy = y0; yy < y1; yy++)
		std::fill(&pixels[(size_t)yy * w + x0], &pixels[(size_t)yy * w + std::max(x0, x1)], 0xff000000 | rgb);
}

void Canvas::blend(int x, int y, u32 rgb, u32 alpha)
{
	if (x < 0 || y < 0 || x >= w || y >= h || alpha == 0)
		return;
	u32& p = pixels[(size_t)y * w + x];
	if (alpha >= 255)
	{
		p = 0xff000000 | rgb;
		return;
	}
	const u32 inv = 255 - alpha;
	const u32 r = (((rgb >> 16) & 0xff) * alpha + ((p >> 16) & 0xff) * inv + 127) / 255;
	const u32 g = (((rgb >> 8) & 0xff) * alpha + ((p >> 8) & 0xff) * inv + 127) / 255;
	const u32 b = ((rgb & 0xff) * alpha + (p & 0xff) * inv + 127) / 255;
	p = 0xff000000 | (r << 16) | (g << 8) | b;
}

void Canvas::roundRect(int x, int y, int width, int height, int r, u32 rgb)
{
	r = std::min(r, std::min(width, height) / 2);
	for (int yy = 0; yy < height; yy++)
	{
		const float py = yy + 0.5f;
		// the corners' rows: each pixel covered as far as the arc is from its center
		const float cy = py < r ? r : py > height - r ? height - r : py;
		if (cy == py)
		{
			fill(x, y + yy, width, 1, rgb);
			continue;
		}
		auto corner = [&](int xx) {
			const float px = xx + 0.5f;
			const float cx = px < r ? r : px > width - r ? width - r : px;
			const float d = std::hypot(px - cx, py - cy) - r;
			blend(x + xx, y + yy, rgb, (u32)(std::clamp(0.5f - d, 0.f, 1.f) * 255.f + 0.5f));
		};
		for (int xx = 0; xx < r; xx++)
			corner(xx);
		// between the corners, the row's edge alone
		const u32 alpha = (u32)(std::clamp(0.5f - (std::abs(py - cy) - r), 0.f, 1.f) * 255.f + 0.5f);
		if (alpha >= 255)
			fill(x + r, y + yy, width - 2 * r, 1, rgb);
		else
			for (int xx = r; xx < width - r; xx++)
				blend(x + xx, y + yy, rgb, alpha);
		for (int xx = std::max(r, width - r); xx < width; xx++)
			corner(xx);
	}
}

void Canvas::polygon(const float *xy, int count, u32 rgb)
{
	float minX = xy[0], maxX = xy[0], minY = xy[1], maxY = xy[1];
	for (int i = 1; i < count; i++)
	{
		minX = std::min(minX, xy[i * 2]);
		maxX = std::max(maxX, xy[i * 2]);
		minY = std::min(minY, xy[i * 2 + 1]);
		maxY = std::max(maxY, xy[i * 2 + 1]);
	}
	// 4 x 4 samples a pixel, each in or out by the even-odd rule
	for (int y = (int)std::floor(minY); y < (int)std::ceil(maxY); y++)
		for (int x = (int)std::floor(minX); x < (int)std::ceil(maxX); x++)
		{
			int inside = 0;
			for (int s = 0; s < 16; s++)
			{
				const float px = x + ((s & 3) + 0.5f) / 4.f;
				const float py = y + ((s >> 2) + 0.5f) / 4.f;
				bool in = false;
				for (int i = 0, j = count - 1; i < count; j = i++)
				{
					const float xi = xy[i * 2], yi = xy[i * 2 + 1], xj = xy[j * 2], yj = xy[j * 2 + 1];
					if ((yi > py) != (yj > py) && px < (xj - xi) * (py - yi) / (yj - yi) + xi)
						in = !in;
				}
				inside += in;
			}
			blend(x, y, rgb, (u32)(inside * 255 / 16));
		}
}

void Canvas::picture(const u8 *rgb, int width, int height, int x, int y, int toWidth, int toHeight, float brightness)
{
	if (width <= 0 || height <= 0 || toWidth <= 0 || toHeight <= 0)
		return;
	// bilinear, sampling at the destination pixels' centers
	const float sx = (float)width / toWidth;
	const float sy = (float)height / toHeight;
	const u32 scale = (u32)(std::clamp(brightness, 0.f, 1.f) * 256.f);
	for (int yy = std::max(0, -y); yy < toHeight && y + yy < h; yy++)
	{
		const float fy = std::clamp((yy + 0.5f) * sy - 0.5f, 0.f, (float)height - 1);
		const int y0 = (int)fy, y1 = std::min(y0 + 1, height - 1);
		const u32 wy = (u32)((fy - y0) * 256.f);
		u32 *out = &pixels[(size_t)(y + yy) * w];
		for (int xx = std::max(0, -x); xx < toWidth && x + xx < w; xx++)
		{
			const float fx = std::clamp((xx + 0.5f) * sx - 0.5f, 0.f, (float)width - 1);
			const int x0 = (int)fx, x1 = std::min(x0 + 1, width - 1);
			const u32 wx = (u32)((fx - x0) * 256.f);
			const u8 *a = &rgb[((size_t)y0 * width + x0) * 3];
			const u8 *b = &rgb[((size_t)y0 * width + x1) * 3];
			const u8 *c = &rgb[((size_t)y1 * width + x0) * 3];
			const u8 *d = &rgb[((size_t)y1 * width + x1) * 3];
			u32 p = 0xff000000;
			for (int ch = 0; ch < 3; ch++)
			{
				const u32 top = a[ch] * (256 - wx) + b[ch] * wx;
				const u32 bottom = c[ch] * (256 - wx) + d[ch] * wx;
				const u32 v = ((top * (256 - wy) + bottom * wy) >> 16) * scale >> 8;
				p |= std::min(v, 255u) << (16 - ch * 8);
			}
			out[x + xx] = p;
		}
	}
}

int Canvas::text(Font& font, const std::string& text, int x, int baseline, u32 rgb)
{
	for (size_t pos = 0; pos < text.size(); )
	{
		const Font::Glyph& g = font.glyph(nextCodepoint(text, pos));
		for (int gy = 0; gy < g.height; gy++)
			for (int gx = 0; gx < g.width; gx++)
				blend(x + g.left + gx, baseline - g.top + gy, rgb, g.alpha[(size_t)gy * g.width + gx]);
		x += g.advance;
	}
	return x;
}

std::vector<u8> blur(const std::vector<u8>& rgb, int width, int height, int shrink, int radius, int& outWidth, int& outHeight)
{
	const int w = outWidth = width / shrink;
	const int h = outHeight = height / shrink;
	if (w <= 0 || h <= 0 || rgb.size() < (size_t)width * height * 3)
	{
		outWidth = outHeight = 0;
		return {};
	}
	// each pixel the average of its shrink x shrink block
	std::vector<u32> sums((size_t)w * h * 3);
	for (int y = 0; y < h * shrink; y++)
		for (int x = 0; x < w * shrink; x++)
			for (int ch = 0; ch < 3; ch++)
				sums[((size_t)(y / shrink) * w + x / shrink) * 3 + ch] += rgb[((size_t)y * width + x) * 3 + ch];
	std::vector<u32> a((size_t)w * h * 3), b((size_t)w * h * 3);
	for (size_t i = 0; i < a.size(); i++)
		a[i] = sums[i] / (shrink * shrink);
	// a box blur along rows (step 1) or columns (step w), the edges repeated
	auto pass = [&](const std::vector<u32>& in, std::vector<u32>& out, int count, int lines, int step, int lineStep) {
		const int span = 2 * radius + 1;
		for (int line = 0; line < lines; line++)
			for (int ch = 0; ch < 3; ch++)
			{
				auto at = [&](int i) { return in[((size_t)line * lineStep + (size_t)std::clamp(i, 0, count - 1) * step) * 3 + ch]; };
				u32 sum = 0;
				for (int i = -radius; i <= radius; i++)
					sum += at(i);
				for (int i = 0; i < count; i++)
				{
					out[((size_t)line * lineStep + (size_t)i * step) * 3 + ch] = sum / span;
					sum += at(i + radius + 1) - at(i - radius);
				}
			}
	};
	for (int i = 0; i < 3; i++)
	{
		pass(a, b, w, h, 1, w);
		pass(b, a, h, w, w, 1);
	}
	std::vector<u8> out(a.size());
	for (size_t i = 0; i < a.size(); i++)
		out[i] = (u8)std::min(a[i], 255u);
	return out;
}

std::string Canvas::fit(Font& font, const std::string& text, int maxWidth)
{
	if (font.measure(text) <= maxWidth)
		return text;
	std::string cut = text;
	while (!cut.empty())
	{
		// drop the last codepoint and any trailing spaces
		size_t end = cut.size() - 1;
		while (end > 0 && ((u8)cut[end] & 0xc0) == 0x80)
			end--;
		cut.erase(end);
		while (!cut.empty() && cut.back() == ' ')
			cut.pop_back();
		if (font.measure(cut + "...") <= maxWidth)
			break;
	}
	return cut + "...";
}

}
