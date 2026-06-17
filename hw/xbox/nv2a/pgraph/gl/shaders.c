/*
 * Geforce NV2A PGRAPH OpenGL Renderer
 *
 * Copyright (c) 2015 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2020-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/fast-hash.h"
#include "qemu/mstring.h"

#include "xemu-version.h"
#include "ui/xemu-settings.h"
#include "ui/xemu-notifications.h"
#include "hw/xbox/nv2a/pgraph/util.h"
#include "hw/xbox/nv2a/pgraph/glsl/ubershader.h"
#include "debug.h"
#include "renderer.h"

static void init_uber_uniform_locs_for(GLuint p,
                                       struct UberPshLocs *psh,
                                       struct UberVshLocs *vsh,
                                       struct UberGeomLocs *geom);

static GLenum get_gl_primitive_mode(enum ShaderPolygonMode polygon_mode, enum ShaderPrimitiveMode primitive_mode)
{
    switch (primitive_mode) {
    case PRIM_TYPE_POINTS: return GL_POINTS;
    case PRIM_TYPE_LINES: return GL_LINES;
    case PRIM_TYPE_LINE_LOOP: return GL_LINE_LOOP;
    case PRIM_TYPE_LINE_STRIP: return GL_LINE_STRIP;
    case PRIM_TYPE_TRIANGLES: return GL_TRIANGLES;
    case PRIM_TYPE_TRIANGLE_STRIP: return GL_TRIANGLE_STRIP;
    case PRIM_TYPE_TRIANGLE_FAN: return GL_TRIANGLE_FAN;
    case PRIM_TYPE_QUADS: return GL_LINES_ADJACENCY;
    case PRIM_TYPE_QUAD_STRIP: return GL_LINE_STRIP_ADJACENCY;
    case PRIM_TYPE_POLYGON:
        if (polygon_mode == POLY_MODE_LINE) {
            return GL_LINE_LOOP;
        } else if (polygon_mode == POLY_MODE_FILL) {
            return GL_TRIANGLE_FAN;
        }

        assert(!"PRIM_TYPE_POLYGON with invalid polygon_mode");
        return 0;
    default:
        assert(!"Invalid primitive_mode");
        return 0;
    }
}

static GLuint create_gl_shader(GLenum gl_shader_type,
                               const char *code,
                               const char *name)
{
    GLint compiled = 0;

    NV2A_GL_DGROUP_BEGIN("Creating new %s", name);

    NV2A_DPRINTF("compile new %s, code:\n%s\n", name, code);

    GLuint shader = glCreateShader(gl_shader_type);
    glShaderSource(shader, 1, &code, 0);
    glCompileShader(shader);

    /* Check it compiled */
    compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        GLchar* log;
        GLint log_length;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);
        log = g_malloc(log_length * sizeof(GLchar));
        glGetShaderInfoLog(shader, log_length, NULL, log);
        fprintf(stderr, "%s\n\n" "nv2a: %s compilation failed: %s\n", code, name, log);
        g_free(log);

        NV2A_GL_DGROUP_END();
        abort();
    }

    NV2A_GL_DGROUP_END();

    return shader;
}

static void set_texture_sampler_uniforms(ShaderBinding *binding)
{
    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        char samplerName[16];
        snprintf(samplerName, sizeof(samplerName), "texSamp%d", i);
        GLint texSampLoc =
            glGetUniformLocation(binding->gl_program, samplerName);
        if (texSampLoc >= 0) {
            glUniform1i(texSampLoc, i);
        }
    }
}

static void update_shader_uniform_locs(ShaderBinding *binding)
{
    char tmp[64];

    for (int i = 0; i < ARRAY_SIZE(binding->uniform_locs.vsh); i++) {
        const char *name = VshUniformInfo[i].name;
        if (VshUniformInfo[i].count > 1) {
            snprintf(tmp, sizeof(tmp), "%s[0]", name);
            name = tmp;
        }
        binding->uniform_locs.vsh[i] = glGetUniformLocation(binding->gl_program, name);
    }

    for (int i = 0; i < ARRAY_SIZE(binding->uniform_locs.psh); i++) {
        const char *name = PshUniformInfo[i].name;
        if (PshUniformInfo[i].count > 1) {
            snprintf(tmp, sizeof(tmp), "%s[0]", name);
            name = tmp;
        }
        binding->uniform_locs.psh[i] = glGetUniformLocation(binding->gl_program, name);
    }
}

static void shader_module_cache_entry_init(Lru *lru, LruNode *node,
                                           const void *key)
{
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    memcpy(&module->key, key, sizeof(ShaderModuleCacheKey));

    const char *kind_str;
    MString *code;

    switch (module->key.kind) {
    case GL_VERTEX_SHADER:
        kind_str = "vertex shader";
        code = pgraph_glsl_gen_vsh(&module->key.vsh.state,
                                   module->key.vsh.glsl_opts);
        break;
    case GL_GEOMETRY_SHADER:
        kind_str = "geometry shader";
        code = pgraph_glsl_gen_geom(&module->key.geom.state,
                                    module->key.geom.glsl_opts);
        break;
    case GL_FRAGMENT_SHADER:
        kind_str = "fragment shader";
        code = pgraph_glsl_gen_psh(&module->key.psh.state,
                                   module->key.psh.glsl_opts);
        break;
    default:
        assert(!"Invalid shader module kind");
        kind_str = "unknown";
        code = NULL;
    }

    module->gl_shader =
        create_gl_shader(module->key.kind, mstring_get_str(code), kind_str);
    mstring_unref(code);
}

static void shader_module_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    glDeleteShader(module->gl_shader);
}

static bool shader_module_cache_entry_compare(Lru *lru, LruNode *node,
                                              const void *key)
{
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    return memcmp(&module->key, key, sizeof(ShaderModuleCacheKey));
}

static GLuint get_shader_module_for_key(PGRAPHGLState *r,
                                        const ShaderModuleCacheKey *key)
{
    uint64_t hash = fast_hash((void *)key, sizeof(ShaderModuleCacheKey));
    LruNode *node = lru_lookup(&r->shader_module_cache, hash, key);
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    return module->gl_shader;
}

static void generate_shaders(PGRAPHGLState *r, ShaderBinding *binding)
{
    GLuint program = glCreateProgram();

    ShaderState *state = &binding->state;
    ShaderModuleCacheKey key;

    bool need_geometry_shader = pgraph_glsl_need_geom(&state->geom);
    if (need_geometry_shader) {
        memset(&key, 0, sizeof(key));
        key.kind = GL_GEOMETRY_SHADER;
        key.geom.state = state->geom;
        glAttachShader(program, get_shader_module_for_key(r, &key));
    }

    /* create the vertex shader */
    memset(&key, 0, sizeof(key));
    key.kind = GL_VERTEX_SHADER;
    key.vsh.state = state->vsh;
    key.vsh.glsl_opts.prefix_outputs = need_geometry_shader;
    glAttachShader(program, get_shader_module_for_key(r, &key));

    /* generate a fragment shader from register combiners */
    memset(&key, 0, sizeof(key));
    key.kind = GL_FRAGMENT_SHADER;
    key.psh.state = state->psh;
    glAttachShader(program, get_shader_module_for_key(r, &key));

    /* link the program */
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if(!linked) {
        GLchar log[2048];
        glGetProgramInfoLog(program, 2048, NULL, log);
        fprintf(stderr, "nv2a: shader linking failed: %s\n", log);
        abort();
    }

    glUseProgram(program);

    binding->gl_program = program;
    binding->gl_primitive_mode = get_gl_primitive_mode(
        state->geom.polygon_front_mode, state->geom.primitive_mode);
    binding->initialized = true;

    set_texture_sampler_uniforms(binding);

    /* validate the program */
    GLint valid = 0;
    glValidateProgram(program);
    glGetProgramiv(program, GL_VALIDATE_STATUS, &valid);
    if (!valid) {
        GLchar log[1024];
        glGetProgramInfoLog(program, 1024, NULL, log);
        fprintf(stderr, "nv2a: shader validation failed: %s\n", log);
        abort();
    }

    update_shader_uniform_locs(binding);
}

static const char *shader_gl_vendor = NULL;

static void shader_create_cache_folder(void)
{
    char *shader_path = g_strdup_printf("%sshaders", xemu_settings_get_base_path());
    qemu_mkdir(shader_path);
    g_free(shader_path);
}

static char *uber_cache_get_path(const char *suffix)
{
    return g_strdup_printf("%s/ubershader_cache_%s.bin",
                           xemu_settings_get_base_path(), suffix);
}

static bool uber_cache_load_gl(GLuint programs[6])
{
    char *path = uber_cache_get_path("gl");
    FILE *f = qemu_fopen(path, "rb");
    g_free(path);
    if (!f) return false;

    bool ok = false;
    char *cached_version = NULL;
    char *cached_vendor = NULL;

    uint64_t ver_len, vendor_len;

    #define READ(dst, sz) if (fread(dst, sz, 1, f) != 1) goto done

    READ(&ver_len, sizeof(ver_len));
    cached_version = g_malloc(ver_len);
    READ(cached_version, ver_len);
    if (strcmp(cached_version, xemu_version) != 0) goto done;

    READ(&vendor_len, sizeof(vendor_len));
    cached_vendor = g_malloc(vendor_len);
    READ(cached_vendor, vendor_len);
    if (strcmp(cached_vendor, shader_gl_vendor) != 0) goto done;

    for (int i = 0; i < 6; i++) {
        GLenum bin_fmt;
        uint64_t bin_size;
        READ(&bin_fmt, sizeof(bin_fmt));
        READ(&bin_size, sizeof(bin_size));
        void *binary = g_malloc(bin_size);
        if (fread(binary, bin_size, 1, f) != 1) {
            g_free(binary);
            goto done;
        }

        GLuint prog = glCreateProgram();
        glProgramBinary(prog, bin_fmt, binary, (GLsizei)bin_size);
        g_free(binary);

        GLint linked = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &linked);
        if (!linked) {
            glDeleteProgram(prog);
            for (int j = 0; j < i; j++) {
                glDeleteProgram(programs[j]);
                programs[j] = 0;
            }
            goto done;
        }
        programs[i] = prog;
    }
    ok = true;

    #undef READ

