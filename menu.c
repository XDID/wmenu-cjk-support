#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <ctype.h>
#include <poll.h>
#include <stdbool.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <xkbcommon/xkbcommon.h>

#include "menu.h"

#include "pango.h"
#include "render.h"
#include "wayland.h"

// Creates and returns a new menu.
struct menu *menu_create(menu_callback callback) {
	struct menu *menu = calloc(1, sizeof(struct menu));
	menu->strncmp = strncmp;
	menu->font = "monospace 10";
	menu->latin_font = NULL;
	menu->cjk_font = NULL;
	menu->normalbg = 0x222222ff;
	menu->normalfg = 0xbbbbbbff;
	menu->promptbg = 0x005577ff;
	menu->promptfg = 0xeeeeeeff;
	menu->selectionbg = 0x005577ff;
	menu->selectionfg = 0xeeeeeeff;
	menu->callback = callback;
	menu->test_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
	menu->test_cairo = cairo_create(menu->test_surface);
	return menu;
}

static void free_pages(struct menu *menu) {
	struct page *next = menu->pages;
	while (next) {
		struct page *page = next;
		next = page->next;
		free(page);
	}
}

static void free_items(struct menu *menu) {
	for (size_t i = 0; i < menu->item_count; i++) {
		struct item *item = &menu->items[i];
		free(item->text);
	}
	free(menu->items);
}

// Destroys the menu, freeing memory associated with it.
void menu_destroy(struct menu *menu) {
	free_pages(menu);
	free_items(menu);
	cairo_destroy(menu->test_cairo);
	cairo_surface_destroy(menu->test_surface);
	free(menu);
}

static bool parse_color(const char *color, uint32_t *result) {
	if (color[0] == '#') {
		++color;
	}
	size_t len = strlen(color);
	if ((len != 6 && len != 8) || !isxdigit(color[0]) || !isxdigit(color[1])) {
		return false;
	}
	char *ptr;
	uint32_t parsed = (uint32_t)strtoul(color, &ptr, 16);
	if (*ptr != '\0') {
		return false;
	}
	*result = len == 6 ? ((parsed << 8) | 0xFF) : parsed;
	return true;
}

// Parse menu options from command line arguments.
void menu_getopts(struct menu *menu, int argc, char *argv[]) {
        const char *usage =
                "Usage: wmenu [-bFiPv] [-f font] [-E english-font] [-C chinese-font]\n"
                "\t[-l lines] [-o output] [-p prompt]\n"
                "\t[-N color] [-n color] [-M color] [-m color] [-S color] [-s color]\n"
                "\t-F enable fuzzy matching\n";

	int opt;
	while ((opt = getopt(argc, argv, "bhiPFvf:E:C:l:o:p:N:n:M:m:S:s:")) != -1) {
		switch (opt) {
		case 'b':
			menu->bottom = true;
			break;
		case 'i':
			menu->strncmp = strncasecmp;
			break;
		case 'P':
			menu->passwd = true;
			break;
		case 'F':
			menu->fuzzy = true;
			break;
		case 'v':
			puts("wmenu " VERSION);
			exit(EXIT_SUCCESS);
		case 'f':
			menu->font = optarg;
			break;
		case 'E':
			menu->latin_font = optarg;
			break;
		case 'C':
			menu->cjk_font = optarg;
			break;
		case 'l':
			menu->lines = atoi(optarg);
			break;
		case 'o':
			menu->output_name = optarg;
			break;
		case 'p':
			menu->prompt = optarg;
			break;
		case 'N':
			if (!parse_color(optarg, &menu->normalbg)) {
				fprintf(stderr, "Invalid background color: %s", optarg);
			}
			break;
		case 'n':
			if (!parse_color(optarg, &menu->normalfg)) {
				fprintf(stderr, "Invalid foreground color: %s", optarg);
			}
			break;
		case 'M':
			if (!parse_color(optarg, &menu->promptbg)) {
				fprintf(stderr, "Invalid prompt background color: %s", optarg);
			}
			break;
		case 'm':
			if (!parse_color(optarg, &menu->promptfg)) {
				fprintf(stderr, "Invalid prompt foreground color: %s", optarg);
			}
			break;
		case 'S':
			if (!parse_color(optarg, &menu->selectionbg)) {
				fprintf(stderr, "Invalid selection background color: %s", optarg);
			}
			break;
		case 's':
			if (!parse_color(optarg, &menu->selectionfg)) {
				fprintf(stderr, "Invalid selection foreground color: %s", optarg);
			}
			break;
		default:
			fprintf(stderr, "%s", usage);
			exit(EXIT_FAILURE);
		}
	}

	if (optind < argc) {
		fprintf(stderr, "%s", usage);
		exit(EXIT_FAILURE);
	}

	int height = get_font_height(menu->font, menu->latin_font, menu->cjk_font);
	menu->line_height = height + 2;
	menu->height = menu->line_height;
	if (menu->lines > 0) {
		menu->height += menu->height * menu->lines;
	}
	menu->padding = height / 2;
}

