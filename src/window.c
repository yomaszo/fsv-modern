/* window.c */

/* Main window definition */

/* fsv - 3D File System Visualizer
 * Copyright (C)1999 Daniel Richard G. <skunk@mit.edu>
 * Copyright (C) 2021 Janne Blomqvist <blomqvist.janne@gmail.com>
 *
 * SPDX-License-Identifier:  LGPL-2.1-or-later
 */


#include "common.h"
#include "window.h"

#include <gtk/gtk.h>
#include <time.h>

#include "about.h"
#include "callbacks.h"
#include "camera.h"
#include "color.h"
#include "colexp.h"
#include "dialog.h"
#include "dirtree.h"
#include "filelist.h"
#include "fsv.h"
#include "geometry.h"
#include "gui.h"
#include "ogl.h"
#include "viewport.h"

/* Main program icon */
#include "xmaps/fsv-icon.xpm"


/* Color radio menu items */
static GtkWidget *color_by_nodetype_rmenu_item_w;
static GtkWidget *color_by_timestamp_rmenu_item_w;
static GtkWidget *color_by_wpattern_rmenu_item_w;
static GtkWidget *color_by_filetype_rmenu_item_w;
static GtkWidget *color_mode_button_w;
static GtkWidget *color_mode_label_w;
static GtkWidget *mapv_view_button_w;
static GtkWidget *treev_view_button_w;
static GtkWidget *search_entry_w;
static gchar *search_last_query;
static gchar *search_last_match_path;

/* Bird's-eye view toggle in the header */
static GtkWidget *overview_tbutton_w;

/* List of widgets that can be enabled or disabled on the fly using
 * window_set_access( ) */
static GList *sw_widget_list = NULL;

/* Three parts of the bottom status bar */
static GtkWidget *summary_label_w;
static GtkWidget *path_label_w;
static GtkWidget *fps_label_w;
static GtkWidget *files_section_label_w;
static GtkWidget *breadcrumb_label_w;
static GtkWidget *breadcrumb_panel_w;
static GtkWidget *context_toolbar_w;
static GtkWidget *context_node_label_w;
static GtkWidget *context_expand_button_w;
static GtkWidget *context_expand_all_button_w;
static GtkWidget *context_look_button_w;
static GtkWidget *context_properties_button_w;
static GtkWidget *properties_panel_w;
static GtkWidget *properties_name_w;
static GtkWidget *properties_type_w;
static GtkWidget *properties_size_w;
static GtkWidget *properties_ratio_w;
static GtkWidget *properties_path_w;
static GtkWidget *properties_files_w;
static GtkWidget *properties_folders_w;
static GtkWidget *properties_modified_w;
static GtkWidget *properties_owner_w;
static GtkWidget *properties_mode_w;
static GtkWidget *properties_type_bar_w;
static GtkWidget *legend_w;
static GtkWidget *legend_items_w;
static GtkWidget *legend_toggle_menu_item_w;
static GtkWidget *scan_logo_w;
static GtkWidget *scene_fade_w;
static guint scan_logo_tick_id;
static guint scene_fade_tick_id;
static gint64 scan_logo_started_at;
static gboolean motion_overlays_hidden;
typedef struct {
	int64 bytes[6];
	const char *labels[6];
	const char *classes[6];
} FileTypeTotals;
static GNode *selected_node;
static FileTypeTotals properties_type_totals;
static GNode *properties_scan_root;
static GNode *properties_scan_current;
static guint properties_scan_source;

static void window_refresh_context_toolbar(void);
static void window_add_style_class(GtkWidget *widget, const char *class_name);
static void window_update_properties_panel(void);

void
window_reset_search(void)
{
	g_clear_pointer(&search_last_query, g_free);
	g_clear_pointer(&search_last_match_path, g_free);
	if (search_entry_w != NULL)
		gtk_entry_set_text(GTK_ENTRY(search_entry_w), "");
}

static GNode *
window_next_search_node(GNode *node)
{
	if (node->children != NULL)
		return node->children;
	while (node != NULL && node->next == NULL)
		node = node->parent;
	return node != NULL ? node->next : NULL;
}

static gchar *
window_search_fold(const char *text)
{
	gchar *valid = g_utf8_make_valid(text != NULL ? text : "", -1);
	gchar *folded = g_utf8_casefold(valid, -1);
	g_free(valid);
	return folded;
}

static void
window_search_activate(GtkEntry *entry, gpointer user_data)
{
	const char *text = gtk_entry_get_text(entry);
	gchar *query = g_strstrip(g_strdup(text));
	gchar *folded_query;
	GNode *node, *first_match = NULL, *next_match = NULL, *match = NULL;
	gboolean previous_match_seen;
	guint match_count = 0, next_match_index = 0;
	(void)user_data;

	if (*query == '\0') {
		g_free(query);
		return;
	}
	if (globals.fsv_mode == FSV_SPLASH || root_dnode == NULL) {
		window_statusbar(SB_RIGHT, _("Please wait for the filesystem scan to finish."));
		g_free(query);
		return;
	}

	if (g_strcmp0(query, search_last_query) != 0) {
		g_free(search_last_query);
		search_last_query = g_strdup(query);
		g_clear_pointer(&search_last_match_path, g_free);
	}

	folded_query = window_search_fold(query);
	previous_match_seen = search_last_match_path == NULL;
	for (node = root_dnode; node != NULL; node = window_next_search_node(node)) {
		gchar *folded_name;
		gboolean is_match;
		if (NODE_IS_METANODE(node))
			continue;
		folded_name = window_search_fold(NODE_DESC(node)->name);
		is_match = strstr(folded_name, folded_query) != NULL;
		g_free(folded_name);
		if (!is_match)
			continue;

		match_count++;
		if (first_match == NULL)
			first_match = node;
		if (!previous_match_seen) {
			if (g_strcmp0(node_absname(node), search_last_match_path) == 0) {
				previous_match_seen = TRUE;
			}
		}
		else if (next_match == NULL) {
			next_match = node;
			next_match_index = match_count;
		}
	}

	g_free(folded_query);
	if (match_count == 0) {
		g_clear_pointer(&search_last_match_path, g_free);
		window_statusbar(SB_RIGHT, _("No files or folders match that search."));
		g_free(query);
		return;
	}

	match = next_match != NULL ? next_match : first_match;
	if (next_match == NULL)
		next_match_index = 1;
	g_free(search_last_match_path);
	search_last_match_path = g_strdup(node_absname(match));
	window_set_selected_node(match);
	camera_look_at(match);

	gchar *message = g_strdup_printf(_("Search result %u of %u: %s"),
		next_match_index, match_count, node_absname(match));
	window_statusbar(SB_RIGHT, message);
	g_free(message);
	g_free(query);
}

static gboolean
window_scan_logo_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
	double elapsed;
	double pulse;
	(void)data;
	if (scan_logo_started_at == 0)
		scan_logo_started_at = gdk_frame_clock_get_frame_time(clock);
	elapsed = (gdk_frame_clock_get_frame_time(clock) - scan_logo_started_at) / 1000000.0;
	pulse = 0.5 + 0.5 * cos(2.0 * PI * elapsed / 2.4);
	gtk_widget_set_opacity(widget, 0.28 + 0.72 * pulse);
	return G_SOURCE_CONTINUE;
}

static gboolean
window_scene_fade_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
	double progress, eased;
	(void)clock;
	(void)data;
	/* Use the camera's master-pan progress as the clock. That value is
	 * advanced by the same animation loop as the camera, so the scene can
	 * never fade ahead of (or finish before) its first camera movement. */
	progress = CLAMP(camera->pan_part, 0.0, 1.0);
	/* Match the MORPH_SIGMOID curve used by the initial camera pan. */
	eased = 0.5 * (1.0 - cos(PI * progress));
	gtk_widget_set_opacity(widget, 1.0 - eased);
	if (progress >= 1.0) {
		gtk_widget_hide(widget);
		scene_fade_tick_id = 0;
		return G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
}

void
window_scan_logo_show(boolean show)
{
	if (scan_logo_w == NULL)
		return;
	if (show) {
		scan_logo_started_at = 0;
		gtk_widget_set_opacity(scan_logo_w, 1.0);
		gtk_widget_show(scan_logo_w);
		if (breadcrumb_panel_w != NULL)
			gtk_widget_hide(breadcrumb_panel_w);
		if (legend_w != NULL)
			gtk_widget_hide(legend_w);
		if (scan_logo_tick_id == 0)
			scan_logo_tick_id = gtk_widget_add_tick_callback(scan_logo_w,
				window_scan_logo_tick, NULL, NULL);
	}
	else {
		if (scan_logo_tick_id != 0) {
			gtk_widget_remove_tick_callback(scan_logo_w, scan_logo_tick_id);
			scan_logo_tick_id = 0;
		}
		gtk_widget_set_opacity(scan_logo_w, 1.0);
		gtk_widget_hide(scan_logo_w);
		if (breadcrumb_panel_w != NULL)
			gtk_widget_show(breadcrumb_panel_w);
		if (legend_w != NULL && legend_toggle_menu_item_w != NULL &&
		    gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(legend_toggle_menu_item_w)))
			gtk_widget_show(legend_w);
	}
}