done:
    if (!ok) {
        /* delete any programs created before the failure (the array is
         * zero-initialized by the caller, so untouched slots are skipped) */
        for (int j = 0; j < 6; j++) {
            if (programs[j]) {
                glDeleteProgram(programs[j]);
                programs[j] = 0;
            }
        }
    }
    fclose(f);
    g_free(cached_version);
    g_free(cached_vendor);
    return ok;
}

static void uber_cache_save_gl(GLuint programs[6])
{
    char *path = uber_cache_get_path("gl");
    FILE *f = qemu_fopen(path, "wb");
    g_free(path);
    if (!f) return;

    uint64_t ver_len = strlen(xemu_version) + 1;
    uint64_t vendor_len = strlen(shader_gl_vendor) + 1;

    #define WRITE(src, sz) if (fwrite(src, sz, 1, f) != 1) goto err

    WRITE(&ver_len, sizeof(ver_len));
    WRITE(xemu_version, ver_len);
    WRITE(&vendor_len, sizeof(vendor_len));
    WRITE(shader_gl_vendor, vendor_len);

    for (int i = 0; i < 6; i++) {
        GLint bin_len = 0;
        glGetProgramiv(programs[i], GL_PROGRAM_BINARY_LENGTH, &bin_len);
        if (bin_len == 0) goto err;

        void *binary = g_malloc(bin_len);
        GLenum bin_fmt;
        GLsizei copied;
        glGetProgramBinary(programs[i], bin_len, &copied, &bin_fmt, binary);
        if (glGetError() != GL_NO_ERROR || copied == 0) {
            g_free(binary);
            goto err;
        }

        uint64_t bin_size = (uint64_t)copied;
        bool ok = true;
        if (fwrite(&bin_fmt, sizeof(bin_fmt), 1, f) != 1) ok = false;
        if (ok && fwrite(&bin_size, sizeof(bin_size), 1, f) != 1) ok = false;
        if (ok && fwrite(binary, bin_size, 1, f) != 1) ok = false;
        g_free(binary);
        if (!ok) goto err;
    }

    #undef WRITE

    fclose(f);
    fprintf(stderr, "nv2a: OpenGL ubershader binary cached to disk\n");
    return;

err:
    fclose(f);
    char *err_path = uber_cache_get_path("gl");
    remove(err_path);
    g_free(err_path);
    fprintf(stderr, "nv2a: Failed to save OpenGL ubershader binary cache\n");
}

static char *shader_get_lru_cache_path(void)
{
    return g_strdup_printf("%s/shader_cache_list", xemu_settings_get_base_path());
}

static void shader_write_lru_list_entry_to_disk(Lru *lru, LruNode *node, void *opaque)
{
    FILE *lru_list_file = (FILE*) opaque;
    size_t written = fwrite(&node->hash, sizeof(uint64_t), 1, lru_list_file);
    if (written != 1) {
        fprintf(stderr, "nv2a: Failed to write shader list entry %llx to disk\n",
                (unsigned long long) node->hash);
    }
}

void pgraph_gl_shader_write_cache_reload_list(PGRAPHState *pg)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    if (r->uber_active || !g_config.perf.cache_shaders) {
        qatomic_set(&r->shader_cache_writeback_pending, false);
        qemu_event_set(&r->shader_cache_writeback_complete);
        return;
    }

    char *shader_lru_path = shader_get_lru_cache_path();
    qemu_thread_join(&r->shader_disk_thread);

    FILE *lru_list = qemu_fopen(shader_lru_path, "wb");
    g_free(shader_lru_path);
    if (!lru_list) {
        fprintf(stderr, "nv2a: Failed to open shader LRU cache for writing\n");
        return;
    }

    lru_visit_active(&r->shader_cache, shader_write_lru_list_entry_to_disk, lru_list);
    fclose(lru_list);

    lru_flush(&r->shader_cache);

    qatomic_set(&r->shader_cache_writeback_pending, false);
    qemu_event_set(&r->shader_cache_writeback_complete);
}

bool pgraph_gl_shader_load_from_memory(ShaderBinding *binding)
{
    assert(glGetError() == GL_NO_ERROR);

    if (!binding->program) {
        return false;
    }

    GLuint gl_program = glCreateProgram();
    glProgramBinary(gl_program, binding->program_format, binding->program,
                    binding->program_size);
    GLint gl_error = glGetError();
    if (gl_error != GL_NO_ERROR) {
        NV2A_DPRINTF(
            "failed to load shader binary from disk: GL error code %d\n",
            gl_error);
        glDeleteProgram(gl_program);
        return false;
    }

    GLint link_status = GL_FALSE;
    glGetProgramiv(gl_program, GL_LINK_STATUS, &link_status);
    if (!link_status) {
        NV2A_DPRINTF(
            "failed to load shader binary from disk: link status is FALSE\n");
        glDeleteProgram(gl_program);
        return false;
    }

    glUseProgram(gl_program);

    g_free(binding->program);

    binding->program = NULL;
    binding->gl_program = gl_program;
    binding->gl_primitive_mode =
        get_gl_primitive_mode(binding->state.geom.polygon_front_mode,
                              binding->state.geom.primitive_mode);
    binding->initialized = true;

    set_texture_sampler_uniforms(binding);

    glValidateProgram(gl_program);
    GLint valid = 0;
    glGetProgramiv(gl_program, GL_VALIDATE_STATUS, &valid);
    if (!valid) {
        GLchar log[1024];
        glGetProgramInfoLog(gl_program, 1024, NULL, log);
        NV2A_DPRINTF("failed to load shader binary from disk: %s\n", log);
        glDeleteProgram(gl_program);
        return false;
    }

    update_shader_uniform_locs(binding);

    return true;
}

static char *shader_get_bin_directory(uint64_t hash)
{
    const char *cfg_dir = xemu_settings_get_base_path();
    char *shader_bin_dir =
        g_strdup_printf("%s/shaders/%04x", cfg_dir, (uint32_t)(hash >> 48));
    return shader_bin_dir;
}

static char *shader_get_binary_path(const char *shader_bin_dir, uint64_t hash)
{
    uint64_t bin_mask = (uint64_t)0xffff << 48;
    return g_strdup_printf("%s/%012" PRIx64, shader_bin_dir, hash & ~bin_mask);
}

static void shader_load_from_disk(PGRAPHState *pg, uint64_t hash)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    char *shader_bin_dir = shader_get_bin_directory(hash);
    char *shader_path = shader_get_binary_path(shader_bin_dir, hash);
    char *cached_xemu_version = NULL;
    char *cached_gl_vendor = NULL;
    void *program_buffer = NULL;

    uint64_t cached_xemu_version_len;
    uint64_t gl_vendor_len;
    GLenum program_binary_format;
    ShaderState state;
    size_t shader_size;

    g_free(shader_bin_dir);

    qemu_mutex_lock(&r->shader_cache_lock);
    if (lru_contains_hash(&r->shader_cache, hash)) {
        qemu_mutex_unlock(&r->shader_cache_lock);
        return;
    }
    qemu_mutex_unlock(&r->shader_cache_lock);

    FILE *shader_file = qemu_fopen(shader_path, "rb");
    if (!shader_file) {
        goto error;
    }

    size_t nread;
    #define READ_OR_ERR(data, data_len) \
        do { \
            nread = fread(data, data_len, 1, shader_file); \
            if (nread != 1) { \
                fclose(shader_file); \
                goto error; \
            } \
        } while (0)

    READ_OR_ERR(&cached_xemu_version_len, sizeof(cached_xemu_version_len));

    cached_xemu_version = g_malloc(cached_xemu_version_len +1);
    READ_OR_ERR(cached_xemu_version, cached_xemu_version_len);
    if (strcmp(cached_xemu_version, xemu_version) != 0) {
        fclose(shader_file);
        goto error;
    }

    READ_OR_ERR(&gl_vendor_len, sizeof(gl_vendor_len));

    cached_gl_vendor = g_malloc(gl_vendor_len);
    READ_OR_ERR(cached_gl_vendor, gl_vendor_len);
    if (strcmp(cached_gl_vendor, shader_gl_vendor) != 0) {
        fclose(shader_file);
        goto error;
    }

    READ_OR_ERR(&program_binary_format, sizeof(program_binary_format));
    READ_OR_ERR(&state, sizeof(state));
    READ_OR_ERR(&shader_size, sizeof(shader_size));

    program_buffer = g_malloc(shader_size);
    READ_OR_ERR(program_buffer, shader_size);

    #undef READ_OR_ERR

    fclose(shader_file);
    g_free(shader_path);
    g_free(cached_xemu_version);
    g_free(cached_gl_vendor);

    qemu_mutex_lock(&r->shader_cache_lock);
    LruNode *node = lru_lookup(&r->shader_cache, hash, &state);
    ShaderBinding *binding = container_of(node, ShaderBinding, node);

    /* If we happened to regenerate this shader already, then we may as well use the new one */
    if (binding->initialized) {
        qemu_mutex_unlock(&r->shader_cache_lock);
        return;
    }

    binding->program_format = program_binary_format;
    binding->program_size = shader_size;
    binding->program = program_buffer;
    binding->cached = true;
    qemu_mutex_unlock(&r->shader_cache_lock);
    return;

error:
    /* Delete the shader so it won't be loaded again */
    qemu_unlink(shader_path);
    g_free(shader_path);
    g_free(program_buffer);
    g_free(cached_xemu_version);
    g_free(cached_gl_vendor);
}

