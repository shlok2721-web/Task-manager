#define _GNU_SOURCE

#include "fan_control.h"

#include <adwaita.h>
#include <epoxy/gl.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <json-glib/json-glib.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FAN_PROFILE_PATH \
    "/sys/devices/platform/hp-wmi/platform-profile/platform-profile-0/profile"
#define FAN_APPLY_DELAY_MS 600
#define FAN_MAX_RPM 6000.0

#ifndef TASK_MANAGER_DATA_DIR
#define TASK_MANAGER_DATA_DIR "/usr/share/task-manager"
#endif
#define MODEL_PATH_DEFAULT "resources/fan.glb"
#define MODEL_PATH_INSTALLED TASK_MANAGER_DATA_DIR "/resources/fan.glb"
#define MODEL_PATH_FALLBACK "resources/fan.glb"
#define TARGET_MIN_PERCENT 20.0
#define TARGET_MAX_PERCENT 100.0
#define SAMPLE_MS 500
#define ANIMATION_MS 16

/* The uploaded model is a flat XZ-plane mesh, so it is corrected into the XY
 * plane before spinning around Z. A different fan model can use the same
 * loader; only this correction may need adjustment. */

static const char *FAN_CSS =
".fan-page { padding: 18px; }"
".fan-title { font-size: 26px; font-weight: 700; }"
".fan-subtitle { opacity: 0.72; }"
".fan-stage { background: #000; border-radius: 18px; min-height: 560px; }"
".fan-stage-label { color: rgba(255,255,255,0.92); font-weight: 650; padding: 16px; }"
".fan-stage-status { color: rgba(255,255,255,0.62); padding: 14px 16px; }"
".fan-card { border-radius: 16px; }"
".fan-value { font-size: 27px; font-weight: 700; }"
".fan-muted { opacity: 0.68; }"
".fan-danger { color: #ff6b6b; }"
".fan-safe { color: #7ee787; }"
".fan-curve { min-height: 170px; }"
".fan-section-title { font-size: 16px; font-weight: 700; }";

typedef struct {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
} FanVertex;

typedef struct {
    float m[16];
} Mat4;

typedef struct {
    FanVertex *vertices;
    size_t vertex_count;
    uint32_t *indices;
    size_t index_count;
    float center[3];
    float scale;
    GBytes *base_color_png;
    char *model_path;
} FanModel;

typedef enum {
    FAN_MODE_QUIET = 0,
    FAN_MODE_BALANCED,
    FAN_MODE_PERFORMANCE,
    FAN_MODE_COOL,
    FAN_MODE_COUNT
} FanMode;

static const char *const FAN_MODE_PROFILE[FAN_MODE_COUNT] =
    { "quiet", "balanced", "performance", "cool" };
static const char *const FAN_MODE_TITLE[FAN_MODE_COUNT] =
    { "Quiet", "Balanced", "Performance", "Cool" };
/* Nominal rotor animation speed, used only when no RPM can be read. */
static const double FAN_MODE_VISUAL[FAN_MODE_COUNT] = { 25, 45, 70, 100 };

typedef struct {
    GtkWidget *root;
    GtkGLArea *gl_area;
    GtkDropDown *mode_drop;
    GtkDropDown *fan_drop;
    GtkScale *level_scale;
    GtkScale *anim_scale;
    double anim_speed;
    GtkWidget *backend_label;
    GtkLabel *temp_label;
    GtkLabel *target_label;
    GtkLabel *rpm_label;
    GtkLabel *level_label;

    gboolean backend_available;
    unsigned selected_fan;
    FanMode mode;
    gboolean syncing;
    gboolean apply_pending;
    gboolean rpm_valid;
    guint apply_timer;
    unsigned fan_count;
    gchar *hwmon_dir;
    gchar *status;
    GCancellable *cancel;
    double current_temp;
    double target_percent;
    double reported_rpm;

    double angle;
    gint64 last_frame_us;
    guint animation_source;
    guint sample_source;

    FanModel model;
    GLuint vao;
    GLuint vbo;
    GLuint ebo;
    GLuint program;
    GLuint texture;
    GLint u_mvp;
    GLint u_model;
    GLint u_sampler;
    gboolean gl_ready;
    gboolean model_ready;

    GError *model_error;
    App *app;
} FanPage;

static Mat4 mat4_identity(void)
{
    Mat4 r = { { 1,0,0,0,
                 0,1,0,0,
                 0,0,1,0,
                 0,0,0,1 } };
    return r;
}

static Mat4 mat4_mul(Mat4 a, Mat4 b)
{
    Mat4 r = {{0}};
    for (int c = 0; c < 4; ++c) {
        for (int row = 0; row < 4; ++row) {
            double value = 0.0;
            for (int k = 0; k < 4; ++k)
                value += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = (float)value;
        }
    }
    return r;
}

static Mat4 mat4_translate(float x, float y, float z)
{
    Mat4 r = mat4_identity();
    r.m[12] = x;
    r.m[13] = y;
    r.m[14] = z;
    return r;
}

static Mat4 mat4_scale(float s)
{
    Mat4 r = mat4_identity();
    r.m[0] = r.m[5] = r.m[10] = s;
    return r;
}

static Mat4 mat4_rotate_x(float a)
{
    float c = cosf(a), s = sinf(a);
    Mat4 r = mat4_identity();
    r.m[5] = c;
    r.m[6] = s;
    r.m[9] = -s;
    r.m[10] = c;
    return r;
}

static Mat4 mat4_rotate_z(float a)
{
    float c = cosf(a), s = sinf(a);
    Mat4 r = mat4_identity();
    r.m[0] = c;
    r.m[1] = s;
    r.m[4] = -s;
    r.m[5] = c;
    return r;
}

static Mat4 mat4_perspective(float fovy, float aspect, float znear, float zfar)
{
    const float f = 1.0f / tanf(fovy * 0.5f);
    Mat4 r = {{0}};
    r.m[0] = f / aspect;
    r.m[5] = f;
    r.m[10] = (zfar + znear) / (znear - zfar);
    r.m[11] = -1.0f;
    r.m[14] = (2.0f * zfar * znear) / (znear - zfar);
    return r;
}

static gboolean read_u32(const guint8 *p, gsize len, gsize off, guint32 *out)
{
    if (off + 4 > len)
        return FALSE;
    memcpy(out, p + off, 4);
    return TRUE;
}