void
window_prepare_scene_fade(void)
{
	if (scene_fade_w == NULL)
		return;
	if (scene_fade_tick_id != 0) {
		gtk_widget_remove_tick_callback(scene_fade_w, scene_fade_tick_id);
		scene_fade_tick_id = 0;
	}
	gtk_widget_set_opacity(scene_fade_w, 1.0);
	gtk_widget_show(scene_fade_w);
}

void
window_set_motion_overlays(boolean moving)
{
	if (moving) {
		motion_overlays_hidden = TRUE;
		if (legend_w != NULL)
			gtk_widget_hide(legend_w);
		if (context_toolbar_w != NULL)
			gtk_widget_hide(context_toolbar_w);
		if (breadcrumb_panel_w != NULL)
			gtk_widget_hide(breadcrumb_panel_w);
		return;
	}

	if (!motion_overlays_hidden)
		return;

	motion_overlays_hidden = FALSE;
	if (legend_w != NULL && legend_toggle_menu_item_w != NULL &&
	    gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(legend_toggle_menu_item_w)))
		gtk_widget_show(legend_w);
	if (breadcrumb_panel_w != NULL &&
	    (scan_logo_w == NULL || !gtk_widget_get_visible(scan_logo_w)))
		gtk_widget_show(breadcrumb_panel_w);
	window_refresh_context_toolbar();
}

void
window_start_scene_fade(void)
{
	if (scene_fade_w == NULL)
		return;
	if (scene_fade_tick_id == 0)
		scene_fade_tick_id = gtk_widget_add_tick_callback(scene_fade_w,
			window_scene_fade_tick, NULL, NULL);
}

static void
window_legend_visibility_toggled(GtkCheckMenuItem *item, gpointer user_data)
{
	(void)user_data;
	if (legend_w != NULL) {
		gboolean visible = gtk_check_menu_item_get_active(item);
		if (motion_overlays_hidden)
			gtk_widget_hide(legend_w);
		else
			gtk_widget_set_visible(legend_w, visible);
	}
}

static void
window_legend_close_clicked(GtkButton *button, gpointer user_data)
{
	(void)button;
	(void)user_data;
	if (legend_toggle_menu_item_w != NULL)
		gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(legend_toggle_menu_item_w), FALSE);
}

static void
window_header_view_toggled(GtkToggleButton *button, gpointer user_data)
{
	FsvMode mode = GPOINTER_TO_INT(user_data);
	if (gtk_toggle_button_get_active(button) && globals.fsv_mode != mode)
		fsv_set_mode(mode);
}

static void
window_change_root_clicked(GtkButton *button, gpointer user_data)
{
	(void)button;
	(void)user_data;
	dialog_change_root();
}

static void
window_menu_look_at_root(GtkMenuItem *item, gpointer user_data)
{
	(void)item;
	(void)user_data;
	if (root_dnode != NULL)
		camera_look_at(root_dnode);
}

static void
window_menu_back(GtkMenuItem *item, gpointer user_data)
{
	(void)item;
	(void)user_data;
	camera_look_at_previous();
}

static GtkWidget *
window_menu_button(const char *label, GtkWidget *menu)
{
	GtkWidget *button = gtk_menu_button_new();
	GtkWidget *child = label ? gtk_label_new(label) :
		gtk_image_new_from_icon_name("open-menu-symbolic", GTK_ICON_SIZE_BUTTON);
	gtk_container_add(GTK_CONTAINER(button), child);
	gtk_menu_button_set_popup(GTK_MENU_BUTTON(button), menu);
	gtk_widget_show(child);
	return button;
}

static void
window_button_icon_label(GtkWidget *button, const char *icon_name,
			 const char *label)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *image = gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_BUTTON);
	GtkWidget *text = gtk_label_new(label);
	gtk_box_pack_start(GTK_BOX(box), image, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), text, FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(button), box);
	gtk_widget_show_all(box);
}

static void
window_context_expand_clicked(GtkButton *button, gpointer user_data)
{
	boolean expanded;
	(void)button;
	(void)user_data;
	if (selected_node == NULL || !NODE_IS_DIR(selected_node))
		return;
	expanded = dirtree_entry_expanded(selected_node) ||
		(!dirtree_entry_has_subdir(selected_node) &&
		 DIR_NODE_DESC(selected_node)->deployment > (1.0 - EPSILON));
	colexp(selected_node, expanded ? COLEXP_COLLAPSE_RECURSIVE : COLEXP_EXPAND);
	window_refresh_context_toolbar();
}

static void
window_context_expand_all_clicked(GtkButton *button, gpointer user_data)
{
	(void)button;
	(void)user_data;
	if (selected_node != NULL && NODE_IS_DIR(selected_node)) {
		colexp(selected_node, COLEXP_EXPAND_RECURSIVE);
		window_refresh_context_toolbar();
	}
}

static void
window_context_look_clicked(GtkButton *button, gpointer user_data)
{
	(void)button;
	(void)user_data;
	if (selected_node != NULL)
		camera_look_at(selected_node);
}

static void
window_context_properties_clicked(GtkButton *button, gpointer user_data)
{
	(void)button;
	(void)user_data;
	if (selected_node != NULL && properties_panel_w != NULL) {
		window_update_properties_panel();
		gtk_widget_show(properties_panel_w);
	}
}

static void
window_properties_close_clicked(GtkButton *button, gpointer user_data)
{
	(void)button;
	(void)user_data;
	if (properties_scan_source != 0) {
		g_source_remove(properties_scan_source);
		properties_scan_source = 0;
	}
	gtk_widget_hide(properties_panel_w);
}

static int
window_file_category(GNode *node)
{
	const char *name = NODE_DESC(node)->name;
	const char *dot = strrchr(name, '.');
	char *ext;
	static const char *source[] = {"c", "h", "cc", "cpp", "hpp", "py", "js", "ts", "rs", "go", "java", "sh", "json", "toml", "yml", "yaml", "xml", "html", "css", "sql", NULL};
	static const char *media[] = {"png", "jpg", "jpeg", "gif", "webp", "svg", "mp3", "ogg", "flac", "wav", "mp4", "mkv", "mov", NULL};
	static const char *archives[] = {"zip", "tar", "gz", "xz", "bz2", "7z", "rar", "iso", "db", "sqlite", "sqlite3", NULL};
	static const char *documents[] = {"pdf", "txt", "md", "odt", "doc", "docx", "rtf", "epub", "ppt", "pptx", "xls", "xlsx", NULL};
	const char **list;
	int category;
	if (NODE_DESC(node)->type != NODE_REGFILE)
		return NODE_DESC(node)->type == NODE_SYMLINK ? 5 : 3;
	if (dot == NULL || dot == name || dot[1] == '\0')
		return (NODE_DESC(node)->perms & 0111) ? 3 : 5;
	ext = g_ascii_strdown(dot + 1, -1);
	category = (NODE_DESC(node)->perms & 0111) ? 3 : 5;
	for (int i = 0; i < 4; i++) {
		list = i == 0 ? source : i == 1 ? media : i == 2 ? archives : documents;
		for (int j = 0; list[j] != NULL; j++)
			if (strcmp(ext, list[j]) == 0) {
				category = i;
				goto classified;
			}
	}
classified:
	g_free(ext);
	return category;
}

static void
window_properties_add_row(GtkWidget *grid, const char *caption,
			  GtkWidget **value_out, int row)
{
	GtkWidget *key = gtk_label_new(caption);
	GtkWidget *value = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(key), 0.0);
	gtk_label_set_xalign(GTK_LABEL(value), 1.0);
	gtk_label_set_ellipsize(GTK_LABEL(value), PANGO_ELLIPSIZE_MIDDLE);
	gtk_widget_set_hexpand(value, TRUE);
	gtk_grid_attach(GTK_GRID(grid), key, 0, row, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), value, 1, row, 1, 1);
	*value_out = value;
}

static void
window_render_properties_type_bar(void)
{
	GList *children, *iter;
	int64 total = 0;
	gchar *tooltip;
	if (properties_type_bar_w == NULL || properties_scan_root != selected_node)
		return;
	children = gtk_container_get_children(GTK_CONTAINER(properties_type_bar_w));
	for (iter = children; iter != NULL; iter = iter->next)
		gtk_widget_destroy(GTK_WIDGET(iter->data));
	g_list_free(children);
	for (int i = 0; i < 6; i++)
		total += properties_type_totals.bytes[i];
	for (int i = 0; i < 6; i++) {
		GtkWidget *segment = gtk_drawing_area_new();
		int width = total > 0 ? MAX(2, (int)(220.0 * properties_type_totals.bytes[i] / total)) : 2;
		gtk_widget_set_size_request(segment, width, 5);
		window_add_style_class(segment, "fsv-swatch");
		window_add_style_class(segment, properties_type_totals.classes[i]);
		tooltip = g_strdup_printf("%s · %s", properties_type_totals.labels[i], abbrev_size(properties_type_totals.bytes[i]));
		gtk_widget_set_tooltip_text(segment, tooltip);
		g_free(tooltip);
		gtk_box_pack_start(GTK_BOX(properties_type_bar_w), segment, FALSE, FALSE, 0);
		gtk_widget_show(segment);
	}
}

