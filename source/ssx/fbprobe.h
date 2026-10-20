/*
 * SSX port: render pass probe
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef SSX_FBPROBE_H
#define SSX_FBPROBE_H

#include <vitaGL.h>

#ifdef DEBUG_SOLOADER
// Called by the glutil wrappers
void fbprobe_tex_image(GLenum target, GLint level, GLsizei w, GLsizei h);
void fbprobe_rb_storage(GLsizei w, GLsizei h);
void fbprobe_draw(void);
void fbprobe_clear(void);
// From gl_swap
void fbprobe_frame(void);

// The game's framebuffer calls (dynlib.c)
void glBindFramebuffer_fbprobe(GLenum target, GLuint fb);
void glFramebufferTexture2D_fbprobe(GLenum target, GLenum attachment, GLenum textarget, GLuint tex, GLint level);
void glFramebufferRenderbuffer_fbprobe(GLenum target, GLenum attachment, GLenum rbtarget, GLuint rb);
void glViewport_fbprobe(GLint x, GLint y, GLsizei w, GLsizei h);
void glCopyTexSubImage2D_fbprobe(GLenum target, GLint level, GLint xoff, GLint yoff, GLint x, GLint y,
                                 GLsizei w, GLsizei h);
#else
#define fbprobe_tex_image(target, level, w, h) ((void)0)
#define fbprobe_rb_storage(w, h) ((void)0)
#define fbprobe_draw() ((void)0)
#define fbprobe_clear() ((void)0)
#define fbprobe_frame() ((void)0)
#endif

#endif // SSX_FBPROBE_H