static gsize json_optional_uint(const JsonObject *o, const char *key)
{
    if (!json_object_has_member(o, key))
        return 0;
    return (gsize)json_object_get_int_member(o, key);
}

static gboolean get_accessor(const JsonArray *accessors, guint index,
                             JsonObject **out)
{
    if (!accessors || index >= json_array_get_length(accessors))
        return FALSE;
    *out = json_array_get_object_element(accessors, index);
    return *out != NULL;
}

static gboolean get_buffer_view(const JsonArray *views, guint index,
                                JsonObject **out)
{
    if (!views || index >= json_array_get_length(views))
        return FALSE;
    *out = json_array_get_object_element(views, index);
    return *out != NULL;
}

static const guint8 *accessor_data(const JsonObject *accessor,
                                   const JsonArray *views,
                                   const guint8 *bin, gsize bin_len,
                                   gsize *stride_out)
{
    if (!json_object_has_member(accessor, "bufferView"))
        return NULL;

    guint view_index = (guint)json_object_get_int_member(accessor, "bufferView");
    JsonObject *view = NULL;
    if (!get_buffer_view(views, view_index, &view))
        return NULL;

    guint buffer_index = 0;
    if (json_object_has_member(view, "buffer"))
        buffer_index = (guint)json_object_get_int_member(view, "buffer");
    if (buffer_index != 0)
        return NULL;

    gsize view_offset = json_optional_uint(view, "byteOffset");
    gsize accessor_offset = json_optional_uint(accessor, "byteOffset");
    gsize stride = json_optional_uint(view, "byteStride");
    if (stride == 0) {
        const char *type = json_object_get_string_member(accessor, "type");
        gint component_type = (gint)json_object_get_int_member(accessor, "componentType");
        gsize component_size = component_type == 5126 ? 4 :
                               component_type == 5125 ? 4 :
                               component_type == 5123 ? 2 :
                               component_type == 5121 ? 1 : 0;
        gint comps = g_strcmp0(type, "VEC4") == 0 ? 4 :
                     g_strcmp0(type, "VEC3") == 0 ? 3 :
                     g_strcmp0(type, "VEC2") == 0 ? 2 : 1;
        stride = component_size * (gsize)comps;
    }

    if (view_offset + accessor_offset >= bin_len)
        return NULL;

    *stride_out = stride;
    return bin + view_offset + accessor_offset;
}

static gboolean read_accessor_float_vec(const guint8 *base, gsize stride,
                                        guint index, guint components,
                                        float *out)
{
    if (!base || !out)
        return FALSE;
    const guint8 *p = base + ((gsize)index * stride);
    memcpy(out, p, components * sizeof(float));
    return TRUE;
}

