/* compositor.c — EGL/KHR_image_pixmap accelerated compositor for awm
 *
 * Architecture:
 *   - XCompositeRedirectSubwindows(root, CompositeRedirectManual) captures
 *     all root children into server-side pixmaps.
 *   - An overlay window (XCompositeGetOverlayWindow) is used as the EGL
 *     surface; a GL context is created on it and windows are rendered
 *     directly to it via textured quads.
 *   - Each window's XCompositeNameWindowPixmap is bound as a GL texture
 *     via EGL_KHR_image_pixmap + GL_OES_EGL_image (zero CPU copy, GPU
 *     compositing).  This replaces the old GLX_EXT_texture_from_pixmap
 *     (TFP) path.
 *   - XDamage tracks which windows have changed since the last repaint.
 *   - eglSwapInterval(1) enables vsync so frames are presented at display
 *     rate with no tearing.
 *   - Border rectangles for managed clients are drawn as GL quads in the
 *     same pass.
 *   - XRender is retained only for building the alpha-picture cache that
 *     was used for opacity; opacity is now handled by the GL blend equation
 *     directly (no XRender needed at runtime).
 *
 * Why EGL uses a dedicated second XCB connection (gl_xc):
 *   Mesa's DRI3/gallium backend sends XCB requests directly on the
 *   xcb_connection_t it is given.  Using a separate connection keeps Mesa's
 *   traffic off the main xc so the main sequence counter is never corrupted.
 *   EGL_PLATFORM_XCB_EXT (Mesa >= 21) takes an xcb_connection_t* directly,
 *   so no Xlib bridge is required.  Pixmap XIDs are server-side and valid on
 *   both connections (same X server), so EGLImageKHR creation works unchanged.
 *
 * Fallback:
 *   - If EGL_KHR_image_pixmap is unavailable the compositor falls back to
 *     the original XRender path (comp_do_repaint_xrender) so the WM still
 *     works on software-only X servers.
 *
 * Compile-time guard: the entire file is dead code unless -DCOMPOSITOR.
 */

#ifdef COMPOSITOR

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <xcb/xcb.h>
#include <xcb/composite.h>
#include <xcb/damage.h>
#include <xcb/render.h>
#include <xcb/xfixes.h>
#include <xcb/shape.h>
#include <xcb/xcb_renderutil.h>
#include <xcb/present.h>

/* EGL + GL — only included when -DCOMPOSITOR is active */
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <glib.h>

#include "awm.h"
#include "log.h"
#include "compositor.h"

/* -------------------------------------------------------------------------
 * Internal types
 * ---------------------------------------------------------------------- */

typedef struct CompWin {
	xcb_window_t win;
	Client      *client; /* NULL for override_redirect windows        */
	xcb_pixmap_t pixmap; /* XCompositeNameWindowPixmap result          */
	/* XRender path (fallback) */
	xcb_render_picture_t picture; /* XRenderCreatePicture on pixmap */
	/* GL/EGL path */
	EGLImageKHR egl_image; /* EGL image wrapping pixmap (KHR_image_pixmap) */
	GLuint      texture;   /* GL_TEXTURE_2D bound via EGL image            */
	xcb_damage_damage_t damage;
	int    x, y, w, h, bw; /* last known geometry                */
	int    depth;          /* window depth                              */
	int    argb;           /* depth == 32                               */
	double opacity;        /* 0.0 – 1.0                                 */
	int    redirected;     /* 0 = bypass (fullscreen/bypass-hint)    */
	int    hidden;         /* 1 = moved off-screen by showhide()        */
	int    ever_damaged;   /* 0 = no damage received yet (since map) */
	xcb_present_event_t present_eid; /* 0 = not subscribed to Present events */
	struct CompWin     *next;
} CompWin;

/* -------------------------------------------------------------------------
 * Module state (all static, no global pollution)
 * ---------------------------------------------------------------------- */

static struct {
	int          active;
	xcb_window_t overlay;

	/* ---- GL path (primary) ---- */
	int use_gl; /* 1 if EGL_KHR_image_pixmap available and ctx ok */
	xcb_connection_t *gl_xc; /* dedicated XCB connection for EGL/Mesa;
	                          * avoids Mesa's DRI3 XCB calls corrupting the
	                          * main xc's sequence counter                  */
	EGLDisplay egl_dpy; /* EGL display wrapping gl_xc                     */
	EGLContext egl_ctx; /* EGL/GL context                                  */
	EGLSurface egl_win; /* EGL surface wrapping comp.overlay               */
	GLuint     prog;    /* shader program                                  */
	GLuint     vbo;     /* quad vertex buffer                              */
	GLuint     vao;     /* vertex array object                             */
	/* uniform locations */
	GLint u_tex;
	GLint u_opacity;
	GLint u_flip_y; /* reserved: flip V coord (unused for EGL images)  */
	GLint u_solid;  /* 1 = draw solid colour quad (borders)            */
	GLint u_color;  /* solid colour (borders)                          */
	GLint u_rect;   /* x, y, w, h in pixels (cached)                   */
	GLint u_screen; /* screen width, height (cached)                   */
	/* EGL_KHR_image_pixmap function pointers (loaded at runtime) */
	PFNEGLCREATEIMAGEKHRPROC            egl_create_image;
	PFNEGLDESTROYIMAGEKHRPROC           egl_destroy_image;
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC egl_image_target_tex;
	/* EGL_EXT_buffer_age partial repaint ring buffer.
	 * Each slot holds the bounding box of one past frame's dirty region.
	 * ring_idx is the slot that will be written after the *next* swap. */
#define DAMAGE_RING_SIZE 6
	xcb_rectangle_t damage_ring[DAMAGE_RING_SIZE];
	int             ring_idx; /* next write position (0..DAMAGE_RING_SIZE-1) */
	int has_buffer_age;       /* 1 if EGL_EXT_buffer_age is available    */

	/* ---- XRender path (fallback) ---- */
	xcb_render_picture_t target; /* XRenderPicture on overlay */
	xcb_pixmap_t         back_pixmap;
	xcb_render_picture_t back; /* XRenderPicture on back_pixmap       */
	xcb_render_picture_t
	    alpha_pict[256]; /* pre-built 1×1 RepeatNormal solids   */

	/* ---- Shared state ---- */
	int   damage_ev_base;
	int   damage_err_base;
	int   xfixes_ev_base;
	int   xfixes_err_base;
	guint repaint_id;          /* GLib idle source id, 0 = none            */
	int   paused;              /* 1 = overlay hidden, repaints suppressed  */
	xcb_xfixes_region_t dirty; /* accumulated dirty region                 */
	CompWin            *windows;
	GMainContext       *ctx;
	/* Wallpaper support */
	xcb_atom_t           atom_rootpmap;
	xcb_atom_t           atom_esetroot;
	xcb_pixmap_t         wallpaper_pixmap;
	xcb_render_picture_t wallpaper_pict; /* XRender picture (fallback path) */
	EGLImageKHR wallpaper_egl_image; /* EGL image for wallpaper (GL)         */
	GLuint      wallpaper_texture;   /* GL texture for wallpaper (GL)        */
	/* XRender extension codes — needed for error whitelisting */
	int render_request_base;
	int render_err_base;
	/* XShape extension — optional */
	int has_xshape;
	int shape_ev_base;
	int shape_err_base;
	/* X Present extension — optional, used to detect DRI3/Present frames */
	int     has_present;    /* 1 if Present extension available */
	uint8_t present_opcode; /* major opcode for GenericEvent filter */
	xcb_present_event_t present_eid_next; /* monotonically incrementing EID */
	/* _NET_WM_CM_Sn selection ownership */
	xcb_window_t
	           cm_owner_win; /* utility window used to hold the CM selection */
	xcb_atom_t atom_cm_sn;   /* _NET_WM_CM_S<screen> atom                    */
	/* Per-window opacity atom */
	xcb_atom_t atom_net_wm_opacity; /* _NET_WM_WINDOW_OPACITY */
	/* XRender picture format cache (queried once in compositor_init) */
	const xcb_render_query_pict_formats_reply_t *render_formats;
} comp;

/* ---- compositor compile-time invariants ---- */
_Static_assert(sizeof(unsigned short) == 2,
    "unsigned short must be 16 bits for xcb_render_color_t alpha/channel "
    "field scaling");
_Static_assert(sizeof(short) == 2,
    "short must be 16 bits to match xcb_rectangle_t x and y field types");
_Static_assert(sizeof(xcb_pixmap_t) == sizeof(uint32_t),
    "xcb_pixmap_t must be 32 bits for format-32 property reads");

/* -------------------------------------------------------------------------
 * Forward declarations
 * ---------------------------------------------------------------------- */

static void                 comp_add_by_xid(xcb_window_t w);
static CompWin             *comp_find_by_xid(xcb_window_t w);
static CompWin             *comp_find_by_client(Client *c);
static void                 comp_free_win(CompWin *cw);
static void                 comp_refresh_pixmap(CompWin *cw);
static void                 comp_apply_shape(CompWin *cw);
static void                 comp_update_wallpaper(void);
static void                 schedule_repaint(void);
static void                 comp_do_repaint(void);
static void                 comp_do_repaint_gl(void);
static void                 comp_do_repaint_xrender(void);
static gboolean             comp_repaint_idle(gpointer data);
static xcb_render_picture_t make_alpha_picture(double a);

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

static xcb_render_picture_t
make_alpha_picture(double a)
{
	const xcb_render_pictforminfo_t *fi;
	xcb_pixmap_t                     pix;
	xcb_render_picture_t             pic;
	xcb_render_color_t               col;
	xcb_rectangle_t                  r    = { 0, 0, 1, 1 };
	uint32_t                         mask = XCB_RENDER_CP_REPEAT;
	uint32_t                         val  = XCB_RENDER_REPEAT_NORMAL;

	fi = xcb_render_util_find_standard_format(
	    comp.render_formats, XCB_PICT_STANDARD_A_8);
	pix = xcb_generate_id(xc);
	xcb_create_pixmap(xc, 8, pix, (xcb_drawable_t) root, 1, 1);
	pic = xcb_generate_id(xc);
	xcb_render_create_picture(
	    xc, pic, (xcb_drawable_t) pix, fi ? fi->id : 0, mask, &val);
	col.alpha = (uint16_t) (a * 0xffff);
	col.red = col.green = col.blue = 0;
	xcb_render_fill_rectangles(xc, XCB_RENDER_PICT_OP_SRC, pic, col, 1, &r);
	xcb_free_pixmap(xc, pix);
	return pic;
}

/* -------------------------------------------------------------------------
 * GL shader source
 * ---------------------------------------------------------------------- */

/* Vertex shader: maps pixel coordinates to NDC.
 * Uniforms: screen width/height are baked into the projection done here.
 * We pass (x, y, w, h) as per-draw uniforms and generate the quad inline. */
static const char *vert_src =
    "#version 130\n"
    "in vec2 a_pos;\n" /* unit quad [0,1]×[0,1]                   */
    "in vec2 a_uv;\n"
    "out vec2 v_uv;\n"
    "uniform vec4 u_rect;\n"   /* x, y, w, h in pixels (top-left origin)  */
    "uniform vec2 u_screen;\n" /* screen width, height                    */
    "uniform int  u_flip_y;\n" /* reserved: flip V if texture origin differs */
    "void main() {\n"
    "    vec2 px = u_rect.xy + a_pos * u_rect.zw;\n"
    "    gl_Position = vec4(\n"
    "        px.x / u_screen.x * 2.0 - 1.0,\n"
    "        1.0 - px.y / u_screen.y * 2.0,\n" /* Y-flip: screen top=0   */
    "        0.0, 1.0);\n"
    "    v_uv = (u_flip_y == 1) ? vec2(a_uv.x, 1.0 - a_uv.y) : a_uv;\n"
    "}\n";

/* Fragment shader: samples the window texture with opacity, or fills solid. */
static const char *frag_src =
    "#version 130\n"
    "in vec2 v_uv;\n"
    "out vec4 frag_color;\n"
    "uniform sampler2D u_tex;\n"
    "uniform float     u_opacity;\n"
    "uniform int       u_solid;\n"
    "uniform vec4      u_color;\n"
    "void main() {\n"
    "    if (u_solid == 1) {\n"
    "        frag_color = u_color;\n"
    "    } else {\n"
    "        vec4 c = texture(u_tex, v_uv).rgba;\n"
    /* Straight alpha: just scale alpha by opacity.  The blend equation
     * GL_SRC_ALPHA/GL_ONE_MINUS_SRC_ALPHA handles the rest. */
    "        c.a *= u_opacity;\n"
    "        frag_color = c;\n"
    "    }\n"
    "}\n";

/* -------------------------------------------------------------------------
 * GL init helpers
 * ---------------------------------------------------------------------- */

