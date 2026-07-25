#ifndef PSVITA_USB_SCOPE_TEXT_H
#define PSVITA_USB_SCOPE_TEXT_H

#include <stdint.h>

#define PSVITA_USB_SCOPE_GLYPH_COUNT 36u
#define PSVITA_USB_SCOPE_GLYPH_WIDTH 5u
#define PSVITA_USB_SCOPE_GLYPH_HEIGHT 7u
#define PSVITA_USB_SCOPE_MAX_TEXT_CHARS 72u
#define PSVITA_USB_SCOPE_SAFE_QUAD_BUDGET 4096u

int psvita_usb_scope_glyph_index(char character);
const uint8_t *psvita_usb_scope_glyph_rows(uint32_t index);
uint32_t psvita_usb_scope_text_glyph_quads(const char *text);
uint32_t psvita_usb_scope_text_bitmap_quads(const char *text);
uint32_t psvita_usb_scope_text_render_quads(const char *text);

#endif
