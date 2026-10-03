/* geometry.c */

/* 3D geometry generation and rendering */

/* fsv - 3D File System Visualizer
 * Copyright (C)1999 Daniel Richard G. <skunk@mit.edu>
 * SPDX-FileCopyrightText: 2021 Janne Blomqvist <blomqvist.janne@gmail.com>
 *
 * SPDX-License-Identifier:  LGPL-2.1-or-later
 */


#include "common.h"
#include "geometry.h"

#include <cglm/call.h>

#include "about.h"
#include "animation.h"
#include "camera.h"
#include "color.h"
#include "dirtree.h" /* dirtree_entry_expanded( ) */
#include "ogl.h"
#include "tmaptext.h"
#include "viewport.h"

/* 3D geometry for splash screen */
#include "fsv3d.h"

/* TreeVGeomParams overlays NodeDesc.geomparams[] then DirNodeDesc.geomparams2[].
 * Files only have the first 5 doubles (leaf fields). Directories must fit all
 * 9. If this fires, enlarge geomparams2 and this count together. */
G_STATIC_ASSERT(sizeof(TreeVGeomParams) == 9 * sizeof(double));
G_STATIC_ASSERT(sizeof(TreeVGeomParams) <= sizeof(((DirNodeDesc *)0)->node_desc.geomparams) + sizeof(((DirNodeDesc *)0)->geomparams2));


/* Cursor position remapping from linear to quarter-sine
 * (input and output are both [0, 1]) */
#define CURSOR_POS(x)			sin( (0.5 * PI) * (x) )


// TODO: Implement this caching strategy with VBO's
/* Current "drawing stage" for low- and high-detail geometry:
 * Stage 0: Full recursive draw, some geometry will be rebuilt along the way
 * Stage 1: Full recursive draw, no geometry rebuilt, capture everything in
 *          a display list (fstree_*_dlist, see above)
 * Stage 2: Fast draw using display list from stage 1 (no recursion) */
static int fstree_low_draw_stage;
static int fstree_high_draw_stage;


/* Forward declarations */
static void cursor_pre( void );
static void cursor_hidden_part( void );
static void cursor_visible_part( void );
static void cursor_post( void );
static void queue_uncached_draw( void );
static void treev_batch_flush( void );
static void treev_cache_invalidate( void );
static void mapv_cache_invalidate( void );


// Vertex struct for modern OpenGL with normals
typedef struct Vertex {
	GLfloat position[3];
	GLfloat normal[3];
} Vertex;

// Vertex struct with only position
typedef struct VertexPos {
	GLfloat position[3];
} VertexPos;

// Vertex struct carrying its own per-vertex color, for batched rendering
// (see mapv_batch_* below) where many differently-colored nodes need to
// be merged into a handful of draw calls instead of one draw call each.
typedef struct ColorVertex {
	GLfloat position[3];
	GLfloat normal[3];
	GLfloat color[3];
	GLfloat node_id;
} ColorVertex;

/* Compact per-node parameters for instanced MapV drawing. */
typedef struct MapVInstance {
	GLfloat bounds[4];
	GLfloat shape[4];
	GLfloat transform_color[4];
	GLfloat node_id;
} MapVInstance;


// Print the legacy and modern OpenGL projection and modelview matrices.
// which = 0: both modelview and projection matrices
// which = 1: Only modelview
// which = 2: Only projection
__attribute__((unused)) static void
debug_print_matrices(int which)
{
#ifdef DEBUG
	if (which == 0 || which == 1) {
		g_print("Modelview matrix:\n");
		glmc_mat4_print(gl.modelview, stdout);
	}
	if (which == 0 || which == 2) {
		g_print("\nProjection matrix:\n");
		glmc_mat4_print(gl.projection, stdout);
	}
#endif
}


static unsigned int highlight_node_id;
static boolean treev_cache_building;

// Set node color and lightning enabled uniform. GL Program must be in use when
// calling this.
static void
node_set_color(GNode *node)
{
	GLfloat color[4];
	color[3] = 1.0;	 // Alpha
	if (gl.render_mode == RENDERMODE_RENDER) {
		memcpy(color, NODE_DESC(node)->color, 3 * sizeof(GLfloat));
		// Check highlight
		if (NODE_DESC(node)->id == highlight_node_id) {
			for (size_t i = 0; i < 3; i++)
				color[i] *= 1.3f;
		}
		glUniform1i(gl.lightning_enabled_location, 1);
	} else {
		GLuint c = NODE_DESC(node)->id;
		// const char *name = NODE_DESC(node)->name;
		GLuint r = (c & 0x000000FF) >> 0;
		GLuint g = (c & 0x0000FF00) >> 8;
		GLuint b = (c & 0x00FF0000) >> 16;
		color[0] = (GLfloat)r / G_MAXUINT8;
		color[1] = (GLfloat)g / G_MAXUINT8;
		color[2] = (GLfloat)b / G_MAXUINT8;
		// g_print("Painting node %s id %u with Color red %f green %f
		// blue %f\n", 	name, c, (double)color[0], (double)color[1],
		//(double)color[2]);
		glUniform1i(gl.lightning_enabled_location, 0);
	}

	glUniform4fv(gl.color_location, 1, color);
}

static const RGBcolor color_black = {0, 0, 0};

// Upload and draw a bunch of VertexPos vertices.
// Note this is highly inefficient and implements every known modern GL
// anti-pattern (e.g. does not take any advantage of batching, or keeping
// vertex data on the GPU instead of reuploading it every time). But hey,
// it's simple.
static void
drawVertexPos(GLenum mode, VertexPos *vert, size_t vert_cnt, const RGBcolor *color)
{
	if (gl.render_mode == RENDERMODE_SELECT && globals.fsv_mode == FSV_TREEV &&
	    !treev_cache_building)
		treev_batch_flush( );
	static GLuint vbo;
	if (!vbo)
		glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(VertexPos) * vert_cnt, vert, GL_STREAM_DRAW);

	glEnableVertexAttribArray(gl.position_location);
	glVertexAttribPointer(gl.position_location, 3, GL_FLOAT, GL_FALSE,
			      sizeof(VertexPos), (void *)offsetof(VertexPos, position));

	glUseProgram(gl.program);
	glUniform4f(gl.color_location, color->r, color->g, color->b, 1);
	glUniform1i(gl.lightning_enabled_location, 0);
	glDrawArrays(mode, 0, vert_cnt);
	glUseProgram(0);

	// Avoid implicit sync by allowing GL to dealloc memory
	glBufferData(GL_ARRAY_BUFFER, sizeof(VertexPos) * vert_cnt, NULL, GL_STREAM_DRAW);

	glBindBuffer(GL_ARRAY_BUFFER, 0);
}


// Similar to above, but draw a Vertex.
// The color is taken either from the color argument, or the node argument.
// One of these must be NULL and the other non-null.
static void
drawVertex(GLenum mode, Vertex *vert, size_t vert_cnt, const RGBcolor *color, GNode *node)
{
	if (gl.render_mode == RENDERMODE_SELECT && globals.fsv_mode == FSV_TREEV &&
	    !treev_cache_building)
		treev_batch_flush( );
	if (color != NULL)
		g_assert(node == NULL);
	else
		g_assert(node != NULL);

	static GLuint vbo;
	if (!vbo) glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(Vertex) * vert_cnt, vert,
		     GL_STREAM_DRAW);

	glEnableVertexAttribArray(gl.position_location);
	glVertexAttribPointer(gl.position_location, 3, GL_FLOAT, GL_FALSE,
			      sizeof(Vertex),
			      (void *)offsetof(Vertex, position));
	glEnableVertexAttribArray(gl.normal_location);
	glVertexAttribPointer(gl.normal_location, 3, GL_FLOAT, GL_FALSE,
			      sizeof(Vertex), (void *)offsetof(Vertex, normal));

	glUseProgram(gl.program);
	if (color) {
		glUniform4f(gl.color_location, color->r, color->g, color->b, 1);
		glUniform1i(gl.lightning_enabled_location, 1);
	} else
		node_set_color(node);
	glDrawArrays(mode, 0, vert_cnt);
	glUseProgram(0);

	// Avoid implicit sync by allowing GL to dealloc memory
	glBufferData(GL_ARRAY_BUFFER, sizeof(Vertex) * vert_cnt, NULL,
		     GL_STREAM_DRAW);

	glBindBuffer(GL_ARRAY_BUFFER, 0);
}


/**** MAP VISUALIZATION ***************************************/


/* Geometry constants */
#define MAPV_BORDER_PROPORTION	0.01
#define MAPV_ROOT_ASPECT_RATIO	1.2

/* Skip MapV labels below roughly four pixels high, using the same
 * projected-size threshold as TreeV. */
#define MAPV_LABEL_MIN_NDC_SIZE		0.008

/* A directory whose footprint would project to less than this fraction
 * of the screen (in both width and height, in normalized device
 * coordinates) is skipped entirely -- geometry and all -- even if it's
 * technically still inside the view frustum. This matters for a very
 * large directory (e.g. one that dominates the tree by size, and so
 * has a correspondingly huge footprint): its bounding box can remain
 * inside the theoretical view volume while the camera is zoomed into a
 * small, unrelated corner elsewhere, where it wouldn't actually be
 * visible (or occupies only a meaningless sliver of the screen). */
#define MAPV_GEOMETRY_MIN_NDC_SIZE	0.02
#define MAPV_FLAT_LOD_NDC_SIZE		0.018

/* Messages for mapv_draw_recursive( ) */
enum {
	MAPV_DRAW_GEOMETRY,
	MAPV_DRAW_LABELS,
	MAPV_DRAW_SUBTREES,
	MAPV_DRAW_FOLDERS
};

typedef struct MapVDrawRow {
	GNode *first;
	GNode *end;
	double y0, y1;
	double zmax;
} MapVDrawRow;

/* Per-directory row spans mirror the existing treemap layout. They let a
 * frame reject whole bands of children before inspecting individual nodes. */
static GHashTable *mapv_draw_rows;
static GHashTable *mapv_draw_peak_heights;


/* Node side face offset ratios, by node type
 * (these define the obliqueness of a node's side faces) */
static const float mapv_side_slant_ratios[NUM_NODE_TYPES] = {
	NIL,	/* Metanode (not used) */
	0.032,	/* Directory */
	0.064,	/* Regular file */
	0.333,	/* Symlink */
	0.0,	/* FIFO */
	0.0,	/* Socket */
	0.25,	/* Character device */
	0.25,	/* Block device */
	0.0	/* Unknown */
};

/* Heights of directory and leaf nodes */
static double mapv_dir_height = 384.0;
static double mapv_leaf_height = 128.0;

/* Previous steady-state positions of the cursor corners
 * (if the cursor is moving from A to B, these delineate A) */
static XYZvec mapv_cursor_prev_c0;
static XYZvec mapv_cursor_prev_c1;


/* Returns the z-position of the bottom of a node */
double
geometry_mapv_node_z0( GNode *node )
{
	GNode *up_node;
	double z = 0.0;

	up_node = node->parent;
	while (up_node != NULL) {
		z += MAPV_GEOM_PARAMS(up_node)->height;
		up_node = up_node->parent;
	}

	return z;
}


/* Returns the peak height of a directory's contents (measured relative
 * to its top face), dictated by its expansion state as indicated by the
 * directory tree */
double
geometry_mapv_max_expanded_height( GNode *dnode )
{
	GNode *node;
	double height, max_height = 0.0;

	g_assert( NODE_IS_DIR(dnode) );

	if (dirtree_entry_expanded( dnode )) {
		node = dnode->children;
		while (node != NULL) {
			height = MAPV_GEOM_PARAMS(node)->height;
			if (NODE_IS_DIR(node)) {
				height += geometry_mapv_max_expanded_height( node );
				max_height = MAX(max_height, height);
			}
			else {
				max_height = MAX(max_height, height);
				break;
			}
			node = node->next;
		}
	}

	return max_height;
}


/* Helper function for mapv_init( ).
 * This is, in essence, the MapV layout engine */
static double
mapv_init_recursive( GNode *dnode )
{
	struct MapVBlock {
		GNode *node;
		double area;
		double subtree_height;
	} *block, *next_first_block;
	struct MapVRow {
		struct MapVBlock *first_block;
		double area;
	} *row = NULL;
	GArray *draw_rows = g_array_new(FALSE, FALSE, sizeof(MapVDrawRow));
	MapVGeomParams *gparams;
	GNode *node;
	GList *block_list = NULL, *block_llink;
	GList *row_list = NULL, *row_llink;
	XYvec dir_dims, block_dims;
	XYvec start_pos, pos;
	double area, dir_area, total_block_area = 0.0;
	double nominal_border, border;
	double scale_factor;
	double a, b, k;
	double max_subtree_height = 0.0;
	int64 size;

	g_assert( NODE_IS_DIR(dnode) );

	morph_break( &DIR_NODE_DESC(dnode)->deployment );
	if (dirtree_entry_expanded( dnode ) ||
	    (!dirtree_entry_has_subdir( dnode ) && (DIR_NODE_DESC(dnode)->deployment > (1.0 - EPSILON))))
		DIR_NODE_DESC(dnode)->deployment = 1.0;
	else
		DIR_NODE_DESC(dnode)->deployment = 0.0;
	geometry_queue_rebuild( dnode );

	/* If this directory has no children,
	 * there is nothing further to do here */
	if (dnode->children == NULL)
		goto done;

	/* Obtain dimensions of top face of directory */
	dir_dims.x = MAPV_NODE_WIDTH(dnode);
	dir_dims.y = MAPV_NODE_DEPTH(dnode);
	k = mapv_side_slant_ratios[NODE_DIRECTORY];
	dir_dims.x -= 2.0 * MIN(MAPV_GEOM_PARAMS(dnode)->height, k * dir_dims.x);
	dir_dims.y -= 2.0 * MIN(MAPV_GEOM_PARAMS(dnode)->height, k * dir_dims.y);

	/* Approximate/nominal node border width (nodes will be spaced
	 * apart at about twice this distance) */
	a = MAPV_BORDER_PROPORTION * sqrt( dir_dims.x * dir_dims.y );
	b = MIN(dir_dims.x, dir_dims.y) / 3.0;
	nominal_border = MIN(a, b);

	/* Trim half a border width off the perimeter of the directory,
	 * so that nodes aren't laid down too close to the edges */
	dir_dims.x -= nominal_border;
	dir_dims.y -= nominal_border;
	dir_area = dir_dims.x * dir_dims.y;

	/* First pass
	 * 1. Create blocks. (A block is equivalent to a node, except
	 *    that it includes the node's surrounding border area)
	 * 2. Find total area of the blocks
	 * 3. Create a list of the blocks */
	node = dnode->children;
	while (node != NULL) {
		size = MAX(256, NODE_DESC(node)->size);
		if (NODE_IS_DIR(node))
			size += DIR_NODE_DESC(node)->subtree.size;
		k = sqrt( (double)size ) + nominal_border;
		area = SQR(k);
		total_block_area += area;

		block = NEW(struct MapVBlock);
		block->node = node;
		block->area = area;
		block->subtree_height = 0.0;
		G_LIST_APPEND(block_list, block);

		node = node->next;
	}

	/* The blocks are going to have a total area greater than the
	 * directory can provide, so they'll have to be scaled down */
	scale_factor = dir_area / total_block_area;

	/* Second pass
	 * 1. Scale down the blocks
	 * 2. Generate a first-draft set of rows */
	block_llink = block_list;
	while (block_llink != NULL) {
		block = (struct MapVBlock *)block_llink->data;
		block->area *= scale_factor;

		if (row == NULL) {
			/* Begin new row */
			row = NEW(struct MapVRow);
			row->first_block = block;
			row->area = 0.0;
			G_LIST_APPEND(row_list, row);
		}

		/* Add block to row */
		row->area += block->area;

		/* Dimensions of block (block_dims.y == depth of row) */
		block_dims.y = row->area / dir_dims.x;
		block_dims.x = block->area / block_dims.y;

		/* Check aspect ratio of block */
		if ((block_dims.x / block_dims.y) < 1.0) {
			/* Next block will go into next row */
			row = NULL;
		}

		block_llink = block_llink->next;
	}

	/* Third pass - optimize layout */
	/* Note to self: write layout optimization routine sometime */

	/* Fourth pass - output final arrangement
	 * Start at right/rear corner, laying out rows of (mostly)
	 * successively smaller blocks */
	start_pos.x = MAPV_NODE_CENTER_X(dnode) + 0.5 * dir_dims.x;
	start_pos.y = MAPV_NODE_CENTER_Y(dnode) + 0.5 * dir_dims.y;
	pos.y = start_pos.y;
	block_llink = block_list;
	row_llink = row_list;
	while (row_llink != NULL) {
		MapVDrawRow draw_row;
		double row_zmax = 0.0;
		row = (struct MapVRow *)row_llink->data;
		block_dims.y = row->area / dir_dims.x;
		pos.x = start_pos.x;

		/* Note first block of next row */
		if (row_llink->next == NULL)
			next_first_block = NULL;
		else
			next_first_block = ((struct MapVRow *)row_llink->next->data)->first_block;
		draw_row.first = row->first_block->node;
		draw_row.end = next_first_block ? next_first_block->node : NULL;
		draw_row.y1 = pos.y;
		draw_row.y0 = pos.y - block_dims.y;

		/* Output one row */
		while (block_llink != NULL) {
			block = (struct MapVBlock *)block_llink->data;
			if (block == next_first_block)
				break; /* finished with row */
			block_dims.x = block->area / block_dims.y;

			size = MAX(256, NODE_DESC(block->node)->size);
			if (NODE_IS_DIR(block->node))
				size += DIR_NODE_DESC(block->node)->subtree.size;
			area = scale_factor * (double)size;

			/* Calculate exact width of block's border region */
			k = block_dims.x + block_dims.y;
			/* Note: area == scaled area of node,
			 * block->area == scaled area of node + border */
			border = 0.25 * (k - sqrt( SQR(k) - 4.0 * (block->area - area) ));

			/* Assign geometry
			 * (Note: pos is right/rear corner of block) */
			gparams = MAPV_GEOM_PARAMS(block->node);
			gparams->c0.x = pos.x - block_dims.x + border;
			gparams->c0.y = pos.y - block_dims.y + border;
			gparams->c1.x = pos.x - border;
			gparams->c1.y = pos.y - border;

			if (NODE_IS_DIR(block->node)) {
				gparams->height = mapv_dir_height;

				/* Recurse into directory */
				block->subtree_height = mapv_init_recursive( block->node );
			}
			else
				gparams->height = mapv_leaf_height;

			row_zmax = MAX(row_zmax, gparams->height + block->subtree_height);

			pos.x -= block_dims.x;
			block_llink = block_llink->next;
		}
		draw_row.zmax = row_zmax;
		max_subtree_height = MAX(max_subtree_height, row_zmax);
		g_array_append_val(draw_rows, draw_row);

		pos.y -= block_dims.y;
		row_llink = row_llink->next;
	}

	if (mapv_draw_rows)
		g_hash_table_insert(mapv_draw_rows, dnode, draw_rows);
	else
		g_array_unref(draw_rows);
draw_rows = NULL;

	/* Clean up */

	block_llink = block_list;
	while (block_llink != NULL) {
		xfree( block_llink->data );
		block_llink = block_llink->next;
	}
	g_list_free( block_list );

	row_llink = row_list;
	while (row_llink != NULL) {
		xfree( row_llink->data );
		row_llink = row_llink->next;
	}
	g_list_free( row_list );
	done:
	if (draw_rows)
		g_array_unref(draw_rows);
	if (mapv_draw_peak_heights) {
		double *peak = NEW(double);
		*peak = max_subtree_height;
		g_hash_table_insert(mapv_draw_peak_heights, dnode, peak);
	}
	return max_subtree_height;
}


/* Top-level call to initialize MapV mode */
static void
mapv_init( void )
{
	MapVGeomParams *gparams;
	XYvec root_dims;
	double k;

	if (mapv_draw_rows)
		g_hash_table_destroy(mapv_draw_rows);
	mapv_draw_rows = g_hash_table_new_full(g_direct_hash, g_direct_equal,
						      NULL, (GDestroyNotify)g_array_unref);
	if (mapv_draw_peak_heights)
		g_hash_table_destroy(mapv_draw_peak_heights);
	mapv_draw_peak_heights = g_hash_table_new_full(g_direct_hash, g_direct_equal,
						       NULL, g_free);

	/* Determine dimensions of bottommost (root) node */
	root_dims.y = sqrt( (double)DIR_NODE_DESC(globals.fstree)->subtree.size / MAPV_ROOT_ASPECT_RATIO );
	root_dims.x = MAPV_ROOT_ASPECT_RATIO * root_dims.y;

	/* Set up base geometry */
	MAPV_GEOM_PARAMS(globals.fstree)->height = 0.0;
	gparams = MAPV_GEOM_PARAMS(root_dnode);
	gparams->c0.x = -0.5 * root_dims.x;
	gparams->c0.y = -0.5 * root_dims.y;
	gparams->c1.x = 0.5 * root_dims.x;
	gparams->c1.y = 0.5 * root_dims.y;
	gparams->height = mapv_dir_height;

	mapv_init_recursive( root_dnode );
	mapv_cache_invalidate( );

	/* Initial cursor state */
	if (globals.current_node == root_dnode)
		k = 4.0;
	else
		k = 1.25;
	mapv_cursor_prev_c0.x = k * MAPV_GEOM_PARAMS(root_dnode)->c0.x;
	mapv_cursor_prev_c0.y = k * MAPV_GEOM_PARAMS(root_dnode)->c0.y;
	mapv_cursor_prev_c0.z = - 0.25 * k * MAPV_NODE_DEPTH(root_dnode);
	mapv_cursor_prev_c1.x = k * MAPV_GEOM_PARAMS(root_dnode)->c1.x;
	mapv_cursor_prev_c1.y = k * MAPV_GEOM_PARAMS(root_dnode)->c1.y;
	mapv_cursor_prev_c1.z = 0.25 * k * MAPV_NODE_DEPTH(root_dnode);
}


/* Hook function for camera pan completion */
static void
mapv_camera_pan_finished( void )
{
	/* Save cursor position */
	mapv_cursor_prev_c0.x = MAPV_GEOM_PARAMS(globals.current_node)->c0.x;
	mapv_cursor_prev_c0.y = MAPV_GEOM_PARAMS(globals.current_node)->c0.y;
	mapv_cursor_prev_c0.z = geometry_mapv_node_z0( globals.current_node );
	mapv_cursor_prev_c1.x = MAPV_GEOM_PARAMS(globals.current_node)->c1.x;
	mapv_cursor_prev_c1.y = MAPV_GEOM_PARAMS(globals.current_node)->c1.y;
	mapv_cursor_prev_c1.z = mapv_cursor_prev_c0.z + MAPV_GEOM_PARAMS(globals.current_node)->height;
}


/* Batching for MapV node geometry -- RENDERMODE_RENDER only. Accumulates
 * vertices/indices for many nodes and uploads+draws them in a handful of
 * large calls instead of one small draw call per node, which is what
 * made MapV frame time scale so badly with the number of visible nodes
 * (measured 2026-08-29: FPS dropped from 60 to 27-40 with many nodes
 * visible, recovering to 60 once zoomed into a small area -- tracking
 * visible node count, not camera direction). RENDERMODE_SELECT
 * (node-picking) keeps using the original one-draw-call-per-node path
 * below unchanged: it only runs once per click/hover, so call count
 * doesn't matter there, and it needs each node isolated with its own
 * ID-encoded, unlit color anyway. */
#define MAPV_BATCH_MAX_NODES	3276	/* 3276*20 verts stays under 65536, the GLushort index limit */
#define MAPV_INSTANCE_BATCH_INITIAL_NODES 8192
static ColorVertex *mapv_batch_verts = NULL;
static GLushort *mapv_batch_idx = NULL;
static size_t mapv_batch_vert_cnt = 0;
static size_t mapv_batch_idx_cnt = 0;
static GLuint mapv_batch_vbo, mapv_batch_ebo;
static MapVInstance *mapv_instances = NULL;
static size_t mapv_instance_cnt = 0;
static size_t mapv_instance_capacity = 0;
static GLuint mapv_instance_vbo, mapv_unit_vbo, mapv_unit_ebo;
static GLuint mapv_cached_instance_vbo;
static GLsizei mapv_cached_instance_count;
static boolean mapv_cache_valid;
static boolean mapv_cache_dirty = TRUE;
static boolean mapv_cache_using;
static boolean mapv_cache_building;
static double mapv_cache_dirty_since;
static int mapv_instancing_available = -1;
static boolean mapv_instancing_enabled( void );

static void
mapv_cache_invalidate(void)
{
	mapv_cache_valid = FALSE;
	mapv_cache_dirty = TRUE;
	mapv_cache_dirty_since = xgettime();
}

/* Store every box in the wide, stable view. Camera motion can then reuse the
 * instance buffer directly; the normal per-node culling path remains active
 * for close views where it can skip most of the scene. */
static boolean
mapv_cache_overview(void)
{
	MapVGeomParams *root_gp;
	double subtree_height = 0.0;
	double extent, field_height;
	if (camera == NULL || root_dnode == NULL || !mapv_instancing_enabled())
		return FALSE;
	root_gp = MAPV_GEOM_PARAMS(root_dnode);
	if (mapv_draw_peak_heights != NULL) {
		double *peak = g_hash_table_lookup(mapv_draw_peak_heights, root_dnode);
		if (peak != NULL)
			subtree_height = *peak;
	}
	extent = MAX(MAPV_NODE_WIDTH(root_dnode), MAPV_NODE_DEPTH(root_dnode));
	extent = MAX(extent, root_gp->height + subtree_height);
	field_height = camera->distance * tan(RAD(0.5 * camera->fov));
	return extent > 0.0 && field_height >= 0.25 * extent;
}

static boolean
mapv_instancing_enabled( void )
{
	if (mapv_instancing_available < 0)
		mapv_instancing_available = (epoxy_gl_version() >= 33 ||
			epoxy_has_gl_extension("GL_ARB_instanced_arrays"));
	return mapv_instancing_available;
}

/* gl.modelview as it stood right before the traversal that's filling the
 * batch began (i.e. the plain camera view matrix, with none of the
 * per-node translate/scale steps mapv_draw_recursive( ) applies while
 * descending). Everything in the batch gets baked into THIS frame at
 * add-time (see mapv_batch_add_node( )), because the batch is only
 * flushed (drawn) once, after the whole traversal returns -- by which
 * point gl.modelview has been restored to exactly this value, so a
 * single draw call under it renders every node at its correct nested
 * position. The hierarchy's simple Z-only transform is tracked separately
 * below, so no per-node matrix inversion is needed. */
static mat4 mapv_batch_root_modelview;
static mat4 mapv_batch_root_mvp;
/* MapV's hierarchy only translates/scales on Z. Track that relative
 * transform as two scalars instead of multiplying and inverting matrices
 * separately for each of the many nodes submitted to the batch. */
