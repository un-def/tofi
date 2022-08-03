#include <cairo/cairo.h>
#include <harfbuzz/hb-ft.h>
#include <math.h>
#include "harfbuzz.h"
#include "../entry.h"
#include "../log.h"
#include "../nelem.h"
#include "../xmalloc.h"

/*
 * FreeType is normally compiled without error strings, so we have to do this
 * funky macro trick to get them. See <freetype/fterrors.h> for more details.
 */
#undef FTERRORS_H_
#define FT_ERRORDEF( e, v, s )  { e, s },
#define FT_ERROR_START_LIST     {
#define FT_ERROR_END_LIST       { 0, NULL } };

const struct {
	int err_code;
	const char *err_msg;
} ft_errors[] =

#include <freetype/fterrors.h>

#undef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))

static const char *get_ft_error_string(int err_code)
{
	for (size_t i = 0; i < N_ELEM(ft_errors); i++) {
		if (ft_errors[i].err_code == err_code) {
			return ft_errors[i].err_msg;
		}
	}

	return "Unknown FT error";
}

/*
 * Cairo / FreeType use 72 Pts per inch, but Pango uses 96 DPI, so we have to
 * rescale for consistency.
 */
#define PT_TO_DPI (96.0 / 72.0)

/*
 * hb_buffer_clear_contents also clears some basic script information, so group
 * them here for convenience.
 */
static void setup_hb_buffer(hb_buffer_t *buffer)
{
	hb_buffer_set_direction(buffer, HB_DIRECTION_LTR);
	hb_buffer_set_script(buffer, HB_SCRIPT_LATIN);
	hb_buffer_set_language(buffer, hb_language_from_string("en", -1));
}


/*
 * Render a hb_buffer with Cairo, and return the width of the rendered area in
 * Cairo units.
 */
static uint32_t render_hb_buffer(cairo_t *cr, hb_buffer_t *buffer)
{
	cairo_save(cr);

	/*
	 * Cairo uses y-down coordinates, but HarfBuzz uses y-up, so we
	 * shift the text down by its ascent height to compensate.
	 */
	cairo_font_extents_t font_extents;
	cairo_font_extents(cr, &font_extents);
	cairo_translate(cr, 0, font_extents.ascent);

	unsigned int glyph_count;
	hb_glyph_info_t *glyph_info = hb_buffer_get_glyph_infos(buffer, &glyph_count);
	hb_glyph_position_t *glyph_pos = hb_buffer_get_glyph_positions(buffer, &glyph_count);
	cairo_glyph_t *cairo_glyphs = xmalloc(sizeof(cairo_glyph_t) * glyph_count);

	double x = 0;
	double y = 0;
	for (unsigned int i=0; i < glyph_count; i++) {
		/*
		 * The coordinates returned by HarfBuzz are in 26.6 fixed-point
		 * format, so we divide by 64.0 (2^6) to get floats.
		 */
		cairo_glyphs[i].index = glyph_info[i].codepoint;
		cairo_glyphs[i].x = x + glyph_pos[i].x_offset / 64.0;
		cairo_glyphs[i].y = y - glyph_pos[i].y_offset / 64.0;
		x += glyph_pos[i].x_advance / 64.0;
		y -= glyph_pos[i].y_advance / 64.0;
	}

	cairo_show_glyphs(cr, cairo_glyphs, glyph_count);

	free(cairo_glyphs);

	cairo_restore(cr);

	double width = 0;
	for (unsigned int i=0; i < glyph_count; i++) {
		width += glyph_pos[i].x_advance / 64.0;
	}
	return ceil(width);
}

