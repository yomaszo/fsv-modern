/* tmaptext.c */

/* Texture-mapped text */

/* fsv - 3D File System Visualizer
 * Copyright (C)1999 Daniel Richard G. <skunk@mit.edu>
 * SPDX-FileCopyrightText: 2021 Janne Blomqvist <blomqvist.janne@gmail.com>
 * SPDX-FileCopyrightText: 2026 Contributors (dynamic Pango/Cairo glyph atlas)
 *
 * SPDX-License-Identifier:  LGPL-2.1-or-later
 */


#include "common.h"
#include "tmaptext.h"

#include "ogl.h"

#include <gio/gio.h>
#include <glib.h>
#include <stdint.h>
#include <cairo.h>
#include <pango/pangocairo.h>

/* ---------------------------------------------------------------------
 * Dynamic glyph atlas
 *
 * Rather than a fixed, hand-drawn bitmap font (the old xmaps/charset.xbm,
 * limited to ASCII plus a handful of manually added accented letters),
 * text is now rendered on demand from a real font via Pango + Cairo, one
 * Unicode code point at a time, into fixed-size cells of a single large
 * OpenGL texture (the "atlas"). Each code point is rendered at most once
 * per run; after that its texture coordinates are served from a cache.
 *
 * This supports any script the system's fonts cover -- Hungarian,
 * German, French, Cyrillic, CJK, etc. -- with no manual glyph-drawing
 * required, and everything lines up on a single shared baseline because
 * every glyph is positioned using the font's own metrics rather than
 * its individual ink extents.
 * ------------------------------------------------------------------- */

/* Size (in pixels) of the square texture atlas */
#define ATLAS_SIZE 1024

/* Size (in pixels) of the cell each glyph is rendered into. All glyphs
 * occupy a cell of this size regardless of the font's natural advance
 * width, so the rest of this file can keep treating text as a simple
 * monospace grid (matching the original bitmap-font behavior) */
#define CELL_W 32
#define CELL_H 64

#define ATLAS_COLS (ATLAS_SIZE / CELL_W)
#define ATLAS_ROWS (ATLAS_SIZE / CELL_H)
#define ATLAS_MAX_GLYPHS (ATLAS_COLS * ATLAS_ROWS)

/* Font used to render glyphs. Any installed monospace font works; bold
 * is used to stay visually close to the old bitmap font's weight */
#define GLYPH_FONT_DESC "Monospace Bold 36"

/* Fallback glyph shown for anything that fails to decode */
#define FALLBACK_CODEPOINT ((gunichar)'?')


/* Text can be squeezed to at most half its normal width */
#define TEXT_MAX_SQUEEZE 2.0


/* Normal character (cell) aspect ratio -- unchanged in spirit from the
 * old fixed bitmap font, just driven by the atlas cell size now */
static const double char_aspect_ratio = (double)CELL_W / (double)CELL_H;

/* Font texture object (the atlas) */
static GLuint text_tobj;

/* Pango font description, created once and reused for every glyph */
static PangoFontDescription *glyph_font_desc;

/* Vertical pixel offset (within a cell) at which every glyph is drawn,
 * computed once from the font's own metrics so that every glyph shares
 * the same baseline regardless of its individual shape */
static double glyph_baseline_y;

/* Texture-space rectangle for one cached glyph */
typedef struct {
	float u0, v0, u1, v1;
} GlyphUV;

/* code point -> GlyphUV */
static GHashTable *glyph_cache;

/* Index of the next free cell in the atlas (simple bump allocator --
 * more than enough cells exist for any realistic mix of scripts in one
 * session; see glyph_cache_lookup() for the overflow fallback) */
static int next_free_slot = 0;


// Global state for modern GL
static struct FsvGlTextState {
	// Don't need a VAO as we just use the one global VAO which is
	// always active.

	GLuint program; // Handle for the shaders

	// These _location variables are handles to input 'slots' in the
	// vertex shader.
	GLint mvp_location;
	GLint position_location;
	GLint texcoord_location;
	GLint texture_location;
	GLint color_location;

