#include <assert.h>
#include <cairo/cairo.h>
#include <glib.h>
#include <harfbuzz/hb-ft.h>
#include <harfbuzz/hb-glib.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <pango/pango.h>
#include <wchar.h>
#include "../entry.h"
#include "../log.h"
#include "../nelem.h"
#include "../xmalloc.h"

static void setup_hb_buffer(hb_buffer_t *buffer)
{
	hb_buffer_set_direction(buffer, HB_DIRECTION_LTR);
	hb_buffer_set_script(buffer, HB_SCRIPT_LATIN);
	hb_buffer_set_language(buffer, hb_language_from_string("en", -1));
}


static hb_buffer_t *create_hb_buffer(void)
{
	hb_buffer_t *buffer = hb_buffer_create();
	hb_buffer_set_unicode_funcs(buffer, hb_glib_get_unicode_funcs());
	setup_hb_buffer(buffer);

	return buffer;
}

static uint32_t render_hb_buffer(cairo_t *cr, hb_buffer_t *buffer, uint32_t scale)
{
	cairo_save(cr);

	cairo_font_extents_t font_extents;
	cairo_font_extents(cr, &font_extents);
	cairo_matrix_t font_matrix;
	cairo_get_font_matrix(cr, &font_matrix);
	double baseline = (font_matrix.xx - font_extents.height) / 2 + font_extents.ascent;
	cairo_translate(cr, 0, baseline);

	unsigned int glyph_count;
	hb_glyph_info_t *glyph_info = hb_buffer_get_glyph_infos(buffer, &glyph_count);
	hb_glyph_position_t *glyph_pos = hb_buffer_get_glyph_positions(buffer, &glyph_count);
	cairo_glyph_t *cairo_glyphs = xmalloc(sizeof(cairo_glyph_t) * glyph_count);

	unsigned int width = 0;
	for (unsigned int i=0; i < glyph_count; ++i) {
		width += ceil(glyph_pos[i].x_advance / 64.0 / scale);
	}

	int x = 0;
	int y = 0;
	for (unsigned int i=0; i < glyph_count; ++i) {
		cairo_glyphs[i].index = glyph_info[i].codepoint;
		cairo_glyphs[i].x = x + ceil(glyph_pos[i].x_offset / 64.0 / scale);
		cairo_glyphs[i].y = y - ceil(glyph_pos[i].y_offset / 64.0 / scale);
		x += ceil(glyph_pos[i].x_advance / 64.0 / scale);
		y -= ceil(glyph_pos[i].y_advance / 64.0 / scale);
	}

	cairo_show_glyphs(cr, cairo_glyphs, glyph_count);

	free(cairo_glyphs);

	cairo_restore(cr);

	return width;
}

void entry_backend_init(struct entry *entry, uint32_t width, uint32_t height, uint32_t scale)
{
	cairo_t *cr = entry->cairo.cr;

	/* Setup FreeType. */
	log_debug("Creating FreeType library.\n");
	assert(!FT_Init_FreeType(&entry->backend.ft_library));

	log_debug("Loading FreeType font.\n");
	assert(!FT_New_Face(entry->backend.ft_library, "font.ttf", 0, &entry->backend.ft_face));
	assert(!FT_Set_Char_Size(entry->backend.ft_face, entry->font_size * 64, entry->font_size * 64, 0, 0));

	log_debug("Creating Cairo font.\n");
	entry->backend.cairo_face = cairo_ft_font_face_create_for_ft_face(entry->backend.ft_face, 0);

	struct color color = entry->foreground_color;
	cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
	cairo_set_font_face(cr, entry->backend.cairo_face);
	cairo_set_font_size(cr, entry->font_size * (96.0 / 72.0));

	cairo_font_extents_t font_extents;
	cairo_font_extents(cr, &font_extents);

	log_debug("Creating Harfbuzz font.\n");
	entry->backend.hb_font = hb_ft_font_create_referenced(entry->backend.ft_face);

	log_debug("Creating Harfbuzz buffer.\n");
	entry->backend.hb_buffer = create_hb_buffer();

	/* Draw the prompt now, as this only needs to be done once */
	log_debug("Drawing prompt.\n");
	hb_buffer_add_utf8(entry->backend.hb_buffer, "run: ", -1, 0, -1);
	hb_shape(entry->backend.hb_font, entry->backend.hb_buffer, NULL, 0);

	/* Move and clip so we don't draw over the prompt */
	uint32_t prompt_width = render_hb_buffer(cr, entry->backend.hb_buffer, scale);
	cairo_translate(cr, prompt_width, 0);
	width -= prompt_width;
	cairo_rectangle(cr, 0, 0, width, height);
	cairo_clip(cr);
}

void entry_backend_destroy(struct entry *entry)
{
	hb_buffer_destroy(entry->backend.hb_buffer);
	hb_font_destroy(entry->backend.hb_font);
	FT_Done_Face(entry->backend.ft_face);
	FT_Done_FreeType(entry->backend.ft_library);
}

void entry_backend_update(struct entry *entry)
{
	cairo_t *cr = entry->cairo.cr;

	hb_buffer_clear_contents(entry->backend.hb_buffer);
	setup_hb_buffer(entry->backend.hb_buffer);
	hb_buffer_add_utf8(entry->backend.hb_buffer, entry->input_mb, -1, 0, -1);
	hb_shape(entry->backend.hb_font, entry->backend.hb_buffer, NULL, 0);
	render_hb_buffer(cr, entry->backend.hb_buffer, entry->image.scale);

	cairo_font_extents_t font_extents;
	cairo_font_extents(cr, &font_extents);

	for (size_t i = 0; i < 5; i++) {
		cairo_translate(cr, 0, font_extents.height);

		hb_buffer_t *buffer = entry->backend.hb_buffer;

		hb_buffer_clear_contents(buffer);
		setup_hb_buffer(buffer);
		if (i < entry->results.count) {
			hb_buffer_add_utf8(buffer, entry->results.buf[i], -1, 0, -1);
			hb_shape(entry->backend.hb_font, buffer, NULL, 0);
			render_hb_buffer(cr, buffer, entry->image.scale);
		}
	}
}