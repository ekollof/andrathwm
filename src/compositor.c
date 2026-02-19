/* compositor.c — GLX/TFP accelerated compositor for awm
 *
 * Architecture:
 *   - XCompositeRedirectSubwindows(root, CompositeRedirectManual) captures
 *     all root children into server-side pixmaps.
 *   - An overlay window (XCompositeGetOverlayWindow) is used as the GLX
 *     drawable; a GL context is created on it and windows are rendered
 *     directly to it via textured quads.
 *   - Each window's XCompositeNameWindowPixmap is bound as a GL texture
 *     via GLX_EXT_texture_from_pixmap (zero CPU copy, GPU compositing).
 *   - XDamage tracks which windows have changed since the last repaint.
 *   - glXSwapIntervalMESA(1) enables vsync so frames are presented at
 *     display rate with no tearing.
 *   - Border rectangles for managed clients are drawn as GL quads in the
 *     same pass.
 *   - XRender is retained only for building the alpha-picture cache that
 *     was used for opacity; opacity is now handled by the GL blend equation
 *     directly (no XRender needed at runtime).
 *
 * Fallback:
 *   - If GLX_EXT_texture_from_pixmap is unavailable the compositor falls
 *     back to the original XRender path (comp_do_repaint_xrender) so the
 *     WM still works on software-only X servers.
 *
 * Compile-time guard: the entire file is dead code unless -DCOMPOSITOR.
 */

#ifdef COMPOSITOR

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <X11/Xlib.h>
#include <X11/Xlibint.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrender.h>

/* GL/GLX — only included when -DCOMPOSITOR is active */
#define GL_GLEXT_PROTOTYPES
#define GLX_GLXEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glx.h>
#include <GL/glxext.h>

#include <glib.h>

/* XSetEventQueueOwner — from libX11-xcb (Xlib-xcb.h).
 * Declared inline here so we don't require the libx11-xcb-dev headers;
 * the symbol is provided by -lX11-xcb at link time. */
#ifndef XCBOwnsEventQueue
typedef enum {
	XlibOwnsEventQueue = 0,
	XCBOwnsEventQueue  = 1
} XEventQueueOwner;
void XSetEventQueueOwner(Display *dpy, XEventQueueOwner owner);
#endif

#include "awm.h"
#include "log.h"
#include "compositor.h"

/* -------------------------------------------------------------------------
 * Internal types
 * ---------------------------------------------------------------------- */

typedef struct CompWin {
	Window  win;
	Client *client; /* NULL for override_redirect windows        */
	Pixmap  pixmap; /* XCompositeNameWindowPixmap result          */
	/* XRender path (fallback) */
	Picture picture; /* XRenderCreatePicture on pixmap             */
	/* GL/TFP path */
	GLXPixmap       glx_pixmap; /* glXCreatePixmap (TFP)                  */
	GLuint          texture;    /* GL_TEXTURE_2D bound via TFP             */
	Damage          damage;
	int             x, y, w, h, bw; /* last known geometry                */
	int             depth;   /* window depth                              */
	int             argb;    /* depth == 32                               */
	double          opacity; /* 0.0 – 1.0                                 */
	int             redirected; /* 0 = bypass (fullscreen/bypass-hint)    */
	int             hidden;     /* 1 = moved off-screen by showhide()        */
	int             ever_damaged; /* 0 = no damage received yet (since map) */
	struct CompWin *next;
} CompWin;

/* -------------------------------------------------------------------------
 * Module state (all static, no global pollution)
 * ---------------------------------------------------------------------- */

static struct {
	int    active;
	Window overlay;

	/* ---- GL path (primary) ---- */
	int      use_gl; /* 1 if TFP is available and context ok  */
	Display *gl_dpy; /* separate X connection used for all GLX calls;
	                  * keeps GLX/Mesa XCB sequence numbering isolated
	                  * from the WM's Xlib connection to prevent
	                  * "Xlib: sequence lost" warnings on DRI3/Intel */
	GLXContext glx_ctx;
	GLXWindow  glx_win; /* GLX drawable wrapping comp.overlay    */
	GLuint     prog;    /* shader program                        */
	GLuint     vbo;     /* quad vertex buffer                    */
	GLuint     vao;     /* vertex array object                   */
	/* uniform locations */
	GLint u_tex;
	GLint u_opacity;
	GLint u_flip_y; /* reserved: flip V coord (unused for TFP) */
	GLint u_solid;  /* 1 = draw solid colour quad (borders)  */
	GLint u_color;  /* solid colour (borders)                */
	GLint u_rect;   /* x, y, w, h in pixels (cached)         */
	GLint u_screen; /* screen width, height (cached)         */
	/* TFP function pointers (loaded at runtime) */
	PFNGLXBINDTEXIMAGEEXTPROC    glx_bind_tex;
	PFNGLXRELEASETEXIMAGEEXTPROC glx_release_tex;
	/* swap control (vsync) */
	PFNGLXSWAPINTERVALMESAPROC glx_swap_interval;
	/* GLX_EXT_buffer_age partial repaint ring buffer.
	 * Each slot holds the bounding box of one past frame's dirty region.
	 * ring_idx is the slot that will be written after the *next* swap. */
#define DAMAGE_RING_SIZE 6
	XRectangle damage_ring[DAMAGE_RING_SIZE];
	int        ring_idx; /* next write position (0..DAMAGE_RING_SIZE-1) */
	int        has_buffer_age; /* 1 if GLX_EXT_buffer_age is available    */

	/* ---- XRender path (fallback) ---- */
	Picture target; /* XRenderPicture on overlay                   */
	Pixmap  back_pixmap;
	Picture back;            /* XRenderPicture on back_pixmap       */
	Picture alpha_pict[256]; /* pre-built 1×1 RepeatNormal solids   */

	/* ---- Shared state ---- */
	int           damage_ev_base;
	int           damage_err_base;
	int           xfixes_ev_base;
	int           xfixes_err_base;
	guint         repaint_id; /* GLib idle source id, 0 = none            */
	int           paused;     /* 1 = overlay hidden, repaints suppressed  */
	XserverRegion dirty;      /* accumulated dirty region                 */
	CompWin      *windows;
	GMainContext *ctx;
	/* Wallpaper support */
	Atom      atom_rootpmap;
	Atom      atom_esetroot;
	Pixmap    wallpaper_pixmap;
	Picture   wallpaper_pict;       /* XRender picture (fallback path)       */
	GLXPixmap wallpaper_glx_pixmap; /* TFP GLXPixmap for wallpaper (GL)   */
	GLuint    wallpaper_texture;    /* GL texture for wallpaper (GL)      */
	/* XRender extension codes — needed for error whitelisting */
	int render_request_base;
	int render_err_base;
	/* GLX extension codes — needed for error whitelisting */
	int glx_req_base;
	int glx_err_base;
	/* XShape extension — optional */
	int has_xshape;
	int shape_ev_base;
	int shape_err_base;
	/* _NET_WM_CM_Sn selection ownership */
	Window cm_owner_win; /* utility window used to hold the CM selection */
	Atom   atom_cm_sn;   /* _NET_WM_CM_S<screen> atom                    */
	/* Per-window opacity atom */
	Atom atom_net_wm_opacity; /* _NET_WM_WINDOW_OPACITY                 */
} comp;

/* ---- compositor compile-time invariants ---- */
_Static_assert(sizeof(unsigned short) == 2,
    "unsigned short must be 16 bits for XRenderColor alpha/channel field "
    "scaling");
_Static_assert(sizeof(short) == 2,
    "short must be 16 bits to match XRectangle.x and XRectangle.y field "
    "types");
_Static_assert(sizeof(Pixmap) == sizeof(unsigned long),
    "Pixmap (XID) must equal unsigned long in size for format-32 property "
    "reads");

/* -------------------------------------------------------------------------
 * Forward declarations
 * ---------------------------------------------------------------------- */

static void     comp_add_by_xid(Window w);
static CompWin *comp_find_by_xid(Window w);
static CompWin *comp_find_by_client(Client *c);
static void     comp_free_win(CompWin *cw);
static void     comp_refresh_pixmap(CompWin *cw);
static void     comp_apply_shape(CompWin *cw);
static void     comp_update_wallpaper(void);
static void     schedule_repaint(void);
static void     comp_do_repaint(void);
static void     comp_do_repaint_gl(void);
static void     comp_do_repaint_xrender(void);
static gboolean comp_repaint_idle(gpointer data);
static Picture  make_alpha_picture(double a);

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

