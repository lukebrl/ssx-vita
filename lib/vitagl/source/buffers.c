/*
 * This file is part of vitaGL
 * Copyright 2017, 2018, 2019, 2020 Rinnegatamante
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation, version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/* 
 * buffers.c:
 * Implementation for buffers related functions
 */

#include "shared.h"

#define DISABLED_ATTRIBS_POOL_SIZE (256 * 1024) // Disabled attributes circular pool size in bytes for the default VAO
#define DISABLED_AUX_ATTRIBS_POOL_SIZE (64 * 1024) // Disabled attributes circular pool size in bytes for non default VAOs

uint32_t main_vertex_attrib_pool_size = DISABLED_ATTRIBS_POOL_SIZE;
uint32_t aux_vertex_attrib_pool_size = DISABLED_AUX_ATTRIBS_POOL_SIZE;
uint32_t vertex_array_unit = 0; // Current in-use vertex array buffer unit
uint32_t uniform_array_unit = 0; // Current in-use uniform buffer unit

void *vertex_object; // Vertex object address for vgl* draw pipeline
void *color_object; // Color object address for vgl* draw pipeline
void *texture_object; // Texture object address for vgl* draw pipeline
void *index_object; // Index object address for vgl* draw pipeline

static void ssx_ring_release(vbo *gpu_buf); // SSX port: see ssx_ring_update

static vao default_vao; // Vertex Array Object used when no vao is bound
vao *cur_vao = &default_vao; // Current in-use vertex array object
#if defined(HAVE_VERTEX_LAYOUT_CACHE) && !defined(STRICT_DRAW_COMPLIANCE) && !defined(DRAW_SPEEDHACK)
uint32_t vertex_layout_cur_ver = 0;
#endif

query *active_query = NULL; // Active query object
static query queries[MAX_QUERIES_NUM]; // Available query objects pool
uint32_t *queries_buffer = NULL; // Buffer used for visibility testing
SceGxmNotification query_fence; // Fence used for occlusion queries sync

void reset_vao(vao *v) {
#if defined(HAVE_VERTEX_LAYOUT_CACHE) && !defined(STRICT_DRAW_COMPLIANCE) && !defined(DRAW_SPEEDHACK)
	v->vertex_layout_version = ++vertex_layout_cur_ver;
#endif
	vgl_memset(v->vertex_attrib_offsets, 0, sizeof(uint32_t) * VERTEX_ATTRIBS_NUM);
	vgl_memset(v->vertex_attrib_vbo, 0, sizeof(uint32_t) * VERTEX_ATTRIBS_NUM);
	for (int i = 0; i < VERTEX_ATTRIBS_NUM; i++) {
		v->vertex_attrib_size[i] = 4;
		v->vertex_attrib_config[i].regIndex = i;
	}
	v->vertex_attrib_state = 0;
	v->vertex_attrib_divisor = 0;
	v->index_array_unit = 0;

	uint32_t circular_pool_size = v == &default_vao ? main_vertex_attrib_pool_size : aux_vertex_attrib_pool_size;
	v->vertex_attrib_pool = (float *)gpu_alloc_mapped_for_cpu(circular_pool_size);
	v->vertex_attrib_pool_ptr = v->vertex_attrib_pool;
	v->vertex_attrib_pool_limit = (float *)((uint8_t *)v->vertex_attrib_pool + circular_pool_size);

	// Init generic vertex attrib arrays
	vao *vao_bkp = cur_vao;
	cur_vao = v;
	for (int i = 0; i < VERTEX_ATTRIBS_NUM; i++) {
		v->vertex_attrib_value[i] = reserve_attrib_pool(4);
		v->vertex_attrib_config[i].componentCount = 4;
		v->vertex_attrib_config[i].offset = 0;
		v->vertex_attrib_config[i].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
		v->vertex_attrib_config[i].regIndex = i;
		v->vertex_attrib_config[i].streamIndex = i;
		v->vertex_stream_config[i].stride = 0;
		v->vertex_stream_config[i].indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
	}
	cur_vao = vao_bkp;
}

void reset_queries() {
	queries_buffer = gpu_alloc_mapped_for_cpu(MAX_QUERIES_NUM * 4 * sizeof(uint32_t));
	sceGxmSetVisibilityBuffer(gxm_context, queries_buffer, MAX_QUERIES_NUM * sizeof(uint32_t));
	query_fence.value = 0;
	query_fence.address = sceGxmGetNotificationRegion();
	*query_fence.address = 0;
	
	for (GLuint i = 0; i < MAX_QUERIES_NUM; i++) {
		queries[i].id = 0xFF;
		queries[i].sync = 0;
	}
}

/*
 * ------------------------------
 * - IMPLEMENTATION STARTS HERE -
 * ------------------------------
 */
 
void glGenQueries(GLsizei n, GLuint *ids) {
	THREAD_SAFE()

#ifndef SKIP_ERROR_HANDLING
	if (n < 0) {
		SET_GL_ERROR(GL_INVALID_VALUE)
	} else if (n == 0) {
		return;
	}
#endif
	for (GLuint i = 0; i < MAX_QUERIES_NUM; i++) {
		if (queries[i].id == 0xFF) {
			ids[n - 1] = (GLuint)&queries[i];
			queries[i].id = i;
			n--;
			if (n == 0) {
				break;
			}
		}
	}
}

void glDeleteQueries(GLsizei n, const GLuint *ids) {
	THREAD_SAFE()

#ifndef SKIP_ERROR_HANDLING
	if (n < 0) {
		SET_GL_ERROR(GL_INVALID_VALUE)
	}
#endif
	for (GLuint i = 0; i < n; i++) {
		query *q = (query*)ids[i];
		q->id = 0xFF;
	}
}

void glBeginQuery(GLenum target, GLuint id) {
	THREAD_SAFE()

	switch (target) {
	case GL_SAMPLES_PASSED:
		sceGxmSetFrontVisibilityTestOp(gxm_context, SCE_GXM_VISIBILITY_TEST_OP_INCREMENT);
		sceGxmSetBackVisibilityTestOp(gxm_context, SCE_GXM_VISIBILITY_TEST_OP_INCREMENT);
		break;
	case GL_ANY_SAMPLES_PASSED:
	case GL_ANY_SAMPLES_PASSED_CONSERVATIVE:
		sceGxmSetFrontVisibilityTestOp(gxm_context, SCE_GXM_VISIBILITY_TEST_OP_SET);
		sceGxmSetBackVisibilityTestOp(gxm_context, SCE_GXM_VISIBILITY_TEST_OP_SET);
		break;
	default:
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, target)
	}
	active_query = (query *)id;
	queries_buffer[active_query->id] = 0;
	queries_buffer[active_query->id + MAX_QUERIES_NUM] = 0;
	queries_buffer[active_query->id + MAX_QUERIES_NUM * 2] = 0;
	queries_buffer[active_query->id + MAX_QUERIES_NUM * 3] = 0;
	active_query->mode = target;
	sceGxmSetFrontVisibilityTestIndex(gxm_context, active_query->id);
	sceGxmSetBackVisibilityTestIndex(gxm_context, active_query->id);
	sceGxmSetFrontVisibilityTestEnable(gxm_context, SCE_GXM_VISIBILITY_TEST_ENABLED);
	sceGxmSetBackVisibilityTestEnable(gxm_context, SCE_GXM_VISIBILITY_TEST_ENABLED);
}