void entry_backend_harfbuzz_init(
		struct entry *entry,
		uint32_t *width,
		uint32_t *height)
{
	cairo_t *cr = entry->cairo[0].cr;
	uint32_t font_size = floor(entry->font_size * PT_TO_DPI);

	/* Setup FreeType. */
	log_debug("Creating FreeType library.\n");
	int err;
	err = FT_Init_FreeType(&entry->harfbuzz.ft_library);
	if (err) {
		log_error("Error initialising FreeType: %s\n",
				get_ft_error_string(err));
		exit(EXIT_FAILURE);
	}

	log_debug("Loading FreeType font.\n");
	err = FT_New_Face(
			entry->harfbuzz.ft_library,
			entry->font_name,
			0,
			&entry->harfbuzz.ft_face);
	if (err) {
		log_error("Error loading font: %s\n", get_ft_error_string(err));
		exit(EXIT_FAILURE);
	}
	err = FT_Set_Char_Size(
			entry->harfbuzz.ft_face,
			font_size * 64,
			font_size * 64,
			0,
			0);
	if (err) {
		log_error("Error setting font size: %s\n",
				get_ft_error_string(err));
	}

	log_debug("Creating Cairo font.\n");
	entry->harfbuzz.cairo_face =
		cairo_ft_font_face_create_for_ft_face(entry->harfbuzz.ft_face, 0);

	struct color color = entry->foreground_color;
	cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
	cairo_set_font_face(cr, entry->harfbuzz.cairo_face);
	cairo_set_font_size(cr, font_size);
	cairo_font_options_t *opts = cairo_font_options_create();
	if (entry->harfbuzz.disable_hinting) {
		cairo_font_options_set_hint_style(opts, CAIRO_HINT_STYLE_NONE);
	} else {
		cairo_font_options_set_hint_metrics(opts, CAIRO_HINT_METRICS_ON);
	}
	cairo_set_font_options(cr, opts);


	/* We also need to set up the font for our other Cairo context. */
	cairo_set_font_face(entry->cairo[1].cr, entry->harfbuzz.cairo_face);
	cairo_set_font_size(entry->cairo[1].cr, font_size);
	cairo_set_font_options(entry->cairo[1].cr, opts);

	cairo_font_options_destroy(opts);

	log_debug("Creating Harfbuzz font.\n");
	entry->harfbuzz.hb_font =
		hb_ft_font_create_referenced(entry->harfbuzz.ft_face);

	log_debug("Creating Harfbuzz buffer.\n");
	hb_buffer_t *buffer = hb_buffer_create();
	entry->harfbuzz.hb_buffer = buffer;
	setup_hb_buffer(buffer);
}

void entry_backend_harfbuzz_destroy(struct entry *entry)
{
	hb_buffer_destroy(entry->harfbuzz.hb_buffer);
	hb_font_destroy(entry->harfbuzz.hb_font);
	cairo_font_face_destroy(entry->harfbuzz.cairo_face);
	FT_Done_Face(entry->harfbuzz.ft_face);
	FT_Done_FreeType(entry->harfbuzz.ft_library);
}

