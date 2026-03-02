/* compositor_egl.c — EGL/GL compositor backend for awm
 *
 * Implements the CompBackend vtable for the EGL + KHR_image_pixmap path.
 * Included in the build only when -DCOMPOSITOR is active.
 *
 * All private state (EGL handles, GL objects, damage ring, etc.) is kept in
 * the file-scope static `egl` struct.  Shared compositor state is accessed
 * through the `comp` extern defined in compositor.c.
 */

#ifdef COMPOSITOR

#include <assert.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <xcb/xcb.h>
#include <xcb/xfixes.h>

#include <cairo/cairo.h>

#include "awm.h"
#include "wmstate.h"
#include "log.h"
#include "compositor_backend.h"
#include "compositor.h"

/* -------------------------------------------------------------------------
 * Private backend state
 * ---------------------------------------------------------------------- */

#define DAMAGE_RING_SIZE 6

static struct {
	xcb_connection_t *gl_xc; /* dedicated XCB connection for EGL/Mesa;
	                          * avoids Mesa's DRI3 XCB calls corrupting the
	                          * main xc sequence counter                    */
	EGLDisplay egl_dpy;
	EGLConfig  egl_cfg; /* saved at init for surface recreation on resize */
	EGLContext egl_ctx;
	EGLSurface egl_win;
	GLuint     prog;
	GLuint     vbo;
	GLuint     vao;
	/* uniform locations */
	GLint u_tex;
	GLint u_tint; /* vec4: RGBA multiply (opacity = tint.a) */
	GLint u_solid;
	GLint u_color;
	GLint u_rect;
	GLint u_proj;        /* mat4: pixel-to-NDC projection, set once */
	GLint u_mask;        /* sampler2D: per-window soft alpha mask */
	GLint u_mask_offset; /* vec2: top-left of mask in screen space */
	GLint u_has_mask;    /* int 0/1: whether mask sampler is active */
	/* EGL_KHR_image_pixmap function pointers */
	PFNEGLCREATEIMAGEKHRPROC            egl_create_image;
	PFNEGLDESTROYIMAGEKHRPROC           egl_destroy_image;
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC egl_image_target_tex;
	/* EGL_EXT_buffer_age partial repaint ring */
	xcb_rectangle_t damage_ring[DAMAGE_RING_SIZE];
	int             ring_idx;
	int             has_buffer_age;
	/* Wallpaper */
	EGLImageKHR wallpaper_egl_image;
	GLuint      wallpaper_texture;
} egl;

/* -------------------------------------------------------------------------
 * GLSL shader source
 * ---------------------------------------------------------------------- */

/* Vertex shader: maps pixel coordinates to NDC via precomputed projection. */
static const char *vert_src =
    "#version 330 core\n"
    "in vec2 a_pos;\n"
    "in vec2 a_uv;\n"
    "out vec2 v_uv;\n"
    "uniform vec4 u_rect;\n"
    "uniform mat4 u_proj;\n"
    "void main() {\n"
    "    vec2 px = u_rect.xy + a_pos * u_rect.zw;\n"
    "    gl_Position = u_proj * vec4(px, 0.0, 1.0);\n"
    "    v_uv = a_uv;\n"
    "}\n";

/* Bayer 8x8 ordered dither — reduces banding on 8-bit output.
 * Returns a per-pixel offset in [0, 1/255) that is added before quantisation.
 * coord should be gl_FragCoord.xy (pixel centre, integer-aligned). */
static const char *frag_src =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "out vec4 frag_color;\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec4      u_tint;\n"
    "uniform int       u_solid;\n"
    "uniform vec4      u_color;\n"
    "uniform sampler2D u_mask;\n"
    "uniform vec2      u_mask_offset;\n"
    "uniform int       u_has_mask;\n"
    "\n"
    "float bayer8(vec2 coord) {\n"
    "    int x = int(mod(coord.x, 8.0));\n"
    "    int y = int(mod(coord.y, 8.0));\n"
    "    int bayer[64] = int[64](\n"
    "         0, 32,  8, 40,  2, 34, 10, 42,\n"
    "        48, 16, 56, 24, 50, 18, 58, 26,\n"
    "        12, 44,  4, 36, 14, 46,  6, 38,\n"
    "        60, 28, 52, 20, 62, 30, 54, 22,\n"
    "         3, 35, 11, 43,  1, 33,  9, 41,\n"
    "        51, 19, 59, 27, 49, 17, 57, 25,\n"
    "        15, 47,  7, 39, 13, 45,  5, 37,\n"
    "        63, 31, 55, 23, 61, 29, 53, 21\n"
    "    );\n"
    "    return float(bayer[y * 8 + x]) / 64.0;\n"
    "}\n"
    "\n"
    "void main() {\n"
    "    vec4 c;\n"
    "    if (u_solid == 1) {\n"
    "        c = u_color * u_tint;\n"
    "    } else {\n"
    "        c = texture(u_tex, v_uv) * u_tint;\n"
    "        /* Bayer 8x8 dither: add sub-LSB noise to reduce banding */\n"
    "        float noise = (bayer8(gl_FragCoord.xy) - 0.5) / 255.0;\n"
    "        c.rgb = clamp(c.rgb + noise, 0.0, 1.0);\n"
    "    }\n"
    "    if (u_has_mask == 1) {\n"
    "        vec2 mask_uv = (gl_FragCoord.xy - u_mask_offset)\n"
    "                       / vec2(textureSize(u_mask, 0));\n"
    "        c.a *= texture(u_mask, mask_uv).r;\n"
    "    }\n"
    "    frag_color = c;\n"
    "}\n";