static void *shader_reload_lru_from_disk(void *arg)
{
    if (!g_config.perf.cache_shaders) {
        return NULL;
    }

    PGRAPHState *pg = (PGRAPHState*) arg;
    char *shader_lru_path = shader_get_lru_cache_path();

    FILE *lru_shaders_list = qemu_fopen(shader_lru_path, "rb");
    g_free(shader_lru_path);
    if (!lru_shaders_list) {
        return NULL;
    }

    uint64_t hash;
    while (fread(&hash, sizeof(uint64_t), 1, lru_shaders_list) == 1) {
        shader_load_from_disk(pg, hash);
    }

    return NULL;
}

static void shader_cache_entry_init(Lru *lru, LruNode *node, const void *state)
{
    ShaderBinding *binding = container_of(node, ShaderBinding, node);
    memcpy(&binding->state, state, sizeof(ShaderState));
    binding->initialized = false;
    binding->cached = false;
    binding->program = NULL;
    binding->save_thread = NULL;
}

static void shader_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    ShaderBinding *binding = container_of(node, ShaderBinding, node);

    if (binding->save_thread) {
        qemu_thread_join(binding->save_thread);
        g_free(binding->save_thread);
    }

    glDeleteProgram(binding->gl_program);
    if (binding->program) {
        g_free(binding->program);
    }

    binding->cached = false;
    binding->save_thread = NULL;
    binding->program = NULL;
    memset(&binding->state, 0, sizeof(ShaderState));
}

static bool shader_cache_entry_compare(Lru *lru, LruNode *node, const void *key)
{
    ShaderBinding *binding = container_of(node, ShaderBinding, node);
    return memcmp(&binding->state, key, sizeof(ShaderState));
}

/* Build the optional GS-less smooth-triangle program from an already-compiled
 * uber vertex shader. Any compile or link failure leaves uber_program_tri_nogs
 * at 0 and the classic geometry-shader program serves instead, so this is
 * inlined with a non-aborting compile (create_gl_shader() abort()s on error,
 * which would hang a headless run on a modal dialog). */
static void build_uber_gl_tri_nogs(PGRAPHGLState *r, GLuint vs)
{
    r->uber_program_tri_nogs = 0;
    GenUberGlslOptions nogs_opts = { .vulkan = false, .gl_nogs_tri = true };
    MString *nogs_psh_code = pgraph_glsl_gen_uber_psh(nogs_opts);
    const char *nfs_src = mstring_get_str(nogs_psh_code);
    GLuint nfs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(nfs, 1, &nfs_src, NULL);
    glCompileShader(nfs);
    GLint ncompiled = 0;
    glGetShaderiv(nfs, GL_COMPILE_STATUS, &ncompiled);
    if (!ncompiled) {
        GLchar log[2048];
        glGetShaderInfoLog(nfs, 2048, NULL, log);
        fprintf(stderr, "nv2a: uber tri_nogs PSH compile failed "
                        "(classic GS serves): %s\n", log);
        glDeleteShader(nfs);
    } else {
        GLuint nprog = glCreateProgram();
        glAttachShader(nprog, vs);
        glAttachShader(nprog, nfs);
        glLinkProgram(nprog);
        GLint nlinked = 0;
        glGetProgramiv(nprog, GL_LINK_STATUS, &nlinked);
        if (nlinked) {
            r->uber_program_tri_nogs = nprog;
            fprintf(stderr, "nv2a: uber GS-bypass smooth-triangle program ready\n");
        } else {
            GLchar log[2048];
            glGetProgramInfoLog(nprog, 2048, NULL, log);
            fprintf(stderr, "nv2a: uber tri_nogs link failed "
                            "(classic GS serves): %s\n", log);
            glDeleteProgram(nprog);
        }
        glDeleteShader(nfs);
    }
    mstring_unref(nogs_psh_code);
}

void pgraph_gl_init_shaders(PGRAPHState *pg)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    qemu_mutex_init(&r->shader_cache_lock);
    qemu_event_init(&r->shader_cache_writeback_complete, false);

    if (!shader_gl_vendor) {
        shader_gl_vendor = (const char *) glGetString(GL_VENDOR);
    }

    shader_create_cache_folder();

    r->uber_active = g_config.perf.use_ubershaders;

    if (!r->uber_active) {
        const size_t shader_cache_size = 50*1024;
        lru_init(&r->shader_cache);
        r->shader_cache_entries = malloc(shader_cache_size * sizeof(ShaderBinding));
        assert(r->shader_cache_entries != NULL);
        for (int i = 0; i < shader_cache_size; i++) {
            lru_add_free(&r->shader_cache, &r->shader_cache_entries[i].node);
        }

        r->shader_cache.init_node = shader_cache_entry_init;
        r->shader_cache.compare_nodes = shader_cache_entry_compare;
        r->shader_cache.post_node_evict = shader_cache_entry_post_evict;

        qemu_thread_create(&r->shader_disk_thread,
                           "pgraph.renderer_state->shader_cache",
                           shader_reload_lru_from_disk, pg,
                           QEMU_THREAD_JOINABLE);

        const size_t shader_module_cache_size = 50*1024;
        lru_init(&r->shader_module_cache);
        r->shader_module_cache_entries =
            g_malloc_n(shader_module_cache_size, sizeof(ShaderModuleCacheEntry));
        assert(r->shader_module_cache_entries != NULL);
        for (int i = 0; i < shader_module_cache_size; i++) {
            lru_add_free(&r->shader_module_cache,
                         &r->shader_module_cache_entries[i].node);
        }

        r->shader_module_cache.init_node = shader_module_cache_entry_init;
        r->shader_module_cache.compare_nodes = shader_module_cache_entry_compare;
        r->shader_module_cache.post_node_evict = shader_module_cache_entry_post_evict;
    }
    if (r->uber_active) {
        GLuint *prog_ptrs[6] = {
            &r->uber_program, &r->uber_program_tri_wireframe,
            &r->uber_program_quad,
            &r->uber_program_quad_wireframe,
            &r->uber_program_line, &r->uber_program_points
        };
        ShaderBinding *bind_ptrs[6] = {
            &r->uber_shader_binding, &r->uber_shader_binding_tri_wireframe,
            &r->uber_shader_binding_quad,
            &r->uber_shader_binding_quad_wireframe,
            &r->uber_shader_binding_line, &r->uber_shader_binding_points
        };

        GLuint cached[6] = {0};
        bool loaded_from_cache = g_config.perf.cache_shaders && uber_cache_load_gl(cached);
        if (loaded_from_cache) {
            for (int v = 0; v < 6; v++) {
                *prog_ptrs[v] = cached[v];
            }
            fprintf(stderr, "nv2a: OpenGL ubershaders loaded from binary cache\n");
            xemu_queue_notification("OpenGL ubershaders loaded from cache");

            /* The six classic programs come back from the cache as linked
             * binaries, but the optional GS-bypass program is not stored there.
             * Rebuild it from a throwaway uber vertex shader so the fast path is
             * available on cached runs too (without it, cached launches would
             * permanently fall back to the slow classic-GS path). */
            GenUberGlslOptions uber_opts = { .vulkan = false };
            MString *vsh_code = pgraph_glsl_gen_uber_vsh(uber_opts);
            GLuint vs = create_gl_shader(GL_VERTEX_SHADER,
                                         mstring_get_str(vsh_code),
                                         "uber vertex shader");
            build_uber_gl_tri_nogs(r, vs);
            glDeleteShader(vs);
            mstring_unref(vsh_code);
        } else {
            GenUberGlslOptions uber_opts = { .vulkan = false };

            MString *uber_vsh_code = pgraph_glsl_gen_uber_vsh(uber_opts);
            MString *uber_psh_code = pgraph_glsl_gen_uber_psh(uber_opts);

            GLuint vs = create_gl_shader(GL_VERTEX_SHADER,
                                         mstring_get_str(uber_vsh_code),
                                         "uber vertex shader");

            GLuint fs = create_gl_shader(GL_FRAGMENT_SHADER,
                                         mstring_get_str(uber_psh_code),
                                         "uber fragment shader");

            MString *(*gen_funcs[6])(GenUberGlslOptions) = {
                pgraph_glsl_gen_uber_geom,
                pgraph_glsl_gen_uber_geom_tri_wireframe,
                pgraph_glsl_gen_uber_geom_quad,
                pgraph_glsl_gen_uber_geom_quad_wireframe,
                pgraph_glsl_gen_uber_geom_line,
                pgraph_glsl_gen_uber_geom_points,
            };
            const char *names[6] = {
                "tri", "tri_wireframe", "quad", "quad_wireframe", "line", "points"
            };

            for (int v = 0; v < 6; v++) {
                MString *gs_code = gen_funcs[v](uber_opts);
                GLuint gs = create_gl_shader(GL_GEOMETRY_SHADER,
                                             mstring_get_str(gs_code),
                                             names[v]);

                GLuint prog = glCreateProgram();
                glAttachShader(prog, vs);
                glAttachShader(prog, fs);
                glAttachShader(prog, gs);
                glLinkProgram(prog);

                GLint linked = 0;
                glGetProgramiv(prog, GL_LINK_STATUS, &linked);
                if (!linked) {
                    GLchar log[2048];
                    glGetProgramInfoLog(prog, 2048, NULL, log);
                    fprintf(stderr, "nv2a: uber %s shader link failed: %s\n",
                            names[v], log);
                    abort();
                }

                glDeleteShader(gs);
                mstring_unref(gs_code);

                *prog_ptrs[v] = prog;
            }

            /* Optional GS-less smooth-triangle program. It links the same
             * compiled VSH object the six classic-GS programs use (so the
             * FMA-sensitive transform math stays bit-identical) to a PSH
             * variant that reads the three vertex screen positions per-fragment
             * via GL_NV_fragment_shader_barycentric and runs the geometry
             * stage's rotation and plane math itself, so depth matches the GS
             * path. NVIDIA's GL driver runs geometry-shader pipelines on a slow
             * path, and smooth triangles are the common case, so bypassing the
             * geometry shader here is a large win. */
            build_uber_gl_tri_nogs(r, vs);

            glDeleteShader(vs);
            glDeleteShader(fs);
            mstring_unref(uber_vsh_code);
            mstring_unref(uber_psh_code);

            GLuint save_progs[6] = {
                r->uber_program, r->uber_program_tri_wireframe,
                r->uber_program_quad,
                r->uber_program_quad_wireframe,
                r->uber_program_line, r->uber_program_points
            };
            if (g_config.perf.cache_shaders) {
                uber_cache_save_gl(save_progs);
            }

            fprintf(stderr, "nv2a: OpenGL ubershaders compiled (tri/tri_wireframe/quad/quad_wireframe/line/points)\n");
            xemu_queue_notification("OpenGL ubershaders compiled and ready");
        }

        struct UberPshLocs *psh_loc_ptrs[6] = {
            &r->uber_psh_locs, &r->uber_psh_locs_tri_wireframe,
            &r->uber_psh_locs_quad,
            &r->uber_psh_locs_quad_wireframe,
            &r->uber_psh_locs_line, &r->uber_psh_locs_points
        };
        struct UberVshLocs *vsh_loc_ptrs[6] = {
            &r->uber_vsh_locs, &r->uber_vsh_locs_tri_wireframe,
            &r->uber_vsh_locs_quad,
            &r->uber_vsh_locs_quad_wireframe,
            &r->uber_vsh_locs_line, &r->uber_vsh_locs_points
        };
        struct UberGeomLocs *geom_loc_ptrs[6] = {
            &r->uber_geom_locs, &r->uber_geom_locs_tri_wireframe,
            &r->uber_geom_locs_quad,
            &r->uber_geom_locs_quad_wireframe,
            &r->uber_geom_locs_line, &r->uber_geom_locs_points
        };

        for (int v = 0; v < 6; v++) {
            GLuint prog = *prog_ptrs[v];
            glUseProgram(prog);
            /* arrayed samplers, same unit layout */
            static const GLint u2d[4] = { 0, 1, 2, 3 };
            static const GLint ucb[4] = { 4, 5, 6, 7 };
            static const GLint u3d[4] = { 8, 9, 10, 11 };
            GLint loc = glGetUniformLocation(prog, "texSamp[0]");
            if (loc >= 0) glUniform1iv(loc, 4, u2d);
            loc = glGetUniformLocation(prog, "texSampCube[0]");
            if (loc >= 0) glUniform1iv(loc, 4, ucb);
            loc = glGetUniformLocation(prog, "texSamp3D[0]");
            if (loc >= 0) glUniform1iv(loc, 4, u3d);

            ShaderBinding *b = bind_ptrs[v];
            memset(b, 0, sizeof(*b));
            b->gl_program = prog;
            b->initialized = true;
            update_shader_uniform_locs(b);
            init_uber_uniform_locs_for(prog, psh_loc_ptrs[v],
                                       vsh_loc_ptrs[v], geom_loc_ptrs[v]);
        }

        if (r->uber_program_tri_nogs) {
            GLuint prog = r->uber_program_tri_nogs;
            glUseProgram(prog);
            static const GLint pt_u2d[4] = { 0, 1, 2, 3 };
            static const GLint pt_ucb[4] = { 4, 5, 6, 7 };
            static const GLint pt_u3d[4] = { 8, 9, 10, 11 };
            GLint pt_loc = glGetUniformLocation(prog, "texSamp[0]");
            if (pt_loc >= 0) glUniform1iv(pt_loc, 4, pt_u2d);
            pt_loc = glGetUniformLocation(prog, "texSampCube[0]");
            if (pt_loc >= 0) glUniform1iv(pt_loc, 4, pt_ucb);
            pt_loc = glGetUniformLocation(prog, "texSamp3D[0]");
            if (pt_loc >= 0) glUniform1iv(pt_loc, 4, pt_u3d);

            ShaderBinding *b = &r->uber_shader_binding_tri_nogs;
            memset(b, 0, sizeof(*b));
            b->gl_program = prog;
            b->initialized = true;
            update_shader_uniform_locs(b);
            init_uber_uniform_locs_for(prog, &r->uber_psh_locs_tri_nogs,
                                       &r->uber_vsh_locs_tri_nogs,
                                       &r->uber_geom_locs_tri_nogs);
        }

        /* c[] upload gates. Generation starts at 1 so the
         * zeroed bindings (uber_c_gen == 0) always take the first upload.
         * The dirty-range path needs c[i]'s location to be c[0]'s + i —
         * not spec-guaranteed, so verify the array's layout per program
         * (spot ends + middle) and fall back to full uploads if any uber
         * program disagrees. */
        r->uber_gl_c_gen = 1;
        /* force the first draw to read everything. */
        r->uber_gl_seen_c_wgen = (uint64_t)-1;
        r->uber_gl_seen_ltctx_wgen = (uint64_t)-1;
        r->uber_gl_c_locs_consecutive = true;
        for (int v = 0; v < 7; v++) {
            ShaderBinding *b = (v < 6) ? bind_ptrs[v] :
                                         &r->uber_shader_binding_tri_nogs;
            if (v == 6 && !r->uber_program_tri_nogs) {
                continue;
            }
            GLint c0 = b->uniform_locs.vsh[VshUniform_c];
            if (c0 == -1) {
                continue;
            }
            static const int probe[] = { 1, 96, NV2A_VERTEXSHADER_CONSTANTS - 1 };
            for (int p = 0; p < ARRAY_SIZE(probe); p++) {
                char cname[16];
                snprintf(cname, sizeof(cname), "c[%d]", probe[p]);
                if (glGetUniformLocation(b->gl_program, cname) !=
                    c0 + probe[p]) {
                    r->uber_gl_c_locs_consecutive = false;
                    break;
                }
            }
        }

        glUseProgram(0);
    }
}