void glEndQuery(GLenum target) {
	THREAD_SAFE()

	sceGxmSetFrontVisibilityTestEnable(gxm_context, SCE_GXM_VISIBILITY_TEST_DISABLED);
	sceGxmSetBackVisibilityTestEnable(gxm_context, SCE_GXM_VISIBILITY_TEST_DISABLED);
	active_query->sync = query_fence.value + 1;
	active_query = NULL;
}

void glGetQueryObjectiv(GLuint id, GLenum pname, GLint *params) {
	THREAD_SAFE()

	query *q = (query *)id;

	switch (pname) {
	case GL_QUERY_RESULT:
		if (q->sync > *query_fence.address) {
			dirty_query = GL_TRUE;
			scene_reset();
			sceGxmNotificationWait(&query_fence);
		}
		*params = queries_buffer[q->id] + queries_buffer[q->id + MAX_QUERIES_NUM] + queries_buffer[q->id + MAX_QUERIES_NUM * 2] + queries_buffer[q->id + MAX_QUERIES_NUM * 3];
		if (q->mode != GL_SAMPLES_PASSED) {
			*params = *params > 0 ? GL_TRUE : GL_FALSE;
		}
		break;
	case GL_QUERY_RESULT_NO_WAIT:
		if (q->sync <= *query_fence.address) {
			*params = queries_buffer[q->id] + queries_buffer[q->id + MAX_QUERIES_NUM] + queries_buffer[q->id + MAX_QUERIES_NUM * 2] + queries_buffer[q->id + MAX_QUERIES_NUM * 3];
			if (q->mode != GL_SAMPLES_PASSED) {
				*params = *params > 0 ? GL_TRUE : GL_FALSE;
			}
		}
		break;
	case GL_QUERY_RESULT_AVAILABLE:
		*params = (q->sync <= *query_fence.address) ? GL_TRUE : GL_FALSE;
		break;
	case GL_QUERY_TARGET:
		*params = q->mode;
		break;
	default:
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, pname)
	}
}

void glGetQueryObjectuiv(GLuint id, GLenum pname, GLuint *params) {
	THREAD_SAFE()

	query *q = (query *)id;

	switch (pname) {
	case GL_QUERY_RESULT:
		if (q->sync > *query_fence.address) {
			dirty_query = GL_TRUE;
			scene_reset();
			sceGxmNotificationWait(&query_fence);
		}
		*params = queries_buffer[q->id] + queries_buffer[q->id + MAX_QUERIES_NUM] + queries_buffer[q->id + MAX_QUERIES_NUM * 2] + queries_buffer[q->id + MAX_QUERIES_NUM * 3];
		if (q->mode != GL_SAMPLES_PASSED) {
			*params = *params > 0 ? GL_TRUE : GL_FALSE;
		}
		break;
	case GL_QUERY_RESULT_NO_WAIT:
		if (q->sync <= *query_fence.address) {
			*params = queries_buffer[q->id] + queries_buffer[q->id + MAX_QUERIES_NUM] + queries_buffer[q->id + MAX_QUERIES_NUM * 2] + queries_buffer[q->id + MAX_QUERIES_NUM * 3];
			if (q->mode != GL_SAMPLES_PASSED) {
				*params = *params > 0 ? GL_TRUE : GL_FALSE;
			}
		}
		break;
	case GL_QUERY_RESULT_AVAILABLE:
		*params = (q->sync <= *query_fence.address) ? GL_TRUE : GL_FALSE;
		break;
	case GL_QUERY_TARGET:
		*params = q->mode;
		break;
	default:
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, pname)
	}
}

void glGenVertexArrays(GLsizei n, GLuint *res) {
	THREAD_SAFE()

#ifndef SKIP_ERROR_HANDLING
	if (n < 0) {
		SET_GL_ERROR(GL_INVALID_VALUE)
	}
#endif
	for (int i = 0; i < n; i++) {
		res[i] = (GLuint)(vglMalloc(sizeof(vao)));
#ifdef LOG_ERRORS
		if (!res[i]) {
			vgl_log("%s:%d glGenVertexArrays failed to alloc a buffer (%d/%lu).\n", __FILE__, __LINE__, i, n);
		}
#endif
		reset_vao((vao *)res[i]);
	}
}

void glBindVertexArray(GLuint array) {
	THREAD_SAFE()

	cur_vao = (vao *)array;
	if (!cur_vao)
		cur_vao = &default_vao;
}

void glDeleteVertexArrays(GLsizei n, const GLuint *gl_arrays) {
	THREAD_SAFE()

#ifndef SKIP_ERROR_HANDLING
	if (n < 0) {
		SET_GL_ERROR(GL_INVALID_VALUE)
	}
#endif
	for (int j = 0; j < n; j++) {
		if (gl_arrays[j]) {
			vao *gpu_buf = (vao *)gl_arrays[j];
			mark_as_dirty(gpu_buf->vertex_attrib_pool);
			vgl_free(gpu_buf);
		}
	}
}

inline __attribute__((always_inline)) void glGenBuffers(GLsizei n, GLuint *res) {
	THREAD_SAFE()

#ifndef SKIP_ERROR_HANDLING
	if (n < 0) {
		SET_GL_ERROR(GL_INVALID_VALUE)
	}
#endif
	for (int i = 0; i < n; i++) {
		vbo *gpu_buf = (vbo *)vglMalloc(sizeof(vbo));
#ifdef LOG_ERRORS
		if (!gpu_buf)
			vgl_log("%s:%d glGenBuffers failed to alloc a buffer (%d/%lu).\n", __FILE__, __LINE__, i, n);
#endif
		gpu_buf->ptr = NULL;
		gpu_buf->last_frame = OBJ_NOT_USED;
		gpu_buf->ssx_ring = NULL;
		gpu_buf->mapped = GL_FALSE; // SSX port: was left as whatever the allocator returned
		res[i] = (GLuint)gpu_buf;
	}
}

void glCreateBuffers(GLsizei n, GLuint *buffers) {
	glGenBuffers(n, buffers);
}

void glBindBuffer(GLenum target, GLuint buffer) {
	THREAD_SAFE()

	switch (target) {
	case GL_ARRAY_BUFFER:
		vertex_array_unit = buffer;
		break;
	case GL_ELEMENT_ARRAY_BUFFER:
		cur_vao->index_array_unit = buffer;
		break;
	case GL_UNIFORM_BUFFER:
		uniform_array_unit = buffer;
		break;
	default:
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, target)
	}
}

/*
 * SSX port: deleted buffer objects stay around, empty, for a while.
 * The game deletes level geometry on the way back to the menus while the
 * current vertex array still points at some of it, and its next draw read
 * the freed vbo struct (a crash in sceGxmDraw). An emptied buffer (ptr NULL,
 * size 0) is safe: draws using it are skipped. After ZOMBIE_FRAMES frames
 * the struct is freed, once the vertex arrays no longer point at it.
 */
#define ZOMBIE_SLOTS 4096
#define ZOMBIE_FRAMES 8
static struct {
	vbo *buf;
	uint32_t frame;
} zombies[ZOMBIE_SLOTS];
static int num_zombies;

static void unreference_buffer(vao *v, uint32_t name) {
	if (v->index_array_unit == name)
		v->index_array_unit = 0;
	for (int i = 0; i < VERTEX_ATTRIBS_NUM; i++)
		if (v->vertex_attrib_vbo[i] == name)
			v->vertex_attrib_vbo[i] = 0;
}