/* -------------------------------------------------------------------------
 * GL helpers
 * ---------------------------------------------------------------------- */

/* Build a column-major orthographic projection mapping pixel (x,y) to NDC,
 * with Y flipped so that pixel (0,0) is the top-left of the g_plat.screen.
 * Result is written into out[16] in column-major order (OpenGL convention).
 *
 *   out = { 2/W, 0,  0, 0,
 *           0, -2/H, 0, 0,
 *           0,    0, 0, 0,
 *          -1,    1, 0, 1 }
 */
static void
make_proj(float out[16], int w, int h)
{
	out[0] = 2.0f / (float) w;
	out[4] = 0.0f;
	out[1] = 0.0f;
	out[5] = -2.0f / (float) h;
	out[2] = 0.0f;
	out[6] = 0.0f;
	out[3] = 0.0f;
	out[7] = 0.0f;

	out[8]  = 0.0f;
	out[12] = -1.0f;
	out[9]  = 0.0f;
	out[13] = 1.0f;
	out[10] = 0.0f;
	out[14] = 0.0f;
	out[11] = 0.0f;
	out[15] = 1.0f;
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
		awm_warn("compositor/egl: shader compile error: %s", buf);
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
		awm_warn("compositor/egl: shader link error: %s", buf);
		glDeleteProgram(p);
		return 0;
	}
	return p;
}

/* -------------------------------------------------------------------------
 * Backend vtable — init
 * ---------------------------------------------------------------------- */