/* Suppress X errors for a single call */
static int
comp_xerror_ignore(Display *d, XErrorEvent *e)
{
	(void) d;
	(void) e;
	return 0;
}

static int (*prev_xerror)(Display *, XErrorEvent *) = NULL;
static int xerror_ignore_depth                      = 0;

static void
xerror_push_ignore(void)
{
	if (xerror_ignore_depth++ == 0)
		prev_xerror = XSetErrorHandler(comp_xerror_ignore);
}

static void
xerror_pop(void)
{
	XSync(dpy, False);
	if (--xerror_ignore_depth == 0) {
		XSetErrorHandler(prev_xerror);
		prev_xerror = NULL;
	}
}

/* -------------------------------------------------------------------------
 * XESetWireToEvent workaround (picom-derived)
 *
 * Some X extension libraries (notably GL/DRI2 drivers) register
 * XESetWireToEvent handlers.  When Xlib processes the event queue it calls
 * those handlers on every event that matches the registered type.  If the
 * compositor pulls events while a GL driver has such a handler registered,
 * Xlib may call it on unrelated events, corrupting GL state or causing
 * "lost sequence number" warnings.
 *
 * Fix (same as picom): before handing an event to our handler, temporarily
 * clear the wire-to-event hook for that event type so Xlib won't invoke it
 * again for this event, tweak the sequence number to keep Xlib consistent,
 * call the original hook on a dummy event to let it do any internal Xlib
 * bookkeeping, then restore the hook.
 * ---------------------------------------------------------------------- */

void
compositor_fix_wire_to_event(XEvent *ev)
{
	int type = ev->type & 0x7f; /* strip SendEvent bit */
	Bool (*proc)(Display *, XEvent *, xEvent *);

	if (!comp.use_gl)
		return;

	proc = XESetWireToEvent(dpy, type, NULL);
	if (proc) {
		XEvent dummy;
		/* Restore immediately so future events of this type are handled. */
		XESetWireToEvent(dpy, type, proc);
		/* Adjust sequence number: the driver compares it against its own
		 * last-known value; feeding it the current last-known processed
		 * request prevents "lost sequence number" warnings. */
		{
			unsigned long seq_saved = ev->xany.serial;
			ev->xany.serial = LastKnownRequestProcessed(dpy) & 0xffffffff;
			proc(dpy, &dummy, (xEvent *) ev);
			ev->xany.serial = seq_saved;
		}
	}
}

