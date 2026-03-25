#include <cairo/cairo.h>
#include <glib.h>
#include <pango/pangocairo.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pango.h"

static int get_single_font_height(const char *fontstr) {
	PangoFontMap *fontmap = pango_cairo_font_map_get_default();
	PangoContext *context = pango_font_map_create_context(fontmap);
	PangoFontDescription *desc = pango_font_description_from_string(fontstr);
	PangoFont *font = pango_font_map_load_font(fontmap, context, desc);
	if (font == NULL) {
		pango_font_description_free(desc);
		g_object_unref(context);
		return -1;
	}
	PangoFontMetrics *metrics = pango_font_get_metrics(font, NULL);
	int height = pango_font_metrics_get_height(metrics) / PANGO_SCALE;
	pango_font_metrics_unref(metrics);
	g_object_unref(font);
	pango_font_description_free(desc);
	g_object_unref(context);
	return height;
}

static bool use_cjk_font(gunichar ch) {
	switch (g_unichar_get_script(ch)) {
	case G_UNICODE_SCRIPT_HAN:
	case G_UNICODE_SCRIPT_HANGUL:
	case G_UNICODE_SCRIPT_HIRAGANA:
	case G_UNICODE_SCRIPT_KATAKANA:
	case G_UNICODE_SCRIPT_BOPOMOFO:
		return true;
	default:
		break;
	}

	return (ch >= 0x2E80 && ch <= 0x9FFF) ||
		(ch >= 0xF900 && ch <= 0xFAFF) ||
		(ch >= 0xFE30 && ch <= 0xFE6F) ||
		(ch >= 0xFF00 && ch <= 0xFFEF);
}

static PangoAttrList *build_font_attrs(const char *latin_font, const char *cjk_font,
		const char *text, double scale) {
	PangoAttrList *attrs = pango_attr_list_new();
	pango_attr_list_insert(attrs, pango_attr_scale_new(scale));

	if ((!latin_font && !cjk_font) || !text || !text[0]) {
		return attrs;
	}

	for (const char *p = text; *p;) {
		const char *start = p;
		gunichar ch = g_utf8_get_char(p);
		bool cjk = use_cjk_font(ch);
		const char *font = cjk ? cjk_font : latin_font;
		p = g_utf8_next_char(p);

		while (*p) {
			ch = g_utf8_get_char(p);
			if (use_cjk_font(ch) != cjk) {
				break;
			}
			p = g_utf8_next_char(p);
		}

		if (!font || !font[0]) {
			continue;
		}

		PangoFontDescription *desc = pango_font_description_from_string(font);
		PangoAttribute *attr = pango_attr_font_desc_new(desc);
		pango_font_description_free(desc);
		attr->start_index = (guint)(start - text);
		attr->end_index = (guint)(p - text);
		pango_attr_list_insert(attrs, attr);
	}

	return attrs;
}

int get_font_height(const char *font, const char *latin_font, const char *cjk_font) {
	int height = get_single_font_height(font);
	if (latin_font && latin_font[0]) {
		int latin_height = get_single_font_height(latin_font);
		if (latin_height > height) {
			height = latin_height;
		}
	}
	if (cjk_font && cjk_font[0]) {
		int cjk_height = get_single_font_height(cjk_font);
		if (cjk_height > height) {
			height = cjk_height;
		}
	}
	return height;
}

PangoLayout *get_pango_layout(cairo_t *cairo, const char *font,
		const char *latin_font, const char *cjk_font,
		const char *text, double scale) {
	PangoLayout *layout = pango_cairo_create_layout(cairo);
	PangoAttrList *attrs = build_font_attrs(latin_font, cjk_font, text, scale);
	pango_layout_set_text(layout, text, -1);
	PangoFontDescription *desc = pango_font_description_from_string(font);
	pango_layout_set_font_description(layout, desc);
	pango_layout_set_single_paragraph_mode(layout, 1);
	pango_layout_set_attributes(layout, attrs);
	pango_font_description_free(desc);
	pango_attr_list_unref(attrs);
	return layout;
}

void get_text_size(cairo_t *cairo, const char *font, const char *latin_font,
		const char *cjk_font, int *width, int *height, int *baseline,
		double scale, const char *text) {
	PangoLayout *layout = get_pango_layout(cairo, font, latin_font, cjk_font, text, scale);
	pango_cairo_update_layout(cairo, layout);
	pango_layout_get_pixel_size(layout, width, height);
	if (baseline) {
		*baseline = pango_layout_get_baseline(layout) / PANGO_SCALE;
	}
	g_object_unref(layout);
}

int text_width(cairo_t *cairo, const char *font, const char *latin_font,
		const char *cjk_font, const char *text) {
	int text_width;
	get_text_size(cairo, font, latin_font, cjk_font, &text_width, NULL, NULL, 1, text);
	return text_width;
}

void pango_printf(cairo_t *cairo, const char *font, const char *latin_font,
		const char *cjk_font, double scale, const char *text) {
	PangoLayout *layout = get_pango_layout(cairo, font, latin_font, cjk_font, text, scale);
	cairo_font_options_t *fo = cairo_font_options_create();
	cairo_get_font_options(cairo, fo);
	pango_cairo_context_set_font_options(pango_layout_get_context(layout), fo);
	cairo_font_options_destroy(fo);
	pango_cairo_update_layout(cairo, layout);
	pango_cairo_show_layout(cairo, layout);
	g_object_unref(layout);
}