static gboolean
window_properties_type_scan_idle(gpointer user_data)
{
	int budget = 500;
	(void)user_data;
	while (properties_scan_current != NULL && budget-- > 0) {
		GNode *node = properties_scan_current;
		if (!NODE_IS_DIR(node) && !NODE_IS_METANODE(node))
			properties_type_totals.bytes[window_file_category(node)] += MAX((int64)0, NODE_DESC(node)->size);
		if (node->children != NULL)
			properties_scan_current = node->children;
		else {
			while (node != properties_scan_root && node->next == NULL)
				node = node->parent;
			properties_scan_current = node == properties_scan_root ? NULL : node->next;
		}
	}
	if (properties_scan_current != NULL)
		return G_SOURCE_CONTINUE;
	properties_scan_source = 0;
	window_render_properties_type_bar();
	properties_scan_root = NULL;
	return G_SOURCE_REMOVE;
}

static void
window_update_properties_panel(void)
{
	GNode *node = selected_node;
	GDateTime *modified;
	gchar *modified_text;
	int64 size;
	unsigned int files = 0, folders = 0;
	char *size_text;
	char *path;
	char *display_name;
	char mode_text[8];
	if (node == NULL || properties_name_w == NULL)
		return;
	display_name = g_filename_display_name(NODE_DESC(node)->name[0] ?
		NODE_DESC(node)->name : _("Filesystem root"));
	gtk_label_set_text(GTK_LABEL(properties_name_w), display_name);
	g_free(display_name);
	gtk_label_set_text(GTK_LABEL(properties_type_w), NODE_IS_DIR(node) ?
		(DIR_EXPANDED(node) ? _("Directory · expanded") : _("Directory · collapsed")) : _(node_type_names[NODE_DESC(node)->type]));
	size = NODE_IS_DIR(node) ? DIR_NODE_DESC(node)->subtree.size : NODE_DESC(node)->size;
	size_text = g_strdup(abbrev_size(size));
	gtk_label_set_text(GTK_LABEL(properties_size_w), size_text);
	g_free(size_text);
	if (NODE_IS_DIR(node)) {
		for (int i = NODE_REGFILE; i < NUM_NODE_TYPES; i++)
			files += DIR_NODE_DESC(node)->subtree.counts[i];
		folders = DIR_NODE_DESC(node)->subtree.counts[NODE_DIRECTORY];
	}
	else
		files = 1;
	modified_text = g_strdup_printf("%u", files);
	gtk_label_set_text(GTK_LABEL(properties_files_w), modified_text);
	g_free(modified_text);
	modified_text = g_strdup_printf("%u", folders);
	gtk_label_set_text(GTK_LABEL(properties_folders_w), modified_text);
	g_free(modified_text);
	path = (char *)node_absname(node);
	display_name = g_filename_display_name(path);
	gtk_label_set_text(GTK_LABEL(properties_path_w), display_name);
	g_free(display_name);
	if (node->parent != NULL && NODE_IS_DIR(node->parent) && DIR_NODE_DESC(node->parent)->subtree.size > 0) {
		double percent = 100.0 * (double)size / DIR_NODE_DESC(node->parent)->subtree.size;
		modified_text = g_strdup_printf(_("%.0f%% of parent"), percent);
		gtk_label_set_text(GTK_LABEL(properties_ratio_w), modified_text);
		g_free(modified_text);
	}
	else
		gtk_label_set_text(GTK_LABEL(properties_ratio_w), _("Root directory"));
	modified = g_date_time_new_from_unix_local(NODE_DESC(node)->mtime);
	modified_text = modified ? g_date_time_format(modified, "%Y-%m-%d %H:%M") : g_strdup(_("Unknown"));
	gtk_label_set_text(GTK_LABEL(properties_modified_w), modified_text);
	g_free(modified_text);
	if (modified != NULL)
		g_date_time_unref(modified);
	modified_text = g_strdup_printf("%u : %u", (unsigned int)NODE_DESC(node)->user_id, (unsigned int)NODE_DESC(node)->group_id);
	gtk_label_set_text(GTK_LABEL(properties_owner_w), modified_text);
	g_free(modified_text);
	g_snprintf(mode_text, sizeof(mode_text), "%04o", NODE_DESC(node)->perms & 0777);
	gtk_label_set_text(GTK_LABEL(properties_mode_w), mode_text);
	if (properties_scan_source != 0)
		g_source_remove(properties_scan_source);
	memset(&properties_type_totals, 0, sizeof(properties_type_totals));
	properties_type_totals.labels[0] = _("SOURCE CODE");
	properties_type_totals.labels[1] = _("MEDIA");
	properties_type_totals.labels[2] = _("ARCHIVES");
	properties_type_totals.labels[3] = _("SYSTEM / EXEC");
	properties_type_totals.labels[4] = _("DOCUMENTS");
	properties_type_totals.labels[5] = _("OTHER");
	properties_type_totals.classes[0] = "type-code";
	properties_type_totals.classes[1] = "type-media";
	properties_type_totals.classes[2] = "type-archive";
	properties_type_totals.classes[3] = "type-system";
	properties_type_totals.classes[4] = "type-docs";
	properties_type_totals.classes[5] = "type-other";
	properties_scan_root = node;
	properties_scan_current = node;
	properties_scan_source = g_idle_add(window_properties_type_scan_idle, NULL);
}

typedef struct {
	gboolean gradient;
	SpectrumType spectrum_type;
	RGBcolor start;
	RGBcolor end;
} LegendSwatch;

static gboolean
window_legend_swatch_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
	LegendSwatch *swatch = user_data;
	GtkAllocation allocation;
	double width, height;

	gtk_widget_get_allocation(widget, &allocation);
	width = allocation.width;
	height = allocation.height;
	if (swatch->gradient) {
		RGBcolor *boundary_colors[] = { &swatch->start, &swatch->end };
		cairo_pattern_t *pattern = cairo_pattern_create_linear(0, 0, width, 0);
		for (int i = 0; i <= 16; i++) {
			double position = i / 16.0;
			RGBcolor color = color_spectrum_color(swatch->spectrum_type,
				position, swatch->spectrum_type == SPECTRUM_GRADIENT ?
				boundary_colors : NULL);
			cairo_pattern_add_color_stop_rgb(pattern, position,
				color.r, color.g, color.b);
		}
		cairo_set_source(cr, pattern);
		cairo_rectangle(cr, 0.5, 0.5, width - 1.0, height - 1.0);
		cairo_fill(cr);
		cairo_pattern_destroy(pattern);
	}
	else {
		cairo_set_source_rgb(cr, swatch->start.r, swatch->start.g, swatch->start.b);
		cairo_rectangle(cr, 0.5, 0.5, width - 1.0, height - 1.0);
		cairo_fill(cr);
	}

	cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.55);
	cairo_set_line_width(cr, 1.0);
	cairo_rectangle(cr, 0.5, 0.5, width - 1.0, height - 1.0);
	cairo_stroke(cr);
	return FALSE;
}

static void
window_legend_swatch_free(gpointer user_data, GClosure *closure)
{
	(void)closure;
	g_free(user_data);
}

static void
window_add_legend_item(const char *label_text, const RGBcolor *start,
		       const RGBcolor *end, SpectrumType spectrum_type)
{
	GtkWidget *row_w = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *swatch_w = gtk_drawing_area_new();
	GtkWidget *label_w = gtk_label_new(label_text);
	LegendSwatch *swatch = g_new0(LegendSwatch, 1);

	gtk_widget_set_size_request(swatch_w, 28, 12);
	swatch->start = *start;
	swatch->end = end != NULL ? *end : *start;
	swatch->gradient = end != NULL;
	swatch->spectrum_type = spectrum_type;
	g_signal_connect_data(swatch_w, "draw", G_CALLBACK(window_legend_swatch_draw),
		swatch, window_legend_swatch_free, 0);
	gtk_box_pack_start(GTK_BOX(row_w), swatch_w, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row_w), label_w, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(legend_items_w), row_w, FALSE, FALSE, 2);
	gtk_widget_show_all(row_w);
}

static void
window_add_legend_section(const char *title)
{
	GtkWidget *label_w = gtk_label_new(title);
	gtk_label_set_xalign(GTK_LABEL(label_w), 0.0);
	gtk_style_context_add_class(gtk_widget_get_style_context(label_w), "fsv-meta");
	gtk_box_pack_start(GTK_BOX(legend_items_w), label_w, FALSE, FALSE, 3);
	gtk_widget_show(label_w);
}