static gboolean parse_glb(FanModel *model, const char *path, GError **error)
{
    gchar *file_data = NULL;
    gsize file_len = 0;
    if (!g_file_get_contents(path, &file_data, &file_len, error))
        return FALSE;

    if (file_len < 20 || memcmp(file_data, "glTF", 4) != 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "'%s' is not a valid GLB file", path);
        g_free(file_data);
        return FALSE;
    }

    guint32 version = 0, total_length = 0;
    memcpy(&version, file_data + 4, 4);
    memcpy(&total_length, file_data + 8, 4);
    if (version != 2 || total_length > file_len) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "Unsupported or truncated GLB file");
        g_free(file_data);
        return FALSE;
    }

    const guint8 *json_data = NULL;
    gsize json_len = 0;
    const guint8 *bin_data = NULL;
    gsize bin_len = 0;

    gsize off = 12;
    while (off + 8 <= file_len) {
        guint32 chunk_len = 0, chunk_type = 0;
        read_u32((const guint8 *)file_data, file_len, off, &chunk_len);
        read_u32((const guint8 *)file_data, file_len, off + 4, &chunk_type);
        off += 8;
        if (off + chunk_len > file_len)
            break;

        if (chunk_type == 0x4E4F534A) {
            json_data = (const guint8 *)file_data + off;
            json_len = chunk_len;
        } else if (chunk_type == 0x004E4942) {
            bin_data = (const guint8 *)file_data + off;
            bin_len = chunk_len;
        }
        off += chunk_len;
    }

    if (!json_data || !bin_data) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "GLB does not contain JSON and binary chunks");
        g_free(file_data);
        return FALSE;
    }

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, (const gchar *)json_data,
                                    (gssize)json_len, error)) {
        g_object_unref(parser);
        g_free(file_data);
        return FALSE;
    }

    JsonNode *root_node = json_parser_get_root(parser);
    JsonObject *root = json_node_get_object(root_node);
    JsonArray *meshes = json_object_get_array_member(root, "meshes");
    JsonArray *accessors = json_object_get_array_member(root, "accessors");
    JsonArray *views = json_object_get_array_member(root, "bufferViews");

    if (!meshes || json_array_get_length(meshes) == 0 || !accessors || !views) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "GLB contains no usable mesh data");
        g_object_unref(parser);
        g_free(file_data);
        return FALSE;
    }

    JsonObject *mesh = json_array_get_object_element(meshes, 0);
    JsonArray *primitives = mesh ? json_object_get_array_member(mesh, "primitives") : NULL;
    JsonObject *primitive = (primitives && json_array_get_length(primitives) > 0)
        ? json_array_get_object_element(primitives, 0) : NULL;
    JsonObject *attributes = primitive ? json_object_get_object_member(primitive, "attributes") : NULL;

    if (!attributes || !json_object_has_member(attributes, "POSITION") ||
        !json_object_has_member(primitive, "indices")) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "GLB primitive does not contain POSITION and indices");
        g_object_unref(parser);
        g_free(file_data);
        return FALSE;
    }

    guint pos_acc_i = (guint)json_object_get_int_member(attributes, "POSITION");
    guint normal_acc_i = json_object_has_member(attributes, "NORMAL")
        ? (guint)json_object_get_int_member(attributes, "NORMAL") : G_MAXUINT;
    guint uv_acc_i = json_object_has_member(attributes, "TEXCOORD_0")
        ? (guint)json_object_get_int_member(attributes, "TEXCOORD_0") : G_MAXUINT;
    guint idx_acc_i = (guint)json_object_get_int_member(primitive, "indices");

    JsonObject *pos_acc = NULL, *normal_acc = NULL, *uv_acc = NULL, *idx_acc = NULL;
    if (!get_accessor(accessors, pos_acc_i, &pos_acc) ||
        !get_accessor(accessors, idx_acc_i, &idx_acc)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "GLB accessors are invalid");
        g_object_unref(parser);
        g_free(file_data);
        return FALSE;
    }
    if (normal_acc_i != G_MAXUINT)
        get_accessor(accessors, normal_acc_i, &normal_acc);
    if (uv_acc_i != G_MAXUINT)
        get_accessor(accessors, uv_acc_i, &uv_acc);

    guint vertex_count = (guint)json_object_get_int_member(pos_acc, "count");
    guint index_count = (guint)json_object_get_int_member(idx_acc, "count");
    if (vertex_count == 0 || index_count == 0 || vertex_count > 10000000 || index_count > 30000000) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "GLB mesh dimensions are invalid");
        g_object_unref(parser);
        g_free(file_data);
        return FALSE;
    }

    gsize pos_stride = 0, normal_stride = 0, uv_stride = 0, idx_stride = 0;
    const guint8 *pos_ptr = accessor_data(pos_acc, views, bin_data, bin_len, &pos_stride);
    const guint8 *normal_ptr = normal_acc
        ? accessor_data(normal_acc, views, bin_data, bin_len, &normal_stride) : NULL;
    const guint8 *uv_ptr = uv_acc
        ? accessor_data(uv_acc, views, bin_data, bin_len, &uv_stride) : NULL;
    const guint8 *idx_ptr = accessor_data(idx_acc, views, bin_data, bin_len, &idx_stride);

    if (!pos_ptr || !idx_ptr) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "GLB attribute buffers are invalid");
        g_object_unref(parser);
        g_free(file_data);
        return FALSE;
    }

    model->vertices = g_new0(FanVertex, vertex_count);
    model->indices = g_new0(uint32_t, index_count);
    model->vertex_count = vertex_count;
    model->index_count = index_count;

    float minv[3] = { G_MAXFLOAT, G_MAXFLOAT, G_MAXFLOAT };
    float maxv[3] = { -G_MAXFLOAT, -G_MAXFLOAT, -G_MAXFLOAT };

    for (guint i = 0; i < vertex_count; ++i) {
        float p[3] = {0};
        read_accessor_float_vec(pos_ptr, pos_stride, i, 3, p);
        model->vertices[i].px = p[0];
        model->vertices[i].py = p[1];
        model->vertices[i].pz = p[2];

        if (normal_ptr) {
            float n[3] = {0};
            read_accessor_float_vec(normal_ptr, normal_stride, i, 3, n);
            model->vertices[i].nx = n[0];
            model->vertices[i].ny = n[1];
            model->vertices[i].nz = n[2];
        } else {
            model->vertices[i].nx = 0.0f;
            model->vertices[i].ny = 1.0f;
            model->vertices[i].nz = 0.0f;
        }

        if (uv_ptr) {
            float uv[2] = {0};
            read_accessor_float_vec(uv_ptr, uv_stride, i, 2, uv);
            model->vertices[i].u = uv[0];
            model->vertices[i].v = uv[1];
        }

        for (int a = 0; a < 3; ++a) {
            minv[a] = MIN(minv[a], p[a]);
            maxv[a] = MAX(maxv[a], p[a]);
        }
    }

    gint idx_component = (gint)json_object_get_int_member(idx_acc, "componentType");
    if (idx_component != 5125 && idx_component != 5123 && idx_component != 5121) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                    "Unsupported index component type");
        g_object_unref(parser);
        g_free(file_data);
        return FALSE;
    }

    for (guint i = 0; i < index_count; ++i) {
        const guint8 *p = idx_ptr + ((gsize)i * idx_stride);
        uint32_t idx = 0;
        if (idx_component == 5125) {
            memcpy(&idx, p, 4);
        } else if (idx_component == 5123) {
            guint16 v = 0;
            memcpy(&v, p, 2);
            idx = v;
        } else {
            idx = *p;
        }
        if (idx >= vertex_count) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "GLB contains an out-of-range vertex index");
            g_object_unref(parser);
            g_free(file_data);
            return FALSE;
        }
        model->indices[i] = idx;
    }

    model->center[0] = (minv[0] + maxv[0]) * 0.5f;
    model->center[1] = (minv[1] + maxv[1]) * 0.5f;
    model->center[2] = (minv[2] + maxv[2]) * 0.5f;

    float extent_x = maxv[0] - minv[0];
    float extent_y = maxv[1] - minv[1];
    float extent_z = maxv[2] - minv[2];
    float extent = MAX(extent_x, MAX(extent_y, extent_z));
    model->scale = extent > 0.0001f ? 2.4f / extent : 1.0f;

    /* Locate base-color PNG through material -> texture -> image -> bufferView. */
    JsonArray *materials = json_object_get_array_member(root, "materials");
    JsonArray *textures = json_object_get_array_member(root, "textures");
    JsonArray *images = json_object_get_array_member(root, "images");
    if (materials && textures && images && json_array_get_length(materials) > 0) {
        JsonObject *mat = json_array_get_object_element(materials, 0);
        JsonObject *pbr = mat ? json_object_get_object_member(mat, "pbrMetallicRoughness") : NULL;
        JsonObject *bc = pbr ? json_object_get_object_member(pbr, "baseColorTexture") : NULL;
        if (bc) {
            guint tex_i = (guint)json_object_get_int_member(bc, "index");
            if (tex_i < json_array_get_length(textures)) {
                JsonObject *tex = json_array_get_object_element(textures, tex_i);
                if (tex && json_object_has_member(tex, "source")) {
                    guint img_i = (guint)json_object_get_int_member(tex, "source");
                    if (img_i < json_array_get_length(images)) {
                        JsonObject *img = json_array_get_object_element(images, img_i);
                        if (img && json_object_has_member(img, "bufferView")) {
                            guint view_i = (guint)json_object_get_int_member(img, "bufferView");
                            JsonObject *view = NULL;
                            if (get_buffer_view(views, view_i, &view)) {
                                gsize img_off = json_optional_uint(view, "byteOffset");
                                gsize img_len = json_optional_uint(view, "byteLength");
                                if (img_off + img_len <= bin_len && img_len > 0) {
                                    model->base_color_png = g_bytes_new(bin_data + img_off, img_len);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    model->model_path = g_strdup(path);
    g_object_unref(parser);
    g_free(file_data);
    return TRUE;
}

static void fan_model_clear(FanModel *model)
{
    if (!model)
        return;
    g_free(model->vertices);
    g_free(model->indices);
    g_clear_pointer(&model->base_color_png, g_bytes_unref);
    g_clear_pointer(&model->model_path, g_free);
    memset(model, 0, sizeof(*model));
}

static GLuint compile_shader(GLenum type, const char *source, GError **error)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
        gchar *log = g_malloc0((gsize)MAX(len, 1));
        glGetShaderInfoLog(shader, len, NULL, log);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Shader compilation failed: %s", log);
        g_free(log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint create_program(GError **error)
{
    static const char *vs =
        "#version 330 core\n"
        "layout(location=0) in vec3 a_pos;\n"
        "layout(location=1) in vec3 a_normal;\n"
        "layout(location=2) in vec2 a_uv;\n"
        "uniform mat4 u_mvp;\n"
        "uniform mat4 u_model;\n"
        "out vec3 v_normal;\n"
        "out vec2 v_uv;\n"
        "void main(){\n"
        "  v_normal = mat3(u_model) * a_normal;\n"
        "  v_uv = a_uv;\n"
        "  gl_Position = u_mvp * vec4(a_pos,1.0);\n"
        "}\n";

    static const char *fs =
        "#version 330 core\n"
        "in vec3 v_normal;\n"
        "in vec2 v_uv;\n"
        "uniform sampler2D u_tex;\n"
        "out vec4 frag;\n"
        "void main(){\n"
        "  vec3 n = normalize(v_normal);\n"
        "  vec3 l = normalize(vec3(-0.35,0.55,1.0));\n"
        "  float diffuse = max(dot(n,l),0.0);\n"
        "  vec4 base = texture(u_tex,v_uv);\n"
        "  if(base.a < 0.03) discard;\n"
        "  vec3 c = base.rgb * (0.22 + 0.78*diffuse);\n"
        "  frag = vec4(c,1.0);\n"
        "}\n";

    GLuint v = compile_shader(GL_VERTEX_SHADER, vs, error);
    if (!v)
        return 0;
    GLuint f = compile_shader(GL_FRAGMENT_SHADER, fs, error);
    if (!f) {
        glDeleteShader(v);
        return 0;
    }

    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);

    GLint ok = GL_FALSE;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetProgramiv(p, GL_INFO_LOG_LENGTH, &len);
        gchar *log = g_malloc0((gsize)MAX(len, 1));
        glGetProgramInfoLog(p, len, NULL, log);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Shader link failed: %s", log);
        g_free(log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

static GLuint upload_texture(GBytes *bytes, GError **error)
{
    if (!bytes)
        return 0;

    gsize len = 0;
    const void *data = g_bytes_get_data(bytes, &len);
    if (!data || len == 0)
        return 0;

    GInputStream *stream = g_memory_input_stream_new_from_data(data, len, NULL);
    GError *local_error = NULL;
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_stream(stream, NULL, &local_error);
    g_object_unref(stream);
    if (!pixbuf) {
        g_propagate_error(error, local_error);
        return 0;
    }

    if (!gdk_pixbuf_get_has_alpha(pixbuf)) {
        GdkPixbuf *rgba = gdk_pixbuf_add_alpha(pixbuf, FALSE, 0, 0, 0);
        g_object_unref(pixbuf);
        pixbuf = rgba;
    }

    gint width = gdk_pixbuf_get_width(pixbuf);
    gint height = gdk_pixbuf_get_height(pixbuf);
    gint rowstride = gdk_pixbuf_get_rowstride(pixbuf);
    const guchar *pixels = gdk_pixbuf_get_pixels(pixbuf);

    gsize packed_stride = (gsize)width * 4;
    guchar *packed = g_malloc((gsize)height * packed_stride);
    for (gint y = 0; y < height; ++y)
        memcpy(packed + ((gsize)y * packed_stride),
               pixels + ((gsize)y * rowstride), packed_stride);

    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, packed);
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);

    g_free(packed);
    g_object_unref(pixbuf);
    return texture;
}

static gboolean fan_upload_model(FanPage *p, GError **error)
{
    if (!parse_glb(&p->model, MODEL_PATH_DEFAULT, error)) {
        g_clear_error(error);
        if (!parse_glb(&p->model, MODEL_PATH_FALLBACK, error))
            return FALSE;
    }

    glGenVertexArrays(1, &p->vao);
    glGenBuffers(1, &p->vbo);
    glGenBuffers(1, &p->ebo);

    glBindVertexArray(p->vao);
    glBindBuffer(GL_ARRAY_BUFFER, p->vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)(p->model.vertex_count * sizeof(FanVertex)),
                 p->model.vertices, GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, p->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 (GLsizeiptr)(p->model.index_count * sizeof(uint32_t)),
                 p->model.indices, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(FanVertex),
                          GSIZE_TO_POINTER(offsetof(FanVertex, px)));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(FanVertex),
                          GSIZE_TO_POINTER(offsetof(FanVertex, nx)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(FanVertex),
                          GSIZE_TO_POINTER(offsetof(FanVertex, u)));
    glEnableVertexAttribArray(2);
    glBindVertexArray(0);

    p->program = create_program(error);
    if (!p->program)
        return FALSE;

    p->u_mvp = glGetUniformLocation(p->program, "u_mvp");
    p->u_model = glGetUniformLocation(p->program, "u_model");
    p->u_sampler = glGetUniformLocation(p->program, "u_tex");

    p->texture = upload_texture(p->model.base_color_png, error);
    if (!p->texture) {
        /* A model without a texture still renders as a neutral white object. */
        glGenTextures(1, &p->texture);
        glBindTexture(GL_TEXTURE_2D, p->texture);
        const guint8 white[4] = {255, 255, 255, 255};
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, white);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindTexture(GL_TEXTURE_2D, 0);
        g_clear_error(error);
    }

    p->model_ready = TRUE;
    return TRUE;
}

static void fan_delete_gl(FanPage *p)
{
    if (p->texture) glDeleteTextures(1, &p->texture);
    if (p->program) glDeleteProgram(p->program);
    if (p->ebo) glDeleteBuffers(1, &p->ebo);
    if (p->vbo) glDeleteBuffers(1, &p->vbo);
    if (p->vao) glDeleteVertexArrays(1, &p->vao);
    p->texture = p->program = p->ebo = p->vbo = p->vao = 0;
    p->gl_ready = FALSE;
    p->model_ready = FALSE;
}



static gchar *fan_find_hwmon(void)
{
    for (int i = 0; i < 32; ++i) {
        g_autofree gchar *np = g_strdup_printf("/sys/class/hwmon/hwmon%d/name", i);
        g_autofree gchar *name = NULL;
        if (g_file_get_contents(np, &name, NULL, NULL)) {
            g_strstrip(name);
            if (g_strcmp0(name, "hp") == 0)
                return g_strdup_printf("/sys/class/hwmon/hwmon%d", i);
        }
    }
    return NULL;
}

static gboolean fan_read_rpm(FanPage *p, unsigned fan, double *rpm)
{
    if (!p->hwmon_dir)
        return FALSE;
    g_autofree gchar *path = g_strdup_printf("%s/fan%u_input", p->hwmon_dir, fan + 1);
    g_autofree gchar *txt = NULL;
    if (!g_file_get_contents(path, &txt, NULL, NULL))
        return FALSE;
    *rpm = g_ascii_strtod(txt, NULL);
    return TRUE;
}

static gboolean fan_read_profile(FanMode *mode)
{
    g_autofree gchar *txt = NULL;
    if (!g_file_get_contents(FAN_PROFILE_PATH, &txt, NULL, NULL))
        return FALSE;
    g_strstrip(txt);
    for (int i = 0; i < FAN_MODE_COUNT; ++i) {
        if (g_strcmp0(txt, FAN_MODE_PROFILE[i]) == 0) {
            *mode = (FanMode)i;
            return TRUE;
        }
    }
    return FALSE;
}

static double fan_compute_target(FanPage *p)
{
    if (p->rpm_valid)
        return CLAMP(p->reported_rpm / FAN_MAX_RPM * 100.0, 0.0, 100.0);
    return FAN_MODE_VISUAL[p->mode];
}

static gboolean read_cpu_temperature(double *out_c)
{
    for (int i = 0; i < 64; ++i) {
        gchar *type_path = g_strdup_printf("/sys/class/thermal/thermal_zone%d/type", i);
        gchar *temp_path = g_strdup_printf("/sys/class/thermal/thermal_zone%d/temp", i);
        gchar *type = NULL;
        gchar *value = NULL;

        gboolean ok = g_file_get_contents(type_path, &type, NULL, NULL) &&
                      g_file_get_contents(temp_path, &value, NULL, NULL);
        g_free(type_path);
        g_free(temp_path);

        if (!ok) {
            g_free(type);
            g_free(value);
            continue;
        }

        g_strstrip(type);
        if (g_strrstr(type, "cpu") || g_strrstr(type, "x86_pkg_temp") ||
            g_strrstr(type, "Tctl") || g_strrstr(type, "k10temp")) {
            gchar *end = NULL;
            double raw = g_ascii_strtod(value, &end);
            if (end != value) {
                if (fabs(raw) > 1000.0)
                    raw /= 1000.0;
                *out_c = raw;
                g_free(type);
                g_free(value);
                return TRUE;
            }
        }

        g_free(type);
        g_free(value);
    }
    return FALSE;
}

static void fan_update_labels(FanPage *p)
{
    g_autofree gchar *temp_text = g_strdup_printf("%.0f°C", p->current_temp);
    gtk_label_set_text(p->temp_label, temp_text);
    gtk_label_set_text(p->target_label, FAN_MODE_TITLE[p->mode]);

    if (p->rpm_valid) {
        g_autofree gchar *rpm_text = g_strdup_printf("%.0f RPM", p->reported_rpm);
        gtk_label_set_text(p->rpm_label, rpm_text);
    } else {
        gtk_label_set_text(p->rpm_label, "RPM —");
    }

    double display_speed = p->target_percent <= 20.0
        ? 0.15 : 0.15 + pow(p->target_percent / 100.0, 1.45) * 8.0;
    display_speed *= p->anim_speed;
    g_autofree gchar *speed_text = g_strdup_printf("%.1fx visual speed", display_speed);
    gtk_label_set_text(p->level_label, speed_text);

    const char *msg = p->status ? p->status
        : p->backend_available ? "HP WMI platform profile — changes ask for authorization"
        : "Preview mode — HP WMI platform profile not found";
    gtk_label_set_text(GTK_LABEL(p->backend_label), msg);
}

typedef struct {
    FanPage *p;
    GCancellable *cancel;
} ApplyCtx;

static void fan_set_status(FanPage *p, gchar *text)
{
    g_free(p->status);
    p->status = text;
}

static void on_apply_done(GObject *src, GAsyncResult *res, gpointer data)
{
    ApplyCtx *ctx = data;
    GSubprocess *proc = G_SUBPROCESS(src);
    GError *err = NULL;
    gboolean waited = g_subprocess_wait_finish(proc, res, &err);

    if (!g_cancellable_is_cancelled(ctx->cancel)) {
        FanPage *p = ctx->p;
        p->apply_pending = FALSE;
        if (waited && g_subprocess_get_successful(proc))
            fan_set_status(p, g_strdup_printf("Profile set to %s", FAN_MODE_TITLE[p->mode]));
        else
            fan_set_status(p, g_strdup("Profile change failed or authorization was cancelled"));
        fan_update_labels(p);
    }
    g_clear_error(&err);
    g_object_unref(ctx->cancel);
    g_free(ctx);
}

static gboolean fan_apply_timeout(gpointer data)
{
    FanPage *p = data;
    p->apply_timer = 0;
    if (p->apply_pending) {
        p->apply_timer = g_timeout_add(300, fan_apply_timeout, p);
        return G_SOURCE_REMOVE;
    }

    const char *argv[] = {
        "pkexec", "sh", "-c", "printf '%s' \"$1\" > \"$2\"", "fan-profile",
        FAN_MODE_PROFILE[p->mode], FAN_PROFILE_PATH, NULL
    };
    GError *err = NULL;
    GSubprocess *proc = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDERR_SILENCE, &err);
    if (!proc) {
        fan_set_status(p, g_strdup_printf("Could not run pkexec: %s", err->message));
        g_error_free(err);
        fan_update_labels(p);
        return G_SOURCE_REMOVE;
    }

    p->apply_pending = TRUE;
    ApplyCtx *ctx = g_new0(ApplyCtx, 1);
    ctx->p = p;
    ctx->cancel = g_object_ref(p->cancel);
    g_subprocess_wait_async(proc, p->cancel, on_apply_done, ctx);
    g_object_unref(proc);
    return G_SOURCE_REMOVE;
}

/* apply == TRUE: user action, write to hardware (debounced).
 * apply == FALSE: just mirror what the hardware already reports. */
static void fan_set_mode(FanPage *p, FanMode mode, gboolean apply)
{
    p->mode = mode;
    p->syncing = TRUE;
    gtk_drop_down_set_selected(p->mode_drop, (guint)mode);
    gtk_range_set_value(GTK_RANGE(p->level_scale), (double)mode);
    p->syncing = FALSE;

    if (apply && p->backend_available) {
        fan_set_status(p, NULL);
        if (p->apply_timer)
            g_source_remove(p->apply_timer);
        p->apply_timer = g_timeout_add(FAN_APPLY_DELAY_MS, fan_apply_timeout, p);
    }
    p->target_percent = fan_compute_target(p);
    fan_update_labels(p);
}




static void on_mode_changed(GObject *obj, GParamSpec *pspec, gpointer user_data)
{
    (void)pspec;
    FanPage *p = user_data;
    if (p->syncing)
        return;
    guint index = gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
    if (index < FAN_MODE_COUNT && (FanMode)index != p->mode)
        fan_set_mode(p, (FanMode)index, TRUE);
}

static void on_level_changed(GtkRange *range, gpointer user_data)
{
    FanPage *p = user_data;
    if (p->syncing)
        return;
    int index = (int)lround(gtk_range_get_value(range));
    if (index >= 0 && index < FAN_MODE_COUNT && (FanMode)index != p->mode)
        fan_set_mode(p, (FanMode)index, TRUE);
}

static void on_anim_changed(GtkRange *range, gpointer user_data)
{
    FanPage *p = user_data;
    p->anim_speed = gtk_range_get_value(range);
    fan_update_labels(p);
}

static void on_fan_changed(GObject *obj, GParamSpec *pspec, gpointer user_data)
{
    (void)pspec;
    FanPage *p = user_data;
    p->selected_fan = gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
    p->rpm_valid = fan_read_rpm(p, p->selected_fan, &p->reported_rpm);
    if (!p->rpm_valid)
        p->reported_rpm = 0.0;
    p->target_percent = fan_compute_target(p);
    fan_update_labels(p);
}


static GtkWidget *make_metric_card(const char *title, GtkWidget **value_out)
{
    GtkWidget *frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(frame, "card");
    gtk_widget_add_css_class(frame, "fan-card");
    gtk_widget_set_margin_start(frame, 2);
    gtk_widget_set_margin_end(frame, 2);
    gtk_widget_set_margin_top(frame, 2);
    gtk_widget_set_margin_bottom(frame, 2);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_set_margin_start(box, 14);
    gtk_widget_set_margin_end(box, 14);
    gtk_widget_set_margin_top(box, 11);
    gtk_widget_set_margin_bottom(box, 11);
    gtk_box_append(GTK_BOX(frame), box);

    GtkWidget *title_label = gtk_label_new(title);
    gtk_widget_set_halign(title_label, GTK_ALIGN_START);
    gtk_widget_add_css_class(title_label, "dim-label");
    gtk_box_append(GTK_BOX(box), title_label);

    GtkWidget *value = gtk_label_new("—");
    gtk_widget_set_halign(value, GTK_ALIGN_START);
    gtk_widget_add_css_class(value, "fan-value");
    gtk_box_append(GTK_BOX(box), value);
    *value_out = value;
    return frame;
}

static GtkWidget *make_settings_row(const char *title, GtkWidget *control)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_hexpand(row, TRUE);
    gtk_widget_set_margin_top(row, 4);
    gtk_widget_set_margin_bottom(row, 4);

    GtkWidget *label = gtk_label_new(title);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_box_append(GTK_BOX(row), label);

    gtk_widget_set_size_request(control, 180, -1);
    gtk_box_append(GTK_BOX(row), control);
    return row;
}


static GtkWidget *create_control_panel(FanPage *p)
{
    GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_set_size_request(panel, 355, -1);

    GtkWidget *heading = gtk_label_new("Cooling control");
    gtk_widget_set_halign(heading, GTK_ALIGN_START);
    gtk_widget_add_css_class(heading, "fan-section-title");
    gtk_box_append(GTK_BOX(panel), heading);

    const char *modes[] = { "Quiet", "Balanced", "Performance", "Cool", NULL };
    p->mode_drop = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(modes));
    gtk_drop_down_set_selected(p->mode_drop, (guint)p->mode);
    gtk_box_append(GTK_BOX(panel), make_settings_row("Profile", GTK_WIDGET(p->mode_drop)));
    g_signal_connect(p->mode_drop, "notify::selected", G_CALLBACK(on_mode_changed), p);

    GtkWidget *level_title = gtk_label_new("Fan speed level");
    gtk_widget_set_halign(level_title, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(panel), level_title);

    p->level_scale = GTK_SCALE(gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,
                                                         0, FAN_MODE_COUNT - 1, 1));
    gtk_scale_set_digits(p->level_scale, 0);
    gtk_scale_set_draw_value(p->level_scale, FALSE);
    gtk_range_set_round_digits(GTK_RANGE(p->level_scale), 0);
    gtk_widget_set_hexpand(GTK_WIDGET(p->level_scale), TRUE);
    for (int i = 0; i < FAN_MODE_COUNT; ++i)
        gtk_scale_add_mark(p->level_scale, i, GTK_POS_BOTTOM, FAN_MODE_TITLE[i]);
    gtk_range_set_value(GTK_RANGE(p->level_scale), (double)p->mode);
    gtk_box_append(GTK_BOX(panel), GTK_WIDGET(p->level_scale));
    g_signal_connect(p->level_scale, "value-changed", G_CALLBACK(on_level_changed), p);

    GtkWidget *anim_title = gtk_label_new("Rotor animation speed (visual only)");
    gtk_widget_set_halign(anim_title, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(panel), anim_title);

    p->anim_scale = GTK_SCALE(gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,
                                                        0.0, 3.0, 0.1));
    gtk_scale_set_digits(p->anim_scale, 1);
    gtk_scale_set_draw_value(p->anim_scale, TRUE);
    gtk_scale_add_mark(p->anim_scale, 1.0, GTK_POS_BOTTOM, "1x");
    gtk_widget_set_hexpand(GTK_WIDGET(p->anim_scale), TRUE);
    gtk_range_set_value(GTK_RANGE(p->anim_scale), p->anim_speed);
    gtk_box_append(GTK_BOX(panel), GTK_WIDGET(p->anim_scale));
    g_signal_connect(p->anim_scale, "value-changed", G_CALLBACK(on_anim_changed), p);

    const char *fans[] = { "Fan 1", "Fan 2", NULL };
    p->fan_drop = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(fans));
    p->selected_fan = 0;
    gtk_drop_down_set_selected(p->fan_drop, 0);
    gtk_widget_set_visible(GTK_WIDGET(p->fan_drop), p->fan_count > 1);
    gtk_box_append(GTK_BOX(panel), make_settings_row("Fan", GTK_WIDGET(p->fan_drop)));
    g_signal_connect(p->fan_drop, "notify::selected", G_CALLBACK(on_fan_changed), p);

    GtkWidget *safe = gtk_label_new(
        "This laptop has no manual fan duty-cycle control. Speed is set through the "
        "firmware profile; Cool is the highest hardware level. Changing it asks for "
        "administrator authorization.");
    gtk_label_set_wrap(GTK_LABEL(safe), TRUE);
    gtk_widget_set_halign(safe, GTK_ALIGN_START);
    gtk_widget_add_css_class(safe, "fan-muted");
    gtk_box_append(GTK_BOX(panel), safe);

    return panel;
}