// Add an item to the menu.
void menu_add_item(struct menu *menu, char *text) {
	if ((menu->item_count & (menu->item_count - 1)) == 0) {
		size_t alloc_size = menu->item_count ? 2 * menu->item_count : 1;
		void *new_array = realloc(menu->items, sizeof(struct item) * alloc_size);
		if (!new_array) {
			fprintf(stderr, "could not realloc %zu bytes", sizeof(struct item) * alloc_size);
			exit(EXIT_FAILURE);
		}
		menu->items = new_array;
	}

	struct item *new = &menu->items[menu->item_count];
	new->text = text;

	menu->item_count++;
}

static int compare_items(const void *a, const void *b) {
	const struct item *item_a = a;
	const struct item *item_b = b;
	return strcmp(item_a->text, item_b->text);
}

void menu_sort_and_deduplicate(struct menu *menu) {
	size_t j = 1;
	size_t i;

	qsort(menu->items, menu->item_count, sizeof(*menu->items), compare_items);

	for (i = 1; i < menu->item_count; i++) {
		if (strcmp(menu->items[i].text, menu->items[j - 1].text) == 0) {
			free(menu->items[i].text);
		} else {
			menu->items[j] = menu->items[i];
			j++;
		}
	}
	menu->item_count = j;
}

static void append_page(struct page *page, struct page **first, struct page **last) {
	if (*last) {
		(*last)->next = page;
	} else {
		*first = page;
	}
	page->prev = *last;
	page->next = NULL;
	*last = page;
}

static void page_items(struct menu *menu) {
	// Free existing pages
	while (menu->pages != NULL) {
		struct page *page = menu->pages;
		menu->pages = menu->pages->next;
		free(page);
	}

	if (!menu->matches) {
		return;
	}

	// Make new pages
	if (menu->lines > 0) {
		struct page *pages_end = NULL;
		struct item *item = menu->matches;
		while (item) {
			struct page *page = calloc(1, sizeof(struct page));
			page->first = item;

			for (int i = 1; item && i <= menu->lines; i++) {
				item->page = page;
				page->last = item;
				item = item->next_match;
			}
			append_page(page, &menu->pages, &pages_end);
		}
	} else {
		// Calculate available space
		int max_width = menu->width - menu->inputw - menu->promptw
			- menu->left_arrow - menu->right_arrow;

		struct page *pages_end = NULL;
		struct item *item = menu->matches;
		while (item) {
			struct page *page = calloc(1, sizeof(struct page));
			page->first = item;

			int total_width = 0;
			int items = 0;
			while (item) {
				total_width += item->width + 2 * menu->padding;
				if (total_width > max_width && items > 0) {
					break;
				}
				items++;

				item->page = page;
				page->last = item;
				item = item->next_match;
			}
			append_page(page, &menu->pages, &pages_end);
		}
	}
}

static const char *fstrstr(struct menu *menu, const char *s, const char *sub) {
	for (size_t len = strlen(sub); *s; s++) {
		if (!menu->strncmp(s, sub, len)) {
			return s;
		}
	}
	return NULL;
}

static void append_match(struct item *item, struct item **first, struct item **last) {
	if (*last) {
		(*last)->next_match = item;
	} else {
		*first = item;
	}
	item->prev_match = *last;
	item->next_match = NULL;
	*last = item;
}

struct fuzzy_match {
	struct item *item;
	int score;
	size_t index;
};

static char normalize_char(struct menu *menu, char c) {
	if (menu->strncmp == strncasecmp) {
		return (char)tolower((unsigned char)c);
	}
	return c;
}