static gchar *
window_format_timestamp(time_t timestamp)
{
	GDateTime *date = g_date_time_new_from_unix_local((gint64)timestamp);
	gchar *text = date != NULL ? g_date_time_format(date, "%Y-%m-%d") :
		g_strdup(_("Unknown"));
	if (date != NULL)
		g_date_time_unref(date);
	return text;
}

void
window_update_color_legend(void)
{
	GList *children, *link;
	struct ColorConfig config;
	ColorMode mode;

	if (legend_items_w == NULL)
		return;

	children = gtk_container_get_children(GTK_CONTAINER(legend_items_w));
	for (link = children; link != NULL; link = link->next)
		gtk_widget_destroy(GTK_WIDGET(link->data));
	g_list_free(children);

	color_get_config(&config);
	mode = color_get_mode();
	switch (mode) {
		case COLOR_BY_NODETYPE:
			window_add_legend_section(_("Node types"));
			for (int type = NODE_DIRECTORY; type < NUM_NODE_TYPES; type++)
				window_add_legend_item(_(node_type_names[type]),
					&config.by_nodetype.colors[type], NULL, SPECTRUM_NONE);
			break;

		case COLOR_BY_TIMESTAMP: {
			const char *timestamp_name;
			gchar *old_text, *new_text, *range_text;
			switch (config.by_timestamp.timestamp_type) {
				case TIMESTAMP_ACCESS: timestamp_name = _("Last accessed"); break;
				case TIMESTAMP_MODIFY: timestamp_name = _("Last modified"); break;
				case TIMESTAMP_ATTRIB: timestamp_name = _("Attributes changed"); break;
				default: timestamp_name = _("Timestamp"); break;
			}
			old_text = window_format_timestamp(config.by_timestamp.old_time);
			new_text = window_format_timestamp(config.by_timestamp.new_time);
			range_text = g_strdup_printf(_("%s: %s to %s"),
				timestamp_name, old_text, new_text);
			window_add_legend_item(range_text,
				&config.by_timestamp.old_color, &config.by_timestamp.new_color,
				config.by_timestamp.spectrum_type);
			g_free(range_text);
			g_free(old_text);
			g_free(new_text);
			window_add_legend_section(_("Directories"));
			window_add_legend_item(_(node_type_names[NODE_DIRECTORY]),
				&config.by_nodetype.colors[NODE_DIRECTORY], NULL, SPECTRUM_NONE);
			break;
		}

		case COLOR_BY_WPATTERN: {
			GList *group_link;
			int group_number = 1;
			window_add_legend_section(_("Wildcard matches"));
			for (group_link = config.by_wpattern.wpgroup_list;
			     group_link != NULL; group_link = group_link->next, group_number++) {
				struct WPatternGroup *group = group_link->data;
				GString *patterns = g_string_new("");
				GList *pattern_link;
				for (pattern_link = group->wp_list; pattern_link != NULL;
				     pattern_link = pattern_link->next) {
					if (patterns->len > 0)
						g_string_append(patterns, ", ");
					g_string_append(patterns, (const char *)pattern_link->data);
				}
				gchar *label = patterns->len > 0 ?
					g_strdup_printf("%s", patterns->str) :
					g_strdup_printf(_("Group %d (no patterns)"), group_number);
				window_add_legend_item(label, &group->color, NULL, SPECTRUM_NONE);
				g_free(label);
				g_string_free(patterns, TRUE);
			}
			window_add_legend_item(_("No wildcard match"),
				&config.by_wpattern.default_color, NULL, SPECTRUM_NONE);
			window_add_legend_section(_("Directories"));
			window_add_legend_item(_(node_type_names[NODE_DIRECTORY]),
				&config.by_nodetype.colors[NODE_DIRECTORY], NULL, SPECTRUM_NONE);
			break;
		}

		case COLOR_BY_FILETYPE:
			window_add_legend_section(_("Files by category"));
			for (int category = 0; category < COLOR_FILETYPE_CATEGORY_COUNT; category++) {
				RGBcolor color = color_filetype_category_color(category);
				window_add_legend_item(_(color_filetype_category_name(category)),
					&color, NULL, SPECTRUM_NONE);
			}
			window_add_legend_section(_("Directories use muted dominant-category colors"));
			break;

		case COLOR_NONE:
			break;
	}
	color_config_destroy(&config);
}