	// Projection and modelview matrices (using cglm library) from the
	// global state.
} glt;

typedef struct {
	GLfloat position[3];
	GLfloat texCoord[2];
} TextVertex;


// Initialize OpenGL text shaders
static GLuint
text_init_shaders()
{
	GBytes *source;
	GLuint program = 0, vertex = 0, fragment = 0;

	/* load the vertex shader */
	source = g_resources_lookup_data("/jabl/fsv/fsv-text-vertex.glsl", 0, NULL);
	vertex = ogl_create_shader(GL_VERTEX_SHADER, g_bytes_get_data(source, NULL));
	g_bytes_unref(source);
	if (vertex == 0)
		goto out;

	/* load the fragment shader */
	source = g_resources_lookup_data("/jabl/fsv/fsv-text-fragment.glsl", 0, NULL);
	fragment = ogl_create_shader(GL_FRAGMENT_SHADER, g_bytes_get_data(source, NULL));
	g_bytes_unref(source);
	if (fragment == 0)
		goto out;

	/* link the vertex and fragment shaders together */
	program = glCreateProgram();
	glAttachShader(program, vertex);
	glAttachShader(program, fragment);
	glLinkProgram(program);

	GLint status = 0;
	glGetProgramiv(program, GL_LINK_STATUS, &status);
	if (status == GL_FALSE)
	{
		GLint log_len = 0;
		glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_len);

		char *buffer = g_malloc(log_len + 1);
		glGetProgramInfoLog(program, log_len, NULL, buffer);

		g_error("Linking failure in program: %s", buffer);

		g_free(buffer);

		glDeleteProgram(program);
		program = 0;

		goto out;
	}

	/* get the location of the "mvp" uniform */
	glt.mvp_location = glGetUniformLocation(program, "mvp");
	glt.color_location = glGetUniformLocation(program, "color");
	glt.texture_location = glGetUniformLocation(program, "tex");

	/* get the location of the "position" and "color" attributes */
	glt.position_location = glGetAttribLocation(program, "position");
	glt.texcoord_location = glGetAttribLocation(program, "texcoord");

	/* the individual shaders can be detached and destroyed */
	glDetachShader(program, vertex);
	glDetachShader(program, fragment);

out:
	if (vertex != 0)
		glDeleteShader(vertex);
	if (fragment != 0)
		glDeleteShader(fragment);

	return program;
}


/* Computes glyph_baseline_y from the chosen font's own metrics. Using
 * font metrics (rather than each glyph's individual ink extents) is
 * what guarantees every glyph -- accented or not, tall or short --
 * lines up on the same baseline when placed side by side */
static void
glyph_metrics_init( void )
{
	cairo_surface_t *tmp_surface;
	cairo_t *tmp_cr;
	PangoLayout *tmp_layout;
	PangoContext *pango_ctx;
	PangoFontMetrics *metrics;
	double ascent_px, descent_px, line_height_px;

	tmp_surface = cairo_image_surface_create( CAIRO_FORMAT_A8, 1, 1 );
	tmp_cr = cairo_create( tmp_surface );
	tmp_layout = pango_cairo_create_layout( tmp_cr );
	pango_layout_set_font_description( tmp_layout, glyph_font_desc );

	pango_ctx = pango_layout_get_context( tmp_layout );
	metrics = pango_context_get_metrics( pango_ctx, glyph_font_desc, NULL );

	ascent_px = (double)pango_font_metrics_get_ascent( metrics ) / PANGO_SCALE;
	descent_px = (double)pango_font_metrics_get_descent( metrics ) / PANGO_SCALE;
	line_height_px = ascent_px + descent_px;

	/* Center the font's own line box vertically within the cell; if
	 * the font is too large for the cell this clamps to the top
	 * rather than producing a negative offset */
	glyph_baseline_y = MAX( 0.0, (CELL_H - line_height_px) / 2.0 );

	pango_font_metrics_unref( metrics );
	g_object_unref( tmp_layout );
	cairo_destroy( tmp_cr );
	cairo_surface_destroy( tmp_surface );
}