void pgraph_gl_finalize_shaders(PGRAPHState *pg)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    if (!r->uber_active) {
        pgraph_gl_shader_write_cache_reload_list(pg);
        free(r->shader_cache_entries);
        r->shader_cache_entries = NULL;

        lru_flush(&r->shader_module_cache);
        g_free(r->shader_module_cache_entries);
        r->shader_module_cache_entries = NULL;
    }

    if (r->uber_active) {
        glDeleteProgram(r->uber_program);
        /* tri_wireframe was missing here (shutdown leak). */
        glDeleteProgram(r->uber_program_tri_wireframe);
        glDeleteProgram(r->uber_program_quad);
        glDeleteProgram(r->uber_program_quad_wireframe);
        glDeleteProgram(r->uber_program_line);
        glDeleteProgram(r->uber_program_points);
        glDeleteProgram(r->uber_program_tri_nogs);
        r->uber_program = 0;
        r->uber_program_tri_wireframe = 0;
        r->uber_program_quad = 0;
        r->uber_program_quad_wireframe = 0;
        r->uber_program_line = 0;
        r->uber_program_points = 0;
        r->uber_program_tri_nogs = 0;
        r->uber_active = false;
    }

    qemu_mutex_destroy(&r->shader_cache_lock);
}

static void *shader_write_to_disk(void *arg)
{
    ShaderBinding *binding = (ShaderBinding*) arg;

    char *shader_bin = shader_get_bin_directory(binding->node.hash);
    char *shader_path = shader_get_binary_path(shader_bin, binding->node.hash);

    static uint64_t gl_vendor_len;
    if (gl_vendor_len == 0) {
        gl_vendor_len = (uint64_t) (strlen(shader_gl_vendor) + 1);
    }

    static uint64_t xemu_version_len = 0;
    if (xemu_version_len == 0) {
        xemu_version_len = (uint64_t) (strlen(xemu_version) + 1);
    }

    qemu_mkdir(shader_bin);
    g_free(shader_bin);

    FILE *shader_file = qemu_fopen(shader_path, "wb");
    if (!shader_file) {
        goto error;
    }

    size_t written;
    #define WRITE_OR_ERR(data, data_size) \
        do { \
            written = fwrite(data, data_size, 1, shader_file); \
            if (written != 1) { \
                fclose(shader_file); \
                goto error; \
            } \
        } while (0)

    WRITE_OR_ERR(&xemu_version_len, sizeof(xemu_version_len));
    WRITE_OR_ERR(xemu_version, xemu_version_len);

    WRITE_OR_ERR(&gl_vendor_len, sizeof(gl_vendor_len));
    WRITE_OR_ERR(shader_gl_vendor, gl_vendor_len);

    WRITE_OR_ERR(&binding->program_format, sizeof(binding->program_format));
    WRITE_OR_ERR(&binding->state, sizeof(binding->state));

    WRITE_OR_ERR(&binding->program_size, sizeof(binding->program_size));
    WRITE_OR_ERR(binding->program, binding->program_size);

    #undef WRITE_OR_ERR

    fclose(shader_file);

    g_free(shader_path);
    g_free(binding->program);
    binding->program = NULL;

    return NULL;

error:
    fprintf(stderr, "nv2a: Failed to write shader binary file to %s\n", shader_path);
    qemu_unlink(shader_path);
    g_free(shader_path);
    g_free(binding->program);
    binding->program = NULL;
    return NULL;
}