static double mapv_batch_z_scale = 1.0;
static double mapv_batch_z_offset = 0.0;

/* Call once per frame, before any mapv_gldraw_node( ) calls, to reset
 * the batch (allocating its backing storage on first use) */
static void
mapv_batch_begin( void )
{
	if (mapv_instancing_enabled( )) {
		if (mapv_instances == NULL) {
			mapv_instance_capacity = MAPV_INSTANCE_BATCH_INITIAL_NODES;
			mapv_instances = NEW_ARRAY(MapVInstance, mapv_instance_capacity);
		}
		mapv_instance_cnt = 0;
	} else if (mapv_batch_verts == NULL) {
		mapv_batch_verts = NEW_ARRAY(ColorVertex, MAPV_BATCH_MAX_NODES * 20);
		mapv_batch_idx = NEW_ARRAY(GLushort, MAPV_BATCH_MAX_NODES * 30);
	}
	mapv_batch_vert_cnt = 0;
	mapv_batch_idx_cnt = 0;
	mapv_cache_using = gl.render_mode == RENDERMODE_RENDER &&
		mapv_cache_valid && !mapv_cache_dirty && mapv_cache_overview();
	mapv_cache_building = gl.render_mode == RENDERMODE_RENDER &&
		!mapv_cache_using && mapv_cache_dirty && mapv_cache_overview() &&
		(xgettime() - mapv_cache_dirty_since) >= 0.25;
	mapv_batch_z_scale = 1.0;
	mapv_batch_z_offset = 0.0;
	glm_mat4_copy(gl.modelview, mapv_batch_root_modelview);
	glm_mat4_mul(gl.projection, mapv_batch_root_modelview, mapv_batch_root_mvp);
}

/* Upload one shared unit box, then submit the per-node parameters as
 * instances. A dozen floats describe a node, versus 180 floats plus 30
 * indices in the expanded-vertex batch. */
static void
mapv_instance_flush( void )
{
	static const Vertex unit_vertices[20] = {
	    {{0,1,0},{0, 1,0}}, {{0,1,1},{0, 1,0}}, {{1,1,0},{0, 1,0}}, {{1,1,1},{0, 1,0}},
	    {{1,1,0},{1, 0,0}}, {{1,1,1},{1, 0,0}}, {{1,0,0},{1, 0,0}}, {{1,0,1},{1, 0,0}},
	    {{1,0,0},{0,-1,0}}, {{1,0,1},{0,-1,0}}, {{0,0,0},{0,-1,0}}, {{0,0,1},{0,-1,0}},
	    {{0,0,0},{-1,0,0}}, {{0,0,1},{-1,0,0}}, {{0,1,0},{-1,0,0}}, {{0,1,1},{-1,0,0}},
	    {{0,0,1},{0,0,1}}, {{1,0,1},{0,0,1}}, {{0,1,1},{0,0,1}}, {{1,1,1},{0,0,1}}
	};
	static const GLushort unit_indices[30] = {
	    0,1,2, 2,1,3, 4,5,6, 6,5,7, 8,9,10, 10,9,11,
	    12,13,14, 14,13,15, 16,17,18, 18,17,19
	};
	mat4 root_mvp;
	mat3 root_normal_matrix;

	if (mapv_instance_cnt == 0)
		return;

	if (!mapv_unit_vbo) {
		glGenBuffers(1, &mapv_unit_vbo);
		glBindBuffer(GL_ARRAY_BUFFER, mapv_unit_vbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(unit_vertices), unit_vertices, GL_STATIC_DRAW);
		glGenBuffers(1, &mapv_unit_ebo);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mapv_unit_ebo);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(unit_indices), unit_indices, GL_STATIC_DRAW);
		glGenBuffers(1, &mapv_instance_vbo);
	}

	glBindBuffer(GL_ARRAY_BUFFER, mapv_unit_vbo);
	glEnableVertexAttribArray(gl.position_location);
	glVertexAttribPointer(gl.position_location, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
			      (void *)offsetof(Vertex, position));
	glEnableVertexAttribArray(gl.normal_location);
	glVertexAttribPointer(gl.normal_location, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
			      (void *)offsetof(Vertex, normal));

	glBindBuffer(GL_ARRAY_BUFFER, mapv_instance_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(MapVInstance) * mapv_instance_cnt,
		     mapv_instances, GL_STREAM_DRAW);
	glEnableVertexAttribArray(gl.instance_bounds_location);
	glVertexAttribPointer(gl.instance_bounds_location, 4, GL_FLOAT, GL_FALSE,
			      sizeof(MapVInstance), (void *)offsetof(MapVInstance, bounds));
	glVertexAttribDivisor(gl.instance_bounds_location, 1);
	glEnableVertexAttribArray(gl.instance_shape_location);
	glVertexAttribPointer(gl.instance_shape_location, 4, GL_FLOAT, GL_FALSE,
			      sizeof(MapVInstance), (void *)offsetof(MapVInstance, shape));
	glVertexAttribDivisor(gl.instance_shape_location, 1);
	glEnableVertexAttribArray(gl.instance_transform_color_location);
	glVertexAttribPointer(gl.instance_transform_color_location, 4, GL_FLOAT, GL_FALSE,
			      sizeof(MapVInstance), (void *)offsetof(MapVInstance, transform_color));
	glVertexAttribDivisor(gl.instance_transform_color_location, 1);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mapv_unit_ebo);

	glm_mat4_mul(gl.projection, mapv_batch_root_modelview, root_mvp);
	glm_mat4_pick3(mapv_batch_root_modelview, root_normal_matrix);
	glm_mat3_inv(root_normal_matrix, root_normal_matrix);
	glm_mat3_transpose(root_normal_matrix);
	glUseProgram(gl.program);
	glUniformMatrix4fv(gl.modelview_location, 1, GL_FALSE, (float *)mapv_batch_root_modelview);
	glUniformMatrix3fv(gl.normal_matrix_location, 1, GL_FALSE, (float *)root_normal_matrix);
	glUniformMatrix4fv(gl.mvp_location, 1, GL_FALSE, (float *)root_mvp);
	glUniform1i(gl.lightning_enabled_location,
		    gl.render_mode == RENDERMODE_RENDER);
	glUniform1i(gl.use_vertex_color_location, 1);
	glUniform1i(gl.use_node_id_location, 1);
	glUniform1i(gl.selection_mode_location,
		    gl.render_mode == RENDERMODE_SELECT);
	glUniform1f(gl.highlighted_node_id_location, (GLfloat)highlight_node_id);
	glUniform1i(gl.instanced_geometry_location, 1);
	glUniform1i(gl.instanced_lod_dynamic_location, 0);
	glEnableVertexAttribArray(gl.instance_node_id_location);
	glVertexAttribPointer(gl.instance_node_id_location, 1, GL_FLOAT, GL_FALSE,
			      sizeof(MapVInstance), (void *)offsetof(MapVInstance, node_id));
	glVertexAttribDivisor(gl.instance_node_id_location, 1);
	glDrawElementsInstanced(GL_TRIANGLES, 30, GL_UNSIGNED_SHORT, 0,
			 (GLsizei)mapv_instance_cnt);
	glUniform1i(gl.instanced_geometry_location, 0);
	glUniform1i(gl.instanced_lod_dynamic_location, 0);
	glUniform1i(gl.use_vertex_color_location, 0);
	glUniform1i(gl.use_node_id_location, 0);
	glUniform1i(gl.selection_mode_location, 0);
	glUseProgram(0);
	glVertexAttribDivisor(gl.instance_bounds_location, 0);
	glVertexAttribDivisor(gl.instance_shape_location, 0);
	glVertexAttribDivisor(gl.instance_transform_color_location, 0);
	glVertexAttribDivisor(gl.instance_node_id_location, 0);
	glDisableVertexAttribArray(gl.instance_bounds_location);
	glDisableVertexAttribArray(gl.instance_shape_location);
	glDisableVertexAttribArray(gl.instance_transform_color_location);
	glDisableVertexAttribArray(gl.instance_node_id_location);

	/* A full batch can flush mid-traversal; restore matrices for the current
	 * hierarchy frame before outlines or later siblings are submitted. */
	ogl_upload_matrices(FALSE);
	glBufferData(GL_ARRAY_BUFFER, sizeof(MapVInstance) * mapv_instance_cnt,
		     NULL, GL_STREAM_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	mapv_instance_cnt = 0;
}

/* Draw the stable overview instance buffer without walking the file tree or
 * streaming the same records again. */
static void
mapv_cached_instance_draw(void)
{
	mat4 root_mvp;
	mat3 root_normal_matrix;
	if (mapv_cached_instance_count == 0 || mapv_unit_vbo == 0 ||
	    mapv_unit_ebo == 0 || mapv_cached_instance_vbo == 0)
		return;
	glBindBuffer(GL_ARRAY_BUFFER, mapv_unit_vbo);
	glEnableVertexAttribArray(gl.position_location);
	glVertexAttribPointer(gl.position_location, 3, GL_FLOAT, GL_FALSE,
			      sizeof(Vertex), (void *)offsetof(Vertex, position));
	glEnableVertexAttribArray(gl.normal_location);
	glVertexAttribPointer(gl.normal_location, 3, GL_FLOAT, GL_FALSE,
			      sizeof(Vertex), (void *)offsetof(Vertex, normal));
	glBindBuffer(GL_ARRAY_BUFFER, mapv_cached_instance_vbo);
	glEnableVertexAttribArray(gl.instance_bounds_location);
	glVertexAttribPointer(gl.instance_bounds_location, 4, GL_FLOAT, GL_FALSE,
			      sizeof(MapVInstance), (void *)offsetof(MapVInstance, bounds));
	glVertexAttribDivisor(gl.instance_bounds_location, 1);
	glEnableVertexAttribArray(gl.instance_shape_location);
	glVertexAttribPointer(gl.instance_shape_location, 4, GL_FLOAT, GL_FALSE,
			      sizeof(MapVInstance), (void *)offsetof(MapVInstance, shape));
	glVertexAttribDivisor(gl.instance_shape_location, 1);
	glEnableVertexAttribArray(gl.instance_transform_color_location);
	glVertexAttribPointer(gl.instance_transform_color_location, 4, GL_FLOAT, GL_FALSE,
			      sizeof(MapVInstance), (void *)offsetof(MapVInstance, transform_color));
	glVertexAttribDivisor(gl.instance_transform_color_location, 1);
	glEnableVertexAttribArray(gl.instance_node_id_location);
	glVertexAttribPointer(gl.instance_node_id_location, 1, GL_FLOAT, GL_FALSE,
			      sizeof(MapVInstance), (void *)offsetof(MapVInstance, node_id));
	glVertexAttribDivisor(gl.instance_node_id_location, 1);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mapv_unit_ebo);
	glm_mat4_mul(gl.projection, gl.modelview, root_mvp);
	glm_mat4_pick3(gl.modelview, root_normal_matrix);
	glm_mat3_inv(root_normal_matrix, root_normal_matrix);
	glm_mat3_transpose(root_normal_matrix);
	glUseProgram(gl.program);
	glUniformMatrix4fv(gl.modelview_location, 1, GL_FALSE, (float *)gl.modelview);
	glUniformMatrix3fv(gl.normal_matrix_location, 1, GL_FALSE, (float *)root_normal_matrix);
	glUniformMatrix4fv(gl.mvp_location, 1, GL_FALSE, (float *)root_mvp);
	glUniform1i(gl.lightning_enabled_location, 1);
	glUniform1i(gl.use_vertex_color_location, 1);
	glUniform1i(gl.use_node_id_location, 1);
	glUniform1i(gl.selection_mode_location, 0);
	glUniform1f(gl.highlighted_node_id_location, (GLfloat)highlight_node_id);
	glUniform1i(gl.instanced_geometry_location, 1);
	glUniform1i(gl.instanced_lod_dynamic_location, 1);
	glDrawElementsInstanced(GL_TRIANGLES, 30, GL_UNSIGNED_SHORT, 0,
			 mapv_cached_instance_count);
	glUniform1i(gl.instanced_geometry_location, 0);
	glUniform1i(gl.instanced_lod_dynamic_location, 0);
	glUniform1i(gl.use_vertex_color_location, 0);
	glUniform1i(gl.use_node_id_location, 0);
	glUniform1i(gl.selection_mode_location, 0);
	glUseProgram(0);
	glVertexAttribDivisor(gl.instance_bounds_location, 0);
	glVertexAttribDivisor(gl.instance_shape_location, 0);
	glVertexAttribDivisor(gl.instance_transform_color_location, 0);
	glVertexAttribDivisor(gl.instance_node_id_location, 0);
	glDisableVertexAttribArray(gl.instance_bounds_location);
	glDisableVertexAttribArray(gl.instance_shape_location);
	glDisableVertexAttribArray(gl.instance_transform_color_location);
	glDisableVertexAttribArray(gl.instance_node_id_location);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

/* Uploads and draws everything accumulated in the batch so far (in one
 * draw call), then resets it. Called whenever the batch is full, and
 * once more at the end of the frame to flush whatever's left over */
static void
mapv_batch_flush( void )
{
	if (mapv_instancing_enabled( )) {
		mapv_instance_flush( );
		return;
	}

	if (mapv_batch_vert_cnt == 0)
		return;

	if (!mapv_batch_vbo) {
		glGenBuffers(1, &mapv_batch_vbo);
		glGenBuffers(1, &mapv_batch_ebo);
	}

	glBindBuffer(GL_ARRAY_BUFFER, mapv_batch_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(ColorVertex) * mapv_batch_vert_cnt, mapv_batch_verts, GL_STREAM_DRAW);

	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mapv_batch_ebo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(GLushort) * mapv_batch_idx_cnt, mapv_batch_idx, GL_STREAM_DRAW);

	glEnableVertexAttribArray(gl.position_location);
	glVertexAttribPointer(gl.position_location, 3, GL_FLOAT, GL_FALSE,
			      sizeof(ColorVertex), (void *)offsetof(ColorVertex, position));
	glEnableVertexAttribArray(gl.normal_location);
	glVertexAttribPointer(gl.normal_location, 3, GL_FLOAT, GL_FALSE,
			      sizeof(ColorVertex), (void *)offsetof(ColorVertex, normal));
	glEnableVertexAttribArray(gl.vcolor_location);
	glVertexAttribPointer(gl.vcolor_location, 3, GL_FLOAT, GL_FALSE,
			      sizeof(ColorVertex), (void *)offsetof(ColorVertex, color));
	glVertexAttribPointer(gl.node_id_location, 1, GL_FLOAT, GL_FALSE,
			      sizeof(ColorVertex), (void *)offsetof(ColorVertex, node_id));

	/* The batch's vertices were baked into mapv_batch_root_modelview's
	 * frame at add-time (see mapv_batch_add_node( )), NOT whatever
	 * gl.modelview happens to be right now -- this flush can be forced
	 * mid-traversal if the batch fills up (MAPV_BATCH_MAX_NODES), at
	 * which point gl.modelview is some descendant's frame, not root's.
	 * So upload root's own mvp/modelview/normal_matrix just for this
	 * draw call, then restore whatever was active before via
	 * ogl_upload_matrices( ), since later draws in the traversal (folder
	 * outlines, sibling nodes not yet batched) still need it to reflect
	 * gl.modelview as it currently stands, not root. */
	mat4 root_mvp;
	mat3 root_normal_matrix;
	glm_mat4_mul(gl.projection, mapv_batch_root_modelview, root_mvp);
	glm_mat4_pick3(mapv_batch_root_modelview, root_normal_matrix);
	glm_mat3_inv(root_normal_matrix, root_normal_matrix);
	glm_mat3_transpose(root_normal_matrix);

	glUseProgram(gl.program);
	glUniformMatrix4fv(gl.modelview_location, 1, GL_FALSE, (float *)mapv_batch_root_modelview);
	glUniformMatrix3fv(gl.normal_matrix_location, 1, GL_FALSE, (float *)root_normal_matrix);
	glUniformMatrix4fv(gl.mvp_location, 1, GL_FALSE, (float *)root_mvp);
	glUniform1i(gl.lightning_enabled_location,
		    gl.render_mode == RENDERMODE_RENDER);
	glUniform1i(gl.use_vertex_color_location, 1);
	glUniform1i(gl.use_node_id_location, 0);
	glUniform1i(gl.selection_mode_location, 0);

	glDrawElements(GL_TRIANGLES, mapv_batch_idx_cnt, GL_UNSIGNED_SHORT, 0);

	glUniform1i(gl.use_vertex_color_location, 0);
	glUseProgram(0);

	/* Restore modelview/mvp/normal_matrix to reflect gl.modelview as the
	 * traversal in progress currently has it (a no-op if this flush
	 * happened at the natural end of the traversal, where gl.modelview
	 * had already been restored to root; necessary if it was forced
	 * mid-traversal by the batch filling up). */
	ogl_upload_matrices(FALSE);

	glBufferData(GL_ARRAY_BUFFER, sizeof(ColorVertex) * mapv_batch_vert_cnt, NULL, GL_STREAM_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

	mapv_batch_vert_cnt = 0;
	mapv_batch_idx_cnt = 0;
}

/* Appends a compact per-instance node record. */
static void
mapv_instance_add_node( GNode *node )
{
	MapVGeomParams *gparams = MAPV_GEOM_PARAMS(node);
	MapVInstance *instance;
	double width = MAPV_NODE_WIDTH(node);
	double depth = MAPV_NODE_DEPTH(node);
	double height = gparams->height;
	double slant = mapv_side_slant_ratios[NODE_DESC(node)->type];
	double lod_scale = mapv_batch_z_scale;
	GLfloat color[3];
	int i;
	vec4 clip_center;
	double cx = 0.5 * (gparams->c0.x + gparams->c1.x);
	double cy = 0.5 * (gparams->c0.y + gparams->c1.y);
	double cz = mapv_batch_z_offset + lod_scale * 0.5 * height;
	vec4 center = {(float)cx, (float)cy, (float)cz, 1.0f};
	glm_mat4_mulv(mapv_batch_root_mvp, center, clip_center);
	if (gl.render_mode == RENDERMODE_RENDER && clip_center[3] > 0.0001f) {
		double inv_w = 1.0 / clip_center[3];
		double ndc_w = (fabs(mapv_batch_root_mvp[0][0]) * width +
				fabs(mapv_batch_root_mvp[1][0]) * depth +
				fabs(mapv_batch_root_mvp[2][0]) * lod_scale * height) * inv_w;
		double ndc_h = (fabs(mapv_batch_root_mvp[0][1]) * width +
			fabs(mapv_batch_root_mvp[1][1]) * depth +
			fabs(mapv_batch_root_mvp[2][1]) * lod_scale * height) * inv_w;
		if (MAX(ndc_w, ndc_h) < MAPV_FLAT_LOD_NDC_SIZE)
			lod_scale = -lod_scale;
	}

	if (mapv_instance_cnt >= mapv_instance_capacity) {
		if (mapv_instance_capacity > G_MAXSIZE / 2 / sizeof(*mapv_instances))
			g_error("MapV instance buffer exceeded addressable memory");
		size_t new_capacity = mapv_instance_capacity * 2;
		g_assert(new_capacity > mapv_instance_capacity);
		mapv_instances = g_realloc_n(mapv_instances, new_capacity,
					      sizeof(*mapv_instances));
		mapv_instance_capacity = new_capacity;
	}

	if (gl.render_mode == RENDERMODE_SELECT) {
		GLuint id = NODE_DESC(node)->id;
		color[0] = (GLfloat)(id & 0xFF) / 255.0f;
		color[1] = (GLfloat)((id >> 8) & 0xFF) / 255.0f;
		color[2] = (GLfloat)((id >> 16) & 0xFF) / 255.0f;
	} else {
		memcpy(color, NODE_DESC(node)->color, sizeof(color));
		if (NODE_DESC(node)->id == highlight_node_id)
			for (i = 0; i < 3; i++)
				color[i] *= 1.3f;
	}

	instance = &mapv_instances[mapv_instance_cnt++];
	instance->bounds[0] = (GLfloat)gparams->c0.x;
	instance->bounds[1] = (GLfloat)gparams->c0.y;
	instance->bounds[2] = (GLfloat)gparams->c1.x;
	instance->bounds[3] = (GLfloat)gparams->c1.y;
	instance->shape[0] = (GLfloat)height;
	instance->shape[1] = (GLfloat)MIN(height, slant * width);
	instance->shape[2] = (GLfloat)MIN(height, slant * depth);
	instance->shape[3] = (GLfloat)lod_scale;
	instance->transform_color[0] = (GLfloat)mapv_batch_z_offset;
	instance->transform_color[1] = color[0];
	instance->transform_color[2] = color[1];
	instance->transform_color[3] = color[2];
	instance->node_id = (GLfloat)NODE_DESC(node)->id;
}


/* Appends node's 20-vertex/30-index box geometry (with its lit,
 * highlight-adjusted color baked into each vertex) to the batch,
 * flushing first if there isn't room left for it */
static void
mapv_batch_add_node( GNode *node )
{
	MapVGeomParams *gparams;
	XYZvec dims;
	XYvec offset, normal;
	double normal_z_nx, normal_z_ny;
	double a, b, k;
	GLfloat color[3];
	size_t base;
	int i;
	static const GLushort elements[] = {
	    0,	1,  2,	2,  1,	3,
	    4,	5,  6,	6,  5,	7,
	    8,	9,  10, 10, 9,	11,
	    12, 13, 14, 14, 13, 15,
	    16, 17, 18, 18, 17, 19
	};

	if (mapv_instancing_enabled( )) {
		mapv_instance_add_node( node );
		return;
	}

	if ((mapv_batch_vert_cnt + 20) > (size_t)(MAPV_BATCH_MAX_NODES * 20))
		mapv_batch_flush( );

	if (gl.render_mode == RENDERMODE_SELECT) {
		GLuint id = NODE_DESC(node)->id;
		color[0] = (GLfloat)(id & 0xFF) / 255.0f;
		color[1] = (GLfloat)((id >> 8) & 0xFF) / 255.0f;
		color[2] = (GLfloat)((id >> 16) & 0xFF) / 255.0f;
	} else {
		/* Same color logic as node_set_color( )'s render branch. */
		memcpy(color, NODE_DESC(node)->color, 3 * sizeof(GLfloat));
		if (NODE_DESC(node)->id == highlight_node_id) {
			for (i = 0; i < 3; i++)
				color[i] *= 1.3f;
		}
	}

	dims.x = MAPV_NODE_WIDTH(node);
	dims.y = MAPV_NODE_DEPTH(node);
	dims.z = MAPV_GEOM_PARAMS(node)->height;

	k = mapv_side_slant_ratios[NODE_DESC(node)->type];
	offset.x = MIN(dims.z, k * dims.x);
	offset.y = MIN(dims.z, k * dims.y);
	a = sqrt( SQR(offset.x) + SQR(dims.z) );
	b = sqrt( SQR(offset.y) + SQR(dims.z) );
	normal.x = dims.z / a;
	normal.y = dims.z / b;
	normal_z_nx = offset.x / a;
	normal_z_ny = offset.y / b;

	gparams = MAPV_GEOM_PARAMS(node);

	base = mapv_batch_vert_cnt;

	/* Bake the current hierarchical Z transform into each vertex. X/Y are
	 * already laid out in root coordinates; only Z varies with expansion. */

#define CV(px, py, pz, nx, ny, nz) do { \
	GLfloat _tz = (GLfloat)(mapv_batch_z_offset + mapv_batch_z_scale * (pz)); \
	GLfloat _nz = (GLfloat)((nz) / mapv_batch_z_scale); \
	mapv_batch_verts[mapv_batch_vert_cnt++] = (ColorVertex){{(GLfloat)(px), (GLfloat)(py), _tz}, {(GLfloat)(nx), (GLfloat)(ny), _nz}, {color[0], color[1], color[2]}}; \
} while (0)

	CV(gparams->c0.x, gparams->c1.y, 0.0, 0.0, normal.y, normal_z_ny); /* Rear face */
	CV(gparams->c0.x + offset.x, gparams->c1.y - offset.y, gparams->height, 0.0, normal.y, normal_z_ny);
	CV(gparams->c1.x, gparams->c1.y, 0.0, 0.0, normal.y, normal_z_ny);
	CV(gparams->c1.x - offset.x, gparams->c1.y - offset.y, gparams->height, 0.0, normal.y, normal_z_ny);
	CV(gparams->c1.x, gparams->c1.y, 0.0, normal.x, 0.0, normal_z_nx); /* Right face */
	CV(gparams->c1.x - offset.x, gparams->c1.y - offset.y, gparams->height, normal.x, 0.0, normal_z_nx);
	CV(gparams->c1.x, gparams->c0.y, 0.0, normal.x, 0.0, normal_z_nx);
	CV(gparams->c1.x - offset.x, gparams->c0.y + offset.y, gparams->height, normal.x, 0.0, normal_z_nx);
	CV(gparams->c1.x, gparams->c0.y, 0.0, 0.0, -normal.y, normal_z_ny); /* Front face */
	CV(gparams->c1.x - offset.x, gparams->c0.y + offset.y, gparams->height, 0.0, -normal.y, normal_z_ny);
	CV(gparams->c0.x, gparams->c0.y, 0.0, 0.0, -normal.y, normal_z_ny);
	CV(gparams->c0.x + offset.x, gparams->c0.y + offset.y, gparams->height, 0.0, -normal.y, normal_z_ny);
	CV(gparams->c0.x, gparams->c0.y, 0.0, -normal.x, 0.0, normal_z_nx); /* Left face */
	CV(gparams->c0.x + offset.x, gparams->c0.y + offset.y, gparams->height, -normal.x, 0.0, normal_z_nx);
	CV(gparams->c0.x, gparams->c1.y, 0.0, -normal.x, 0.0, normal_z_nx);
	CV(gparams->c0.x + offset.x, gparams->c1.y - offset.y, gparams->height, -normal.x, 0.0, normal_z_nx);
	/* Top face */
	CV(gparams->c0.x + offset.x, gparams->c0.y + offset.y, gparams->height, 0.0f, 0.0f, 1.0f);
	CV(gparams->c1.x - offset.x, gparams->c0.y + offset.y, gparams->height, 0.0f, 0.0f, 1.0f);
	CV(gparams->c0.x + offset.x, gparams->c1.y - offset.y, gparams->height, 0.0f, 0.0f, 1.0f);
	CV(gparams->c1.x - offset.x, gparams->c1.y - offset.y, gparams->height, 0.0f, 0.0f, 1.0f);

#undef CV

	for (i = 0; i < 30; i++)
		mapv_batch_idx[mapv_batch_idx_cnt++] = (GLushort)(base + elements[i]);
}


