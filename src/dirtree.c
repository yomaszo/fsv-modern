/* dirtree.c */

/* Directory tree control */

/* fsv - 3D File System Visualizer
 * Copyright (C)1999 Daniel Richard G. <skunk@mit.edu>
 * Copyright (C) 2021 Janne Blomqvist <blomqvist.janne@gmail.com>
 *
 * SPDX-License-Identifier:  LGPL-2.1-or-later
 */


#include "common.h"
#include "dirtree.h"

#include <gtk/gtk.h>

#include "about.h"
#include "camera.h"
#include "colexp.h"
#include "dialog.h"
#include "filelist.h"
#include "geometry.h"
#include "gui.h"
#include "window.h"

/* Mini collapsed/expanded directory icon XPM's */
#define mini_folder_xpm mini_folder_closed_xpm
#include "xmaps/mini-folder.xpm"
#include "xmaps/mini-folder-open.xpm"


/* Time for the directory tree to scroll to a given entry (in seconds) */
#define DIRTREE_SCROLL_TIME 0.5


/* The directory tree widget */
static GtkWidget *dir_tree_w;

/* Mini collapsed/expanded directory icons */
static Icon dir_colexp_mini_icons[2];

/* Current directory */
static GNode *dirtree_current_dnode;


/* Returns TRUE if the given directory has at least one subdirectory
 * (i.e. something that could ever appear as an expandable child row in
 * the tree widget). A directory with none can never satisfy
 * dirtree_entry_expanded( ) -- there's nothing to expand -- so callers
 * that gate an action on "already expanded" should usually also allow
 * it when this returns FALSE, or that action becomes permanently
 * unreachable for childless directories */
boolean
dirtree_entry_has_subdir( GNode *dnode )
{
	GNode *child;

	if (!dnode)
		return FALSE;

	for (child = dnode->children; child != NULL; child = child->next)
		if (NODE_IS_DIR(child))
			return TRUE;

	return FALSE;
}


/* Callback for button press in the directory tree area */
static void
dirtree_select_cb(GtkTreeSelection *selection, gpointer data)
{
	GNode *dnode = NULL;
	GtkTreeIter iter;
	GtkTreeModel *model;

	/* If About presentation is up, end it */
	about( ABOUT_END );

	if (globals.fsv_mode == FSV_SPLASH)
		return;

	if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
		gtk_tree_model_get(model, &iter, DIRTREE_NODE_COLUMN, &dnode, -1);
		if (!dnode)
			return;
		/* Selecting a directory in the sidebar always navigates the
		 * camera to it now -- previously this only happened if the
		 * directory was already expanded (or had no subdirectories),
		 * otherwise the click just selected/highlighted it without
		 * moving the camera. Mirrors filelist_select_cb( )'s behavior
		 * for the file list, which has always done this unconditionally.
		 * camera_look_at( ) handles a collapsed target fine on its own
		 * (it expands the necessary ancestor chain itself -- see
		 * camera_look_at_full( ) -- and updates the current node, file
		 * list, and status bar once the pan completes, via
		 * post_pan_end( ) -> filelist_show_entry( ) -> dirtree_entry_show( )). */
		camera_look_at( dnode );
		geometry_highlight_node(dnode, FALSE);
		window_statusbar(SB_RIGHT, node_absname(dnode));
		g_signal_stop_emission_by_name(G_OBJECT(selection), "changed" );
	}
}


/* Callback for a right-click (secondary button) on the directory tree
 * area: selects the row under the pointer -- with dirtree_select_cb( )
 * blocked, since a right-click should only highlight and bring up the
 * context menu (mirroring the 3D view's right-click behavior in
 * viewport_cb( )), not also move the camera there the way a left click
 * now does (see dirtree_select_cb( )) -- then brings up the same
 * context-sensitive menu already used in the 3D view and file list;
 * see context_menu( ) in dialog.c */