void pgraph_gl_shader_cache_to_disk(ShaderBinding *binding)
{
    if (binding->cached) {
        return;
    }

    GLint program_size;
    glGetProgramiv(binding->gl_program, GL_PROGRAM_BINARY_LENGTH, &program_size);

    if (binding->program) {
        g_free(binding->program);
        binding->program = NULL;
    }

    /* program_size might be zero on some systems, if no binary formats are supported */
    if (program_size == 0) {
        return;
    }

    binding->program = g_malloc(program_size);
    GLsizei program_size_copied;
    glGetProgramBinary(binding->gl_program, program_size, &program_size_copied,
                       &binding->program_format, binding->program);
    assert(glGetError() == GL_NO_ERROR);

    binding->program_size = program_size_copied;
    binding->cached = true;

    char name[24];
    snprintf(name, sizeof(name), "scache-%llx", (unsigned long long) binding->node.hash);
    binding->save_thread = g_malloc0(sizeof(QemuThread));
    qemu_thread_create(binding->save_thread, name, shader_write_to_disk, binding, QEMU_THREAD_JOINABLE);
}

static void apply_uniform_updates(const UniformInfo *info, int *locs,
                                  void *values, size_t count)
{
    for (int i = 0; i < count; i++) {
        if (locs[i] == -1) {
            continue;
        }

        void *value = (char*)values + info[i].val_offs;

        switch (info[i].type) {
        case UniformElementType_uint:
            glUniform1uiv(locs[i], info[i].count, value);
            break;
        case UniformElementType_int:
            glUniform1iv(locs[i], info[i].count, value);
            break;
        case UniformElementType_ivec2:
            glUniform2iv(locs[i], info[i].count, value);
            break;
        case UniformElementType_ivec4:
            glUniform4iv(locs[i], info[i].count, value);
            break;
        case UniformElementType_float:
            glUniform1fv(locs[i], info[i].count, value);
            break;
        case UniformElementType_vec2:
            glUniform2fv(locs[i], info[i].count, value);
            break;
        case UniformElementType_vec3:
            glUniform3fv(locs[i], info[i].count, value);
            break;
        case UniformElementType_vec4:
            glUniform4fv(locs[i], info[i].count, value);
            break;
        case UniformElementType_mat2:
            glUniformMatrix2fv(locs[i], info[i].count, GL_FALSE, value);
            break;
        default:
            g_assert_not_reached();
        }
    }

    assert(glGetError() == GL_NO_ERROR);
}

static void upload_uber_psh_uniforms(const struct UberPshLocs *l,
                                     const UberPshUniforms *u)
{
    if (l->combiner_control >= 0)
        glUniform1ui(l->combiner_control, u->combiner_control);
    /* shader_stage_program / other_stage_input uploads removed
     * (uniforms deleted — GLSL never read them). */
    if (l->final_inputs_0 >= 0)
        glUniform1ui(l->final_inputs_0, u->final_inputs_0);
    if (l->final_inputs_1 >= 0)
        glUniform1ui(l->final_inputs_1, u->final_inputs_1);
    if (l->rgb_inputs >= 0)
        glUniform1uiv(l->rgb_inputs, 8, u->rgb_inputs);
    if (l->rgb_outputs >= 0)
        glUniform1uiv(l->rgb_outputs, 8, u->rgb_outputs);
    if (l->alpha_inputs >= 0)
        glUniform1uiv(l->alpha_inputs, 8, u->alpha_inputs);
    if (l->alpha_outputs >= 0)
        glUniform1uiv(l->alpha_outputs, 8, u->alpha_outputs);
    if (l->uber_flags >= 0)
        glUniform1ui(l->uber_flags, u->uber_flags);
    if (l->alpha_func >= 0)
        glUniform1i(l->alpha_func, u->alpha_func);
    if (l->shadow_depth_func >= 0)
        glUniform1i(l->shadow_depth_func, u->shadow_depth_func);
    if (l->depth_format >= 0)
        glUniform1i(l->depth_format, u->depth_format);
    if (l->tex_modes >= 0)
        glUniform1ui(l->tex_modes, u->tex_modes);
    if (l->tex_flags >= 0)
        glUniform1uiv(l->tex_flags, 4, u->tex_flags);
        if (l->tex_size != -1) {
            glUniform2iv(l->tex_size, 4, (const GLint *)u->tex_size);
        }
    /* dim_tex upload removed (uniform deleted). */
    if (l->colorkey_mode >= 0)
        glUniform1iv(l->colorkey_mode, 4, u->colorkey_mode);
    if (l->border_logical_size >= 0)
        glUniform4fv(l->border_logical_size, 4,
                     &u->border_logical_size[0][0]);
    if (l->border_inv_real_size >= 0)
        glUniform4fv(l->border_inv_real_size, 4,
                     &u->border_inv_real_size[0][0]);
    if (l->compare_mode_packed >= 0)
        glUniform1ui(l->compare_mode_packed, u->compare_mode_packed);
    if (l->dot_map >= 0)
        glUniform1iv(l->dot_map, 4, u->dot_map);
    if (l->input_tex >= 0)
        glUniform1iv(l->input_tex, 4, u->input_tex);
    if (l->conv_tex >= 0)
        glUniform1iv(l->conv_tex, 4, u->conv_tex);
}

static void upload_uber_vsh_uniforms(const struct UberVshLocs *l,
                                     const UberVshUniforms *u)
{
    if (l->uber_vsh_flags >= 0)
        glUniform1ui(l->uber_vsh_flags, u->uber_vsh_flags);
    if (l->skinning_mode >= 0)
        glUniform1i(l->skinning_mode, u->skinning_mode);
    if (l->fog_mode >= 0)
        glUniform1i(l->fog_mode, u->fog_mode);
    if (l->foggen_mode >= 0)
        glUniform1i(l->foggen_mode, u->foggen_mode);
    if (l->light_enable_packed >= 0)
        glUniform1ui(l->light_enable_packed, u->light_enable_packed);
    if (l->texgen_packed >= 0)
        glUniform1uiv(l->texgen_packed, 4, u->texgen_packed);
    if (l->mat_color_src_packed >= 0)
        glUniform1ui(l->mat_color_src_packed, u->mat_color_src_packed);
    if (l->uniform_attrs >= 0)
        glUniform1ui(l->uniform_attrs, u->uniform_attrs);
    if (l->compressed_attrs >= 0)
        glUniform1ui(l->compressed_attrs, u->compressed_attrs);
    if (l->swizzle_attrs >= 0)
        glUniform1ui(l->swizzle_attrs, u->swizzle_attrs);
    if (l->point_size >= 0)
        glUniform1f(l->point_size, u->point_size);
    if (l->point_params >= 0)
        glUniform1fv(l->point_params, 8, u->point_params);
    if (l->surface_scale_factor >= 0)
        glUniform1i(l->surface_scale_factor, u->surface_scale_factor);
    if (l->program_length >= 0)
        glUniform1i(l->program_length, u->program_length);
    if (l->program_data >= 0)
        glUniform1uiv(l->program_data, 136 * 3, u->program_data);
}

static void upload_uber_geom_uniforms(const struct UberGeomLocs *l,
                                      const UberGeomUniforms *u)
{
    if (l->primitive_mode >= 0)
        glUniform1i(l->primitive_mode, u->primitive_mode);
    if (l->polygon_front_mode >= 0)
        glUniform1i(l->polygon_front_mode, u->polygon_front_mode);
    if (l->polygon_back_mode >= 0)
        glUniform1i(l->polygon_back_mode, u->polygon_back_mode);
    if (l->uber_geom_flags >= 0)
        glUniform1ui(l->uber_geom_flags, u->uber_geom_flags);
    if (l->tri_rot0 >= 0)
        glUniform1i(l->tri_rot0, u->tri_rot0);
    if (l->tri_rot1 >= 0)
        glUniform1i(l->tri_rot1, u->tri_rot1);
}

static void init_uber_uniform_locs_for(GLuint p,
                                       struct UberPshLocs *psh,
                                       struct UberVshLocs *vsh,
                                       struct UberGeomLocs *geom)
{
    psh->combiner_control = glGetUniformLocation(p, "u_combiner_control");
    psh->final_inputs_0 = glGetUniformLocation(p, "u_final_inputs_0");
    psh->final_inputs_1 = glGetUniformLocation(p, "u_final_inputs_1");
    psh->rgb_inputs = glGetUniformLocation(p, "u_rgb_inputs[0]");
    psh->rgb_outputs = glGetUniformLocation(p, "u_rgb_outputs[0]");
    psh->alpha_inputs = glGetUniformLocation(p, "u_alpha_inputs[0]");
    psh->alpha_outputs = glGetUniformLocation(p, "u_alpha_outputs[0]");
    psh->uber_flags = glGetUniformLocation(p, "u_uber_flags");
    psh->alpha_func = glGetUniformLocation(p, "u_alpha_func");
    psh->shadow_depth_func = glGetUniformLocation(p, "u_shadow_depth_func");
    psh->depth_format = glGetUniformLocation(p, "u_depth_format");
    psh->tex_modes = glGetUniformLocation(p, "u_tex_modes");
    psh->tex_flags = glGetUniformLocation(p, "u_tex_flags[0]");
    psh->tex_size = glGetUniformLocation(p, "u_tex_size[0]");
    psh->colorkey_mode = glGetUniformLocation(p, "u_colorkey_mode[0]");
    psh->border_logical_size = glGetUniformLocation(p, "u_border_logical_size[0]");
    psh->border_inv_real_size = glGetUniformLocation(p, "u_border_inv_real_size[0]");
    psh->compare_mode_packed = glGetUniformLocation(p, "u_compare_mode_packed");
    psh->dot_map = glGetUniformLocation(p, "u_dot_map[0]");
    psh->input_tex = glGetUniformLocation(p, "u_input_tex[0]");
    psh->conv_tex = glGetUniformLocation(p, "u_conv_tex[0]");