static int buffer_referenced(vao *v, uint32_t name) {
	if (v->index_array_unit == name)
		return 1;
	for (int i = 0; i < VERTEX_ATTRIBS_NUM; i++)
		if (v->vertex_attrib_vbo[i] == name)
			return 1;
	return 0;
}

static void free_zombies(int all) {
	int kept = 0;
	for (int i = 0; i < num_zombies; i++) {
		vbo *b = zombies[i].buf;
		if (all || (vgl_framecount - zombies[i].frame > ZOMBIE_FRAMES &&
				!buffer_referenced(cur_vao, (uint32_t)b) && !buffer_referenced(&default_vao, (uint32_t)b))) {
			if (all) {
				unreference_buffer(cur_vao, (uint32_t)b);
				unreference_buffer(&default_vao, (uint32_t)b);
			}
			vgl_free(b);
		} else {
			zombies[kept++] = zombies[i];
		}
	}
	num_zombies = kept;
}

void glDeleteBuffers(GLsizei n, const GLuint *gl_buffers) {
	THREAD_SAFE()

#ifndef SKIP_ERROR_HANDLING
	if (n < 0) {
		SET_GL_ERROR(GL_INVALID_VALUE)
	}
#endif
	for (int j = 0; j < n; j++) {
		if (gl_buffers[j]) {
			vbo *gpu_buf = (vbo *)gl_buffers[j];
			ssx_ring_release(gpu_buf);
#if defined(HAVE_SCRATCH_MEMORY) && !defined(DISABLE_CIRCULAR_POOL)
			if (gpu_buf->ptr && !gpu_buf->scratch) {
#else
			if (gpu_buf->ptr) {
#endif
				if (gpu_buf->last_frame != OBJ_NOT_USED && (vgl_framecount - gpu_buf->last_frame <= FRAME_PURGE_FREQ)) {
					mark_as_dirty(gpu_buf->ptr);
				} else {
					vgl_free(gpu_buf->ptr);
				}
			}
			if (vertex_array_unit == gl_buffers[j])
				vertex_array_unit = 0;
			gpu_buf->ptr = NULL;
			gpu_buf->size = 0;
			gpu_buf->mapped = GL_FALSE;
			if (num_zombies == ZOMBIE_SLOTS)
				free_zombies(0);
			if (num_zombies == ZOMBIE_SLOTS)
				free_zombies(1);
			zombies[num_zombies].buf = gpu_buf;
			zombies[num_zombies].frame = vgl_framecount;
			num_zombies++;
		}
	}
	if (num_zombies > ZOMBIE_SLOTS / 2)
		free_zombies(0);
}

inline void glNamedBufferData(GLuint buffer, GLsizei size, const void *data, GLenum usage) {
	THREAD_SAFE()

	vbo *gpu_buf = (vbo *)buffer;
	
#ifndef SKIP_ERROR_HANDLING
	if (size < 0) {
		SET_GL_ERROR(GL_INVALID_VALUE)
	} else if (!gpu_buf) {
		SET_GL_ERROR(GL_INVALID_OPERATION)
	} else if (size == 0) {
		vgl_log("%s:%d glNamedBufferData called with size 0. Skipping...\n", __FILE__, __LINE__);
		return;
	}
#endif
	ssx_ring_release(gpu_buf);
#if defined(HAVE_SCRATCH_MEMORY) && !defined(DISABLE_CIRCULAR_POOL)
	GLboolean was_scratch = gpu_buf->scratch;
#endif
	switch (usage) {
	case GL_STREAM_DRAW:
	case GL_STREAM_COPY:
	case GL_STREAM_READ:
#if defined(HAVE_SCRATCH_MEMORY) && !defined(DISABLE_CIRCULAR_POOL)
		gpu_buf->scratch = vgl_stream_wants_scratch;
#endif
		gpu_buf->alloc_func = gpu_alloc_mapped_for_cpu;
		break;
	case GL_DYNAMIC_DRAW:
	case GL_DYNAMIC_READ:
	case GL_DYNAMIC_COPY:
#if defined(HAVE_SCRATCH_MEMORY) && !defined(DISABLE_CIRCULAR_POOL)
		gpu_buf->scratch = vgl_dynamic_wants_scratch;
#endif
		gpu_buf->alloc_func = gpu_alloc_mapped_for_cpu;
		break;
	default:
#if defined(HAVE_SCRATCH_MEMORY) && !defined(DISABLE_CIRCULAR_POOL)
		gpu_buf->scratch = GL_FALSE;
#endif
		gpu_buf->alloc_func = gpu_alloc_mapped_for_gpu;
		break;
	}

	// Marking previous content for deletion or deleting it straight if unused
#if defined(HAVE_SCRATCH_MEMORY) && !defined(DISABLE_CIRCULAR_POOL)
	if (gpu_buf->ptr && !was_scratch) {
#else
	if (gpu_buf->ptr) {
#endif
		if (gpu_buf->last_frame != OBJ_NOT_USED && (vgl_framecount - gpu_buf->last_frame <= FRAME_PURGE_FREQ)) {
			mark_as_dirty(gpu_buf->ptr);
		} else {
			vgl_free(gpu_buf->ptr);
		}
	}

	// Allocating a new buffer
#if defined(HAVE_SCRATCH_MEMORY) && !defined(DISABLE_CIRCULAR_POOL)
	if (gpu_buf->scratch)
		gpu_buf->ptr = vgl_reserve_data_pool(size);
	else
#endif
		gpu_buf->ptr = gpu_buf->alloc_func(size);	

	// SSX port: out of memory everywhere (VRAM, RAM, phycont, heap). Release
	// builds skip the error check below and copied the data to NULL (a crash
	// creating a stream for a race); the buffer is left empty instead
	if (!gpu_buf->ptr) {
		vgl_ssx_oom_count++;
		vgl_ssx_oom_bytes = size;
		vgl_log("%s:%d glNamedBufferData: out of GPU memory for %u bytes\n", __FILE__, __LINE__, (unsigned)size);
		gpu_buf->size = 0;
		gpu_buf->last_frame = OBJ_NOT_USED;
		return;
	}
#ifndef SKIP_ERROR_HANDLING
	if (!gpu_buf->ptr) {
		SET_GL_ERROR(GL_OUT_OF_MEMORY)
	}
#endif

	gpu_buf->size = size;
	gpu_buf->last_frame = OBJ_NOT_USED;

	if (data)
		vgl_fast_memcpy(gpu_buf->ptr, data, size);
}

void glBufferData(GLenum target, GLsizei size, const GLvoid *data, GLenum usage) {
	THREAD_SAFE()

	vbo *gpu_buf;
	switch (target) {
	case GL_ARRAY_BUFFER:
		gpu_buf = (vbo *)vertex_array_unit;
		break;
	case GL_ELEMENT_ARRAY_BUFFER:
		gpu_buf = (vbo *)cur_vao->index_array_unit;
		break;
	case GL_UNIFORM_BUFFER:
		gpu_buf = (vbo *)uniform_array_unit;
		break;
	default:
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, target)
	}
	
	glNamedBufferData((GLuint)gpu_buf, size, data, usage);
}

uint32_t vgl_ssx_buffer_copies, vgl_ssx_buffer_inplace; // SSX port: see glNamedBufferSubData
uint32_t vgl_ssx_oom_count, vgl_ssx_oom_bytes; // SSX port: see glNamedBufferData

// SSX port: rings of storages for big buffers updated often. Scaleform keeps
// its meshes in a RAM copy of a 1.7 MB vertex buffer and uploads all of it
// whenever a mesh is added, 1 to 2 times per frame in a race: each upload
// wrote 1.7 MB to uncached memory (7 ms), or, while the GPU could still read
// the buffer, took fresh storage and copied it whole. Instead the buffer gets
// up to SSX_RING_SLOTS storages, each with a bitmap of the 1 KB pages it
// lacks; an update marks the pages that change, and a storage the GPU is done
// with (its frames finished rendering, see ssx_slot_busy) gets just the pages
// it lacks and becomes the buffer's storage. Usually the current storage is
// done with: more are made only when not, and each holds a whole copy of the
// buffer (4 of Scaleform's two came to 12 MB, which ran memory out).
//
// Which pages change: vglSsxBufferSubDataPages is told by the caller (the
// port records where Scaleform writes its meshes) and copies them from the
// new content, which holds all of the buffer. Other big updates through
// glBufferSubData are compared with a copy of the content in cached RAM (the
// shadow), page by page.
#define SSX_RING_SLOTS 2 // whole copies of the buffer kept; see ssx_ring_commit
#define SSX_RING_PAGE_SHIFT 10
#define SSX_RING_MIN_SIZE (64 * 1024)
#define SSX_RING_VERIFY_CHUNK (16 * 1024)

typedef struct {
	uint8_t *shadow; // NULL for rings fed by vglSsxBufferSubDataPages
	uint8_t *slot[SSX_RING_SLOTS];
	uint32_t used[SSX_RING_SLOTS]; // last frame drawn from (OBJ_NOT_USED: never)
	int cur;
	uint32_t words; // bitmap words per slot
	uint32_t verify_pos;
	uint32_t missing[]; // SSX_RING_SLOTS bitmaps of pages each slot lacks
} ssx_ring;

uint32_t vgl_ssx_ring_updates, vgl_ssx_ring_bytes, vgl_ssx_ring_compared, vgl_ssx_ring_waits, vgl_ssx_rings_created;
uint32_t vgl_ssx_ring_mismatches;
uint32_t vgl_ssx_ring_extra_bytes; // storages made beyond the buffers' own

// A storage the GPU may still read: drawn from by a frame it hasn't finished
// (the display queue tells, vgl_ssx_frame_done), and in any case not older
// than the frames that can be queued
static inline GLboolean ssx_slot_busy(uint32_t used) {
	return used != OBJ_NOT_USED && vgl_framecount - used < gxm_display_buffer_count &&
		(int32_t)(used - __atomic_load_n(&vgl_ssx_frame_done, __ATOMIC_ACQUIRE)) > 0;
}

// Whether len bytes differ, comparing 32 bytes at a time (sceClibMemcmp took
// ~12 ms for 1.7 MB)
static GLboolean ssx_differ(const uint8_t *a, const uint8_t *b, uint32_t len) {
	if ((((uintptr_t)a | (uintptr_t)b) & 3) == 0) {
		const uint32_t *x = (const uint32_t *)a, *y = (const uint32_t *)b;
		uint32_t n = len >> 2;
		while (n >= 8) {
			if ((x[0] ^ y[0]) | (x[1] ^ y[1]) | (x[2] ^ y[2]) | (x[3] ^ y[3]) |
				(x[4] ^ y[4]) | (x[5] ^ y[5]) | (x[6] ^ y[6]) | (x[7] ^ y[7]))
				return GL_TRUE;
			x += 8;
			y += 8;
			n -= 8;
		}
		while (n--)
			if (*x++ != *y++)
				return GL_TRUE;
		a = (const uint8_t *)x;
		b = (const uint8_t *)y;
		len &= 3;
	}
	while (len--)
		if (*a++ != *b++)
			return GL_TRUE;
	return GL_FALSE;
}

// Back to a plain buffer (the current storage), before anything else changes it
static void ssx_ring_release(vbo *gpu_buf) {
	ssx_ring *r = (ssx_ring *)gpu_buf->ssx_ring;
	if (!r)
		return;
	if (gpu_buf->last_frame == OBJ_NOT_USED)
		gpu_buf->last_frame = r->used[r->cur];
	for (int i = 0; i < SSX_RING_SLOTS; i++) {
		if (i == r->cur || !r->slot[i])
			continue;
		if (r->used[i] != OBJ_NOT_USED && vgl_framecount - r->used[i] <= FRAME_PURGE_FREQ)
			mark_as_dirty(r->slot[i]);
		else
			vgl_free(r->slot[i]);
		vgl_ssx_ring_extra_bytes -= gpu_buf->size;
	}
	if (r->shadow)
		vglFree(r->shadow);
	vglFree(r);
	gpu_buf->ssx_ring = NULL;
}

// A new ring around the buffer's storage; its first slot lacks everything
// (refreshed whole once) unless the caller says otherwise
static ssx_ring *ssx_ring_create(vbo *gpu_buf, GLboolean with_shadow) {
	uint32_t total = gpu_buf->size;
	uint32_t words = (((total + (1u << SSX_RING_PAGE_SHIFT) - 1) >> SSX_RING_PAGE_SHIFT) + 31) >> 5;
	ssx_ring *r = (ssx_ring *)vglCalloc(1, sizeof(ssx_ring) + SSX_RING_SLOTS * words * sizeof(uint32_t));
	if (!r)
		return NULL;
	if (with_shadow) {
		r->shadow = (uint8_t *)vglMalloc(total);
		if (!r->shadow) {
			vglFree(r);
			return NULL;
		}
	}
	r->slot[0] = gpu_buf->ptr;
	for (int i = 0; i < SSX_RING_SLOTS; i++)
		r->used[i] = OBJ_NOT_USED;
	r->used[0] = gpu_buf->last_frame;
	r->words = words;
	vgl_memset(r->missing, 0xFF, words * sizeof(uint32_t));
	gpu_buf->ssx_ring = r;
	vgl_ssx_rings_created++;
	return r;
}

static inline void ssx_ring_mark(ssx_ring *r, uint32_t page) {
	for (int i = 0; i < SSX_RING_SLOTS; i++)
		r->missing[i * r->words + (page >> 5)] |= 1u << (page & 31);
}

static uint32_t ssx_ring_copy_missing(ssx_ring *r, int s, const uint8_t *src, uint32_t size) {
	uint32_t *missing = r->missing + s * r->words;
	uint32_t pages = r->words << 5, copied = 0;
	for (uint32_t p = 0; p < pages;) {
		uint32_t bits = missing[p >> 5] >> (p & 31);
		if (!bits) {
			p = (p | 31) + 1;
			continue;
		}
		p += __builtin_ctz(bits);
		// One run of consecutive missing pages: [p, q)
		uint32_t q = p + 1;
		while (q < pages && ((missing[q >> 5] >> (q & 31)) & 1))
			q++;
		uint32_t start = p << SSX_RING_PAGE_SHIFT, end = q << SSX_RING_PAGE_SHIFT;
		if (end > size)
			end = size;
		if (start < end) {
			vgl_memcpy(r->slot[s] + start, src + start, end - start);
			copied += end - start;
		}
		p = q;
	}
	vgl_memset(missing, 0, r->words * sizeof(uint32_t));
	return copied;
}

// Brings a storage the GPU is done with up to date from src (all of the
// content) and makes it the buffer's storage
static void ssx_ring_commit(vbo *gpu_buf, ssx_ring *r, const uint8_t *src) {
	uint32_t total = gpu_buf->size;
	// The current storage if the GPU is done with it, else the one drawn from
	// longest ago among those it is done with, else a new one, else wait
	int s = -1;
	if (!ssx_slot_busy(r->used[r->cur])) {
		s = r->cur;
	} else {
		for (int i = 0; i < SSX_RING_SLOTS; i++) {
			if (r->slot[i] && !ssx_slot_busy(r->used[i]) &&
				(s < 0 || r->used[i] == OBJ_NOT_USED || (r->used[s] != OBJ_NOT_USED && r->used[i] < r->used[s])))
				s = i;
		}
	}
	if (s < 0) {
		for (int i = 0; i < SSX_RING_SLOTS; i++) {
			if (!r->slot[i]) {
				r->slot[i] = gpu_alloc_mapped_nowait(gpu_buf->alloc_func == gpu_alloc_mapped_for_gpu, total);
				if (r->slot[i]) {
					vgl_memset(r->missing + i * r->words, 0xFF, r->words * sizeof(uint32_t));
					r->used[i] = OBJ_NOT_USED;
					vgl_ssx_ring_extra_bytes += total;
					s = i;
				}
				break;
			}
		}
	}
	if (s < 0) {
		// All storages in use (several updates in one frame): replace the
		// other one with fresh storage, the old one freed once its frames are
		// done, as vitaGL's copy-on-write does, rather than keeping more
		// whole copies around (4 per buffer ran memory out)
		int i = r->cur == 0 ? 1 : 0;
		uint8_t *fresh = gpu_alloc_mapped_nowait(gpu_buf->alloc_func == gpu_alloc_mapped_for_gpu, total);
		if (fresh && r->slot[i]) {
			mark_as_dirty(r->slot[i]);
			r->slot[i] = fresh;
			vgl_memset(r->missing + i * r->words, 0xFF, r->words * sizeof(uint32_t));
			r->used[i] = OBJ_NOT_USED;
			s = i;
		} else if (fresh) {
			vgl_free(fresh);
		}
	}
	if (s < 0) {
		// Every storage may still be read: let the GPU finish
		glFinish();
		sceGxmFinish(gxm_context);
		vgl_ssx_ring_waits++;
		s = r->cur;
	}

	vgl_ssx_ring_bytes += ssx_ring_copy_missing(r, s, src, total);
	vgl_ssx_ring_updates++;
	if (s != r->cur) {
		r->cur = s;
		gpu_buf->ptr = r->slot[s];
	}
	gpu_buf->last_frame = OBJ_NOT_USED;
}

// glNamedBufferSubData through the ring; GL_FALSE if the buffer can't have one
static GLboolean ssx_ring_update(vbo *gpu_buf, uint32_t offset, uint32_t size, const uint8_t *data) {
	ssx_ring *r = (ssx_ring *)gpu_buf->ssx_ring;
	if (r && !r->shadow) {
		ssx_ring_release(gpu_buf); // fed by vglSsxBufferSubDataPages until now
		r = NULL;
	}
	if (!r) {
		r = ssx_ring_create(gpu_buf, GL_TRUE);
		if (!r)
			return GL_FALSE;
		// The shadow starts as the current storage's content (read once from
		// uncached memory, unless all of it is replaced): it lacks only the
		// pages that change
		if (offset != 0 || size != gpu_buf->size) {
			vgl_fast_memcpy(r->shadow, gpu_buf->ptr, gpu_buf->size);
			vgl_memset(r->missing, 0, r->words * sizeof(uint32_t));
		} else {
			vgl_fast_memcpy(r->shadow, data, gpu_buf->size);
		}
	} else if (gpu_buf->last_frame != OBJ_NOT_USED) {
		r->used[r->cur] = gpu_buf->last_frame;
	}

	// Pages whose content changes
	uint32_t page = 1u << SSX_RING_PAGE_SHIFT;
	for (uint32_t p = offset >> SSX_RING_PAGE_SHIFT; (p << SSX_RING_PAGE_SHIFT) < offset + size; p++) {
		uint32_t start = p << SSX_RING_PAGE_SHIFT, end = start + page;
		if (start < offset)
			start = offset;
		if (end > offset + size)
			end = offset + size;
		vgl_ssx_ring_compared += end - start;
		if (ssx_differ(r->shadow + start, data + (start - offset), end - start)) {
			vgl_fast_memcpy(r->shadow + start, data + (start - offset), end - start);
			ssx_ring_mark(r, p);
		}
	}
	ssx_ring_commit(gpu_buf, r, r->shadow);
	return GL_TRUE;
}

void vglSsxBufferSubDataPages(GLenum target, GLsizeiptr size, const void *data, const uint32_t *dirty, uint32_t page_shift) {
	vbo *gpu_buf;
	switch (target) {
	case GL_ARRAY_BUFFER:
		gpu_buf = (vbo *)vertex_array_unit;
		break;
	case GL_ELEMENT_ARRAY_BUFFER:
		gpu_buf = (vbo *)cur_vao->index_array_unit;
		break;
	default:
		glBufferSubData(target, 0, size, data);
		return;
	}
	if (!gpu_buf || !gpu_buf->ptr || size != gpu_buf->size || page_shift != SSX_RING_PAGE_SHIFT || gpu_buf->mapped
#if defined(HAVE_SCRATCH_MEMORY) && !defined(DISABLE_CIRCULAR_POOL)
		|| gpu_buf->scratch
#endif
		) {
		glBufferSubData(target, 0, size, data);
		return;
	}
	ssx_ring *r = (ssx_ring *)gpu_buf->ssx_ring;
	if (r && r->shadow) {
		ssx_ring_release(gpu_buf); // was updated through glBufferSubData
		r = NULL;
	}
	if (!r) {
		r = ssx_ring_create(gpu_buf, GL_FALSE);
		if (!r) {
			glBufferSubData(target, 0, size, data);
			return;
		}
	} else if (gpu_buf->last_frame != OBJ_NOT_USED) {
		r->used[r->cur] = gpu_buf->last_frame;
	}

	for (uint32_t w = 0; w < r->words; w++) {
		uint32_t bits = dirty[w];
		if (bits)
			for (int i = 0; i < SSX_RING_SLOTS; i++)
				r->missing[i * r->words + w] |= bits;
		vgl_ssx_ring_compared += __builtin_popcount(bits) << SSX_RING_PAGE_SHIFT;
	}
	ssx_ring_commit(gpu_buf, r, (const uint8_t *)data);

	// Check a piece of the storage against the content each time: a change
	// the caller didn't report would show here (and is repaired)
	uint32_t at = r->verify_pos, total = gpu_buf->size;
	uint32_t len = total - at < SSX_RING_VERIFY_CHUNK ? total - at : SSX_RING_VERIFY_CHUNK;
	if (ssx_differ((const uint8_t *)gpu_buf->ptr + at, (const uint8_t *)data + at, len)) {
		if (vgl_ssx_ring_mismatches++ == 0)
			vgl_log("vglSsxBufferSubDataPages: unreported change near offset %u of a %u byte buffer\n", at, total);
		vgl_memcpy(gpu_buf->ptr, data, total);
		for (int i = 0; i < SSX_RING_SLOTS; i++)
			if (r->slot[i] != gpu_buf->ptr)
				vgl_memset(r->missing + i * r->words, 0xFF, r->words * sizeof(uint32_t));
	}
	r->verify_pos = at + len >= total ? 0 : at + len;
}

inline void glNamedBufferSubData(GLuint buffer, GLintptr offset, GLsizeiptr size, const void *data) {
	THREAD_SAFE()

	vbo *gpu_buf = (vbo *)buffer;

#ifndef SKIP_ERROR_HANDLING
	if (!gpu_buf) {
		SET_GL_ERROR(GL_INVALID_OPERATION)
	} else if (offset < 0 || offset + size > gpu_buf->size) {
		SET_GL_ERROR(GL_INVALID_VALUE)
	} else if (size == 0) {
		vgl_log("%s:%d glNamedBufferSubData called with size 0. Skipping...\n", __FILE__, __LINE__);
		return;
	}
#endif

	// SSX port: big buffers given big updates, or updated while in use, go
	// through a ring, see ssx_ring_update
	if (gpu_buf->ssx_ring || (gpu_buf->size >= SSX_RING_MIN_SIZE && !gpu_buf->mapped &&
		(size >= SSX_RING_MIN_SIZE || (gpu_buf->last_frame != OBJ_NOT_USED && vgl_framecount - gpu_buf->last_frame <= FRAME_PURGE_FREQ))
#if defined(HAVE_SCRATCH_MEMORY) && !defined(DISABLE_CIRCULAR_POOL)
		&& !gpu_buf->scratch
#endif
		)) {
		if (ssx_ring_update(gpu_buf, offset, size, data))
			return;
		ssx_ring_release(gpu_buf);
	}
#ifndef BUFFERS_SPEEDHACK
	// Allocating a new buffer
	if (gpu_buf->last_frame != OBJ_NOT_USED && (vgl_framecount - gpu_buf->last_frame <= FRAME_PURGE_FREQ)) {
		// SSX port: take the new copy only if memory is free right away. With
		// memory full, the normal allocation ran the garbage collector and slept
		// 20 ms, up to 4 times (spikes of tens of ms per update of the game's
		// 1.7 MB dynamic vertex buffer); the update is written in place then,
		// at worst showing in a frame the GPU still had queued.
#if defined(HAVE_SCRATCH_MEMORY) && !defined(DISABLE_CIRCULAR_POOL)
		uint8_t *ptr = gpu_buf->scratch ? vgl_reserve_data_pool(gpu_buf->size) :
			gpu_alloc_mapped_nowait(gpu_buf->alloc_func == gpu_alloc_mapped_for_gpu, gpu_buf->size);
#else
		uint8_t *ptr = gpu_alloc_mapped_nowait(gpu_buf->alloc_func == gpu_alloc_mapped_for_gpu, gpu_buf->size);
#endif
		if (!ptr) {
			vgl_ssx_buffer_inplace++;
			vgl_memcpy((uint8_t *)gpu_buf->ptr + offset, data, size);
			return;
		}
		vgl_ssx_buffer_copies++;

#ifdef LOG_ERRORS
		if (!ptr) {
			vgl_log("%s:%d glBufferSubData failed to alloc a buffer of %ld bytes. Buffer content won't be updated.\n", __FILE__, __LINE__, gpu_buf->size);
			return;
		}
#endif

		// Copying up previous data combined to modified data
		if (offset > 0)
			vgl_memcpy(ptr, gpu_buf->ptr, offset);
		vgl_memcpy(ptr + offset, data, size);
		if (gpu_buf->size - size - offset > 0)
			vgl_memcpy(ptr + offset + size, (uint8_t *)gpu_buf->ptr + offset + size, gpu_buf->size - size - offset);

		// Marking previous content for deletion
#if defined(HAVE_SCRATCH_MEMORY) && !defined(DISABLE_CIRCULAR_POOL)
		if (!gpu_buf->scratch)
#endif
			mark_as_dirty(gpu_buf->ptr);

		gpu_buf->ptr = ptr;
		gpu_buf->last_frame = OBJ_NOT_USED;
	} else
#endif
	{
		vgl_memcpy((uint8_t *)gpu_buf->ptr + offset, data, size);
	}
}

void glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data) {
	THREAD_SAFE()

	vbo *gpu_buf;
	switch (target) {
	case GL_ARRAY_BUFFER:
		gpu_buf = (vbo *)vertex_array_unit;
		break;
	case GL_ELEMENT_ARRAY_BUFFER:
		gpu_buf = (vbo *)cur_vao->index_array_unit;
		break;
	case GL_UNIFORM_BUFFER:
		gpu_buf = (vbo *)uniform_array_unit;
		break;
	default:
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, target)
	}
	
	glNamedBufferSubData((GLuint)gpu_buf, offset, size, data);
}