/* Renders a single Unicode code point into the given atlas cell
 * (identified by its pixel-space top-left corner) and uploads it into
 * the atlas texture. Assumes text_tobj is already bound */
static void
glyph_render_to_atlas( gunichar cp, int cell_px, int cell_py )
{
	cairo_surface_t *surface;
	cairo_t *cr;
	PangoLayout *layout;
	char utf8_buf[6];
	int utf8_len;
	int logical_w, logical_h;
	byte *alpha_pixels;
	unsigned char *cairo_data;
	int stride;
	int x, y;

	surface = cairo_image_surface_create( CAIRO_FORMAT_ARGB32, CELL_W, CELL_H );
	cr = cairo_create( surface );

	/* Transparent background */
	cairo_set_operator( cr, CAIRO_OPERATOR_CLEAR );
	cairo_paint( cr );
	cairo_set_operator( cr, CAIRO_OPERATOR_OVER );

	/* Solid white glyph -- the fragment shader tints it using the
	 * color uniform, same convention the old bitmap font used */
	cairo_set_source_rgba( cr, 1.0, 1.0, 1.0, 1.0 );

	utf8_len = g_unichar_to_utf8( cp, utf8_buf );

	layout = pango_cairo_create_layout( cr );
	pango_layout_set_font_description( layout, glyph_font_desc );
	pango_layout_set_text( layout, utf8_buf, utf8_len );
	pango_layout_get_pixel_size( layout, &logical_w, &logical_h );

	cairo_move_to( cr, (CELL_W - logical_w) / 2.0, glyph_baseline_y );
	pango_cairo_show_layout( cr, layout );

	g_object_unref( layout );
	cairo_surface_flush( surface );

	/* Extract the alpha channel (glyph coverage) into a plain
	 * single-byte-per-pixel buffer for glTexSubImage2D. cairo's
	 * ARGB32 is premultiplied, but since we drew with full-white,
	 * full-alpha source, alpha alone already gives us the coverage */
	cairo_data = cairo_image_surface_get_data( surface );
	stride = cairo_image_surface_get_stride( surface );
	alpha_pixels = NEW_ARRAY(byte, CELL_W * CELL_H);
	for (y = 0; y < CELL_H; y++) {
		uint32_t *row = (uint32_t *)(cairo_data + y * stride);
		for (x = 0; x < CELL_W; x++) {
			uint32_t px = row[x];
			alpha_pixels[y * CELL_W + x] = (byte)((px >> 24) & 0xFF);
		}
	}

	glTexSubImage2D( GL_TEXTURE_2D, 0, cell_px, cell_py, CELL_W, CELL_H,
			  GL_RED, GL_UNSIGNED_BYTE, alpha_pixels );

	xfree( alpha_pixels );
	cairo_destroy( cr );
	cairo_surface_destroy( surface );
}


/* Returns the texture-space coordinates of the bottom-left and
 * upper-right corners of the glyph for the given Unicode code point,
 * rendering and caching it first if this is the first time it's been
 * requested */