static bool fuzzy_match(struct menu *menu, const char *text, const char *pattern, int *score) {
	if (!pattern[0]) {
		*score = 0;
		return true;
	}
	int start = -1;
	int last = -1;
	int total_gap = 0;
	int idx = 0;
	for (size_t pi = 0; pattern[pi]; pi++) {
		char pc = normalize_char(menu, pattern[pi]);
		bool found = false;
		for (; text[idx]; idx++) {
			char tc = normalize_char(menu, text[idx]);
			if (tc == pc) {
				if (start == -1) {
					start = idx;
				} else {
					total_gap += idx - last - 1;
				}
				last = idx;
				idx++;
				found = true;
				break;
			}
		}
		if (!found) {
			return false;
		}
	}
	if (start == -1) {
		return false;
	}
	int span = last - start;
	*score = start * 2048 + total_gap * 128 + span;
	return true;
}

static int compare_fuzzy(const void *a, const void *b) {
	const struct fuzzy_match *ma = a;
	const struct fuzzy_match *mb = b;
	if (ma->score != mb->score) {
		return (ma->score > mb->score) - (ma->score < mb->score);
	}
	if (ma->index < mb->index) {
		return -1;
	}
	if (ma->index > mb->index) {
		return 1;
	}
	return 0;
}

static void fuzzy_match_items(struct menu *menu) {
	struct fuzzy_match *matches = NULL;
	size_t match_count = 0;
	size_t match_capacity = 0;
	for (size_t k = 0; k < menu->item_count; k++) {
		struct item *item = &menu->items[k];
		int score;
		if (!fuzzy_match(menu, item->text, menu->input, &score)) {
			continue;
		}
		if (match_count == match_capacity) {
			size_t new_capacity = match_capacity ? match_capacity * 2 : 16;
			void *new_matches = realloc(matches, new_capacity * sizeof(*matches));
			if (!new_matches) {
				fprintf(stderr, "could not realloc %zu bytes", new_capacity * sizeof(*matches));
				exit(EXIT_FAILURE);
			}
			match_capacity = new_capacity;
			matches = new_matches;
		}
		matches[match_count].item = item;
		matches[match_count].score = score;
		matches[match_count].index = k;
		match_count++;
	}
	if (match_count == 0) {
		free(matches);
		menu->matches = NULL;
		menu->matches_end = NULL;
		return;
	}
	qsort(matches, match_count, sizeof(*matches), compare_fuzzy);
	menu->matches = matches[0].item;
	menu->matches->prev_match = NULL;
	menu->matches->next_match = NULL;
	struct item *prev = menu->matches;
	for (size_t i = 1; i < match_count; i++) {
		struct item *item = matches[i].item;
		item->prev_match = prev;
		item->next_match = NULL;
		prev->next_match = item;
		prev = item;
	}
	prev->next_match = NULL;
	menu->matches_end = prev;
	free(matches);
}

static void match_items(struct menu *menu) {
	struct item *lexact = NULL, *exactend = NULL;
	struct item *lprefix = NULL, *prefixend = NULL;
	struct item *lsubstr  = NULL, *substrend = NULL;
	char buf[sizeof menu->input], *tok;
	char **tokv = NULL;
	int i, tokc = 0;
	size_t k;
	size_t tok_len;
	menu->matches = NULL;
	menu->matches_end = NULL;
	menu->sel = NULL;

	size_t input_len = strlen(menu->input);
	if (menu->fuzzy && input_len > 0) {
		fuzzy_match_items(menu);
		page_items(menu);
		if (menu->pages) {
			menu->sel = menu->pages->first;
		}
		return;
	}

	/* tokenize input by space for matching the tokens individually */
	strcpy(buf, menu->input);
	tok = strtok(buf, " ");
	while (tok) {
		tokv = realloc(tokv, (tokc + 1) * sizeof *tokv);
		if (!tokv) {
			fprintf(stderr, "could not realloc %zu bytes",
				(tokc + 1) * sizeof *tokv);
			exit(EXIT_FAILURE);
		}
		tokv[tokc] = tok;
		tokc++;
		tok = strtok(NULL, " ");
	}
	tok_len = tokc ? strlen(tokv[0]) : 0;

	for (k = 0; k < menu->item_count; k++) {
		struct item *item = &menu->items[k];
		for (i = 0; i < tokc; i++) {
			if (!fstrstr(menu, item->text, tokv[i])) {
				/* token does not match */
				break;
			}
		}
		if (i != tokc) {
			/* not all tokens match */
			continue;
		}
		if (!tokc || !menu->strncmp(menu->input, item->text, input_len + 1)) {
			append_match(item, &lexact, &exactend);
		} else if (!menu->strncmp(tokv[0], item->text, tok_len)) {
			append_match(item, &lprefix, &prefixend);
		} else {
			append_match(item, &lsubstr, &substrend);
		}
	}

	free(tokv);

	if (lexact) {
		menu->matches = lexact;
		menu->matches_end = exactend;
	}
	if (lprefix) {
		if (menu->matches_end) {
			menu->matches_end->next_match = lprefix;
			lprefix->prev_match = menu->matches_end;
		} else {
			menu->matches = lprefix;
		}
		menu->matches_end = prefixend;
	}
	if (lsubstr) {
		if (menu->matches_end) {
			menu->matches_end->next_match = lsubstr;
			lsubstr->prev_match = menu->matches_end;
		} else {
			menu->matches = lsubstr;
		}
		menu->matches_end = substrend;
	}

	page_items(menu);
	if (menu->pages) {
		menu->sel = menu->pages->first;
	}
}
// Marks the menu as needing to be rendered again.
void menu_invalidate(struct menu *menu) {
	menu->rendered = false;
}