static int
egl_init(void)
{
	const char *egl_exts;
	EGLConfig   cfg     = NULL;
	EGLint      num_cfg = 0;
	GLuint      vert = 0, frag = 0;

	static const float quad[] = {
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

	/* --- Open dedicated XCB connection for EGL/Mesa ---------------------
	 * Mesa's DRI3/gallium backend sends XCB requests on the connection it
	 * is given.  Using a separate connection keeps Mesa's traffic off the
	 * main xc so the main sequence counter is never corrupted.
	 */
	egl.gl_xc = xcb_connect(NULL, NULL);
	if (!egl.gl_xc || xcb_connection_has_error(egl.gl_xc)) {
		if (egl.gl_xc) {
			xcb_disconnect(egl.gl_xc);
			egl.gl_xc = NULL;
		}
		awm_warn("compositor/egl: xcb_connect for GL failed — "
		         "EGL path unavailable");
		return -1;
	}

	/* --- Get EGL display -----------------------------------------------*/
	{
		PFNEGLGETPLATFORMDISPLAYEXTPROC get_plat_dpy =
		    (PFNEGLGETPLATFORMDISPLAYEXTPROC) eglGetProcAddress(
		        "eglGetPlatformDisplayEXT");
		if (get_plat_dpy) {
			egl.egl_dpy = get_plat_dpy(EGL_PLATFORM_XCB_EXT, egl.gl_xc, NULL);
			awm_debug("compositor/egl: used "
			          "eglGetPlatformDisplayEXT(EGL_PLATFORM_XCB_EXT)");
		} else {
			egl.egl_dpy = eglGetDisplay((EGLNativeDisplayType) egl.gl_xc);
			awm_debug("compositor/egl: eglGetPlatformDisplayEXT unavailable, "
			          "used legacy eglGetDisplay");
		}
	}
	if (egl.egl_dpy == EGL_NO_DISPLAY) {
		awm_warn("compositor/egl: eglGetDisplay failed — "
		         "falling back to XRender");
		xcb_disconnect(egl.gl_xc);
		egl.gl_xc = NULL;
		return -1;
	}

	{
		EGLint major = 0, minor = 0;
		if (!eglInitialize(egl.egl_dpy, &major, &minor)) {
			awm_warn("compositor/egl: eglInitialize failed (0x%x) — "
			         "falling back to XRender",
			    (unsigned int) eglGetError());
			egl.egl_dpy = EGL_NO_DISPLAY;
			xcb_disconnect(egl.gl_xc);
			egl.gl_xc = NULL;
			return -1;
		}
		awm_debug("compositor/egl: EGL %d.%d initialised", major, minor);
	}

	egl_exts = eglQueryString(egl.egl_dpy, EGL_EXTENSIONS);

	if (!egl_exts || !strstr(egl_exts, "EGL_KHR_image_base") ||
	    !strstr(egl_exts, "EGL_KHR_image_pixmap")) {
		awm_warn("compositor/egl: EGL_KHR_image_base/pixmap unavailable — "
		         "falling back to XRender");
		eglTerminate(egl.egl_dpy);
		egl.egl_dpy = EGL_NO_DISPLAY;
		xcb_disconnect(egl.gl_xc);
		egl.gl_xc = NULL;
		return -1;
	}

	egl.egl_create_image =
	    (PFNEGLCREATEIMAGEKHRPROC) eglGetProcAddress("eglCreateImageKHR");
	egl.egl_destroy_image =
	    (PFNEGLDESTROYIMAGEKHRPROC) eglGetProcAddress("eglDestroyImageKHR");
	egl.egl_image_target_tex =
	    (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC) eglGetProcAddress(
	        "glEGLImageTargetTexture2DOES");

	if (!egl.egl_create_image || !egl.egl_destroy_image ||
	    !egl.egl_image_target_tex) {
		awm_warn("compositor/egl: EGL image extension procs not found — "
		         "falling back to XRender");
		eglTerminate(egl.egl_dpy);
		egl.egl_dpy = EGL_NO_DISPLAY;
		xcb_disconnect(egl.gl_xc);
		egl.gl_xc = NULL;
		return -1;
	}

	if (!eglBindAPI(EGL_OPENGL_API)) {
		awm_warn("compositor/egl: eglBindAPI(EGL_OPENGL_API) failed (0x%x) — "
		         "falling back to XRender",
		    (unsigned int) eglGetError());
		eglTerminate(egl.egl_dpy);
		egl.egl_dpy = EGL_NO_DISPLAY;
		xcb_disconnect(egl.gl_xc);
		egl.gl_xc = NULL;
		return -1;
	}

	{
		EGLint attr[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
			EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8,
			EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
		if (!eglChooseConfig(egl.egl_dpy, attr, &cfg, 1, &num_cfg) ||
		    num_cfg == 0) {
			EGLint attr2[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
				EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8,
				EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };
			if (!eglChooseConfig(egl.egl_dpy, attr2, &cfg, 1, &num_cfg) ||
			    num_cfg == 0) {
				awm_warn("compositor/egl: no suitable EGL config found — "
				         "falling back to XRender");
				eglTerminate(egl.egl_dpy);
				egl.egl_dpy = EGL_NO_DISPLAY;
				xcb_disconnect(egl.gl_xc);
				egl.gl_xc = NULL;
				return -1;
			}
		}
	}

	{
		EGLint ctx_attr[] = { EGL_CONTEXT_MAJOR_VERSION, 3,
			EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE };
		egl.egl_ctx =
		    eglCreateContext(egl.egl_dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
	}
	if (egl.egl_ctx == EGL_NO_CONTEXT) {
		awm_warn("compositor/egl: eglCreateContext failed (0x%x) — "
		         "falling back to XRender",
		    (unsigned int) eglGetError());
		eglTerminate(egl.egl_dpy);
		egl.egl_dpy = EGL_NO_DISPLAY;
		xcb_disconnect(egl.gl_xc);
		egl.gl_xc = NULL;
		return -1;
	}

	egl.egl_cfg = cfg; /* save for surface recreation on resize */
	egl.egl_win = eglCreateWindowSurface(
	    egl.egl_dpy, cfg, (EGLNativeWindowType) comp.overlay, NULL);
	if (egl.egl_win == EGL_NO_SURFACE) {
		awm_warn("compositor/egl: eglCreateWindowSurface failed (0x%x) — "
		         "falling back to XRender",
		    (unsigned int) eglGetError());
		eglDestroyContext(egl.egl_dpy, egl.egl_ctx);
		egl.egl_ctx = EGL_NO_CONTEXT;
		eglTerminate(egl.egl_dpy);
		egl.egl_dpy = EGL_NO_DISPLAY;
		xcb_disconnect(egl.gl_xc);
		egl.gl_xc = NULL;
		return -1;
	}

	if (!eglMakeCurrent(egl.egl_dpy, egl.egl_win, egl.egl_win, egl.egl_ctx)) {
		awm_warn("compositor/egl: eglMakeCurrent failed (0x%x) — "
		         "falling back to XRender",
		    (unsigned int) eglGetError());
		eglDestroySurface(egl.egl_dpy, egl.egl_win);
		eglDestroyContext(egl.egl_dpy, egl.egl_ctx);
		egl.egl_win = EGL_NO_SURFACE;
		egl.egl_ctx = EGL_NO_CONTEXT;
		eglTerminate(egl.egl_dpy);
		egl.egl_dpy = EGL_NO_DISPLAY;
		xcb_disconnect(egl.gl_xc);
		egl.gl_xc = NULL;
		return -1;
	}

	eglSwapInterval(egl.egl_dpy, 0);

	{
		const char *gl_exts = (const char *) glGetString(GL_EXTENSIONS);
		if (!gl_exts || !strstr(gl_exts, "GL_OES_EGL_image")) {
			awm_warn("compositor/egl: GL_OES_EGL_image unavailable — "
			         "falling back to XRender");
			eglMakeCurrent(
			    egl.egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
			eglDestroySurface(egl.egl_dpy, egl.egl_win);
			eglDestroyContext(egl.egl_dpy, egl.egl_ctx);
			egl.egl_win = EGL_NO_SURFACE;
			egl.egl_ctx = EGL_NO_CONTEXT;
			eglTerminate(egl.egl_dpy);
			egl.egl_dpy = EGL_NO_DISPLAY;
			xcb_disconnect(egl.gl_xc);
			egl.gl_xc = NULL;
			return -1;
		}
	}

	vert = gl_compile_shader(GL_VERTEX_SHADER, vert_src);
	frag = gl_compile_shader(GL_FRAGMENT_SHADER, frag_src);
	if (!vert || !frag) {
		if (vert)
			glDeleteShader(vert);
		if (frag)
			glDeleteShader(frag);
		eglMakeCurrent(
		    egl.egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		eglDestroySurface(egl.egl_dpy, egl.egl_win);
		eglDestroyContext(egl.egl_dpy, egl.egl_ctx);
		egl.egl_win = EGL_NO_SURFACE;
		egl.egl_ctx = EGL_NO_CONTEXT;
		eglTerminate(egl.egl_dpy);
		egl.egl_dpy = EGL_NO_DISPLAY;
		xcb_disconnect(egl.gl_xc);
		egl.gl_xc = NULL;
		return -1;
	}

	egl.prog = gl_link_program(vert, frag);
	glDeleteShader(vert);
	glDeleteShader(frag);
	if (!egl.prog) {
		eglMakeCurrent(
		    egl.egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		eglDestroySurface(egl.egl_dpy, egl.egl_win);
		eglDestroyContext(egl.egl_dpy, egl.egl_ctx);
		egl.egl_win = EGL_NO_SURFACE;
		egl.egl_ctx = EGL_NO_CONTEXT;
		eglTerminate(egl.egl_dpy);
		egl.egl_dpy = EGL_NO_DISPLAY;
		xcb_disconnect(egl.gl_xc);
		egl.gl_xc = NULL;
		return -1;
	}

	egl.u_tex         = glGetUniformLocation(egl.prog, "u_tex");
	egl.u_tint        = glGetUniformLocation(egl.prog, "u_tint");
	egl.u_solid       = glGetUniformLocation(egl.prog, "u_solid");
	egl.u_color       = glGetUniformLocation(egl.prog, "u_color");
	egl.u_rect        = glGetUniformLocation(egl.prog, "u_rect");
	egl.u_proj        = glGetUniformLocation(egl.prog, "u_proj");
	egl.u_mask        = glGetUniformLocation(egl.prog, "u_mask");
	egl.u_mask_offset = glGetUniformLocation(egl.prog, "u_mask_offset");
	egl.u_has_mask    = glGetUniformLocation(egl.prog, "u_has_mask");

	glUseProgram(egl.prog);
	glUniform1i(egl.u_tex, 0);
	glUniform1i(egl.u_mask, 1);
	glUniform1i(egl.u_has_mask, 0);
	glUseProgram(0);

	glGenVertexArrays(1, &egl.vao);
	glGenBuffers(1, &egl.vbo);
	glBindVertexArray(egl.vao);
	glBindBuffer(GL_ARRAY_BUFFER, egl.vbo);
	glBufferData(
	    GL_ARRAY_BUFFER, (GLsizeiptr) sizeof(quad), quad, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(
	    0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *) 0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
	    (void *) (2 * sizeof(float)));
	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	/* Upload the initial projection matrix now that g_plat.sw/sh are known */
	{
		float proj[16];
		make_proj(proj, g_plat.sw, g_plat.sh);
		glUseProgram(egl.prog);
		glUniformMatrix4fv(egl.u_proj, 1, GL_FALSE, proj);
		glUseProgram(0);
	}

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_SCISSOR_TEST);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glViewport(0, 0, g_plat.sw, g_plat.sh);

	egl.has_buffer_age =
	    (egl_exts && strstr(egl_exts, "EGL_EXT_buffer_age") != NULL);
	/* Pre-fill the damage ring with full-screen rectangles so that buffer-age
	 * history lookups on early frames always produce conservative
	 * (full-screen) scissors rather than zero-sized no-ops.  Without this, a
	 * driver that returns age=1 on the very first frame would scissor to only
	 * the current dirty region while the buffer contains uninitialised GPU
	 * memory. */
	{
		int i;
		for (i = 0; i < DAMAGE_RING_SIZE; i++) {
			egl.damage_ring[i].x      = 0;
			egl.damage_ring[i].y      = 0;
			egl.damage_ring[i].width  = (unsigned short) g_plat.sw;
			egl.damage_ring[i].height = (unsigned short) g_plat.sh;
		}
	}
	egl.ring_idx = 0;

	egl.wallpaper_egl_image = EGL_NO_IMAGE_KHR;
	egl.wallpaper_texture   = 0;

	awm_debug("compositor/egl: EGL/GL path initialised (renderer: %s, "
	          "buffer_age=%d)",
	    (const char *) glGetString(GL_RENDERER), egl.has_buffer_age);
	return 0;
}

/* -------------------------------------------------------------------------
 * Backend vtable — cleanup
 * ---------------------------------------------------------------------- */

static void
egl_cleanup(void)
{
	/* Wallpaper resources are already freed by egl_release_wallpaper(), which
	 * compositor_cleanup() calls before calling cleanup().  Do not free them
	 * again here to avoid a double-free if the ordering is ever changed. */
	if (egl.prog)
		glDeleteProgram(egl.prog);
	if (egl.vao)
		glDeleteVertexArrays(1, &egl.vao);
	if (egl.vbo)
		glDeleteBuffers(1, &egl.vbo);
	eglMakeCurrent(
	    egl.egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	if (egl.egl_win != EGL_NO_SURFACE)
		eglDestroySurface(egl.egl_dpy, egl.egl_win);
	if (egl.egl_ctx != EGL_NO_CONTEXT)
		eglDestroyContext(egl.egl_dpy, egl.egl_ctx);
	if (egl.egl_dpy != EGL_NO_DISPLAY)
		eglTerminate(egl.egl_dpy);

	/* Close the dedicated GL connection last — after all EGL objects are
	 * destroyed by eglTerminate above. */
	if (egl.gl_xc) {
		xcb_disconnect(egl.gl_xc);
		egl.gl_xc = NULL;
	}
}

/* -------------------------------------------------------------------------
 * Backend vtable — bind / release pixmap
 * ---------------------------------------------------------------------- */

static void
egl_bind_pixmap(CompWin *cw)
{
	EGLint img_attr[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };

	assert(cw != NULL);
	if (!cw->pixmap)
		return;

	/* Release any existing EGL image / texture first */
	if (cw->texture) {
		glDeleteTextures(1, &cw->texture);
		cw->texture = 0;
	}
	if (cw->egl_image != EGL_NO_IMAGE_KHR) {
		egl.egl_destroy_image(egl.egl_dpy, cw->egl_image);
		cw->egl_image = EGL_NO_IMAGE_KHR;
	}

	cw->egl_image = egl.egl_create_image(egl.egl_dpy, EGL_NO_CONTEXT,
	    EGL_NATIVE_PIXMAP_KHR, (EGLClientBuffer) (uintptr_t) cw->pixmap,
	    img_attr);

	if (cw->egl_image == EGL_NO_IMAGE_KHR) {
		awm_warn("compositor/egl: eglCreateImageKHR failed for window 0x%x "
		         "(pixmap 0x%x, error 0x%x) — window will not be painted",
		    (unsigned) cw->win, (unsigned) cw->pixmap,
		    (unsigned) eglGetError());
		return;
	}

	glGenTextures(1, &cw->texture);
	glBindTexture(GL_TEXTURE_2D, cw->texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	egl.egl_image_target_tex(GL_TEXTURE_2D, (GLeglImageOES) cw->egl_image);
	glBindTexture(GL_TEXTURE_2D, 0);
}

static void
egl_release_pixmap(CompWin *cw)
{
	assert(cw != NULL);
	if (cw->texture) {
		glDeleteTextures(1, &cw->texture);
		cw->texture = 0;
	}
	if (cw->egl_image != EGL_NO_IMAGE_KHR) {
		egl.egl_destroy_image(egl.egl_dpy, cw->egl_image);
		cw->egl_image = EGL_NO_IMAGE_KHR;
	}
}

/* -------------------------------------------------------------------------
 * Backend vtable — wallpaper
 * ---------------------------------------------------------------------- */

static void
egl_release_wallpaper(void)
{
	if (egl.wallpaper_texture) {
		glDeleteTextures(1, &egl.wallpaper_texture);
		egl.wallpaper_texture = 0;
	}
	if (egl.wallpaper_egl_image != EGL_NO_IMAGE_KHR) {
		egl.egl_destroy_image(egl.egl_dpy, egl.wallpaper_egl_image);
		egl.wallpaper_egl_image = EGL_NO_IMAGE_KHR;
	}
}

static void
egl_update_wallpaper(void)
{
	EGLint img_attr[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };

	/* comp.wallpaper_pixmap is set by comp_update_wallpaper() before calling
	 * here. */
	egl.wallpaper_egl_image = egl.egl_create_image(egl.egl_dpy, EGL_NO_CONTEXT,
	    EGL_NATIVE_PIXMAP_KHR,
	    (EGLClientBuffer) (uintptr_t) comp.wallpaper_pixmap, img_attr);

	if (egl.wallpaper_egl_image != EGL_NO_IMAGE_KHR) {
		glGenTextures(1, &egl.wallpaper_texture);
		glBindTexture(GL_TEXTURE_2D, egl.wallpaper_texture);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		egl.egl_image_target_tex(
		    GL_TEXTURE_2D, (GLeglImageOES) egl.wallpaper_egl_image);
		glBindTexture(GL_TEXTURE_2D, 0);
	} else {
		awm_warn("compositor/egl: eglCreateImageKHR failed for wallpaper "
		         "(pixmap 0x%x, error 0x%x) — background will be black",
		    (unsigned) comp.wallpaper_pixmap, (unsigned) eglGetError());
	}
}

/* -------------------------------------------------------------------------
 * Backend vtable — notify_resize
 * ---------------------------------------------------------------------- */

static void
egl_notify_resize(void)
{
	/* The overlay window has been resized to g_plat.sw×sh by the caller.
	 * Destroy and recreate the EGL window surface — EGL surfaces are
	 * bound to the window at creation time and do not auto-resize. */
	eglMakeCurrent(
	    egl.egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	if (egl.egl_win != EGL_NO_SURFACE) {
		eglDestroySurface(egl.egl_dpy, egl.egl_win);
		egl.egl_win = EGL_NO_SURFACE;
	}
	egl.egl_win = eglCreateWindowSurface(
	    egl.egl_dpy, egl.egl_cfg, (EGLNativeWindowType) comp.overlay, NULL);
	if (egl.egl_win == EGL_NO_SURFACE) {
		awm_warn("compositor/egl: eglCreateWindowSurface failed on "
		         "resize (0x%x) — compositor disabled",
		    (unsigned int) eglGetError());
		comp.active = 0;
		return;
	}
	if (!eglMakeCurrent(egl.egl_dpy, egl.egl_win, egl.egl_win, egl.egl_ctx)) {
		awm_warn("compositor/egl: eglMakeCurrent failed on resize "
		         "(0x%x) — compositor disabled",
		    (unsigned int) eglGetError());
		eglDestroySurface(egl.egl_dpy, egl.egl_win);
		egl.egl_win = EGL_NO_SURFACE;
		comp.active = 0;
		return;
	}
	glViewport(0, 0, g_plat.sw, g_plat.sh);
	/* Re-upload projection matrix for the new screen dimensions */
	{
		float proj[16];
		make_proj(proj, g_plat.sw, g_plat.sh);
		glUseProgram(egl.prog);
		glUniformMatrix4fv(egl.u_proj, 1, GL_FALSE, proj);
		glUseProgram(0);
	}
	/* Old damage ring entries are in the old coordinate space — pre-fill with
	 * full-screen rects so the first DAMAGE_RING_SIZE frames after resize
	 * always produce a full repaint rather than a stale scissor. */
	{
		int i;
		for (i = 0; i < DAMAGE_RING_SIZE; i++) {
			egl.damage_ring[i].x      = 0;
			egl.damage_ring[i].y      = 0;
			egl.damage_ring[i].width  = (unsigned short) g_plat.sw;
			egl.damage_ring[i].height = (unsigned short) g_plat.sh;
		}
	}
	egl.ring_idx = 0;
}

/* -------------------------------------------------------------------------
 * Backend vtable — repaint
 * ---------------------------------------------------------------------- */

static void
egl_repaint(void)
{
	CompWin        *cw;
	xcb_rectangle_t scissor;
	int             use_scissor = 0;

	assert(egl.egl_dpy != EGL_NO_DISPLAY);
	assert(egl.egl_ctx != EGL_NO_CONTEXT);

	/* --- Partial repaint via EGL_EXT_buffer_age + glScissor ------------- */
	if (egl.has_buffer_age) {
		EGLint age = 0;
		eglQuerySurface(egl.egl_dpy, egl.egl_win, EGL_BUFFER_AGE_EXT, &age);

		if (age > 0 && age <= (EGLint) DAMAGE_RING_SIZE) {
			xcb_rectangle_t cur;

			/* Use the CPU-side bbox — no round-trip to X server */
			if (comp.dirty_bbox_valid) {
				cur.x      = (short) comp.dirty_x1;
				cur.y      = (short) comp.dirty_y1;
				cur.width  = (unsigned short) (comp.dirty_x2 - comp.dirty_x1);
				cur.height = (unsigned short) (comp.dirty_y2 - comp.dirty_y1);
			} else {
				cur.x = cur.y = 0;
				cur.width     = (unsigned short) g_plat.sw;
				cur.height    = (unsigned short) g_plat.sh;
			}

			int x1 = cur.x, y1 = cur.y;
			int x2 = x1 + cur.width, y2 = y1 + cur.height;
			for (EGLint a = 1; a < age; a++) {
				int slot = ((egl.ring_idx - (int) a) + DAMAGE_RING_SIZE * 2) %
				    DAMAGE_RING_SIZE;
				xcb_rectangle_t *r = &egl.damage_ring[slot];
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

			egl.damage_ring[egl.ring_idx] = cur;
			egl.ring_idx = (egl.ring_idx + 1) % DAMAGE_RING_SIZE;

			if (x1 < 0)
				x1 = 0;
			if (y1 < 0)
				y1 = 0;
			if (x2 > g_plat.sw)
				x2 = g_plat.sw;
			if (y2 > g_plat.sh)
				y2 = g_plat.sh;

			scissor.x      = (short) x1;
			scissor.y      = (short) y1;
			scissor.width  = (unsigned short) (x2 - x1);
			scissor.height = (unsigned short) (y2 - y1);

			if (scissor.width > 0 && scissor.height > 0)
				use_scissor = 1;
		} else {
			egl.damage_ring[egl.ring_idx].x      = 0;
			egl.damage_ring[egl.ring_idx].y      = 0;
			egl.damage_ring[egl.ring_idx].width  = (unsigned short) g_plat.sw;
			egl.damage_ring[egl.ring_idx].height = (unsigned short) g_plat.sh;
			egl.ring_idx = (egl.ring_idx + 1) % DAMAGE_RING_SIZE;
		}
	}

	if (use_scissor) {
		glEnable(GL_SCISSOR_TEST);
		glScissor(scissor.x, g_plat.sh - scissor.y - scissor.height, scissor.width,
		    scissor.height);
	}

	glUseProgram(egl.prog);
	glUniform1i(egl.u_tex, 0);
	glUniform1i(egl.u_has_mask, 0);

	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	if (egl.wallpaper_texture) {
		glBindTexture(GL_TEXTURE_2D, egl.wallpaper_texture);
		glUniform4f(egl.u_rect, 0.0f, 0.0f, (float) g_plat.sw, (float) g_plat.sh);
		glUniform4f(egl.u_tint, 1.0f, 1.0f, 1.0f, 1.0f);
		glUniform1i(egl.u_solid, 0);
		glBindVertexArray(egl.vao);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		glBindVertexArray(0);
		glBindTexture(GL_TEXTURE_2D, 0);
	}

	glBindVertexArray(egl.vao);
	glActiveTexture(GL_TEXTURE0);

	for (cw = comp.windows; cw; cw = cw->next) {
		if (!cw->redirected || !cw->texture || cw->hidden)
			continue;

		glBindTexture(GL_TEXTURE_2D, cw->texture);
		glUniform4f(egl.u_rect, (float) cw->x, (float) cw->y,
		    (float) (cw->w + 2 * cw->bw), (float) (cw->h + 2 * cw->bw));
		glUniform4f(egl.u_tint, 1.0f, 1.0f, 1.0f, (float) cw->opacity);
		glUniform1i(egl.u_solid, 0);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

		if (cw->client && cw->bw > 0) {
			int sel =
			    (g_awm.selmon_num >= 0 && cw->client == g_awm_selmon->sel);
			Clr         *bc = &scheme[sel ? SchemeSel : SchemeNorm][ColBorder];
			float        r  = (float) bc->r / 65535.0f;
			float        g  = (float) bc->g / 65535.0f;
			float        b  = (float) bc->b / 65535.0f;
			float        a  = (float) bc->a / 65535.0f;
			unsigned int bw = (unsigned int) cw->bw;
			unsigned int ow = (unsigned int) cw->w + 2 * bw;
			unsigned int oh = (unsigned int) cw->h + 2 * bw;

			glBindTexture(GL_TEXTURE_2D, 0);
			glUniform1i(egl.u_solid, 1);
			glUniform4f(egl.u_color, r, g, b, a);

			/* top */
			glUniform4f(egl.u_rect, (float) cw->x, (float) cw->y, (float) ow,
			    (float) bw);
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
			/* bottom */
			glUniform4f(egl.u_rect, (float) cw->x,
			    (float) (cw->y + (int) (oh - bw)), (float) ow, (float) bw);
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
			/* left */
			glUniform4f(egl.u_rect, (float) cw->x, (float) (cw->y + (int) bw),
			    (float) bw, (float) cw->h);
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
			/* right */
			glUniform4f(egl.u_rect, (float) (cw->x + (int) (ow - bw)),
			    (float) (cw->y + (int) bw), (float) bw, (float) cw->h);
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

			glUniform1i(egl.u_solid, 0);
			glBindTexture(GL_TEXTURE_2D, 0);
		}
	}

	glBindVertexArray(0);
	glUseProgram(0);

	if (use_scissor)
		glDisable(GL_SCISSOR_TEST);

	{
		/* Re-check paused immediately before the swap: if a fullscreen bypass
		 * raced in between the repaint start and here, the overlay window may
		 * already be lowered.  Only swap if we are not paused — leaving dirty
		 * state intact ensures the repaint loop restarts correctly once
		 * compositing resumes.
		 *
		 * NOTE: do NOT gate the swap on dirty_bbox_valid.  The bbox is only
		 * used for the partial-repaint scissor rect; it can be legitimately
		 * zero (e.g. after compositor_repaint_now() already called
		 * comp_dirty_clear()) even when the scene has been repainted and the
		 * new frame must be presented.  Gating the swap here would stall the
		 * vblank loop after any forced repaint until new damage arrives. */
		if (!comp.paused) {
			comp_dirty_clear();
			eglSwapBuffers(egl.egl_dpy, egl.egl_win);
		}
	}
}

/* -------------------------------------------------------------------------
 * Thumbnail capture — EGL/GL path.
 * Render cw->texture into an FBO, read back via glReadPixels.
 * ---------------------------------------------------------------------- */

static cairo_surface_t *
egl_capture_thumb(CompWin *cw, int max_w, int max_h)
{
	static const cairo_user_data_key_t pixels_key = { 0 };
	GLuint                             fbo = 0, color_tex = 0;
	int                                tw, th;
	double                             sx, sy, scale;
	uint8_t                           *pixels = NULL;
	cairo_surface_t                   *surf   = NULL;

	assert(cw != NULL);
	if (!cw->texture || cw->w <= 0 || cw->h <= 0)
		return NULL;

	/* Compute thumbnail size preserving aspect ratio */
	sx    = (double) max_w / (double) cw->w;
	sy    = (double) max_h / (double) cw->h;
	scale = sx < sy ? sx : sy;
	if (scale > 1.0)
		scale = 1.0;
	tw = (int) (cw->w * scale);
	th = (int) (cw->h * scale);
	if (tw < 1)
		tw = 1;
	if (th < 1)
		th = 1;

	/* Create FBO with a texture color attachment at thumb size */
	glGenTextures(1, &color_tex);
	glBindTexture(GL_TEXTURE_2D, color_tex);
	glTexImage2D(
	    GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glBindTexture(GL_TEXTURE_2D, 0);

	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(
	    GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_tex, 0);

	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		awm_warn("comp_capture_thumb: FBO incomplete");
		goto out;
	}

	/* Render the window texture into the FBO at thumb size */
	glViewport(0, 0, tw, th);
	glUseProgram(egl.prog);
	{
		float proj[16];
		make_proj(proj, tw, th);
		glUniformMatrix4fv(egl.u_proj, 1, GL_FALSE, proj);
	}
	glUniform4f(egl.u_rect, 0.0f, 0.0f, (float) tw, (float) th);
	glUniform4f(egl.u_tint, 1.0f, 1.0f, 1.0f, 1.0f);
	glUniform1i(egl.u_solid, 0);
	glUniform1i(egl.u_has_mask, 0);
	glUniform1i(egl.u_tex, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, cw->texture);
	glBindVertexArray(egl.vao);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glBindVertexArray(0);
	glBindTexture(GL_TEXTURE_2D, 0);
	glUseProgram(0);

	/* Read pixels back — GL origin is bottom-left, cairo is top-left.
	 * Read row by row in reverse so the image is not upside down. */
	pixels = malloc((size_t) (tw * th * 4));
	if (!pixels)
		goto out;

	glReadPixels(0, 0, tw, th, GL_BGRA, GL_UNSIGNED_BYTE, pixels);

	/* Flip rows vertically: GL bottom-left → cairo top-left */
	{
		uint8_t *row = malloc((size_t) (tw * 4));
		if (row) {
			for (int y = 0; y < th / 2; y++) {
				uint8_t *top = pixels + (size_t) (y * tw * 4);
				uint8_t *bot = pixels + (size_t) ((th - 1 - y) * tw * 4);
				memcpy(row, top, (size_t) (tw * 4));
				memcpy(top, bot, (size_t) (tw * 4));
				memcpy(bot, row, (size_t) (tw * 4));
			}
			free(row);
		}
	}

	/* Wrap in a cairo image surface (RGB24 — alpha channel ignored) */
	surf = cairo_image_surface_create_for_data(
	    pixels, CAIRO_FORMAT_RGB24, tw, th, tw * 4);
	if (!surf || cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
		if (surf) {
			cairo_surface_destroy(surf);
			surf = NULL;
		}
		free(pixels);
		pixels = NULL;
		goto out;
	}
	/* Transfer ownership of pixels to the surface via a destroy callback */
	cairo_surface_set_user_data(surf, &pixels_key, pixels, free);
	pixels = NULL; /* now owned by the surface */

out:
	/* Restore normal render target, viewport, and projection matrix */
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, g_plat.sw, g_plat.sh);
	{
		float proj[16];
		make_proj(proj, g_plat.sw, g_plat.sh);
		glUseProgram(egl.prog);
		glUniformMatrix4fv(egl.u_proj, 1, GL_FALSE, proj);
		glUseProgram(0);
	}
	if (fbo)
		glDeleteFramebuffers(1, &fbo);
	if (color_tex)
		glDeleteTextures(1, &color_tex);
	free(pixels); /* NULL-safe; only non-NULL if surface creation failed */
	return surf;
}

/* -------------------------------------------------------------------------
 * Backend vtable singleton
 * ---------------------------------------------------------------------- */

/* -------------------------------------------------------------------------
 * Backend vtable — notify_damage
 * ---------------------------------------------------------------------- */

static void
egl_notify_damage(CompWin *cw)
{
	/* Re-sync the GL texture from the existing EGLImage.  With
	 * EGL_KHR_image_pixmap the EGLImage is a live reference to the X
	 * pixmap, but on many Mesa/X.org drivers the GL texture does not
	 * automatically reflect pixmap updates — the driver requires a
	 * fresh glEGLImageTargetTexture2DOES call to pull in new pixel data.
	 * This is cheap (no X round-trip, no new EGLImage allocation). */
	if (!cw->texture || cw->egl_image == EGL_NO_IMAGE_KHR)
		return;
	glBindTexture(GL_TEXTURE_2D, cw->texture);
	egl.egl_image_target_tex(GL_TEXTURE_2D, (GLeglImageOES) cw->egl_image);
	glBindTexture(GL_TEXTURE_2D, 0);
}

const CompBackend comp_backend_egl = {
	.init              = egl_init,
	.cleanup           = egl_cleanup,
	.bind_pixmap       = egl_bind_pixmap,
	.release_pixmap    = egl_release_pixmap,
	.update_wallpaper  = egl_update_wallpaper,
	.release_wallpaper = egl_release_wallpaper,
	.repaint           = egl_repaint,
	.notify_resize     = egl_notify_resize,
	.capture_thumb     = egl_capture_thumb,
	.apply_shape = NULL, /* EGL handles ShapeNotify via comp_refresh_pixmap
	                        in compositor.c */
	.notify_damage = egl_notify_damage,
};

#endif /* COMPOSITOR */

/* Satisfy ISO C99: a translation unit must contain at least one declaration */
typedef int compositor_egl_translation_unit_nonempty;