static void
glyph_cache_lookup( gunichar cp, XYvec *t_c0, XYvec *t_c1 )
{
	GlyphUV *uv;
	int slot, col, row, px, py;

	uv = g_hash_table_lookup( glyph_cache, GUINT_TO_POINTER(cp) );
	if (!uv) {
		if (next_free_slot >= ATLAS_MAX_GLYPHS) {
			/* Atlas is full (extremely unlikely in one
			 * session) -- fall back to '?' rather than
			 * growing the atlas or evicting anything */
			uv = g_hash_table_lookup( glyph_cache, GUINT_TO_POINTER(FALLBACK_CODEPOINT) );
			g_assert( uv != NULL );
		}
		else {
			slot = next_free_slot++;
			col = slot % ATLAS_COLS;
			row = slot / ATLAS_COLS;
			px = col * CELL_W;
			py = row * CELL_H;

			glBindTexture( GL_TEXTURE_2D, text_tobj );
			glyph_render_to_atlas( cp, px, py );
			glGenerateMipmap( GL_TEXTURE_2D );

			uv = g_new(GlyphUV, 1);
			uv->u0 = (float)px / (float)ATLAS_SIZE;
			uv->v0 = (float)(py + CELL_H) / (float)ATLAS_SIZE;
			uv->u1 = (float)(px + CELL_W) / (float)ATLAS_SIZE;
			uv->v1 = (float)py / (float)ATLAS_SIZE;
			g_hash_table_insert( glyph_cache, GUINT_TO_POINTER(cp), uv );
		}
	}

	t_c0->x = uv->u0;
	t_c0->y = uv->v0;
	t_c1->x = uv->u1;
	t_c1->y = uv->v1;
}


/* Initializes texture-mapping state for drawing text */
void
text_init( void )
{
	float border_color[] = { 0.0, 0.0, 0.0, 1.0 };

	/* Set up text texture object -- starts out empty; glyphs are
	 * rendered into it lazily, on first use, by glyph_cache_lookup() */
	glGenTextures( 1, &text_tobj );
	glBindTexture( GL_TEXTURE_2D, text_tobj );

	/* Set up texture-mapping parameters */
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR );

	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameterfv( GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border_color );

	glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
	// In modern GL GL_RED is the only supported single channel texture format.
	// But actually the texture is an alpha map that decides where the color
	// (specified via a uniform) will be shown and where it will be transparent.
	// In the fragment shader the red component in the texture is swizzled to
	// the alpha component of the output color.
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, ATLAS_SIZE, ATLAS_SIZE,
		     0, GL_RED, GL_UNSIGNED_BYTE, NULL);

	glyph_font_desc = pango_font_description_from_string( GLYPH_FONT_DESC );
	glyph_metrics_init( );

	glyph_cache = g_hash_table_new_full( g_direct_hash, g_direct_equal, NULL, g_free );
	next_free_slot = 0;

	/* Pre-cache the fallback glyph so glyph_cache_lookup() always
	 * has something to fall back to, even in the (extremely
	 * unlikely) event the atlas fills up completely */
	{
		XYvec dummy0, dummy1;
		glyph_cache_lookup( FALLBACK_CODEPOINT, &dummy0, &dummy1 );
	}

	glt.program = text_init_shaders();
	if (!glt.program)
		g_error("Compiling shaders failed");
}


/* Call before drawing text */
void
text_pre( void )
{
	glDisable( GL_POLYGON_OFFSET_FILL );
	glEnable( GL_BLEND );
	glBindTexture( GL_TEXTURE_2D, text_tobj );
}


/* Call after drawing text */
void
text_post( void )
{
	glDisable( GL_BLEND );
	glEnable( GL_POLYGON_OFFSET_FILL );
	glBindTexture(GL_TEXTURE_2D, 0);
}


/* Given the length of a string, and the dimensions into which that string
 * has to be rendered, this returns the dimensions that should be used
 * for each character */
static void
get_char_dims( int len, const XYvec *max_dims, XYvec *cdims )
{
	double min_width, max_width;

	/* Maximum and minimum widths of the string if it were to occupy
	 * the full depth (y-dimension) available to it */
	max_width = (double)len * max_dims->y * char_aspect_ratio;
	min_width = max_width / TEXT_MAX_SQUEEZE;

	if (max_width > max_dims->x) {
		if (min_width > max_dims->x) {
			/* Text will span full avaiable width, squeezed
			 * horizontally as much as it can take */
			cdims->x = max_dims->x / (double)len;
			cdims->y = TEXT_MAX_SQUEEZE * cdims->x / char_aspect_ratio;
		}
		else {
			/* Text will occupy full available width and
			 * height, squeezed horizontally a bit */
			cdims->x = max_dims->x / (double)len;
			cdims->y = max_dims->y;
		}
	}
	else {
		/* Text will use full available height (characters
		 * will have their natural aspect ratio) */
		cdims->y = max_dims->y;
		cdims->x = cdims->y * char_aspect_ratio;
	}
}


