/* geometry.h */

/* 3D geometry generation and rendering */

/* fsv - 3D File System Visualizer
 * Copyright (C)1999 Daniel Richard G. <skunk@mit.edu>
 * SPDX-FileCopyrightText: 2021 Janne Blomqvist <blomqvist.janne@gmail.com>
 *
 * SPDX-License-Identifier:  LGPL-2.1-or-later
 */


#ifdef FSV_GEOMETRY_H
	#error
#endif
#define FSV_GEOMETRY_H


/* Exported geometry constants */
#define TREEV_LEAF_NODE_EDGE		256.0
#define TREEV_PLATFORM_SPACING_DEPTH	2048.0

#define MAPV_GEOM_PARAMS(node)		((MapVGeomParams *)(NODE_DESC(node)->geomparams))
#define TREEV_GEOM_PARAMS(node)		((TreeVGeomParams *)(NODE_DESC(node)->geomparams))

#define MAPV_NODE_WIDTH(node)		(MAPV_GEOM_PARAMS(node)->c1.x - MAPV_GEOM_PARAMS(node)->c0.x)
#define MAPV_NODE_DEPTH(node)		(MAPV_GEOM_PARAMS(node)->c1.y - MAPV_GEOM_PARAMS(node)->c0.y)
#define MAPV_NODE_CENTER_X(node)	(0.5 * (MAPV_GEOM_PARAMS(node)->c0.x + MAPV_GEOM_PARAMS(node)->c1.x))
#define MAPV_NODE_CENTER_Y(node)	(0.5 * (MAPV_GEOM_PARAMS(node)->c0.y + MAPV_GEOM_PARAMS(node)->c1.y))


/* Geometry parameters for a node in MapV mode */
typedef struct _MapVGeomParams MapVGeomParams;
struct _MapVGeomParams {
	XYvec	c0;	/* 2D left/front corner (x0,y0) */
	XYvec	c1;	/* 2D right/rear corner (x1,y1) */
	/* Note: x1 > x0 and y1 > y0 */
	double	height;	/* Height of node (bottom to top) */
};

/* Geometry parameters for a node in TreeV mode */
typedef struct _TreeVGeomParams TreeVGeomParams;
struct _TreeVGeomParams {
	struct {
		/* Distance from center of leaf to inner edge of parent */
		double distance;
		/* Angular position, relative to parent's centerline */
		double theta;
		/* Height of leaf (measured from bottom to top, not from z=0) */
		double height;
	} leaf;

	/* This next set is for expanded directories (platforms) only */
	struct {
		/* Angular position of centerline, relative to centerline
		 * of the parent directory */
		double theta;
		/* Distance from inner to outer edge */
		double depth;
		/* Arc width in degrees. This includes the constant-width
		 * spacer regions at either side of the platform */
		double arc_width;
		/* Height of platform (measured from z=0 to top) */
		double height;
		/* Overall arc width of subtree */
		double subtree_arc_width;
		/* Outermost radius of this subtree as an offset from r0
		 * (own outer edge, or further if an expanded child reaches
		 * further). Cached in treev_arrange_recursive( ) so draw-time
		 * culling is O(1). Lives in geomparams2[]; do not add more
		 * platform fields without enlarging that array -- see DirNodeDesc. */
		double subtree_max_depth;
	} platform;
};


double geometry_mapv_node_z0( GNode *node );
double geometry_mapv_max_expanded_height( GNode *dnode );
boolean geometry_treev_is_leaf( GNode *node );
GNode *geometry_treev_platform_node( GNode *node );
double geometry_treev_platform_r0( GNode *dnode );
double geometry_treev_platform_theta( GNode *dnode );
double geometry_treev_max_leaf_height( GNode *dnode );
void geometry_treev_get_extents( GNode *dnode, RTvec *ext_c0, RTvec *ext_c1 );
void geometry_queue_rebuild( GNode *dnode );
void geometry_init( FsvMode mode );
void geometry_gldraw_fsv( void );
void geometry_draw( boolean high_detail );
void geometry_camera_pan_finished( void );
void geometry_colexp_initiated( GNode *dnode );
void geometry_colexp_in_progress( GNode *dnode );
boolean geometry_should_highlight(GNode *node);
void geometry_highlight_node( GNode *node, boolean strong );
void geometry_free_recursive( GNode *dnode );

/* Runtime performance toggles (Help menu check items; see geometry.c for
 * the chosen defaults and the env vars that override them at startup) */
boolean geometry_treev_cull_enabled( void );
void geometry_set_treev_cull( boolean enabled );
boolean geometry_treev_lod_enabled( void );
void geometry_set_treev_lod( boolean enabled );
boolean geometry_treev_hide_labels_moving( void );
void geometry_set_treev_hide_labels_moving( boolean enabled );
boolean geometry_mapv_hide_labels_moving( void );
void geometry_set_mapv_hide_labels_moving( boolean enabled );


/* end geometry.h */