void entry_backend_harfbuzz_update(struct entry *entry)
{
	cairo_t *cr = entry->cairo[entry->index].cr;
	hb_buffer_t *buffer = entry->harfbuzz.hb_buffer;
	uint32_t width;

	cairo_save(cr);
	struct color color = entry->foreground_color;
	cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);

	/* Render the prompt */
	hb_buffer_clear_contents(buffer);
	setup_hb_buffer(buffer);
	hb_buffer_add_utf8(entry->harfbuzz.hb_buffer, entry->prompt_text, -1, 0, -1);
	hb_shape(entry->harfbuzz.hb_font, entry->harfbuzz.hb_buffer, NULL, 0);
	width = render_hb_buffer(cr, buffer);

	cairo_translate(cr, width, 0);

	/* Render the entry text */
	hb_buffer_clear_contents(buffer);
	setup_hb_buffer(buffer);
	hb_buffer_add_utf8(buffer, entry->input_mb, -1, 0, -1);
	hb_shape(entry->harfbuzz.hb_font, buffer, NULL, 0);
	width = render_hb_buffer(cr, buffer);
	width = MAX(width, entry->input_width);

	cairo_font_extents_t font_extents;
	cairo_font_extents(cr, &font_extents);

	cairo_matrix_t mat;
	/* Render our results */
	for (size_t i = 0; i < entry->results.count; i++) {
		if (entry->horizontal) {
			cairo_translate(cr, width + entry->result_spacing, 0);
		} else {
			cairo_translate(cr, 0, font_extents.height + entry->result_spacing);
		}
		if (entry->num_results == 0) {
			cairo_get_matrix(cr, &mat);
			if (entry->horizontal) {
				if (mat.x0 > entry->clip_x + entry->clip_width) {
					entry->num_results_drawn = i;
					log_debug("Drew %zu results.\n", i);
					break;
				}
			} else {
				if (mat.y0 > entry->clip_y + entry->clip_height) {
					entry->num_results_drawn = i;
					log_debug("Drew %zu results.\n", i);
					break;
				}
			}
		} else if (i >= entry->num_results) {
			break;
		}


		size_t index = i + entry->num_results * entry->page;
		/*
		 * We may be on the last page, which could have fewer results
		 * than expected, so check and break if necessary.
		 */
		if (index >= entry->results.count) {
			break;
		}

		const char *result = entry->results.buf[index].string;
		/* If this isn't the selected result, just print as normal. */
		if (i != entry->selection) {
			hb_buffer_clear_contents(buffer);
			setup_hb_buffer(buffer);
			hb_buffer_add_utf8(buffer, result, -1, 0, -1);
			hb_shape(entry->harfbuzz.hb_font, buffer, NULL, 0);
			width = render_hb_buffer(cr, buffer);
		} else {
			/*
			 * For the selected result, there's a bit more to do.
			 *
			 * First, we need to use a different foreground color -
			 * simple enough.
			 *
			 * Next, we may need to draw a background box - this
			 * involves rendering to a cairo group, measuring the
			 * size of the text, drawing the background on the main
			 * canvas, then finally drawing the group on top of
			 * that.
			 *
			 * Finally, we may need to highlight the matching
			 * portion of text - this is achieved simply by
			 * splitting the text into prematch, match and
			 * postmatch chunks, and drawing each separately.
			 */
			size_t prematch_len;
			char *prematch = xstrdup(result);
			char *match = NULL;
			char *postmatch = NULL;
			uint32_t subwidth;
			if (entry->input_mb_length > 0 && entry->selection_highlight_color.a != 0) {
				char *match_pos = strcasestr(prematch, entry->input_mb);
				if (match_pos != NULL) {
					match = xstrdup(result);
					postmatch = xstrdup(result);
					prematch_len = (match_pos - prematch);
					prematch[prematch_len] = '\0';
					match[entry->input_mb_length + prematch_len] = '\0';
				}
			}

			cairo_push_group(cr);
			color = entry->selection_foreground_color;
			cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);

			hb_buffer_clear_contents(buffer);
			setup_hb_buffer(buffer);
			hb_buffer_add_utf8(buffer, prematch, -1, 0, -1);
			hb_shape(entry->harfbuzz.hb_font, buffer, NULL, 0);
			subwidth = render_hb_buffer(cr, buffer);
			width = subwidth;

			if (match != NULL) {
				cairo_translate(cr, subwidth, 0);
				color = entry->selection_highlight_color;
				cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
				hb_buffer_clear_contents(buffer);
				setup_hb_buffer(buffer);
				hb_buffer_add_utf8(buffer, &match[prematch_len], -1, 0, -1);
				hb_shape(entry->harfbuzz.hb_font, buffer, NULL, 0);
				subwidth = render_hb_buffer(cr, buffer);
				width += subwidth;

				cairo_translate(cr, subwidth, 0);
				color = entry->selection_foreground_color;
				cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
				hb_buffer_clear_contents(buffer);
				setup_hb_buffer(buffer);
				hb_buffer_add_utf8(buffer, &postmatch[entry->input_mb_length + prematch_len], -1, 0, -1);
				hb_shape(entry->harfbuzz.hb_font, buffer, NULL, 0);
				subwidth = render_hb_buffer(cr, buffer);
				width += subwidth;

				free(match);
				free(postmatch);
				match = NULL;
				postmatch = NULL;
			}

			free(prematch);
			prematch = NULL;

			cairo_pop_group_to_source(cr);
			cairo_save(cr);
			color = entry->selection_background_color;
			cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
			int32_t pad = entry->selection_background_padding;
			if (pad < 0) {
				pad = entry->clip_width;
			}
			cairo_translate(cr, -pad, 0);
			cairo_rectangle(cr, 0, 0, width + pad * 2, font_extents.height);
			cairo_translate(cr, pad, 0);
			cairo_fill(cr);
			cairo_restore(cr);
			cairo_paint(cr);
			color = entry->foreground_color;
			cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
		}
	}

	cairo_restore(cr);
}