static void
window_add_viewport_overlays(GtkWidget *overlay_w)
{
	GtkWidget *panel_w;
	GtkWidget *row_w;
	GError *logo_error = NULL;
	GdkPixbuf *logo_pixbuf;

	/* This opaque cover sits behind all viewport controls and covers only the
	 * GL viewport during the synchronized first camera move. */
	scene_fade_w = gtk_event_box_new();
	gtk_widget_set_no_show_all(scene_fade_w, TRUE);
	gtk_widget_set_hexpand(scene_fade_w, TRUE);
	gtk_widget_set_vexpand(scene_fade_w, TRUE);
	window_add_style_class(scene_fade_w, "fsv-scene-fade");
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay_w), scene_fade_w);
	gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(overlay_w), scene_fade_w, TRUE);
	gtk_widget_hide(scene_fade_w);

	panel_w = gtk_event_box_new();
	breadcrumb_panel_w = panel_w;
	window_add_style_class(panel_w, "fsv-panel");
	window_add_style_class(panel_w, "fsv-breadcrumb");
	breadcrumb_label_w = gtk_label_new("");
	gtk_label_set_ellipsize(GTK_LABEL(breadcrumb_label_w), PANGO_ELLIPSIZE_MIDDLE);
	gtk_container_add(GTK_CONTAINER(panel_w), breadcrumb_label_w);
	gtk_widget_set_halign(panel_w, GTK_ALIGN_START);
	gtk_widget_set_valign(panel_w, GTK_ALIGN_START);
	gtk_widget_set_margin_start(panel_w, 14);
	gtk_widget_set_margin_top(panel_w, 14);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay_w), panel_w);
	gtk_widget_show_all(panel_w);

	context_toolbar_w = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	window_add_style_class(context_toolbar_w, "fsv-panel");
	window_add_style_class(context_toolbar_w, "fsv-context-toolbar");
	context_node_label_w = gtk_label_new("");
	gtk_style_context_add_class(gtk_widget_get_style_context(context_node_label_w), "fsv-title");
	gtk_box_pack_start(GTK_BOX(context_toolbar_w), context_node_label_w, FALSE, FALSE, 2);
	context_expand_button_w = gtk_button_new_with_label(_("Expand"));
	g_signal_connect(context_expand_button_w, "clicked",
		G_CALLBACK(window_context_expand_clicked), NULL);
	gtk_box_pack_start(GTK_BOX(context_toolbar_w), context_expand_button_w, FALSE, FALSE, 0);
	context_expand_all_button_w = gtk_button_new_with_label(_("Expand all"));
	g_signal_connect(context_expand_all_button_w, "clicked",
		G_CALLBACK(window_context_expand_all_clicked), NULL);
	gtk_box_pack_start(GTK_BOX(context_toolbar_w), context_expand_all_button_w, FALSE, FALSE, 0);
	context_look_button_w = gtk_button_new_with_label(_("Look at"));
	g_signal_connect(context_look_button_w, "clicked",
		G_CALLBACK(window_context_look_clicked), NULL);
	gtk_box_pack_start(GTK_BOX(context_toolbar_w), context_look_button_w, FALSE, FALSE, 0);
	context_properties_button_w = gtk_button_new_with_label(_("Properties"));
	g_signal_connect(context_properties_button_w, "clicked",
		G_CALLBACK(window_context_properties_clicked), NULL);
	gtk_box_pack_start(GTK_BOX(context_toolbar_w), context_properties_button_w, FALSE, FALSE, 0);
	gtk_widget_set_halign(context_toolbar_w, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(context_toolbar_w, GTK_ALIGN_END);
	gtk_widget_set_margin_bottom(context_toolbar_w, 14);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay_w), context_toolbar_w);
	gtk_widget_show_all(context_toolbar_w);
	gtk_widget_hide(context_toolbar_w);
	/* Set this only after showing its children: gtk_widget_show_all() skips
	 * the entire subtree when no-show-all is already set on the parent. */
	gtk_widget_set_no_show_all(context_toolbar_w, TRUE);

	legend_w = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	window_add_style_class(legend_w, "fsv-panel");
	window_add_style_class(legend_w, "fsv-legend");
	row_w = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *legend_title_w = gtk_label_new(_("LEGEND"));
	gtk_label_set_xalign(GTK_LABEL(legend_title_w), 0.0);
	gtk_widget_set_hexpand(legend_title_w, TRUE);
	gtk_style_context_add_class(gtk_widget_get_style_context(legend_title_w), "fsv-meta");
	gtk_box_pack_start(GTK_BOX(row_w), legend_title_w, TRUE, TRUE, 0);
	GtkWidget *legend_close_w = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU);
	gtk_button_set_relief(GTK_BUTTON(legend_close_w), GTK_RELIEF_NONE);
	g_signal_connect(legend_close_w, "clicked", G_CALLBACK(window_legend_close_clicked), NULL);
	gtk_box_pack_end(GTK_BOX(row_w), legend_close_w, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(legend_w), row_w, FALSE, FALSE, 0);
	legend_items_w = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	gtk_box_pack_start(GTK_BOX(legend_w), legend_items_w, FALSE, FALSE, 0);
	gtk_widget_set_halign(legend_w, GTK_ALIGN_END);
	gtk_widget_set_valign(legend_w, GTK_ALIGN_END);
	gtk_widget_set_margin_end(legend_w, 14);
	gtk_widget_set_margin_bottom(legend_w, 14);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay_w), legend_w);
	gtk_widget_show_all(legend_w);

	properties_panel_w = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	window_add_style_class(properties_panel_w, "fsv-panel");
	window_add_style_class(properties_panel_w, "fsv-properties");
	gtk_widget_set_size_request(properties_panel_w, 272, -1);
	gtk_widget_set_halign(properties_panel_w, GTK_ALIGN_END);
	gtk_widget_set_valign(properties_panel_w, GTK_ALIGN_START);
	gtk_widget_set_margin_top(properties_panel_w, 14);
	gtk_widget_set_margin_end(properties_panel_w, 14);
	row_w = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	properties_name_w = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(properties_name_w), 0.0);
	gtk_widget_set_hexpand(properties_name_w, TRUE);
	gtk_style_context_add_class(gtk_widget_get_style_context(properties_name_w), "fsv-title");
	gtk_box_pack_start(GTK_BOX(row_w), properties_name_w, TRUE, TRUE, 0);
	GtkWidget *close_button = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU);
	gtk_button_set_relief(GTK_BUTTON(close_button), GTK_RELIEF_NONE);
	g_signal_connect(close_button, "clicked", G_CALLBACK(window_properties_close_clicked), NULL);
	gtk_box_pack_end(GTK_BOX(row_w), close_button, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(properties_panel_w), row_w, FALSE, FALSE, 0);
	properties_type_w = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(properties_type_w), 0.0);
	gtk_style_context_add_class(gtk_widget_get_style_context(properties_type_w), "fsv-meta");
	gtk_box_pack_start(GTK_BOX(properties_panel_w), properties_type_w, FALSE, FALSE, 0);
	properties_size_w = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(properties_size_w), 0.0);
	gtk_style_context_add_class(gtk_widget_get_style_context(properties_size_w), "fsv-property-size");
	gtk_box_pack_start(GTK_BOX(properties_panel_w), properties_size_w, FALSE, FALSE, 0);
	properties_ratio_w = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(properties_ratio_w), 0.0);
	gtk_box_pack_start(GTK_BOX(properties_panel_w), properties_ratio_w, FALSE, FALSE, 0);
	GtkWidget *details_grid = gtk_grid_new();
	gtk_grid_set_column_spacing(GTK_GRID(details_grid), 12);
	gtk_grid_set_row_spacing(GTK_GRID(details_grid), 5);
	window_properties_add_row(details_grid, _("Path"), &properties_path_w, 0);
	window_properties_add_row(details_grid, _("Files"), &properties_files_w, 1);
	window_properties_add_row(details_grid, _("Folders"), &properties_folders_w, 2);
	window_properties_add_row(details_grid, _("Modified"), &properties_modified_w, 3);
	window_properties_add_row(details_grid, _("Owner"), &properties_owner_w, 4);
	window_properties_add_row(details_grid, _("Mode"), &properties_mode_w, 5);
	gtk_box_pack_start(GTK_BOX(properties_panel_w), details_grid, FALSE, FALSE, 0);
	GtkWidget *type_title = gtk_label_new(_("BY TYPE"));
	gtk_label_set_xalign(GTK_LABEL(type_title), 0.0);
	gtk_style_context_add_class(gtk_widget_get_style_context(type_title), "fsv-meta");
	gtk_box_pack_start(GTK_BOX(properties_panel_w), type_title, FALSE, FALSE, 0);
	properties_type_bar_w = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
	gtk_box_pack_start(GTK_BOX(properties_panel_w), properties_type_bar_w, FALSE, FALSE, 0);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay_w), properties_panel_w);
	gtk_widget_show_all(properties_panel_w);
	gtk_widget_hide(properties_panel_w);
	gtk_widget_set_no_show_all(properties_panel_w, TRUE);

	logo_pixbuf = gdk_pixbuf_new_from_resource_at_scale(
		"/jabl/fsv/fsv-logo.svg", 400, 367, TRUE, &logo_error);
	if (logo_pixbuf != NULL) {
		scan_logo_w = gtk_image_new_from_pixbuf(logo_pixbuf);
		g_object_unref(logo_pixbuf);
	}
	else {
		g_warning("Could not load startup logo: %s", logo_error ? logo_error->message : "unknown error");
		scan_logo_w = gtk_label_new("FSV");
	}
	if (logo_error != NULL)
		g_error_free(logo_error);
	window_add_style_class(scan_logo_w, "fsv-scan-logo");
	gtk_widget_set_halign(scan_logo_w, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(scan_logo_w, GTK_ALIGN_CENTER);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay_w), scan_logo_w);
	gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(overlay_w), scan_logo_w, TRUE);
	gtk_widget_show(scan_logo_w);
}

static void
window_refresh_context_toolbar(void)
{
	boolean is_dir;
	boolean expanded;
	char *display_name;
	if (context_toolbar_w == NULL)
		return;
	if (selected_node == NULL) {
		gtk_widget_hide(context_toolbar_w);
		return;
	}
	/* These actions apply to both files and directories. Explicitly restore
	 * them whenever the toolbar is reused after a previous directory choice. */
	gtk_widget_show(context_node_label_w);
	gtk_widget_show(context_look_button_w);
	gtk_widget_show(context_properties_button_w);
	display_name = g_filename_display_name(NODE_DESC(selected_node)->name);
	gtk_label_set_text(GTK_LABEL(context_node_label_w), display_name);
	g_free(display_name);
	is_dir = NODE_IS_DIR(selected_node);
	if (is_dir) {
		expanded = dirtree_entry_expanded(selected_node) ||
			(!dirtree_entry_has_subdir(selected_node) &&
			 DIR_NODE_DESC(selected_node)->deployment > (1.0 - EPSILON));
		gtk_button_set_label(GTK_BUTTON(context_expand_button_w),
			expanded ? _("Collapse") : _("Expand"));
		gtk_widget_show(context_expand_button_w);
		if (DIR_NODE_DESC(selected_node)->subtree.counts[NODE_DIRECTORY] > 0)
			gtk_widget_show(context_expand_all_button_w);
		else
			gtk_widget_hide(context_expand_all_button_w);
	}
	else {
		gtk_widget_hide(context_expand_button_w);
		gtk_widget_hide(context_expand_all_button_w);
	}
	if (motion_overlays_hidden)
		gtk_widget_hide(context_toolbar_w);
	else
		gtk_widget_show(context_toolbar_w);
}

static void
window_update_breadcrumb(GNode *node)
{
	GList *parts = NULL, *link;
	GNode *part;
	GString *text;
	if (breadcrumb_label_w == NULL)
		return;
	if (node == NULL && globals.fstree != NULL)
		node = root_dnode;
	for (part = node; part != NULL && !NODE_IS_METANODE(part); part = part->parent)
		parts = g_list_prepend(parts, part);
	text = g_string_new("");
	for (link = parts; link != NULL; link = link->next) {
		GNode *breadcrumb_node = (GNode *)link->data;
		const char *name = NODE_DESC(breadcrumb_node)->name;
		char *display_name;
		if (text->len > 0)
			g_string_append(text, " › ");
		display_name = g_filename_display_name(name[0] ? name : _("/. (root)"));
		g_string_append(text, display_name);
		g_free(display_name);
	}
	gtk_label_set_text(GTK_LABEL(breadcrumb_label_w), text->str);
	g_string_free(text, TRUE);
	g_list_free(parts);
}

static void
window_css_parsing_error(GtkCssProvider *provider, GtkCssSection *section,
			 GError *error, gpointer user_data)
{
	(void)provider;
	(void)section;
	(void)user_data;
	g_warning("Could not parse fsv-modern.css: %s", error->message);
}