/* Draws a MapV node */
static void
mapv_gldraw_node( GNode *node )
{
	MapVGeomParams *gparams;
	XYZvec dims;
	XYvec offset, normal;
	double normal_z_nx, normal_z_ny;
	double a, b, k;

	if (gl.render_mode == RENDERMODE_RENDER || mapv_instancing_enabled( )) {
		mapv_batch_add_node( node );
		return;
	}

	/* Dimensions of node */
	dims.x = MAPV_NODE_WIDTH(node);
	dims.y = MAPV_NODE_DEPTH(node);
	dims.z = MAPV_GEOM_PARAMS(node)->height;

	/* Calculate normals for slanted sides */
	k = mapv_side_slant_ratios[NODE_DESC(node)->type];
	offset.x = MIN(dims.z, k * dims.x);
	offset.y = MIN(dims.z, k * dims.y);
	a = sqrt( SQR(offset.x) + SQR(dims.z) );
	b = sqrt( SQR(offset.y) + SQR(dims.z) );
	normal.x = dims.z / a;
	normal.y = dims.z / b;
	normal_z_nx = offset.x / a;
	normal_z_ny = offset.y / b;

	gparams = MAPV_GEOM_PARAMS(node);

	Vertex vertex_data[] = {
	    {{gparams->c0.x, gparams->c1.y, 0.0}, /* Rear face */
	     {0.0, normal.y, normal_z_ny}},
	    {{gparams->c0.x + offset.x, gparams->c1.y - offset.y,
	      gparams->height},
	     {0.0, normal.y, normal_z_ny}},
	    {{gparams->c1.x, gparams->c1.y, 0.0}, {0.0, normal.y, normal_z_ny}},
	    {{gparams->c1.x - offset.x, gparams->c1.y - offset.y,
	      gparams->height},
	     {0.0, normal.y, normal_z_ny}},
	    {{gparams->c1.x, gparams->c1.y, 0.0}, /* Right face */
	     {normal.x, 0.0, normal_z_nx}},
	    {{gparams->c1.x - offset.x, gparams->c1.y - offset.y,
	      gparams->height},
	     {normal.x, 0.0, normal_z_nx}},
	    {{gparams->c1.x, gparams->c0.y, 0.0}, {normal.x, 0.0, normal_z_nx}},
	    {{gparams->c1.x - offset.x, gparams->c0.y + offset.y,
	      gparams->height},
	     {normal.x, 0.0, normal_z_nx}},
	    {{gparams->c1.x, gparams->c0.y, 0.0},  // Front face
	     {0.0, -normal.y, normal_z_ny}},
	    {{gparams->c1.x - offset.x, gparams->c0.y + offset.y,
	      gparams->height},
	     {0.0, -normal.y, normal_z_ny}},
	    {{gparams->c0.x, gparams->c0.y, 0.0},
	     {0.0, -normal.y, normal_z_ny}},
	    {{gparams->c0.x + offset.x, gparams->c0.y + offset.y,
	      gparams->height},
	     {0.0, -normal.y, normal_z_ny}},
	    {{gparams->c0.x, gparams->c0.y, 0.0},  // Left face
	     {-normal.x, 0.0, normal_z_nx}},
	    {{gparams->c0.x + offset.x, gparams->c0.y + offset.y,
	      gparams->height},
	     {-normal.x, 0.0, normal_z_nx}},
	    {{gparams->c0.x, gparams->c1.y, 0.0},
	     {-normal.x, 0.0, normal_z_nx}},
	    {{gparams->c0.x + offset.x, gparams->c1.y - offset.y,
	      gparams->height},
	     {-normal.x, 0.0, normal_z_nx}},
	    // Top face
	    {{gparams->c0.x + offset.x, gparams->c0.y + offset.y,
	      gparams->height},	 // 1
	     {0.0f, 0.0f, 1.0f}},
	    {{gparams->c1.x - offset.x, gparams->c0.y + offset.y,
	      gparams->height},	 // 2
	     {0.0f, 0.0f, 1.0f}},
	    {{gparams->c0.x + offset.x, gparams->c1.y - offset.y,
	      gparams->height},	 // 4
	     {0.0f, 0.0f, 1.0f}},
	    {{gparams->c1.x - offset.x, gparams->c1.y - offset.y,
	      gparams->height},	 // 3
	     {0.0f, 0.0f, 1.0f}}};
	static const GLushort elements[] = {
	    0,	1,  2,	2,  1,	3,   // Rear face
	    4,	5,  6,	6,  5,	7,   // Right face
	    8,	9,  10, 10, 9,	11,  // Front face
	    12, 13, 14, 14, 13, 15,  // Left face
	    16, 17, 18, 18, 17, 19   // Top face
	};
	ogl_error();
	//debug_print_matrices(0);
	static GLuint vbo;
	if (!vbo)
		glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertex_data), &vertex_data, GL_DYNAMIC_DRAW);

	static GLuint ebo;
	if (!ebo) {
		glGenBuffers(1, &ebo);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(elements), &elements, GL_STATIC_DRAW);
	}

	glEnableVertexAttribArray(gl.position_location);
	glVertexAttribPointer(gl.position_location, 3, GL_FLOAT, GL_FALSE,
			      sizeof(Vertex), (void *)offsetof(Vertex, position));

	glEnableVertexAttribArray(gl.normal_location);
	glVertexAttribPointer(gl.normal_location, 3, GL_FLOAT, GL_FALSE,
			      sizeof(Vertex), (void *)offsetof(Vertex, normal));

	ogl_error();

	glUseProgram(gl.program);

	node_set_color(node);
#if 0
#ifdef DEBUG
	mat4 mvp;
	glm_mat4_mul(gl.projection, gl.modelview, mvp);
	// Check coords
	vec3 out;
	g_print("quad coords with rendermode %d:\n", gl.render_mode);
	glm_mat4_mulv3(mvp, vertex_data[0].position, 1, out);
	glmc_vec3_print(out, stdout);
	glm_mat4_mulv3(mvp, vertex_data[1].position, 1, out);
	glmc_vec3_print(out, stdout);
	glm_mat4_mulv3(mvp, vertex_data[3].position, 1, out);
	glmc_vec3_print(out, stdout);
#endif
#endif
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
	GLsizei cnt = sizeof(elements) / sizeof(GLushort);
	glDrawElements(GL_TRIANGLES, cnt, GL_UNSIGNED_SHORT, 0);
	glUseProgram(0);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertex_data), NULL, GL_DYNAMIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}


/* Draws a "folder" shape atop a directory */
static void
mapv_gldraw_folder( GNode *dnode )
{
	XYvec dims, offset;
	XYvec c0, c1;
	XYvec folder_c0, folder_c1, folder_tab;
	double k, border;

	g_assert( NODE_IS_DIR(dnode) );

	/* Obtain corners/dimensions of top face */
	dims.x = MAPV_NODE_WIDTH(dnode);
	dims.y = MAPV_NODE_DEPTH(dnode);
	k = mapv_side_slant_ratios[NODE_DIRECTORY];
	offset.x = MIN(MAPV_GEOM_PARAMS(dnode)->height, k * dims.x);
	offset.y = MIN(MAPV_GEOM_PARAMS(dnode)->height, k * dims.y);
	c0.x = MAPV_GEOM_PARAMS(dnode)->c0.x + offset.x;
	c0.y = MAPV_GEOM_PARAMS(dnode)->c0.y + offset.y;
	c1.x = MAPV_GEOM_PARAMS(dnode)->c1.x - offset.x;
	c1.y = MAPV_GEOM_PARAMS(dnode)->c1.y - offset.y;
	dims.x -= 2.0 * offset.x;
	dims.y -= 2.0 * offset.y;

	/* Folder geometry */
	border = 0.0625 * MIN(dims.x, dims.y);
	folder_c0.x = c0.x + border;
	folder_c0.y = c0.y + border;
	folder_c1.x = c1.x - border;
	folder_c1.y = c1.y - border;
	/* Coordinates of the concave vertex */
	folder_tab.x = folder_c1.x - (MAGIC_NUMBER - 1.0) * (folder_c1.x - folder_c0.x);
	folder_tab.y = folder_c1.y - border;

	VertexPos vert[] = {
		{{folder_c0.x, folder_c0.y, 0}},
		{{folder_c0.x, folder_tab.y, 0}},
		{{folder_c0.x + border, folder_c1.y, 0}},
		{{folder_tab.x - border, folder_c1.y, 0}},
		{{folder_tab.x, folder_tab.y, 0}},
		{{folder_c1.x, folder_tab.y, 0}},
		{{folder_c1.x, folder_c0.y, 0}}
	};

	drawVertexPos(GL_LINE_LOOP, vert, 7, &color_black);
}


/* Builds the children of a directory (but not the directory itself;
 * that geometry belongs to the parent) */
static void
mapv_extract_frustum_planes(mat4 mvp, vec4 planes[6])
{
	int i;
	for (i = 0; i < 4; i++) {
		planes[0][i] = mvp[i][3] + mvp[i][0];
		planes[1][i] = mvp[i][3] - mvp[i][0];
		planes[2][i] = mvp[i][3] + mvp[i][1];
		planes[3][i] = mvp[i][3] - mvp[i][1];
		planes[4][i] = mvp[i][3] + mvp[i][2];
		planes[5][i] = mvp[i][3] - mvp[i][2];
	}
}

static boolean
mapv_aabb_outside_frustum(vec4 planes[6], vec3 bbmin, vec3 bbmax)
{
	int i;
	for (i = 0; i < 6; i++) {
		float px = planes[i][0] >= 0.0f ? bbmax[0] : bbmin[0];
		float py = planes[i][1] >= 0.0f ? bbmax[1] : bbmin[1];
		float pz = planes[i][2] >= 0.0f ? bbmax[2] : bbmin[2];
		float d = planes[i][0] * px + planes[i][1] * py +
			  planes[i][2] * pz + planes[i][3];
		if (d < 0.0f)
			return TRUE;
	}
	return FALSE;
}

static boolean
mapv_aabb_inside_frustum(vec4 planes[6], vec3 bbmin, vec3 bbmax)
{
	int i;
	for (i = 0; i < 6; i++) {
		float nx = planes[i][0] >= 0.0f ? bbmin[0] : bbmax[0];
		float ny = planes[i][1] >= 0.0f ? bbmin[1] : bbmax[1];
		float nz = planes[i][2] >= 0.0f ? bbmin[2] : bbmax[2];
		float d = planes[i][0] * nx + planes[i][1] * ny +
			  planes[i][2] * nz + planes[i][3];
		if (d < 0.0f)
			return FALSE;
	}
	return TRUE;
}

static boolean
mapv_node_outside_frustum(GNode *node, vec4 planes[6], double max_height)
{
	MapVGeomParams *gp = MAPV_GEOM_PARAMS(node);
	vec3 bbmin = {(float)MIN(gp->c0.x, gp->c1.x),
		      (float)MIN(gp->c0.y, gp->c1.y), 0.0f};
	vec3 bbmax = {(float)MAX(gp->c0.x, gp->c1.x),
		      (float)MAX(gp->c0.y, gp->c1.y), (float)max_height};
	return mapv_aabb_outside_frustum(planes, bbmin, bbmax);
}

static void mapv_apply_label(GNode *node);
static void mapv_draw_recursive(GNode *dnode, int action);

static void
mapv_process_children(GNode *dnode, int action, int recurse_action)
{
	GArray *rows = mapv_draw_rows ? g_hash_table_lookup(mapv_draw_rows, dnode) : NULL;
	mat4 mvp;
	vec4 planes[6];
	guint r;

	glm_mat4_mul(gl.projection, gl.modelview, mvp);
	mapv_extract_frustum_planes(mvp, planes);
	if (rows) {
		for (r = 0; r < rows->len; r++) {
			MapVDrawRow *row = &g_array_index(rows, MapVDrawRow, r);
			vec3 bbmin = {(float)MIN(MAPV_GEOM_PARAMS(dnode)->c0.x,
						 MAPV_GEOM_PARAMS(dnode)->c1.x),
				      (float)row->y0, 0.0f};
			vec3 bbmax = {(float)MAX(MAPV_GEOM_PARAMS(dnode)->c0.x,
						 MAPV_GEOM_PARAMS(dnode)->c1.x),
				      (float)row->y1, (float)row->zmax};
			GNode *node;
			boolean row_inside;
			if (!mapv_cache_building &&
			    mapv_aabb_outside_frustum(planes, bbmin, bbmax))
				continue;
			row_inside = mapv_cache_building ||
				mapv_aabb_inside_frustum(planes, bbmin, bbmax);
			for (node = row->first; node && node != row->end; node = node->next) {
				if (action == MAPV_DRAW_SUBTREES) {
					if (NODE_IS_DIR(node))
						mapv_draw_recursive(node, recurse_action);
				} else if (mapv_cache_building || row_inside || !mapv_node_outside_frustum(node, planes,
									 MAPV_GEOM_PARAMS(node)->height)) {
					if (action == MAPV_DRAW_GEOMETRY)
						mapv_gldraw_node(node);
					else if (!NODE_IS_DIR(node))
						mapv_apply_label(node);
				}
			}
		}
		return;
	}

	/* Fallback during the first layout or if a node was inserted without
	 * rebuilding the cached row spans. */
	for (GNode *node = dnode->children; node; node = node->next) {
		if (action == MAPV_DRAW_SUBTREES) {
			if (NODE_IS_DIR(node))
				mapv_draw_recursive(node, recurse_action);
		} else if (mapv_cache_building || !mapv_node_outside_frustum(node, planes,
								 MAPV_GEOM_PARAMS(node)->height)) {
			if (action == MAPV_DRAW_GEOMETRY)
				mapv_gldraw_node(node);
			else if (!NODE_IS_DIR(node))
				mapv_apply_label(node);
		}
	}
}

static void
mapv_build_dir( GNode *dnode )
{
	g_assert( NODE_IS_DIR(dnode) || NODE_IS_METANODE(dnode) );
	mapv_process_children(dnode, MAPV_DRAW_GEOMETRY, MAPV_DRAW_GEOMETRY);
}


/* Projected-height LOD for MapV labels. Measure along the label's local Y
 * axis (the glyph height direction), as TreeV measures along its radial
 * label axis. If the segment crosses the near plane, keep it conservatively. */
static boolean
mapv_label_too_small( const XYZvec *pos, double world_height )
{
	mat4 mvp;
	double clip[2][4];
	int endpoint, k;

	if (!geometry_mapv_lod_enabled( ))
		return FALSE;

	glm_mat4_mul(gl.projection, gl.modelview, mvp);
	for (endpoint = 0; endpoint < 2; endpoint++) {
		double y = pos->y + (endpoint ? 0.5 : -0.5) * world_height;
		for (k = 0; k < 4; k++)
			clip[endpoint][k] = (double)mvp[0][k] * pos->x +
				(double)mvp[1][k] * y + (double)mvp[2][k] * pos->z +
				(double)mvp[3][k];
	}

	if (clip[0][3] <= 0.0001 && clip[1][3] <= 0.0001)
		return TRUE;
	if (clip[0][3] <= 0.0001 || clip[1][3] <= 0.0001)
		return FALSE;

	{
		double dx = clip[0][0] / clip[0][3] - clip[1][0] / clip[1][3];
		double dy = clip[0][1] / clip[0][3] - clip[1][1] / clip[1][3];
		return hypot(dx, dy) < MAPV_LABEL_MIN_NDC_SIZE;
	}
}


/* Draws a node name label */
static void
mapv_apply_label( GNode *node )
{
	XYZvec label_pos;
	XYvec dims, label_dims;
	double k;

	/* Obtain dimensions of top face */
	dims.x = MAPV_NODE_WIDTH(node);
	dims.y = MAPV_NODE_DEPTH(node);
	k = mapv_side_slant_ratios[NODE_DESC(node)->type];
	dims.x -= 2.0 * MIN(MAPV_GEOM_PARAMS(node)->height, k * dims.x);
	dims.y -= 2.0 * MIN(MAPV_GEOM_PARAMS(node)->height, k * dims.y);

	/* (Maximum) dimensions of label */
	label_dims.x = 0.8125 * dims.x;
	label_dims.y = (2.0 - MAGIC_NUMBER) * dims.y;

	/* Center position of label */
	label_pos.x = MAPV_NODE_CENTER_X(node);
	label_pos.y = MAPV_NODE_CENTER_Y(node);
	if (NODE_IS_DIR(node))
		label_pos.z = 0.0;
	else
		label_pos.z = MAPV_GEOM_PARAMS(node)->height;
	if (mapv_label_too_small(&label_pos, label_dims.y))
		return;

	text_draw_straight( NODE_DESC(node)->name, &label_pos, &label_dims );
}


/* MapV mode "full draw" */
static void
mapv_draw_recursive( GNode *dnode, int action )
{
	DirNodeDesc *dir_ndesc;
	boolean dir_collapsed;
	boolean dir_expanded;
	double saved_z_scale, saved_z_offset;

	g_assert( NODE_IS_DIR(dnode) || NODE_IS_METANODE(dnode) );

	mat4 tmpmat;
	glm_mat4_copy(gl.modelview, tmpmat);
	saved_z_scale = mapv_batch_z_scale;
	saved_z_offset = mapv_batch_z_offset;

	/* Frustum cull: if dnode's own footprint (which entirely contains
	 * all of its descendants' footprints, since they're laid out
	 * within it) can't possibly be visible right now, skip drawing it
	 * and recursing into it altogether. Deliberately does NOT bound
	 * the z-extent tightly (that would require walking the whole
	 * subtree just to find its peak height, defeating the point of
	 * culling it) -- left/right/top/bottom culling from the x/y
	 * footprint alone already covers the common "camera is looking
	 * elsewhere" case this exists for. Tested using the modelview as
	 * it stood on entry (i.e. dnode's parent's frame), matching where
	 * c0/c1 are defined.
	 * The metanode is skipped here: unlike every real directory, its
	 * c0/c1 are never actually laid out (only its height is set, in
	 * mapv_init( )), so they can't be used for a meaningful bound. */
	if (!mapv_cache_building && !NODE_IS_METANODE(dnode)) {
		MapVGeomParams *dnode_gp = MAPV_GEOM_PARAMS(dnode);
		double *subtree_peak = mapv_draw_peak_heights ?
			g_hash_table_lookup(mapv_draw_peak_heights, dnode) : NULL;
		float full_height = (float)(dnode_gp->height +
					    (subtree_peak ? *subtree_peak : 0.0));
		vec3 bbmin, bbmax;
		mat4 mvp;
		boolean culled;

		bbmin[0] = (float)MIN(dnode_gp->c0.x, dnode_gp->c1.x);
		bbmax[0] = (float)MAX(dnode_gp->c0.x, dnode_gp->c1.x);
		bbmin[1] = (float)MIN(dnode_gp->c0.y, dnode_gp->c1.y);
		bbmax[1] = (float)MAX(dnode_gp->c0.y, dnode_gp->c1.y);
		/* Descendants are stacked above this directory's top face.
		 * Bound the full tower, not only the directory block itself, so
		 * steep camera angles cannot cull visible child platforms. */
		bbmin[2] = 0.0f;
		bbmax[2] = full_height;

		glm_mat4_mul(gl.projection, tmpmat, mvp);
		culled = ogl_aabb_outside_frustum(mvp, bbmin, bbmax);

		/* Even when not fully outside the frustum, also cull if this
		 * directory's footprint would project to a negligible size on
		 * screen, or would fall entirely outside the [-1,1] NDC screen
		 * rectangle -- see MAPV_GEOMETRY_MIN_NDC_SIZE. Only applied when
		 * all four footprint corners are in front of the camera (w>0
		 * for all); a box straddling the camera is a different,
		 * close-up case this isn't meant to handle, so it's left to
		 * the frustum test above instead. */
		if (!culled) {
			/* Sample both the bottom (z=0, this directory's floor)
			 * and top (z=height, where its top face and any children
			 * actually sit) of the box -- not just the bottom. A tall
			 * directory box's footprint at its base can project to a
			 * very different (and, at a steep tilt, much thinner or
			 * off-screen) NDC region than its elevated top, which is
			 * where the visible content actually is. Testing only the
			 * base let this tiny/offscreen check wrongly cull boxes
			 * whose top (and children) were still clearly on screen. */
			float dnode_height = full_height;
			float cx[8] = { bbmin[0], bbmax[0], bbmin[0], bbmax[0],
					bbmin[0], bbmax[0], bbmin[0], bbmax[0] };
			float cy[8] = { bbmin[1], bbmin[1], bbmax[1], bbmax[1],
					bbmin[1], bbmin[1], bbmax[1], bbmax[1] };
			float cz[8] = { 0.0f, 0.0f, 0.0f, 0.0f,
					dnode_height, dnode_height, dnode_height, dnode_height };
			float ndc_x0 = 1.0e9f, ndc_x1 = -1.0e9f;
			float ndc_y0 = 1.0e9f, ndc_y1 = -1.0e9f;
			boolean all_in_front = TRUE;
			int corner;

			for (corner = 0; corner < 8; corner++) {
				vec4 p = { cx[corner], cy[corner], cz[corner], 1.0f };
				vec4 clip;

				glm_mat4_mulv(mvp, p, clip);
				if (clip[3] <= 0.0001f) {
					all_in_front = FALSE;
					break;
				}
				ndc_x0 = MIN(ndc_x0, clip[0] / clip[3]);
				ndc_x1 = MAX(ndc_x1, clip[0] / clip[3]);
				ndc_y0 = MIN(ndc_y0, clip[1] / clip[3]);
				ndc_y1 = MAX(ndc_y1, clip[1] / clip[3]);
			}

			if (all_in_front) {
				/* Genuinely tiny on screen. Checked with OR (not AND)
				 * plus an area check, not just width-and-height both
				 * being small: at a shallow/tilted viewing angle, a
				 * box can foreshorten to paper-thin in one dimension
				 * (e.g. height) while remaining wide in the other, and
				 * an AND-based test lets that slip through uncculled
				 * even though it's projecting a near-invisible sliver.
				 * The thresholds here are deliberately much smaller
				 * than MAPV_GEOMETRY_MIN_NDC_SIZE (0.02) precisely
				 * because OR triggers far more easily than AND did --
				 * reusing 0.02 here would cull much more aggressively
				 * than intended and risk cutting genuinely visible
				 * content, not just slivers. */
				float ndc_w = ndc_x1 - ndc_x0;
				float ndc_h = ndc_y1 - ndc_y0;
				boolean tiny = (ndc_w < 0.0015f) || (ndc_h < 0.0015f) ||
					       ((ndc_w * ndc_h) < 0.00001f);
				/* Entirely outside the [-1,1] NDC screen rectangle,
				 * regardless of size -- catches large-footprint
				 * directories sitting well off to the side, which the
				 * size check alone misses (a big box can have a
				 * large NDC width while still being nowhere near the
				 * viewport). */
				boolean offscreen = (ndc_x1 < -1.0f) || (ndc_x0 > 1.0f) ||
						     (ndc_y1 < -1.0f) || (ndc_y0 > 1.0f);

				if (tiny || offscreen)
					culled = TRUE;
			}
		}

		if (culled)
			return;
	}

	glm_translate(gl.modelview, (vec3){0.0f, 0.0f, MAPV_GEOM_PARAMS(dnode)->height});
	mapv_batch_z_offset += mapv_batch_z_scale * MAPV_GEOM_PARAMS(dnode)->height;

	dir_ndesc = DIR_NODE_DESC(dnode);
	dir_collapsed = DIR_COLLAPSED(dnode);
	dir_expanded = DIR_EXPANDED(dnode);

	if (!dir_collapsed && !dir_expanded) {
		/* Grow/shrink children heightwise */
		glm_scale(gl.modelview, (vec3){1.0f, 1.0f, dir_ndesc->deployment});
		mapv_batch_z_scale *= dir_ndesc->deployment;
	}

	ogl_error();
	ogl_upload_matrices(TRUE);
	ogl_error();

	if (action == MAPV_DRAW_GEOMETRY) {
		/* Draw directory face or geometry of children
		 */
		if (dir_collapsed)
			mapv_gldraw_folder(dnode);
		else
			mapv_build_dir(dnode);
	} else if (action == MAPV_DRAW_FOLDERS && dir_collapsed) {
		mapv_gldraw_folder(dnode);
	}
	ogl_error();

	if (action == MAPV_DRAW_LABELS) {
		/* Draw name label(s) */
		if (dir_collapsed)
		{
			/* Label directory */
			mapv_apply_label(dnode);
		}
		else
		{
			/* Label non-subdirectory children */
			mapv_process_children(dnode, MAPV_DRAW_LABELS, MAPV_DRAW_LABELS);
		}
	}

	/* Update geometry status */
	dir_ndesc->geom_expanded = !dir_collapsed;

	if (!dir_collapsed) {
		/* Recurse into subdirectories */
		if (action == MAPV_DRAW_FOLDERS) {
			GNode *node = dnode->children;
			while (node != NULL && NODE_IS_DIR(node)) {
				mapv_draw_recursive(node, MAPV_DRAW_FOLDERS);
				node = node->next;
			}
		} else {
			mapv_process_children(dnode, MAPV_DRAW_SUBTREES, action);
		}
	}

	glm_mat4_copy(tmpmat, gl.modelview);
	ogl_upload_matrices(FALSE);
	mapv_batch_z_scale = saved_z_scale;
	mapv_batch_z_offset = saved_z_offset;
}


/* Draws the node cursor, size/position specified by corners */
static void
mapv_gldraw_cursor( const XYZvec *c0, const XYZvec *c1 )
{
	static const double bar_part = SQR(SQR(MAGIC_NUMBER - 1.0));
	XYZvec corner_dims;
	XYZvec p, delta;
	int i, c;

	corner_dims.x = bar_part * (c1->x - c0->x);
	corner_dims.y = bar_part * (c1->y - c0->y);
	corner_dims.z = bar_part * (c1->z - c0->z);

	cursor_pre( );
	static GLuint vbo;
	if (!vbo) glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	for (i = 0; i < 2; i++) {
		if (i == 0)
			cursor_hidden_part( );
		else if (i == 1)
			cursor_visible_part( );

		for (c = 0; c < 8; c++) {
			if (c & 1) {
				p.x = c1->x;
				delta.x = - corner_dims.x;
			}
			else {
				p.x = c0->x;
				delta.x = corner_dims.x;
			}

			if (c & 2) {
				p.y = c1->y;
				delta.y = - corner_dims.y;
			}
			else {
				p.y = c0->y;
				delta.y = corner_dims.y;
			}

			if (c & 4) {
				p.z = c1->z;
				delta.z = - corner_dims.z;
			}
			else {
				p.z = c0->z;
				delta.z = corner_dims.z;
			}

			VertexPos vert[] = {
				{{p.x, p.y, p.z}}, // First line
				{{p.x + delta.x, p.y, p.z}},
				{{p.x, p.y, p.z}}, // Second
				{{p.x, p.y + delta.y, p.z}},
				{{p.x, p.y, p.z}}, // Third
				{{p.x, p.y, p.z + delta.z}}
			};

			glBufferData(GL_ARRAY_BUFFER, sizeof(vert),
				     vert, GL_STREAM_DRAW);
			glEnableVertexAttribArray(gl.position_location);
			glVertexAttribPointer(
			    gl.position_location, 3, GL_FLOAT, GL_FALSE,
			    sizeof(VertexPos),
			    (void *)offsetof(VertexPos, position));
			glDrawArrays(GL_LINES, 0, 6);
			glBufferData(GL_ARRAY_BUFFER, sizeof(vert),
				     NULL, GL_STREAM_DRAW);
		}
	}
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	cursor_post( );
}