static Picture
make_alpha_picture(double a)
{
	XRenderPictFormat       *fmt;
	XRenderPictureAttributes pa;
	Pixmap                   pix;
	Picture                  pic;
	XRenderColor             col;

	fmt       = XRenderFindStandardFormat(dpy, PictStandardA8);
	pix       = XCreatePixmap(dpy, root, 1, 1, 8);
	pa.repeat = RepeatNormal;
	pic       = XRenderCreatePicture(dpy, pix, fmt, CPRepeat, &pa);
	col.alpha = (unsigned short) (a * 0xffff);
	col.red = col.green = col.blue = 0;
	XRenderFillRectangle(dpy, PictOpSrc, pic, &col, 0, 0, 1, 1);
	XFreePixmap(dpy, pix);
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

/* Pick the first FBConfig in the list whose RGB channel sizes are exactly
 * 8 bits, and optionally also has an 8-bit alpha channel.
 * Mesa on Intel Arc sorts 10bpc configs first; if we blindly take
 * fbc[0] we get a 10bpc config and the TFP texture data is misinterpreted.
 * Returns NULL if none qualify (caller falls back to fbc[0]). */
static GLXFBConfig
gl_pick_8bpc(GLXFBConfig *fbc, int nfbc, int need_alpha)
{
	int i;
	for (i = 0; i < nfbc; i++) {
		int r = 0, g = 0, b = 0, a = 0;
		glXGetFBConfigAttrib(comp.gl_dpy, fbc[i], GLX_RED_SIZE, &r);
		glXGetFBConfigAttrib(comp.gl_dpy, fbc[i], GLX_GREEN_SIZE, &g);
		glXGetFBConfigAttrib(comp.gl_dpy, fbc[i], GLX_BLUE_SIZE, &b);
		glXGetFBConfigAttrib(comp.gl_dpy, fbc[i], GLX_ALPHA_SIZE, &a);
		if (r == 8 && g == 8 && b == 8 && (!need_alpha || a == 8))
			return fbc[i];
	}
	return NULL;
}

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

/* Attempt to initialise the GL/TFP path.  Returns 0 on success, -1 if GL
 * is unavailable (caller falls back to XRender). */
static int
comp_init_gl(void)
{
	const char  *exts;
	GLXFBConfig *fbc    = NULL;
	int          nfbc   = 0;
	GLXFBConfig  chosen = NULL;
	GLuint       vert = 0, frag = 0;
	int          i;

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

	/* --- Check for GLX_EXT_texture_from_pixmap -------------------------*/
	exts = glXQueryExtensionsString(comp.gl_dpy, screen);
	if (!exts || !strstr(exts, "GLX_EXT_texture_from_pixmap")) {
		awm_warn("compositor: GLX_EXT_texture_from_pixmap unavailable, "
		         "falling back to XRender");
		return -1;
	}

	/* Load TFP function pointers */
	comp.glx_bind_tex = (PFNGLXBINDTEXIMAGEEXTPROC) glXGetProcAddressARB(
	    (const GLubyte *) "glXBindTexImageEXT");
	comp.glx_release_tex = (PFNGLXRELEASETEXIMAGEEXTPROC) glXGetProcAddressARB(
	    (const GLubyte *) "glXReleaseTexImageEXT");
	if (!comp.glx_bind_tex || !comp.glx_release_tex) {
		awm_warn("compositor: glXBindTexImageEXT not found, "
		         "falling back to XRender");
		return -1;
	}

	/* Vsync via GLX_MESA_swap_control (preferred on Mesa) */
	comp.glx_swap_interval = (PFNGLXSWAPINTERVALMESAPROC) glXGetProcAddressARB(
	    (const GLubyte *) "glXSwapIntervalMESA");

	/* --- Find an appropriate FBConfig for the overlay window -----------
	 * We want one that:
	 *   - supports GLX_DOUBLEBUFFER
	 *   - supports GLX_BIND_TO_TEXTURE_RGBA_EXT (for ARGB windows)
	 *   - depth matches DefaultDepth
	 * Start with a broad query and filter manually.
	 */
	{
		int attr[] = { GLX_RENDER_TYPE, GLX_RGBA_BIT, GLX_DRAWABLE_TYPE,
			GLX_WINDOW_BIT | GLX_PIXMAP_BIT, GLX_DOUBLEBUFFER, True,
			GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8,
			GLX_ALPHA_SIZE, 8, GLX_DEPTH_SIZE, 0, GLX_STENCIL_SIZE, 0, None };
		fbc        = glXChooseFBConfig(comp.gl_dpy, screen, attr, &nfbc);
	}

	if (!fbc || nfbc == 0) {
		awm_warn("compositor: no suitable GLX FBConfig found, "
		         "falling back to XRender");
		return -1;
	}

	/* Pick the first 8bpc config that supports TFP for both RGB and RGBA.
	 * Mesa on Intel Arc sorts 10bpc configs first; using one of those
	 * causes the GPU to misinterpret 8-bit X11 pixmap data as 10-bit,
	 * producing badly garbled colours.  We must insist on r==g==b==8. */
	for (i = 0; i < nfbc; i++) {
		int rgb_ok = 0, rgba_ok = 0, r = 0, g = 0, b = 0;
		glXGetFBConfigAttrib(comp.gl_dpy, fbc[i], GLX_RED_SIZE, &r);
		glXGetFBConfigAttrib(comp.gl_dpy, fbc[i], GLX_GREEN_SIZE, &g);
		glXGetFBConfigAttrib(comp.gl_dpy, fbc[i], GLX_BLUE_SIZE, &b);
		if (r != 8 || g != 8 || b != 8)
			continue;
		glXGetFBConfigAttrib(
		    comp.gl_dpy, fbc[i], GLX_BIND_TO_TEXTURE_RGB_EXT, &rgb_ok);
		glXGetFBConfigAttrib(
		    comp.gl_dpy, fbc[i], GLX_BIND_TO_TEXTURE_RGBA_EXT, &rgba_ok);
		if (rgb_ok && rgba_ok) {
			chosen = fbc[i];
			break;
		}
	}
	/* Fallback: any 8bpc config (no alpha needed for the overlay window) */
	if (!chosen)
		chosen = gl_pick_8bpc(fbc, nfbc, 0);
	/* Last resort: first config (may be 10bpc, colours will be wrong) */
	if (!chosen)
		chosen = fbc[0];
	XFree(fbc);

	/* --- Create GL context ---------------------------------------------*/
	{
		/* Request a core 2.1 context for compatibility with glxext.h
		 * function signatures; we only use GLSL 1.30 features anyway. */
		int ctx_attr[] = { GLX_CONTEXT_MAJOR_VERSION_ARB, 2,
			GLX_CONTEXT_MINOR_VERSION_ARB, 1, None };
		PFNGLXCREATECONTEXTATTRIBSARBPROC create_ctx =
		    (PFNGLXCREATECONTEXTATTRIBSARBPROC) glXGetProcAddressARB(
		        (const GLubyte *) "glXCreateContextAttribsARB");
		if (create_ctx) {
			xerror_push_ignore();
			comp.glx_ctx =
			    create_ctx(comp.gl_dpy, chosen, NULL, True, ctx_attr);
			xerror_pop();
		}
		if (!comp.glx_ctx) {
			/* Fall back to legacy glXCreateNewContext */
			comp.glx_ctx = glXCreateNewContext(
			    comp.gl_dpy, chosen, GLX_RGBA_TYPE, NULL, True);
		}
	}
	if (!comp.glx_ctx) {
		awm_warn("compositor: failed to create GL context, "
		         "falling back to XRender");
		return -1;
	}

	/* --- Wrap the overlay window as a GLX drawable ---------------------*/
	comp.glx_win = glXCreateWindow(comp.gl_dpy, chosen, comp.overlay, NULL);
	if (!comp.glx_win) {
		awm_warn("compositor: glXCreateWindow failed, "
		         "falling back to XRender");
		glXDestroyContext(comp.gl_dpy, comp.glx_ctx);
		comp.glx_ctx = NULL;
		return -1;
	}

	if (!glXMakeCurrent(comp.gl_dpy, comp.glx_win, comp.glx_ctx)) {
		awm_warn("compositor: glXMakeCurrent failed, "
		         "falling back to XRender");
		glXDestroyWindow(comp.gl_dpy, comp.glx_win);
		glXDestroyContext(comp.gl_dpy, comp.glx_ctx);
		comp.glx_ctx = 0;
		comp.glx_win = 0;
		return -1;
	}

	/* Enable vsync */
	if (comp.glx_swap_interval)
		comp.glx_swap_interval(1);

	/* --- Compile shaders -----------------------------------------------*/
	vert = gl_compile_shader(GL_VERTEX_SHADER, vert_src);
	frag = gl_compile_shader(GL_FRAGMENT_SHADER, frag_src);
	if (!vert || !frag) {
		if (vert)
			glDeleteShader(vert);
		if (frag)
			glDeleteShader(frag);
		glXMakeCurrent(comp.gl_dpy, None, NULL);
		glXDestroyWindow(comp.gl_dpy, comp.glx_win);
		glXDestroyContext(comp.gl_dpy, comp.glx_ctx);
		comp.glx_ctx = 0;
		comp.glx_win = 0;
		return -1;
	}

	comp.prog = gl_link_program(vert, frag);
	glDeleteShader(vert);
	glDeleteShader(frag);
	if (!comp.prog) {
		glXMakeCurrent(comp.gl_dpy, None, NULL);
		glXDestroyWindow(comp.gl_dpy, comp.glx_win);
		glXDestroyContext(comp.gl_dpy, comp.glx_ctx);
		comp.glx_ctx = 0;
		comp.glx_win = 0;
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

	/* Cache the u_rect and u_screen locations via use */
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

	/* Detect GLX_EXT_buffer_age for partial repaints */
	{
		const char *glx_exts = glXQueryExtensionsString(comp.gl_dpy, screen);
		comp.has_buffer_age =
		    (glx_exts && strstr(glx_exts, "GLX_EXT_buffer_age") != NULL);
	}
	memset(comp.damage_ring, 0, sizeof(comp.damage_ring));
	comp.ring_idx = 0;

	awm_debug("compositor: GL/TFP path initialised (renderer: %s, "
	          "buffer_age=%d)",
	    (const char *) glGetString(GL_RENDERER), comp.has_buffer_age);
	return 0;
}

/* Create a TFP GLXPixmap + GL texture for cw->pixmap.
 * Fills cw->glx_pixmap and cw->texture.
 * Called after comp_refresh_pixmap() sets cw->pixmap. */
static void
comp_bind_tfp(CompWin *cw)
{
	int          tfp_attr[7];
	int          n    = 0;
	GLXFBConfig *fbc  = NULL;
	int          nfbc = 0;

	if (!comp.use_gl || !cw->pixmap)
		return;

	/* Release any existing TFP resources first */
	if (cw->texture) {
		if (cw->glx_pixmap)
			comp.glx_release_tex(
			    comp.gl_dpy, cw->glx_pixmap, GLX_FRONT_LEFT_EXT);
		glDeleteTextures(1, &cw->texture);
		cw->texture = 0;
	}
	if (cw->glx_pixmap) {
		glXDestroyPixmap(comp.gl_dpy, cw->glx_pixmap);
		cw->glx_pixmap = 0;
	}

	/* Find an FBConfig that supports the pixmap's depth */
	{
		int depth_attr[] = { GLX_RENDER_TYPE, GLX_RGBA_BIT, GLX_DRAWABLE_TYPE,
			GLX_PIXMAP_BIT, GLX_BIND_TO_TEXTURE_TARGETS_EXT,
			GLX_TEXTURE_2D_BIT_EXT,
			cw->argb ? GLX_BIND_TO_TEXTURE_RGBA_EXT
			         : GLX_BIND_TO_TEXTURE_RGB_EXT,
			True, GLX_DOUBLEBUFFER, False, None };
		fbc = glXChooseFBConfig(comp.gl_dpy, screen, depth_attr, &nfbc);
	}
	if (!fbc || nfbc == 0) {
		if (fbc)
			XFree(fbc);
		return;
	}

	/* Build TFP pixmap attribute list */
	tfp_attr[n++] = GLX_TEXTURE_TARGET_EXT;
	tfp_attr[n++] = GLX_TEXTURE_2D_EXT;
	tfp_attr[n++] = GLX_TEXTURE_FORMAT_EXT;
	tfp_attr[n++] =
	    cw->argb ? GLX_TEXTURE_FORMAT_RGBA_EXT : GLX_TEXTURE_FORMAT_RGB_EXT;
	tfp_attr[n++] = GLX_MIPMAP_TEXTURE_EXT;
	tfp_attr[n++] = False;
	tfp_attr[n++] = None;

	/* Use an 8bpc config to avoid the 10bpc misinterpretation bug.
	 * For ARGB windows we must also have a==8 or the alpha channel is
	 * lost and the window renders fully opaque. */
	{
		GLXFBConfig cfg = gl_pick_8bpc(fbc, nfbc, cw->argb);
		if (!cfg)
			cfg = fbc[0]; /* last resort */
		xerror_push_ignore();
		cw->glx_pixmap =
		    glXCreatePixmap(comp.gl_dpy, cfg, cw->pixmap, tfp_attr);
		XSync(comp.gl_dpy, False);
		xerror_pop();
	}

	XFree(fbc);

	if (!cw->glx_pixmap)
		return;

	/* Allocate GL texture */
	glGenTextures(1, &cw->texture);
	glBindTexture(GL_TEXTURE_2D, cw->texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	/* Bind the TFP pixmap as the texture's image */
	xerror_push_ignore();
	comp.glx_bind_tex(comp.gl_dpy, cw->glx_pixmap, GLX_FRONT_LEFT_EXT, NULL);
	XSync(comp.gl_dpy, False);
	xerror_pop();

	glBindTexture(GL_TEXTURE_2D, 0);
}

/* Release TFP resources for a CompWin without touching its XRender Picture.
 * Must be called before comp_free_win() when use_gl==1 so textures are
 * released before the underlying pixmap is freed. */
static void
comp_release_tfp(CompWin *cw)
{
	if (!comp.use_gl)
		return;

	if (cw->texture) {
		if (cw->glx_pixmap)
			comp.glx_release_tex(
			    comp.gl_dpy, cw->glx_pixmap, GLX_FRONT_LEFT_EXT);
		glDeleteTextures(1, &cw->texture);
		cw->texture = 0;
	}
	if (cw->glx_pixmap) {
		glXDestroyPixmap(comp.gl_dpy, cw->glx_pixmap);
		cw->glx_pixmap = 0;
	}
}

/* -------------------------------------------------------------------------
 * compositor_init
 * ---------------------------------------------------------------------- */

int
compositor_init(GMainContext *ctx)
{
	int                      comp_ev, comp_err, render_ev, render_err;
	int                      xfixes_major = 2, xfixes_minor = 0;
	int                      damage_major = 1, damage_minor = 1;
	int                      i;
	XRenderPictFormat       *fmt;
	XRenderPictureAttributes pa;
	XserverRegion            empty;

	memset(&comp, 0, sizeof(comp));
	comp.ctx = ctx;

	/* --- Check required extensions ----------------------------------------
	 */

	if (!XCompositeQueryExtension(dpy, &comp_ev, &comp_err)) {
		awm_warn("compositor: XComposite extension not available");
		return -1;
	}
	{
		int major = 0, minor = 2;
		XCompositeQueryVersion(dpy, &major, &minor);
		if (major < 0 || (major == 0 && minor < 2)) {
			awm_warn("compositor: XComposite >= 0.2 required (got %d.%d)",
			    major, minor);
			return -1;
		}
	}

	if (!XDamageQueryExtension(
	        dpy, &comp.damage_ev_base, &comp.damage_err_base)) {
		awm_warn("compositor: XDamage extension not available");
		return -1;
	}
	XDamageQueryVersion(dpy, &damage_major, &damage_minor);

	if (!XFixesQueryExtension(
	        dpy, &comp.xfixes_ev_base, &comp.xfixes_err_base)) {
		awm_warn("compositor: XFixes extension not available");
		return -1;
	}
	XFixesQueryVersion(dpy, &xfixes_major, &xfixes_minor);

	if (!XRenderQueryExtension(dpy, &render_ev, &render_err)) {
		awm_warn("compositor: XRender extension not available");
		return -1;
	}
	comp.render_err_base = render_err;
	{
		int op, ev_dummy, err_dummy;
		if (XQueryExtension(dpy, "RENDER", &op, &ev_dummy, &err_dummy))
			comp.render_request_base = op;
	}
	/* Query GLX extension opcode/error base for error whitelisting */
	{
		int op, ev_dummy, err_dummy;
		if (XQueryExtension(dpy, "GLX", &op, &ev_dummy, &err_dummy)) {
			comp.glx_req_base = op;
			comp.glx_err_base = err_dummy;
		}
	}

	if (XShapeQueryExtension(dpy, &comp.shape_ev_base, &comp.shape_err_base))
		comp.has_xshape = 1;

	/* --- Redirect all root children ---------------------------------------
	 */

	xerror_push_ignore();
	XCompositeRedirectSubwindows(dpy, root, CompositeRedirectManual);
	XSync(dpy, False);
	xerror_pop();

	/* --- Overlay window ---------------------------------------------------
	 */

	comp.overlay = XCompositeGetOverlayWindow(dpy, root);
	if (!comp.overlay) {
		awm_warn("compositor: failed to get overlay window");
		XCompositeUnredirectSubwindows(dpy, root, CompositeRedirectManual);
		return -1;
	}

	/* Make the overlay click-through */
	empty = XFixesCreateRegion(dpy, NULL, 0);
	XFixesSetWindowShapeRegion(dpy, comp.overlay, ShapeInput, 0, 0, empty);
	XFixesDestroyRegion(dpy, empty);

	/* --- Try to initialise the GL/TFP path --------------------------------
	 */

	/* Open a dedicated Display connection for all GLX operations.
	 * Mesa's DRI3 backend obtains the underlying xcb_connection_t via
	 * XGetXCBConnection() and sends xcb_present_pixmap /
	 * xcb_sync_trigger_fence requests directly over XCB, bypassing Xlib's
	 * request counter.  Xlib's widen_seq() then sees reply/event sequence
	 * numbers it never recorded and fires "Xlib: sequence lost" on every
	 * frame.
	 *
	 * XSetEventQueueOwner(XCBOwnsEventQueue) makes XCB the authoritative
	 * sequence counter for this connection.  All of Xlib's requests are then
	 * routed through XCB's numbering, so Mesa's XCB-sent requests and Xlib's
	 * requests share the same counter and widen_seq() never desynchronises. */
	comp.gl_dpy = XOpenDisplay(NULL);
	if (!comp.gl_dpy) {
		awm_warn("compositor: XOpenDisplay for GL failed, "
		         "GL path unavailable");
	} else {
		XSetEventQueueOwner(comp.gl_dpy, XCBOwnsEventQueue);
	}

	if (comp.gl_dpy && comp_init_gl() != 0) {
		/* GL path unavailable — set up XRender back-buffer + target */
		fmt = XRenderFindVisualFormat(dpy, DefaultVisual(dpy, screen));
		pa.subwindow_mode = IncludeInferiors;
		comp.target =
		    XRenderCreatePicture(dpy, comp.overlay, fmt, CPSubwindowMode, &pa);
		comp.back_pixmap = XCreatePixmap(dpy, root, (unsigned int) sw,
		    (unsigned int) sh, (unsigned int) DefaultDepth(dpy, screen));
		comp.back        = XRenderCreatePicture(
            dpy, comp.back_pixmap, fmt, CPSubwindowMode, &pa);
		for (i = 0; i < 256; i++)
			comp.alpha_pict[i] = make_alpha_picture((double) i / 255.0);
	}

	/* --- Dirty region (starts as full screen) -----------------------------
	 */

	{
		XRectangle full = { 0, 0, (unsigned short) sw, (unsigned short) sh };
		comp.dirty      = XFixesCreateRegion(dpy, &full, 1);
	}

	/* --- Claim _NET_WM_CM_S<n> composite manager selection ---------------
	 * Required by ICCCM/EWMH: signals to applications that a compositor is
	 * running and lets us detect if another compositor starts
	 * (SelectionClear).
	 */
	{
		char sel_name[32];
		snprintf(sel_name, sizeof(sel_name), "_NET_WM_CM_S%d", screen);
		comp.atom_cm_sn = XInternAtom(dpy, sel_name, False);

		/* Create a small, invisible utility window to hold the selection. */
		comp.cm_owner_win =
		    XCreateSimpleWindow(dpy, root, -1, -1, 1, 1, 0, 0, 0);

		XSetSelectionOwner(
		    dpy, comp.atom_cm_sn, comp.cm_owner_win, CurrentTime);

		if (XGetSelectionOwner(dpy, comp.atom_cm_sn) != comp.cm_owner_win) {
			awm_warn("compositor: could not claim _NET_WM_CM_S%d — "
			         "another compositor may be running",
			    screen);
			/* Non-fatal: proceed anyway, but log the warning. */
		} else {
			awm_debug("compositor: claimed _NET_WM_CM_S%d selection", screen);
		}

		/* Select SelectionClear on the owner window so we are notified
		 * if another program takes the selection from us. */
		XSelectInput(dpy, comp.cm_owner_win, StructureNotifyMask);
	}

	/* --- Scan existing windows --------------------------------------------
	 */

	{
		Window       root_ret, parent_ret;
		Window      *children  = NULL;
		unsigned int nchildren = 0;
		unsigned int j;

		if (XQueryTree(
		        dpy, root, &root_ret, &parent_ret, &children, &nchildren)) {
			for (j = 0; j < nchildren; j++)
				comp_add_by_xid(children[j]);
			if (children)
				XFree(children);
		}
	}

	/* --- Intern wallpaper atoms and read initial wallpaper ---------------
	 */

	comp.atom_rootpmap = XInternAtom(dpy, "_XROOTPMAP_ID", False);
	comp.atom_esetroot = XInternAtom(dpy, "ESETROOT_PMAP_ID", False);
	comp.atom_net_wm_opacity =
	    XInternAtom(dpy, "_NET_WM_WINDOW_OPACITY", False);
	comp_update_wallpaper();

	comp.active = 1;

	/* Raise overlay so it sits above all windows */
	XRaiseWindow(dpy, comp.overlay);
	XMapWindow(dpy, comp.overlay);

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
		glXMakeCurrent(comp.gl_dpy, None, NULL);
		if (comp.glx_win)
			glXDestroyWindow(comp.gl_dpy, comp.glx_win);
		if (comp.glx_ctx)
			glXDestroyContext(comp.gl_dpy, comp.glx_ctx);
	} else {
		/* XRender path cleanup */
		for (i = 0; i < 256; i++)
			if (comp.alpha_pict[i])
				XRenderFreePicture(dpy, comp.alpha_pict[i]);
		if (comp.back)
			XRenderFreePicture(dpy, comp.back);
		if (comp.back_pixmap)
			XFreePixmap(dpy, comp.back_pixmap);
		if (comp.target)
			XRenderFreePicture(dpy, comp.target);
	}

	if (comp.wallpaper_pict)
		XRenderFreePicture(dpy, comp.wallpaper_pict);

	/* Release cached GL wallpaper texture */
	if (comp.use_gl) {
		if (comp.wallpaper_texture) {
			if (comp.wallpaper_glx_pixmap)
				comp.glx_release_tex(comp.gl_dpy, comp.wallpaper_glx_pixmap,
				    GLX_FRONT_LEFT_EXT);
			glDeleteTextures(1, &comp.wallpaper_texture);
		}
		if (comp.wallpaper_glx_pixmap)
			glXDestroyPixmap(comp.gl_dpy, comp.wallpaper_glx_pixmap);
	}

	if (comp.overlay)
		XCompositeReleaseOverlayWindow(dpy, comp.overlay);

	/* Release _NET_WM_CM_Sn selection */
	if (comp.cm_owner_win) {
		XDestroyWindow(dpy, comp.cm_owner_win);
		comp.cm_owner_win = 0;
	}

	if (comp.dirty)
		XFixesDestroyRegion(dpy, comp.dirty);

	XCompositeUnredirectSubwindows(dpy, root, CompositeRedirectManual);
	XFlush(dpy);

	/* Close the dedicated GL display connection last */
	if (comp.gl_dpy) {
		XCloseDisplay(comp.gl_dpy);
		comp.gl_dpy = NULL;
	}

	comp.active = 0;
}

/* -------------------------------------------------------------------------
 * Window tracking — internal helpers
 * ---------------------------------------------------------------------- */

static CompWin *
comp_find_by_xid(Window w)
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

static void
comp_free_win(CompWin *cw)
{
	/* Deregister shape event mask before releasing other resources.
	 * This is a no-op if the window is already destroyed (X ignores it),
	 * but prevents a leak when comp_free_win is called on live windows
	 * (e.g. unmap of non-client override-redirect windows). */
	if (comp.has_xshape) {
		xerror_push_ignore();
		XShapeSelectInput(dpy, cw->win, 0);
		XSync(dpy, False);
		xerror_pop();
	}

	if (cw->damage) {
		xerror_push_ignore();
		XDamageDestroy(dpy, cw->damage);
		XSync(dpy, False);
		xerror_pop();
		cw->damage = 0;
	}
	if (cw->picture) {
		XRenderFreePicture(dpy, cw->picture);
		cw->picture = None;
	}
	if (cw->pixmap) {
		XFreePixmap(dpy, cw->pixmap);
		cw->pixmap = None;
	}
}

static void
comp_refresh_pixmap(CompWin *cw)
{
	XRenderPictFormat       *fmt;
	XRenderPictureAttributes pa;

	/* Release TFP resources before freeing the pixmap */
	if (comp.use_gl)
		comp_release_tfp(cw);

	if (cw->picture) {
		XRenderFreePicture(dpy, cw->picture);
		cw->picture = None;
	}
	if (cw->pixmap) {
		XFreePixmap(dpy, cw->pixmap);
		cw->pixmap = None;
	}

	/* New pixmap — require a full dirty on its first damage notification. */
	cw->ever_damaged = 0;

	xerror_push_ignore();
	cw->pixmap = XCompositeNameWindowPixmap(dpy, cw->win);
	XSync(dpy, False);
	xerror_pop();

	if (!cw->pixmap)
		return;

	{
		Window       root_ret;
		int          x, y;
		unsigned int w, h, bw, depth;
		if (!XGetGeometry(
		        dpy, cw->pixmap, &root_ret, &x, &y, &w, &h, &bw, &depth)) {
			XFreePixmap(dpy, cw->pixmap);
			cw->pixmap = None;
			return;
		}
	}

	if (comp.use_gl) {
		/* Bind as GL texture via TFP */
		comp_bind_tfp(cw);
	} else {
		/* XRender fallback: create an XRender Picture */
		xerror_push_ignore();
		fmt = XRenderFindVisualFormat(dpy, DefaultVisual(dpy, screen));
		if (cw->argb)
			fmt = XRenderFindStandardFormat(dpy, PictStandardARGB32);
		pa.subwindow_mode = IncludeInferiors;
		cw->picture =
		    XRenderCreatePicture(dpy, cw->pixmap, fmt, CPSubwindowMode, &pa);
		XSync(dpy, False);
		xerror_pop();
		comp_apply_shape(cw);
	}
}

/* Apply the window's ShapeBounding clip region to cw->picture.
 * Only used by the XRender fallback path. */
static void
comp_apply_shape(CompWin *cw)
{
	int         nrects, ordering;
	XRectangle *rects;

	if (!cw->picture)
		return;

	if (!comp.has_xshape) {
		XFixesSetPictureClipRegion(dpy, cw->picture, 0, 0, None);
		return;
	}

	rects =
	    XShapeGetRectangles(dpy, cw->win, ShapeBounding, &nrects, &ordering);

	if (!rects || nrects == 0) {
		if (rects)
			XFree(rects);
		XFixesSetPictureClipRegion(dpy, cw->picture, 0, 0, None);
		return;
	}

	{
		XserverRegion region =
		    XFixesCreateRegion(dpy, rects, (unsigned int) nrects);
		XFixesSetPictureClipRegion(dpy, cw->picture, 0, 0, region);
		XFixesDestroyRegion(dpy, region);
	}
	XFree(rects);
}

/* Read _XROOTPMAP_ID (or ESETROOT_PMAP_ID fallback) and rebuild wallpaper.
 * For the GL path we read the wallpaper pixmap into a GL texture.
 * For the XRender path we keep an XRender Picture. */
static void
comp_update_wallpaper(void)
{
	Atom           actual_type;
	int            actual_fmt;
	unsigned long  nitems, bytes_after;
	unsigned char *prop = NULL;
	Pixmap         pmap = None;
	Atom           atoms[2];
	int            i;

	/* Release previous wallpaper resources */
	if (comp.wallpaper_pict) {
		XRenderFreePicture(dpy, comp.wallpaper_pict);
		comp.wallpaper_pict   = None;
		comp.wallpaper_pixmap = None;
	}
	/* Release cached GL wallpaper texture if any */
	if (comp.use_gl) {
		if (comp.wallpaper_texture) {
			if (comp.wallpaper_glx_pixmap)
				comp.glx_release_tex(comp.gl_dpy, comp.wallpaper_glx_pixmap,
				    GLX_FRONT_LEFT_EXT);
			glDeleteTextures(1, &comp.wallpaper_texture);
			comp.wallpaper_texture = 0;
		}
		if (comp.wallpaper_glx_pixmap) {
			glXDestroyPixmap(comp.gl_dpy, comp.wallpaper_glx_pixmap);
			comp.wallpaper_glx_pixmap = 0;
		}
	}

	atoms[0] = comp.atom_rootpmap;
	atoms[1] = comp.atom_esetroot;

	for (i = 0; i < 2 && pmap == None; i++) {
		prop = NULL;
		if (XGetWindowProperty(dpy, root, atoms[i], 0, 1, False, XA_PIXMAP,
		        &actual_type, &actual_fmt, &nitems, &bytes_after,
		        &prop) == Success &&
		    prop && actual_type == XA_PIXMAP && actual_fmt == 32 &&
		    nitems == 1) {
			pmap = *(Pixmap *) prop;
		}
		if (prop)
			XFree(prop);
	}

	if (pmap == None)
		return;

	/* Always build the XRender picture — used by the XRender fallback path
	 * and as a sentinel meaning "we have a wallpaper" in the GL path. */
	{
		XRenderPictFormat       *fmt;
		XRenderPictureAttributes pa;

		fmt       = XRenderFindVisualFormat(dpy, DefaultVisual(dpy, screen));
		pa.repeat = RepeatNormal;
		xerror_push_ignore();
		comp.wallpaper_pict =
		    XRenderCreatePicture(dpy, pmap, fmt, CPRepeat, &pa);
		XSync(dpy, False);
		xerror_pop();

		if (comp.wallpaper_pict)
			comp.wallpaper_pixmap = pmap;
	}

	/* For the GL path, build and cache a TFP texture for the wallpaper.
	 * This is expensive (GLX round-trips), so we only do it here when the
	 * wallpaper changes — not every frame. */
	if (comp.use_gl && comp.wallpaper_pixmap) {
		int          nfbc = 0;
		GLXFBConfig *fbc  = NULL;
		int wp_attr[]     = { GLX_RENDER_TYPE, GLX_RGBA_BIT, GLX_DRAWABLE_TYPE,
			    GLX_PIXMAP_BIT, GLX_BIND_TO_TEXTURE_TARGETS_EXT,
			    GLX_TEXTURE_2D_BIT_EXT, GLX_BIND_TO_TEXTURE_RGB_EXT, True,
			    GLX_DOUBLEBUFFER, False, None };
		int tfp_attr[]    = { GLX_TEXTURE_TARGET_EXT, GLX_TEXTURE_2D_EXT,
			   GLX_TEXTURE_FORMAT_EXT, GLX_TEXTURE_FORMAT_RGB_EXT,
			   GLX_MIPMAP_TEXTURE_EXT, False, None };

		fbc = glXChooseFBConfig(comp.gl_dpy, screen, wp_attr, &nfbc);
		if (fbc && nfbc > 0) {
			GLXFBConfig wp_cfg = gl_pick_8bpc(fbc, nfbc, 0);
			if (!wp_cfg)
				wp_cfg = fbc[0];
			xerror_push_ignore();
			comp.wallpaper_glx_pixmap = glXCreatePixmap(
			    comp.gl_dpy, wp_cfg, comp.wallpaper_pixmap, tfp_attr);
			XSync(comp.gl_dpy, False);
			xerror_pop();

			if (comp.wallpaper_glx_pixmap) {
				glGenTextures(1, &comp.wallpaper_texture);
				glBindTexture(GL_TEXTURE_2D, comp.wallpaper_texture);
				glTexParameteri(
				    GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
				glTexParameteri(
				    GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
				glTexParameteri(
				    GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
				glTexParameteri(
				    GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
				xerror_push_ignore();
				comp.glx_bind_tex(comp.gl_dpy, comp.wallpaper_glx_pixmap,
				    GLX_FRONT_LEFT_EXT, NULL);
				XSync(comp.gl_dpy, False);
				xerror_pop();

				glBindTexture(GL_TEXTURE_2D, 0);
			}
			XFree(fbc);
		}
	}
}

/* Move cw to just above the window with XID `above_xid` in the stacking
 * order maintained in comp.windows (bottom-to-top linked list).
 * `above_xid == None` means place cw at the bottom of the stack. */
static void
comp_restack_above(CompWin *cw, Window above_xid)
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
comp_add_by_xid(Window w)
{
	XWindowAttributes wa;
	CompWin          *cw;

	if (comp_find_by_xid(w))
		return;

	if (w == comp.overlay)
		return;

	xerror_push_ignore();
	int ok = XGetWindowAttributes(dpy, w, &wa);
	XSync(dpy, False);
	xerror_pop();

	if (!ok)
		return;

	if (wa.class == InputOnly)
		return;

	if (wa.map_state != IsViewable)
		return;

	cw = (CompWin *) calloc(1, sizeof(CompWin));
	if (!cw)
		return;

	cw->win        = w;
	cw->x          = wa.x;
	cw->y          = wa.y;
	cw->w          = wa.width;
	cw->h          = wa.height;
	cw->bw         = wa.border_width;
	cw->depth      = wa.depth;
	cw->argb       = (wa.depth == 32);
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
		xerror_push_ignore();
		cw->damage = XDamageCreate(dpy, w, XDamageReportNonEmpty);
		XSync(dpy, False);
		xerror_pop();
	}

	if (comp.has_xshape)
		XShapeSelectInput(dpy, w, ShapeNotifyMask);

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
				XRectangle    r;
				XserverRegion sr;
				r.x      = (short) cw->x;
				r.y      = (short) cw->y;
				r.width  = (unsigned short) (cw->w + 2 * cw->bw);
				r.height = (unsigned short) (cw->h + 2 * cw->bw);
				sr       = XFixesCreateRegion(dpy, &r, 1);
				XFixesUnionRegion(dpy, comp.dirty, comp.dirty, sr);
				XFixesDestroyRegion(dpy, sr);
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
	CompWin      *cw;
	int           resized;
	XRectangle    old_rect;
	XserverRegion old_r;

	if (!comp.active || !c)
		return;

	cw = comp_find_by_client(c);
	if (!cw)
		return;

	old_rect.x      = (short) cw->x;
	old_rect.y      = (short) cw->y;
	old_rect.width  = (unsigned short) (cw->w + 2 * cw->bw);
	old_rect.height = (unsigned short) (cw->h + 2 * cw->bw);
	old_r           = XFixesCreateRegion(dpy, &old_rect, 1);
	XFixesUnionRegion(dpy, comp.dirty, comp.dirty, old_r);
	XFixesDestroyRegion(dpy, old_r);

	resized = (c->w != cw->w || c->h != cw->h);

	cw->x  = c->x - actual_bw;
	cw->y  = c->y - actual_bw;
	cw->w  = c->w;
	cw->h  = c->h;
	cw->bw = actual_bw;

	{
		XRectangle    new_rect;
		XserverRegion new_r;
		new_rect.x      = (short) cw->x;
		new_rect.y      = (short) cw->y;
		new_rect.width  = (unsigned short) (cw->w + 2 * cw->bw);
		new_rect.height = (unsigned short) (cw->h + 2 * cw->bw);
		new_r           = XFixesCreateRegion(dpy, &new_rect, 1);
		XFixesUnionRegion(dpy, comp.dirty, comp.dirty, new_r);
		XFixesDestroyRegion(dpy, new_r);
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

	xerror_push_ignore();
	if (bypass) {
		XCompositeUnredirectWindow(dpy, c->win, CompositeRedirectManual);
		cw->redirected = 0;
		if (comp.use_gl)
			comp_release_tfp(cw);
		comp_free_win(cw);
	} else {
		XCompositeRedirectWindow(dpy, c->win, CompositeRedirectManual);
		cw->redirected = 1;
		comp_refresh_pixmap(cw);
		if (cw->pixmap && !cw->damage)
			cw->damage = XDamageCreate(dpy, c->win, XDamageReportNonEmpty);
	}
	XSync(dpy, False);
	xerror_pop();

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
	CompWin      *cw;
	XRectangle    r;
	XserverRegion sr;

	if (!comp.active || !c)
		return;

	cw = comp_find_by_client(c);
	if (!cw || cw->bw <= 0)
		return;

	r.x      = (short) cw->x;
	r.y      = (short) cw->y;
	r.width  = (unsigned short) (cw->w + 2 * cw->bw);
	r.height = (unsigned short) (cw->h + 2 * cw->bw);
	sr       = XFixesCreateRegion(dpy, &r, 1);
	XFixesUnionRegion(dpy, comp.dirty, comp.dirty, sr);
	XFixesDestroyRegion(dpy, sr);
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
		XRectangle    r;
		XserverRegion sr;
		r.x      = (short) cw->x;
		r.y      = (short) cw->y;
		r.width  = (unsigned short) (cw->w + 2 * cw->bw);
		r.height = (unsigned short) (cw->h + 2 * cw->bw);
		sr       = XFixesCreateRegion(dpy, &r, 1);
		XFixesUnionRegion(dpy, comp.dirty, comp.dirty, sr);
		XFixesDestroyRegion(dpy, sr);
	}
	schedule_repaint();
}

void
compositor_damage_all(void)
{
	XRectangle full;

	if (!comp.active)
		return;

	full.x = full.y = 0;
	full.width      = (unsigned short) sw;
	full.height     = (unsigned short) sh;
	XFixesSetRegion(dpy, comp.dirty, &full, 1);
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
			XRenderFreePicture(dpy, comp.back);
			comp.back = None;
		}
		if (comp.back_pixmap) {
			XFreePixmap(dpy, comp.back_pixmap);
			comp.back_pixmap = None;
		}
		comp.back_pixmap = XCreatePixmap(dpy, root, (unsigned int) sw,
		    (unsigned int) sh, (unsigned int) DefaultDepth(dpy, screen));
		if (comp.back_pixmap) {
			XRenderPictFormat *fmt =
			    XRenderFindVisualFormat(dpy, DefaultVisual(dpy, screen));
			XRenderPictureAttributes pa = { 0 };
			pa.subwindow_mode           = IncludeInferiors;
			comp.back                   = XRenderCreatePicture(
                dpy, comp.back_pixmap, fmt, CPSubwindowMode, &pa);
		}
	}

	compositor_damage_all();
}

void
compositor_raise_overlay(void)
{
	if (!comp.active)
		return;
	XRaiseWindow(dpy, comp.overlay);
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
		/* Hide overlay: lower it below the fullscreen window so the
		 * window draws directly to the display with no GL overhead. */
		if (comp.repaint_id) {
			g_source_remove(comp.repaint_id);
			comp.repaint_id = 0;
		}
		XLowerWindow(dpy, comp.overlay);
		awm_debug("compositor: suspended (fullscreen unredirect)");
	} else {
		/* Resume: raise overlay and repaint everything. */
		XRaiseWindow(dpy, comp.overlay);
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
	if (!comp.active || !comp.use_gl) {
		*req_base = -1;
		*err_base = -1;
		return;
	}
	*req_base = comp.glx_req_base;
	*err_base = comp.glx_err_base;
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
compositor_handle_event(XEvent *ev)
{
	if (!comp.active)
		return;

	/* Apply the XESetWireToEvent workaround before dispatching.
	 * See compositor_fix_wire_to_event() for a full explanation. */
	compositor_fix_wire_to_event(ev);

	if (ev->type == comp.damage_ev_base + XDamageNotify) {
		XDamageNotifyEvent *dev = (XDamageNotifyEvent *) ev;
		CompWin            *dcw = comp_find_by_xid(dev->drawable);

		if (!dcw) {
			/* Unknown window — just ack the damage and ignore. */
			xerror_push_ignore();
			XDamageSubtract(dpy, dev->damage, None, None);
			xerror_pop();
			schedule_repaint();
			return;
		}

		if (!dcw->ever_damaged) {
			/* First damage since (re)map or pixmap refresh: dirty the
			 * entire window rect rather than the (possibly partial) area
			 * reported in the notify.  Ack by discarding the region. */
			dcw->ever_damaged = 1;
			xerror_push_ignore();
			XDamageSubtract(dpy, dev->damage, None, None);
			xerror_pop();
			{
				XRectangle    r;
				XserverRegion sr;
				r.x      = (short) dcw->x;
				r.y      = (short) dcw->y;
				r.width  = (unsigned short) (dcw->w + 2 * dcw->bw);
				r.height = (unsigned short) (dcw->h + 2 * dcw->bw);
				sr       = XFixesCreateRegion(dpy, &r, 1);
				XFixesUnionRegion(dpy, comp.dirty, comp.dirty, sr);
				XFixesDestroyRegion(dpy, sr);
			}
		} else {
			/* Subsequent damage: fetch the exact damage region from the
			 * server via XDamageSubtract so we can dirty only what changed.
			 * dev->area is only the bounding box of the event's inline area,
			 * not the full accumulated server-side damage region. */
			XserverRegion dmg_region = XFixesCreateRegion(dpy, NULL, 0);
			xerror_push_ignore();
			XDamageSubtract(dpy, dev->damage, None, dmg_region);
			xerror_pop();
			/* Translate from window-local to screen coordinates */
			XFixesTranslateRegion(dpy, dmg_region, dcw->x, dcw->y);
			XFixesUnionRegion(dpy, comp.dirty, comp.dirty, dmg_region);
			XFixesDestroyRegion(dpy, dmg_region);
		}

		schedule_repaint();
		return;
	}

	if (ev->type == MapNotify) {
		XMapEvent *mev = (XMapEvent *) ev;
		if (mev->event == root)
			comp_add_by_xid(mev->window);
		schedule_repaint();
		return;
	}

	if (ev->type == UnmapNotify) {
		XUnmapEvent *uev = (XUnmapEvent *) ev;
		CompWin     *cw  = comp_find_by_xid(uev->window);
		if (cw && !cw->client) {
			CompWin *prev = NULL, *cur;
			{
				XRectangle    r;
				XserverRegion sr;
				r.x      = (short) cw->x;
				r.y      = (short) cw->y;
				r.width  = (unsigned short) (cw->w + 2 * cw->bw);
				r.height = (unsigned short) (cw->h + 2 * cw->bw);
				sr       = XFixesCreateRegion(dpy, &r, 1);
				XFixesUnionRegion(dpy, comp.dirty, comp.dirty, sr);
				XFixesDestroyRegion(dpy, sr);
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

	if (ev->type == ConfigureNotify) {
		XConfigureEvent *cev = (XConfigureEvent *) ev;
		CompWin         *cw  = comp_find_by_xid(cev->window);
		if (cw) {
			if (cw->client)
				return;

			int resized = (cev->width != cw->w || cev->height != cw->h);

			{
				XRectangle    old_rect;
				XserverRegion old_r;
				old_rect.x      = (short) cw->x;
				old_rect.y      = (short) cw->y;
				old_rect.width  = (unsigned short) (cw->w + 2 * cw->bw);
				old_rect.height = (unsigned short) (cw->h + 2 * cw->bw);
				old_r           = XFixesCreateRegion(dpy, &old_rect, 1);
				XFixesUnionRegion(dpy, comp.dirty, comp.dirty, old_r);
				XFixesDestroyRegion(dpy, old_r);
			}

			cw->x  = cev->x;
			cw->y  = cev->y;
			cw->w  = cev->width;
			cw->h  = cev->height;
			cw->bw = cev->border_width;

			/* Restack in our internal list to match the X stacking order.
			 * cev->above is the sibling directly below this window. */
			comp_restack_above(cw, cev->above);

			if (cw->redirected && resized)
				comp_refresh_pixmap(cw);

			schedule_repaint();
		}
		return;
	}

	if (ev->type == DestroyNotify) {
		XDestroyWindowEvent *dev  = (XDestroyWindowEvent *) ev;
		CompWin             *prev = NULL, *cw;
		for (cw = comp.windows; cw; cw = cw->next) {
			if (cw->win == dev->window) {
				{
					XRectangle    r;
					XserverRegion sr;
					r.x      = (short) cw->x;
					r.y      = (short) cw->y;
					r.width  = (unsigned short) (cw->w + 2 * cw->bw);
					r.height = (unsigned short) (cw->h + 2 * cw->bw);
					sr       = XFixesCreateRegion(dpy, &r, 1);
					XFixesUnionRegion(dpy, comp.dirty, comp.dirty, sr);
					XFixesDestroyRegion(dpy, sr);
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

	if (ev->type == PropertyNotify) {
		XPropertyEvent *pev = (XPropertyEvent *) ev;
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
				Atom           actual_type;
				int            actual_fmt;
				unsigned long  nitems, bytes_after;
				unsigned char *prop = NULL;

				if (XGetWindowProperty(dpy, pev->window,
				        comp.atom_net_wm_opacity, 0, 1, False, XA_CARDINAL,
				        &actual_type, &actual_fmt, &nitems, &bytes_after,
				        &prop) == Success &&
				    prop && actual_type == XA_CARDINAL && actual_fmt == 32 &&
				    nitems == 1) {
					unsigned long raw = *(unsigned long *) prop;
					compositor_set_opacity(cw->client, raw);
				} else {
					/* Property deleted — restore to fully opaque */
					compositor_set_opacity(cw->client, 0xFFFFFFFFUL);
				}
				if (prop)
					XFree(prop);
			}
		}
		return;
	}

	if (comp.has_xshape && ev->type == comp.shape_ev_base + ShapeNotify) {
		XShapeEvent *sev = (XShapeEvent *) ev;
		if (sev->kind == ShapeBounding) {
			CompWin *cw = comp_find_by_xid(sev->window);
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

	if (ev->type == SelectionClear) {
		XSelectionClearEvent *sce = (XSelectionClearEvent *) ev;
		if (sce->selection == comp.atom_cm_sn) {
			/* Another compositor has claimed our selection — shut down. */
			awm_warn("compositor: lost _NET_WM_CM_S%d selection to another "
			         "compositor; disabling compositing",
			    screen);
			compositor_cleanup();
		}
		return;
	}
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
		XEvent ev;
		while (
		    XCheckTypedEvent(dpy, comp.damage_ev_base + XDamageNotify, &ev)) {
			compositor_fix_wire_to_event(&ev);
			compositor_handle_event(&ev);
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
 * Fetch the bounding box of comp.dirty as a single XRectangle.
 * Returns 1 on success, 0 if the region is empty or the fetch fails.
 * On failure *out is set to the full screen rect.
 */
static int
dirty_get_bbox(XRectangle *out)
{
	int         nrects = 0;
	XRectangle *rects  = XFixesFetchRegion(dpy, comp.dirty, &nrects);

	if (!rects || nrects == 0) {
		if (rects)
			XFree(rects);
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
	XFree(rects);

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
	CompWin   *cw;
	XRectangle scissor;
	int        use_scissor = 0;

	/* --- Partial repaint via GLX_EXT_buffer_age + glScissor ------------- */
	if (comp.has_buffer_age) {
		unsigned int age = 0;
		glXQueryDrawable(
		    comp.gl_dpy, comp.glx_win, GLX_BACK_BUFFER_AGE_EXT, &age);

		/* age==0 means undefined (e.g. first frame or after resize);
		 * fall back to full repaint.  age==1 means back buffer is one
		 * frame old — only this frame's dirty rect needs repainting. */
		if (age > 0 && age <= (unsigned int) DAMAGE_RING_SIZE) {
			/* Collect current dirty bbox */
			XRectangle cur;
			dirty_get_bbox(&cur);

			/* Union current dirty with the past (age-1) frames */
			int x1 = cur.x, y1 = cur.y;
			int x2 = x1 + cur.width, y2 = y1 + cur.height;
			for (unsigned int a = 1; a < age; a++) {
				int slot = ((comp.ring_idx - (int) a) + DAMAGE_RING_SIZE * 2) %
				    DAMAGE_RING_SIZE;
				XRectangle *r = &comp.damage_ring[slot];
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
		/* Re-bind to ensure the GPU sees the latest pixmap contents */
		if (comp.wallpaper_glx_pixmap) {
			comp.glx_release_tex(
			    comp.gl_dpy, comp.wallpaper_glx_pixmap, GLX_FRONT_LEFT_EXT);
			comp.glx_bind_tex(comp.gl_dpy, comp.wallpaper_glx_pixmap,
			    GLX_FRONT_LEFT_EXT, NULL);
		}
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
	xerror_push_ignore();

	glBindVertexArray(comp.vao);
	glActiveTexture(GL_TEXTURE0);

	for (cw = comp.windows; cw; cw = cw->next) {
		if (!cw->redirected || !cw->texture || cw->hidden)
			continue;

		/* Re-bind the TFP texture so the GL sees the latest pixmap
		 * contents (the server may have updated it since last frame). */
		glBindTexture(GL_TEXTURE_2D, cw->texture);
		if (cw->glx_pixmap) {
			comp.glx_release_tex(
			    comp.gl_dpy, cw->glx_pixmap, GLX_FRONT_LEFT_EXT);
			comp.glx_bind_tex(
			    comp.gl_dpy, cw->glx_pixmap, GLX_FRONT_LEFT_EXT, NULL);
		}

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
			XRenderColor xrc =
			    scheme[sel ? SchemeSel : SchemeNorm][ColBorder].color;
			float        r  = (float) xrc.red / 65535.0f;
			float        g  = (float) xrc.green / 65535.0f;
			float        b  = (float) xrc.blue / 65535.0f;
			float        a  = (float) xrc.alpha / 65535.0f;
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
	/* Drain pending GLX replies from the per-window TFP rebind calls above
	 * before popping the error handler, so any errors on gl_dpy are
	 * processed before Xlib's sequence counter advances further. */
	XSync(comp.gl_dpy, False);
	xerror_pop();

	glUseProgram(0);

	if (use_scissor)
		glDisable(GL_SCISSOR_TEST);

	/* Reset dirty region */
	XFixesSetRegion(dpy, comp.dirty, NULL, 0);

	/* Present — glXSwapBuffers is vsync-aware (swap interval = 1).
	 * Re-check paused immediately before the swap: if a fullscreen bypass
	 * raced in between the repaint start and here, the overlay window may
	 * already be lowered and the GL context in an inconsistent state.
	 * Skipping the swap is safe — the dirty region is already cleared. */
	if (!comp.paused)
		glXSwapBuffers(comp.gl_dpy, comp.glx_win);
	/* Drain any pending GLX replies so Xlib's 16-bit sequence counter on
	 * gl_dpy does not wrap and trigger "Xlib: sequence lost" warnings. */
	XSync(comp.gl_dpy, False);
}

/* -------------------------------------------------------------------------
 * XRender repaint path (fallback for software-only X servers)
 * ---------------------------------------------------------------------- */

static void
comp_do_repaint_xrender(void)
{
	CompWin     *cw;
	XRenderColor bg_color = { 0, 0, 0, 0xffff };

	/* Clip back-buffer to dirty region */
	XFixesSetPictureClipRegion(dpy, comp.back, 0, 0, comp.dirty);

	/* Paint background */
	if (comp.wallpaper_pict) {
		XRenderComposite(dpy, PictOpSrc, comp.wallpaper_pict, None, comp.back,
		    0, 0, 0, 0, 0, 0, (unsigned int) sw, (unsigned int) sh);
	} else {
		XRenderFillRectangle(dpy, PictOpSrc, comp.back, &bg_color, 0, 0,
		    (unsigned int) sw, (unsigned int) sh);
	}

	/* Walk windows bottom-to-top using our internal list.
	 * comp.windows is maintained in bottom-to-top order by comp_restack_above
	 * and comp_add_by_xid, eliminating the per-frame XQueryTree round-trip. */
	xerror_push_ignore();

	for (cw = comp.windows; cw; cw = cw->next) {
		int     alpha_idx;
		Picture mask;

		if (!cw->redirected || cw->picture == None || cw->hidden)
			continue;

		alpha_idx = (int) (cw->opacity * 255.0 + 0.5);
		if (alpha_idx < 0)
			alpha_idx = 0;
		if (alpha_idx > 255)
			alpha_idx = 255;

		if (cw->argb || alpha_idx < 255) {
			mask = comp.alpha_pict[alpha_idx];
			XRenderComposite(dpy, PictOpOver, cw->picture, mask, comp.back, 0,
			    0, 0, 0, cw->x + cw->bw, cw->y + cw->bw, (unsigned int) cw->w,
			    (unsigned int) cw->h);
		} else {
			XRenderComposite(dpy, PictOpSrc, cw->picture, None, comp.back, 0,
			    0, 0, 0, cw->x + cw->bw, cw->y + cw->bw, (unsigned int) cw->w,
			    (unsigned int) cw->h);
		}

		if (cw->client && cw->bw > 0) {
			int          sel = (selmon && cw->client == selmon->sel);
			XRenderColor bc =
			    scheme[sel ? SchemeSel : SchemeNorm][ColBorder].color;
			unsigned int bw = (unsigned int) cw->bw;
			unsigned int ow = (unsigned int) cw->w + 2 * bw;
			unsigned int oh = (unsigned int) cw->h + 2 * bw;

			XRenderFillRectangle(
			    dpy, PictOpSrc, comp.back, &bc, cw->x, cw->y, ow, bw);
			XRenderFillRectangle(dpy, PictOpSrc, comp.back, &bc, cw->x,
			    cw->y + (int) (oh - bw), ow, bw);
			XRenderFillRectangle(dpy, PictOpSrc, comp.back, &bc, cw->x,
			    cw->y + (int) bw, bw, (unsigned int) cw->h);
			XRenderFillRectangle(dpy, PictOpSrc, comp.back, &bc,
			    cw->x + (int) (ow - bw), cw->y + (int) bw, bw,
			    (unsigned int) cw->h);
		}
	}

	xerror_pop();

	/* Blit full back-buffer to overlay — unconditional, no clip */
	XFixesSetPictureClipRegion(dpy, comp.target, 0, 0, None);
	XRenderComposite(dpy, PictOpSrc, comp.back, None, comp.target, 0, 0, 0, 0,
	    0, 0, (unsigned int) sw, (unsigned int) sh);

	/* Reset dirty region */
	XFixesSetRegion(dpy, comp.dirty, NULL, 0);

	XFixesSetPictureClipRegion(dpy, comp.back, 0, 0, None);
	XFlush(dpy);
}

#endif /* COMPOSITOR */

/* Satisfy ISO C99: a translation unit must contain at least one declaration.
 */
typedef int compositor_translation_unit_nonempty;
