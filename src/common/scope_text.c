#include "scope_text.h"

static const uint8_t glyphs[PSVITA_USB_SCOPE_GLYPH_COUNT]
	[PSVITA_USB_SCOPE_GLYPH_HEIGHT] = {
	{14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},
	{30,1,1,14,1,1,30},{2,6,10,18,31,2,2},{31,16,16,30,1,1,30},
	{14,16,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},
	{14,17,17,15,1,1,14},
	{14,17,17,31,17,17,17},{30,17,17,30,17,17,30},
	{14,17,16,16,16,17,14},{30,17,17,17,17,17,30},
	{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
	{14,17,16,23,17,17,14},{17,17,17,31,17,17,17},
	{14,4,4,4,4,4,14},{7,2,2,2,18,18,12},
	{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
	{17,27,21,21,17,17,17},{17,25,21,19,17,17,17},
	{14,17,17,17,17,17,14},{30,17,17,30,16,16,16},
	{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},
	{15,16,16,14,1,1,30},{31,4,4,4,4,4,4},
	{17,17,17,17,17,17,14},{17,17,17,17,17,10,4},
	{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},
	{17,17,10,4,4,4,4},{31,1,2,4,8,16,31}
};

int psvita_usb_scope_glyph_index(char character)
{
	if (character >= '0' && character <= '9') return character - '0';
	if (character >= 'A' && character <= 'Z') return 10 + character - 'A';
	return -1;
}

const uint8_t *psvita_usb_scope_glyph_rows(uint32_t index)
{
	return index < PSVITA_USB_SCOPE_GLYPH_COUNT ? glyphs[index] : 0;
}

uint32_t psvita_usb_scope_text_glyph_quads(const char *text)
{
	if (!text) return 0;
	uint32_t quads = 0;
	for (uint32_t i = 0; text[i] && i < PSVITA_USB_SCOPE_MAX_TEXT_CHARS; ++i)
		if (psvita_usb_scope_glyph_index(text[i]) >= 0) quads++;
	return quads;
}

uint32_t psvita_usb_scope_text_bitmap_quads(const char *text)
{
	if (!text) return 0;
	uint32_t quads = 0;
	for (uint32_t i = 0; text[i] && i < PSVITA_USB_SCOPE_MAX_TEXT_CHARS; ++i) {
		int index = psvita_usb_scope_glyph_index(text[i]);
		if (index < 0) continue;
		for (uint32_t row = 0; row < PSVITA_USB_SCOPE_GLYPH_HEIGHT; ++row) {
			uint8_t bits = glyphs[index][row];
			for (uint32_t column = 0; column < PSVITA_USB_SCOPE_GLYPH_WIDTH;
			     ++column)
				if (bits & (1u << (4u - column))) quads++;
		}
	}
	return quads;
}

uint32_t psvita_usb_scope_text_render_quads(const char *text)
{
	/* The atlas renderer emits one textured quad per visible character. */
	return psvita_usb_scope_text_glyph_quads(text);
}