/* Decodes a UTF-8 string into an array of Unicode code points -- one
 * per displayed character, not per byte. Any invalid byte sequence
 * decodes to the fallback code point. "codepoints" must be able to
 * hold at least strlen(text)+1 entries (a safe upper bound, since a
 * string can never decode to more code points than it has bytes).
 * Returns the number of code points written */
static size_t
utf8_decode_codepoints( const char *text, gunichar *codepoints, size_t max_codepoints )
{
	const unsigned char *p = (const unsigned char *)text;
	size_t n = 0;

	while (*p && n < max_codepoints) {
		gunichar cp;
		int char_len;

		if (!g_utf8_validate( (const char *)p, -1, NULL )) {
			/* Whole remaining tail is invalid; bail one byte
			 * at a time so we still make progress */
			codepoints[n++] = FALLBACK_CODEPOINT;
			p++;
			continue;
		}

		cp = g_utf8_get_char( (const char *)p );
		char_len = g_utf8_next_char( (const char *)p ) - (const char *)p;

		codepoints[n++] = cp;
		p += char_len;
	}

	return n;
}


// Draw a set of text vertices with a specified color.
// Use indexed drawing.
// The vertices for each char must be in order
// LL - LR - UL - UR
static void
draw_text_vertices(TextVertex *tv, size_t nchars)
{
	GLsizeiptr ntv = nchars * 4;
	GLsizei idx_len = nchars * 6;
	static GLuint vbo;
	if (!vbo)
		glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(TextVertex) * ntv, tv, GL_STREAM_DRAW);

	glEnableVertexAttribArray(glt.position_location);
	glVertexAttribPointer(glt.position_location, 3, GL_FLOAT, GL_FALSE,
			      sizeof(TextVertex), (void *)offsetof(TextVertex, position));

	glEnableVertexAttribArray(glt.texcoord_location);
	glVertexAttribPointer(glt.texcoord_location, 2, GL_FLOAT, GL_FALSE,
			      sizeof(TextVertex), (void *)offsetof(TextVertex, texCoord));

	GLushort *idx = NEW_ARRAY(GLushort, idx_len);
	for (size_t i = 0; i < nchars; i++) {
		size_t j = 6 * i;  // 6 indices per char
		GLushort v = 4 * i;  // 4 Vertices per char
		// First triangle in a character
		idx[j] = v;
		idx[j + 1] = v + 1;
		idx[j + 2] = v + 2;
		// Second triangle (counter-clockwise order)
		idx[j + 3] = v + 2;
		idx[j + 4] = v + 1;
		idx[j + 5] = v + 3;
	}

	static GLuint ebo;
	if (!ebo)
		glGenBuffers(1, &ebo);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(GLushort) * idx_len, idx, GL_STREAM_DRAW);

	glUseProgram(glt.program);

	// Set the texture unit. TODO. Move to text_init()?
	glUniform1i(glt.texture_location, 0);
	glDrawElements(GL_TRIANGLES, idx_len, GL_UNSIGNED_SHORT, 0);
	glUseProgram(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	xfree(idx);
}

/* Draws a straight line of text centered at the given position,
 * fitting within the dimensions specified */