static gboolean animation_tick(gpointer user_data)
{
    FanPage *p = user_data;
    gint64 now = g_get_monotonic_time();
    if (p->last_frame_us == 0)
        p->last_frame_us = now;

    double dt = (now - p->last_frame_us) / 1000000.0;
    p->last_frame_us = now;
    dt = CLAMP(dt, 0.0, 0.1);

    /* Non-linear visual mapping: low settings are intentionally slow,
     * higher settings accelerate sharply for a convincing fan effect. */
    double turns_per_sec = p->target_percent <= TARGET_MIN_PERCENT
        ? 0.08
        : 0.08 + pow(p->target_percent / 100.0, 1.55) * 11.5;
    turns_per_sec *= p->anim_speed;
    p->angle += turns_per_sec * 2.0 * G_PI * dt;
    if (p->angle > 2.0 * G_PI)
        p->angle = fmod(p->angle, 2.0 * G_PI);

    if (p->gl_area && p->gl_ready)
        gtk_gl_area_queue_render(p->gl_area);
    return G_SOURCE_CONTINUE;
}

static gboolean sample_tick(gpointer user_data)
{
    FanPage *p = user_data;
    double temp = 0.0;
    if (read_cpu_temperature(&temp))
        p->current_temp = temp;
    else
        p->current_temp = 50.0;

    double rpm = 0.0;
    p->rpm_valid = fan_read_rpm(p, p->selected_fan, &rpm);
    p->reported_rpm = p->rpm_valid ? rpm : 0.0;

    /* Mirror external changes (Fn key, other tools) and revert the UI
     * if an authorization was cancelled. */
    FanMode actual;
    if (p->backend_available && !p->apply_pending && !p->apply_timer &&
        fan_read_profile(&actual) && actual != p->mode)
        fan_set_mode(p, actual, FALSE);

    p->target_percent = fan_compute_target(p);
    fan_update_labels(p);
    return G_SOURCE_CONTINUE;
}