static GLuint
gl_compile_shader(GLenum type, const char *src)
{
	GLuint s  = glCreateShader(type);
	GLint  ok = 0;
	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char buf[512];
		glGetShaderInfoLog(s, sizeof(buf), NULL, buf);
		awm_warn("compositor: shader compile error: %s", buf);
		glDeleteShader(s);
		return 0;
	}
	return s;
}

static GLuint
gl_link_program(GLuint vert, GLuint frag)
{
	GLuint p  = glCreateProgram();
	GLint  ok = 0;
	glAttachShader(p, vert);
	glAttachShader(p, frag);
	glBindAttribLocation(p, 0, "a_pos");
	glBindAttribLocation(p, 1, "a_uv");
	glLinkProgram(p);
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) {
		char buf[512];
		glGetProgramInfoLog(p, sizeof(buf), NULL, buf);
		awm_warn("compositor: shader link error: %s", buf);
		glDeleteProgram(p);
		return 0;
	}
	return p;
}

/* Attempt to initialise the GL/EGL path.  Returns 0 on success, -1 if EGL
 * is unavailable (caller falls back to XRender). */
static int
comp_init_gl(void)
{
	const char *egl_exts;
	EGLConfig   cfg     = NULL;
	EGLint      num_cfg = 0;
	GLuint      vert = 0, frag = 0;

	/* Unit-quad geometry: two triangles covering [0,1]×[0,1] */
	static const float quad[] = {
		/* a_pos    a_uv */
		0.0f,
		0.0f,
		0.0f,
		0.0f,
		1.0f,
		0.0f,
		1.0f,
		0.0f,
		0.0f,
		1.0f,
		0.0f,
		1.0f,
		1.0f,
		1.0f,
		1.0f,
		1.0f,
	};

	/* --- Get EGL display wrapping the dedicated GL connection ----------
	 * Use EGL_PLATFORM_XCB_EXT (Mesa >= 21) to pass our xcb_connection_t
	 * directly to EGL — no Xlib involved.  Fall back to the legacy
	 * eglGetDisplay() cast if the function pointer is unavailable.
	 */
	{
		PFNEGLGETPLATFORMDISPLAYEXTPROC get_plat_dpy =
		    (PFNEGLGETPLATFORMDISPLAYEXTPROC) eglGetProcAddress(
		        "eglGetPlatformDisplayEXT");
		if (get_plat_dpy) {
			comp.egl_dpy =
			    get_plat_dpy(EGL_PLATFORM_XCB_EXT, comp.gl_xc, NULL);
			awm_debug("compositor: used eglGetPlatformDisplayEXT"
			          "(EGL_PLATFORM_XCB_EXT)");
		} else {
			comp.egl_dpy = eglGetDisplay((EGLNativeDisplayType) comp.gl_xc);
			awm_debug("compositor: eglGetPlatformDisplayEXT "
			          "unavailable, used legacy eglGetDisplay");
		}
	}
	if (comp.egl_dpy == EGL_NO_DISPLAY) {
		awm_warn("compositor: eglGetDisplay failed, "
		         "falling back to XRender");
		return -1;
	}

	{
		EGLint major = 0, minor = 0;
		if (!eglInitialize(comp.egl_dpy, &major, &minor)) {
			awm_warn("compositor: eglInitialize failed (0x%x), "
			         "falling back to XRender",
			    (unsigned int) eglGetError());
			comp.egl_dpy = EGL_NO_DISPLAY;
			return -1;
		}
		awm_debug("compositor: EGL %d.%d initialised", major, minor);
	}

	/* --- Check for required EGL extensions ----------------------------*/
	egl_exts = eglQueryString(comp.egl_dpy, EGL_EXTENSIONS);

	if (!egl_exts || !strstr(egl_exts, "EGL_KHR_image_pixmap")) {
		awm_warn("compositor: EGL_KHR_image_pixmap unavailable, "
		         "falling back to XRender");
		eglTerminate(comp.egl_dpy);
		comp.egl_dpy = EGL_NO_DISPLAY;
		return -1;
	}

	/* --- Load EGL/GL extension function pointers ----------------------*/
	comp.egl_create_image =
	    (PFNEGLCREATEIMAGEKHRPROC) eglGetProcAddress("eglCreateImageKHR");
	comp.egl_destroy_image =
	    (PFNEGLDESTROYIMAGEKHRPROC) eglGetProcAddress("eglDestroyImageKHR");
	comp.egl_image_target_tex =
	    (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC) eglGetProcAddress(
	        "glEGLImageTargetTexture2DOES");

	if (!comp.egl_create_image || !comp.egl_destroy_image ||
	    !comp.egl_image_target_tex) {
		awm_warn("compositor: EGL image extension procs not found, "
		         "falling back to XRender");
		eglTerminate(comp.egl_dpy);
		comp.egl_dpy = EGL_NO_DISPLAY;
		return -1;
	}

	/* --- Bind desktop OpenGL API (not GLES) ---------------------------*/
	if (!eglBindAPI(EGL_OPENGL_API)) {
		awm_warn("compositor: eglBindAPI(EGL_OPENGL_API) failed (0x%x), "
		         "falling back to XRender",
		    (unsigned int) eglGetError());
		eglTerminate(comp.egl_dpy);
		comp.egl_dpy = EGL_NO_DISPLAY;
		return -1;
	}

	/* --- Choose EGL config --------------------------------------------*/
	{
		EGLint attr[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
			EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8,
			EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
		if (!eglChooseConfig(comp.egl_dpy, attr, &cfg, 1, &num_cfg) ||
		    num_cfg == 0) {
			/* Retry without alpha requirement */
			EGLint attr2[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
				EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8,
				EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };
			if (!eglChooseConfig(comp.egl_dpy, attr2, &cfg, 1, &num_cfg) ||
			    num_cfg == 0) {
				awm_warn("compositor: no suitable EGL config found, "
				         "falling back to XRender");
				eglTerminate(comp.egl_dpy);
				comp.egl_dpy = EGL_NO_DISPLAY;
				return -1;
			}
		}
	}

	/* --- Create GL context --------------------------------------------*/
	{
		EGLint ctx_attr[] = { EGL_CONTEXT_MAJOR_VERSION, 2,
			EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE };
		comp.egl_ctx =
		    eglCreateContext(comp.egl_dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
	}
	if (comp.egl_ctx == EGL_NO_CONTEXT) {
		awm_warn("compositor: eglCreateContext failed (0x%x), "
		         "falling back to XRender",
		    (unsigned int) eglGetError());
		eglTerminate(comp.egl_dpy);
		comp.egl_dpy = EGL_NO_DISPLAY;
		return -1;
	}

	/* --- Create EGL window surface wrapping overlay -------------------*/
	comp.egl_win = eglCreateWindowSurface(
	    comp.egl_dpy, cfg, (EGLNativeWindowType) comp.overlay, NULL);
	if (comp.egl_win == EGL_NO_SURFACE) {
		awm_warn("compositor: eglCreateWindowSurface failed (0x%x), "
		         "falling back to XRender",
		    (unsigned int) eglGetError());
		eglDestroyContext(comp.egl_dpy, comp.egl_ctx);
		comp.egl_ctx = EGL_NO_CONTEXT;
		eglTerminate(comp.egl_dpy);
		comp.egl_dpy = EGL_NO_DISPLAY;
		return -1;
	}

	if (!eglMakeCurrent(
	        comp.egl_dpy, comp.egl_win, comp.egl_win, comp.egl_ctx)) {
		awm_warn("compositor: eglMakeCurrent failed (0x%x), "
		         "falling back to XRender",
		    (unsigned int) eglGetError());
		eglDestroySurface(comp.egl_dpy, comp.egl_win);
		eglDestroyContext(comp.egl_dpy, comp.egl_ctx);
		comp.egl_win = EGL_NO_SURFACE;
		comp.egl_ctx = EGL_NO_CONTEXT;
		eglTerminate(comp.egl_dpy);
		comp.egl_dpy = EGL_NO_DISPLAY;
		return -1;
	}

	/* Enable vsync */
	eglSwapInterval(comp.egl_dpy, 1);

	/* --- Compile shaders -----------------------------------------------*/
	vert = gl_compile_shader(GL_VERTEX_SHADER, vert_src);
	frag = gl_compile_shader(GL_FRAGMENT_SHADER, frag_src);
	if (!vert || !frag) {
		if (vert)
			glDeleteShader(vert);
		if (frag)
			glDeleteShader(frag);
		eglMakeCurrent(
		    comp.egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		eglDestroySurface(comp.egl_dpy, comp.egl_win);
		eglDestroyContext(comp.egl_dpy, comp.egl_ctx);
		comp.egl_win = EGL_NO_SURFACE;
		comp.egl_ctx = EGL_NO_CONTEXT;
		eglTerminate(comp.egl_dpy);
		comp.egl_dpy = EGL_NO_DISPLAY;
		return -1;
	}

	comp.prog = gl_link_program(vert, frag);
	glDeleteShader(vert);
	glDeleteShader(frag);
	if (!comp.prog) {
		eglMakeCurrent(
		    comp.egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		eglDestroySurface(comp.egl_dpy, comp.egl_win);
		eglDestroyContext(comp.egl_dpy, comp.egl_ctx);
		comp.egl_win = EGL_NO_SURFACE;
		comp.egl_ctx = EGL_NO_CONTEXT;
		eglTerminate(comp.egl_dpy);
		comp.egl_dpy = EGL_NO_DISPLAY;
		return -1;
	}

	/* Cache uniform locations */
	comp.u_tex     = glGetUniformLocation(comp.prog, "u_tex");
	comp.u_opacity = glGetUniformLocation(comp.prog, "u_opacity");
	comp.u_flip_y  = glGetUniformLocation(comp.prog, "u_flip_y");
	comp.u_solid   = glGetUniformLocation(comp.prog, "u_solid");
	comp.u_color   = glGetUniformLocation(comp.prog, "u_color");
	comp.u_rect    = glGetUniformLocation(comp.prog, "u_rect");
	comp.u_screen  = glGetUniformLocation(comp.prog, "u_screen");

	glUseProgram(comp.prog);
	glUniform1i(comp.u_tex, 0); /* texture unit 0 */
	glUseProgram(0);

	/* --- Build unit-quad VBO/VAO ----------------------------------------*/
	glGenVertexArrays(1, &comp.vao);
	glGenBuffers(1, &comp.vbo);
	glBindVertexArray(comp.vao);
	glBindBuffer(GL_ARRAY_BUFFER, comp.vbo);
	glBufferData(
	    GL_ARRAY_BUFFER, (GLsizeiptr) sizeof(quad), quad, GL_STATIC_DRAW);
	/* a_pos: 2 floats at offset 0, stride 4 floats */
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(
	    0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *) 0);
	/* a_uv: 2 floats at offset 2 floats */
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
	    (void *) (2 * sizeof(float)));
	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	/* --- GL state ---------------------------------------------------------*/
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_SCISSOR_TEST);
	glEnable(GL_BLEND);
	/* Straight (non-pre-multiplied) alpha blend.  X11 ARGB windows
	 * (terminals etc.) deliver straight alpha, so SRC_ALPHA is correct.
	 * Using GL_ONE caused colour fringing on sub-pixel font rendering. */
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glViewport(0, 0, sw, sh);

	comp.use_gl = 1;

	/* Detect EGL_EXT_buffer_age for partial repaints */
	comp.has_buffer_age =
	    (egl_exts && strstr(egl_exts, "EGL_EXT_buffer_age") != NULL);
	memset(comp.damage_ring, 0, sizeof(comp.damage_ring));
	comp.ring_idx = 0;

	awm_debug("compositor: EGL/GL path initialised (renderer: %s, "
	          "buffer_age=%d)",
	    (const char *) glGetString(GL_RENDERER), comp.has_buffer_age);
	return 0;
}

/* Create an EGLImageKHR from cw->pixmap and attach it to a GL texture.
 * Fills cw->egl_image and cw->texture.
 * Called after comp_refresh_pixmap() sets cw->pixmap. */
static void
comp_bind_tfp(CompWin *cw)
{
	EGLint img_attr[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };

	if (!comp.use_gl || !cw->pixmap)
		return;

	/* Release any existing EGL image / texture first */
	if (cw->texture) {
		glDeleteTextures(1, &cw->texture);
		cw->texture = 0;
	}
	if (cw->egl_image != EGL_NO_IMAGE_KHR) {
		comp.egl_destroy_image(comp.egl_dpy, cw->egl_image);
		cw->egl_image = EGL_NO_IMAGE_KHR;
	}

	/* Create EGL image from the X pixmap */
	cw->egl_image = comp.egl_create_image(comp.egl_dpy, EGL_NO_CONTEXT,
	    EGL_NATIVE_PIXMAP_KHR, (EGLClientBuffer) (uintptr_t) cw->pixmap,
	    img_attr);

	if (cw->egl_image == EGL_NO_IMAGE_KHR)
		return;

	/* Allocate GL texture and attach the EGL image */
	glGenTextures(1, &cw->texture);
	glBindTexture(GL_TEXTURE_2D, cw->texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	comp.egl_image_target_tex(GL_TEXTURE_2D, (GLeglImageOES) cw->egl_image);
	glBindTexture(GL_TEXTURE_2D, 0);
}

/* Release EGL image + GL texture for a CompWin.
 * Must be called before comp_free_win() when use_gl==1 so the EGL image
 * is destroyed before the underlying pixmap is freed. */
static void
comp_release_tfp(CompWin *cw)
{
	if (!comp.use_gl)
		return;

	if (cw->texture) {
		glDeleteTextures(1, &cw->texture);
		cw->texture = 0;
	}
	if (cw->egl_image != EGL_NO_IMAGE_KHR) {
		comp.egl_destroy_image(comp.egl_dpy, cw->egl_image);
		cw->egl_image = EGL_NO_IMAGE_KHR;
	}
}

/* -------------------------------------------------------------------------
 * compositor_init
 * ---------------------------------------------------------------------- */

int
compositor_init(GMainContext *ctx)
{
	int                                i;
	const xcb_query_extension_reply_t *ext;

	memset(&comp, 0, sizeof(comp));
	comp.ctx = ctx;

	/* --- Query/cache XRender picture formats (needed for all format lookups)
	 */
	comp.render_formats = xcb_render_util_query_formats(xc);
	if (!comp.render_formats) {
		awm_warn("compositor: xcb_render_util_query_formats failed");
		return -1;
	}

	/* --- Check required extensions ----------------------------------------
	 */

	ext = xcb_get_extension_data(xc, &xcb_composite_id);
	if (!ext || !ext->present) {
		awm_warn("compositor: XComposite extension not available");
		return -1;
	}
	{
		xcb_composite_query_version_cookie_t vck;
		xcb_composite_query_version_reply_t *vr;
		vck = xcb_composite_query_version(xc, 0, 2);
		vr  = xcb_composite_query_version_reply(xc, vck, NULL);
		if (!vr || (vr->major_version == 0 && vr->minor_version < 2)) {
			awm_warn("compositor: XComposite >= 0.2 required (got %d.%d)",
			    vr ? (int) vr->major_version : 0,
			    vr ? (int) vr->minor_version : 0);
			free(vr);
			return -1;
		}
		free(vr);
	}

	ext = xcb_get_extension_data(xc, &xcb_damage_id);
	if (!ext || !ext->present) {
		awm_warn("compositor: XDamage extension not available");
		return -1;
	}
	comp.damage_ev_base  = ext->first_event;
	comp.damage_err_base = ext->first_error;
	{
		xcb_damage_query_version_cookie_t dvck;
		xcb_damage_query_version_reply_t *dvr;
		dvck = xcb_damage_query_version(xc, 1, 1);
		dvr  = xcb_damage_query_version_reply(xc, dvck, NULL);
		free(dvr);
	}

	ext = xcb_get_extension_data(xc, &xcb_xfixes_id);
	if (!ext || !ext->present) {
		awm_warn("compositor: XFixes extension not available");
		return -1;
	}
	comp.xfixes_ev_base  = ext->first_event;
	comp.xfixes_err_base = ext->first_error;
	{
		xcb_xfixes_query_version_cookie_t fvck;
		xcb_xfixes_query_version_reply_t *fvr;
		fvck = xcb_xfixes_query_version(xc, 2, 0);
		fvr  = xcb_xfixes_query_version_reply(xc, fvck, NULL);
		free(fvr);
	}

	ext = xcb_get_extension_data(xc, &xcb_render_id);
	if (!ext || !ext->present) {
		awm_warn("compositor: XRender extension not available");
		return -1;
	}
	comp.render_err_base     = ext->first_error;
	comp.render_request_base = ext->major_opcode;

	/* Query GLX extension opcode for error whitelisting — EGL has no X
	 * request codes, but the glx_errors stub in compositor.h still exists
	 * to avoid changing events.c; it will always return -1 after this. */

	ext = xcb_get_extension_data(xc, &xcb_shape_id);
	if (ext && ext->present) {
		comp.has_xshape     = 1;
		comp.shape_ev_base  = ext->first_event;
		comp.shape_err_base = ext->first_error;
	}

	/* --- Query X Present extension (optional) ----------------------------
	 * Used to subscribe to PresentCompleteNotify so DRI3/Present GPU frames
	 * from Chrome/Chromium trigger a pixmap refresh and repaint rather than
	 * showing a frozen frame when the window is re-redirected after
	 * fullscreen bypass.
	 */
	{
		const xcb_query_extension_reply_t *pext =
		    xcb_get_extension_data(xc, &xcb_present_id);
		if (pext && pext->present) {
			comp.has_present      = 1;
			comp.present_opcode   = pext->major_opcode;
			comp.present_eid_next = 1; /* start EID allocation at 1 */
			awm_debug("compositor: X Present extension available "
			          "(opcode=%d)",
			    comp.present_opcode);
		}
	}

	/* --- Redirect all root children ---------------------------------------
	 */

	xcb_composite_redirect_subwindows(
	    xc, (xcb_window_t) root, XCB_COMPOSITE_REDIRECT_MANUAL);
	xcb_flush(xc);

	/* --- Overlay window ---------------------------------------------------
	 */

	{
		xcb_composite_get_overlay_window_cookie_t owck;
		xcb_composite_get_overlay_window_reply_t *owr;
		owck = xcb_composite_get_overlay_window(xc, (xcb_window_t) root);
		owr  = xcb_composite_get_overlay_window_reply(xc, owck, NULL);
		comp.overlay = owr ? owr->overlay_win : 0;
		free(owr);
	}
	if (!comp.overlay) {
		awm_warn("compositor: failed to get overlay window");
		xcb_composite_unredirect_subwindows(
		    xc, (xcb_window_t) root, XCB_COMPOSITE_REDIRECT_MANUAL);
		return -1;
	}

	/* Make the overlay click-through */
	{
		xcb_xfixes_region_t empty = xcb_generate_id(xc);
		xcb_xfixes_create_region(xc, empty, 0, NULL);
		xcb_xfixes_set_window_shape_region(
		    xc, (xcb_window_t) comp.overlay, XCB_SHAPE_SK_INPUT, 0, 0, empty);
		xcb_xfixes_destroy_region(xc, empty);
	}

	/* --- Try to initialise the EGL/GL path --------------------------------
	 * A dedicated XCB connection (gl_xc) is opened for all EGL/Mesa
	 * operations.  Because EGL_PLATFORM_XCB_EXT uses XCB natively, Mesa's
	 * DRI3 calls stay on this connection and never touch the main xc
	 * sequence counter.  Pixmap XIDs are server-side and valid on both
	 * connections (same X server), so EGLImageKHR creation works unchanged.
	 */
	comp.gl_xc = xcb_connect(NULL, NULL);
	if (!comp.gl_xc || xcb_connection_has_error(comp.gl_xc)) {
		if (comp.gl_xc) {
			xcb_disconnect(comp.gl_xc);
			comp.gl_xc = NULL;
		}
		awm_warn("compositor: xcb_connect for GL failed, "
		         "GL path unavailable");
	}

	if (comp.gl_xc && comp_init_gl() != 0) {
		/* GL path unavailable — set up XRender back-buffer + target */
		const xcb_render_pictvisual_t *pv;
		xcb_render_pictformat_t        fmt;
		uint32_t                       pict_mask;
		uint32_t                       pict_val;

		pv = xcb_render_util_find_visual_format(
		    comp.render_formats, xcb_screen_root_visual(xc, screen));
		fmt = pv ? pv->format : 0;

		pict_mask = XCB_RENDER_CP_SUBWINDOW_MODE;
		pict_val  = XCB_SUBWINDOW_MODE_INCLUDE_INFERIORS;

		comp.target = xcb_generate_id(xc);
		xcb_render_create_picture(xc, comp.target,
		    (xcb_drawable_t) comp.overlay, fmt, pict_mask, &pict_val);

		comp.back_pixmap = xcb_generate_id(xc);
		xcb_create_pixmap(xc, xcb_screen_root_depth(xc, screen),
		    comp.back_pixmap, (xcb_drawable_t) root, (uint16_t) sw,
		    (uint16_t) sh);

		comp.back = xcb_generate_id(xc);
		xcb_render_create_picture(xc, comp.back,
		    (xcb_drawable_t) comp.back_pixmap, fmt, pict_mask, &pict_val);

		for (i = 0; i < 256; i++)
			comp.alpha_pict[i] = make_alpha_picture((double) i / 255.0);
	}

	/* --- Dirty region (starts as full screen) -----------------------------
	 */

	{
		xcb_rectangle_t full = { 0, 0, (uint16_t) sw, (uint16_t) sh };
		comp.dirty           = xcb_generate_id(xc);
		xcb_xfixes_create_region(xc, comp.dirty, 1, &full);
	}

	/* --- Claim _NET_WM_CM_S<n> composite manager selection ---------------
	 * Required by ICCCM/EWMH: signals to applications that a compositor is
	 * running and lets us detect if another compositor starts
	 * (SelectionClear).
	 */
	{
		char                     sel_name[32];
		xcb_intern_atom_cookie_t ck;
		xcb_intern_atom_reply_t *r;
		snprintf(sel_name, sizeof(sel_name), "_NET_WM_CM_S%d", screen);
		ck = xcb_intern_atom(xc, 0, (uint16_t) strlen(sel_name), sel_name);
		r  = xcb_intern_atom_reply(xc, ck, NULL);
		comp.atom_cm_sn = r ? r->atom : XCB_ATOM_NONE;
		free(r);

		/* Create a small, invisible utility window to hold the selection. */
		{
			xcb_window_t win = xcb_generate_id(xc);
			xcb_create_window(xc, XCB_COPY_FROM_PARENT, win,
			    (xcb_window_t) root, -1, -1, 1, 1, 0,
			    XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, 0, NULL);
			comp.cm_owner_win = win;
		}

		xcb_set_selection_owner(xc, comp.cm_owner_win,
		    (xcb_atom_t) comp.atom_cm_sn, XCB_CURRENT_TIME);

		{
			xcb_get_selection_owner_cookie_t gck;
			xcb_get_selection_owner_reply_t *gr;
			gck = xcb_get_selection_owner(xc, (xcb_atom_t) comp.atom_cm_sn);
			gr  = xcb_get_selection_owner_reply(xc, gck, NULL);
			if (!gr || gr->owner != comp.cm_owner_win) {
				awm_warn("compositor: could not claim _NET_WM_CM_S%d — "
				         "another compositor may be running",
				    screen);
			} else {
				awm_debug(
				    "compositor: claimed _NET_WM_CM_S%d selection", screen);
			}
			free(gr);
		}

		/* Select SelectionClear on the owner window so we are notified
		 * if another program takes the selection from us. */
		{
			uint32_t evmask = StructureNotifyMask;
			xcb_change_window_attributes(
			    xc, comp.cm_owner_win, XCB_CW_EVENT_MASK, &evmask);
		}
	}

	/* --- Scan existing windows --------------------------------------------
	 */

	{
		xcb_query_tree_cookie_t qtck;
		xcb_query_tree_reply_t *qtr;
		uint32_t                j;

		qtck = xcb_query_tree(xc, root);
		qtr  = xcb_query_tree_reply(xc, qtck, NULL);
		if (qtr) {
			xcb_window_t *ch = xcb_query_tree_children(qtr);
			int           nc = xcb_query_tree_children_length(qtr);
			for (j = 0; j < (uint32_t) nc; j++)
				comp_add_by_xid(ch[j]);
			free(qtr);
		} else {
			awm_warn("compositor: xcb_query_tree failed on root during scan");
		}
	}

	/* --- Intern wallpaper atoms and read initial wallpaper ---------------
	 */

	{
		xcb_intern_atom_cookie_t ck0 =
		    xcb_intern_atom(xc, 0, 12, "_XROOTPMAP_ID");
		xcb_intern_atom_cookie_t ck1 =
		    xcb_intern_atom(xc, 0, 15, "ESETROOT_PMAP_ID");
		xcb_intern_atom_cookie_t ck2 =
		    xcb_intern_atom(xc, 0, 24, "_NET_WM_WINDOW_OPACITY");
		xcb_intern_atom_reply_t *r0 = xcb_intern_atom_reply(xc, ck0, NULL);
		xcb_intern_atom_reply_t *r1 = xcb_intern_atom_reply(xc, ck1, NULL);
		xcb_intern_atom_reply_t *r2 = xcb_intern_atom_reply(xc, ck2, NULL);
		comp.atom_rootpmap          = r0 ? r0->atom : XCB_ATOM_NONE;
		comp.atom_esetroot          = r1 ? r1->atom : XCB_ATOM_NONE;
		comp.atom_net_wm_opacity    = r2 ? r2->atom : XCB_ATOM_NONE;
		free(r0);
		free(r1);
		free(r2);
	}
	comp_update_wallpaper();

	comp.active = 1;

	/* Raise overlay so it sits above all windows */
	{
		uint32_t stack = XCB_STACK_MODE_ABOVE;
		xcb_configure_window(
		    xc, comp.overlay, XCB_CONFIG_WINDOW_STACK_MODE, &stack);
		xcb_map_window(xc, comp.overlay);
	}

	schedule_repaint();

	awm_debug("compositor: initialised (gl=%d damage_ev_base=%d)", comp.use_gl,
	    comp.damage_ev_base);
	return 0;
}

/* -------------------------------------------------------------------------
 * compositor_cleanup
 * ---------------------------------------------------------------------- */

void
compositor_cleanup(void)
{
	int      i;
	CompWin *cw, *next;

	if (!comp.active)
		return;

	if (comp.repaint_id) {
		g_source_remove(comp.repaint_id);
		comp.repaint_id = 0;
	}

	/* Free all tracked windows */
	for (cw = comp.windows; cw; cw = next) {
		next = cw->next;
		if (comp.use_gl)
			comp_release_tfp(cw);
		comp_free_win(cw);
		free(cw);
	}
	comp.windows = NULL;

	if (comp.use_gl) {
		/* Destroy GL resources */
		if (comp.prog)
			glDeleteProgram(comp.prog);
		if (comp.vao)
			glDeleteVertexArrays(1, &comp.vao);
		if (comp.vbo)
			glDeleteBuffers(1, &comp.vbo);
		eglMakeCurrent(
		    comp.egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		if (comp.egl_win != EGL_NO_SURFACE)
			eglDestroySurface(comp.egl_dpy, comp.egl_win);
		if (comp.egl_ctx != EGL_NO_CONTEXT)
			eglDestroyContext(comp.egl_dpy, comp.egl_ctx);
		if (comp.egl_dpy != EGL_NO_DISPLAY)
			eglTerminate(comp.egl_dpy);
	} else {
		/* XRender path cleanup */

		for (i = 0; i < 256; i++)
			if (comp.alpha_pict[i])
				xcb_render_free_picture(xc, comp.alpha_pict[i]);
		if (comp.back)
			xcb_render_free_picture(xc, comp.back);
		if (comp.back_pixmap)
			xcb_free_pixmap(xc, comp.back_pixmap);
		if (comp.target)
			xcb_render_free_picture(xc, comp.target);
	}

	{

		if (comp.wallpaper_pict)
			xcb_render_free_picture(xc, comp.wallpaper_pict);

		/* Release cached GL wallpaper resources */
		if (comp.use_gl) {
			if (comp.wallpaper_texture)
				glDeleteTextures(1, &comp.wallpaper_texture);
			if (comp.wallpaper_egl_image != EGL_NO_IMAGE_KHR)
				comp.egl_destroy_image(comp.egl_dpy, comp.wallpaper_egl_image);
		}

		if (comp.overlay)
			xcb_composite_release_overlay_window(xc, (xcb_window_t) root);

		/* Release _NET_WM_CM_Sn selection */
		if (comp.cm_owner_win) {
			xcb_destroy_window(xc, comp.cm_owner_win);
			comp.cm_owner_win = 0;
		}

		if (comp.dirty)
			xcb_xfixes_destroy_region(xc, comp.dirty);

		xcb_composite_unredirect_subwindows(
		    xc, (xcb_window_t) root, XCB_COMPOSITE_REDIRECT_MANUAL);

		/* Release xc-renderutil format cache */
		xcb_render_util_disconnect(xc);
	}
	xflush();

	/* Close the dedicated EGL/GL XCB connection last — after all EGL
	 * objects have been destroyed by eglTerminate above. */
	if (comp.gl_xc)
		xcb_disconnect(comp.gl_xc);

	comp.active = 0;
}

/* -------------------------------------------------------------------------
 * Window tracking — internal helpers
 * ---------------------------------------------------------------------- */

static CompWin *
comp_find_by_xid(xcb_window_t w)
{
	CompWin *cw;
	for (cw = comp.windows; cw; cw = cw->next)
		if (cw->win == w)
			return cw;
	return NULL;
}

static CompWin *
comp_find_by_client(Client *c)
{
	CompWin *cw;
	for (cw = comp.windows; cw; cw = cw->next)
		if (cw->client == c)
			return cw;
	return NULL;
}

/* Subscribe cw to XPresent CompleteNotify events so that GPU-rendered
 * frames via DRI3/Present (e.g. Chrome video) are detected even when
 * XDamageNotify is not generated.
 *
 * Each subscription needs a unique 32-bit event ID (eid).  We allocate
 * one from comp.present_eid_next.  The eid is stored in cw->present_eid
 * so we can unsubscribe later with the same value.
 *
 * Called when a CompWin is first created and when it is re-redirected
 * after fullscreen bypass (compositor_check_unredirect resume path). */
static void
comp_subscribe_present(CompWin *cw)
{

	if (!comp.has_present || cw->present_eid)
		return; /* already subscribed or extension absent */

	cw->present_eid = comp.present_eid_next++;

	xcb_present_select_input(xc, cw->present_eid, (xcb_window_t) cw->win,
	    XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY);
	xcb_flush(xc);

	awm_debug("compositor: subscribed Present CompleteNotify for "
	          "window 0x%lx (eid=%u)",
	    cw->win, cw->present_eid);
}

/* Unsubscribe from Present events for cw.  Safe to call on destroyed
 * windows — the server ignores requests on dead XIDs. */
static void
comp_unsubscribe_present(CompWin *cw)
{

	if (!comp.has_present || !cw->present_eid)
		return;

	xcb_present_select_input(xc, cw->present_eid, (xcb_window_t) cw->win,
	    XCB_PRESENT_EVENT_MASK_NO_EVENT);
	xcb_flush(xc);
	cw->present_eid = 0;
}

static void
comp_free_win(CompWin *cw)
{
	/* Deregister shape event mask before releasing other resources.
	 * This is a no-op if the window is already destroyed (X ignores it),
	 * but prevents a leak when comp_free_win is called on live windows
	 * (e.g. unmap of non-client override-redirect windows). */
	if (comp.has_xshape) {
		xcb_void_cookie_t    ck;
		xcb_generic_error_t *err;

		ck  = xcb_shape_select_input_checked(xc, (xcb_window_t) cw->win, 0);
		err = xcb_request_check(xc, ck);
		free(err); /* error intentionally discarded — window may be gone */
	}

	/* Unsubscribe Present events (no-op on dead windows). */
	comp_unsubscribe_present(cw);

	if (cw->damage) {
		xcb_void_cookie_t    ck;
		xcb_generic_error_t *err;

		ck  = xcb_damage_destroy_checked(xc, cw->damage);
		err = xcb_request_check(xc, ck);
		free(err); /* error intentionally discarded — damage may be gone */
		cw->damage = 0;
	}
	if (cw->picture) {
		xcb_render_free_picture(xc, cw->picture);
		cw->picture = None;
	}
	if (cw->pixmap) {
		xcb_free_pixmap(xc, cw->pixmap);
		cw->pixmap = None;
	}
}

static void
comp_refresh_pixmap(CompWin *cw)
{

	/* Release TFP resources before freeing the pixmap */
	if (comp.use_gl)
		comp_release_tfp(cw);

	if (cw->picture) {
		xcb_render_free_picture(xc, cw->picture);
		cw->picture = None;
	}
	if (cw->pixmap) {
		xcb_free_pixmap(xc, cw->pixmap);
		cw->pixmap = None;
	}

	/* New pixmap — require a full dirty on its first damage notification. */
	cw->ever_damaged = 0;

	{
		xcb_pixmap_t         pix = xcb_generate_id(xc);
		xcb_void_cookie_t    ck;
		xcb_generic_error_t *err;

		ck = xcb_composite_name_window_pixmap_checked(
		    xc, (xcb_window_t) cw->win, pix);
		xcb_flush(xc);
		err = xcb_request_check(xc, ck);
		free(err); /* error intentionally discarded — window may be gone */
		cw->pixmap = pix;
	}

	if (!cw->pixmap)
		return;

	{
		xcb_get_geometry_cookie_t gck;
		xcb_get_geometry_reply_t *gr;
		gck = xcb_get_geometry(xc, (xcb_drawable_t) cw->pixmap);
		gr  = xcb_get_geometry_reply(xc, gck, NULL);
		if (!gr) {
			awm_warn("compositor: pixmap geometry query failed — releasing "
			         "stale pixmap");
			xcb_free_pixmap(xc, cw->pixmap);
			cw->pixmap = None;
			return;
		}
		free(gr);
	}

	if (comp.use_gl) {
		/* Bind as GL texture via TFP */
		comp_bind_tfp(cw);
	} else {
		/* XRender fallback: create an XRender Picture */
		const xcb_render_pictvisual_t *pv;
		xcb_render_pictformat_t        fmt;
		uint32_t                       pmask;
		uint32_t                       pval;
		xcb_void_cookie_t              ck;
		xcb_generic_error_t           *err;

		pv = xcb_render_util_find_visual_format(
		    comp.render_formats, xcb_screen_root_visual(xc, screen));
		fmt = pv ? pv->format : 0;
		if (cw->argb) {
			const xcb_render_pictforminfo_t *fi =
			    xcb_render_util_find_standard_format(
			        comp.render_formats, XCB_PICT_STANDARD_ARGB_32);
			fmt = fi ? fi->id : fmt;
		}
		pmask       = XCB_RENDER_CP_SUBWINDOW_MODE;
		pval        = XCB_SUBWINDOW_MODE_INCLUDE_INFERIORS;
		cw->picture = xcb_generate_id(xc);
		ck          = xcb_render_create_picture_checked(
            xc, cw->picture, (xcb_drawable_t) cw->pixmap, fmt, pmask, &pval);
		xcb_flush(xc);
		err = xcb_request_check(xc, ck);
		free(err); /* error intentionally discarded — pixmap may be gone */
		comp_apply_shape(cw);
	}
}

/* Apply the window's ShapeBounding clip region to cw->picture.
 * Only used by the XRender fallback path. */
static void
comp_apply_shape(CompWin *cw)
{

	if (!cw->picture)
		return;

	if (!comp.has_xshape) {
		xcb_xfixes_set_picture_clip_region(xc, cw->picture, XCB_NONE, 0, 0);
		return;
	}

	{
		xcb_shape_get_rectangles_cookie_t sck;
		xcb_shape_get_rectangles_reply_t *sr;
		xcb_rectangle_t                  *rects;
		int                               nrects;

		sck = xcb_shape_get_rectangles(
		    xc, (xcb_window_t) cw->win, XCB_SHAPE_SK_BOUNDING);
		sr     = xcb_shape_get_rectangles_reply(xc, sck, NULL);
		rects  = sr ? xcb_shape_get_rectangles_rectangles(sr) : NULL;
		nrects = sr ? xcb_shape_get_rectangles_rectangles_length(sr) : 0;

		if (!rects || nrects == 0) {
			free(sr);
			xcb_xfixes_set_picture_clip_region(
			    xc, cw->picture, XCB_NONE, 0, 0);
			return;
		}

		{
			xcb_xfixes_region_t region = xcb_generate_id(xc);
			xcb_xfixes_create_region(xc, region, (uint32_t) nrects, rects);
			xcb_xfixes_set_picture_clip_region(xc, cw->picture, region, 0, 0);
			xcb_xfixes_destroy_region(xc, region);
		}
		free(sr);
	}
}

/* Read _XROOTPMAP_ID (or ESETROOT_PMAP_ID fallback) and rebuild wallpaper.
 * For the GL path we read the wallpaper pixmap into a GL texture.
 * For the XRender path we keep an XRender Picture. */
static void
comp_update_wallpaper(void)
{
	xcb_pixmap_t              pmap = 0;
	xcb_atom_t                atoms[2];
	int                       i;
	xcb_get_property_cookie_t ck;
	xcb_get_property_reply_t *r;

	/* Release previous wallpaper resources */
	if (comp.wallpaper_pict) {
		xcb_render_free_picture(xc, comp.wallpaper_pict);
		comp.wallpaper_pict   = None;
		comp.wallpaper_pixmap = None;
	}
	/* Release cached GL wallpaper resources if any */
	if (comp.use_gl) {
		if (comp.wallpaper_texture) {
			glDeleteTextures(1, &comp.wallpaper_texture);
			comp.wallpaper_texture = 0;
		}
		if (comp.wallpaper_egl_image != EGL_NO_IMAGE_KHR) {
			comp.egl_destroy_image(comp.egl_dpy, comp.wallpaper_egl_image);
			comp.wallpaper_egl_image = EGL_NO_IMAGE_KHR;
		}
	}

	atoms[0] = comp.atom_rootpmap;
	atoms[1] = comp.atom_esetroot;

	for (i = 0; i < 2 && pmap == None; i++) {
		ck = xcb_get_property(xc, 0, (xcb_window_t) root,
		    (xcb_atom_t) atoms[i], XCB_ATOM_PIXMAP, 0, 1);
		r  = xcb_get_property_reply(xc, ck, NULL);
		if (r &&
		    xcb_get_property_value_length(r) >= (int) sizeof(xcb_pixmap_t))
			pmap = (Pixmap) * (xcb_pixmap_t *) xcb_get_property_value(r);
		free(r);
	}

	if (pmap == None)
		return;

	/* Always build the XRender picture — used by the XRender fallback path
	 * and as a sentinel meaning "we have a wallpaper" in the GL path. */
	{
		const xcb_render_pictvisual_t *pv;
		xcb_render_pictformat_t        fmt;
		uint32_t                       pmask;
		uint32_t                       pval;

		pv = xcb_render_util_find_visual_format(
		    comp.render_formats, xcb_screen_root_visual(xc, screen));
		fmt   = pv ? pv->format : 0;
		pmask = XCB_RENDER_CP_REPEAT;
		pval  = XCB_RENDER_REPEAT_NORMAL;
		{
			xcb_void_cookie_t    ck;
			xcb_generic_error_t *err;

			comp.wallpaper_pict = xcb_generate_id(xc);
			ck = xcb_render_create_picture_checked(xc, comp.wallpaper_pict,
			    (xcb_drawable_t) pmap, fmt, pmask, &pval);
			xcb_flush(xc);
			err = xcb_request_check(xc, ck);
			free(err); /* error intentionally discarded */
		}

		if (comp.wallpaper_pict)
			comp.wallpaper_pixmap = pmap;
	}

	/* For the GL path, build an EGL image from the wallpaper pixmap.
	 * EGLImageKHR is a live mapping — the GPU always sees the current
	 * pixmap contents, so no per-frame rebind is needed. */
	if (comp.use_gl && comp.wallpaper_pixmap) {
		EGLint img_attr[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };

		comp.wallpaper_egl_image = comp.egl_create_image(comp.egl_dpy,
		    EGL_NO_CONTEXT, EGL_NATIVE_PIXMAP_KHR,
		    (EGLClientBuffer) (uintptr_t) comp.wallpaper_pixmap, img_attr);

		if (comp.wallpaper_egl_image != EGL_NO_IMAGE_KHR) {
			glGenTextures(1, &comp.wallpaper_texture);
			glBindTexture(GL_TEXTURE_2D, comp.wallpaper_texture);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(
			    GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(
			    GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			comp.egl_image_target_tex(
			    GL_TEXTURE_2D, (GLeglImageOES) comp.wallpaper_egl_image);
			glBindTexture(GL_TEXTURE_2D, 0);
		}
	}
}

/* Move cw to just above the window with XID `above_xid` in the stacking
 * order maintained in comp.windows (bottom-to-top linked list).
 * `above_xid == 0` means place cw at the bottom of the stack. */
static void
comp_restack_above(CompWin *cw, xcb_window_t above_xid)
{
	CompWin *prev = NULL, *cur;
	CompWin *above_cw;
	CompWin *ins_prev; /* node after which we insert cw */

	/* Remove cw from its current position */
	for (cur = comp.windows; cur; cur = cur->next) {
		if (cur == cw) {
			if (prev)
				prev->next = cw->next;
			else
				comp.windows = cw->next;
			cw->next = NULL;
			break;
		}
		prev = cur;
	}

	if (above_xid == None) {
		/* Place at the bottom (head of the list) */
		cw->next     = comp.windows;
		comp.windows = cw;
		return;
	}

	/* Find the node for above_xid; insert cw immediately after it */
	above_cw = comp_find_by_xid(above_xid);
	if (!above_cw) {
		/* Unknown sibling — place at top (append to tail) */
		ins_prev = NULL;
		for (cur = comp.windows; cur; cur = cur->next)
			ins_prev = cur;
		if (ins_prev) {
			cw->next       = ins_prev->next;
			ins_prev->next = cw;
		} else {
			cw->next     = comp.windows;
			comp.windows = cw;
		}
		return;
	}

	cw->next       = above_cw->next;
	above_cw->next = cw;
}

/* Add window by X ID */
static void
comp_add_by_xid(xcb_window_t w)
{
	CompWin *cw;

	if (comp_find_by_xid(w))
		return;

	if (w == comp.overlay)
		return;

	{
		xcb_get_window_attributes_cookie_t wac =
		    xcb_get_window_attributes(xc, (xcb_window_t) w);
		xcb_get_geometry_cookie_t gc =
		    xcb_get_geometry(xc, (xcb_drawable_t) w);
		xcb_get_window_attributes_reply_t *war =
		    xcb_get_window_attributes_reply(xc, wac, NULL);
		xcb_get_geometry_reply_t *gr = xcb_get_geometry_reply(xc, gc, NULL);

		if (!war || !gr) {
			free(war);
			free(gr);
			return;
		}
		if (war->_class == XCB_WINDOW_CLASS_INPUT_ONLY) {
			free(war);
			free(gr);
			return;
		}
		if (war->map_state != XCB_MAP_STATE_VIEWABLE) {
			free(war);
			free(gr);
			return;
		}

		cw = (CompWin *) calloc(1, sizeof(CompWin));
		if (!cw) {
			free(war);
			free(gr);
			return;
		}

		cw->win   = w;
		cw->x     = gr->x;
		cw->y     = gr->y;
		cw->w     = gr->width;
		cw->h     = gr->height;
		cw->bw    = gr->border_width;
		cw->depth = gr->depth;
		cw->argb  = (gr->depth == 32);
		free(war);
		free(gr);
	}
	cw->opacity    = 1.0;
	cw->redirected = 1;

	{
		Client  *c;
		Monitor *m;
		cw->client = NULL;
		for (m = mons; m; m = m->next)
			for (c = cl->clients; c; c = c->next)
				if (c->win == w) {
					cw->client = c;
					break;
				}
		if (cw->client)
			cw->opacity = cw->client->opacity;
	}

	comp_refresh_pixmap(cw);

	if (cw->pixmap) {
		xcb_void_cookie_t    ck;
		xcb_generic_error_t *err;

		cw->damage = xcb_generate_id(xc);
		ck = xcb_damage_create_checked(xc, cw->damage, (xcb_drawable_t) w,
		    XCB_DAMAGE_REPORT_LEVEL_NON_EMPTY);
		xcb_flush(xc);
		err = xcb_request_check(xc, ck);
		free(err); /* error intentionally discarded */
	}

	/* Subscribe to X Present CompleteNotify so DRI3/Present GPU frames
	 * (e.g. Chrome video) trigger repaints even without XDamageNotify. */
	comp_subscribe_present(cw);

	if (comp.has_xshape) {

		xcb_shape_select_input(xc, (xcb_window_t) w, 1);
	}

	/* Insert at the tail (topmost position in bottom-to-top ordering) */
	if (!comp.windows) {
		cw->next     = NULL;
		comp.windows = cw;
	} else {
		CompWin *tail = comp.windows;
		while (tail->next)
			tail = tail->next;
		tail->next = cw;
		cw->next   = NULL;
	}
}

/* -------------------------------------------------------------------------
 * Public API — called from WM core
 * ---------------------------------------------------------------------- */

void
compositor_add_window(Client *c)
{
	CompWin *cw;

	if (!comp.active || !c)
		return;

	cw = comp_find_by_xid(c->win);
	if (cw) {
		cw->client  = c;
		cw->opacity = c->opacity;
		return;
	}

	comp_add_by_xid(c->win);

	cw = comp_find_by_xid(c->win);
	if (cw) {
		cw->client  = c;
		cw->opacity = c->opacity;
	}

	schedule_repaint();
}

void
compositor_remove_window(Client *c)
{
	CompWin *cw, *prev;

	if (!comp.active || !c)
		return;

	prev = NULL;
	for (cw = comp.windows; cw; cw = cw->next) {
		if (cw->client == c || cw->win == c->win) {
			{
				xcb_rectangle_t     r;
				xcb_xfixes_region_t sr;
				r.x      = (short) cw->x;
				r.y      = (short) cw->y;
				r.width  = (uint16_t) (cw->w + 2 * cw->bw);
				r.height = (uint16_t) (cw->h + 2 * cw->bw);
				sr       = xcb_generate_id(xc);
				xcb_xfixes_create_region(xc, sr, 1, &r);
				xcb_xfixes_union_region(xc, comp.dirty, sr, comp.dirty);
				xcb_xfixes_destroy_region(xc, sr);
			}
			if (prev)
				prev->next = cw->next;
			else
				comp.windows = cw->next;
			if (comp.use_gl)
				comp_release_tfp(cw);
			comp_free_win(cw);
			free(cw);
			schedule_repaint();
			return;
		}
		prev = cw;
	}
}

void
compositor_configure_window(Client *c, int actual_bw)
{
	CompWin            *cw;
	int                 resized;
	xcb_rectangle_t     old_rect;
	xcb_xfixes_region_t old_r;

	if (!comp.active || !c)
		return;

	cw = comp_find_by_client(c);
	if (!cw)
		return;

	old_rect.x      = (short) cw->x;
	old_rect.y      = (short) cw->y;
	old_rect.width  = (uint16_t) (cw->w + 2 * cw->bw);
	old_rect.height = (uint16_t) (cw->h + 2 * cw->bw);
	old_r           = xcb_generate_id(xc);
	xcb_xfixes_create_region(xc, old_r, 1, &old_rect);
	xcb_xfixes_union_region(xc, comp.dirty, old_r, comp.dirty);
	xcb_xfixes_destroy_region(xc, old_r);

	resized = (c->w != cw->w || c->h != cw->h);

	cw->x  = c->x - actual_bw;
	cw->y  = c->y - actual_bw;
	cw->w  = c->w;
	cw->h  = c->h;
	cw->bw = actual_bw;

	{
		xcb_rectangle_t     new_rect;
		xcb_xfixes_region_t new_r;
		new_rect.x      = (short) cw->x;
		new_rect.y      = (short) cw->y;
		new_rect.width  = (uint16_t) (cw->w + 2 * cw->bw);
		new_rect.height = (uint16_t) (cw->h + 2 * cw->bw);
		new_r           = xcb_generate_id(xc);
		xcb_xfixes_create_region(xc, new_r, 1, &new_rect);
		xcb_xfixes_union_region(xc, comp.dirty, new_r, comp.dirty);
		xcb_xfixes_destroy_region(xc, new_r);
	}

	if (cw->redirected && resized)
		comp_refresh_pixmap(cw);

	schedule_repaint();
}

void
compositor_bypass_window(Client *c, int bypass)
{
	CompWin *cw;

	if (!comp.active || !c)
		return;

	cw = comp_find_by_client(c);
	if (!cw)
		return;

	if (bypass == !cw->redirected)
		return;

	if (bypass) {
		xcb_void_cookie_t    ck;
		xcb_generic_error_t *err;

		ck = xcb_composite_unredirect_window_checked(
		    xc, (xcb_window_t) c->win, XCB_COMPOSITE_REDIRECT_MANUAL);
		err = xcb_request_check(xc, ck);
		free(err); /* error intentionally discarded */
		cw->redirected = 0;
		if (comp.use_gl)
			comp_release_tfp(cw);
		comp_free_win(cw);
	} else {
		xcb_void_cookie_t    ck;
		xcb_generic_error_t *err;

		ck = xcb_composite_redirect_window_checked(
		    xc, (xcb_window_t) c->win, XCB_COMPOSITE_REDIRECT_MANUAL);
		err = xcb_request_check(xc, ck);
		free(err); /* error intentionally discarded */
		cw->redirected = 1;
		comp_refresh_pixmap(cw);
		if (cw->pixmap && !cw->damage) {
			cw->damage = xcb_generate_id(xc);
			xcb_damage_create(xc, cw->damage, (xcb_drawable_t) c->win,
			    XCB_DAMAGE_REPORT_LEVEL_NON_EMPTY);
		}
		comp_subscribe_present(cw);
	}

	schedule_repaint();
}

void
compositor_set_opacity(Client *c, unsigned long raw)
{
	CompWin *cw;

	if (!comp.active || !c)
		return;

	cw = comp_find_by_client(c);
	if (!cw)
		return;

	cw->opacity = (raw == 0) ? 0.0 : (double) raw / (double) 0xFFFFFFFFUL;
	c->opacity  = cw->opacity;
	schedule_repaint();
}

void
compositor_focus_window(Client *c)
{
	CompWin            *cw;
	xcb_rectangle_t     r;
	xcb_xfixes_region_t sr;

	if (!comp.active || !c)
		return;

	cw = comp_find_by_client(c);
	if (!cw || cw->bw <= 0)
		return;

	r.x      = (short) cw->x;
	r.y      = (short) cw->y;
	r.width  = (uint16_t) (cw->w + 2 * cw->bw);
	r.height = (uint16_t) (cw->h + 2 * cw->bw);
	sr       = xcb_generate_id(xc);
	xcb_xfixes_create_region(xc, sr, 1, &r);
	xcb_xfixes_union_region(xc, comp.dirty, sr, comp.dirty);
	xcb_xfixes_destroy_region(xc, sr);
	schedule_repaint();
}

void
compositor_set_hidden(Client *c, int hidden)
{
	CompWin *cw;

	if (!comp.active || !c)
		return;

	cw = comp_find_by_client(c);
	if (!cw)
		return;

	if (cw->hidden == hidden)
		return;

	cw->hidden = hidden;

	/* Dirty the window region so the vacated area gets repainted */
	{
		xcb_rectangle_t     r;
		xcb_xfixes_region_t sr;
		r.x      = (short) cw->x;
		r.y      = (short) cw->y;
		r.width  = (uint16_t) (cw->w + 2 * cw->bw);
		r.height = (uint16_t) (cw->h + 2 * cw->bw);
		sr       = xcb_generate_id(xc);
		xcb_xfixes_create_region(xc, sr, 1, &r);
		xcb_xfixes_union_region(xc, comp.dirty, sr, comp.dirty);
		xcb_xfixes_destroy_region(xc, sr);
	}
	schedule_repaint();
}

void
compositor_damage_all(void)
{
	xcb_rectangle_t full;

	if (!comp.active)
		return;

	full.x = full.y = 0;
	full.width      = (uint16_t) sw;
	full.height     = (uint16_t) sh;
	xcb_xfixes_set_region(xc, comp.dirty, 1, &full);
	schedule_repaint();
}

/*
 * Called from the root ConfigureNotify handler (events.c) after sw/sh have
 * been updated to reflect a screen resize (xrandr).  Updates the GL viewport,
 * resizes the XRender back-buffer, resets the damage ring (all old bboxes are
 * now stale), and forces a full repaint.
 */
void
compositor_notify_screen_resize(void)
{
	if (!comp.active)
		return;

	if (comp.use_gl) {
		glViewport(0, 0, sw, sh);
		/* Old damage ring entries are in the old coordinate space —
		 * invalidate them so the next frame does a full repaint. */
		memset(comp.damage_ring, 0, sizeof(comp.damage_ring));
		comp.ring_idx = 0;
	} else {
		/* XRender: rebuild back pixmap at new size */

		if (comp.back) {
			xcb_render_free_picture(xc, comp.back);
			comp.back = None;
		}
		if (comp.back_pixmap) {
			xcb_free_pixmap(xc, comp.back_pixmap);
			comp.back_pixmap = None;
		}
		comp.back_pixmap = xcb_generate_id(xc);
		xcb_create_pixmap(xc, xcb_screen_root_depth(xc, screen),
		    comp.back_pixmap, (xcb_drawable_t) root, (uint16_t) sw,
		    (uint16_t) sh);
		if (comp.back_pixmap) {
			const xcb_render_pictvisual_t *pv =
			    xcb_render_util_find_visual_format(
			        comp.render_formats, xcb_screen_root_visual(xc, screen));
			xcb_render_pictformat_t fmt   = pv ? pv->format : 0;
			uint32_t                pmask = XCB_RENDER_CP_SUBWINDOW_MODE;
			uint32_t pval = XCB_SUBWINDOW_MODE_INCLUDE_INFERIORS;
			comp.back     = xcb_generate_id(xc);
			xcb_render_create_picture(xc, comp.back,
			    (xcb_drawable_t) comp.back_pixmap, fmt, pmask, &pval);
		}
	}

	compositor_damage_all();
}

void
compositor_raise_overlay(void)
{
	if (!comp.active)
		return;
	{
		uint32_t stack = XCB_STACK_MODE_ABOVE;
		xcb_configure_window(
		    xc, comp.overlay, XCB_CONFIG_WINDOW_STACK_MODE, &stack);
	}
}

/*
 * Evaluate whether the topmost visible window warrants suspending all
 * compositing.  Called from focus() and setfullscreen() whenever the
 * window stack or fullscreen state changes.
 *
 * Suspend criteria (all must hold):
 *   - compositor is active and using the GL path
 *   - the focused window is fullscreen
 *   - it covers the entire monitor geometry
 *   - it is opaque (opacity == 1.0)
 *
 * On suspend  : lower the overlay below all windows and cancel repaints.
 * On resume   : raise the overlay, force a full dirty, schedule repaint.
 */
void
compositor_check_unredirect(void)
{
	Client *sel;
	int     should_pause;

	if (!comp.active || !comp.use_gl)
		return;

	sel          = selmon ? selmon->sel : NULL;
	should_pause = 0;

	if (sel && sel->isfullscreen && sel->opacity >= 1.0 &&
	    sel->x == sel->mon->mx && sel->y == sel->mon->my &&
	    sel->w == sel->mon->mw && sel->h == sel->mon->mh) {
		should_pause = 1;
	}

	if (should_pause == comp.paused)
		return; /* no change */

	comp.paused = should_pause;

	if (comp.paused) {
		/* Unredirect the fullscreen window and hide the overlay so that
		 * the window can do DRI3/Present page-flips directly to the
		 * display without the X server downgrading them to copies.
		 * Without XCompositeUnredirectWindow the window is still
		 * redirected into a backing pixmap, DRI3 flips stall, and
		 * GPU-rendered video (Chrome, mpv) freezes. */
		if (comp.repaint_id) {
			g_source_remove(comp.repaint_id);
			comp.repaint_id = 0;
		}
		{
			CompWin *cw;
			for (cw = comp.windows; cw; cw = cw->next) {
				if (cw->client && cw->client->isfullscreen && cw->redirected) {
					xcb_void_cookie_t    ck;
					xcb_generic_error_t *err;

					ck  = xcb_composite_unredirect_window_checked(xc,
					     (xcb_window_t) cw->win, XCB_COMPOSITE_REDIRECT_MANUAL);
					err = xcb_request_check(xc, ck);
					free(err); /* error intentionally discarded */
					cw->redirected = 0;
					/* Release TFP/pixmap — they'll be rebuilt on resume */
					if (comp.use_gl)
						comp_release_tfp(cw);
					comp_free_win(cw); /* also unsubscribes Present */
				}
			}
		}
		{
			uint32_t stack = XCB_STACK_MODE_BELOW;
			xcb_configure_window(
			    xc, comp.overlay, XCB_CONFIG_WINDOW_STACK_MODE, &stack);
		}
		awm_debug("compositor: suspended (fullscreen unredirect)");
	} else {
		/* Resume: re-redirect any fullscreen windows that were bypassed
		 * while the compositor was paused.  Without this, focusing away
		 * from a fullscreen window raises the overlay but leaves the
		 * fullscreen window unredirected (cw->redirected==0), so the
		 * compositor skips it and paints the wallpaper over it instead. */
		CompWin *cw;
		for (cw = comp.windows; cw; cw = cw->next) {
			if (cw->client && cw->client->isfullscreen && !cw->redirected) {
				xcb_void_cookie_t    ck;
				xcb_generic_error_t *err;

				ck = xcb_composite_redirect_window_checked(
				    xc, (xcb_window_t) cw->win, XCB_COMPOSITE_REDIRECT_MANUAL);
				err = xcb_request_check(xc, ck);
				free(err); /* error intentionally discarded */
				cw->redirected = 1;
				comp_refresh_pixmap(cw);
				if (cw->pixmap && !cw->damage) {
					cw->damage = xcb_generate_id(xc);
					xcb_damage_create(xc, cw->damage, (xcb_drawable_t) cw->win,
					    XCB_DAMAGE_REPORT_LEVEL_NON_EMPTY);
				}
				/* Re-subscribe Present events — the eid was cleared by
				 * comp_unsubscribe_present() when we unredirected earlier. */
				comp_subscribe_present(cw);
				awm_debug("compositor: re-redirected fullscreen "
				          "window 0x%lx on resume",
				    cw->win);
			}
		}
		/* Raise overlay and repaint everything. */
		{
			uint32_t stack = XCB_STACK_MODE_ABOVE;
			xcb_configure_window(
			    xc, comp.overlay, XCB_CONFIG_WINDOW_STACK_MODE, &stack);
		}
		compositor_damage_all();
		awm_debug("compositor: resumed");
	}
}

void
compositor_xrender_errors(int *req_base, int *err_base)
{
	if (!comp.active) {
		*req_base = -1;
		*err_base = -1;
		return;
	}
	*req_base = comp.render_request_base;
	*err_base = comp.render_err_base;
}

void
compositor_damage_errors(int *err_base)
{
	if (!comp.active) {
		*err_base = -1;
		return;
	}
	*err_base = comp.damage_err_base;
}

void
compositor_glx_errors(int *req_base, int *err_base)
{
	/* EGL has no X request codes — always return sentinel values so
	 * the error-whitelisting logic in events.c is always a no-op. */
	*req_base = -1;
	*err_base = -1;
}

void
compositor_repaint_now(void)
{
	if (!comp.active)
		return;
	if (comp.repaint_id) {
		g_source_remove(comp.repaint_id);
		comp.repaint_id = 0;
	}
	comp_do_repaint();
}

/* -------------------------------------------------------------------------
 * Event handler
 * ---------------------------------------------------------------------- */

void
compositor_handle_event(xcb_generic_event_t *ev)
{
	if (!comp.active)
		return;

	if ((ev->response_type & ~0x80) ==
	    (uint8_t) (comp.damage_ev_base + XCB_DAMAGE_NOTIFY)) {
		xcb_damage_notify_event_t *dev =
		    (xcb_damage_notify_event_t *) (void *) ev;
		CompWin *dcw = comp_find_by_xid(dev->drawable);

		if (!dcw) {
			/* Unknown window — just ack the damage and ignore. */
			xcb_void_cookie_t    ck;
			xcb_generic_error_t *err;

			ck = xcb_damage_subtract_checked(
			    xc, dev->damage, XCB_NONE, XCB_NONE);
			err = xcb_request_check(xc, ck);
			free(err); /* error intentionally discarded */
			schedule_repaint();
			return;
		}

		if (!dcw->ever_damaged) {
			/* First damage since (re)map or pixmap refresh: dirty the
			 * entire window rect rather than the (possibly partial) area
			 * reported in the notify.  Ack by discarding the region. */
			xcb_void_cookie_t    ck;
			xcb_generic_error_t *err;

			dcw->ever_damaged = 1;
			ck                = xcb_damage_subtract_checked(
                xc, dev->damage, XCB_NONE, XCB_NONE);
			err = xcb_request_check(xc, ck);
			free(err); /* error intentionally discarded */
			{
				xcb_rectangle_t     r;
				xcb_xfixes_region_t sr;
				r.x      = (short) dcw->x;
				r.y      = (short) dcw->y;
				r.width  = (uint16_t) (dcw->w + 2 * dcw->bw);
				r.height = (uint16_t) (dcw->h + 2 * dcw->bw);
				sr       = xcb_generate_id(xc);
				xcb_xfixes_create_region(xc, sr, 1, &r);
				xcb_xfixes_union_region(xc, comp.dirty, sr, comp.dirty);
				xcb_xfixes_destroy_region(xc, sr);
			}
		} else {
			/* Subsequent damage: fetch the exact damage region from the
			 * server via xcb_damage_subtract so we can dirty only what
			 * changed. dev->area is only the bounding box of the event's
			 * inline area, not the full accumulated server-side damage
			 * region. */
			xcb_xfixes_region_t  dmg_region = xcb_generate_id(xc);
			xcb_void_cookie_t    ck;
			xcb_generic_error_t *err;

			xcb_xfixes_create_region(xc, dmg_region, 0, NULL);
			ck = xcb_damage_subtract_checked(
			    xc, dev->damage, XCB_NONE, dmg_region);
			err = xcb_request_check(xc, ck);
			free(err); /* error intentionally discarded */
			/* Translate from window-local to screen coordinates */
			xcb_xfixes_translate_region(
			    xc, dmg_region, (int16_t) dcw->x, (int16_t) dcw->y);
			xcb_xfixes_union_region(xc, comp.dirty, dmg_region, comp.dirty);
			xcb_xfixes_destroy_region(xc, dmg_region);
		}

		schedule_repaint();
		return;
	}

	{
		uint8_t type = ev->response_type & ~0x80;

		if (type == MapNotify) {
			xcb_map_notify_event_t *mev = (xcb_map_notify_event_t *) ev;
			if (mev->event == root)
				comp_add_by_xid(mev->window);
			schedule_repaint();
			return;
		}

		if (type == UnmapNotify) {
			xcb_unmap_notify_event_t *uev = (xcb_unmap_notify_event_t *) ev;
			CompWin                  *cw  = comp_find_by_xid(uev->window);
			if (cw && !cw->client) {
				CompWin *prev = NULL, *cur;
				{
					xcb_rectangle_t     r;
					xcb_xfixes_region_t sr;
					r.x      = (short) cw->x;
					r.y      = (short) cw->y;
					r.width  = (uint16_t) (cw->w + 2 * cw->bw);
					r.height = (uint16_t) (cw->h + 2 * cw->bw);
					sr       = xcb_generate_id(xc);
					xcb_xfixes_create_region(xc, sr, 1, &r);
					xcb_xfixes_union_region(xc, comp.dirty, sr, comp.dirty);
					xcb_xfixes_destroy_region(xc, sr);
				}
				for (cur = comp.windows; cur; cur = cur->next) {
					if (cur == cw) {
						if (prev)
							prev->next = cw->next;
						else
							comp.windows = cw->next;
						if (comp.use_gl)
							comp_release_tfp(cw);
						comp_free_win(cw);
						free(cw);
						break;
					}
					prev = cur;
				}
			}
			schedule_repaint();
			return;
		}

		if (type == ConfigureNotify) {
			xcb_configure_notify_event_t *cev =
			    (xcb_configure_notify_event_t *) ev;
			CompWin *cw = comp_find_by_xid(cev->window);
			if (cw) {
				if (cw->client) {
					/* Geometry for managed clients is tracked by
					 * compositor_configure_window() (called from
					 * resizeclient()). We only need to update the Z-order in
					 * our internal list so the painter's algorithm draws them
					 * correctly. Without this, comp.windows ordering is frozen
					 * at map time and monocle / floating windows appear in the
					 * wrong layer. */
					comp_restack_above(cw, cev->above_sibling);
					schedule_repaint();
					return;
				}

				int resized = (cev->width != cw->w || cev->height != cw->h);

				{
					xcb_rectangle_t     old_rect;
					xcb_xfixes_region_t old_r;
					old_rect.x      = (short) cw->x;
					old_rect.y      = (short) cw->y;
					old_rect.width  = (uint16_t) (cw->w + 2 * cw->bw);
					old_rect.height = (uint16_t) (cw->h + 2 * cw->bw);
					old_r           = xcb_generate_id(xc);
					xcb_xfixes_create_region(xc, old_r, 1, &old_rect);
					xcb_xfixes_union_region(xc, comp.dirty, old_r, comp.dirty);
					xcb_xfixes_destroy_region(xc, old_r);
				}

				cw->x  = cev->x;
				cw->y  = cev->y;
				cw->w  = cev->width;
				cw->h  = cev->height;
				cw->bw = cev->border_width;

				/* Restack in our internal list to match the X stacking order.
				 * cev->above_sibling is the sibling directly below this
				 * window. */
				comp_restack_above(cw, cev->above_sibling);

				if (cw->redirected && resized)
					comp_refresh_pixmap(cw);

				schedule_repaint();
			}
			return;
		}

		if (type == DestroyNotify) {
			xcb_destroy_notify_event_t *dev =
			    (xcb_destroy_notify_event_t *) ev;
			CompWin *prev = NULL, *cw;
			for (cw = comp.windows; cw; cw = cw->next) {
				if (cw->win == dev->window) {
					{
						xcb_rectangle_t     r;
						xcb_xfixes_region_t sr;
						r.x      = (short) cw->x;
						r.y      = (short) cw->y;
						r.width  = (uint16_t) (cw->w + 2 * cw->bw);
						r.height = (uint16_t) (cw->h + 2 * cw->bw);
						sr       = xcb_generate_id(xc);
						xcb_xfixes_create_region(xc, sr, 1, &r);
						xcb_xfixes_union_region(
						    xc, comp.dirty, sr, comp.dirty);
						xcb_xfixes_destroy_region(xc, sr);
					}
					if (prev)
						prev->next = cw->next;
					else
						comp.windows = cw->next;
					if (comp.use_gl)
						comp_release_tfp(cw);
					comp_free_win(cw);
					free(cw);
					schedule_repaint();
					return;
				}
				prev = cw;
			}
			return;
		}

		if (type == PropertyNotify) {
			xcb_property_notify_event_t *pev =
			    (xcb_property_notify_event_t *) ev;
			if (pev->window == root &&
			    (pev->atom == comp.atom_rootpmap ||
			        pev->atom == comp.atom_esetroot)) {
				comp_update_wallpaper();
				compositor_damage_all();
			} else if (pev->atom == comp.atom_net_wm_opacity &&
			    pev->window != root) {
				/* _NET_WM_WINDOW_OPACITY changed on a client window.
				 * Read the new value and propagate it to the CompWin. */
				CompWin *cw = comp_find_by_xid(pev->window);
				if (cw && cw->client) {
					xcb_get_property_cookie_t ck2;
					xcb_get_property_reply_t *r2;

					ck2 = xcb_get_property(xc, 0, (xcb_window_t) pev->window,
					    (xcb_atom_t) comp.atom_net_wm_opacity,
					    XCB_ATOM_CARDINAL, 0, 1);
					r2  = xcb_get_property_reply(xc, ck2, NULL);
					if (r2 &&
					    xcb_get_property_value_length(r2) >=
					        (int) sizeof(uint32_t)) {
						unsigned long raw =
						    *(uint32_t *) xcb_get_property_value(r2);
						compositor_set_opacity(cw->client, raw);
					} else {
						/* Property deleted — restore to fully opaque */
						compositor_set_opacity(cw->client, 0xFFFFFFFFUL);
					}
					free(r2);
				}
			}
			return;
		}

		if (comp.has_xshape &&
		    type == (uint8_t) (comp.shape_ev_base + XCB_SHAPE_NOTIFY)) {
			xcb_shape_notify_event_t *sev =
			    (xcb_shape_notify_event_t *) (void *) ev;
			if (sev->shape_kind == XCB_SHAPE_SK_BOUNDING) {
				CompWin *cw = comp_find_by_xid(sev->affected_window);
				if (cw) {
					if (comp.use_gl) {
						/* In the GL path there's no per-picture clip region;
						 * the shape is handled by re-acquiring the pixmap so
						 * TFP naturally masks via the window's shape. */
						if (cw->redirected)
							comp_refresh_pixmap(cw);
					} else if (cw->picture) {
						comp_apply_shape(cw);
					}
					schedule_repaint();
				}
			}
			return;
		}

		if (type == SelectionClear) {
			xcb_selection_clear_event_t *sce =
			    (xcb_selection_clear_event_t *) ev;
			if (sce->selection == comp.atom_cm_sn) {
				/* Another compositor has claimed our selection — shut down. */
				awm_warn(
				    "compositor: lost _NET_WM_CM_S%d selection to another "
				    "compositor; disabling compositing",
				    screen);
				compositor_cleanup();
			}
			return;
		}

		/* ---- X Present CompleteNotify
		 * ----------------------------------------- Chrome/Chromium and other
		 * DRI3/Present clients submit GPU video frames via xcb_present_pixmap
		 * rather than triggering XDamageNotify.  Without this handler the
		 * compositor would paint one static frame on resume from fullscreen
		 * bypass and then freeze.
		 *
		 * Present events arrive as XCB GenericEvent (response_type == 35).
		 * We check that the extension field matches the Present major opcode,
		 * and for CompleteNotify we force a pixmap refresh + full damage.
		 */
		if (comp.has_present && type == XCB_GE_GENERIC) {
			xcb_ge_generic_event_t *ge = (xcb_ge_generic_event_t *) ev;
			if (ge->extension == (uint8_t) comp.present_opcode &&
			    ge->event_type == XCB_PRESENT_COMPLETE_NOTIFY) {
				xcb_present_complete_notify_event_t *pev =
				    (xcb_present_complete_notify_event_t *) ev;
				/* Only react to pixmap presents (kind==0), not MSC queries */
				if (pev->kind == XCB_PRESENT_COMPLETE_KIND_PIXMAP) {
					CompWin *cw = comp_find_by_xid(pev->window);
					/* Skip while paused (fullscreen bypass): the window
					 * draws directly to the display and the compositor is
					 * not painting.  Calling comp_refresh_pixmap here at
					 * video frame rate would issue XSync on every frame,
					 * stalling Chrome's rendering pipeline and causing the
					 * "freezes when focused" symptom. */
					if (cw && cw->redirected && !comp.paused) {
						/* Grab a fresh pixmap snapshot — the DRI3/Present
						 * buffer Chromium rendered into may not be the same
						 * pixmap we already have a TFP binding for.  A new
						 * XCompositeNameWindowPixmap call retrieves the
						 * current backing store. */
						comp_refresh_pixmap(cw);
						compositor_damage_all();
						schedule_repaint();
						awm_debug("compositor: Present CompleteNotify on "
						          "window 0x%lx — refreshed pixmap",
						    cw->win);
					}
				}
			}
			return;
		}

	} /* end type switch block */
}

/* -------------------------------------------------------------------------
 * Repaint scheduler
 * ---------------------------------------------------------------------- */

static void
schedule_repaint(void)
{
	if (!comp.active || comp.paused || comp.repaint_id)
		return;

	comp.repaint_id =
	    g_idle_add_full(G_PRIORITY_HIGH_IDLE, comp_repaint_idle, NULL, NULL);
}

/* -------------------------------------------------------------------------
 * Repaint — the core paint loop
 * ---------------------------------------------------------------------- */

static gboolean
comp_repaint_idle(gpointer data)
{
	(void) data;
	comp.repaint_id = 0;

	/* Guard: compositor may have been paused (fullscreen bypass) between
	 * the time this idle was queued and now.  Bail out immediately rather
	 * than touching GL with a lowered/hidden overlay window. */
	if (!comp.active || comp.paused)
		return G_SOURCE_REMOVE;

	/* Drain any XDamageNotify events still queued in the X connection
	 * before painting so we paint one complete frame covering all
	 * accumulated damage instead of a series of partial frames. */
	{

		uint8_t dmgt = (uint8_t) (comp.damage_ev_base + XCB_DAMAGE_NOTIFY);
		xcb_generic_event_t *xe;
		xcb_flush(xc);
		while ((xe = xcb_poll_for_event(xc))) {
			if ((xe->response_type & ~0x80) == dmgt) {
				compositor_handle_event(xe);
			}
			free(xe);
		}
	}

	comp_do_repaint();
	return G_SOURCE_REMOVE;
}

/* Dispatch to GL or XRender repaint */
static void
comp_do_repaint(void)
{
	if (!comp.active || comp.paused)
		return;

	if (comp.use_gl)
		comp_do_repaint_gl();
	else
		comp_do_repaint_xrender();
}

/* -------------------------------------------------------------------------
 * GL repaint path
 * ---------------------------------------------------------------------- */

/*
 * Fetch the bounding box of comp.dirty as a single xcb_rectangle_t.
 * Returns 1 on success, 0 if the region is empty or the fetch fails.
 * On failure *out is set to the full screen rect.
 */
static int
dirty_get_bbox(xcb_rectangle_t *out)
{
	xcb_xfixes_fetch_region_cookie_t fck;
	xcb_xfixes_fetch_region_reply_t *fr;
	xcb_rectangle_t                 *rects;
	int                              nrects;

	fck    = xcb_xfixes_fetch_region(xc, comp.dirty);
	fr     = xcb_xfixes_fetch_region_reply(xc, fck, NULL);
	rects  = fr ? xcb_xfixes_fetch_region_rectangles(fr) : NULL;
	nrects = fr ? xcb_xfixes_fetch_region_rectangles_length(fr) : 0;

	if (!rects || nrects == 0) {
		free(fr);
		out->x = out->y = 0;
		out->width      = (unsigned short) sw;
		out->height     = (unsigned short) sh;
		return 0;
	}

	/* Union all returned rects into a single bounding box */
	int x1 = rects[0].x, y1 = rects[0].y;
	int x2 = x1 + rects[0].width, y2 = y1 + rects[0].height;
	for (int r = 1; r < nrects; r++) {
		if (rects[r].x < x1)
			x1 = rects[r].x;
		if (rects[r].y < y1)
			y1 = rects[r].y;
		int ex = rects[r].x + rects[r].width;
		int ey = rects[r].y + rects[r].height;
		if (ex > x2)
			x2 = ex;
		if (ey > y2)
			y2 = ey;
	}
	free(fr); /* rects is interior to fr — do NOT free separately */

	/* Clamp to screen */
	if (x1 < 0)
		x1 = 0;
	if (y1 < 0)
		y1 = 0;
	if (x2 > sw)
		x2 = sw;
	if (y2 > sh)
		y2 = sh;

	out->x      = (short) x1;
	out->y      = (short) y1;
	out->width  = (unsigned short) (x2 - x1);
	out->height = (unsigned short) (y2 - y1);
	return (out->width > 0 && out->height > 0);
}

static void
comp_do_repaint_gl(void)
{
	CompWin        *cw;
	xcb_rectangle_t scissor;
	int             use_scissor = 0;

	/* --- Partial repaint via EGL_EXT_buffer_age + glScissor ------------- */
	if (comp.has_buffer_age) {
		unsigned int age = 0;
		eglQuerySurface(
		    comp.egl_dpy, comp.egl_win, EGL_BUFFER_AGE_EXT, (EGLint *) &age);

		/* age==0 means undefined (e.g. first frame or after resize);
		 * fall back to full repaint.  age==1 means back buffer is one
		 * frame old — only this frame's dirty rect needs repainting. */
		if (age > 0 && age <= (unsigned int) DAMAGE_RING_SIZE) {
			/* Collect current dirty bbox */
			xcb_rectangle_t cur;
			dirty_get_bbox(&cur);

			/* Union current dirty with the past (age-1) frames */
			int x1 = cur.x, y1 = cur.y;
			int x2 = x1 + cur.width, y2 = y1 + cur.height;
			for (unsigned int a = 1; a < age; a++) {
				int slot = ((comp.ring_idx - (int) a) + DAMAGE_RING_SIZE * 2) %
				    DAMAGE_RING_SIZE;
				xcb_rectangle_t *r = &comp.damage_ring[slot];
				if (r->width == 0 || r->height == 0)
					continue;
				if (r->x < x1)
					x1 = r->x;
				if (r->y < y1)
					y1 = r->y;
				int ex = r->x + r->width;
				int ey = r->y + r->height;
				if (ex > x2)
					x2 = ex;
				if (ey > y2)
					y2 = ey;
			}

			/* Store this frame's bbox into ring before swap */
			comp.damage_ring[comp.ring_idx] = cur;
			comp.ring_idx = (comp.ring_idx + 1) % DAMAGE_RING_SIZE;

			/* Clamp to screen */
			if (x1 < 0)
				x1 = 0;
			if (y1 < 0)
				y1 = 0;
			if (x2 > sw)
				x2 = sw;
			if (y2 > sh)
				y2 = sh;

			scissor.x      = (short) x1;
			scissor.y      = (short) y1;
			scissor.width  = (unsigned short) (x2 - x1);
			scissor.height = (unsigned short) (y2 - y1);

			if (scissor.width > 0 && scissor.height > 0)
				use_scissor = 1;
		} else {
			/* Full repaint — record full screen in ring */
			comp.damage_ring[comp.ring_idx].x      = 0;
			comp.damage_ring[comp.ring_idx].y      = 0;
			comp.damage_ring[comp.ring_idx].width  = (unsigned short) sw;
			comp.damage_ring[comp.ring_idx].height = (unsigned short) sh;
			comp.ring_idx = (comp.ring_idx + 1) % DAMAGE_RING_SIZE;
		}
	}

	if (use_scissor) {
		/* GL scissor is in bottom-left origin; flip Y */
		glEnable(GL_SCISSOR_TEST);
		glScissor(scissor.x, sh - scissor.y - scissor.height, scissor.width,
		    scissor.height);
	}

	glUseProgram(comp.prog);
	glUniform2f(comp.u_screen, (float) sw, (float) sh);
	glUniform1i(comp.u_tex, 0);

	/* Clear to black (or paint wallpaper) */
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	/* Paint wallpaper via cached TFP texture if available */
	if (comp.wallpaper_texture) {
		glBindTexture(GL_TEXTURE_2D, comp.wallpaper_texture);
		/* EGLImageKHR is a live mapping — no rebind needed */
		glUniform4f(comp.u_rect, 0.0f, 0.0f, (float) sw, (float) sh);
		glUniform1f(comp.u_opacity, 1.0f);
		glUniform1i(comp.u_flip_y, 0);
		glUniform1i(comp.u_solid, 0);
		glBindVertexArray(comp.vao);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		glBindVertexArray(0);
		glBindTexture(GL_TEXTURE_2D, 0);
	}

	/* Walk windows bottom-to-top using our internal list.
	 * comp.windows is maintained in bottom-to-top order by comp_restack_above
	 * and comp_add_by_xid, eliminating the per-frame XQueryTree round-trip. */
	glBindVertexArray(comp.vao);
	glActiveTexture(GL_TEXTURE0);

	for (cw = comp.windows; cw; cw = cw->next) {
		if (!cw->redirected || !cw->texture || cw->hidden)
			continue;

		/* EGLImageKHR is a live mapping — GPU always sees current pixmap
		 * contents, no per-frame rebind needed. */
		glBindTexture(GL_TEXTURE_2D, cw->texture);

		/* Draw the full window pixmap (XCompositeNameWindowPixmap includes
		 * borders), positioned at cw->x,cw->y with full outer size.
		 * Using the inner w/h caused the bottom bw rows to be clipped. */
		glUniform4f(comp.u_rect, (float) cw->x, (float) cw->y,
		    (float) (cw->w + 2 * cw->bw), (float) (cw->h + 2 * cw->bw));
		glUniform1f(comp.u_opacity, (float) cw->opacity);
		glUniform1i(comp.u_flip_y, 0); /* NDC Y-flip in vert shader suffices */
		glUniform1i(comp.u_solid, 0);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

		/* Draw borders for managed clients */
		if (cw->client && cw->bw > 0) {
			int          sel = (selmon && cw->client == selmon->sel);
			Clr         *bc = &scheme[sel ? SchemeSel : SchemeNorm][ColBorder];
			float        r  = (float) bc->r / 65535.0f;
			float        g  = (float) bc->g / 65535.0f;
			float        b  = (float) bc->b / 65535.0f;
			float        a  = (float) bc->a / 65535.0f;
			unsigned int bw = (unsigned int) cw->bw;
			unsigned int ow = (unsigned int) cw->w + 2 * bw;
			unsigned int oh = (unsigned int) cw->h + 2 * bw;

			glBindTexture(GL_TEXTURE_2D, 0);
			glUniform1i(comp.u_solid, 1);
			/* Straight-alpha: pass r,g,b,a directly */
			glUniform4f(comp.u_color, r, g, b, a);

			/* top */
			glUniform4f(comp.u_rect, (float) cw->x, (float) cw->y, (float) ow,
			    (float) bw);
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
			/* bottom */
			glUniform4f(comp.u_rect, (float) cw->x,
			    (float) (cw->y + (int) (oh - bw)), (float) ow, (float) bw);
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
			/* left */
			glUniform4f(comp.u_rect, (float) cw->x, (float) (cw->y + (int) bw),
			    (float) bw, (float) cw->h);
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
			/* right */
			glUniform4f(comp.u_rect, (float) (cw->x + (int) (ow - bw)),
			    (float) (cw->y + (int) bw), (float) bw, (float) cw->h);
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

			glUniform1i(comp.u_solid, 0);
			/* Restore texture binding for next window */
			glBindTexture(GL_TEXTURE_2D, 0);
		}
	}

	glBindVertexArray(0);

	glUseProgram(0);

	if (use_scissor)
		glDisable(GL_SCISSOR_TEST);

	/* Reset dirty region */
	xcb_xfixes_set_region(xc, comp.dirty, 0, NULL);

	/* Present — eglSwapBuffers is vsync-aware (swap interval = 1).
	 * Re-check paused immediately before the swap: if a fullscreen bypass
	 * raced in between the repaint start and here, the overlay window may
	 * already be lowered and the GL context in an inconsistent state.
	 * Skipping the swap is safe — the dirty region is already cleared. */

	if (!comp.paused)
		eglSwapBuffers(comp.egl_dpy, comp.egl_win);
}

/* -------------------------------------------------------------------------
 * XRender repaint path (fallback for software-only X servers)
 * ---------------------------------------------------------------------- */

static void
comp_do_repaint_xrender(void)
{
	CompWin           *cw;
	xcb_render_color_t bg_color = { 0, 0, 0, 0xffff };

	/* Clip back-buffer to dirty region */
	xcb_xfixes_set_picture_clip_region(xc, comp.back, comp.dirty, 0, 0);

	/* Paint background */
	if (comp.wallpaper_pict) {
		xcb_render_composite(xc, XCB_RENDER_PICT_OP_SRC, comp.wallpaper_pict,
		    XCB_NONE, comp.back, 0, 0, 0, 0, 0, 0, (uint16_t) sw,
		    (uint16_t) sh);
	} else {
		xcb_rectangle_t bg_rect = { 0, 0, (uint16_t) sw, (uint16_t) sh };
		xcb_render_fill_rectangles(
		    xc, XCB_RENDER_PICT_OP_SRC, comp.back, bg_color, 1, &bg_rect);
	}

	/* Walk windows bottom-to-top using our internal list.
	 * comp.windows is maintained in bottom-to-top order by comp_restack_above
	 * and comp_add_by_xid, eliminating the per-frame XQueryTree round-trip. */
	for (cw = comp.windows; cw; cw = cw->next) {
		int                  alpha_idx;
		xcb_render_picture_t mask;

		if (!cw->redirected || cw->picture == None || cw->hidden)
			continue;

		alpha_idx = (int) (cw->opacity * 255.0 + 0.5);
		if (alpha_idx < 0)
			alpha_idx = 0;
		if (alpha_idx > 255)
			alpha_idx = 255;

		if (cw->argb || alpha_idx < 255) {
			mask = comp.alpha_pict[alpha_idx];
			xcb_render_composite(xc, XCB_RENDER_PICT_OP_OVER, cw->picture,
			    mask, comp.back, 0, 0, 0, 0, (int16_t) (cw->x + cw->bw),
			    (int16_t) (cw->y + cw->bw), (uint16_t) cw->w,
			    (uint16_t) cw->h);
		} else {
			xcb_render_composite(xc, XCB_RENDER_PICT_OP_SRC, cw->picture,
			    XCB_NONE, comp.back, 0, 0, 0, 0, (int16_t) (cw->x + cw->bw),
			    (int16_t) (cw->y + cw->bw), (uint16_t) cw->w,
			    (uint16_t) cw->h);
		}

		if (cw->client && cw->bw > 0) {
			int  sel = (selmon && cw->client == selmon->sel);
			Clr *clr = &scheme[sel ? SchemeSel : SchemeNorm][ColBorder];
			xcb_render_color_t bc         = { clr->r, clr->g, clr->b, clr->a };
			uint16_t           bw         = (uint16_t) cw->bw;
			uint16_t           ow         = (uint16_t) (cw->w + 2 * cw->bw);
			uint16_t           oh         = (uint16_t) (cw->h + 2 * cw->bw);
			xcb_rectangle_t    borders[4] = {
                { (int16_t) cw->x, (int16_t) cw->y, ow, bw },
                { (int16_t) cw->x, (int16_t) (cw->y + (int) (oh - bw)), ow,
				       bw },
                { (int16_t) cw->x, (int16_t) (cw->y + (int) bw), bw,
				       (uint16_t) cw->h },
                { (int16_t) (cw->x + (int) (ow - bw)),
				       (int16_t) (cw->y + (int) bw), bw, (uint16_t) cw->h },
			};
			xcb_render_fill_rectangles(
			    xc, XCB_RENDER_PICT_OP_SRC, comp.back, bc, 4, borders);
		}
	}

	/* Blit full back-buffer to overlay — unconditional, no clip */
	xcb_xfixes_set_picture_clip_region(xc, comp.target, XCB_NONE, 0, 0);
	xcb_render_composite(xc, XCB_RENDER_PICT_OP_SRC, comp.back, XCB_NONE,
	    comp.target, 0, 0, 0, 0, 0, 0, (uint16_t) sw, (uint16_t) sh);

	/* Reset dirty region */
	xcb_xfixes_set_region(xc, comp.dirty, 0, NULL);

	xcb_xfixes_set_picture_clip_region(xc, comp.back, XCB_NONE, 0, 0);
	xflush();
}

#endif /* COMPOSITOR */

/* Satisfy ISO C99: a translation unit must contain at least one declaration.
 */
typedef int compositor_translation_unit_nonempty;