void
text_draw_straight( const char *text, const XYZvec *text_pos, const XYvec *text_max_dims )
{
	XYvec cdims;
	XYvec t_c0, t_c1, c0, c1;
	size_t len;
	gunichar *codepoints;

	codepoints = NEW_ARRAY(gunichar, strlen( text ) + 1);
	len = utf8_decode_codepoints( text, codepoints, strlen( text ) + 1 );
	if (len == 0) {
		xfree( codepoints );
		return;
	}

	get_char_dims( len, text_max_dims, &cdims );

	/* Corners of first character */
	c0.x = text_pos->x - 0.5 * (double)len * cdims.x;
	c0.y = text_pos->y - 0.5 * cdims.y;
	c1.x = c0.x + cdims.x;
	c1.y = c0.y + cdims.y;

	GLsizeiptr nverts = len * 4;
	TextVertex *tv = NEW_ARRAY(TextVertex, nverts);
	for (size_t i = 0; i < len; i++) {
		glyph_cache_lookup( codepoints[i], &t_c0, &t_c1 );
		size_t j = i * 4;
		// Each char defined by corners in zigzag order
		// Lower left {pos, texcoords}
		tv[j] = (TextVertex){{c0.x, c0.y, text_pos->z}, {t_c0.x, t_c0.y}};
		// Lower right
		tv[j + 1] = (TextVertex){{c1.x, c0.y, text_pos->z}, {t_c1.x, t_c0.y}};
		// Upper left
		tv[j + 2] = (TextVertex){{c0.x, c1.y, text_pos->z}, {t_c0.x, t_c1.y}};
		// Upper right
		tv[j + 3] = (TextVertex){{c1.x, c1.y, text_pos->z}, {t_c1.x, t_c1.y}};

		c0.x = c1.x;
		c1.x += cdims.x;
	}
	draw_text_vertices(tv, len);
	xfree(tv);
	xfree(codepoints);
}


/* Draws a straight line of text centered at the given position, rotated
 * to be tangent to a circle around the origin, and fitting within the
 * dimensions specified (which are also rotated) */
void
text_draw_straight_rotated( const char *text, const RTZvec *text_pos, const XYvec *text_max_dims )
{
	XYvec cdims;
	XYvec t_c0, t_c1, c0, c1;
	XYvec hdelta, vdelta;
	double sin_theta, cos_theta;
	size_t len;
	gunichar *codepoints;

	codepoints = NEW_ARRAY(gunichar, strlen( text ) + 1);
	len = utf8_decode_codepoints( text, codepoints, strlen( text ) + 1 );
	if (len == 0) {
		xfree( codepoints );
		return;
	}

	get_char_dims( len, text_max_dims, &cdims );

	sin_theta = sin( RAD(text_pos->theta) );
	cos_theta = cos( RAD(text_pos->theta) );

	/* Vector to move from one character to the next */
	hdelta.x = sin_theta * cdims.x;
	hdelta.y = - cos_theta * cdims.x;
	/* Vector to move from bottom of character to top */
	vdelta.x = cos_theta * cdims.y;
	vdelta.y = sin_theta * cdims.y;

	/* Corners of first character */
	c0.x = cos_theta * text_pos->r - 0.5 * ((double)len * hdelta.x + vdelta.x);
	c0.y = sin_theta * text_pos->r - 0.5 * ((double)len * hdelta.y + vdelta.y);
	c1.x = c0.x + hdelta.x + vdelta.x;
	c1.y = c0.y + hdelta.y + vdelta.y;

	GLsizeiptr nverts = len * 4;
	TextVertex *tv = NEW_ARRAY(TextVertex, nverts);
	for (size_t i = 0; i < len; i++) {
		glyph_cache_lookup( codepoints[i], &t_c0, &t_c1 );
		size_t j = i * 4;
		// Lower left
		tv[j] = (TextVertex){{c0.x, c0.y, text_pos->z}, {t_c0.x, t_c0.y}};
		// Lower right
		tv[j + 1] = (TextVertex){{c0.x + hdelta.x, c0.y + hdelta.y, text_pos->z},
					 {t_c1.x, t_c0.y}};
		// Upper left
		tv[j + 2] = (TextVertex){{c1.x - hdelta.x, c1.y - hdelta.y, text_pos->z},
					 {t_c0.x, t_c1.y}};
		// Upper right
		tv[j + 3] = (TextVertex){{c1.x, c1.y, text_pos->z}, {t_c1.x, t_c1.y}};

		c0.x += hdelta.x;
		c0.y += hdelta.y;
		c1.x += hdelta.x;
		c1.y += hdelta.y;
	}
	draw_text_vertices(tv, len);
	xfree(tv);
	xfree(codepoints);
}