    vsh->uber_vsh_flags = glGetUniformLocation(p, "u_uber_vsh_flags");
    vsh->skinning_mode = glGetUniformLocation(p, "u_skinning_mode");
    vsh->fog_mode = glGetUniformLocation(p, "u_fog_mode");
    vsh->foggen_mode = glGetUniformLocation(p, "u_foggen_mode");
    vsh->light_enable_packed = glGetUniformLocation(p, "u_light_enable_packed");
    vsh->texgen_packed = glGetUniformLocation(p, "u_texgen_packed[0]");
    vsh->mat_color_src_packed = glGetUniformLocation(p, "u_mat_color_src_packed");
    vsh->uniform_attrs = glGetUniformLocation(p, "u_uniform_attrs");
    vsh->compressed_attrs = glGetUniformLocation(p, "u_compressed_attrs");
    vsh->swizzle_attrs = glGetUniformLocation(p, "u_swizzle_attrs");
    vsh->point_size = glGetUniformLocation(p, "u_point_size");
    vsh->point_params = glGetUniformLocation(p, "u_point_params[0]");
    vsh->surface_scale_factor = glGetUniformLocation(p, "u_surface_scale_factor");
    vsh->program_length = glGetUniformLocation(p, "u_program_length");
    vsh->program_data = glGetUniformLocation(p, "u_program_data[0]");

    geom->primitive_mode = glGetUniformLocation(p, "u_primitive_mode");
    geom->polygon_front_mode = glGetUniformLocation(p, "u_polygon_front_mode");
    geom->polygon_back_mode = glGetUniformLocation(p, "u_polygon_back_mode");
    geom->uber_geom_flags = glGetUniformLocation(p, "u_uber_geom_flags");
    geom->tri_rot0 = glGetUniformLocation(p, "u_tri_rot0");
    geom->tri_rot1 = glGetUniformLocation(p, "u_tri_rot1");
}

// FIXME: Dirty tracking
// FIXME: Consider UBO to align with VK renderer
/* (GL port of the VK upload-skip): compute values first, hash the
 * full source (vsh/psh values + 3 uber structs + c[]) vs the last applied
 * draw; equal hash implies an equal geom struct (it holds the variant
 * selectors) so the baseline cannot alias across programs. Returns false on
 * a hit — the caller must also skip the 3 uber-struct uploads (hash-covered).
 * Adaptive credit: hash while credit >= 0, decay on sustained change, re-arm
 * 1/1024; a non-hashed draw force-writes + invalidates the baseline.
 * Spec path: always true, GL call sequence unchanged. */
static bool update_shader_uniforms(PGRAPHState *pg, ShaderBinding *binding)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    /* compute phase — for uber the value structs are persistent members of r
     * (g_malloc0'd: loc==-1 holes stay zero, hashed bytes deterministic). */
    VshUniformValues vsh_values_local;
    PshUniformValues psh_values_local;
    VshUniformValues *vshv =
        r->uber_active ? &r->uber_gl_vsh_values : &vsh_values_local;
    PshUniformValues *pshv =
        r->uber_active ? &r->uber_gl_psh_values : &psh_values_local;

    /* (GL arm): skip the uber's wasted 3KB c[] fill (it just copies
     * pg->vsh_constants into vsh_values.c) and upload c[] straight from
     * pg->vsh_constants. Parity-identical bytes; uber-only; spec keeps full fill. */
    GLint c_loc = binding->uniform_locs.vsh[VshUniform_c];
    bool uber_c = r->uber_active && c_loc != -1;
    int vsh_locs[VshUniform__COUNT];
    if (uber_c) {
        memcpy(vsh_locs, binding->uniform_locs.vsh, sizeof(vsh_locs));
        vsh_locs[VshUniform_c] = -1;
    }
    pgraph_glsl_set_vsh_uniform_values(
        pg, &binding->state.vsh,
        uber_c ? vsh_locs : binding->uniform_locs.vsh, vshv);

    pgraph_glsl_set_psh_uniform_values(pg, binding->uniform_locs.psh, pshv);
    for (int i = 0; i < 4; i++) {
        if (r->texture_binding[i] != NULL) {
            float scale = r->texture_binding[i]->scale;
            pshv->texScale[i] = scale;
        }
    }

    uint64_t vsh_hash = 0, psh_hash = 0;
    bool hashed = false;

    /* hash decision. c[] contributes via the shape subhash:
     * recomputed only while its dirty flags are raised (they are consumed by
     * uber_gl_snapshot after every uber bind, so the fingerprint is stale-proof). */
    if (r->uber_active) {
        if (r->uber_gl_hash_credit >= 0) {
            if (pg->vsh_constants_wgen != r->uber_gl_seen_c_wgen ||
                !r->uber_gl_c_hash_valid) {
                r->uber_gl_c_subhash =
                    fast_hash((const uint8_t *)pg->vsh_constants,
                              sizeof(pg->vsh_constants));
                r->uber_gl_c_hash_valid = true;
            }
            /* keep the per-family fingerprints — the
             * write phase applies only families whose bytes moved. */
            vsh_hash = fast_hash((const uint8_t *)vshv, sizeof(*vshv));
            psh_hash = fast_hash((const uint8_t *)pshv, sizeof(*pshv));
            hashed = true;
            uint64_t h = vsh_hash;
            h = (h * 0x100000001b3ull) ^ psh_hash;
            h = (h * 0x100000001b3ull) ^ r->uber_gl_structs_subhash;
            h = (h * 0x100000001b3ull) ^ r->uber_gl_c_subhash;
            if (!r->uber_gl_src_hash_valid) {
                r->uber_gl_src_hash = h;
                r->uber_gl_src_hash_valid = true;
            } else if (h == r->uber_gl_src_hash) {
                if (r->uber_gl_hash_credit < 8) {
                    r->uber_gl_hash_credit++;
                }
                nv2a_profile_inc_counter(NV2A_PROF_SHADER_BIND_NOTDIRTY);
                return false;
            } else {
                r->uber_gl_src_hash = h;
                if (r->uber_gl_hash_credit > -8) {
                    r->uber_gl_hash_credit--;
                }
            }
        } else {
            r->uber_gl_src_hash_valid = false;
            if ((r->uber_gl_hash_probe++ & 1023u) == 0) {
                r->uber_gl_hash_credit = 0; /* re-arm */
            }
        }
    }

    /* write phase (source changed, first draw, hashing off, or spec).
     * On a hashed miss, apply only the families whose
     * fingerprint moved — GL retains per-program uniform state, so an equal
     * fingerprint means THIS program already holds those exact bytes. */
    if (r->uber_active) {
        /* uniforms are about to change under this program —
         * a deferred multidraw must go out under the old values first. */
        pgraph_gl_uber_submit_md(r);
    }
    bool skip_ok = hashed && binding->uber_family_valid;
    if (!(skip_ok && vsh_hash == binding->uber_vshv_hash)) {
        apply_uniform_updates(VshUniformInfo,
                              uber_c ? vsh_locs : binding->uniform_locs.vsh,
                              vshv, VshUniform__COUNT);
    }
    if (uber_c) {
        /* c[] is a persistent GL uniform —
         * skip clean re-uploads and extend across program switches
         * via a generation counter; a program current before the bump
         * (gen-1) uploads only the dirty min..max span, others full-upload.
         * vsh_constants_dirty is read before uber_gl_snapshot clears it. */
        bool c_dirty =
            pg->vsh_constants_wgen != r->uber_gl_seen_c_wgen;
        if (c_dirty) {
            r->uber_gl_c_gen++;
        }
        if (binding->uber_c_gen != r->uber_gl_c_gen) {
            if (c_dirty && binding->uber_c_gen == r->uber_gl_c_gen - 1 &&
                r->uber_gl_c_locs_consecutive) {
                int lo = 0, hi = NV2A_VERTEXSHADER_CONSTANTS - 1;
                while (lo < hi && !pg->vsh_constants_dirty[lo]) {
                    lo++;
                }
                while (hi > lo && !pg->vsh_constants_dirty[hi]) {
                    hi--;
                }
                glUniform4fv(c_loc + lo, hi - lo + 1,
                             (const GLfloat *)pg->vsh_constants[lo]);
            } else {
                glUniform4fv(c_loc, NV2A_VERTEXSHADER_CONSTANTS,
                             (const GLfloat *)pg->vsh_constants);
            }
            binding->uber_c_gen = r->uber_gl_c_gen;
        }
    }
    if (!(skip_ok && psh_hash == binding->uber_pshv_hash)) {
        apply_uniform_updates(PshUniformInfo, binding->uniform_locs.psh,
                              pshv, PshUniform__COUNT);
    }
    if (r->uber_active) {
        /* record what this program now holds. A non-hashed
         * apply (force-write path) computed no fingerprints — invalidate. */
        binding->uber_vshv_hash = vsh_hash;
        binding->uber_pshv_hash = psh_hash;
        binding->uber_family_valid = hashed;
    }
    return true;
}

/* two-layer GL uber dirty-skip. Layer 1: same shader state +
 * textures + surface -> skip ShaderState rebuild/variant switch/3 uber-struct
 * uploads, refresh only regular VSH/PSH values. Layer 2: additionally no
 * value change -> total skip (split required: content dirties c[] nearly
 * every frame). Signals: check_shader_state_dirty (its primitive/SETUPRASTER
 * coverage pins the variant binding), uniform_state_gen, the per-entry
 * c[]/ltctx dirty flags (uniform_state_gen alone does not cover
 * c[] — both required; flags cleared in uber_gl_snapshot), VK's value-reg
 * list mirror, texture pointers/scales + surface dims. Enabler: the GL
 * dirty-reg map is now cleared at end of pgraph_gl_flush_draw, gated on
 * uber_active — spec keeps its exact prior behavior. */