static gboolean
dirtree_button_press_cb(GtkWidget *tree_w, GdkEventButton *ev_button, gpointer data)
{
	GtkTreeSelection *select;
	GtkTreePath *path;
	GtkTreeModel *model;
	GtkTreeIter iter;
	GNode *dnode;

	if (globals.fsv_mode == FSV_SPLASH)
		return FALSE;

	if (ev_button->button != 3)
		return FALSE;

	if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(tree_w), (gint)ev_button->x, (gint)ev_button->y, &path, NULL, NULL, NULL))
		return FALSE;

	model = gtk_tree_view_get_model(GTK_TREE_VIEW(tree_w));
	if (!gtk_tree_model_get_iter(model, &iter, path)) {
		gtk_tree_path_free(path);
		return FALSE;
	}
	gtk_tree_model_get(model, &iter, DIRTREE_NODE_COLUMN, &dnode, -1);

	select = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree_w));
	g_signal_handlers_block_by_func(G_OBJECT(select), G_CALLBACK(dirtree_select_cb), NULL);
	gtk_tree_view_set_cursor(GTK_TREE_VIEW(tree_w), path, NULL, FALSE);
	g_signal_handlers_unblock_by_func(G_OBJECT(select), G_CALLBACK(dirtree_select_cb), NULL);
	gtk_tree_path_free(path);

	if (dnode) {
		geometry_highlight_node(dnode, FALSE);
		window_statusbar(SB_RIGHT, node_absname(dnode));
		context_menu( dnode );
	}

	return TRUE;
}


/* Callback for collapse of a directory tree entry */
static void
dirtree_collapse_cb(GtkTreeView *tree, GtkTreeIter *iter, GtkTreePath *path, gpointer data)
{
	GNode *dnode;

	if (globals.fsv_mode == FSV_SPLASH)
		return;

	GtkTreeModel *model = gtk_tree_view_get_model(tree);
	//GtkTreeIter iter;

	//gtk_tree_model_get_iter(model, &iter, tnode);
	gtk_tree_model_get(model, iter, DIRTREE_NODE_COLUMN, &dnode, -1);
	DIR_NODE_DESC(dnode)->tree_row_expanded = FALSE;
	colexp( dnode, COLEXP_COLLAPSE_RECURSIVE );
}


/* Callback for expand of a directory tree entry */
static void
dirtree_expand_cb(GtkTreeView *tree, GtkTreeIter *iter, GtkTreePath *path, gpointer data)
{
	GNode *dnode;

	if (globals.fsv_mode == FSV_SPLASH)
		return;

	GtkTreeModel *model = gtk_tree_view_get_model(tree);
	//GtkTreeIter iter;
	//gtk_tree_model_get_iter(model, &iter, tnode);
	gtk_tree_model_get(model, iter, DIRTREE_NODE_COLUMN, &dnode, -1);
	DIR_NODE_DESC(dnode)->tree_row_expanded = TRUE;
	colexp( dnode, COLEXP_EXPAND );
}


/* Loads the mini collapsed/expanded directory icons (from XPM data) */
static void
dirtree_icons_init( void )
{
	static const char **dir_colexp_mini_xpms[] = {
		mini_folder_closed_xpm,
		mini_folder_open_xpm
	};
	int i;

	gtk_widget_realize( dir_tree_w );

	/* Make icons for collapsed and expanded directories */
	for (i = 0; i < 2; i++) {
		GdkPixbuf *pb = gdk_pixbuf_new_from_xpm_data(dir_colexp_mini_xpms[i]);
		dir_colexp_mini_icons[i].pixbuf = pb;
	}
}


/* Correspondence from window_init( ) */
void
dirtree_pass_widget( GtkWidget *tree_w )
{
	dir_tree_w = tree_w;
	GtkTreeSelection *select = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree_w));
	gtk_tree_selection_set_mode(select, GTK_SELECTION_SINGLE);
	/* Connect signal handlers */
	g_signal_connect(G_OBJECT(select), "changed", G_CALLBACK(dirtree_select_cb), NULL );
	g_signal_connect( G_OBJECT(dir_tree_w), "row_collapsed", G_CALLBACK(dirtree_collapse_cb), NULL );
	g_signal_connect( G_OBJECT(dir_tree_w), "row_expanded", G_CALLBACK(dirtree_expand_cb), NULL );
	g_signal_connect( G_OBJECT(dir_tree_w), "button_press_event", G_CALLBACK(dirtree_button_press_cb), NULL );

	dirtree_icons_init( );
}