// SSX port: mapping a buffer the GPU may still read (drawn from in the last
// FRAME_PURGE_FREQ frames) gives it new storage holding a copy of its data,
// like glNamedBufferSubData does; when no memory is free right away, waits
// for the GPU instead. vitaGL used to hand out the storage in use: Scaleform
// writes its text meshes through glMapBufferOES, and frames the GPU had not
// drawn yet showed other text, or vertices flying across the screen.
uint32_t vgl_ssx_map_copies, vgl_ssx_map_waits, vgl_ssx_map_bytes;

static void ssx_map_prepare(vbo *gpu_buf, GLboolean preserve) {
	if (gpu_buf->last_frame == OBJ_NOT_USED || vgl_framecount - gpu_buf->last_frame > FRAME_PURGE_FREQ)
		return;
#if defined(HAVE_SCRATCH_MEMORY) && !defined(DISABLE_CIRCULAR_POOL)
	uint8_t *ptr = gpu_buf->scratch ? vgl_reserve_data_pool(gpu_buf->size) :
		gpu_alloc_mapped_nowait(gpu_buf->alloc_func == gpu_alloc_mapped_for_gpu, gpu_buf->size);
#else
	uint8_t *ptr = gpu_alloc_mapped_nowait(gpu_buf->alloc_func == gpu_alloc_mapped_for_gpu, gpu_buf->size);
#endif
	if (!ptr) {
		// No memory free right away: let the GPU finish with the storage
		glFinish();
		sceGxmFinish(gxm_context);
		vgl_ssx_map_waits++;
		return;
	}
	if (preserve)
		vgl_memcpy(ptr, gpu_buf->ptr, gpu_buf->size);
#if defined(HAVE_SCRATCH_MEMORY) && !defined(DISABLE_CIRCULAR_POOL)
	if (!gpu_buf->scratch)
#endif
		mark_as_dirty(gpu_buf->ptr);
	gpu_buf->ptr = ptr;
	gpu_buf->last_frame = OBJ_NOT_USED;
	vgl_ssx_map_copies++;
	vgl_ssx_map_bytes += gpu_buf->size;
}