static void
window_setup_style(void)
{
	static gboolean initialized = FALSE;
	GtkSettings *settings;
	GtkCssProvider *provider;
	GdkScreen *screen;

	if (initialized)
		return;
	initialized = TRUE;

	settings = gtk_settings_get_default();
	if (settings != NULL)
		g_object_set(settings, "gtk-application-prefer-dark-theme", TRUE, NULL);

	screen = gdk_screen_get_default();
	if (screen == NULL)
		return;

	provider = gtk_css_provider_new();
	g_signal_connect(provider, "parsing-error",
			 G_CALLBACK(window_css_parsing_error), NULL);
	gtk_css_provider_load_from_resource(provider, "/jabl/fsv/fsv-modern.css");
	gtk_style_context_add_provider_for_screen(screen,
		GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(provider);
}

static void
window_add_style_class(GtkWidget *widget, const char *class_name)
{
	gtk_style_context_add_class(gtk_widget_get_style_context(widget), class_name);
}

/* Constructs the main program window. The specified mode will be the one
 * initially selected in the Vis menu */
void
window_init(GtkApplication *app, gpointer user_data)
{
	GtkWidget *main_window_w;
	GtkWidget *main_vbox_w;
	GtkWidget *header_w;
	GtkWidget *hamburger_menu_w;
	GtkWidget *colors_menu_w;
	GtkWidget *settings_menu_w;
	GtkWidget *menu_w;
	GtkWidget *menu_item_w;
	GtkWidget *hpaned_w;
	GtkWidget *vpaned_w;
	GtkWidget *left_vbox_w;
	GtkWidget *right_vbox_w;
	GtkWidget *hbox_w;
	GtkWidget *button_w;
	GtkWidget *directory_section_w;
	GtkWidget *files_section_w;
	GtkWidget *section_label_w;
	GtkWidget *dir_tree_w;
	GtkWidget *file_list_w;
	GtkWidget *gl_area_w;
	GtkWidget *viewport_overlay_w;
	GtkWidget *x_scrollbar_w;
	GtkWidget *y_scrollbar_w;
	GtkWidget *statusbar_w;
	int window_width, window_height;

	Fsv_init_data* fid = (Fsv_init_data*)user_data;
	FsvMode fsv_mode = fid->mode;
	window_setup_style();

	/* Main window widget */
	main_window_w = gtk_application_window_new(app);
	window_add_style_class(main_window_w, "fsv-window");
	gtk_window_set_title( GTK_WINDOW(main_window_w), "fsv" );
	gtk_window_set_resizable(GTK_WINDOW(main_window_w), TRUE);
	window_width = 1920 / 2;
	window_height = 2584 * window_width / 4181;
	gtk_widget_set_size_request(main_window_w, window_width, window_height);

	/* Main vertical box widget */
	main_vbox_w = gui_vbox_add( main_window_w, 0 );

	/* Compact application header and hamburger menu. */
	header_w = gtk_header_bar_new();
	gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header_w), TRUE);
	gtk_header_bar_set_decoration_layout(GTK_HEADER_BAR(header_w),
		":minimize,maximize,close");
	window_add_style_class(header_w, "fsv-header");
	gtk_window_set_titlebar(GTK_WINDOW(main_window_w), header_w);
	gtk_widget_show(header_w);

	hamburger_menu_w = gtk_menu_new();
	menu_w = gui_menu_add(hamburger_menu_w, _("File"));
	/* File menu items */
	menu_item_w = gui_menu_item_add( menu_w, _("Change root..."), on_file_change_root_activate, NULL );
	gui_keybind( menu_item_w, _("^N") );
	G_LIST_APPEND(sw_widget_list, menu_item_w);
#if 0
	gui_menu_item_add( menu_w, _("Save settings"), on_file_save_settings_activate, NULL );
#endif
	gui_separator_add( menu_w );
	menu_item_w = gui_menu_item_add( menu_w, _("Exit"), on_file_exit_activate, NULL );
	gui_keybind( menu_item_w, _("^Q") );

	menu_w = gui_menu_add(hamburger_menu_w, _("Navigation"));
	gui_menu_item_add(menu_w, _("Previous view"), window_menu_back, NULL);
	gui_menu_item_add(menu_w, _("Look at filesystem root"), window_menu_look_at_root, NULL);

	/* Vis menu */
	menu_w = gui_menu_add( hamburger_menu_w, _("Vis") );
	/* Vis menu items */
	gui_radio_menu_begin( fsv_mode );
	gui_radio_menu_item_add( menu_w, _("MapV"), on_vis_mapv_activate, NULL );
	/* Note: TreeV is currently very slow/unresponsive on large
	 * directories (tens of thousands of entries) -- 'Expand All' on
	 * one such directory can make the app freeze and require a force-
	 * quit. Not yet root-caused/fixed. Kept enabled here for now to
	 * make testing/fixing that easier. */
	gui_radio_menu_item_add( menu_w, _("TreeV"), on_vis_treev_activate, NULL );

	/* Color menu */
	colors_menu_w = gtk_menu_new();
	menu_w = colors_menu_w;
	/* Color menu items */
	gui_radio_menu_begin( 0 );
	menu_item_w = gui_radio_menu_item_add( menu_w, _("By node type"), on_color_by_nodetype_activate, NULL );
	G_LIST_APPEND(sw_widget_list, menu_item_w);
	color_by_nodetype_rmenu_item_w = menu_item_w;
	menu_item_w = gui_radio_menu_item_add( menu_w, _("By timestamp"), on_color_by_timestamp_activate, NULL );
	G_LIST_APPEND(sw_widget_list, menu_item_w);
	color_by_timestamp_rmenu_item_w = menu_item_w;
	menu_item_w = gui_radio_menu_item_add( menu_w, _("By wildcards"), on_color_by_wildcards_activate, NULL );
	G_LIST_APPEND(sw_widget_list, menu_item_w);
	color_by_wpattern_rmenu_item_w = menu_item_w;
	menu_item_w = gui_radio_menu_item_add( menu_w, _("By file type"), on_color_by_filetype_activate, NULL );
	G_LIST_APPEND(sw_widget_list, menu_item_w);
	color_by_filetype_rmenu_item_w = menu_item_w;
	gui_separator_add( menu_w );
	gui_menu_item_add( menu_w, _("Setup..."), on_color_setup_activate, NULL );

#ifdef DEBUG
	/* Debug menu */
	menu_w = gui_menu_add( hamburger_menu_w, "Debug" );
	/* Debug menu items */
	gui_menu_item_add( menu_w, "Memory totals", debug_show_mem_totals, NULL );
	gui_menu_item_add( menu_w, "Memory summary", debug_show_mem_summary, NULL );
	gui_menu_item_add( menu_w, "Memory stats", debug_show_mem_stats, NULL );
	gui_separator_add( menu_w );