/* Clears out all entries from the directory tree */
void
dirtree_clear( void )
{
	GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(dir_tree_w));
	GtkTreeStore *store = GTK_TREE_STORE(model);
	gtk_tree_store_clear(store);
	dirtree_current_dnode = NULL;
}


/* Adds a new entry to the directory tree */
void
dirtree_entry_new( GNode *dnode )
{
	GtkTreePath *parent_tnode = NULL;
	const char *name;
	boolean expanded;

	g_assert( NODE_IS_DIR(dnode) );

	parent_tnode = DIR_NODE_DESC(dnode->parent)->tnode;
	if (strlen( NODE_DESC(dnode)->name ) > 0)
		name = NODE_DESC(dnode)->name;
	else
		name = _("/. (root)");
	expanded = g_node_depth( dnode ) <= 2;

	DIR_NODE_DESC(dnode)->tnode = gui_tree_node_add( dir_tree_w, parent_tnode, dir_colexp_mini_icons, name, expanded, dnode );
	DIR_NODE_DESC(dnode)->tree_row_expanded = expanded;
}


/* Call this after the last call to dirtree_entry_new( ) */
void
dirtree_no_more_entries( void )
{
	// TODO: Needs to keep reference to model in dirtree_entry_new
	// so dissociating works.
	// GtkTreeView *view = GTK_TREE_VIEW(dir_tree_w);
	// GtkTreeModel *model = gtk_tree_view_get_model(view);
	// gtk_tree_view_set_model(view, model); /* Re-attach model to view */
	// g_object_unref(model);
}


/* This updates the directory tree to show (and select) a particular
 * directory entry, repopulating the file list with the contents of the
 * directory if not already listed */
void
dirtree_entry_show( GNode *dnode )
{
	g_assert( NODE_IS_DIR(dnode) );

	/* Repopulate file list if directory is different */
	if (dnode != dirtree_current_dnode) {
		filelist_populate( dnode );
/* TODO: try removing this update from here */
		gui_update( );
	}

	/* Scroll directory tree to proper entry */
	GtkTreeSelection *select = gtk_tree_view_get_selection(GTK_TREE_VIEW(dir_tree_w));
	gtk_tree_selection_select_path(select, DIR_NODE_DESC(dnode)->tnode);

	dirtree_current_dnode = dnode;
}


/* Returns TRUE if the entry for the given directory is expanded.
 * Reads a cache mirroring the GTK tree widget's row-expanded state
 * (see tree_row_expanded in DirNodeDesc) rather than querying GTK
 * directly -- this is called very frequently (once per directory,
 * potentially many times per TreeV arrange pass), and the GTK query
 * itself is not free. The cache is kept in sync at every point in
 * this file where a row's actual GTK expansion state can change. */
boolean
dirtree_entry_expanded( GNode *dnode )
{
	if (!dnode)
		return FALSE;

	g_assert( NODE_IS_DIR(dnode) );

	return DIR_NODE_DESC(dnode)->tree_row_expanded;
}


/* Helper function */
static void
block_colexp_handlers( void )
{
	g_signal_handlers_block_by_func( G_OBJECT(dir_tree_w), G_CALLBACK(dirtree_collapse_cb), NULL );
	g_signal_handlers_block_by_func( G_OBJECT(dir_tree_w), G_CALLBACK(dirtree_expand_cb), NULL );
}


/* Helper function */
static void
unblock_colexp_handlers( void )
{
	g_signal_handlers_unblock_by_func( G_OBJECT(dir_tree_w), G_CALLBACK(dirtree_collapse_cb), NULL );
	g_signal_handlers_unblock_by_func( G_OBJECT(dir_tree_w), G_CALLBACK(dirtree_expand_cb), NULL );
}