inline void *glMapNamedBuffer(GLuint buffer, GLenum access) {
	vbo *gpu_buf = (vbo *)buffer;
	
#ifndef SKIP_ERROR_HANDLING
	switch (access) {
	case GL_READ_WRITE:
	case GL_READ_ONLY:
	case GL_WRITE_ONLY:
		break;
	default:
		SET_GL_ERROR_WITH_RET(GL_INVALID_ENUM, NULL);
	}

	if (!gpu_buf) {
		SET_GL_ERROR_WITH_RET(GL_INVALID_OPERATION, NULL)
	}
#endif

	ssx_ring_release(gpu_buf);
	if (access != GL_READ_ONLY)
		ssx_map_prepare(gpu_buf, GL_TRUE);
	gpu_buf->mapped = GL_TRUE;
	return gpu_buf->ptr;
}

// SSX port: the data of a buffer object, without binding it or making a GL call
// (safe from any thread). Buffer names are vbo pointers.
const void *vglGetBufferData(GLuint buffer, GLsizei *size) {
	vbo *gpu_buf = (vbo *)buffer;
	if (!gpu_buf) {
		if (size)
			*size = 0;
		return NULL;
	}
	if (size)
		*size = gpu_buf->size;
	return gpu_buf->ptr;
}

void *glMapBuffer(GLenum target, GLenum access) {
	vbo *gpu_buf;
	switch (target) {
	case GL_ARRAY_BUFFER:
		gpu_buf = (vbo *)vertex_array_unit;
		break;
	case GL_ELEMENT_ARRAY_BUFFER:
		gpu_buf = (vbo *)cur_vao->index_array_unit;
		break;
	case GL_UNIFORM_BUFFER:
		gpu_buf = (vbo *)uniform_array_unit;
		break;
	default:
		SET_GL_ERROR_WITH_RET(GL_INVALID_ENUM, NULL)
	}

	return glMapNamedBuffer((GLuint)gpu_buf, access);
}