#endif

	/* Help retains its documentation/about actions in the hamburger. */
	menu_w = gui_menu_add( hamburger_menu_w, _("Help") );
	/* Help menu items */
	gui_menu_item_add( menu_w, _("Contents..."), on_help_contents_activate, NULL );
	gui_separator_add( menu_w );
	gui_menu_item_add( menu_w, _("About fsv..."), on_help_about_fsv_activate, NULL );

	/* Performance controls live in Settings. */
	settings_menu_w = gtk_menu_new();
	menu_w = settings_menu_w;
	gui_check_menu_item_add( menu_w, _("Show FPS"), TRUE, on_help_show_fps_toggled, NULL );
	gui_check_menu_item_add( menu_w, _("Enable CPU/GPU render profiling"),
				 ogl_profile_enabled( ), on_help_profile_toggled, NULL );
	gui_separator_add( menu_w );
	/* Performance toggles. Initial check state is read from geometry.c so
	 * the menu agrees with the actual defaults (and with any env var used
	 * to override them at startup). */
	gui_check_menu_item_add( menu_w, _("TreeV: subtree culling"),
				 geometry_treev_cull_enabled( ),
				 on_help_treev_culling_toggled, NULL );
	gui_check_menu_item_add( menu_w, _("TreeV: label/leaf detail reduction"),
				 geometry_treev_lod_enabled( ),
				 on_help_treev_lod_toggled, NULL );
	gui_check_menu_item_add( menu_w, _("TreeV: hide labels while camera moves"),
				 geometry_treev_hide_labels_moving( ),
				 on_help_treev_hide_labels_moving_toggled, NULL );
	gui_check_menu_item_add( menu_w, _("MapV: hide labels while camera moves"),
				 geometry_mapv_hide_labels_moving( ),
				 on_help_mapv_hide_labels_moving_toggled, NULL );
	gui_check_menu_item_add( menu_w, _("MapV: reduce labels below readable size"),
				 geometry_mapv_lod_enabled( ),
				 on_help_mapv_lod_toggled, NULL );
	gui_separator_add(menu_w);
	legend_toggle_menu_item_w = gui_check_menu_item_add(menu_w, _("Show color legend"), TRUE,
					     G_CALLBACK(window_legend_visibility_toggled), NULL);
	gtk_widget_show_all(hamburger_menu_w);
	gtk_widget_show_all(colors_menu_w);
	gtk_widget_show_all(settings_menu_w);

	/* Keep the hamburger before the FSV wordmark on the left. */
	button_w = window_menu_button(NULL, hamburger_menu_w);
	gtk_header_bar_pack_start(GTK_HEADER_BAR(header_w), button_w);
	gtk_widget_show(button_w);

	/* Header controls: mode, navigation, placeholder search and menus. */
	menu_item_w = gtk_label_new("F S V");
	gtk_style_context_add_class(gtk_widget_get_style_context(menu_item_w), "title");
	gtk_header_bar_pack_start(GTK_HEADER_BAR(header_w), menu_item_w);
	gtk_widget_show(menu_item_w);

	hbox_w = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	window_add_style_class(hbox_w, "fsv-segmented");
	mapv_view_button_w = gtk_radio_button_new_with_label(NULL, "MapV");
	gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(mapv_view_button_w), FALSE);
	gtk_widget_set_name(mapv_view_button_w, "fsv-mapv-toggle");
	treev_view_button_w = gtk_radio_button_new_with_label_from_widget(
		GTK_RADIO_BUTTON(mapv_view_button_w), "TreeV");
	gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(treev_view_button_w), FALSE);
	gtk_box_pack_start(GTK_BOX(hbox_w), mapv_view_button_w, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(hbox_w), treev_view_button_w, FALSE, FALSE, 0);
	G_LIST_APPEND(sw_widget_list, mapv_view_button_w);
	G_LIST_APPEND(sw_widget_list, treev_view_button_w);
	g_signal_connect(mapv_view_button_w, "toggled", G_CALLBACK(window_header_view_toggled), GINT_TO_POINTER(FSV_MAPV));
	g_signal_connect(treev_view_button_w, "toggled", G_CALLBACK(window_header_view_toggled), GINT_TO_POINTER(FSV_TREEV));
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(fsv_mode == FSV_MAPV ? mapv_view_button_w : treev_view_button_w), TRUE);
	gtk_header_bar_pack_start(GTK_HEADER_BAR(header_w), hbox_w);
	gtk_widget_show_all(hbox_w);

	button_w = gtk_button_new();
	window_button_icon_label(button_w, "find-location-symbolic", _("Set as root"));
	g_signal_connect(button_w, "clicked", G_CALLBACK(window_change_root_clicked), NULL);
	G_LIST_APPEND(sw_widget_list, button_w);
	gtk_header_bar_pack_start(GTK_HEADER_BAR(header_w), button_w);
	gtk_widget_show(button_w);
	button_w = gtk_button_new();
	window_button_icon_label(button_w, "go-up-symbolic", _("Up"));
	g_signal_connect(button_w, "clicked", G_CALLBACK(on_cd_up_button_clicked), NULL);
	G_LIST_APPEND(sw_widget_list, button_w);
	gtk_header_bar_pack_start(GTK_HEADER_BAR(header_w), button_w);
	gtk_widget_show(button_w);
	button_w = gtk_toggle_button_new();
	window_button_icon_label(button_w, "view-reveal-symbolic", _("Overview"));
	g_signal_connect(button_w, "toggled",
		G_CALLBACK(on_birdseye_view_togglebutton_toggled), NULL);
	overview_tbutton_w = button_w;
	G_LIST_APPEND(sw_widget_list, button_w);
	gtk_header_bar_pack_start(GTK_HEADER_BAR(header_w), button_w);
	gtk_widget_show(button_w);

	search_entry_w = gtk_search_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(search_entry_w), _("Search files and folders…"));
	gtk_widget_set_tooltip_text(search_entry_w,
		_("Enter a name to find it; press Enter again to cycle through matches."));
	gtk_widget_set_size_request(search_entry_w, 260, -1);
	window_add_style_class(search_entry_w, "fsv-search");
	g_signal_connect(search_entry_w, "activate", G_CALLBACK(window_search_activate), NULL);
	gtk_header_bar_set_custom_title(GTK_HEADER_BAR(header_w), search_entry_w);
	gtk_widget_show(search_entry_w);

	/* GtkHeaderBar packs end children in visual reverse insertion order. */
	button_w = window_menu_button(_("Settings"), settings_menu_w);
	gtk_header_bar_pack_end(GTK_HEADER_BAR(header_w), button_w);
	gtk_widget_show(button_w);
	color_mode_button_w = gtk_menu_button_new();
	color_mode_label_w = gtk_label_new(_("Color: By node type"));
	gtk_container_add(GTK_CONTAINER(color_mode_button_w), color_mode_label_w);
	gtk_menu_button_set_popup(GTK_MENU_BUTTON(color_mode_button_w), colors_menu_w);
	gtk_widget_show(color_mode_label_w);
	gtk_header_bar_pack_end(GTK_HEADER_BAR(header_w), color_mode_button_w);
	gtk_widget_show(color_mode_button_w);

	/* Main horizontal paned widget */
	hpaned_w = gui_hpaned_add(main_vbox_w, MAX(280, window_width / 5));

	/* Vertical box for everything in the left pane */
	left_vbox_w = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	window_add_style_class(left_vbox_w, "fsv-sidebar");
	gtk_paned_add1( GTK_PANED(hpaned_w), left_vbox_w );
	gtk_widget_show( left_vbox_w );

	/* Vertical paned widget for directory tree / file list */
	vpaned_w = gui_vpaned_add(left_vbox_w, window_height / 2);

	/* Directory section: heading plus the existing tree view. */
	directory_section_w = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	section_label_w = gtk_label_new(_("DIRECTORIES"));
	gtk_label_set_xalign(GTK_LABEL(section_label_w), 0.0);
	window_add_style_class(section_label_w, "fsv-section-title");
	gtk_box_pack_start(GTK_BOX(directory_section_w), section_label_w, FALSE, TRUE, 0);
	dir_tree_w = gui_tree_add(directory_section_w);
	gtk_paned_add1(GTK_PANED(vpaned_w), directory_section_w);
	gtk_widget_show_all(directory_section_w);

	/* Selected directory's file section: heading plus the existing list. */
	files_section_w = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	files_section_label_w = gtk_label_new(_("FILES"));
	gtk_label_set_xalign(GTK_LABEL(files_section_label_w), 0.0);
	window_add_style_class(files_section_label_w, "fsv-section-title");
	gtk_box_pack_start(GTK_BOX(files_section_w), files_section_label_w, FALSE, TRUE, 0);
	file_list_w = gui_filelist_new(files_section_w);
	gtk_paned_add2(GTK_PANED(vpaned_w), files_section_w);
	gtk_widget_show_all(files_section_w);

	/* Vertical box for everything in the right pane */
	right_vbox_w = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_paned_add2( GTK_PANED(hpaned_w), right_vbox_w );
	gtk_widget_show( right_vbox_w );

	/* Horizontal box for viewport and y-scrollbar */
	hbox_w = gui_hbox_add( right_vbox_w, 0 );
	gui_widget_packing( hbox_w, EXPAND, FILL, AT_START );

	/* Main viewport (OpenGL area widget) */
	gl_area_w = gui_gl_area_add( hbox_w );
	viewport_overlay_w = gtk_widget_get_parent(gl_area_w);
	window_add_style_class(viewport_overlay_w, "fsv-viewport");
	window_add_viewport_overlays(viewport_overlay_w);
	g_signal_connect(G_OBJECT(gl_area_w), "event", G_CALLBACK(viewport_cb), NULL);

	/* y-scrollbar */
	y_scrollbar_w = gui_vscrollbar_add( hbox_w, NULL );
	G_LIST_APPEND(sw_widget_list, y_scrollbar_w);
	/* x-scrollbar */
	x_scrollbar_w = gui_hscrollbar_add( right_vbox_w, NULL );
	G_LIST_APPEND(sw_widget_list, x_scrollbar_w);

	/* One bottom bar: directory summary, hovered path, and FPS. */
	statusbar_w = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	window_add_style_class(statusbar_w, "fsv-status");
	summary_label_w = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(summary_label_w), 0.0);
	gtk_label_set_ellipsize(GTK_LABEL(summary_label_w), PANGO_ELLIPSIZE_END);
	gtk_widget_set_size_request(summary_label_w, 300, -1);
	gtk_style_context_add_class(gtk_widget_get_style_context(summary_label_w), "fsv-status-summary");
	gtk_box_pack_start(GTK_BOX(statusbar_w), summary_label_w, FALSE, TRUE, 10);
	GtkWidget *status_separator = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
	gtk_box_pack_start(GTK_BOX(statusbar_w), status_separator, FALSE, FALSE, 0);
	path_label_w = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(path_label_w), 0.0);
	gtk_label_set_ellipsize(GTK_LABEL(path_label_w), PANGO_ELLIPSIZE_MIDDLE);
	gtk_widget_set_hexpand(path_label_w, TRUE);
	gtk_style_context_add_class(gtk_widget_get_style_context(path_label_w), "fsv-status-path");
	gtk_box_pack_start(GTK_BOX(statusbar_w), path_label_w, TRUE, TRUE, 10);
	fps_label_w = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(fps_label_w), 1.0);
	gtk_style_context_add_class(gtk_widget_get_style_context(fps_label_w), "fsv-status-fps");
	gtk_box_pack_end(GTK_BOX(statusbar_w), fps_label_w, FALSE, FALSE, 12);
	gtk_box_pack_end(GTK_BOX(main_vbox_w), statusbar_w, FALSE, FALSE, 0);
	gtk_widget_show_all(statusbar_w);
	ogl_pass_fps_label(fps_label_w);
	ogl_set_fps_display(TRUE);

	/* Bind program icon to main window */
	gui_window_icon_xpm( main_window_w, fsv_icon_xpm );

	/* Attach keybindings */
	gui_keybind( main_window_w, NULL );

	/* Send out the widgets to their respective modules */
	dialog_pass_main_window_widget( main_window_w );
	dirtree_pass_widget( dir_tree_w );
	filelist_pass_widget( file_list_w );
	camera_pass_scrollbar_widgets( x_scrollbar_w, y_scrollbar_w );

	/* Showtime! */
	gtk_widget_show( main_window_w );

	color_init();
	fsv_load(fid->root_dir);
	xfree(fid->root_dir);
}