static const unsigned int uber_gl_uniform_value_regs[] = {
    NV_PGRAPH_COMBINEFACTOR0,      NV_PGRAPH_COMBINEFACTOR0 + 4,
    NV_PGRAPH_COMBINEFACTOR0 + 8,  NV_PGRAPH_COMBINEFACTOR0 + 12,
    NV_PGRAPH_COMBINEFACTOR0 + 16, NV_PGRAPH_COMBINEFACTOR0 + 20,
    NV_PGRAPH_COMBINEFACTOR0 + 24, NV_PGRAPH_COMBINEFACTOR0 + 28,
    NV_PGRAPH_COMBINEFACTOR1,      NV_PGRAPH_COMBINEFACTOR1 + 4,
    NV_PGRAPH_COMBINEFACTOR1 + 8,  NV_PGRAPH_COMBINEFACTOR1 + 12,
    NV_PGRAPH_COMBINEFACTOR1 + 16, NV_PGRAPH_COMBINEFACTOR1 + 20,
    NV_PGRAPH_COMBINEFACTOR1 + 24, NV_PGRAPH_COMBINEFACTOR1 + 28,
    NV_PGRAPH_SPECFOGFACTOR0, NV_PGRAPH_SPECFOGFACTOR1,
    NV_PGRAPH_COLORKEYCOLOR0, NV_PGRAPH_COLORKEYCOLOR1,
    NV_PGRAPH_COLORKEYCOLOR2, NV_PGRAPH_COLORKEYCOLOR3,
    NV_PGRAPH_FOGCOLOR, NV_PGRAPH_FOGPARAM0, NV_PGRAPH_FOGPARAM1,
    NV_PGRAPH_BUMPMAT00, NV_PGRAPH_BUMPMAT00 + 4, NV_PGRAPH_BUMPMAT00 + 8,
    NV_PGRAPH_BUMPMAT01, NV_PGRAPH_BUMPMAT01 + 4, NV_PGRAPH_BUMPMAT01 + 8,
    NV_PGRAPH_BUMPMAT10, NV_PGRAPH_BUMPMAT10 + 4, NV_PGRAPH_BUMPMAT10 + 8,
    NV_PGRAPH_BUMPMAT11, NV_PGRAPH_BUMPMAT11 + 4, NV_PGRAPH_BUMPMAT11 + 8,
    NV_PGRAPH_BUMPSCALE1, NV_PGRAPH_BUMPSCALE1 + 4, NV_PGRAPH_BUMPSCALE1 + 8,
    NV_PGRAPH_BUMPOFFSET1, NV_PGRAPH_BUMPOFFSET1 + 4, NV_PGRAPH_BUMPOFFSET1 + 8,
    NV_PGRAPH_WINDOWCLIPX0,      NV_PGRAPH_WINDOWCLIPX0 + 4,
    NV_PGRAPH_WINDOWCLIPX0 + 8,  NV_PGRAPH_WINDOWCLIPX0 + 12,
    NV_PGRAPH_WINDOWCLIPX0 + 16, NV_PGRAPH_WINDOWCLIPX0 + 20,
    NV_PGRAPH_WINDOWCLIPX0 + 24, NV_PGRAPH_WINDOWCLIPX0 + 28,
    NV_PGRAPH_WINDOWCLIPY0,      NV_PGRAPH_WINDOWCLIPY0 + 4,
    NV_PGRAPH_WINDOWCLIPY0 + 8,  NV_PGRAPH_WINDOWCLIPY0 + 12,
    NV_PGRAPH_WINDOWCLIPY0 + 16, NV_PGRAPH_WINDOWCLIPY0 + 20,
    NV_PGRAPH_WINDOWCLIPY0 + 24, NV_PGRAPH_WINDOWCLIPY0 + 28,
    NV_PGRAPH_ZCLIPMIN, NV_PGRAPH_ZCLIPMAX,
};

/* Layer-1 cleanliness: the inputs the UBER STRUCT uniforms depend on beyond
 * the shader-state check — texture bindings (border sizes/scales are derived
 * from the bound object, invisible to TEXFMT/TEXCTL reg dirt) and surface
 * dims. Required before skipping the uber-struct uploads. */
static bool uber_gl_texsurf_clean(PGRAPHState *pg, PGRAPHGLState *r)
{
    if (pg->surface_binding_dim.width != r->uber_last_surface_width ||
        pg->surface_binding_dim.height != r->uber_last_surface_height) {
        return false;
    }
    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (r->texture_binding[i] != r->uber_last_texture_binding[i]) {
            return false;
        }
        if (r->texture_binding[i] != NULL &&
            (r->texture_binding[i]->scale != r->uber_last_tex_scale[i] ||
             /* a same-binding glTexImage2D redefine moves
              * the stored dims (belt-and-braces; scale covers today's
              * redefine paths) */
             r->texture_binding[i]->uber_tex_size[0] !=
                 r->uber_last_tex_size[i][0] ||
             r->texture_binding[i]->uber_tex_size[1] !=
                 r->uber_last_tex_size[i][1])) {
            return false;
        }
    }
    return true;
}

/* Layer-2 cleanliness: the regular VSH/PSH uniform VALUE inputs (c[]
 * constants, lighting context, material/light value writes, value regs).
 * Content typically updates constants every frame, so this fires only on
 * truly static stretches — hence the split. */
static bool uber_gl_values_clean(PGRAPHState *pg, PGRAPHGLState *r)
{
    if (pg->uniform_state_gen != r->uber_last_uniform_state_gen) {
        return false;
    }
    /* writer generations replace the four flag scans; the generations
     * differ exactly when at least one flag was set since uber_gl_snapshot last
     * recorded (the flags themselves still feed the span logic and are
     * cleared in the snapshot exactly as before). */
    if (pg->vsh_constants_wgen != r->uber_gl_seen_c_wgen ||
        pg->ltctx_wgen != r->uber_gl_seen_ltctx_wgen) {
        return false;
    }
    for (int i = 0; i < ARRAY_SIZE(uber_gl_uniform_value_regs); i++) {
        if (pgraph_is_reg_dirty(pg, uber_gl_uniform_value_regs[i])) {
            return false;
        }
    }
    uint32_t c0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    if (GET_MASK(c0, NV_PGRAPH_CONTROL_0_ALPHAREF) != r->uber_last_alpha_ref ||
        (c0 & NV_PGRAPH_CONTROL_0_Z_PERSPECTIVE_ENABLE) != r->uber_last_z_persp ||
        pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) != r->uber_last_setupraster ||
        pgraph_reg_r(pg, NV_PGRAPH_ZOFFSETBIAS) != r->uber_last_zoffsetbias ||
        pgraph_reg_r(pg, NV_PGRAPH_ZOFFSETFACTOR) != r->uber_last_zoffsetfactor) {
        return false;
    }
    return true;
}

static void uber_gl_snapshot(PGRAPHState *pg, PGRAPHGLState *r)
{
    r->uber_last_uniform_state_gen = pg->uniform_state_gen;
    /* the seen generations advance with the clears. */
    r->uber_gl_seen_c_wgen = pg->vsh_constants_wgen;
    r->uber_gl_seen_ltctx_wgen = pg->ltctx_wgen;
    /* First (and only) consumer of these write-only dirty flags. */
    memset(pg->vsh_constants_dirty, 0, sizeof(pg->vsh_constants_dirty));
    memset(pg->ltctxa_dirty, 0, sizeof(pg->ltctxa_dirty));
    memset(pg->ltctxb_dirty, 0, sizeof(pg->ltctxb_dirty));
    memset(pg->ltc1_dirty, 0, sizeof(pg->ltc1_dirty));
    uint32_t c0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    r->uber_last_alpha_ref = GET_MASK(c0, NV_PGRAPH_CONTROL_0_ALPHAREF);
    r->uber_last_z_persp = c0 & NV_PGRAPH_CONTROL_0_Z_PERSPECTIVE_ENABLE;
    r->uber_last_setupraster = pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER);
    r->uber_last_zoffsetbias = pgraph_reg_r(pg, NV_PGRAPH_ZOFFSETBIAS);
    r->uber_last_zoffsetfactor = pgraph_reg_r(pg, NV_PGRAPH_ZOFFSETFACTOR);
    r->uber_last_surface_width = pg->surface_binding_dim.width;
    r->uber_last_surface_height = pg->surface_binding_dim.height;
    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        r->uber_last_texture_binding[i] = r->texture_binding[i];
        r->uber_last_tex_scale[i] =
            r->texture_binding[i] ? r->texture_binding[i]->scale : 0.0f;
        r->uber_last_tex_size[i][0] = r->texture_binding[i] ?
            r->texture_binding[i]->uber_tex_size[0] : 0;
        r->uber_last_tex_size[i][1] = r->texture_binding[i] ?
            r->texture_binding[i]->uber_tex_size[1] : 0;
    }
}