/* Draws the node cursor in an intermediate position between its previous
 * steady-state position and the current node (pos=0 indicates the former,
 * pos=1 the latter) */
static void
mapv_draw_cursor( double pos )
{
	MapVGeomParams *gparams;
	XYZvec cursor_c0, cursor_c1;
	double z0;

	gparams = MAPV_GEOM_PARAMS(globals.current_node);
        z0 = geometry_mapv_node_z0( globals.current_node );

	/* Interpolate corners */
	cursor_c0.x = INTERPOLATE(pos, mapv_cursor_prev_c0.x, gparams->c0.x);
	cursor_c0.y = INTERPOLATE(pos, mapv_cursor_prev_c0.y, gparams->c0.y);
	cursor_c0.z = INTERPOLATE(pos, mapv_cursor_prev_c0.z, z0);
	cursor_c1.x = INTERPOLATE(pos, mapv_cursor_prev_c1.x, gparams->c1.x);
	cursor_c1.y = INTERPOLATE(pos, mapv_cursor_prev_c1.y, gparams->c1.y);
	cursor_c1.z = INTERPOLATE(pos, mapv_cursor_prev_c1.z, z0 + gparams->height);

	mapv_gldraw_cursor( &cursor_c0, &cursor_c1 );
}


/* Draws MapV geometry */
static void
mapv_draw( boolean high_detail )
{
	/* Draw low-detail geometry */

	mapv_batch_begin( );
	if (mapv_cache_using) {
		mapv_cached_instance_draw( );
		/* Folder outlines are a separate line primitive; keep their normal
		 * hierarchy traversal while reusing the cached box instances. */
		mapv_draw_recursive(globals.fstree, MAPV_DRAW_FOLDERS);
	} else {
		mapv_draw_recursive( globals.fstree, MAPV_DRAW_GEOMETRY );
		if (mapv_cache_building && mapv_instance_cnt > 0) {
			if (mapv_cached_instance_vbo == 0)
				glGenBuffers(1, &mapv_cached_instance_vbo);
			mapv_cached_instance_count = (GLsizei)mapv_instance_cnt;
			glBindBuffer(GL_ARRAY_BUFFER, mapv_cached_instance_vbo);
			MapVInstance *cached_instances = NEW_ARRAY(MapVInstance, mapv_instance_cnt);
			size_t i;
			memcpy(cached_instances, mapv_instances,
			       sizeof(MapVInstance) * mapv_instance_cnt);
			for (i = 0; i < mapv_instance_cnt; i++)
				cached_instances[i].shape[3] = fabs(cached_instances[i].shape[3]);
			glBufferData(GL_ARRAY_BUFFER,
				     sizeof(MapVInstance) * mapv_instance_cnt,
				     cached_instances, GL_STATIC_DRAW);
			xfree(cached_instances);
			glBindBuffer(GL_ARRAY_BUFFER, 0);
			mapv_cache_valid = TRUE;
			mapv_cache_dirty = FALSE;
			if (ogl_profile_enabled())
				g_print("FSV MapV overview instance cache built (%u instances)\n",
					(unsigned)mapv_cached_instance_count);
		}
		mapv_cache_building = FALSE;
		mapv_batch_flush( );
	}

	if ((gl.render_mode == RENDERMODE_RENDER) && (fstree_low_draw_stage <= 1))
		++fstree_low_draw_stage;

	if (high_detail) {
		/* Draw additional high-detail stuff */

		/* Node name labels */
		text_pre( );
		text_set_color(0.0, 0.0, 0.0); /* all labels are black */
		mapv_draw_recursive( globals.fstree, MAPV_DRAW_LABELS );
		text_post( );
		if ((gl.render_mode == RENDERMODE_RENDER) && (fstree_high_draw_stage <= 1))
			++fstree_high_draw_stage;

		/* Node cursor */
		mapv_draw_cursor( CURSOR_POS(camera->pan_part) );
	}
}


/**** TREE VISUALIZATION **************************************/


/* Geometry constants */
#define TREEV_MIN_ARC_WIDTH		90.0
#define TREEV_MAX_ARC_WIDTH		225.0
#define TREEV_BRANCH_WIDTH		256.0
#define TREEV_MIN_CORE_RADIUS		8192.0
#define TREEV_CORE_GROW_FACTOR		1.25
#define TREEV_CURVE_GRANULARITY		5.0
#define TREEV_PLATFORM_HEIGHT		158.2
#define TREEV_PLATFORM_SPACING_WIDTH	512.0
/* Leaf height scales with size via cube root, not sqrt() (the original
 * approach) or log() (tried and rejected -- it compressed the huge
 * range real directories span so much that a 700MB, a 3GB, and a 19GB
 * directory ended up almost the same height). sqrt() differentiates
 * well but makes even a single large file dwarf its own footprint
 * (TREEV_LEAF_NODE_EDGE) many times over; cube root is a middle ground,
 * still clearly showing size differences across orders of magnitude
 * while growing slowly enough that TREEV_LEAF_MAX_HEIGHT only needs to
 * step in for genuinely extreme outliers. TREEV_LEAF_MIN_HEIGHT exists
 * because an empty or near-empty directory would otherwise come out
 * almost paper-flat -- thin enough that its collapsed "X" marker (see
 * treev_gldraw_leaf( )), which is a fixed size regardless of the box's
 * own height, visibly sticks out above/through it. */
#define TREEV_LEAF_HEIGHT_MULTIPLIER	1.0
#define TREEV_LEAF_MIN_HEIGHT		(0.25 * TREEV_LEAF_NODE_EDGE)
#define TREEV_LEAF_MAX_HEIGHT		(16.0 * TREEV_LEAF_NODE_EDGE)
#define TREEV_LEAF_PADDING		(0.125 * TREEV_LEAF_NODE_EDGE)
#define TREEV_PLATFORM_PADDING		(0.5 * TREEV_PLATFORM_SPACING_WIDTH)

/* Extra flags for TreeV mode */
enum {
	TREEV_NEED_REARRANGE	= 1 << 0
};

/* Messages for treev_draw_recursive( ) */
enum {
	/* Note: don't change order of these */
	TREEV_DRAW_LABELS,
	TREEV_DRAW_GEOMETRY,
	TREEV_DRAW_GEOMETRY_WITH_BRANCHES
};


/* Color of interconnecting branches */
static RGBcolor branch_color = { 0.5, 0.0, 0.0 };

/* Label colors for platform and leaf nodes */
static RGBcolor treev_platform_label_color = { 1.0, 1.0, 1.0 };
static RGBcolor treev_leaf_label_color = { 0.0, 0.0, 0.0 };

/* Point buffers used in drawing curved geometry */
static XYvec *inner_edge_buf = NULL;
static XYvec *outer_edge_buf = NULL;

/* Radius of innermost loop */
static double treev_core_radius;

/* Previous steady-state positions of the cursor corners */
static RTZvec treev_cursor_prev_c0;
static RTZvec treev_cursor_prev_c1;


/* Checks if a node is currently a leaf (i.e. a collapsed directory or some
 * other node), or not (an expanded directory) according to the directory
 * tree */
boolean
geometry_treev_is_leaf( GNode *node )
{
	if (NODE_IS_DIR(node)) {
		if (dirtree_entry_expanded( node ))
			return FALSE;
		/* The GTK tree-widget's expanded flag and the 3D deployment
		 * value (which actually drives rendering -- see the various
		 * DIR_NODE_DESC(node)->deployment uses throughout this file)
		 * are two separate pieces of state that can end up out of
		 * sync, e.g. after a bulk "expand all" on a directory whose
		 * tree-widget row didn't exist yet at the time (GTK's tree
		 * view populates rows lazily). When that happens, trust
		 * deployment -- it's what's actually drawn on screen, so
		 * treating the node as a leaf here would make camera
		 * targeting (and anything else using this function) disagree
		 * with what the user can see */
		if (DIR_NODE_DESC(node)->deployment > (1.0 - EPSILON))
			return FALSE;
	}

	return TRUE;
}


/* Returns the nearest ancestor of node (possibly node itself) that
 * currently has a platform of its own -- i.e. walks up past any leaf
 * (collapsed directory, or non-directory) ancestors. Callers that need
 * "the platform a leaf sits on" have historically just used node->parent
 * directly, which is normally correct (a leaf's immediate parent must be
 * expanded for the leaf to be visible/reachable at all) but can be wrong
 * if an ancestor further up gets collapsed while a deeper node is still
 * considered current/selected/clicked -- in that case node->parent may
 * itself now be a leaf too, and code using it directly would trip the
 * assertion in geometry_treev_platform_theta( ) */
GNode *
geometry_treev_platform_node( GNode *node )
{
	GNode *pnode;

	pnode = geometry_treev_is_leaf( node ) ? node->parent : node;
	while (geometry_treev_is_leaf( pnode ) && !NODE_IS_METANODE(pnode))
		pnode = pnode->parent;

	return pnode;
}


/* Returns the inner radius of a directory platform */
double
geometry_treev_platform_r0( GNode *dnode )
{
	GNode *up_node;
	double r0 = 0.0;

	if (NODE_IS_METANODE(dnode))
		return treev_core_radius;

	up_node = dnode->parent;
	while (up_node != NULL) {
		r0 += TREEV_PLATFORM_SPACING_DEPTH;
		r0 += TREEV_GEOM_PARAMS(up_node)->platform.depth;
		up_node = up_node->parent;
	}
	r0 += treev_core_radius;

	return r0;
}


/* Returns the absolute angular position of a directory platform (which
 * is referenced along the platform's radial centerline) */
double
geometry_treev_platform_theta( GNode *dnode )
{
	GNode *up_node;
	double theta = 0.0;

	g_assert( !geometry_treev_is_leaf( dnode ) || NODE_IS_METANODE(dnode) );

	up_node = dnode;
	while (up_node != NULL) {
		theta += TREEV_GEOM_PARAMS(up_node)->platform.theta;
		up_node = up_node->parent;
	}

	return theta;
}


/* This returns the height of the tallest leaf on the given directory
 * platform. Height does not include that of the platform itself */
double
geometry_treev_max_leaf_height( GNode *dnode )
{
	GNode *node;
	double max_height = 0.0;

	g_assert( !geometry_treev_is_leaf( dnode ) );

	node = dnode->children;
	while (node != NULL) {
		if (geometry_treev_is_leaf( node ))
			max_height = MAX(max_height, TREEV_GEOM_PARAMS(node)->leaf.height);
		node = node->next;
	}

	return max_height;
}


/* Helper function for treev_get_extents( ) */
static void
treev_get_extents_recursive( GNode *dnode, RTvec *c0, RTvec *c1, double r0, double theta )
{
	GNode *node;
	double subtree_r0;

	g_assert( NODE_IS_DIR(dnode) );

	subtree_r0 = r0 + TREEV_GEOM_PARAMS(dnode)->platform.depth + TREEV_PLATFORM_SPACING_DEPTH;
	node = dnode->children;
	while (node != NULL) {
		if (!geometry_treev_is_leaf( node ))
			treev_get_extents_recursive( node, c0, c1, subtree_r0, theta + TREEV_GEOM_PARAMS(node)->platform.theta );
/* TODO: try putting this check at top of loop */
		if (!NODE_IS_DIR(node))
			break;
		node = node->next;
	}

	c0->r = MIN(c0->r, r0);
	c0->theta = MIN(c0->theta, theta - TREEV_GEOM_PARAMS(dnode)->platform.arc_width);
	c1->r = MAX(c1->r, r0 + TREEV_GEOM_PARAMS(dnode)->platform.depth);
	c1->theta = MAX(c1->theta, theta + TREEV_GEOM_PARAMS(dnode)->platform.arc_width);
}


/* This returns the 2D corners of the entire subtree rooted at the given
 * directory platform, including the subtree root. (Note that the extents
 * returned depend on the current expansion state) */
void
geometry_treev_get_extents( GNode *dnode, RTvec *ext_c0, RTvec *ext_c1 )
{
	RTvec c0, c1;

	g_assert( !geometry_treev_is_leaf( dnode ) );

	c0.r = DBL_MAX;
	c0.theta = DBL_MAX;
	c1.r = DBL_MIN;
	c1.theta = DBL_MIN;

	treev_get_extents_recursive( dnode, &c0, &c1, geometry_treev_platform_r0( dnode ), geometry_treev_platform_theta( dnode ) );

	if (ext_c0 != NULL)
		*ext_c0 = c0; /* struct assign */
	if (ext_c1 != NULL)
		*ext_c1 = c1; /* struct assign */
}


/* Returns the corners (min/max RTZ points) of the given leaf node or
 * directory platform in absolute polar coordinates, with some padding
 * added on all sides for a not-too-tight fit */
static void
treev_get_corners( GNode *node, RTZvec *c0, RTZvec *c1 )
{
	RTZvec pos;
	double leaf_arc_width;
	double padding_arc_width;

	if (geometry_treev_is_leaf( node )) {
		GNode *platform_node = geometry_treev_platform_node( node );

		/* Absolute position of center of leaf node bottom */
		pos.r = geometry_treev_platform_r0( platform_node ) + TREEV_GEOM_PARAMS(node)->leaf.distance;
		pos.theta = geometry_treev_platform_theta( platform_node ) + TREEV_GEOM_PARAMS(node)->leaf.theta;
		pos.z = TREEV_GEOM_PARAMS(platform_node)->platform.height;

		/* Calculate corners of leaf node */
		leaf_arc_width = (180.0 * TREEV_LEAF_NODE_EDGE / PI) / pos.r;
		c0->r = pos.r - (0.5 * TREEV_LEAF_NODE_EDGE);
		c0->theta = pos.theta - 0.5 * leaf_arc_width;
		c0->z = pos.z;
		c1->r = pos.r + (0.5 * TREEV_LEAF_NODE_EDGE);
		c1->theta = pos.theta + 0.5 * leaf_arc_width;
		c1->z = pos.z + TREEV_GEOM_PARAMS(node)->leaf.height;

		/* Push corners outward a bit */
		padding_arc_width = (180.0 * TREEV_LEAF_PADDING / PI) / pos.r;
		c0->r -= TREEV_LEAF_PADDING;
		c0->theta -= padding_arc_width;
		c0->z -= (0.5 * TREEV_LEAF_PADDING);
		c1->r += TREEV_LEAF_PADDING;
		c1->theta += padding_arc_width;
		c1->z += (0.5 * TREEV_LEAF_PADDING);
	}
	else {
		/* Position of center of inner edge of platform */
		pos.r = geometry_treev_platform_r0( node );
		pos.theta = geometry_treev_platform_theta( node );

		/* Calculate corners of platform region */
		c0->r = pos.r;
		c0->theta = pos.theta - 0.5 * TREEV_GEOM_PARAMS(node)->platform.arc_width;
		c0->z = 0.0;
		c1->r = pos.r + TREEV_GEOM_PARAMS(node)->platform.depth;
		c1->theta = pos.theta + 0.5 * TREEV_GEOM_PARAMS(node)->platform.arc_width;
		c1->z = TREEV_GEOM_PARAMS(node)->platform.height;

		/* Push corners outward a bit. Because the sides already
		 * encompass the platform spacing regions, there is no need
                 * to add extra padding there */
		c0->r -= TREEV_PLATFORM_PADDING;
		c1->r += TREEV_PLATFORM_PADDING;
	}
}


/* This assigns an arc width and depth to a directory platform.
 * Note: depth value is only an estimate; the final value can only be
 * determined by actually laying down leaf nodes */
static void
treev_reshape_platform( GNode *dnode, double r0 )
{
#define edge05 (0.5 * TREEV_LEAF_NODE_EDGE)
#define edge15 (1.5 * TREEV_LEAF_NODE_EDGE)
	static const double w = TREEV_PLATFORM_SPACING_WIDTH;
	static const double w_2 = SQR(TREEV_PLATFORM_SPACING_WIDTH);
	static const double w_3 = SQR(TREEV_PLATFORM_SPACING_WIDTH) * TREEV_PLATFORM_SPACING_WIDTH;
	static const double w_4 = SQR(TREEV_PLATFORM_SPACING_WIDTH) * SQR(TREEV_PLATFORM_SPACING_WIDTH);
	double area;
	double A, A_2, A_3, r, r_2, r_3, r_4, ka, kb, kc, kd, d, theta;
	double depth, arc_width, min_arc_width;
	double k;
	int n;

	/* Estimated area, based on number of (immediate) children */
	n = g_list_length( (GList *)dnode->children );
	k = edge15 * ceil( sqrt( (double)MAX(1, n) ) ) + edge05;
	area = SQR(k);

	/* Known: Area and inner radius of directory, plus the fact that
	 * the aspect ratio (length_of_outer_edge / depth) is exactly 1.
	 * Unknown: depth and arc width of directory.
	 * Raw and distilled equations:
	 * { A ~= PI*theta/360*((r + d)^2 - r^2) - w*d,
	 * s ~= PI*theta*(r + d)/180 - w,
	 * s/d = 1  -->  s = d,
	 * theta = 180*(d + w)/(PI*(r + d)),
	 * d^3 + (2*r + w)*d^2 + (2*w*r - 2*A - w)*d - 2*A*r = 0,
	 * A = area, w = TREEV_PLATFORM_SPACING_WIDTH, r = r0,
	 * s = (length of outer edge), d = depth, theta = arc_width }
	 * Solution: Thank god for Maple */
	A = area;
	A_2 = SQR(A);
	A_3 = A*A_2;
	r = r0;
	r_2 = SQR(r);
	r_3 = r*r_2;
	r_4 = SQR(r_2);
	ka = 72.0*(A*r - w*(A + r)) - 64.0*r_3 + 48.0*r_2*w - 36.0*w_2 + 24.0*r*w_2 - 8.0*w_3;
#define T1 72.0*A*w_2 - 132.0*A*r*w_2 - 240.0*A*w*r_3 + 120.0*A*w_2*r_2 - 24.0*A_2*w*r - 60.0*w_3*r
#define T2 12.0*(w_2*r_2 + A_2*w_2 - w_4*r + w_4*r_2 + A*w_3 + w_3)
#define T3 48.0*(w_2*r_4 - w_2*r_3 - w_3*r_3) + 96.0*(A_3 + w_3*r_2)
#define T4 192.0*A*r_4 + 156.0*A_2*r_2 + 3.0*w_4 + 144.0*A_2*w + 264.0*A*w*r_2
	kb = 12.0*sqrt( T1 + T2 + T3 + T4 );
#undef T1
#undef T2
#undef T3
#undef T4
	kc = cos( atan2( kb, ka ) / 3.0 );
	kd = cbrt( hypot( ka, kb ) );
	/* Bring it all together */
	d = (- w - 2.0*r)/3.0 + ((8.0*r_2 - 4.0*w*r + 2.0*w_2)/3.0 + 4.0*A + 2.0*w)*kc/kd + kc*kd/6.0;
	theta = 180.0*(d + w)/(PI*(r + d));

	depth = d;
	arc_width = theta;

	/* Adjust depth upward to accomodate an integral number of rows */
	depth += (edge15 - fmod( depth - edge05, edge15 )) + edge05;

	/* Final arc width must be at least large enough to yield an
	 * inner edge length that is two leaf node edges long */
	min_arc_width = (180.0 * (2.0 * TREEV_LEAF_NODE_EDGE + TREEV_PLATFORM_SPACING_WIDTH) / PI) / r0;

	TREEV_GEOM_PARAMS(dnode)->platform.arc_width = MAX(min_arc_width, arc_width);
	TREEV_GEOM_PARAMS(dnode)->platform.depth = depth;

	/* Directory will need rebuilding, obviously */
	geometry_queue_rebuild( dnode );

#undef edge05
#undef edge15
}


/* Pure (side-effect free) helper -- computes the "official" depth that
 * treev_build_dir( ) would end up assigning to a directory with
 * n_children immediate children, the given arc_width, and inner radius
 * r0, by replicating its row-layout recurrence exactly (same formulas,
 * same order of operations) without touching any GNode, drawing
 * anything, or writing to platform.depth. Currently used only for
 * diagnostic comparison against treev_reshape_platform( )'s estimate
 * (see FSV_DEBUG_ARRANGE below) -- NOT YET wired into any actual
 * decision; this is deliberately step 1 of a multi-step plan (see
 * backlog) after the 2026-08-31 crash from doing this and wiring it up
 * in one shot. Capped at a generous iteration count as a safety net:
 * if arc_width is ever pathologically small, a row can accommodate 0
 * (or negative) nodes, and the real treev_build_dir( ) recurrence
 * would only terminate once pos_r grows enough for arc_len to turn
 * positive again -- which happens for any arc_width > 0, but
 * potentially only after a huge number of iterations. */
static double
treev_compute_exact_depth( int n_children, double arc_width, double r0 )
{
#define edge05 (0.5 * TREEV_LEAF_NODE_EDGE)
#define edge15 (1.5 * TREEV_LEAF_NODE_EDGE)
#define TREEV_EXACT_DEPTH_MAX_ITERATIONS 10000
	double pos_r;
	double arc_len;
	int row_node_count;
	int remaining_node_count;
	int iterations = 0;

	remaining_node_count = n_children;
	pos_r = r0 + TREEV_LEAF_NODE_EDGE;
	while (remaining_node_count > 0) {
		if (++iterations > TREEV_EXACT_DEPTH_MAX_ITERATIONS) {
			g_warning("treev_compute_exact_depth( ): did not converge after "
				  "%d iterations (n_children=%d, arc_width=%.6g, r0=%.6g) "
				  "-- bailing out with a possibly-wrong result",
				  TREEV_EXACT_DEPTH_MAX_ITERATIONS, n_children, arc_width, r0);
			break;
		}
		/* Same recurrence as treev_build_dir( ) below, minus the actual
		 * leaf placement/drawing side effects */
		arc_len = (PI / 180.0) * pos_r * arc_width - TREEV_PLATFORM_SPACING_WIDTH;
		row_node_count = (int)floor( (arc_len - edge05) / edge15 );
		remaining_node_count -= row_node_count;
		pos_r += edge15;
	}
	pos_r -= edge05;

	return pos_r - r0;
#undef edge05
#undef edge15
#undef TREEV_EXACT_DEPTH_MAX_ITERATIONS
}


/* Helper function for treev_arrange( ). @reshape_tree flag should be TRUE
 * if platform radiuses have changed (thus requiring reshaping).
 *
 * Returns this subtree's outermost radius and stores it on the node as
 * platform.subtree_max_depth (offset from r0). Draw-side culling reads
 * that cache; it must not walk the subtree again.
 *
 * Approximations: early-return uses the last stored cache (correct once
 * a full arrange has run); depth is treev_reshape_platform( )'s estimate
 * until a visible treev_build_dir( ) overwrites it with the exact row
 * layout. The AABB is therefore slightly conservative/stale, which is
 * the right direction for culling. */
static double
treev_arrange_recursive( GNode *dnode, double r0, boolean reshape_tree )
{
	GNode *node;
	double subtree_r0;
	double arc_width, subtree_arc_width = 0.0;
	double theta;
	double subtree_max_r;
	/* Temporary diagnostic for the arc_width=0 investigation (see
	 * backlog) -- enable with FSV_DEBUG_ARRANGE=1 in the environment.
	 * Not gated behind #ifdef DEBUG so it can be flipped on in a
	 * release build without a rebuild-with-different-flags detour;
	 * the getenv( ) is cached so the steady-state cost is one branch
	 * per call when disabled. Remove once the root cause is found. */
	static int treev_debug_arrange = -1;
	if (treev_debug_arrange < 0)
		treev_debug_arrange = (g_getenv("FSV_DEBUG_ARRANGE") != NULL);

	g_assert( NODE_IS_DIR(dnode) || NODE_IS_METANODE(dnode) );

	if (treev_debug_arrange) {
		TreeVGeomParams *dbg_gp = TREEV_GEOM_PARAMS(dnode);
		g_print("treev_arrange_recursive: dnode=%p %s r0=%.6g reshape_tree=%d "
			"arc_width=%.6g depth=%.6g need_rearrange=%d\n",
			(void *)dnode,
			NODE_IS_METANODE(dnode) ? "<meta>" : NODE_DESC(dnode)->name,
			r0, (int)reshape_tree, dbg_gp->platform.arc_width, dbg_gp->platform.depth,
			(NODE_DESC(dnode)->flags & TREEV_NEED_REARRANGE) ? 1 : 0);
	}

	if (!reshape_tree && !(NODE_DESC(dnode)->flags & TREEV_NEED_REARRANGE))
		return r0 + TREEV_GEOM_PARAMS(dnode)->platform.subtree_max_depth;

	if (reshape_tree && NODE_IS_DIR(dnode)) {
		if (geometry_treev_is_leaf(dnode)) {
			/* Ensure directory leaf gets repositioned */
			geometry_queue_rebuild( dnode );
			return r0;
		}
		else {
			/* Reshape directory platform */
			treev_reshape_platform( dnode, r0 );
			if (treev_debug_arrange) {
				TreeVGeomParams *dbg_gp2 = TREEV_GEOM_PARAMS(dnode);
				int n_children = g_list_length( (GList *)dnode->children );
				double exact_depth = treev_compute_exact_depth( n_children, dbg_gp2->platform.arc_width, r0 );
				g_print("  -> after reshape: arc_width=%.6g depth(estimate)=%.6g "
					"depth(exact, n=%d)=%.6g\n",
					dbg_gp2->platform.arc_width, dbg_gp2->platform.depth,
					n_children, exact_depth);
			}
		}
	}

	/* This node's own contribution, before considering any children */
	subtree_max_r = r0 + TREEV_GEOM_PARAMS(dnode)->platform.depth;

	/* Recurse into expanded subdirectories, and obtain the overall
	 * arc width of the subtree */
	subtree_r0 = r0 + TREEV_GEOM_PARAMS(dnode)->platform.depth + TREEV_PLATFORM_SPACING_DEPTH;
	node = dnode->children;
	while (node != NULL) {
		if (!NODE_IS_DIR(node))
			break;
		{
			double child_max_r = treev_arrange_recursive( node, subtree_r0, reshape_tree );
			subtree_max_r = MAX(subtree_max_r, child_max_r);
		}
		arc_width = DIR_NODE_DESC(node)->deployment * MAX(TREEV_GEOM_PARAMS(node)->platform.arc_width, TREEV_GEOM_PARAMS(node)->platform.subtree_arc_width);
		TREEV_GEOM_PARAMS(node)->platform.theta = arc_width; /* temporary value */
		subtree_arc_width += arc_width;
		node = node->next;
	}
	TREEV_GEOM_PARAMS(dnode)->platform.subtree_arc_width = subtree_arc_width;

	/* Spread the subdirectories, sweeping counterclockwise */
	theta = -0.5 * subtree_arc_width;
	node = dnode->children;
	while (node != NULL) {
                if (!NODE_IS_DIR(node))
			break;
		arc_width = TREEV_GEOM_PARAMS(node)->platform.theta;
		TREEV_GEOM_PARAMS(node)->platform.theta = theta + 0.5 * arc_width;
		theta += arc_width;
		node = node->next;
	}

	if (treev_debug_arrange) {
		g_print("  -> subtree_max_r for %s: %.6g (own outer edge %.6g)\n",
			NODE_IS_METANODE(dnode) ? "<meta>" : NODE_DESC(dnode)->name,
			subtree_max_r, r0 + TREEV_GEOM_PARAMS(dnode)->platform.depth);
	}

	/* Persist for treev_draw_recursive( )'s culling test -- stored as an
	 * offset from r0 (see the field's comment in geometry.h) so it stays
	 * valid even if this exact r0 doesn't recur (e.g. read via a stale
	 * early-return above after an ancestor's depth shifts things). */
	TREEV_GEOM_PARAMS(dnode)->platform.subtree_max_depth = subtree_max_r - r0;

	/* Clear the "need rearrange" flag */
	NODE_DESC(dnode)->flags &= ~TREEV_NEED_REARRANGE;

	return subtree_max_r;
}


