/* window.h */

/* Main program window */

/* fsv - 3D File System Visualizer
 * Copyright (C)1999 Daniel Richard G. <skunk@mit.edu>
 *
 * SPDX-License-Identifier:  LGPL-2.1-or-later
 */


#ifdef FSV_WINDOW_H
	#error
#endif
#define FSV_WINDOW_H

#include <gtk/gtk.h>

typedef enum {
	SB_LEFT,
	SB_RIGHT
} StatusBarID;


void window_init(GtkApplication *app, gpointer user_data);
void window_set_access( boolean enabled );
void window_reset_search(void);
void window_set_view_mode( FsvMode mode );
#ifdef FSV_COLOR_H
void window_set_color_mode( ColorMode mode );
void window_update_color_legend(void);
#endif
void window_birdseye_view_off( void);
void window_statusbar( StatusBarID sb_id, const char *message );
void window_set_files_section(const char *directory_name);
void window_set_directory_summary(GNode *dnode);
void window_set_selected_node(GNode *node);
void window_refresh_selected_node(void);
void window_scan_logo_show(boolean show);
void window_prepare_scene_fade(void);
void window_start_scene_fade(void);
void window_set_motion_overlays(boolean moving);


/* end window.h */