/* Draws a curved arc of text, occupying no more than the depth and arc
 * width specified. text_pos indicates outer edge (not center) of text */
void
text_draw_curved( const char *text, const RTZvec *text_pos, const RTvec *text_max_dims )
{
	XYvec straight_dims, cdims;
	XYvec char_pos, fwsl, bwsl;
	XYvec t_c0, t_c1;
	double char_arc_width, theta;
	double sin_theta, cos_theta;
	double text_r;
	size_t len;
	gunichar *codepoints;

	/* Convert curved dimensions to straight equivalent */
	straight_dims.x = (PI / 180.0) * text_pos->r * text_max_dims->theta;
	straight_dims.y = text_max_dims->r;

	codepoints = NEW_ARRAY(gunichar, strlen( text ) + 1);
	len = utf8_decode_codepoints( text, codepoints, strlen( text ) + 1 );
	if (len == 0) {
		xfree( codepoints );
		return;
	}

	get_char_dims( len, &straight_dims, &cdims );

	/* Radius of center of text line */
	text_r = text_pos->r - 0.5 * cdims.y;

	/* Arc width occupied by each character */
	char_arc_width = (180.0 / PI) * cdims.x / text_r;

	theta = text_pos->theta + 0.5 * (double)(len - 1) * char_arc_width;

	GLsizeiptr nverts = len * 4;
	TextVertex *tv = NEW_ARRAY(TextVertex, nverts);
	for (size_t i = 0; i < len; i++) {
		sin_theta = sin( RAD(theta) );
		cos_theta = cos( RAD(theta) );

		/* Center of character and deltas from center to corners */
		char_pos.x = cos_theta * text_r;
		char_pos.y = sin_theta * text_r;
		/* "forward slash / backward slash" */
		fwsl.x = 0.5 * (cdims.y * cos_theta + cdims.x * sin_theta);
		fwsl.y = 0.5 * (cdims.y * sin_theta - cdims.x * cos_theta);
		bwsl.x = 0.5 * (- cdims.y * cos_theta + cdims.x * sin_theta);
		bwsl.y = 0.5 * (- cdims.y * sin_theta - cdims.x * cos_theta);

		glyph_cache_lookup( codepoints[i], &t_c0, &t_c1 );
		size_t j = i * 4;
		// Lower left
		tv[j] = (TextVertex){{char_pos.x - fwsl.x, char_pos.y - fwsl.y, text_pos->z},
				     {t_c0.x, t_c0.y}};
		// Lower right
		tv[j + 1] = (TextVertex){{char_pos.x + bwsl.x, char_pos.y + bwsl.y, text_pos->z},
					 {t_c1.x, t_c0.y}};
		// Upper left
		tv[j + 2] = (TextVertex){{char_pos.x - bwsl.x, char_pos.y - bwsl.y, text_pos->z},
					 {t_c0.x, t_c1.y}};
		// Upper right
		tv[j + 3] = (TextVertex){{char_pos.x + fwsl.x, char_pos.y + fwsl.y, text_pos->z},
					 {t_c1.x, t_c1.y}};

		theta -= char_arc_width;
	}
	draw_text_vertices(tv, len);
	xfree(tv);
	xfree(codepoints);
}

// Set the text color
void
text_set_color(float red, float green, float blue)
{
	glUseProgram(glt.program);
	glUniform3f(glt.color_location, red, green, blue);
	glUseProgram(0);
}


// Upload MVP matrix
void
text_upload_mvp(float* mvp)
{
	glUseProgram(glt.program);
	glUniformMatrix4fv(glt.mvp_location, 1, GL_FALSE, mvp);
	glUseProgram(0);
}

/* end tmaptext.c */