/* Top-level call to arrange the branches of the currently expanded tree,
 * as needed when directories collapse/expand (initial_arrange == FALSE),
 * or when tree is initially created (initial_arrange == TRUE) */
static void
treev_arrange( boolean initial_arrange )
{
	/* Safety net for the retry loop below: growing/shrinking
	 * treev_core_radius is only ever supposed to take a handful of
	 * iterations to settle. But it has a known failure mode -- seen
	 * 2026-08-31 during an unrelated culling experiment -- where a
	 * numerical bug upstream (e.g. subtree_arc_width coming out as
	 * -nan) can make the > / < comparisons below never resolve to
	 * "within bounds", so the loop grows core_radius forever (it
	 * reached ~10^27 before the process had to be killed). Bailing
	 * out after a generous-but-finite number of iterations turns
	 * that into a visible warning instead of a silent hang, without
	 * changing behavior in the normal case. */
#define TREEV_ARRANGE_MAX_ITERATIONS	64
	boolean resized = FALSE;
	int iterations = 0;

	treev_arrange_recursive( globals.fstree, treev_core_radius, initial_arrange );

	/* Check that the tree's total arc width is within bounds */
	for (;;) {
		if (++iterations > TREEV_ARRANGE_MAX_ITERATIONS) {
			g_warning("treev_arrange( ): core-radius retry loop did not "
				  "converge after %d iterations (subtree_arc_width=%g, "
				  "core_radius=%g) -- bailing out to avoid an infinite loop",
				  TREEV_ARRANGE_MAX_ITERATIONS,
				  TREEV_GEOM_PARAMS(globals.fstree)->platform.subtree_arc_width,
				  treev_core_radius);
			break;
		}
		if (TREEV_GEOM_PARAMS(globals.fstree)->platform.subtree_arc_width > TREEV_MAX_ARC_WIDTH) {
			/* Grow core radius */
			treev_core_radius *= TREEV_CORE_GROW_FACTOR;
			treev_arrange_recursive( globals.fstree, treev_core_radius, TRUE );
			resized = TRUE;
		}
		else if ((TREEV_GEOM_PARAMS(globals.fstree)->platform.subtree_arc_width < TREEV_MIN_ARC_WIDTH) && (TREEV_GEOM_PARAMS(globals.fstree)->platform.depth > TREEV_MIN_CORE_RADIUS)) {
			/* Shrink core radius */
			treev_core_radius = MAX(TREEV_MIN_CORE_RADIUS, treev_core_radius / TREEV_CORE_GROW_FACTOR);
			treev_arrange_recursive( globals.fstree, treev_core_radius, TRUE );
			resized = TRUE;
		}
		else
			break;
	}
#undef TREEV_ARRANGE_MAX_ITERATIONS

	if (resized && camera_moving( ) && !camera_treev_follow_active( )) {
		/* Camera's destination has moved, so it will need a
		 * flight path correction */
		camera_pan_break( );
		camera_look_at_full( globals.current_node, MORPH_INV_QUADRATIC, -1.0 );
	}
}


/* Helper function for treev_init( ) */
static void
treev_init_recursive( GNode *dnode )
{
	GNode *node;
	int64 size;

	g_assert( NODE_IS_DIR(dnode) || NODE_IS_METANODE(dnode) );

	if (NODE_IS_DIR(dnode)) {
		morph_break( &DIR_NODE_DESC(dnode)->deployment );
		if (dirtree_entry_expanded( dnode ) ||
		    (!dirtree_entry_has_subdir( dnode ) && (DIR_NODE_DESC(dnode)->deployment > (1.0 - EPSILON))))
			DIR_NODE_DESC(dnode)->deployment = 1.0;
		else
			DIR_NODE_DESC(dnode)->deployment = 0.0;
		geometry_queue_rebuild( dnode );
		/* Initialize cache for child count. */
		DIR_NODE_DESC(dnode)->child_count = g_node_n_children(dnode);
	}

	NODE_DESC(dnode)->flags = 0;

	/* Assign heights to leaf nodes */
	node = dnode->children;
	while (node != NULL) {
		size = MAX(64, NODE_DESC(node)->size);
		if (NODE_IS_DIR(node)) {
			size += DIR_NODE_DESC(node)->subtree.size;
			TREEV_GEOM_PARAMS(node)->platform.height = TREEV_PLATFORM_HEIGHT;
			TREEV_GEOM_PARAMS(node)->platform.arc_width = TREEV_MIN_ARC_WIDTH;
			TREEV_GEOM_PARAMS(node)->platform.subtree_arc_width = TREEV_MIN_ARC_WIDTH;
			/* Not a real estimate -- just a defined, recognizably
			 * "not yet built" placeholder until treev_reshape_platform( )
			 * lays down an actual value the first time this directory is
			 * expanded. Without this, an unexpanded directory's depth is
			 * whatever was in the freshly-allocated GEOM_PARAMS memory --
			 * observed 2026-09-03 via FSV_DEBUG_ARRANGE as denormalized
			 * garbage (e.g. 6.95262e-310), not a clean 0.0. A real depth
			 * from treev_reshape_platform( ) is always > edge05 + edge15,
			 * so 0.0 can never be mistaken for a genuine value. */
			TREEV_GEOM_PARAMS(node)->platform.depth = 0.0;
			TREEV_GEOM_PARAMS(node)->platform.subtree_max_depth = 0.0;
			treev_init_recursive( node );
		}
		TREEV_GEOM_PARAMS(node)->leaf.height = CLAMP(
			TREEV_LEAF_HEIGHT_MULTIPLIER * cbrt( (double)size ),
			TREEV_LEAF_MIN_HEIGHT, TREEV_LEAF_MAX_HEIGHT);
		node = node->next;
	}
}


/* Top-level call to initialize TreeV mode */
static void
treev_init( void )
{
	TreeVGeomParams *gparams;
	int num_points;

	/* Allocate point buffers */
	num_points = (int)ceil( 360.0 / TREEV_CURVE_GRANULARITY ) + 1;
	if (inner_edge_buf == NULL)
		inner_edge_buf = NEW_ARRAY(XYvec, num_points);
	if (outer_edge_buf == NULL)
		outer_edge_buf = NEW_ARRAY(XYvec, num_points);

	treev_core_radius = TREEV_MIN_CORE_RADIUS;

	gparams = TREEV_GEOM_PARAMS(globals.fstree);
	gparams->platform.theta = 90.0;
	gparams->platform.depth = 0.0;
	gparams->platform.subtree_max_depth = 0.0;
	gparams->platform.arc_width = TREEV_MAX_ARC_WIDTH;
	gparams->platform.height = 0.0;

	gparams = TREEV_GEOM_PARAMS(root_dnode);
	gparams->leaf.theta = 0.0;
	gparams->leaf.distance = (0.5 * TREEV_PLATFORM_SPACING_DEPTH);
	gparams->platform.theta = 0.0;

	treev_init_recursive( globals.fstree );
	treev_arrange( TRUE );
	treev_cache_invalidate( );

	/* Initial cursor state */
	treev_get_corners( root_dnode, &treev_cursor_prev_c0, &treev_cursor_prev_c1 );
	treev_cursor_prev_c0.r *= 0.875;
	treev_cursor_prev_c0.theta -= TREEV_GEOM_PARAMS(root_dnode)->platform.arc_width;
        treev_cursor_prev_c0.z = 0.0;
	treev_cursor_prev_c1.r *= 1.125;
	treev_cursor_prev_c1.theta += TREEV_GEOM_PARAMS(root_dnode)->platform.arc_width;
	treev_cursor_prev_c1.z = TREEV_GEOM_PARAMS(root_dnode)->platform.height;
}


/* Hook function for camera pan completion */
static void
treev_camera_pan_finished( void )
{
	/* Save cursor position */
	treev_get_corners( globals.current_node, &treev_cursor_prev_c0, &treev_cursor_prev_c1 );
}


/* Called by a directory as it collapses or expands (from the morph's step
 * callback; see colexp.c). This sets all the necessary flags to allow the
 * directory to move side to side without problems */
static void
treev_queue_rearrange( GNode *dnode )
{
	GNode *up_node;

	g_assert( NODE_IS_DIR(dnode) );

	up_node = dnode;
	while (up_node != NULL) {
		NODE_DESC(up_node)->flags |= TREEV_NEED_REARRANGE;
		up_node = up_node->parent;
	}

	queue_uncached_draw( );
}


/* Batching for TreeV leaf geometry, mirroring mapv_batch_* above (see its
 * comment for the full rationale: per-node draw calls made frame time
 * scale badly with visible node count). SELECT batches encode each leaf's
 * node ID into its vertex color, retaining accurate picking while reducing
 * per-leaf draw calls.
 *
 * A full leaf (top face + 4 side faces) is 20 vertices/30 indices --
 * the same count as a MapV node, purely by coincidence of geometry --
 * so the same GLushort-index-range math applies. */
#define TREEV_BATCH_MAX_NODES	3276	/* 3276*20 verts stays under 65536, the GLushort index limit */
static ColorVertex *treev_batch_verts = NULL;
static GLushort *treev_batch_idx = NULL;
static size_t treev_batch_vert_cnt = 0;
static size_t treev_batch_idx_cnt = 0;
static GLuint treev_batch_vbo, treev_batch_ebo;

/* gl.modelview as it stood right before the traversal filling the batch
 * began -- see mapv_batch_root_modelview above for the full rationale.
 * In TreeV, what accumulates in gl.modelview while descending is a
 * rotate_z chain (each directory platform's own angular offset), plus,
 * for a directory that's mid-expand/collapse, a scale/rotate/translate
 * sequence around its leaf position -- all of which treev_batch_add_leaf( )
 * bakes out per-node relative to this same shared root frame. */
static mat4 treev_batch_root_modelview;
static mat4 treev_batch_root_modelview_inv;
static mat4 treev_batch_cached_modelview;
static mat4 treev_batch_cached_local_transform;
static mat3 treev_batch_cached_normal_transform;
static boolean treev_batch_transform_cache_valid;

typedef struct {
	GLuint vbo, ebo;
	GLsizei index_count;
} TreeVBatchChunk;
typedef struct {
	double r0;
	double arc_width;
	double platform_depth;
	int child_count;
	GNode *first_child;
} TreeVLayoutCacheEntry;
static GArray *treev_cached_chunks;
static size_t treev_cached_chunk_count;
static boolean treev_cache_valid;
static boolean treev_cache_dirty = TRUE;
static boolean treev_cache_using;
static double treev_cache_dirty_since;
/* In the stable overview, the leaf mesh is already in a VBO, but the normal
 * recursive path still walks every row and recomputes every leaf's polar
 * position on each frame. Keep the row-layout result alongside the geometry
 * cache so overview frames can skip that O(number of leaves) CPU work too. */
static GHashTable *treev_layout_cache;

static void
treev_cache_invalidate(void)
{
	treev_cache_valid = FALSE;
	treev_cache_dirty = TRUE;
	treev_cache_dirty_since = xgettime();
	if (treev_layout_cache != NULL)
		g_hash_table_remove_all(treev_layout_cache);
}

static TreeVLayoutCacheEntry *
treev_layout_cache_lookup(GNode *dnode, double r0)
{
	TreeVLayoutCacheEntry *entry;
	if (treev_layout_cache == NULL)
		return NULL;
	entry = g_hash_table_lookup(treev_layout_cache, dnode);
	if (entry == NULL || entry->r0 != r0 ||
	    entry->arc_width != TREEV_GEOM_PARAMS(dnode)->platform.arc_width ||
	    entry->child_count != DIR_NODE_DESC(dnode)->child_count ||
	    entry->first_child != dnode->children)
		return NULL;
	return entry;
}

static void
treev_layout_cache_store(GNode *dnode, double r0)
{
	TreeVLayoutCacheEntry *entry;
	if (treev_layout_cache == NULL)
		treev_layout_cache = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
	entry = g_new(TreeVLayoutCacheEntry, 1);
	entry->r0 = r0;
	entry->arc_width = TREEV_GEOM_PARAMS(dnode)->platform.arc_width;
	entry->platform_depth = TREEV_GEOM_PARAMS(dnode)->platform.depth;
	entry->child_count = DIR_NODE_DESC(dnode)->child_count;
	entry->first_child = dnode->children;
	g_hash_table_replace(treev_layout_cache, dnode, entry);
}

/* The all-leaf cache is used only for overview framing, where per-leaf
 * screen culling rejects little or nothing. Close views keep the existing
 * culling path so off-screen geometry does not burden the GPU. */
static boolean
treev_cache_overview(void)
{
	double extent;
	if (camera == NULL || root_dnode == NULL)
		return FALSE;
	extent = treev_core_radius + TREEV_GEOM_PARAMS(root_dnode)->platform.subtree_max_depth;
	return extent > 0.0 &&
		camera->distance * tan(RAD(0.5 * camera->fov)) >= 0.8 * extent;
}

/* Call once per frame, before any treev_gldraw_leaf( ) calls, to reset
 * the batch (allocating its backing storage on first use) */
static void
treev_batch_begin( void )
{
	if (treev_batch_verts == NULL) {
		treev_batch_verts = NEW_ARRAY(ColorVertex, TREEV_BATCH_MAX_NODES * 20);
		treev_batch_idx = NEW_ARRAY(GLushort, TREEV_BATCH_MAX_NODES * 30);
	}
	treev_batch_vert_cnt = 0;
	treev_batch_idx_cnt = 0;
	treev_batch_transform_cache_valid = FALSE;
	treev_cache_using = treev_cache_valid && !treev_cache_dirty && treev_cache_overview();
	treev_cache_building = !treev_cache_using && treev_cache_dirty &&
		treev_cache_overview() && (xgettime() - treev_cache_dirty_since) >= 0.25;
	if (treev_cache_building) {
		if (treev_cached_chunks == NULL)
			treev_cached_chunks = g_array_new(FALSE, TRUE, sizeof(TreeVBatchChunk));
		treev_cached_chunk_count = 0;
	}
	glm_mat4_copy(gl.modelview, treev_batch_root_modelview);
	glm_mat4_inv(treev_batch_root_modelview, treev_batch_root_modelview_inv);
}

static void
treev_batch_draw_buffers(GLuint vbo, GLuint ebo, GLsizei index_count)
{
	mat4 root_mvp;
	mat3 root_normal_matrix;
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
	glEnableVertexAttribArray(gl.position_location);
	glVertexAttribPointer(gl.position_location, 3, GL_FLOAT, GL_FALSE,
			      sizeof(ColorVertex), (void *)offsetof(ColorVertex, position));
	glEnableVertexAttribArray(gl.normal_location);
	glVertexAttribPointer(gl.normal_location, 3, GL_FLOAT, GL_FALSE,
			      sizeof(ColorVertex), (void *)offsetof(ColorVertex, normal));
	glEnableVertexAttribArray(gl.vcolor_location);
	glVertexAttribPointer(gl.vcolor_location, 3, GL_FLOAT, GL_FALSE,
			      sizeof(ColorVertex), (void *)offsetof(ColorVertex, color));
	glEnableVertexAttribArray(gl.node_id_location);
	glVertexAttribPointer(gl.node_id_location, 1, GL_FLOAT, GL_FALSE,
			      sizeof(ColorVertex), (void *)offsetof(ColorVertex, node_id));

	glm_mat4_mul(gl.projection, treev_batch_root_modelview, root_mvp);
	glm_mat4_pick3(treev_batch_root_modelview, root_normal_matrix);
	glm_mat3_inv(root_normal_matrix, root_normal_matrix);
	glm_mat3_transpose(root_normal_matrix);
	glUseProgram(gl.program);
	glUniformMatrix4fv(gl.modelview_location, 1, GL_FALSE, (float *)treev_batch_root_modelview);
	glUniformMatrix3fv(gl.normal_matrix_location, 1, GL_FALSE, (float *)root_normal_matrix);
	glUniformMatrix4fv(gl.mvp_location, 1, GL_FALSE, (float *)root_mvp);
	glUniform1i(gl.lightning_enabled_location, gl.render_mode == RENDERMODE_RENDER);
	glUniform1i(gl.use_vertex_color_location, 1);
	glUniform1i(gl.use_node_id_location, 1);
	glUniform1i(gl.selection_mode_location, gl.render_mode == RENDERMODE_SELECT);
	glUniform1f(gl.highlighted_node_id_location, (GLfloat)highlight_node_id);
	glDrawElements(GL_TRIANGLES, index_count, GL_UNSIGNED_SHORT, 0);
	glUniform1i(gl.use_vertex_color_location, 0);
	glUniform1i(gl.use_node_id_location, 0);
	glUniform1i(gl.selection_mode_location, 0);
	glUseProgram(0);
	ogl_upload_matrices(FALSE);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

static void
treev_batch_flush( void )
{
	GLuint vbo, ebo;
	if (treev_batch_vert_cnt == 0)
		return;
	if (treev_cache_building) {
		TreeVBatchChunk chunk;
		if (treev_cached_chunk_count == treev_cached_chunks->len) {
			memset(&chunk, 0, sizeof(chunk));
			g_array_append_val(treev_cached_chunks, chunk);
		}
		chunk = g_array_index(treev_cached_chunks, TreeVBatchChunk, treev_cached_chunk_count);
		if (!chunk.vbo) {
			glGenBuffers(1, &chunk.vbo);
			glGenBuffers(1, &chunk.ebo);
		}
		chunk.index_count = (GLsizei)treev_batch_idx_cnt;
		g_array_index(treev_cached_chunks, TreeVBatchChunk, treev_cached_chunk_count) = chunk;
		vbo = chunk.vbo;
		ebo = chunk.ebo;
		glBindBuffer(GL_ARRAY_BUFFER, vbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(ColorVertex) * treev_batch_vert_cnt,
			     treev_batch_verts, GL_STATIC_DRAW);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(GLushort) * treev_batch_idx_cnt,
			     treev_batch_idx, GL_STATIC_DRAW);
		treev_cached_chunk_count++;
	} else {
		if (!treev_batch_vbo) {
			glGenBuffers(1, &treev_batch_vbo);
			glGenBuffers(1, &treev_batch_ebo);
		}
		vbo = treev_batch_vbo;
		ebo = treev_batch_ebo;
		glBindBuffer(GL_ARRAY_BUFFER, vbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(ColorVertex) * treev_batch_vert_cnt,
			     treev_batch_verts, GL_STREAM_DRAW);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(GLushort) * treev_batch_idx_cnt,
			     treev_batch_idx, GL_STREAM_DRAW);
	}
	treev_batch_draw_buffers(vbo, ebo, (GLsizei)treev_batch_idx_cnt);
	if (!treev_cache_building) {
		glBindBuffer(GL_ARRAY_BUFFER, vbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(ColorVertex) * treev_batch_vert_cnt,
			     NULL, GL_STREAM_DRAW);
	}
	treev_batch_vert_cnt = 0;
	treev_batch_idx_cnt = 0;
}

static void
treev_cached_draw_all(void)
{
	size_t i;
	for (i = 0; i < treev_cached_chunk_count; i++) {
		TreeVBatchChunk *chunk = &g_array_index(treev_cached_chunks, TreeVBatchChunk, i);
		treev_batch_draw_buffers(chunk->vbo, chunk->ebo, chunk->index_count);
	}
}

/* Appends a leaf's top face (always) and side faces (only if full_node)
 * to the batch, with base color and node ID stored per vertex,
 * flushing first if there isn't room left. corners[] are already in
 * the leaf's own micro-rotated local frame (see treev_gldraw_leaf( )'s
 * own corner rotation by leaf.theta) -- what's left to bake in here is
 * just this node's slice of gl.modelview relative to the shared root
 * frame, exactly as in mapv_batch_add_node( ). sin_theta/cos_theta are
 * passed through (rather than re-derived from corners) since
 * treev_gldraw_leaf( ) already has them for the side-face normals. */
static void
treev_batch_add_leaf( GNode *node, XYvec *corners, double z0, double z1,
		       double sin_theta, double cos_theta, boolean full_node )
{
	GLfloat color[3];
	size_t base;
	int i;

	if ((treev_batch_vert_cnt + 20) > (size_t)(TREEV_BATCH_MAX_NODES * 20))
		treev_batch_flush( );

	memcpy(color, NODE_DESC(node)->color, 3 * sizeof(GLfloat));

	/* Every leaf sibling in one directory shares the same modelview. Cache
	 * its root-relative transform and normal matrix instead of multiplying
	 * and inverting them for every file in that directory. */
	if (!treev_batch_transform_cache_valid ||
	    memcmp(treev_batch_cached_modelview, gl.modelview,
		   sizeof(treev_batch_cached_modelview)) != 0) {
		glm_mat4_copy(gl.modelview, treev_batch_cached_modelview);
		glm_mat4_mul(treev_batch_root_modelview_inv, gl.modelview,
			     treev_batch_cached_local_transform);
		glm_mat4_pick3(treev_batch_cached_local_transform,
			       treev_batch_cached_normal_transform);
		glm_mat3_inv(treev_batch_cached_normal_transform,
			     treev_batch_cached_normal_transform);
		glm_mat3_transpose(treev_batch_cached_normal_transform);
		treev_batch_transform_cache_valid = TRUE;
	}

#define CV(px, py, pz, nx, ny, nz) do { \
	vec4 _p = { (float)(px), (float)(py), (float)(pz), 1.0f }; \
	vec4 _tp; \
	vec3 _n = { (float)(nx), (float)(ny), (float)(nz) }; \
	vec3 _tn; \
	glm_mat4_mulv(treev_batch_cached_local_transform, _p, _tp); \
	glm_mat3_mulv(treev_batch_cached_normal_transform, _n, _tn); \
	 treev_batch_verts[treev_batch_vert_cnt++] = (ColorVertex){{_tp[0], _tp[1], _tp[2]}, {_tn[0], _tn[1], _tn[2]}, {color[0], color[1], color[2]}, (GLfloat)NODE_DESC(node)->id}; \
} while (0)

	base = treev_batch_vert_cnt;

	/* Top face -- same strip order (0,1,3,2) as the immediate-mode path,
	 * split here into 2 indexed triangles instead */
	CV(corners[0].x, corners[0].y, z1, 0.0, 0.0, 1.0);
	CV(corners[1].x, corners[1].y, z1, 0.0, 0.0, 1.0);
	CV(corners[3].x, corners[3].y, z1, 0.0, 0.0, 1.0);
	CV(corners[2].x, corners[2].y, z1, 0.0, 0.0, 1.0);
	treev_batch_idx[treev_batch_idx_cnt++] = (GLushort)(base + 0);
	treev_batch_idx[treev_batch_idx_cnt++] = (GLushort)(base + 1);
	treev_batch_idx[treev_batch_idx_cnt++] = (GLushort)(base + 2);
	treev_batch_idx[treev_batch_idx_cnt++] = (GLushort)(base + 2);
	treev_batch_idx[treev_batch_idx_cnt++] = (GLushort)(base + 1);
	treev_batch_idx[treev_batch_idx_cnt++] = (GLushort)(base + 3);

	if (full_node) {
		static const GLushort side_elems[] = {
			0,  1,	2,  2,	1,  3,
			4,  5,	6,  6,	5,  7,
			8,  9,	10, 10, 9,  11,
			12, 13, 14, 14, 13, 15
		};
		size_t base2 = treev_batch_vert_cnt;

		/* Front */
		CV(corners[0].x, corners[0].y, z1, sin_theta, -cos_theta, 0.0);
		CV(corners[0].x, corners[0].y, z0, sin_theta, -cos_theta, 0.0);
		CV(corners[1].x, corners[1].y, z1, sin_theta, -cos_theta, 0.0);
		CV(corners[1].x, corners[1].y, z0, sin_theta, -cos_theta, 0.0);
		/* Right */
		CV(corners[1].x, corners[1].y, z1, cos_theta, sin_theta, 0.0);
		CV(corners[1].x, corners[1].y, z0, cos_theta, sin_theta, 0.0);
		CV(corners[2].x, corners[2].y, z1, cos_theta, sin_theta, 0.0);
		CV(corners[2].x, corners[2].y, z0, cos_theta, sin_theta, 0.0);
		/* Back */
		CV(corners[2].x, corners[2].y, z1, -sin_theta, cos_theta, 0.0);
		CV(corners[2].x, corners[2].y, z0, -sin_theta, cos_theta, 0.0);
		CV(corners[3].x, corners[3].y, z1, -sin_theta, cos_theta, 0.0);
		CV(corners[3].x, corners[3].y, z0, -sin_theta, cos_theta, 0.0);
		/* Left */
		CV(corners[3].x, corners[3].y, z1, -cos_theta, -sin_theta, 0.0);
		CV(corners[3].x, corners[3].y, z0, -cos_theta, -sin_theta, 0.0);
		CV(corners[0].x, corners[0].y, z1, -cos_theta, -sin_theta, 0.0);
		CV(corners[0].x, corners[0].y, z0, -cos_theta, -sin_theta, 0.0);

		for (i = 0; i < 24; i++)
			treev_batch_idx[treev_batch_idx_cnt++] = (GLushort)(base2 + side_elems[i]);
	}

#undef CV
}