inline void *glMapNamedBufferRange(GLuint buffer, GLintptr offset, GLsizeiptr length, GLbitfield access) {
	vbo *gpu_buf = (vbo *)buffer;

#ifndef SKIP_ERROR_HANDLING
	if (!gpu_buf) {
		SET_GL_ERROR_WITH_RET(GL_INVALID_OPERATION, NULL)
	} else if (offset < 0 || offset + length > gpu_buf->size) {
		SET_GL_ERROR_WITH_RET(GL_INVALID_VALUE, NULL)
	}
#endif

	// Unsynchronized mappings are the caller's business; invalidating the
	// whole buffer means its old data needn't be copied
	ssx_ring_release(gpu_buf);
	if ((access & GL_MAP_WRITE_BIT) && !(access & GL_MAP_UNSYNCHRONIZED_BIT))
		ssx_map_prepare(gpu_buf, !(access & GL_MAP_INVALIDATE_BUFFER_BIT));
	gpu_buf->mapped = GL_TRUE;
	return (void *)((uint8_t *)gpu_buf->ptr + offset);
}

void *glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access) {
	vbo *gpu_buf;
	switch (target) {
	case GL_ARRAY_BUFFER:
		gpu_buf = (vbo *)vertex_array_unit;
		break;
	case GL_ELEMENT_ARRAY_BUFFER:
		gpu_buf = (vbo *)cur_vao->index_array_unit;
		break;
	case GL_UNIFORM_BUFFER:
		gpu_buf = (vbo *)uniform_array_unit;
		break;
	default:
		SET_GL_ERROR_WITH_RET(GL_INVALID_ENUM, NULL)
	}

	return glMapNamedBufferRange((GLuint)gpu_buf, offset, length, access);
}