// Render menu items.
void menu_render_items(struct menu *menu) {
	calc_widths(menu);
	match_items(menu);
	render_menu(menu);
}

static void insert(struct menu *menu, const char *text, ssize_t len) {
	if (strlen(menu->input) + len > sizeof menu->input - 1) {
		return;
	}
	size_t move_len = strlen(menu->input) - menu->cursor + 1;
	if (len < 0) {
		move_len += (size_t)(-len);
	}
	memmove(menu->input + menu->cursor + len, menu->input + menu->cursor, move_len);
	if (len > 0 && text != NULL) {
		memcpy(menu->input + menu->cursor, text, len);
	}
	menu->cursor += len;
}

static void update_input_state(struct menu *menu, bool rematch) {
	if (rematch) {
		match_items(menu);
	}
	if (menu->context) {
		context_update_text_input(menu->context);
	}
	menu_invalidate(menu);
}

// Add pasted text to the menu input.
void menu_paste(struct menu *menu, const char *text, ssize_t len) {
	insert(menu, text, len);
	update_input_state(menu, true);
}

static void set_preedit(struct menu *menu, const char *text,
		int32_t cursor_begin, int32_t cursor_end) {
	if (!text) {
		text = "";
	}
	strncpy(menu->preedit, text, sizeof(menu->preedit) - 1);
	menu->preedit[sizeof(menu->preedit) - 1] = '\0';
	if (cursor_begin >= 0 && cursor_end >= 0) {
		size_t preedit_len = strlen(menu->preedit);
		size_t cursor = (size_t)cursor_end;
		if (cursor > preedit_len) {
			cursor = preedit_len;
		}
		menu->preedit_cursor = cursor;
		menu->preedit_cursor_visible = true;
	} else {
		menu->preedit_cursor = 0;
		menu->preedit_cursor_visible = false;
	}
}

void menu_apply_text_input(struct menu *menu, uint32_t delete_before,
		uint32_t delete_after, const char *commit, const char *preedit,
		int32_t preedit_cursor_begin, int32_t preedit_cursor_end) {
	menu->preedit[0] = '\0';
	menu->preedit_cursor = 0;
	menu->preedit_cursor_visible = false;

	if (delete_before > menu->cursor) {
		delete_before = menu->cursor;
	}
	size_t len = strlen(menu->input);
	if (delete_after > len - menu->cursor) {
		delete_after = len - menu->cursor;
	}
	if (delete_before || delete_after) {
		size_t start = menu->cursor - delete_before;
		size_t end = menu->cursor + delete_after;
		memmove(menu->input + start, menu->input + end, len - end + 1);
		menu->cursor = start;
	}

	if (commit && commit[0]) {
		insert(menu, commit, (ssize_t)strlen(commit));
	}

	if (preedit && preedit[0]) {
		set_preedit(menu, preedit, preedit_cursor_begin, preedit_cursor_end);
	}

	update_input_state(menu, true);
}

static size_t nextrune(struct menu *menu, int incr) {
	size_t n, len;

	len = strlen(menu->input);
	for(n = menu->cursor + incr; n < len && (menu->input[n] & 0xc0) == 0x80; n += incr);
	return n;
}