/* This enables/disables the switchable widgets */
void
window_set_access( boolean enabled )
{
	GtkWidget *widget;
	GList *llink;

	llink = sw_widget_list;
	while (llink != NULL) {
		widget = (GtkWidget *)llink->data;

		gtk_widget_set_sensitive( widget, enabled );

		llink = llink->next;
	}
}


/* Resets the Color radio menu to the given mode */
void
window_set_color_mode( ColorMode mode )
{
	GtkWidget *rmenu_item_w;
	GCallback handler;

	switch (mode) {
		case COLOR_BY_NODETYPE:
		rmenu_item_w = color_by_nodetype_rmenu_item_w;
		handler = G_CALLBACK(on_color_by_nodetype_activate);
		break;

		case COLOR_BY_TIMESTAMP:
		rmenu_item_w = color_by_timestamp_rmenu_item_w;
		handler = G_CALLBACK(on_color_by_timestamp_activate);
		break;

		case COLOR_BY_WPATTERN:
		rmenu_item_w = color_by_wpattern_rmenu_item_w;
		handler = G_CALLBACK(on_color_by_wildcards_activate);
		break;

		case COLOR_BY_FILETYPE:
		rmenu_item_w = color_by_filetype_rmenu_item_w;
		handler = G_CALLBACK(on_color_by_filetype_activate);
		break;

		SWITCH_FAIL
	}

	g_signal_handlers_block_by_func(G_OBJECT(rmenu_item_w), handler, NULL );
	gtk_check_menu_item_set_active( GTK_CHECK_MENU_ITEM(rmenu_item_w), TRUE );
	g_signal_handlers_unblock_by_func(G_OBJECT(rmenu_item_w), handler, NULL );
	if (color_mode_label_w != NULL) {
		const char *label = mode == COLOR_BY_NODETYPE ? _("Color: By node type") :
			(mode == COLOR_BY_TIMESTAMP ? _("Color: By timestamp") :
			(mode == COLOR_BY_FILETYPE ? _("Color: By file type") : _("Color: By wildcards")));
		gtk_label_set_text(GTK_LABEL(color_mode_label_w), label);
	}
	window_update_color_legend();
}


/* Keep the header mode switch synchronized with the legacy Vis menu. */
void
window_set_view_mode(FsvMode mode)
{
	GtkWidget *active_w = mode == FSV_MAPV ? mapv_view_button_w : treev_view_button_w;
	if (active_w == NULL)
		return;
	g_signal_handlers_block_by_func(G_OBJECT(mapv_view_button_w), G_CALLBACK(window_header_view_toggled), GINT_TO_POINTER(FSV_MAPV));
	g_signal_handlers_block_by_func(G_OBJECT(treev_view_button_w), G_CALLBACK(window_header_view_toggled), GINT_TO_POINTER(FSV_TREEV));
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(active_w), TRUE);
	g_signal_handlers_unblock_by_func(G_OBJECT(mapv_view_button_w), G_CALLBACK(window_header_view_toggled), GINT_TO_POINTER(FSV_MAPV));
	g_signal_handlers_unblock_by_func(G_OBJECT(treev_view_button_w), G_CALLBACK(window_header_view_toggled), GINT_TO_POINTER(FSV_TREEV));
}


/* Pops out the bird's-eye view toggle button
 * Note: This should only be called from camera.c, as the bird's-eye-view
 * mode flag (local to that module) must be updated in tandem */
void
window_birdseye_view_off( void )
{
	if (overview_tbutton_w == NULL)
		return;
	g_signal_handlers_block_by_func(G_OBJECT(overview_tbutton_w), G_CALLBACK(on_birdseye_view_togglebutton_toggled), NULL);
	gtk_toggle_button_set_active( GTK_TOGGLE_BUTTON(overview_tbutton_w), FALSE );
	g_signal_handlers_unblock_by_func(G_OBJECT(overview_tbutton_w), G_CALLBACK(on_birdseye_view_togglebutton_toggled), NULL);
}


/* Displays a message in one of the statusbars */
void
window_statusbar( StatusBarID sb_id, const char *message )
{
	char *valid_message = g_utf8_make_valid(message != NULL ? message : "", -1);
	switch (sb_id) {
		case SB_LEFT:
		gtk_label_set_text(GTK_LABEL(summary_label_w), valid_message);
		break;

		case SB_RIGHT:
		gtk_label_set_text(GTK_LABEL(path_label_w), valid_message);
		break;

		SWITCH_FAIL
	}
	g_free(valid_message);
}


/* Build the selected-directory summary from already computed scan totals. */
void
window_set_directory_summary(GNode *dnode)
{
	char *size_text;
	char *summary;
	char *name;
	unsigned int dirs, files;
	unsigned int type;
	int64 size;
	if (summary_label_w == NULL || dnode == NULL || !NODE_IS_DIR(dnode))
		return;
	size = DIR_NODE_DESC(dnode)->subtree.size;
	name = g_filename_display_name(NODE_DESC(dnode)->name[0] ?
		NODE_DESC(dnode)->name : _("/. (root)"));
	dirs = DIR_NODE_DESC(dnode)->subtree.counts[NODE_DIRECTORY];
	files = 0;
	for (type = NODE_REGFILE; type < NUM_NODE_TYPES; type++)
		files += DIR_NODE_DESC(dnode)->subtree.counts[type];
	size_text = g_strdup(abbrev_size(size));
	if (dnode->parent != NULL && NODE_IS_DIR(dnode->parent) &&
	    DIR_NODE_DESC(dnode->parent)->subtree.size > 0) {
		double parent_size = (double)DIR_NODE_DESC(dnode->parent)->subtree.size;
		double percent = 100.0 * (double)size / parent_size;
		summary = g_strdup_printf(_("%s · %s · %.0f%% of parent · %u files, %u dirs"),
			name, size_text, percent, files, dirs);
	}
	else {
		summary = g_strdup_printf(_("%s · %s · %u files, %u dirs"),
			name, size_text, files, dirs);
	}
	gtk_label_set_text(GTK_LABEL(summary_label_w), summary);
	g_free(summary);
	g_free(size_text);
	g_free(name);
}

void
window_set_selected_node(GNode *node)
{
	GNode *directory;
	selected_node = node;
	viewport_set_selected_node(node);
	geometry_set_selected_node(node);
	window_update_breadcrumb(node);
	window_refresh_context_toolbar();
	if (properties_panel_w != NULL && gtk_widget_get_visible(properties_panel_w))
		window_update_properties_panel();
	if (node == NULL)
		return;
	geometry_highlight_node(node, FALSE);
	window_statusbar(SB_RIGHT, node_absname(node));
	directory = NODE_IS_DIR(node) ? node : node->parent;
	if (directory != NULL && NODE_IS_DIR(directory))
		dirtree_entry_select(directory);
	if (NODE_IS_DIR(node))
		filelist_show_directory(node);
	window_set_directory_summary(directory);
	if (directory != NULL && NODE_IS_DIR(directory))
		window_set_files_section(NODE_DESC(directory)->name);
}

void
window_refresh_selected_node(void)
{
	window_refresh_context_toolbar();
}


/* Updates the title above the file list to match its directory. */
void
window_set_files_section(const char *directory_name)
{
	gchar *upper_name;
	gchar *title;
	gchar *display_name;
	if (files_section_label_w == NULL)
		return;
	display_name = g_filename_display_name(directory_name ? directory_name : "");
	upper_name = g_utf8_strup(display_name, -1);
	g_free(display_name);
	if (upper_name[0] == '\0') {
		g_free(upper_name);
		upper_name = g_utf8_strup(_("/. (root)"), -1);
	}
	title = g_strdup_printf(_("FILES IN %s"), upper_name);
	gtk_label_set_text(GTK_LABEL(files_section_label_w), title);
	g_free(title);
	g_free(upper_name);
}


/* end window.c */