inline GLboolean glUnmapNamedBuffer(GLuint buffer) {
	vbo *gpu_buf = (vbo *)buffer;

#ifndef SKIP_ERROR_HANDLING
	if (!gpu_buf || !gpu_buf->mapped) {
		SET_GL_ERROR_WITH_RET(GL_INVALID_OPERATION, GL_FALSE)
	}
#endif

	gpu_buf->last_frame = OBJ_NOT_USED;
	gpu_buf->mapped = GL_FALSE;
	return GL_TRUE;
}

GLboolean glUnmapBuffer(GLenum target) {
	vbo *gpu_buf;
	switch (target) {
	case GL_ARRAY_BUFFER:
		gpu_buf = (vbo *)vertex_array_unit;
		break;
	case GL_ELEMENT_ARRAY_BUFFER:
		gpu_buf = (vbo *)cur_vao->index_array_unit;
		break;
	case GL_UNIFORM_BUFFER:
		gpu_buf = (vbo *)uniform_array_unit;
		break;
	default:
		SET_GL_ERROR_WITH_RET(GL_INVALID_ENUM, GL_FALSE)
	}

	return glUnmapNamedBuffer((GLuint)gpu_buf);
}

void glFlushMappedNamedBufferRange(GLuint buffer, GLintptr offset, GLsizeiptr length) {
	THREAD_SAFE()

#ifndef SKIP_ERROR_HANDLING
	vbo *gpu_buf = (vbo *)buffer;

	if (!gpu_buf || !gpu_buf->mapped) {
		SET_GL_ERROR(GL_INVALID_OPERATION)
	} else if (offset < 0 || offset + length > gpu_buf->size) {
		SET_GL_ERROR(GL_INVALID_VALUE)
	}
#endif
}

void glFlushMappedBufferRange(GLenum target, GLintptr offset, GLsizeiptr length) {
	THREAD_SAFE()

#ifndef SKIP_ERROR_HANDLING
	vbo *gpu_buf;
	switch (target) {
	case GL_ARRAY_BUFFER:
		gpu_buf = (vbo *)vertex_array_unit;
		break;
	case GL_ELEMENT_ARRAY_BUFFER:
		gpu_buf = (vbo *)cur_vao->index_array_unit;
		break;
	case GL_UNIFORM_BUFFER:
		gpu_buf = (vbo *)uniform_array_unit;
		break;
	default:
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, target)
	}

	if (!gpu_buf || !gpu_buf->mapped) {
		SET_GL_ERROR(GL_INVALID_OPERATION)
	} else if (offset < 0 || offset + length > gpu_buf->size) {
		SET_GL_ERROR(GL_INVALID_VALUE)
	}
#endif
}

inline void glGetNamedBufferParameteriv(GLuint buffer, GLenum pname, GLint *params) {
	THREAD_SAFE()

	vbo *gpu_buf = (vbo *)buffer;
#ifndef SKIP_ERROR_HANDLING
	if (!gpu_buf) {
		SET_GL_ERROR(GL_INVALID_OPERATION)
	}
#endif

	switch (pname) {
	case GL_BUFFER_SIZE:
		*params = gpu_buf->size;
		break;
	default:
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, pname)
	}
}

void glGetBufferParameteriv(GLenum target, GLenum pname, GLint *params) {
	THREAD_SAFE()

	vbo *gpu_buf;
	switch (target) {
	case GL_ARRAY_BUFFER:
		gpu_buf = (vbo *)vertex_array_unit;
		break;
	case GL_ELEMENT_ARRAY_BUFFER:
		gpu_buf = (vbo *)cur_vao->index_array_unit;
		break;
	case GL_UNIFORM_BUFFER:
		gpu_buf = (vbo *)uniform_array_unit;
		break;
	default:
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, target)
	}
	
	glGetNamedBufferParameteriv((GLuint)gpu_buf, pname, params);
}

// VGL_EXT_gpu_objects_array extension implementation

void vglVertexPointer(GLint size, GLenum type, GLsizei stride, GLuint count, const GLvoid *pointer) {
	THREAD_SAFE()

#ifndef SKIP_ERROR_HANDLING
	if ((stride < 0) || (size < 2) || (size > 4)) {
		SET_GL_ERROR(GL_INVALID_VALUE)
	}
#endif
	SceGxmVertexAttribute *attributes = &ffp_vertex_attrib_config[FFP_ATTRIB_POSITION];
	SceGxmVertexStream *streams = &ffp_vertex_stream_config[FFP_ATTRIB_POSITION];

	unsigned short bpe;
	switch (type) {
	case GL_FLOAT:
		attributes->format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
		bpe = 4;
		break;
	case GL_SHORT:
		attributes->format = SCE_GXM_ATTRIBUTE_FORMAT_S16N;
		bpe = 2;
		break;
	default:
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, type)
	}

	attributes->componentCount = size;
	streams->stride = stride ? stride : bpe * size;

	vertex_object = gpu_alloc_mapped_temp(count * streams->stride);
	vgl_fast_memcpy(vertex_object, pointer, count * streams->stride);
}