// Move the cursor to the beginning or end of the word, skipping over any preceding whitespace.
static void movewordedge(struct menu *menu, int dir) {
	if (dir < 0) {
		// Move to beginning of word
		while (menu->cursor > 0 && menu->input[nextrune(menu, -1)] == ' ') {
			menu->cursor = nextrune(menu, -1);
		}
		while (menu->cursor > 0 && menu->input[nextrune(menu, -1)] != ' ') {
			menu->cursor = nextrune(menu, -1);
		}
	} else {
		// Move to end of word
		size_t len = strlen(menu->input);
		while (menu->cursor < len && menu->input[menu->cursor] == ' ') {
			menu->cursor = nextrune(menu, +1);
		}
		while (menu->cursor < len && menu->input[menu->cursor] != ' ') {
			menu->cursor = nextrune(menu, +1);
		}
	}
}

// Handle a keypress.
void menu_keypress(struct menu *menu, enum wl_keyboard_key_state key_state,
		xkb_keysym_t sym) {
	if (key_state != WL_KEYBOARD_KEY_STATE_PRESSED) {
		return;
	}

	struct xkb_state *state = context_get_xkb_state(menu->context);
	bool ctrl = xkb_state_mod_name_is_active(state, XKB_MOD_NAME_CTRL,
			XKB_STATE_MODS_DEPRESSED | XKB_STATE_MODS_LATCHED);
	bool meta = xkb_state_mod_name_is_active(state, XKB_MOD_NAME_ALT,
			XKB_STATE_MODS_DEPRESSED | XKB_STATE_MODS_LATCHED);
	bool shift = xkb_state_mod_name_is_active(state, XKB_MOD_NAME_SHIFT,
			XKB_STATE_MODS_DEPRESSED | XKB_STATE_MODS_LATCHED);

	size_t len = strlen(menu->input);

	if (ctrl) {
		// Emacs-style line editing bindings
		switch (sym) {
		case XKB_KEY_a:
			sym = XKB_KEY_Home;
			break;
		case XKB_KEY_b:
			sym = XKB_KEY_Left;
			break;
		case XKB_KEY_c:
			sym = XKB_KEY_Escape;
			break;
		case XKB_KEY_d:
			sym = XKB_KEY_Delete;
			break;
		case XKB_KEY_e:
			sym = XKB_KEY_End;
			break;
		case XKB_KEY_f:
			sym = XKB_KEY_Right;
			break;
		case XKB_KEY_g:
			sym = XKB_KEY_Escape;
			break;
		case XKB_KEY_bracketleft:
			sym = XKB_KEY_Escape;
			break;
		case XKB_KEY_h:
			sym = XKB_KEY_BackSpace;
			break;
		case XKB_KEY_i:
			sym = XKB_KEY_Tab;
			break;
		case XKB_KEY_j:
		case XKB_KEY_J:
		case XKB_KEY_m:
		case XKB_KEY_M:
			sym = XKB_KEY_Return;
			ctrl = false;
			break;
		case XKB_KEY_n:
			sym = XKB_KEY_Down;
			break;
		case XKB_KEY_p:
			sym = XKB_KEY_Up;
			break;

		case XKB_KEY_k:
			// Delete right
			menu->input[menu->cursor] = '\0';
			update_input_state(menu, true);
			return;
		case XKB_KEY_u:
			// Delete left
			insert(menu, NULL, 0 - menu->cursor);
			update_input_state(menu, true);
			return;
		case XKB_KEY_w:
			// Delete word
			while (menu->cursor > 0 && menu->input[nextrune(menu, -1)] == ' ') {
				insert(menu, NULL, nextrune(menu, -1) - menu->cursor);
			}
			while (menu->cursor > 0 && menu->input[nextrune(menu, -1)] != ' ') {
				insert(menu, NULL, nextrune(menu, -1) - menu->cursor);
			}
			update_input_state(menu, true);
			return;
		case XKB_KEY_Y:
			// Paste clipboard
			if (!context_paste(menu->context)) {
				return;
			}
			return;
		case XKB_KEY_Left:
		case XKB_KEY_KP_Left:
			movewordedge(menu, -1);
			update_input_state(menu, false);
			return;
		case XKB_KEY_Right:
		case XKB_KEY_KP_Right:
			movewordedge(menu, +1);
			update_input_state(menu, false);
			return;

		case XKB_KEY_Return:
		case XKB_KEY_KP_Enter:
			break;
		default:
			return;
		}
	} else if (meta) {
		// Emacs-style line editing bindings
		switch (sym) {
		case XKB_KEY_b:
			movewordedge(menu, -1);
			update_input_state(menu, false);
			return;
		case XKB_KEY_f:
			movewordedge(menu, +1);
			update_input_state(menu, false);
			return;
		case XKB_KEY_g:
			sym = XKB_KEY_Home;
			break;
		case XKB_KEY_G:
			sym = XKB_KEY_End;
			break;
		case XKB_KEY_h:
			sym = XKB_KEY_Up;
			break;
		case XKB_KEY_j:
			sym = XKB_KEY_Next;
			break;
		case XKB_KEY_k:
			sym = XKB_KEY_Prior;
			break;
		case XKB_KEY_l:
			sym = XKB_KEY_Down;
			break;
		default:
			return;
		}
	}

	char buf[8];
	switch (sym) {
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		if (shift) {
			menu->callback(menu, menu->input, true);
		} else {
			char *text = menu->sel ? menu->sel->text : menu->input;
			menu->callback(menu, text, !ctrl);
		}
		break;
	case XKB_KEY_Left:
	case XKB_KEY_KP_Left:
	case XKB_KEY_Up:
	case XKB_KEY_KP_Up:
		if (menu->sel && menu->sel->prev_match) {
			menu->sel = menu->sel->prev_match;
			update_input_state(menu, false);
		} else if (menu->cursor > 0) {
			menu->cursor = nextrune(menu, -1);
			update_input_state(menu, false);
		}
		break;
	case XKB_KEY_Right:
	case XKB_KEY_KP_Right:
	case XKB_KEY_Down:
	case XKB_KEY_KP_Down:
		if (menu->cursor < len) {
			menu->cursor = nextrune(menu, +1);
			update_input_state(menu, false);
		} else if (menu->sel && menu->sel->next_match) {
			menu->sel = menu->sel->next_match;
			update_input_state(menu, false);
		}
		break;
	case XKB_KEY_Prior:
	case XKB_KEY_KP_Prior:
		if (menu->sel && menu->sel->page->prev) {
			menu->sel = menu->sel->page->prev->first;
			update_input_state(menu, false);
		}
		break;
	case XKB_KEY_Next:
	case XKB_KEY_KP_Next:
		if (menu->sel && menu->sel->page->next) {
			menu->sel = menu->sel->page->next->first;
			update_input_state(menu, false);
		}
		break;
	case XKB_KEY_Home:
	case XKB_KEY_KP_Home:
		if (menu->sel == menu->matches) {
			menu->cursor = 0;
			update_input_state(menu, false);
		} else {
			menu->sel = menu->matches;
			update_input_state(menu, false);
		}
		break;
	case XKB_KEY_End:
	case XKB_KEY_KP_End:
		if (menu->cursor < len) {
			menu->cursor = len;
			update_input_state(menu, false);
		} else {
			menu->sel = menu->matches_end;
			update_input_state(menu, false);
		}
		break;
	case XKB_KEY_BackSpace:
		if (menu->cursor > 0) {
			insert(menu, NULL, nextrune(menu, -1) - menu->cursor);
			update_input_state(menu, true);
		}
		break;
	case XKB_KEY_Delete:
	case XKB_KEY_KP_Delete:
		if (menu->cursor == len) {
			return;
		}
		menu->cursor = nextrune(menu, +1);
		insert(menu, NULL, nextrune(menu, -1) - menu->cursor);
		update_input_state(menu, true);
		break;
	case XKB_KEY_Tab:
		if (!menu->sel) {
			return;
		}
		menu->cursor = strnlen(menu->sel->text, sizeof menu->input - 1);
		memcpy(menu->input, menu->sel->text, menu->cursor);
		menu->input[menu->cursor] = '\0';
		update_input_state(menu, true);
		break;
	case XKB_KEY_Escape:
		menu->exit = true;
		menu->failure = true;
		break;
	default:
		if (xkb_keysym_to_utf8(sym, buf, 8)) {
			insert(menu, buf, strnlen(buf, 8));
			update_input_state(menu, true);
		}
	}
}