/* Draws a directory platform, with inner radius of r0 */
static void
treev_gldraw_platform( GNode *dnode, double r0 )
{
	XYvec p0, p1;
	XYvec delta;
	double r1, seg_arc_width;
	double theta, sin_theta, cos_theta;
	double z1;
	int s, seg_count;

	g_assert( NODE_IS_DIR(dnode) );
	if (gl.render_mode == RENDERMODE_SELECT && !treev_cache_building)
		treev_batch_flush( );

	r1 = r0 + TREEV_GEOM_PARAMS(dnode)->platform.depth;
	seg_count = (int)ceil( TREEV_GEOM_PARAMS(dnode)->platform.arc_width / TREEV_CURVE_GRANULARITY );
	seg_arc_width = TREEV_GEOM_PARAMS(dnode)->platform.arc_width / (double)seg_count;

	/* Calculate and cache inner/outer edge vertices */
	theta = -0.5 * TREEV_GEOM_PARAMS(dnode)->platform.arc_width;
	for (s = 0; s <= seg_count; s++) {
		sin_theta = sin( RAD(theta) );
		cos_theta = cos( RAD(theta) );
		/* p0: point on inner edge */
		p0.x = r0 * cos_theta;
		p0.y = r0 * sin_theta;
		/* p1: point on outer edge */
		p1.x = r1 * cos_theta;
		p1.y = r1 * sin_theta;
		if (s == 0) {
			/* Leading edge offset */
			delta.x = - sin_theta * (0.5 * TREEV_PLATFORM_SPACING_WIDTH);
			delta.y = cos_theta * (0.5 * TREEV_PLATFORM_SPACING_WIDTH);
			p0.x += delta.x;
			p0.y += delta.y;
			p1.x += delta.x;
			p1.y += delta.y;
		}
		else if (s == seg_count) {
			/* Trailing edge offset */
			delta.x = sin_theta * (0.5 * TREEV_PLATFORM_SPACING_WIDTH);
			delta.y = - cos_theta * (0.5 * TREEV_PLATFORM_SPACING_WIDTH);
			p0.x += delta.x;
			p0.y += delta.y;
			p1.x += delta.x;
			p1.y += delta.y;
		}

		/* cache */
		inner_edge_buf[s].x = p0.x;
		inner_edge_buf[s].y = p0.y;
		outer_edge_buf[s].x = p1.x;
		outer_edge_buf[s].y = p1.y;

		theta += seg_arc_width;
	}

	/* Height of top face */
        z1 = TREEV_GEOM_PARAMS(dnode)->platform.height;

	size_t vert_cnt = seg_count * (8 + 4) + 8;
	size_t idx_len = 0;
	Vertex *vert = NEW_ARRAY(Vertex, vert_cnt);
	GLushort *idx = NEW_ARRAY(GLushort, vert_cnt * 2);

	/* Draw inner edge */
	for (s = 0; s < seg_count; s++) {
		/* Going up */
		p0.x = inner_edge_buf[s].x;
		p0.y = inner_edge_buf[s].y;
		vert[s * 4] = (Vertex){{p0.x, p0.y, 0}, {-p0.x / r0, -p0.y / r0, 0}};
		vert[s * 4 + 1] = (Vertex){{p0.x, p0.y, z1}, {-p0.x / r0, -p0.y / r0, 0}};

		/* Going down */
		p0.x = inner_edge_buf[s + 1].x;
		p0.y = inner_edge_buf[s + 1].y;
		vert[s * 4 + 2] = (Vertex){{p0.x, p0.y, z1}, {-p0.x / r0, -p0.y / r0, 0}};
		vert[s * 4 + 3] = (Vertex){{p0.x, p0.y, 0}, {-p0.x / r0, -p0.y / r0, 0}};
		idx[idx_len++] = s * 4;
		idx[idx_len++] = s * 4 + 1;
		idx[idx_len++] = s * 4 + 2;
		idx[idx_len++] = s * 4;
		idx[idx_len++] = s * 4 + 2;
		idx[idx_len++] = s * 4 + 3;
	}

	/* Draw outer edge */
	for (s = seg_count; s > 0; s--) {
		/* Going up */
		size_t s2 = (seg_count + seg_count - s) * 4;
		p1.x = outer_edge_buf[s].x;
		p1.y = outer_edge_buf[s].y;
		vert[s2] = (Vertex){{p1.x, p1.y, 0}, {-p1.x / r1, -p1.y / r1, 0}};
		vert[s2 + 1] = (Vertex){{p1.x, p1.y, z1}, {-p1.x / r1, -p1.y / r1, 0}};

		/* Going down */
		p1.x = outer_edge_buf[s - 1].x;
		p1.y = outer_edge_buf[s - 1].y;
		vert[s2 + 2] = (Vertex){{p1.x, p1.y, z1}, {-p1.x / r1, -p1.y / r1, 0}};
		vert[s2 + 3] = (Vertex){{p1.x, p1.y, 0}, {-p1.x / r1, -p1.y / r1, 0}};
		idx[idx_len++] = s2;
		idx[idx_len++] = s2 + 1;
		idx[idx_len++] = s2 + 2;
		idx[idx_len++] = s2;
		idx[idx_len++] = s2 + 2;
		idx[idx_len++] = s2 + 3;
	}

	/* Draw leading edge face */
	p0.x = inner_edge_buf[0].x;
	p0.y = inner_edge_buf[0].y;
	p1.x = outer_edge_buf[0].x;
	p1.y = outer_edge_buf[0].y;
	size_t s2 = seg_count * 2 * 4;
	vert[s2] = (Vertex){{p0.x, p0.y, 0}, {p0.y / r0, -p0.x / r0, 0}};
	vert[s2 + 1] = (Vertex){{p1.x, p1.y, 0}, {p0.y / r0, -p0.x / r0, 0}};
	vert[s2 + 2] = (Vertex){{p1.x, p1.y, z1}, {p0.y / r0, -p0.x / r0, 0}};
	vert[s2 + 3] = (Vertex){{p0.x, p0.y, z1}, {p0.y / r0, -p0.x / r0, 0}};
	idx[idx_len++] = s2;
	idx[idx_len++] = s2 + 1;
	idx[idx_len++] = s2 + 2;
	idx[idx_len++] = s2;
	idx[idx_len++] = s2 + 2;
	idx[idx_len++] = s2 + 3;
	s2 += 4;

	/* Draw trailing edge face */
	p0.x = inner_edge_buf[seg_count].x;
	p0.y = inner_edge_buf[seg_count].y;
	p1.x = outer_edge_buf[seg_count].x;
	p1.y = outer_edge_buf[seg_count].y;
	vert[s2] = (Vertex){{p0.x, p0.y, z1}, {-p0.y / r0, p0.x / r0, 0}};
	vert[s2 + 1] = (Vertex){{p1.x, p1.y, z1}, {-p0.y / r0, p0.x / r0, 0}};
	vert[s2 + 2] = (Vertex){{p1.x, p1.y, 0}, {-p0.y / r0, p0.x / r0, 0}};
	vert[s2 + 3] = (Vertex){{p0.x, p0.y, 0}, {-p0.y / r0, p0.x / r0, 0}};
	idx[idx_len++] = s2;
	idx[idx_len++] = s2 + 1;
	idx[idx_len++] = s2 + 2;
	idx[idx_len++] = s2;
	idx[idx_len++] = s2 + 2;
	idx[idx_len++] = s2 + 3;
	s2 += 4;

	/* Draw top face */
	for (s = 0; s < seg_count; s++) {
		/* Going out */
		size_t s3 = s2 + s * 4;
		p0.x = inner_edge_buf[s].x;
		p0.y = inner_edge_buf[s].y;
		p1.x = outer_edge_buf[s].x;
		p1.y = outer_edge_buf[s].y;
		vert[s3] = (Vertex){{p0.x, p0.y, z1}, {0, 0, 1}};
		vert[s3 + 1] = (Vertex){{p1.x, p1.y, z1}, {0, 0, 1}};


		/* Going in */
		p0.x = inner_edge_buf[s + 1].x;
		p0.y = inner_edge_buf[s + 1].y;
		p1.x = outer_edge_buf[s + 1].x;
		p1.y = outer_edge_buf[s + 1].y;
		vert[s3 + 2] = (Vertex){{p1.x, p1.y, z1}, {0, 0, 1}};
		vert[s3 + 3] = (Vertex){{p0.x, p0.y, z1}, {0, 0, 1}};
		idx[idx_len++] = s3;
		idx[idx_len++] = s3 + 1;
		idx[idx_len++] = s3 + 2;
		idx[idx_len++] = s3;
		idx[idx_len++] = s3 + 2;
		idx[idx_len++] = s3 + 3;
	}

	g_assert(s2 + (seg_count - 1) * 4 + 3 < vert_cnt);
	g_assert(idx_len <= vert_cnt * 2);

	static GLuint vbo;
	if (!vbo)
		glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(Vertex) * vert_cnt, vert, GL_DYNAMIC_DRAW);

	static GLuint ebo;
	if (!ebo)
		glGenBuffers(1, &ebo);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(GLushort) * idx_len, idx, GL_DYNAMIC_DRAW);

	glEnableVertexAttribArray(gl.position_location);
	glVertexAttribPointer(gl.position_location, 3, GL_FLOAT, GL_FALSE,
			      sizeof(Vertex), (void *)offsetof(Vertex, position));

	glEnableVertexAttribArray(gl.normal_location);
	glVertexAttribPointer(gl.normal_location, 3, GL_FLOAT, GL_FALSE,
			      sizeof(Vertex), (void *)offsetof(Vertex, normal));

	glUseProgram(gl.program);

	node_set_color(dnode);

	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
	glDrawElements(GL_TRIANGLES, idx_len, GL_UNSIGNED_SHORT, 0);

	glUseProgram(0);
	glBufferData(GL_ARRAY_BUFFER, sizeof(Vertex) * vert_cnt, NULL, GL_DYNAMIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

	xfree(vert);
	xfree(idx);
}


/* Draws a leaf node. r0 is inner radius of parent; full_node flag
 * specifies whether the full leaf body should be drawn (TRUE) or merely
 * its "footprint" (FALSE). Note: Transformation matrix should be the same
 * one used to draw the underlying parent directory */
static void
treev_gldraw_leaf( GNode *node, double r0, boolean full_node )
{
	static const int x_verts[] = { 0, 2, 1, 3 };
	XYvec corners[4], p;
	double z0, z1;
	double edge, height;
	double sin_theta, cos_theta;
	int i;
	if (treev_cache_using && full_node)
		return;

	if (full_node) {
		edge = TREEV_LEAF_NODE_EDGE;
		height = TREEV_GEOM_PARAMS(node)->leaf.height;
		if (NODE_IS_DIR(node))
			height *= (1.0 - DIR_NODE_DESC(node)->deployment);
	}
	else {
		edge = (0.875 * TREEV_LEAF_NODE_EDGE);
		height = (TREEV_LEAF_NODE_EDGE / 64.0);
	}

	/* Set up corners, centered around (r0+distance,0,0) */

	/* Left/front */
	corners[0].x = r0 + TREEV_GEOM_PARAMS(node)->leaf.distance - 0.5 * edge;
	corners[0].y = -0.5 * edge;

	/* Right/front */
	corners[1].x = corners[0].x + edge;
	corners[1].y = corners[0].y;

	/* Right/rear */
	corners[2].x = corners[1].x;
	corners[2].y = corners[0].y + edge;

	/* Left/rear */
	corners[3].x = corners[0].x;
	corners[3].y = corners[2].y;

	/* Bottom and top */
	z0 = TREEV_GEOM_PARAMS(node->parent)->platform.height;
	z1 = z0 + height;

	sin_theta = sin( RAD(TREEV_GEOM_PARAMS(node)->leaf.theta) );
	cos_theta = cos( RAD(TREEV_GEOM_PARAMS(node)->leaf.theta) );

	/* Rotate corners into position (no glRotated( )-- leaf nodes are
	 * not important enough to mess with the transformation matrix) */
	for (i = 0; i < 4; i++) {
		p.x = corners[i].x;
		p.y = corners[i].y;
		corners[i].x = p.x * cos_theta - p.y * sin_theta;
		corners[i].y = p.x * sin_theta + p.y * cos_theta;
	}

	/* Batch both visible rendering and selection. In SELECT mode each
	 * vertex carries this node's encoded ID color. */
	if (gl.render_mode == RENDERMODE_RENDER || gl.render_mode == RENDERMODE_SELECT)
		treev_batch_add_leaf( node, corners, z0, z1, sin_theta, cos_theta, full_node );

	if (!full_node) {
		/* Draw an "X" and we're done */
		VertexPos vertx[4];
		for (i = 0; i < 4; i++)
			vertx[i] = (VertexPos){{corners[x_verts[i]].x, corners[x_verts[i]].y, z1}};
		if (gl.render_mode == RENDERMODE_SELECT)
			treev_batch_flush( );
		drawVertexPos(GL_LINES, vertx, 4, &color_black);
		return;
	}

	if (gl.render_mode == RENDERMODE_RENDER || gl.render_mode == RENDERMODE_SELECT)
		return; /* side faces already appended by treev_batch_add_leaf( ) above */

	/* Draw side faces */
	Vertex vside[] = {
	    // Front face
	    {{corners[0].x, corners[0].y, z1}, {sin_theta, -cos_theta, 0}},
	    {{corners[0].x, corners[0].y, z0}, {sin_theta, -cos_theta, 0}},
	    {{corners[1].x, corners[1].y, z1}, {sin_theta, -cos_theta, 0}},
	    {{corners[1].x, corners[1].y, z0}, {sin_theta, -cos_theta, 0}},
	    // Right
	    {{corners[1].x, corners[1].y, z1}, {cos_theta, sin_theta, 0}},
	    {{corners[1].x, corners[1].y, z0}, {cos_theta, sin_theta, 0}},
	    {{corners[2].x, corners[2].y, z1}, {cos_theta, sin_theta, 0}},
	    {{corners[2].x, corners[2].y, z0}, {cos_theta, sin_theta, 0}},
	    // Back
	    {{corners[2].x, corners[2].y, z1}, {-sin_theta, cos_theta, 0}},
	    {{corners[2].x, corners[2].y, z0}, {-sin_theta, cos_theta, 0}},
	    {{corners[3].x, corners[3].y, z1}, {-sin_theta, cos_theta, 0}},
	    {{corners[3].x, corners[3].y, z0}, {-sin_theta, cos_theta, 0}},
	    // Left
	    {{corners[3].x, corners[3].y, z1}, {-cos_theta, -sin_theta, 0}},
	    {{corners[3].x, corners[3].y, z0}, {-cos_theta, -sin_theta, 0}},
	    {{corners[0].x, corners[0].y, z1}, {-cos_theta, -sin_theta, 0}},
	    {{corners[0].x, corners[0].y, z0}, {-cos_theta, -sin_theta, 0}},
	};
	static const GLushort elems[] = {
	    0,	1,  2,	2,  1,	3,   // Front
	    4,	5,  6,	6,  5,	7,   // Right
	    8,	9,  10, 10, 9,	11,  // Back
	    12, 13, 14, 14, 13, 15   // Left
	};
	static GLuint vbo;
	if (!vbo)
		glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vside), &vside, GL_DYNAMIC_DRAW);

	static GLuint ebo;
	if (!ebo) {
		glGenBuffers(1, &ebo);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(elems), &elems, GL_STATIC_DRAW);
	}

	glEnableVertexAttribArray(gl.position_location);
	glVertexAttribPointer(gl.position_location, 3, GL_FLOAT, GL_FALSE,
			      sizeof(Vertex), (void *)offsetof(Vertex, position));

	glEnableVertexAttribArray(gl.normal_location);
	glVertexAttribPointer(gl.normal_location, 3, GL_FLOAT, GL_FALSE,
			      sizeof(Vertex), (void *)offsetof(Vertex, normal));

	glUseProgram(gl.program);

	node_set_color(node);

	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
	GLsizei cnt = sizeof(elems) / sizeof(GLushort);
	glDrawElements(GL_TRIANGLES, cnt, GL_UNSIGNED_SHORT, 0);

	glUseProgram(0);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vside), NULL, GL_DYNAMIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}


/* Draws a "folder" shape on top of the given directory leaf node */
static void
treev_gldraw_folder( GNode *dnode, double r0 )
{
#define X1 (-0.4375 * TREEV_LEAF_NODE_EDGE)
#define X2 (0.375 * TREEV_LEAF_NODE_EDGE)
#define X3 (0.4375 * TREEV_LEAF_NODE_EDGE)
#define Y1 (-0.4375 * TREEV_LEAF_NODE_EDGE)
#define Y2 (Y1 + (2.0 - MAGIC_NUMBER) * TREEV_LEAF_NODE_EDGE)
#define Y3 (Y2 + 0.0625 * TREEV_LEAF_NODE_EDGE)
#define Y4 (Y5 - 0.0625 * TREEV_LEAF_NODE_EDGE)
#define Y5 (0.4375 * TREEV_LEAF_NODE_EDGE)
	static const XYvec folder_points[] = {
		{ X1, Y1 },
		{ X2, Y1 },
		{ X2, Y2 },
		{ X3, Y3 },
		{ X3, Y4 },
		{ X2, Y5 },
		{ X1, Y5 }
	};
#undef X1
#undef X2
#undef X3
#undef Y1
#undef Y2
#undef Y3
#undef Y4
#undef Y5
	XYZvec p_rot;
	XYvec p;
	double folder_r;
	double sin_theta, cos_theta;
	int i;

	g_assert( NODE_IS_DIR(dnode) );

	folder_r = r0 + TREEV_GEOM_PARAMS(dnode)->leaf.distance;
	sin_theta = sin( RAD(TREEV_GEOM_PARAMS(dnode)->leaf.theta) );
	cos_theta = cos( RAD(TREEV_GEOM_PARAMS(dnode)->leaf.theta) );
	p_rot.z = (1.0 - DIR_NODE_DESC(dnode)->deployment) * TREEV_GEOM_PARAMS(dnode)->leaf.height + TREEV_GEOM_PARAMS(dnode->parent)->platform.height;

	/* Translate, rotate, and draw folder geometry */
	VertexPos vert[8];
	for (i = 0; i <= 7; i++) {
		p.x = folder_r + folder_points[i % 7].x;
		p.y = folder_points[i % 7].y;
		p_rot.x = p.x * cos_theta - p.y * sin_theta;
		p_rot.y = p.x * sin_theta + p.y * cos_theta;

		vert[i] = (VertexPos){{p_rot.x, p_rot.y, p_rot.z}};
	}

	drawVertexPos(GL_LINE_STRIP, vert, 8, &color_black);
}


/* Draws the loop around the TreeV center, with the given radius */
static void
treev_gldraw_loop( double loop_r )
{
	static const int seg_count = (int)(360.0 / TREEV_CURVE_GRANULARITY + 0.5);
	XYvec p0, p1;
	double loop_r0, loop_r1;
	double theta, sin_theta, cos_theta;
	int s;

	/* Inner/outer loop radii */
	loop_r0 = loop_r - (0.5 * TREEV_BRANCH_WIDTH);
	loop_r1 = loop_r + (0.5 * TREEV_BRANCH_WIDTH);

	/* Draw loop */
	static size_t vert_cnt = (seg_count + 1) * 2;
	Vertex *vert = NEW_ARRAY(Vertex, vert_cnt);
	for (s = 0; s <= seg_count; s++) {
		theta = 360.0 * (double)s / (double)seg_count;
		sin_theta = sin( RAD(theta) );
		cos_theta = cos( RAD(theta) );
		/* p0: point on inner edge */
		p0.x = loop_r0 * cos_theta;
		p0.y = loop_r0 * sin_theta;
		/* p1: point on outer edge */
		p1.x = loop_r1 * cos_theta;
		p1.y = loop_r1 * sin_theta;

		vert[2 * s] = (Vertex){{p0.x, p0.y, 0}, {0, 0, 1}};
		vert[2 * s + 1] = (Vertex){{p1.x, p1.y, 0}, {0, 0, 1}};
	}
	drawVertex(GL_TRIANGLE_STRIP, vert, vert_cnt, &branch_color, NULL);
	xfree(vert);
}


/* Draws part of the branch connecting to the inner edge of a platform.
 * r0 is the platform's inner radius */
static void
treev_gldraw_inbranch( double r0 )
{
	XYvec c0, c1;

	/* Left/front */
	c0.x = r0 - (0.5 * TREEV_PLATFORM_SPACING_DEPTH);
	c0.y = (-0.5 * TREEV_BRANCH_WIDTH);

	/* Right/rear */
	c1.x = r0;
	c1.y = (0.5 * TREEV_BRANCH_WIDTH);

	Vertex vert[] = {
		{{c0.x, c0.y, 0}, {0, 0, 1}},
		{{c1.x, c0.y, 0}, {0, 0, 1}},
		{{c0.x, c1.y, 0}, {0, 0, 1}},
		{{c1.x, c1.y, 0}, {0, 0, 1}},
	};
	drawVertex(GL_TRIANGLE_STRIP, vert, 4, &branch_color, NULL);
}


/* Draws part of the branch present on the outer edge of platforms with
 * expanded subdirectories. r1 is the outer radius of the parent directory,
 * and theta0/theta1 are the start/end angles of the arc portion */
static void
treev_gldraw_outbranch( double r1, double theta0, double theta1 )
{
	XYvec p0, p1;
	double arc_r, arc_r0, arc_r1;
	double arc_width, seg_arc_width;
	double supp_arc_width;
	double theta, sin_theta, cos_theta;
	int s, seg_count;

	g_assert( theta1 >= theta0 );

	/* Radii of branch arc (middle, inner, outer) */
	arc_r = r1 + (0.5 * TREEV_PLATFORM_SPACING_DEPTH);
	arc_r0 = arc_r - (0.5 * TREEV_BRANCH_WIDTH);
	arc_r1 = arc_r + (0.5 * TREEV_BRANCH_WIDTH);

	/* Left/front of stem */
	p0.x = r1;
	p0.y = (-0.5 * TREEV_BRANCH_WIDTH);

	/* Right/rear of stem */
	p1.x = arc_r;
	p1.y = (0.5 * TREEV_BRANCH_WIDTH);

	arc_width = theta1 - theta0;

	/* Supplemental arc width, to yield fully square branch corners
	 * (where directories connect to the ends of the arc) */
	supp_arc_width = (180.0 * TREEV_BRANCH_WIDTH / PI) / arc_r0;

	seg_count = (int)ceil( (arc_width + supp_arc_width) / TREEV_CURVE_GRANULARITY );
	seg_arc_width = (arc_width + supp_arc_width) / (double)seg_count;

	const size_t vert_cnt = 4 + (seg_count + 1) * 2;
	Vertex *vert = NEW_ARRAY(Vertex, vert_cnt);
	/* Branch stem */
	vert[0] = (Vertex){{p0.x, p0.y, 0}, {0, 0, 1}};
	vert[1] = (Vertex){{p1.x, p0.y, 0}, {0, 0, 1}};
	vert[3] = (Vertex){{p1.x, p1.y, 0}, {0, 0, 1}};
	vert[2] = (Vertex){{p0.x, p1.y, 0}, {0, 0, 1}};

	/* Draw branch arc */
	theta = theta0 - 0.5 * supp_arc_width;
	for (s = 0; s <= seg_count; s++) {
		sin_theta = sin( RAD(theta) );
		cos_theta = cos( RAD(theta) );
		/* p0: point on inner edge */
		p0.x = arc_r0 * cos_theta;
		p0.y = arc_r0 * sin_theta;
		/* p1: point on outer edge */
		p1.x = arc_r1 * cos_theta;
		p1.y = arc_r1 * sin_theta;

		vert[4 + s * 2] = (Vertex){{p0.x, p0.y, 0}, {0, 0, 1}};
		vert[4 + s * 2 + 1] = (Vertex){{p1.x, p1.y, 0}, {0, 0, 1}};

		theta += seg_arc_width;
	}
	drawVertex(GL_TRIANGLE_STRIP, vert, vert_cnt, &branch_color, NULL);
	xfree(vert);
}


/* Per-leaf visibility test used by treev_build_dir( ) (added 2026-09-17).
 *
 * The wedge cull in treev_draw_recursive( ) can only skip a whole subtree.
 * Within a visible directory every child still had its geometry submitted
 * unconditionally, so a directory with thousands of entries paid for all of
 * them even when only a handful were on screen. This projects the leaf's
 * centre plus a radially-offset point to get its approximate NDC radius,
 * and reports it invisible only when the resulting box is entirely outside
 * the viewport (or behind the eye).
 *
 * Deliberately offscreen-only: no "too small to matter" rule here, because
 * leaf geometry (unlike labels) is what gives a dense platform its shape at
 * a distance, and dropping small nodes would visibly eat the surface.
 *
 * Only ever called in RENDERMODE_RENDER -- picking must keep considering
 * every node, and its matrices are set up for a 1-pixel frustum where this
 * test would not mean the same thing.
 *
 * Set FSV_TREEV_NO_LOD=1 to disable (same switch as the label LOD). */
static boolean
treev_leaf_offscreen( double r0, double distance, double theta )
{
	mat4 mvp;
	double ang, cs, sn, rc, re;
	double c[4], e[4];
	double cx, cy, ex, ey, rad;
	int k;

	if (!geometry_treev_lod_enabled( ))
		return FALSE;

	glm_mat4_mul(gl.projection, gl.modelview, mvp);

	ang = RAD(theta);
	cs = cos(ang);
	sn = sin(ang);
	rc = r0 + distance;
	re = rc + TREEV_LEAF_NODE_EDGE;

	for (k = 0; k < 4; k++) {
		c[k] = (double)mvp[0][k]*(rc*cs) + (double)mvp[1][k]*(rc*sn) + (double)mvp[3][k];
		e[k] = (double)mvp[0][k]*(re*cs) + (double)mvp[1][k]*(re*sn) + (double)mvp[3][k];
	}

	if ((c[3] <= 0.0) || (e[3] <= 0.0))
		return TRUE; /* behind the eye */

	cx = c[0]/c[3];
	cy = c[1]/c[3];
	ex = e[0]/e[3];
	ey = e[1]/e[3];

	/* Approximate on-screen radius of the leaf box, with a safety factor so
	 * nodes never pop at the viewport edge. */
	rad = 2.0 * sqrt((cx - ex)*(cx - ex) + (cy - ey)*(cy - ey));

	return ((cx + rad < -1.0) || (cx - rad > 1.0) ||
		(cy + rad < -1.0) || (cy - rad > 1.0));
}