static void on_gl_realize(GtkGLArea *area, gpointer user_data)
{
    FanPage *p = user_data;
    gtk_gl_area_make_current(area);
    if (gtk_gl_area_get_error(area) != NULL)
        return;

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glDepthFunc(GL_LEQUAL);

    GError *error = NULL;
    if (!fan_upload_model(p, &error)) {
        if (error) {
            p->model_error = g_error_copy(error);
            gtk_gl_area_set_error(area, error);
        }
        return;
    }

    p->gl_ready = TRUE;
}

static void on_gl_unrealize(GtkGLArea *area, gpointer user_data)
{
    FanPage *p = user_data;
    gtk_gl_area_make_current(area);
    fan_delete_gl(p);
    fan_model_clear(&p->model);
}

static gboolean on_gl_render(GtkGLArea *area, GdkGLContext *context,
                             gpointer user_data)
{
    (void)context;
    FanPage *p = user_data;
    if (!p->model_ready || !p->program)
        return TRUE;

    int width = gtk_widget_get_width(GTK_WIDGET(area));
    int height = gtk_widget_get_height(GTK_WIDGET(area));
    if (width <= 0 || height <= 0)
        return TRUE;

    glViewport(0, 0, width, height);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    float aspect = (float)width / (float)height;
    Mat4 projection = mat4_perspective(45.0f * (float)G_PI / 180.0f,
                                       MAX(aspect, 0.1f), 0.05f, 50.0f);
    Mat4 view = mat4_translate(0.0f, 0.0f, -7.0f);
    Mat4 model = mat4_identity();
    model = mat4_mul(model, mat4_rotate_z((float)p->angle));
    model = mat4_mul(model, mat4_rotate_x((float)(-G_PI / 2.0)));
    model = mat4_mul(model, mat4_scale(p->model.scale));
    model = mat4_mul(model, mat4_translate(-p->model.center[0],
                                           -p->model.center[1],
                                           -p->model.center[2]));

    Mat4 mvp = mat4_mul(projection, mat4_mul(view, model));

    glUseProgram(p->program);
    glUniformMatrix4fv(p->u_mvp, 1, GL_FALSE, mvp.m);
    glUniformMatrix4fv(p->u_model, 1, GL_FALSE, model.m);
    glUniform1i(p->u_sampler, 0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, p->texture);
    glBindVertexArray(p->vao);
    glDrawElements(GL_TRIANGLES, (GLsizei)p->model.index_count,
                   GL_UNSIGNED_INT, NULL);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);

    return TRUE;
}