void vglColorPointer(GLint size, GLenum type, GLsizei stride, GLuint count, const GLvoid *pointer) {
	THREAD_SAFE()

#ifndef SKIP_ERROR_HANDLING
	if ((stride < 0) || (size < 3) || (size > 4)) {
		SET_GL_ERROR(GL_INVALID_VALUE)
	}
#endif
	SceGxmVertexAttribute *attributes = &ffp_vertex_attrib_config[FFP_ATTRIB_COLOR];
	SceGxmVertexStream *streams = &ffp_vertex_stream_config[FFP_ATTRIB_COLOR];

	unsigned short bpe;
	switch (type) {
	case GL_FLOAT:
		attributes->format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
		bpe = 4;
		break;
	case GL_SHORT:
		attributes->format = SCE_GXM_ATTRIBUTE_FORMAT_S16N;
		bpe = 2;
		break;
	case GL_UNSIGNED_SHORT:
		attributes->format = SCE_GXM_ATTRIBUTE_FORMAT_U16N;
		bpe = 2;
		break;
	case GL_BYTE:
		attributes->format = SCE_GXM_ATTRIBUTE_FORMAT_S8N;
		bpe = 1;
		break;
	case GL_UNSIGNED_BYTE:
		attributes->format = SCE_GXM_ATTRIBUTE_FORMAT_U8N;
		bpe = 1;
		break;
	default:
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, type)
	}

	attributes->componentCount = size;
	streams->stride = stride ? stride : bpe * size;

	color_object = gpu_alloc_mapped_temp(count * streams->stride);
	vgl_fast_memcpy(color_object, pointer, count * streams->stride);
}

void vglTexCoordPointer(GLint size, GLenum type, GLsizei stride, GLuint count, const GLvoid *pointer) {
	THREAD_SAFE()

#ifndef SKIP_ERROR_HANDLING
	if ((stride < 0) || (size < 2) || (size > 4)) {
		SET_GL_ERROR(GL_INVALID_VALUE)
	}
#endif
	SceGxmVertexAttribute *attributes = &ffp_vertex_attrib_config[FFP_ATTRIB_TEX0];
	SceGxmVertexStream *streams = &ffp_vertex_stream_config[FFP_ATTRIB_TEX0];

	unsigned short bpe;
	switch (type) {
	case GL_FLOAT:
		attributes->format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
		bpe = 4;
		break;
	case GL_SHORT:
		attributes->format = SCE_GXM_ATTRIBUTE_FORMAT_S16N;
		bpe = 2;
		break;
	default:
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, type)
	}

	attributes->componentCount = size;
	streams->stride = stride ? stride : bpe * size;

	texture_object = gpu_alloc_mapped_temp(count * streams->stride);
	vgl_fast_memcpy(texture_object, pointer, count * streams->stride);
}

void vglIndexPointer(GLenum type, GLsizei stride, GLuint count, const GLvoid *pointer) {
	THREAD_SAFE()

#ifndef SKIP_ERROR_HANDLING
	if (stride < 0) {
		SET_GL_ERROR(GL_INVALID_VALUE)
	}
#endif
	int bpe;
	switch (type) {
	case GL_SHORT:
		bpe = sizeof(GLshort);
		break;
	default:
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, type)
	}
	index_object = gpu_alloc_mapped_temp(count * bpe);
	if (stride == 0)
		vgl_fast_memcpy(index_object, pointer, count * bpe);
	else {
		int i;
		uint8_t *dst = (uint8_t *)index_object;
		uint8_t *src = (uint8_t *)pointer;
		for (i = 0; i < count; i++) {
			vgl_fast_memcpy(dst, src, bpe);
			dst += bpe;
			src += stride;
		}
	}
}

void vglVertexPointerMapped(GLint size, const GLvoid *pointer) {
	THREAD_SAFE()

	SceGxmVertexAttribute *attributes = &ffp_vertex_attrib_config[FFP_ATTRIB_POSITION];
	SceGxmVertexStream *streams = &ffp_vertex_stream_config[FFP_ATTRIB_POSITION];

	attributes->format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
	attributes->componentCount = size;
	streams->stride = size * 4;

	vertex_object = (GLvoid *)pointer;
}

void vglColorPointerMapped(GLenum type, const GLvoid *pointer) {
	THREAD_SAFE()

	SceGxmVertexAttribute *attributes = &ffp_vertex_attrib_config[FFP_ATTRIB_COLOR];
	SceGxmVertexStream *streams = &ffp_vertex_stream_config[FFP_ATTRIB_COLOR];

	unsigned short bpe;
	switch (type) {
	case GL_FLOAT:
		attributes->format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
		bpe = 4;
		break;
	case GL_SHORT:
		attributes->format = SCE_GXM_ATTRIBUTE_FORMAT_S16N;
		bpe = 2;
		break;
	case GL_UNSIGNED_SHORT:
		attributes->format = SCE_GXM_ATTRIBUTE_FORMAT_U16N;
		bpe = 2;
		break;
	case GL_BYTE:
		attributes->format = SCE_GXM_ATTRIBUTE_FORMAT_S8N;
		bpe = 1;
		break;
	case GL_UNSIGNED_BYTE:
		attributes->format = SCE_GXM_ATTRIBUTE_FORMAT_U8N;
		bpe = 1;
		break;
	default:
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, type)
	}

	attributes->componentCount = 4;
	streams->stride = 4 * bpe;

	color_object = (GLvoid *)pointer;
}

void vglTexCoordPointerMapped(const GLvoid *pointer) {
	THREAD_SAFE()

	SceGxmVertexAttribute *attributes = &ffp_vertex_attrib_config[FFP_ATTRIB_TEX0];
	SceGxmVertexStream *streams = &ffp_vertex_stream_config[FFP_ATTRIB_TEX0];

	attributes->format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
	attributes->componentCount = 2;
	streams->stride = 8;

	texture_object = (GLvoid *)pointer;
}

void vglIndexPointerMapped(const GLvoid *pointer) {
	THREAD_SAFE()

	index_object = (GLvoid *)pointer;
}

void vglBufferData(GLenum target, const GLvoid *data) {
	THREAD_SAFE()

	vbo *gpu_buf;
	switch (target) {
	case GL_ARRAY_BUFFER:
		gpu_buf = (vbo *)vertex_array_unit;
		break;
	case GL_ELEMENT_ARRAY_BUFFER:
		gpu_buf = (vbo *)cur_vao->index_array_unit;
		break;
	case GL_UNIFORM_BUFFER:
		gpu_buf = (vbo *)uniform_array_unit;
		break;
	default:
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, target)
	}
#ifndef SKIP_ERROR_HANDLING
	if (!gpu_buf) {
		SET_GL_ERROR(GL_INVALID_OPERATION)
	}
#endif

	gpu_buf->ptr = (GLvoid *)data;
}

void vglSetVertexAttribPoolSize(uint32_t main_size, uint32_t aux_size) {
	main_vertex_attrib_pool_size = main_size;
	aux_vertex_attrib_pool_size = aux_size;
}