/* Arranges/draws leaf nodes on a directory */
static void
treev_build_dir( GNode *dnode, double r0 )
{
#define edge05 (0.5 * TREEV_LEAF_NODE_EDGE)
#define edge15 (1.5 * TREEV_LEAF_NODE_EDGE)
	GNode *node;
	TreeVLayoutCacheEntry *cached_layout;
	RTvec pos;
	double arc_len, inter_arc_width;
	int n, row_node_count, remaining_node_count;
	/* Temporary diagnostic for the label-smear bug (2026-09-13) -- enable
	 * with FSV_DEBUG_LABELS=1. Checks whether child_count (the cache
	 * introduced alongside this bug) matches the real list length, and
	 * shows the very first row's row_node_count -- if that comes out
	 * <= 0, the inner assignment loop never runs and every child keeps
	 * its zeroed leaf.distance/leaf.theta, exactly matching what the
	 * label log showed. Remove once root cause is found. */
	static int treev_debug_labels = -1;
	if (treev_debug_labels < 0)
		treev_debug_labels = (g_getenv("FSV_DEBUG_LABELS") != NULL);

	if (treev_debug_labels) {
		int real_count = g_list_length( (GList *)dnode->children );
		int cached_count = DIR_NODE_DESC(dnode)->child_count;
		double dbg_arc_len = (PI / 180.0) * (r0 + TREEV_LEAF_NODE_EDGE) * TREEV_GEOM_PARAMS(dnode)->platform.arc_width - TREEV_PLATFORM_SPACING_WIDTH;
		int dbg_row_node_count = (int)floor( (dbg_arc_len - edge05) / edge15 );
		g_print("treev_build_dir: %s r0=%.6g arc_width=%.6g child_count=%d real_count=%d "
			"first_row_arc_len=%.6g first_row_node_count=%d\n",
			NODE_DESC(dnode)->name, r0, TREEV_GEOM_PARAMS(dnode)->platform.arc_width,
			cached_count, real_count, dbg_arc_len, dbg_row_node_count);
	}

	g_assert( NODE_IS_DIR(dnode) );

	/* Build rows of leaf nodes, going from the inner edge outward
	 * (this will require laying down nodes in reverse order) */
	cached_layout = treev_cache_using ? treev_layout_cache_lookup(dnode, r0) : NULL;
	if (cached_layout != NULL) {
		/* All leaf vertices are in the overview VBO. Their polar positions
		 * and this platform's exact row depth are unchanged, so avoid walking
		 * every child just to rewrite the same values on each frame. */
		TREEV_GEOM_PARAMS(dnode)->platform.depth = cached_layout->platform_depth;
	} else {
		remaining_node_count = DIR_NODE_DESC(dnode)->child_count;
		pos.r = r0 + TREEV_LEAF_NODE_EDGE;
		node = (GNode *)g_list_last( (GList *)dnode->children );
		while (node != NULL) {
			/* Calculate (available) arc length of row */
			arc_len = (PI / 180.0) * pos.r * TREEV_GEOM_PARAMS(dnode)->platform.arc_width - TREEV_PLATFORM_SPACING_WIDTH;
			/* Number of nodes this row can accomodate */
			row_node_count = (int)floor( (arc_len - edge05) / edge15 );
			/* Arc width between adjacent leaf nodes */
			inter_arc_width = (180.0 * edge15 / PI) / pos.r;

			/* Lay out nodes in this row, sweeping clockwise */
			pos.theta = 0.5 * inter_arc_width * (double)(MIN(row_node_count, remaining_node_count) - 1);
			for (n = 0; (n < row_node_count) && (node != NULL); n++) {
				TREEV_GEOM_PARAMS(node)->leaf.theta = pos.theta;
				TREEV_GEOM_PARAMS(node)->leaf.distance = pos.r - r0;
				/* The two assignments above are LAYOUT and must always run:
				 * labels, picking and the camera all read leaf.theta /
				 * leaf.distance. Only the draw call below is skippable. */
				if (treev_cache_building || treev_cache_using ||
				    (gl.render_mode != RENDERMODE_RENDER) ||
				    !treev_leaf_offscreen(r0, pos.r - r0, pos.theta))
					treev_gldraw_leaf( node, r0, !NODE_IS_DIR(node) );
				pos.theta -= inter_arc_width;
				node = node->prev;
			}

			remaining_node_count -= row_node_count;
			pos.r += edge15;
		}

		/* Official directory depth */
		pos.r -= edge05;
		TREEV_GEOM_PARAMS(dnode)->platform.depth = pos.r - r0;
		if (treev_cache_building)
			treev_layout_cache_store(dnode, r0);
	}

	/* Draw underlying directory */
	treev_gldraw_platform( dnode, r0 );

#undef edge05
#undef edge15
}


/* Screen-size LOD test for TreeV labels (added 2026-09-17).
 *
 * TreeV labels are the dominant per-frame cost: treev_apply_label( ) feeds
 * text_draw_straight_rotated( ) / text_draw_curved( ), which draw
 * immediately -- one draw call per label, no batching -- so a 54k-item tree
 * issues on the order of 54k draw calls every frame. MapV got a large win
 * from hiding labels while the camera moves; doing the same in TreeV was
 * explicitly not wanted (labels should stay visible during rotate/tilt), so
 * instead this culls purely on projected size: a label whose text would be
 * a few pixels tall is unreadable anyway, and skipping it is invisible to
 * the user while removing the draw call. Being size-based rather than
 * motion-based, it also helps when parked and zoomed in, and never makes
 * labels blink out as the camera starts or stops moving.
 *
 * world_size is measured RADIALLY (in the platform plane) rather than along
 * z: TreeV is normally viewed at a tilt, so a z-offset can project to almost
 * nothing even for a label that is large on screen, which would cull
 * perfectly readable labels.
 *
 * Set FSV_TREEV_NO_LOD=1 to disable, restoring the previous draw-everything
 * behaviour without a rebuild. */
/* Runtime performance toggles, driven by the Help menu check items (see
 * callbacks.c / window.c) in the same way as the FPS counter. Each env var
 * below only supplies the STARTUP default, so a headless/scripted run can
 * still pin a setting; the menu is authoritative afterwards.
 *
 * Defaults chosen 2026-09-17:
 *  - TreeV subtree culling ON: with the wedge test's "tiny" heuristic gone
 *    (see treev_draw_recursive( )) it is offscreen/behind-only, which is the
 *    safe half of the test, and it is what makes deep trees usable at all.
 *  - TreeV label + leaf LOD ON: pure win, invisible at normal zoom.
 *  - TreeV motion label hiding OFF: the user specifically wants TreeV labels
 *    readable while rotating/tilting; the size LOD covers most of the cost.
 *  - MapV motion label hiding OFF: keep the default view visually stable;
 *    users can enable the performance trade-off from the Help menu. */
static boolean treev_cull_flag = TRUE;
static boolean treev_lod_flag = TRUE;
static boolean treev_hide_labels_moving_flag = FALSE;
static boolean mapv_hide_labels_moving_flag = FALSE;
static boolean mapv_lod_flag = TRUE;
static boolean perf_flags_initialized = FALSE;

static void
perf_flags_init( void )
{
	if (perf_flags_initialized)
		return;
	perf_flags_initialized = TRUE;

	if (g_getenv("FSV_TREEV_NO_CULL") != NULL)
		treev_cull_flag = FALSE;
	if (g_getenv("FSV_TREEV_NO_LOD") != NULL)
		treev_lod_flag = FALSE;
	if (g_getenv("FSV_TREEV_HIDE_LABELS_MOVING") != NULL)
		treev_hide_labels_moving_flag = TRUE;
	if (g_getenv("FSV_MAPV_HIDE_LABELS_MOVING") != NULL)
		mapv_hide_labels_moving_flag = TRUE;
	if (g_getenv("FSV_MAPV_NO_HIDE_LABELS_MOVING") != NULL)
		mapv_hide_labels_moving_flag = FALSE;
	if (g_getenv("FSV_MAPV_NO_LABEL_LOD") != NULL)
		mapv_lod_flag = FALSE;
}

boolean
geometry_treev_cull_enabled( void )
{
	perf_flags_init( );
	return treev_cull_flag;
}

void
geometry_set_treev_cull( boolean enabled )
{
	perf_flags_init( );
	treev_cull_flag = enabled;
	redraw( );
}

boolean
geometry_treev_lod_enabled( void )
{
	perf_flags_init( );
	return treev_lod_flag;
}

void
geometry_set_treev_lod( boolean enabled )
{
	perf_flags_init( );
	treev_lod_flag = enabled;
	redraw( );
}

boolean
geometry_treev_hide_labels_moving( void )
{
	perf_flags_init( );
	return treev_hide_labels_moving_flag;
}

void
geometry_set_treev_hide_labels_moving( boolean enabled )
{
	perf_flags_init( );
	treev_hide_labels_moving_flag = enabled;
	redraw( );
}

boolean
geometry_mapv_hide_labels_moving( void )
{
	perf_flags_init( );
	return mapv_hide_labels_moving_flag;
}

void
geometry_set_mapv_hide_labels_moving( boolean enabled )
{
	perf_flags_init( );
	mapv_hide_labels_moving_flag = enabled;
	redraw( );
}

boolean
geometry_mapv_lod_enabled( void )
{
	perf_flags_init( );
	return mapv_lod_flag;
}

void
geometry_set_mapv_lod( boolean enabled )
{
	perf_flags_init( );
	mapv_lod_flag = enabled;
	redraw( );
}


/* Projected-size LOD test for TreeV labels -- see the block comment above
 * the toggles for the rationale. */
static boolean
treev_label_too_small( const RTZvec *pos, double world_size )
{
	mat4 mvp;
	double ang, cs, sn;
	double c0[4], c1[4];
	double r1, dx, dy;
	int k;

	if (!geometry_treev_lod_enabled( ))
		return FALSE;

	glm_mat4_mul(gl.projection, gl.modelview, mvp);

	ang = RAD(pos->theta);
	cs = cos(ang);
	sn = sin(ang);
	r1 = pos->r + world_size;

	/* Double-precision transform: TreeV r values reach the millions after a
	 * deep Expand-All, where float32 accumulation loses the sign of w. */
	for (k = 0; k < 4; k++) {
		c0[k] = (double)mvp[0][k]*(pos->r*cs) + (double)mvp[1][k]*(pos->r*sn) +
			(double)mvp[2][k]*pos->z + (double)mvp[3][k];
		c1[k] = (double)mvp[0][k]*(r1*cs) + (double)mvp[1][k]*(r1*sn) +
			(double)mvp[2][k]*pos->z + (double)mvp[3][k];
	}

	/* Behind the camera: not visible, so skipping it is free. */
	if ((c0[3] <= 0.0001) || (c1[3] <= 0.0001))
		return TRUE;

	dx = c0[0]/c0[3] - c1[0]/c1[3];
	dy = c0[1]/c0[3] - c1[1]/c1[3];

	/* ~0.008 NDC is roughly 4 px of text height on a 1080p viewport. */
	return (sqrt(dx*dx + dy*dy) < 0.008);
}


/* Draws a node name label. is_leaf indicates whether the given node should
 * be labeled as a leaf, or as a directory platform (if applicable) */
static void
treev_apply_label( GNode *node, double r0, boolean is_leaf )
{
	RTZvec label_pos;
	XYvec leaf_label_dims;
	RTvec platform_label_dims;
	double height;
	/* Temporary diagnostic for the label-smear bug reported 2026-09-13
	 * (many unrelated labels overlapping when zoomed into a directory)
	 * -- enable with FSV_DEBUG_LABELS=1. Prints every label actually
	 * submitted for drawing, with enough position info to tell whether
	 * the label-visibility filter (radius-only, ignores theta) or
	 * something else is responsible. Remove once root cause is found. */
	static int treev_debug_labels = -1;
	if (treev_debug_labels < 0)
		treev_debug_labels = (g_getenv("FSV_DEBUG_LABELS") != NULL);

	if (treev_debug_labels) {
		double abs_r = is_leaf ? r0 + TREEV_GEOM_PARAMS(node)->leaf.distance : r0;
		double abs_theta = is_leaf ? TREEV_GEOM_PARAMS(node)->leaf.theta : 0.0;
		g_print("treev_apply_label: %s is_leaf=%d r0=%.6g abs_r=%.6g leaf_theta=%.6g "
			"target_r=%.6g target_theta=%.6g\n",
			NODE_DESC(node)->name, (int)is_leaf, r0, abs_r, abs_theta,
			TREEV_CAMERA(camera)->target.r, TREEV_CAMERA(camera)->target.theta);
	}

	if (is_leaf) {
		/* Apply label to top face of leaf node */
		height = TREEV_GEOM_PARAMS(node)->leaf.height;
		if (NODE_IS_DIR(node)) {
			height *= (1.0 - DIR_NODE_DESC(node)->deployment);
			leaf_label_dims.x = (0.8125 * TREEV_LEAF_NODE_EDGE);
		}
		else
			leaf_label_dims.x = (0.875 * TREEV_LEAF_NODE_EDGE);
		leaf_label_dims.y = ((2.0 - MAGIC_NUMBER) * TREEV_LEAF_NODE_EDGE);
		label_pos.r = r0 + TREEV_GEOM_PARAMS(node)->leaf.distance;
		label_pos.theta = TREEV_GEOM_PARAMS(node)->leaf.theta;
		label_pos.z = height + TREEV_GEOM_PARAMS(node->parent)->platform.height;
		if (treev_label_too_small(&label_pos, leaf_label_dims.y))
			return;
		text_draw_straight_rotated( NODE_DESC(node)->name, &label_pos, &leaf_label_dims );
	}
	else {
		/* Label directory platform, inside its inner edge */
		label_pos.r = r0 - (0.0625 * TREEV_PLATFORM_SPACING_DEPTH);
		label_pos.theta = 0.0;
		label_pos.z = 0.0;
		platform_label_dims.r = ((2.0 - MAGIC_NUMBER) * TREEV_PLATFORM_SPACING_DEPTH);
		platform_label_dims.theta = TREEV_GEOM_PARAMS(node)->platform.arc_width - (180.0 * TREEV_PLATFORM_SPACING_WIDTH / PI) / label_pos.r;
		if (treev_label_too_small(&label_pos, platform_label_dims.r))
			return;
		text_draw_curved( NODE_DESC(node)->name, &label_pos, &platform_label_dims );
	}
}


/* TreeV mode "full draw" */
static boolean
treev_draw_recursive( GNode *dnode, double prev_r0, double r0, int action )
{
	DirNodeDesc *dir_ndesc;
	TreeVGeomParams *dir_gparams;
	GNode *node;
	GNode *first_node = NULL, *last_node = NULL;
	RTvec leaf;
	double subtree_r0;
	double theta0, theta1;
	boolean dir_collapsed;
	boolean dir_expanded;
	boolean subtree_culled = FALSE;

	g_assert( NODE_IS_DIR(dnode) || NODE_IS_METANODE(dnode) );
	dir_ndesc = DIR_NODE_DESC(dnode);
	dir_gparams = TREEV_GEOM_PARAMS(dnode);

	mat4 tmpmat;
	glm_mat4_copy(gl.modelview, tmpmat);
	//debug_print_matrices(1);

	dir_collapsed = DIR_COLLAPSED(dnode);
        dir_expanded = DIR_EXPANDED(dnode);

	if (!treev_cache_building && !treev_cache_using &&
	    !NODE_IS_METANODE(dnode) && !dir_collapsed) {
		/* Non-destructive diagnostic for the TreeV culling work (see
		 * backlog) -- enable with FSV_DEBUG_CULL=1. Computes whether this
		 * node's WHOLE visible subtree -- a polar wedge bounded radially
		 * by [r0, r0+platform.subtree_max_depth] (already aggregated by
		 * treev_arrange_recursive( ), no subtree walk needed here) and
		 * angularly by platform.theta +/- 0.5*MAX(platform.arc_width,
		 * platform.subtree_arc_width) -- would be considered culled by a
		 * MapV-style NDC test, WITHOUT skipping anything yet. Sampled at
		 * both radii and 5 angular fractions across the wedge; tested in
		 * the incoming (parent) frame, i.e. BEFORE this node's own
		 * rotation below, matching how MapV tests in its parent's frame.
		 * Purely for comparing the verdict against what's actually on
		 * screen, before this is ever wired into an actual skip -- see
		 * the two 2026-08-31 crashes in the backlog for why that step is
		 * kept separate and this stays inert until proven correct. */
		static int treev_debug_cull = -1;
		if (treev_debug_cull < 0)
			treev_debug_cull = (g_getenv("FSV_DEBUG_CULL") != NULL);
		if (treev_debug_cull || geometry_treev_cull_enabled( )) {
			boolean would_cull = FALSE;
			/* Angular half-extent, with a safety margin: arc_width and
			 * subtree_arc_width describe the platform, but branches and
			 * labels reach slightly outside it. */
			double half_width = 0.6 * MAX(dir_gparams->platform.arc_width,
						       dir_gparams->platform.subtree_arc_width);
			/* Radial extent must cover BOTH this platform's own leaf rows
			 * (platform.depth) and everything aggregated below it
			 * (subtree_max_depth) -- taking only the latter made the wedge
			 * radially degenerate for directories whose subtree is shallow,
			 * which is one half of the 2026-09-17 "items at a certain
			 * distance from the parent vanish" report. */
			double r_outer = r0 + MAX(dir_gparams->platform.subtree_max_depth,
						  dir_gparams->platform.depth)
					    + TREEV_PLATFORM_SPACING_DEPTH;
			mat4 mvp;
			boolean any_in_front = FALSE;
			boolean any_behind = FALSE;
			float ndc_x0 = 1.0e9f, ndc_x1 = -1.0e9f;
			float ndc_y0 = 1.0e9f, ndc_y1 = -1.0e9f;
			int smp;
			/* Angular sample COUNT scales with half_width (added 2026-09-17,
			 * fixing the "items vanish 4x per 360-degree rotate" report).
			 * A fixed 5 samples is fine for an ordinary platform's own
			 * modest arc, but an ancestor close to the root can have a
			 * half_width approaching 180 degrees (many top-level siblings).
			 * With only 5 samples spread across such a wide arc, the actual
			 * near-clip / behind-camera boundary can fall entirely BETWEEN
			 * two adjacent samples as the camera orbits -- every sample
			 * reports "behind", any_in_front stays FALSE, and the whole
			 * subtree (everything under that ancestor, not just the wide
			 * node itself) gets wrongly culled, at specific camera angles
			 * that recur as the camera completes a rotation. Capping the
			 * angular step at ~12 degrees closes that gap; the sample count
			 * is still capped so a pathological arc cannot blow up the cost. */
			int n_samples = (int)MIN(MAX((2.0 * half_width) / 12.0 + 1.0, 5.0), 61.0);

			glm_mat4_mul(gl.projection, tmpmat, mvp);

			for (smp = 0; smp < n_samples; smp++) {
				double frac = (n_samples == 1) ? 0.0 :
					(-1.0 + 2.0 * smp / (double)(n_samples - 1));
				double ang = (dir_gparams->platform.theta + frac * half_width) * M_PI / 180.0;
				double cs = cos(ang), sn = sin(ang);
				double rr[2];
				double zz[2];
				int j, zi;
				rr[0] = r0;
				rr[1] = r_outer;
				/* Sample the wedge's vertical extent too, not just the
				 * z=0 plane. TreeV is viewed at a tilt, so a flat z=0
				 * wedge projects to a near-degenerate sliver at shallow
				 * tilt angles while the actual towers above it are plainly
				 * visible -- the other half of the 2026-09-17 vanishing
				 * report. The upper bound is deliberately generous: an
				 * over-tall box only ever makes the cull more
				 * conservative. */
				zz[0] = 0.0;
				zz[1] = dir_gparams->platform.height + 8.0 * TREEV_LEAF_NODE_EDGE;
				for (j = 0; j < 2; j++)
				for (zi = 0; zi < 2; zi++) {
					/* Manual double-precision transform instead of
					 * glm_mat4_mulv( ) (which multiplies/accumulates in
					 * float32). At TreeV's typical r0 scale after a deep
					 * Expand-All chain (millions), the float accumulation
					 * loses enough precision to spuriously flip clip.w's
					 * sign, making all_in_front FALSE for essentially every
					 * node -- observed 2026-09-16 on a 54k-item tree (100%
					 * "NOT all_in_front", so the cull test never fired and
					 * gave zero FPS benefit). mvp itself is still float --
					 * that's what the renderer actually uses -- but doing
					 * just this dot product in double keeps the test usable
					 * at the magnitudes TreeV can reach. */
					double px = rr[j]*cs, py = rr[j]*sn;
					double clip[4];
					int k;
					for (k = 0; k < 4; k++)
						clip[k] = (double)mvp[0][k]*px + (double)mvp[1][k]*py +
							  (double)mvp[2][k]*zz[zi] + (double)mvp[3][k];
					if (clip[3] <= 0.0001) {
						any_behind = TRUE;
						continue;
					}
					any_in_front = TRUE;
					ndc_x0 = MIN(ndc_x0, (float)(clip[0]/clip[3]));
					ndc_x1 = MAX(ndc_x1, (float)(clip[0]/clip[3]));
					ndc_y0 = MIN(ndc_y0, (float)(clip[1]/clip[3]));
					ndc_y1 = MAX(ndc_y1, (float)(clip[1]/clip[3]));
				}
			}

			if (!any_in_front) {
				/* All 10 sampled corners are behind the camera plane.
				 * Unlike a mixed result (below) this is unambiguous -- the
				 * whole wedge is behind the camera -- so it is safe to cull.
				 * Added 2026-09-17 after finding that ~96% of a wide,
				 * deeply-nested sibling group (a hex-fanout directory with
				 * hundreds of siblings spread over a huge arc) fell into the
				 * old catch-all "not all in front" branch and was never
				 * culled even though it sat entirely behind the camera --
				 * the actual reason FPS did not improve when fully zoomed
				 * into a single item. */
				would_cull = TRUE;
				if (treev_debug_cull)
					g_print("treev_cull_diag: %s r0=%.6g r_outer=%.6g theta=%.6g "
						"half_width=%.6g fully behind camera -- would_cull=1\n",
						NODE_DESC(dnode)->name, r0, r_outer,
						dir_gparams->platform.theta, half_width);
			} else if (any_behind) {
				/* Mixed: some sampled corners in front, some behind -- most
				 * likely straddling the near-clip plane, or a very wide
				 * wedge partly wrapping behind the camera. Stay
				 * conservative, exactly as before. */
				if (treev_debug_cull)
					g_print("treev_cull_diag: %s r0=%.6g r_outer=%.6g theta=%.6g "
						"half_width=%.6g mixed front/behind -- would_cull=0\n",
						NODE_DESC(dnode)->name, r0, r_outer,
						dir_gparams->platform.theta, half_width);
			} else {
				/* Offscreen-only. The old "tiny" rule (cull anything whose
				 * projected box is very thin or small) was REMOVED on
				 * 2026-09-17: a wedge seen nearly edge-on at certain tilt
				 * angles projects to an arbitrarily thin sliver while its
				 * geometry is fully visible, so that rule silently deleted
				 * real content -- exactly the failure mode the backlog
				 * warns about twice. Size-based savings now come from the
				 * per-label LOD instead, which can only drop unreadable
				 * text, never geometry.
				 *
				 * The margin keeps a subtree alive slightly past the
				 * viewport edge, since the sampled wedge is an
				 * approximation of a curved region. */
				static const float margin = 0.15f;
				boolean offscreen = (ndc_x1 < -1.0f - margin) ||
						     (ndc_x0 >  1.0f + margin) ||
						     (ndc_y1 < -1.0f - margin) ||
						     (ndc_y0 >  1.0f + margin);
				would_cull = offscreen;
				if (treev_debug_cull)
					g_print("treev_cull_diag: %s r0=%.6g r_outer=%.6g theta=%.6g "
						"half_width=%.6g ndc=[%.4g,%.4g]x[%.4g,%.4g] "
						"would_cull=%d (offscreen=%d)\n",
						NODE_DESC(dnode)->name, r0, r_outer,
						dir_gparams->platform.theta, half_width,
						ndc_x0, ndc_x1, ndc_y0, ndc_y1,
						(int)would_cull, (int)offscreen);
			}
			if (geometry_treev_cull_enabled( ))
				subtree_culled = would_cull;
		}
	}

	if (!dir_collapsed) {
		if (!dir_expanded) {
			/* Directory is partially deployed, so
			 * draw/label the shrinking/growing leaf */
			if (action >= TREEV_DRAW_GEOMETRY) {
				treev_gldraw_leaf( dnode, prev_r0, TRUE );
				treev_gldraw_folder( dnode, prev_r0 );
			}
			else if (action == TREEV_DRAW_LABELS) {
				text_set_color(treev_leaf_label_color.r,
					       treev_leaf_label_color.g,
					       treev_leaf_label_color.b);
				treev_apply_label( dnode, prev_r0, TRUE );
			}

			/* Platform should shrink to / grow from
			 * corresponding leaf position */
			leaf.r = prev_r0 + dir_gparams->leaf.distance;
			leaf.theta = dir_gparams->leaf.theta;
			glm_rotate_z(gl.modelview, leaf.theta * M_PI/180.0, gl.modelview);
			glm_translate(gl.modelview, (vec3){leaf.r, 0.0, 0.0});
			glm_scale(gl.modelview, (vec3){dir_ndesc->deployment, dir_ndesc->deployment, dir_ndesc->deployment});
			glm_translate(gl.modelview, (vec3){-leaf.r, 0.0, 0.0});
			glm_rotate_z(gl.modelview, -leaf.theta * M_PI/180.0, gl.modelview);
		}

		glm_rotate_z(gl.modelview, dir_gparams->platform.theta * M_PI / 180.0, gl.modelview);
		ogl_upload_matrices(TRUE);

		/* Keep TreeV subtrees during camera movement and close zooms. The
		 * cached polar wedge is only an approximation and can hide valid
		 * nested geometry; label distance culling handles the cheap LOD. */
	}

	if (action >= TREEV_DRAW_GEOMETRY) {
		/* Draw directory, in either leaf or platform form
		 */
		if (dir_collapsed)
		{
			/* Leaf form */
			treev_gldraw_leaf(dnode, prev_r0, TRUE);
			treev_gldraw_folder(dnode, prev_r0);
		}
		else if (NODE_IS_DIR(dnode) && !subtree_culled)
		{
			/* treev_build_dir() computes positions and submits the platform
			 * and leaf geometry to OpenGL, so it must run for every draw. */
			treev_build_dir(dnode, r0);
		}
	}

	if (!dir_collapsed && !subtree_culled) {
		/* Recurse into subdirectories */
		subtree_r0 = r0 + dir_gparams->platform.depth + TREEV_PLATFORM_SPACING_DEPTH;
		node = dnode->children;
		while (node != NULL) {
			if (!NODE_IS_DIR(node))
				break;
			if (treev_draw_recursive( node, r0, subtree_r0, action )) {
				/* This subdirectory is expanded.
				 * Save first/last node information for
				 * drawing interconnecting branches */
				if (first_node == NULL)
					first_node = node;
				last_node = node;
			}
			node = node->next;
		}
	}

	if (dir_expanded && !subtree_culled && (action == TREEV_DRAW_GEOMETRY_WITH_BRANCHES)) {
		/* Draw interconnecting branches */
		if (NODE_IS_METANODE(dnode))
		{
			treev_gldraw_loop(r0);
			treev_gldraw_outbranch(r0, 0.0, 0.0);
		}
		else
		{
			treev_gldraw_inbranch(r0);
			if (first_node != NULL)
			{
				theta0 = MIN(0.0, TREEV_GEOM_PARAMS(first_node)->platform.theta);
				theta1 = MAX(0.0, TREEV_GEOM_PARAMS(last_node)->platform.theta);
				treev_gldraw_outbranch(r0 + dir_gparams->platform.depth, theta0, theta1);
			}
		}
	}

	if (action == TREEV_DRAW_LABELS) {
		/* By this point, gl.modelview (the CPU-side matrix) is
		 * correctly back to this node's own frame -- restored after
		 * each recursive child call below. But the GPU-side text MVP
		 * uniform is shared, global state: every child call along the
		 * way re-uploaded it to ITS OWN frame via ogl_upload_matrices( ),
		 * and never restored it afterward. So without refreshing it
		 * here, this node's own labels would be drawn using whatever
		 * frame the last-visited child/grandchild left behind --
		 * consistent internally (all of this node's labels use the same
		 * stale matrix, so they stay correctly positioned relative to
		 * each other) but shifted as a whole block relative to where
		 * this node's platform actually is. */
		ogl_upload_matrices(TRUE);

		/* Draw name label(s) */
		if (dir_collapsed)
		{
			/* Label directory leaf */
			text_set_color(treev_leaf_label_color.r,
				       treev_leaf_label_color.g,
				       treev_leaf_label_color.b);
			treev_apply_label(dnode, prev_r0, TRUE);
		}
		else if (NODE_IS_DIR(dnode) && !subtree_culled)
		{
			/* Label directory platform */
			text_set_color(treev_platform_label_color.r,
				       treev_platform_label_color.g,
				       treev_platform_label_color.b);
			treev_apply_label(dnode, r0, FALSE);
			/* Label leaf nodes that aren't directories */
			text_set_color(treev_leaf_label_color.r,
				       treev_leaf_label_color.g,
				       treev_leaf_label_color.b);
			node = dnode->children;
			while (node != NULL)
			{
				if (!NODE_IS_DIR(node))
					treev_apply_label(node, r0, TRUE);
				node = node->next;
			}
		}
	}

	/* Update geometry status */
	dir_ndesc->geom_expanded = !dir_collapsed;

	if (!dir_collapsed) {
		glm_mat4_copy(tmpmat, gl.modelview);
		ogl_upload_matrices(FALSE);
	}

	return dir_expanded;
}