static void add_css(void)
{
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider, FAN_CSS, -1);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

static void fan_page_destroy(gpointer data)
{
    FanPage *p = data;
    if (p->animation_source)
        g_source_remove(p->animation_source);
    if (p->sample_source)
        g_source_remove(p->sample_source);
    if (p->apply_timer)
        g_source_remove(p->apply_timer);
    g_cancellable_cancel(p->cancel);
    g_object_unref(p->cancel);
    g_free(p->hwmon_dir);
    g_free(p->status);
    fan_model_clear(&p->model);
    g_clear_error(&p->model_error);
    g_free(p);
}

GtkWidget *fan_control_page_create(App *app)
{
    add_css();

    FanPage *p = g_new0(FanPage, 1);
    p->app = app;
    p->cancel = g_cancellable_new();
    p->anim_speed = 1.0;
    p->current_temp = 50.0;
    p->hwmon_dir = fan_find_hwmon();
    for (unsigned i = 0; i < 2; ++i) {
        double r;
        if (fan_read_rpm(p, i, &r))
            p->fan_count++;
    }
    p->backend_available = g_file_test(FAN_PROFILE_PATH, G_FILE_TEST_EXISTS);
    FanMode initial;
    p->mode = fan_read_profile(&initial) ? initial : FAN_MODE_BALANCED;
    p->rpm_valid = fan_read_rpm(p, 0, &p->reported_rpm);
    p->target_percent = fan_compute_target(p);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    p->root = root;
    gtk_widget_add_css_class(root, "fan-page");
    gtk_widget_set_hexpand(root, TRUE);
    gtk_widget_set_vexpand(root, TRUE);
    g_object_set_data_full(G_OBJECT(root), "fan-page-state", p, fan_page_destroy);

    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    GtkWidget *title = gtk_label_new("Fan Control");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_widget_add_css_class(title, "fan-title");
    gtk_box_append(GTK_BOX(header), title);

    GtkWidget *subtitle = gtk_label_new(
        "Live rotor visualization with HP firmware cooling profiles.");
    gtk_widget_set_halign(subtitle, GTK_ALIGN_START);
    gtk_widget_add_css_class(subtitle, "fan-subtitle");
    gtk_box_append(GTK_BOX(header), subtitle);
    gtk_box_append(GTK_BOX(root), header);

    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    gtk_widget_set_hexpand(content, TRUE);
    gtk_widget_set_vexpand(content, TRUE);

    GtkWidget *left = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(left),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(left, 390, -1);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(left), create_control_panel(p));
    gtk_box_append(GTK_BOX(content), left);

    GtkWidget *stage_overlay = gtk_overlay_new();
    gtk_widget_set_hexpand(stage_overlay, TRUE);
    gtk_widget_set_vexpand(stage_overlay, TRUE);
    gtk_widget_add_css_class(stage_overlay, "fan-stage");

    p->gl_area = GTK_GL_AREA(gtk_gl_area_new());
    gtk_gl_area_set_has_depth_buffer(p->gl_area, TRUE);
    gtk_gl_area_set_required_version(p->gl_area, 3, 3);
    gtk_gl_area_set_auto_render(p->gl_area, FALSE);
    gtk_widget_set_hexpand(GTK_WIDGET(p->gl_area), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(p->gl_area), TRUE);
    gtk_overlay_set_child(GTK_OVERLAY(stage_overlay), GTK_WIDGET(p->gl_area));

    GtkWidget *stage_title = gtk_label_new("Cooling rotor");
    gtk_widget_set_halign(stage_title, GTK_ALIGN_START);
    gtk_widget_set_valign(stage_title, GTK_ALIGN_START);
    gtk_widget_add_css_class(stage_title, "fan-stage-label");
    gtk_overlay_add_overlay(GTK_OVERLAY(stage_overlay), stage_title);

    GtkWidget *status = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    gtk_widget_set_halign(status, GTK_ALIGN_END);
    gtk_widget_set_valign(status, GTK_ALIGN_START);
    gtk_widget_set_margin_top(status, 14);
    gtk_widget_set_margin_end(status, 16);

    GtkWidget *temp_box = make_metric_card("Temperature", (GtkWidget **)&p->temp_label);
    GtkWidget *target_box = make_metric_card("Profile", (GtkWidget **)&p->target_label);
    GtkWidget *rpm_box = make_metric_card("Fan", (GtkWidget **)&p->rpm_label);
    gtk_box_append(GTK_BOX(status), temp_box);
    gtk_box_append(GTK_BOX(status), target_box);
    gtk_box_append(GTK_BOX(status), rpm_box);
    gtk_overlay_add_overlay(GTK_OVERLAY(stage_overlay), status);

    GtkWidget *bottom = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_set_halign(bottom, GTK_ALIGN_START);
    gtk_widget_set_valign(bottom, GTK_ALIGN_END);
    gtk_widget_set_margin_start(bottom, 16);
    gtk_widget_set_margin_bottom(bottom, 12);

    p->level_label = GTK_LABEL(gtk_label_new("0.0x visual speed"));
    gtk_widget_add_css_class(GTK_WIDGET(p->level_label), "fan-stage-label");
    gtk_box_append(GTK_BOX(bottom), GTK_WIDGET(p->level_label));

    p->backend_label = gtk_label_new("");
    gtk_widget_add_css_class(p->backend_label, "fan-stage-status");
    gtk_box_append(GTK_BOX(bottom), p->backend_label);
    gtk_overlay_add_overlay(GTK_OVERLAY(stage_overlay), bottom);

    gtk_box_append(GTK_BOX(content), stage_overlay);
    gtk_box_append(GTK_BOX(root), content);

    g_signal_connect(p->gl_area, "realize", G_CALLBACK(on_gl_realize), p);
    g_signal_connect(p->gl_area, "unrealize", G_CALLBACK(on_gl_unrealize), p);
    g_signal_connect(p->gl_area, "render", G_CALLBACK(on_gl_render), p);

    p->target_percent = fan_compute_target(p);
    fan_update_labels(p);
    p->animation_source = g_timeout_add(ANIMATION_MS, animation_tick, p);
    p->sample_source = g_timeout_add(SAMPLE_MS, sample_tick, p);

    return root;
}
