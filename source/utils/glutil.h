/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021      Rinnegatamante
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  glutil.h
 * @brief OpenGL API initializer, related functions.
 */

#ifndef SOLOADER_GLUTIL_H
#define SOLOADER_GLUTIL_H

#include <vitaGL.h>

#ifdef __cplusplus
extern "C" {
#endif

void gl_init();

void gl_preload();

void gl_swap();

void glCompileShader_soloader(GLuint shader);
void glLinkProgram_soloader(GLuint program);
void glDrawArrays_soloader(GLenum mode, GLint first, GLsizei count);
void glDrawElements_soloader(GLenum mode, GLsizei count, GLenum type, const void *indices);
void glClear_soloader(GLbitfield mask);
void glFinish_soloader(void);
void glBufferData_soloader(GLenum target, GLsizeiptr size, const void *data, GLenum usage);
void glBufferSubData_soloader(GLenum target, GLintptr offset, GLsizeiptr size, const void *data);
// Logs glBufferSubData statistics since the last call
void glutil_subdata_report(uint32_t frames);
// glDelete* that defer to the GL thread when called elsewhere
void glDeleteTextures_soloader(GLsizei n, const GLuint *names);
void glDeleteBuffers_soloader(GLsizei n, const GLuint *names);
void glDeleteFramebuffers_soloader(GLsizei n, const GLuint *names);
void glDeleteRenderbuffers_soloader(GLsizei n, const GLuint *names);
// Logs how many deletes were deferred since the last call
void glutil_deferred_report(void);
// Debug: time per frame in the parts of gl_swap since the last call
void glutil_swap_parts_report(void);
// GPU memory by pool, and how much of it textures and game buffers take
void glutil_mem_report(void);
void glRenderbufferStorage_soloader(GLenum target, GLenum internalformat, GLsizei width, GLsizei height);
void glTexImage2D_soloader(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *data);
void glCompressedTexImage2D_soloader(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const void *data);
extern volatile unsigned gl_draw_calls, gl_clear_calls, gl_texsub_calls;
void glTexSubImage2D_soloader(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels);

void glShaderSource_soloader(GLuint shader, GLsizei count,
                             const GLchar **string, const GLint *_length);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_GLUTIL_H