void pgraph_gl_bind_shaders(PGRAPHState *pg)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    if (r->uber_active) {
        /* skip the full rebuild + ~3.5KB uniform re-upload when
         * provably nothing relevant changed since the last uber draw. The
         * state-dirty check must run before pgraph_glsl_get_shader_state
         * (which clears program_data_dirty as a side effect). */
        if (r->shader_binding &&
            !pgraph_glsl_check_shader_state_dirty(pg,
                                                  &r->shader_binding->state) &&
            uber_gl_texsurf_clean(pg, r)) {
            if (uber_gl_values_clean(pg, r)) {
                /* Layer 2: nothing relevant changed at all — total skip. */
                nv2a_profile_inc_counter(NV2A_PROF_SHADER_BIND_NOTDIRTY);
                return;
            }
            /* Layer 1: same shader state, same textures/surface — only
             * regular uniform VALUES changed. Skip the ShaderState rebuild,
             * variant switch and the 3 uber-struct uploads; refresh just the
             * regular VSH/PSH uniforms (incl. c[] constants). */
            update_shader_uniforms(pg, r->shader_binding);
            uber_gl_snapshot(pg, r);
            return;
        }
        nv2a_profile_inc_counter(NV2A_PROF_SHADER_BIND);

        /* skip set_psh_state (the register-combiner derivation the
         * uber never reads — its PSH uniforms come from registers; state.psh is
         * spec-only). Build vsh+geom + zeta. Parity-identical; helps state-churn
         * draw-heavy content where this runs every draw. */
        ShaderState state;
        pgraph_glsl_set_uber_state_min(pg, &state);

        GLuint prog;
        ShaderBinding *binding;
        struct UberPshLocs *psh_locs;
        struct UberVshLocs *vsh_locs;
        struct UberGeomLocs *geom_locs;

        bool wireframe = (state.geom.polygon_front_mode == POLY_MODE_LINE);

        switch (state.geom.primitive_mode) {
        case PRIM_TYPE_POINTS:
            prog = r->uber_program_points;
            binding = &r->uber_shader_binding_points;
            psh_locs = &r->uber_psh_locs_points;
            vsh_locs = &r->uber_vsh_locs_points;
            geom_locs = &r->uber_geom_locs_points;
            break;
        case PRIM_TYPE_LINES:
        case PRIM_TYPE_LINE_LOOP:
        case PRIM_TYPE_LINE_STRIP:
            prog = r->uber_program_line;
            binding = &r->uber_shader_binding_line;
            psh_locs = &r->uber_psh_locs_line;
            vsh_locs = &r->uber_vsh_locs_line;
            geom_locs = &r->uber_geom_locs_line;
            break;
        case PRIM_TYPE_QUADS:
        case PRIM_TYPE_QUAD_STRIP:
            if (wireframe) {
                prog = r->uber_program_quad_wireframe;
                binding = &r->uber_shader_binding_quad_wireframe;
                psh_locs = &r->uber_psh_locs_quad_wireframe;
                vsh_locs = &r->uber_vsh_locs_quad_wireframe;
                geom_locs = &r->uber_geom_locs_quad_wireframe;
            } else {
                prog = r->uber_program_quad;
                binding = &r->uber_shader_binding_quad;
                psh_locs = &r->uber_psh_locs_quad;
                vsh_locs = &r->uber_vsh_locs_quad;
                geom_locs = &r->uber_geom_locs_quad;
            }
            break;
        case PRIM_TYPE_POLYGON:
            if (wireframe) {
                prog = r->uber_program_line;
                binding = &r->uber_shader_binding_line;
                psh_locs = &r->uber_psh_locs_line;
                vsh_locs = &r->uber_vsh_locs_line;
                geom_locs = &r->uber_geom_locs_line;
                break;
            }
            /* fall through */
        case PRIM_TYPE_TRIANGLES:
        case PRIM_TYPE_TRIANGLE_STRIP:
        case PRIM_TYPE_TRIANGLE_FAN:
            if (wireframe) {
                prog = r->uber_program_tri_wireframe;
                binding = &r->uber_shader_binding_tri_wireframe;
                psh_locs = &r->uber_psh_locs_tri_wireframe;
                vsh_locs = &r->uber_vsh_locs_tri_wireframe;
                geom_locs = &r->uber_geom_locs_tri_wireframe;
                break;
            }
            /* smooth tri-class fill draws take the NV
             * passthrough-GS program (no emit machinery = NVIDIA's GS fast
             * path; same VSH object; byte-identical PSH depth math); flat
             * keeps the classic GS (runtime provoking-vertex select).
             * Selection stays hash-keyed: smooth is a geom-struct flag
             * and primitive/polygon modes are the variant selectors. 0 =
             * extension/link unavailable -> classic GS fallback. */
            if (state.geom.smooth_shading && r->uber_program_tri_nogs) {
                prog = r->uber_program_tri_nogs;
                binding = &r->uber_shader_binding_tri_nogs;
                psh_locs = &r->uber_psh_locs_tri_nogs;
                vsh_locs = &r->uber_vsh_locs_tri_nogs;
                geom_locs = &r->uber_geom_locs_tri_nogs;
                break;
            }
            /* fall through */
        default:
            prog = r->uber_program;
            binding = &r->uber_shader_binding;
            psh_locs = &r->uber_psh_locs;
            vsh_locs = &r->uber_vsh_locs;
            geom_locs = &r->uber_geom_locs;
            break;
        }

        if (r->shader_binding != binding) {
            pgraph_gl_uber_submit_md(r);
            glUseProgram(prog);
        }

        binding->gl_primitive_mode = get_gl_primitive_mode(
            state.geom.polygon_front_mode, state.geom.primitive_mode);
        binding->state = state;
        r->shader_binding = binding;
        pg->program_data_dirty = false;

        /* removed per-draw glGetError drain (GPU sync per draw) */

        /* derive the three uber structs into the persistent
         * caches and refresh their subhash before the value update (it is a
         * hash input there), then upload them only when the source actually
         * changed — on a hash hit this binding already holds these exact
         * bytes (equal hash ⇒ equal geom struct ⇒ same variant selected). */
        pgraph_uber_set_psh_uniforms(pg, &r->uber_gl_psh_cached);
        /* mirror each bound 2D target's level-0
         * dims into u_tex_size (the agnostic setter zeroed them) — the
         * exact ints the per-fragment textureSize() returned. Non-2D
         * stages keep 0 (never consumed: normalize/conv are 2D-only). */
        for (int ti = 0; ti < 4; ti++) {
            TextureBinding *tb = r->texture_binding[ti];
            if (tb && tb->gl_target == GL_TEXTURE_2D) {
                r->uber_gl_psh_cached.tex_size[ti][0] =
                    tb->uber_tex_size[0];
                r->uber_gl_psh_cached.tex_size[ti][1] =
                    tb->uber_tex_size[1];
            }
        }
        pgraph_uber_set_vsh_uniforms(pg, &state.vsh, &r->uber_gl_vsh_cached);
        pgraph_uber_set_geom_uniforms(pg, &state.geom, &r->uber_gl_geom_cached);
        uint64_t sh = fast_hash((const uint8_t *)&r->uber_gl_psh_cached,
                                sizeof(r->uber_gl_psh_cached));
        sh = (sh * 0x100000001b3ull) ^
             fast_hash((const uint8_t *)&r->uber_gl_vsh_cached,
                       sizeof(r->uber_gl_vsh_cached));
        sh = (sh * 0x100000001b3ull) ^
             fast_hash((const uint8_t *)&r->uber_gl_geom_cached,
                       sizeof(r->uber_gl_geom_cached));
        r->uber_gl_structs_subhash = sh;

        if (update_shader_uniforms(pg, r->shader_binding)) {
            /* the 3 struct uploads ride their own family
             * fingerprint — equal subhash means this program already holds
             * these exact struct bytes (update_shader_uniforms just set
             * uber_family_valid for the hashed path). */
            ShaderBinding *sb = r->shader_binding;
            if (!sb->uber_family_valid ||
                sb->uber_structs_hash != r->uber_gl_structs_subhash) {
                upload_uber_psh_uniforms(psh_locs, &r->uber_gl_psh_cached);
                upload_uber_vsh_uniforms(vsh_locs, &r->uber_gl_vsh_cached);
                upload_uber_geom_uniforms(geom_locs, &r->uber_gl_geom_cached);
            }
            sb->uber_structs_hash = r->uber_gl_structs_subhash;
        }

        uber_gl_snapshot(pg, r);

        return;
    }

    bool binding_changed = false;
    if (r->shader_binding &&
        !pgraph_glsl_check_shader_state_dirty(pg, &r->shader_binding->state)) {
        nv2a_profile_inc_counter(NV2A_PROF_SHADER_BIND_NOTDIRTY);
        goto update_uniforms;
    }

    ShaderBinding *old_binding = r->shader_binding;
    ShaderState state = pgraph_glsl_get_shader_state(pg);

    NV2A_GL_DGROUP_BEGIN("%s (%s)", __func__,
                         state.vsh.is_fixed_function ? "FF" : "PROG");

    qemu_mutex_lock(&r->shader_cache_lock);

    uint64_t shader_state_hash =
        fast_hash((uint8_t *)&state, sizeof(ShaderState));

    LruNode *node = lru_lookup(&r->shader_cache, shader_state_hash, &state);
    ShaderBinding *binding = container_of(node, ShaderBinding, node);

    if (!binding->initialized && !pgraph_gl_shader_load_from_memory(binding)) {
        nv2a_profile_inc_counter(NV2A_PROF_SHADER_GEN);
        generate_shaders(r, binding);
        if (g_config.perf.cache_shaders) {
            pgraph_gl_shader_cache_to_disk(binding);
        }
    }
    assert(binding->initialized);
    r->shader_binding = binding;
    pg->program_data_dirty = false;

    qemu_mutex_unlock(&r->shader_cache_lock);

    binding_changed = (r->shader_binding != old_binding);
    if (binding_changed) {
        nv2a_profile_inc_counter(NV2A_PROF_SHADER_BIND);
        glUseProgram(r->shader_binding->gl_program);
    }

    NV2A_GL_DGROUP_END();

update_uniforms:
    assert(r->shader_binding);
    assert(r->shader_binding->initialized);
    update_shader_uniforms(pg, r->shader_binding);
}

GLuint pgraph_gl_compile_shader(const char *vs_src, const char *fs_src)
{
    GLint status;
    char err_buf[512];

    // Compile vertex shader
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vs_src, NULL);
    glCompileShader(vs);
    glGetShaderiv(vs, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        glGetShaderInfoLog(vs, sizeof(err_buf), NULL, err_buf);
        err_buf[sizeof(err_buf)-1] = '\0';
        fprintf(stderr, "Vertex shader compilation failed: %s\n", err_buf);
        exit(1);
    }

    // Compile fragment shader
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fs_src, NULL);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        glGetShaderInfoLog(fs, sizeof(err_buf), NULL, err_buf);
        err_buf[sizeof(err_buf)-1] = '\0';
        fprintf(stderr, "Fragment shader compilation failed: %s\n", err_buf);
        exit(1);
    }

    // Link vertex and fragment shaders
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glUseProgram(prog);

    // Flag shaders for deletion (will still be retained for lifetime of prog)
    glDeleteShader(vs);
    glDeleteShader(fs);

    return prog;
}