/* Draws the node cursor, size/position specified by corners */
static void
treev_gldraw_cursor( RTZvec *c0, RTZvec *c1 )
{
	static const double bar_part = SQR(SQR(MAGIC_NUMBER - 1.0));
	RTZvec corner_dims;
	RTZvec p, delta;
	XYvec cp0, cp1;
	double theta;
	double sin_theta, cos_theta;
	int seg_count;
	int i, c, s;

	g_assert( c1->r > c0->r );
	g_assert( c1->theta > c0->theta );
	g_assert( c1->z > c0->z );

	corner_dims.r = bar_part * (c1->r - c0->r);
	corner_dims.theta = bar_part * (c1->theta - c0->theta);
	corner_dims.z = bar_part * (c1->z - c0->z);

	seg_count = (int)ceil( corner_dims.theta / TREEV_CURVE_GRANULARITY );

	cursor_pre( );
	static GLuint vbo;
	if (!vbo) glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	for (i = 0; i <= 1; i++) {
		if (i == 0)
			cursor_hidden_part( );
		if (i == 1)
			cursor_visible_part( );

		for (c = 0; c < 8; c++) {
			if (c & 1) {
				p.r = c1->r;
				delta.r = - corner_dims.r;
			}
			else {
				p.r = c0->r;
				delta.r = corner_dims.r;
			}

			if (c & 2) {
				p.theta = c1->theta;
				delta.theta = - corner_dims.theta;
			}
			else {
				p.theta = c0->theta;
				delta.theta = corner_dims.theta;
			}

			if (c & 4) {
				p.z = c1->z;
				delta.z = - corner_dims.z;
			}
			else {
				p.z = c0->z;
				delta.z = corner_dims.z;
			}

			sin_theta = sin( RAD(p.theta) );
			cos_theta = cos( RAD(p.theta) );
			cp0.x = p.r * cos_theta;
			cp0.y = p.r * sin_theta;
			cp1.x = (p.r + delta.r) * cos_theta;
			cp1.y = (p.r + delta.r) * sin_theta;

			const size_t vert_cnt = 4 + seg_count + 1;
			VertexPos *vert = NEW_ARRAY(VertexPos, vert_cnt);
			vert[0] = (VertexPos){{cp0.x, cp0.y, p.z + delta.z}}; // Vertical axis start
			vert[1] = (VertexPos){{cp0.x, cp0.y, p.z}}; // Vertical axis end
			vert[2] = (VertexPos){{cp1.x, cp1.y, p.z}}; // Radial axis end
			vert[3] = (VertexPos){{cp0.x, cp0.y, p.z}}; // Back to radial/vertical intersection
			/* Tangent axis (curved part) */
			for (s = 0; s <= seg_count; s++) {
				theta = p.theta + delta.theta * (double)s / (double)seg_count;
				cp0.x = p.r * cos( RAD(theta) );
				cp0.y = p.r * sin( RAD(theta) );
				vert[4 + s] = (VertexPos){{cp0.x, cp0.y, p.z}};
			}

			glBufferData(GL_ARRAY_BUFFER,
				     sizeof(VertexPos) * vert_cnt, vert,
				     GL_STREAM_DRAW);
			glEnableVertexAttribArray(gl.position_location);
			glVertexAttribPointer(
			    gl.position_location, 3, GL_FLOAT, GL_FALSE,
			    sizeof(VertexPos),
			    (void *)offsetof(VertexPos, position));
			glDrawArrays(GL_LINE_STRIP, 0, vert_cnt);
			glBufferData(GL_ARRAY_BUFFER,
				     sizeof(VertexPos) * vert_cnt, NULL,
				     GL_STREAM_DRAW);
		}
	}
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	cursor_post( );
}


/* Draws the node cursor in an intermediate position between its previous
 * steady-state position and the current node (pos=0 indicates the former,
 * pos=1 the latter) */
static void
treev_draw_cursor( double pos )
{
	RTZvec c0, c1;
	RTZvec cursor_c0, cursor_c1;

	treev_get_corners( globals.current_node, &c0, &c1 );

	/* Interpolate corners */
	cursor_c0.r = INTERPOLATE(pos, treev_cursor_prev_c0.r, c0.r);
	cursor_c0.theta = INTERPOLATE(pos, treev_cursor_prev_c0.theta, c0.theta);
	cursor_c0.z = INTERPOLATE(pos, treev_cursor_prev_c0.z, c0.z);
	cursor_c1.r = INTERPOLATE(pos, treev_cursor_prev_c1.r, c1.r);
	cursor_c1.theta = INTERPOLATE(pos, treev_cursor_prev_c1.theta, c1.theta);
	cursor_c1.z = INTERPOLATE(pos, treev_cursor_prev_c1.z, c1.z);

	treev_gldraw_cursor( &cursor_c0, &cursor_c1 );
}


/* Draws TreeV geometry */
static void
treev_draw( boolean high_detail )
{
	boolean draw_labels;

	if ((fstree_low_draw_stage == 0) || (fstree_high_draw_stage == 0))
		treev_arrange( FALSE );

	/* Draw low-detail geometry */

	treev_batch_begin( );
	treev_draw_recursive( globals.fstree, NIL, treev_core_radius, TREEV_DRAW_GEOMETRY_WITH_BRANCHES );
	if (treev_cache_building) {
		treev_batch_flush( );
		treev_cache_valid = TRUE;
		treev_cache_dirty = FALSE;
		treev_cache_building = FALSE;
		if (ogl_profile_enabled())
			g_print("FSV TreeV overview geometry cache built (%zu batches)\n",
				treev_cached_chunk_count);
	} else if (treev_cache_using) {
		treev_cached_draw_all( );
	} else {
		treev_batch_flush( );
	}

	/* Only a genuine on-screen render advances the draw-stage bookkeeping.
	 * treev_draw( ) is also invoked from ogl_select_modern( ) (node-picking,
	 * triggered by mouse hover -- see viewport.c's hover_pick_due( )), which
	 * runs at its own, independent timing relative to the real render( )
	 * callback. Letting a SELECT-mode call increment these shared static
	 * counters lets it silently "use up" a stage transition that the next
	 * real frame was relying on (e.g. skipping treev_arrange( ) because the
	 * counter says stage 1 already happened, when it only happened for a
	 * throwaway picking pass) -- a timing-dependent bug matching the
	 * 2026-09-15 label-smear report (reproduced only at certain zoom points
	 * during scroll-wheel zoom, i.e. exactly when hover-picks are likely to
	 * interleave with real frames). */
	if ((gl.render_mode == RENDERMODE_RENDER) && (fstree_low_draw_stage <= 1))
		++fstree_low_draw_stage;

	draw_labels = high_detail;
	if (draw_labels) {
		/* Draw additional high-detail stuff */

		/* Node name labels */
		text_pre();
		treev_draw_recursive(globals.fstree, NIL, treev_core_radius, TREEV_DRAW_LABELS);
		text_post();

		if ((gl.render_mode == RENDERMODE_RENDER) && (fstree_high_draw_stage <= 1))
			++fstree_high_draw_stage;

		/* Node cursor */
		if (!high_detail)
			return;
		treev_draw_cursor( CURSOR_POS(camera->pan_part) );
	}
}


/**** COMMON ROUTINES *****************************************/


/* Call before drawing the cursor */
static void
cursor_pre( void )
{
	glUseProgram(gl.program);
	ogl_disable_lightning();
}


/* Call to draw the "hidden" part of the cursor */
static void
cursor_hidden_part( void )
{
	/* Hidden part is drawn with a thin line */
	glDepthFunc( GL_GREATER );
	glLineWidth(2.0);
	glUniform4f(gl.color_location, 0.3, 0.3, 0.3, 1);
}


/* Call to draw the visible part of the cursor */
static void
cursor_visible_part( void )
{
	/* Visible part is drawn with a thick solid line */
	glDepthFunc( GL_LEQUAL );
	glLineWidth( 5.0 );
	glUniform4f(gl.color_location, 1, 1, 1, 1);
}


/* Call after drawing the cursor */
static void
cursor_post( void )
{
	glLineWidth( 1.0 );
	ogl_enable_lightning();
	glUseProgram(0);
}


/* Zeroes drawing stages and invalidates TreeV's overview leaf cache when
 * geometry changes. */
static void
queue_uncached_draw( void )
{
	fstree_low_draw_stage = 0;
	fstree_high_draw_stage = 0;
	if (globals.fsv_mode == FSV_TREEV)
		treev_cache_invalidate( );
	else if (globals.fsv_mode == FSV_MAPV)
		mapv_cache_invalidate( );
}


/* Flags a directory's geometry for rebuilding */
void
geometry_queue_rebuild( GNode *dnode )
{
	queue_uncached_draw( );
}


/* Sets up filesystem tree geometry for the specified mode */
void
geometry_init( FsvMode mode )
{
	camera_treev_follow_begin(NULL);
	DIR_NODE_DESC(globals.fstree)->deployment = 1.0;
	geometry_queue_rebuild( globals.fstree );

	switch (mode) {
		case FSV_MAPV:
		mapv_init( );
		break;

		case FSV_TREEV:
		treev_init( );
		break;

		SWITCH_FAIL
	}

	color_assign_recursive( globals.fstree );
}


/* Draws "fsv" in 3D */
void
geometry_gldraw_fsv( void )
{
	XYvec p, n;
	const float *vertices = NULL;
	const int *triangles = NULL, *edges = NULL;
	int v, e, i;
	// Magic constants 490 and 1188 determined by first making these arrays
	// larger and checking vlen and ilen values in the debugger.
#define VERT_MAX_LEN 490
#define IDX_MAX_LEN 1188
	static AboutVertex vert[VERT_MAX_LEN];
	static GLushort idx[IDX_MAX_LEN];
	static GLuint vbo, ebo;
	static size_t vlen;
	static size_t ilen;

	if (!vbo) {
		for (size_t c = 0; c < 3; c++) {
			GLfloat *color = (GLfloat*)&fsv_colors[c];
			vertices = fsv_vertices[c];
			triangles = fsv_triangles[c];
			edges = fsv_edges[c];
			const EdgeSmoothness *es = fsv_edge_smoothness[c];
			// Side faces
			for (e = 0; edges[e] >= 0; e++) {
				// For a smooth edge, calculate the normal from
				// the previous and next vertices. For a sharp
				// edge, duplicate the vertices, normal for the
				// first is calculated from the previous and
				// current vertex, and for the second vertex
				// the normal is calculated from the current
				// and next vertex.
				// Edge here meaning the vertex indices of the
				// vertices forming the sides of the character,
				// not the "edge" from graph theory.
				i = edges[e];
				EdgeSmoothness s = es[e];
				p.x = vertices[2 * i];
				p.y = vertices[2 * i + 1];
				int inext = edges[e + 1];
				int iprev = edges[e - 1];
				XYvec n2;
				if (e == 0) {
					// First edge, must use only "forward"
					// delta
					s = SMOOTH;
					i = inext;
					n.x = vertices[2 * i + 1] - p.y;
					n.y = p.x - vertices[2 * i];
				} else if (inext < 0) {
					// Last edge, must use only "backward"
					// delta
					s = SMOOTH;
					n.x = p.y - vertices[2 * iprev + 1];
					n.y = vertices[2 * iprev] - p.x;
				} else if (s == SMOOTH) {
					i = inext;
					n.x = vertices[2 * i + 1] -
					      vertices[2 * iprev + 1];
					n.y = vertices[2 * iprev] -
					      vertices[2 * i];
				} else if (s == SHARP) {
					// First normal with backward delta.
					n.x = p.y - vertices[2 * iprev + 1];
					n.y = vertices[2 * iprev] - p.x;
					// Second normal with forward delta.
					i = inext;
					n2.x = vertices[2 * i + 1] - p.y;
					n2.y = p.x - vertices[2 * i];
				} else
					g_error("Unable to calculate normal!");

				if (e > 0) {
					idx[ilen++] = vlen - 2;
					idx[ilen++] = vlen - 1;
					idx[ilen++] = vlen;
					idx[ilen++] = vlen;
					idx[ilen++] = vlen - 1;
					idx[ilen++] = vlen + 1;
				}
				vert[vlen++] = (AboutVertex){
				    {p.x, p.y, 30.0},
				    {n.x, n.y, 0.0f},
				    {color[0], color[1], color[2]}};
				vert[vlen++] = (AboutVertex){
				    {p.x, p.y, -30.0},
				    {n.x, n.y, 0.0f},
				    {color[0], color[1], color[2]}};
				if (s == SHARP) {
					// Second set of vertices with forward
					// delta normals.
					vert[vlen++] = (AboutVertex){
					    {p.x, p.y, 30.0},
					    {n2.x, n2.y, 0.0f},
					    {color[0], color[1], color[2]}};
					vert[vlen++] = (AboutVertex){
					    {p.x, p.y, -30.0},
					    {n2.x, n2.y, 0.0f},
					    {color[0], color[1], color[2]}};
				}
			}
			/* Front faces */
			int imax = 0;
			for (v = 0; triangles[v] >= 0; v++) {
				i = triangles[v];
				imax = MAX(i, imax);
				p.x = vertices[2 * i];
				p.y = vertices[2 * i + 1];
				vert[vlen + i] = (AboutVertex){
					{p.x, p.y, 30.0},
					{0.0f, 0.0f, 1.0f},
					{color[0], color[1], color[2]}
				};
				idx[ilen++] = vlen + i;
			}
			vlen += imax + 1;

			/* Back faces */
			imax = 0;
			for (--v; v >= 0; v--) {
				i = triangles[v];
				imax = MAX(i, imax);
				p.x = vertices[2 * i];
				p.y = vertices[2 * i + 1];
				vert[vlen + i] = (AboutVertex){
					{p.x, p.y, -30.0},
					{0.0f, 0.0f, -1.0f},
					{color[0], color[1], color[2]}
				};
				idx[ilen++] = vlen + i;
			}
			vlen += imax + 1;
		}
		g_assert(VERT_MAX_LEN >= vlen);
		g_assert(IDX_MAX_LEN >= ilen);
#undef VERT_MAX_LEN
#undef IDX_MAX_LEN
		glGenBuffers(1, &vbo);
		glBindBuffer(GL_ARRAY_BUFFER, vbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(AboutVertex) * vlen, vert, GL_STATIC_DRAW);

		glGenBuffers(1, &ebo);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(GLushort) * ilen, idx, GL_STATIC_DRAW);

		// Upload fog parameters. Emulate legacy GL with a simple
		// linear fog model.
		glUseProgram(aboutGL.program);
		glUniform3f(aboutGL.fog_color_location, 0.0f, 0.0f, 0.0f);
		glUniform1f(aboutGL.fog_start_location, 200.0f);
		glUniform1f(aboutGL.fog_end_location, 1800.0f);

		ogl_error();
	}
#if 0
	// Useful code snippet for checking vertex coords in NDC
	for (size_t i = 0; i < vlen; i += (vlen - 1)) {
		g_print("%zu th vertex in FSV coords:\n", i);
		vec4 c = (vec4){vert[i].position[0], vert[i].position[1],
				vert[i].position[2], 1.0f};
		vec4 cp;
		glm_mat4_mulv(aboutGL.mvp, c, cp);
		glmc_vec4_print(c, stdout);
		glmc_vec4_print(cp, stdout);
		float w = cp[3];
		vec3 pd = (vec3){cp[0]/w, cp[1]/w, cp[2]/w};
		glmc_vec3_print(pd, stdout);
	}
	glmc_mat4_print(aboutGL.mvp, stdout);
#endif
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glEnableVertexAttribArray(aboutGL.position_location);
	glVertexAttribPointer(aboutGL.position_location, 3, GL_FLOAT, GL_FALSE,
			      sizeof(AboutVertex),
			      (void *)offsetof(AboutVertex, position));
	glEnableVertexAttribArray(aboutGL.normal_location);
	glVertexAttribPointer(aboutGL.normal_location, 3, GL_FLOAT, GL_FALSE,
			      sizeof(AboutVertex),
			      (void *)offsetof(AboutVertex, normal));
	glEnableVertexAttribArray(aboutGL.color_location);
	glVertexAttribPointer(aboutGL.color_location, 3, GL_FLOAT, GL_FALSE,
			      sizeof(AboutVertex),
			      (void *)offsetof(AboutVertex, color));
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
	glUseProgram(aboutGL.program);
	glDrawElements(GL_TRIANGLES, ilen, GL_UNSIGNED_SHORT, 0);
	glUseProgram(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	ogl_error();
}


/* Draws the splash screen */
static void
splash_draw( void )
{
	XYZvec text_pos;
	XYvec text_dims;
	double bottom_y;
	double k;

	/* Draw fsv title */

	/* Set up projection matrix */
	k = 82.84 / ogl_aspect_ratio( );
	mat4 proj;
	glm_frustum(-70.82, 95.40, - k, k, 200.0, 400.0, proj);

	/* Set up modelview matrix */
	mat4 mv;
	glm_mat4_identity(mv);
	glm_translate(mv, (vec3){0.0, 0.0, -300.0});
	glm_rotate_x(mv, 10.5 * M_PI/180.0, mv);
	glm_translate(mv, (vec3){20.0, 20.0, -30.0});

	mat4 mvp;
	glm_mat4_mul(proj, mv, mvp);
	mat3 normmat;
	glm_mat4_pick3(mv, normmat);
	glm_mat3_inv(normmat, normmat);
	glm_mat3_transpose(normmat);
	glUseProgram(aboutGL.program);
	/* update the "mvp" matrix we use in the shader */
	glUniformMatrix4fv(aboutGL.mvp_location, 1, GL_FALSE, (float*)mvp);
	glUniformMatrix4fv(aboutGL.modelview_location, 1, GL_FALSE, (float*) mv);
	glUniformMatrix3fv(aboutGL.normal_matrix_location, 1, GL_FALSE, (float*) normmat);
	glUseProgram(0);

	geometry_gldraw_fsv( );

	/* Draw accompanying text */

	/* Set up projection matrix */
	k = 0.5 / ogl_aspect_ratio( );
	// Reuse proj matrix from the "FSV" drawing
	glm_ortho(0.0, 1.0, - k, k, -1.0, 1.0, proj);
	bottom_y = - k;

	/* Set up modelview matrix */
	// Modelview is the identity, so mvp is just the projection matrix.
	text_upload_mvp((float*) proj);

	text_pre( );

	/* Title */
	text_set_color(1.0, 1.0, 1.0);
	text_pos.x = 0.2059;
	text_pos.y = -0.1700;
	text_pos.z = 0.0;
	text_dims.x = 0.9;
	text_dims.y = 0.0625;
	text_draw_straight( "File", &text_pos, &text_dims );
	text_pos.x = 0.4449;
	text_draw_straight( "System", &text_pos, &text_dims );
	text_pos.x = 0.7456;
	text_draw_straight( "Visualizer", &text_pos, &text_dims );

	/* Version */
	text_set_color(0.75, 0.75, 0.75);
	text_pos.x = 0.5000;
	text_pos.y = (2.0 - MAGIC_NUMBER) * (0.2247 + bottom_y) - 0.2013;
	text_dims.y = 0.0386;
	text_draw_straight( "Version " VERSION, &text_pos, &text_dims );

	/* Copyright/author info */
	text_set_color(0.5, 0.5, 0.5);
	text_pos.y = bottom_y + 0.0417;
	text_dims.y = 0.0234;
	text_draw_straight( "Copyright (C)1999 Daniel Richard G. <skunk@mit.edu>", &text_pos, &text_dims );
	text_pos.y = bottom_y + 0.0117;
	text_draw_straight("Copyright (C) 2021 Janne Blomqvist", &text_pos, &text_dims);

	text_post( );
}


/* Top-level call to draw viewport content */
void
geometry_draw( boolean high_detail )
{
	if (about( ABOUT_CHECK )) {
		/* Currently giving About presentation */
		if (high_detail)
			about( ABOUT_DRAW );
		return;
	}

	switch (globals.fsv_mode) {
		case FSV_SPLASH:
		splash_draw( );
		break;

		case FSV_MAPV:
		mapv_draw( high_detail );
		break;

		case FSV_TREEV:
		treev_draw( high_detail );
		break;

		SWITCH_FAIL
	}
}


/* This gets called upon completion of a camera pan */
void
geometry_camera_pan_finished( void )
{
	switch (globals.fsv_mode) {
		case FSV_MAPV:
		mapv_camera_pan_finished( );
		break;

		case FSV_TREEV:
		treev_camera_pan_finished( );
		break;

		SWITCH_FAIL
	}
}


/* This is called when a directory is about to collapse or expand */
void
geometry_colexp_initiated( GNode *dnode )
{
	g_assert( NODE_IS_DIR(dnode) );

	/* A newly expanding directory in TreeV mode will probably
	 * need (re)shaping (it may be appearing for the first time,
	 * or its inner radius may have changed) */
	if (DIR_COLLAPSED(dnode) && (globals.fsv_mode == FSV_TREEV))
		treev_reshape_platform( dnode, geometry_treev_platform_r0( dnode ) );
	if (globals.fsv_mode == FSV_TREEV)
		treev_queue_rearrange( dnode );
}


/* This is called as a directory collapses or expands (and also when it
 * finishes either operation) */
void
geometry_colexp_in_progress( GNode *dnode )
{
	g_assert( NODE_IS_DIR(dnode) );

	/* Check geometry status against deployment. If they don't concur
	 * properly, then directory geometry has to be rebuilt */
        if (DIR_NODE_DESC(dnode)->geom_expanded != (DIR_NODE_DESC(dnode)->deployment > EPSILON))
		geometry_queue_rebuild( dnode );
        else
		queue_uncached_draw( );

	if (globals.fsv_mode == FSV_TREEV) {
		/* Take care of shifting angles */
		treev_queue_rearrange( dnode );
	}
}


/* Re-arrange immediately using the current interpolated deployment values
 * before colexp( ) computes a camera target. This is the current animation
 * position, not the eventual endpoint; geometry_treev_follow_update( ) keeps
 * the camera attached as later frames change the deployment values. */
void
geometry_treev_force_rearrange( void )
{
	if (globals.fsv_mode == FSV_TREEV)
		treev_arrange( FALSE );
}


/* Apply each animation step to the TreeV layout before rendering, then
 * move the automatic camera target by the focused node's actual delta. */
void
geometry_treev_follow_update( void )
{
	if (globals.fsv_mode != FSV_TREEV || !camera_treev_follow_active())
		return;
	treev_arrange(FALSE);
	camera_treev_follow_layout();
}


/* This tells if the specified node should be highlighted.  */
boolean
geometry_should_highlight(GNode *node)
{
	if (!NODE_IS_DIR(node))
		return TRUE;

	switch (globals.fsv_mode) {
		case FSV_MAPV:
		return DIR_COLLAPSED(node);

		case FSV_TREEV:
		return geometry_treev_is_leaf( node );

		SWITCH_FAIL
	}

	return FALSE;
}


/* Draws a single node, in its absolute position */
__attribute__((unused)) static void
draw_node( GNode *node )
{
	mat4 tmpmat;
	glm_mat4_copy(gl.modelview, tmpmat);

	switch (globals.fsv_mode) {
		case FSV_MAPV:
		glm_translate(gl.modelview, (vec3){0.0f, 0.0f, geometry_mapv_node_z0(node)});
		ogl_upload_matrices(TRUE);
		mapv_gldraw_node( node );
		break;

		case FSV_TREEV:
		if (geometry_treev_is_leaf( node )) {
			glm_rotate_z(gl.modelview, geometry_treev_platform_theta(node->parent) * M_PI/180, gl.modelview);
			ogl_upload_matrices(TRUE);
			treev_gldraw_leaf( node, geometry_treev_platform_r0( node->parent ), TRUE );
		}
		else {
			glm_rotate_z(gl.modelview, geometry_treev_platform_theta(node) * M_PI/180, gl.modelview);
			ogl_upload_matrices(TRUE);
			treev_gldraw_platform( node, geometry_treev_platform_r0( node ) );
		}
		break;

		SWITCH_FAIL
	}

	glm_mat4_copy(tmpmat, gl.modelview);
	ogl_upload_matrices(FALSE);
}


/* Highlights a node. "strong" flag indicates whether a noticeably heavier
   highlight should be drawn (CURRENTLY NOT USED). Passing NULL as the node
   argument clears the current highlight. */
void
geometry_highlight_node( GNode *node, boolean strong )
{
	GLuint new_highlight_id = node ? NODE_DESC(node)->id : 0;
	(void)strong;
	if (new_highlight_id == highlight_node_id)
		return;

	highlight_node_id = new_highlight_id;
	redraw();
}


/* Frees all allocated GL resources for the subtree rooted at the
 * specified directory node */
void
geometry_free_recursive( GNode *dnode )
{
	//DirNodeDesc *dir_ndesc;
	GNode *node;

	g_assert( NODE_IS_DIR(dnode) || NODE_IS_METANODE(dnode) );
	if (mapv_draw_rows)
		g_hash_table_remove(mapv_draw_rows, dnode);
	if (mapv_draw_peak_heights)
		g_hash_table_remove(mapv_draw_peak_heights, dnode);

	//dir_ndesc = DIR_NODE_DESC(dnode);

	// TODO delete VBO's?

	/* Recurse into subdirectories */
	node = dnode->children;
	while (node != NULL) {
		if (NODE_IS_DIR(node))
			geometry_free_recursive( node );
		else
			break;
		node = node->next;
	}
}


/* end geometry.c */
