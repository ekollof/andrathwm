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

#include <string.h>
#include <stdint.h>

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <xcb/xcb.h>
#include <xcb/xfixes.h>

#include "awm.h"
#include "log.h"
#include "compositor_backend.h"

/* -------------------------------------------------------------------------
 * Private backend state
 * ---------------------------------------------------------------------- */

#define DAMAGE_RING_SIZE 6

static struct {
	xcb_connection_t *gl_xc; /* dedicated XCB connection for EGL/Mesa;
	                          * avoids Mesa's DRI3 XCB calls corrupting the
	                          * main xc sequence counter                    */
	EGLDisplay egl_dpy;
	EGLContext egl_ctx;
	EGLSurface egl_win;
	GLuint     prog;
	GLuint     vbo;
	GLuint     vao;
	/* uniform locations */
	GLint u_tex;
	GLint u_opacity;
	GLint u_solid;
	GLint u_color;
	GLint u_rect;
	GLint u_screen;
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

/* Vertex shader: maps pixel coordinates to NDC. */
static const char *vert_src = "#version 130\n"
                              "in vec2 a_pos;\n"
                              "in vec2 a_uv;\n"
                              "out vec2 v_uv;\n"
                              "uniform vec4 u_rect;\n"
                              "uniform vec2 u_screen;\n"
                              "void main() {\n"
                              "    vec2 px = u_rect.xy + a_pos * u_rect.zw;\n"
                              "    gl_Position = vec4(\n"
                              "        px.x / u_screen.x * 2.0 - 1.0,\n"
                              "        1.0 - px.y / u_screen.y * 2.0,\n"
                              "        0.0, 1.0);\n"
                              "    v_uv = a_uv;\n"
                              "}\n";

/* Fragment shader: samples the window texture with opacity, or fills solid. */
static const char *frag_src = "#version 130\n"
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
                              "        c.a *= u_opacity;\n"
                              "        frag_color = c;\n"
                              "    }\n"
                              "}\n";

/* -------------------------------------------------------------------------
 * GL helpers
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

	egl.u_tex     = glGetUniformLocation(egl.prog, "u_tex");
	egl.u_opacity = glGetUniformLocation(egl.prog, "u_opacity");
	egl.u_solid   = glGetUniformLocation(egl.prog, "u_solid");
	egl.u_color   = glGetUniformLocation(egl.prog, "u_color");
	egl.u_rect    = glGetUniformLocation(egl.prog, "u_rect");
	egl.u_screen  = glGetUniformLocation(egl.prog, "u_screen");

	glUseProgram(egl.prog);
	glUniform1i(egl.u_tex, 0);
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

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_SCISSOR_TEST);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glViewport(0, 0, sw, sh);

	egl.has_buffer_age =
	    (egl_exts && strstr(egl_exts, "EGL_EXT_buffer_age") != NULL);
	memset(egl.damage_ring, 0, sizeof(egl.damage_ring));
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
	glViewport(0, 0, sw, sh);
	/* Old damage ring entries are in the old coordinate space — invalidate
	 * them so the next frame does a full repaint. */
	memset(egl.damage_ring, 0, sizeof(egl.damage_ring));
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
				cur.width     = (unsigned short) sw;
				cur.height    = (unsigned short) sh;
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
			egl.damage_ring[egl.ring_idx].x      = 0;
			egl.damage_ring[egl.ring_idx].y      = 0;
			egl.damage_ring[egl.ring_idx].width  = (unsigned short) sw;
			egl.damage_ring[egl.ring_idx].height = (unsigned short) sh;
			egl.ring_idx = (egl.ring_idx + 1) % DAMAGE_RING_SIZE;
		}
	}

	if (use_scissor) {
		glEnable(GL_SCISSOR_TEST);
		glScissor(scissor.x, sh - scissor.y - scissor.height, scissor.width,
		    scissor.height);
	}

	glUseProgram(egl.prog);
	glUniform2f(egl.u_screen, (float) sw, (float) sh);
	glUniform1i(egl.u_tex, 0);

	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	if (egl.wallpaper_texture) {
		glBindTexture(GL_TEXTURE_2D, egl.wallpaper_texture);
		glUniform4f(egl.u_rect, 0.0f, 0.0f, (float) sw, (float) sh);
		glUniform1f(egl.u_opacity, 1.0f);
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
		glUniform1f(egl.u_opacity, (float) cw->opacity);
		glUniform1i(egl.u_solid, 0);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

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
		int had_dirty = comp.dirty_bbox_valid;
		comp_dirty_clear();

		/* Re-check paused immediately before the swap: if a fullscreen bypass
		 * raced in between the repaint start and here, the overlay window may
		 * already be lowered.  Skipping the swap is safe — dirty is cleared.
		 */
		if (!comp.paused && had_dirty)
			eglSwapBuffers(egl.egl_dpy, egl.egl_win);
	}
}

/* -------------------------------------------------------------------------
 * Backend vtable singleton
 * ---------------------------------------------------------------------- */

const CompBackend comp_backend_egl = {
	.init              = egl_init,
	.cleanup           = egl_cleanup,
	.bind_pixmap       = egl_bind_pixmap,
	.release_pixmap    = egl_release_pixmap,
	.update_wallpaper  = egl_update_wallpaper,
	.release_wallpaper = egl_release_wallpaper,
	.repaint           = egl_repaint,
	.notify_resize     = egl_notify_resize,
	.apply_shape = NULL, /* EGL handles ShapeNotify via comp_refresh_pixmap in
	                        compositor.c */
};

#endif /* COMPOSITOR */

/* Satisfy ISO C99: a translation unit must contain at least one declaration */
typedef int compositor_egl_translation_unit_nonempty;