/* Recursively collapses the directory tree entry of the given directory */
void
dirtree_entry_collapse_recursive( GNode *dnode )
{
	if (!dnode)
		return;

	g_assert( NODE_IS_DIR(dnode) );

	block_colexp_handlers( );
	gtk_tree_view_collapse_row(GTK_TREE_VIEW(dir_tree_w), DIR_NODE_DESC(dnode)->tnode);
	unblock_colexp_handlers( );

	/* Collapsing a row in GTK does not clear its descendants' own
	 * internal expanded state (they simply become invisible) -- only
	 * this directory's own row-expanded state actually changed */
	DIR_NODE_DESC(dnode)->tree_row_expanded = FALSE;
}


/* Expands the directory tree entry of the given directory. If any of its
 * ancestor directory entries are not expanded, then they are expanded
 * as well */
void
dirtree_entry_expand( GNode *dnode )
{
	GNode *up_node;

	if (!dnode)
		return;

	g_assert( NODE_IS_DIR(dnode) );

	block_colexp_handlers( );
	gtk_tree_view_expand_to_path(GTK_TREE_VIEW(dir_tree_w), DIR_NODE_DESC(dnode)->tnode);
	unblock_colexp_handlers( );

	/* This directory and all of its ancestors are now expanded in the
	 * tree widget */
	up_node = dnode;
	while (NODE_IS_DIR(up_node)) {
		DIR_NODE_DESC(up_node)->tree_row_expanded = TRUE;
		up_node = up_node->parent;
	}
}


/* Helper function for dirtree_entry_expand_recursive( ): marks the cached
 * expanded flag TRUE for dnode and its entire directory subtree, mirroring
 * what gtk_tree_view_expand_row( ..., TRUE ) just did to the tree widget
 * in a single call */
static void
dirtree_mark_subtree_expanded( GNode *dnode )
{
	GNode *node;

	DIR_NODE_DESC(dnode)->tree_row_expanded = TRUE;

	node = dnode->children;
	while (node != NULL) {
		if (NODE_IS_DIR(node))
			dirtree_mark_subtree_expanded( node );
		node = node->next;
	}
}


/* Recursively expands the entire directory tree subtree of the given
 * directory. If any of its ancestor directory entries are not
 * expanded, then they are expanded as well (mirroring
 * dirtree_entry_expand( )'s behavior for ancestors -- otherwise a
 * stale/incorrect ancestor cache entry would never get corrected by
 * this path, only by dirtree_entry_expand( )'s explicit ancestor walk) */
void
dirtree_entry_expand_recursive( GNode *dnode )
{
	GNode *up_node;

	if (!dnode)
		return;

	g_assert( NODE_IS_DIR(dnode) );

#if DEBUG
	/* Guard against expansions inside collapsed subtrees */
	/** NOTE: This function may be upgraded to behave similarly to
	 ** dirtree_entry_expand( ) w.r.t. collapsed parent directories.
	 ** This has been avoided thus far since such a behavior would
	 ** not be used by the program. */
	if (NODE_IS_DIR(dnode->parent))
		g_assert( dirtree_entry_expanded( dnode->parent ) );
#endif

	block_colexp_handlers( );
	gtk_tree_view_expand_to_path(GTK_TREE_VIEW(dir_tree_w), DIR_NODE_DESC(dnode)->tnode);
	gtk_tree_view_expand_row(GTK_TREE_VIEW(dir_tree_w), DIR_NODE_DESC(dnode)->tnode, TRUE);
	unblock_colexp_handlers( );

	/* The above expanded dnode and its entire subtree in the tree
	 * widget in one call; mirror that in the cache with one walk,
	 * done once here rather than paying for a live GTK query on every
	 * future read of dirtree_entry_expanded( ) for these directories */
	dirtree_mark_subtree_expanded( dnode );

	/* Also mirror dirtree_entry_expand( )'s ancestor walk, so a
	 * previously wrong/stale cache entry on an ancestor gets corrected
	 * here too, not only via a plain (non-recursive) expand */
	up_node = dnode->parent;
	while (NODE_IS_DIR(up_node)) {
		DIR_NODE_DESC(up_node)->tree_row_expanded = TRUE;
		up_node = up_node->parent;
	}
}


/* end dirtree.c */
