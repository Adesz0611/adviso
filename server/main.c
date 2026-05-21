#define CFD_LIB_IMPLEMENTATION
#include "ensight_gold.h"
#include "cfd_render.h"
#include "cfd_slicer.h"

#include "mongoose.h"

#include <float.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

#define PUBLIC_DIR "frontend"
#define LISTEN_URL "http://0.0.0.0:6969"

#define CASE_ARENA_SIZE GB(16)
#define MESH_ARENA_SIZE GB(16)
#define SCRATCH_ARENA_SIZE GB(16)
#define RESPONSE_ARENA_SIZE GB(16)

#define DEFAULT_SLICE_RESOLUTION 512U
#define MESH_PACKET_MAGIC 0x53564441u
#define MESH_PACKET_VERSION 1u

typedef enum Mesh_Packet_Type {
    MESH_PACKET_SURFACE = 1,
    MESH_PACKET_SLICE = 2,
} Mesh_Packet_Type;

typedef enum Slice_Axis {
    SLICE_AXIS_X = 0,
    SLICE_AXIS_Y = 1,
    SLICE_AXIS_Z = 2,
} Slice_Axis;

typedef struct Mesh_Packet_Header {
    u32 magic;
    u32 version;
    u32 payload_type;
    u32 vertex_count;
    u32 index_count;
    u32 triangle_count;
    f32 bounds_min[3];
    f32 bounds_max[3];
    f32 scalar_min;
    f32 scalar_max;
} Mesh_Packet_Header;

typedef enum Async_Job_Type {
    ASYNC_JOB_OPEN_CASE = 1,
    ASYNC_JOB_SURFACE = 2,
    ASYNC_JOB_SLICE = 3,
} Async_Job_Type;

typedef struct Async_Job_Request {
    struct WS_Client *cli;
    Async_Job_Type type;
    s32 variable_idx;
    u32 request_time_idx;
    Slice_Axis slice_axis;
    f32 slice_value;
    u32 resolution;
    char case_path[PATH_MAX];
} Async_Job_Request;

typedef struct Async_Job_Result {
    struct WS_Client *cli;
    Async_Job_Type type;
    b32 success;
    char error_message[256];
    char *text_payload;
    u64 text_payload_len;
    u8 *binary_payload;
    u64 binary_payload_len;
} Async_Job_Result;

typedef struct WS_Client {
    pthread_mutex_t mutex;
    CFD_Arena case_arena;
    CFD_Arena mesh_arena;
    CFD_Arena scratch_arena;
    CFD_Arena response_arena;

    Ensight_Case encase;
    CFD_UnstructuredGrid mesh;
    CFD_BVH8Tree bvh;

    u64 user_id;
    struct mg_mgr *mgr;
    _Atomic u32 ref_count;
    u32 loaded_mesh_time_idx;
    b32 conn_open;
    b32 job_in_progress;
    b32 has_case;
    b32 has_mesh;
    char case_path[PATH_MAX];
} WS_Client;

static unsigned long g_wakeup_conn_id = 0;

static WS_Client *ws_client_get(struct mg_connection *c) {
    return (WS_Client *) c->fn_data;
}

static void ws_client_retain(WS_Client *cli) {
    atomic_fetch_add(&cli->ref_count, 1u);
}

static void ws_client_release(WS_Client *cli) {
    if (atomic_fetch_sub(&cli->ref_count, 1u) != 1u) return;

    pthread_mutex_destroy(&cli->mutex);
    cfd_arena_destroy(&cli->response_arena);
    cfd_arena_destroy(&cli->scratch_arena);
    cfd_arena_destroy(&cli->mesh_arena);
    cfd_arena_destroy(&cli->case_arena);
    free(cli);
}

static b32 ws_client_begin_job(WS_Client *cli) {
    b32 ok = false;
    pthread_mutex_lock(&cli->mutex);
    if (cli->conn_open && !cli->job_in_progress) {
        cli->job_in_progress = true;
        ok = true;
    }
    pthread_mutex_unlock(&cli->mutex);
    return ok;
}

static void ws_client_finish_job(WS_Client *cli) {
    pthread_mutex_lock(&cli->mutex);
    cli->job_in_progress = false;
    pthread_mutex_unlock(&cli->mutex);
}

static b32 ws_client_has_case(WS_Client *cli) {
    b32 has_case;
    pthread_mutex_lock(&cli->mutex);
    has_case = cli->has_case;
    pthread_mutex_unlock(&cli->mutex);
    return has_case;
}

static b32 ws_client_init(WS_Client *cli) {
    memset(cli, 0, sizeof(*cli));

    if (pthread_mutex_init(&cli->mutex, NULL) != 0) return false;
    atomic_init(&cli->ref_count, 1u);
    cli->conn_open = true;

    if (unlikely(!cfd_arena_init(&cli->case_arena, CASE_ARENA_SIZE))) goto fail;
    if (unlikely(!cfd_arena_init(&cli->mesh_arena, MESH_ARENA_SIZE))) goto fail;
    if (unlikely(!cfd_arena_init(&cli->scratch_arena, SCRATCH_ARENA_SIZE))) goto fail;
    if (unlikely(!cfd_arena_init(&cli->response_arena, RESPONSE_ARENA_SIZE))) goto fail;

    return true;

fail:
    pthread_mutex_destroy(&cli->mutex);
    cfd_arena_destroy(&cli->response_arena);
    cfd_arena_destroy(&cli->scratch_arena);
    cfd_arena_destroy(&cli->mesh_arena);
    cfd_arena_destroy(&cli->case_arena);
    return false;
}

static void ws_client_reset_state(WS_Client *cli) {
    cfd_arena_reset(&cli->case_arena);
    cfd_arena_reset(&cli->mesh_arena);
    cfd_arena_reset(&cli->scratch_arena);
    cfd_arena_reset(&cli->response_arena);

    memset(&cli->encase, 0, sizeof(cli->encase));
    memset(&cli->mesh, 0, sizeof(cli->mesh));
    memset(&cli->bvh, 0, sizeof(cli->bvh));
    memset(cli->case_path, 0, sizeof(cli->case_path));

    cli->loaded_mesh_time_idx = 0;
    cli->has_case = false;
    cli->has_mesh = false;
}

static void ws_client_destroy(struct mg_connection *c) {
    WS_Client *cli = ws_client_get(c);
    if (cli == NULL) return;

    pthread_mutex_lock(&cli->mutex);
    cli->conn_open = false;
    pthread_mutex_unlock(&cli->mutex);

    c->fn_data = NULL;
    ws_client_release(cli);
}

static b32 json_append(char *buffer, u64 cap, u64 *offset, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const size_t written = mg_vsnprintf(buffer + *offset, (size_t) (cap - *offset), fmt, &args);
    va_end(args);

    if (*offset + (u64) written >= cap) return false;
    *offset += (u64) written;
    return true;
}

static b32 json_append_escaped_str(char *buffer, u64 cap, u64 *offset, Str8 value) {
    if (*offset >= cap) return false;
    buffer[(*offset)++] = '"';

    for (u64 i = 0; i < value.len; ++i) {
        const u8 ch = value.buffer[i];

        switch (ch) {
            case '"':
            case '\\':
                if (*offset + 2 >= cap) return false;
                buffer[(*offset)++] = '\\';
                buffer[(*offset)++] = (char) ch;
                break;
            case '\b':
                if (*offset + 2 >= cap) return false;
                buffer[(*offset)++] = '\\';
                buffer[(*offset)++] = 'b';
                break;
            case '\f':
                if (*offset + 2 >= cap) return false;
                buffer[(*offset)++] = '\\';
                buffer[(*offset)++] = 'f';
                break;
            case '\n':
                if (*offset + 2 >= cap) return false;
                buffer[(*offset)++] = '\\';
                buffer[(*offset)++] = 'n';
                break;
            case '\r':
                if (*offset + 2 >= cap) return false;
                buffer[(*offset)++] = '\\';
                buffer[(*offset)++] = 'r';
                break;
            case '\t':
                if (*offset + 2 >= cap) return false;
                buffer[(*offset)++] = '\\';
                buffer[(*offset)++] = 't';
                break;
            default:
                if (ch < 0x20) {
                    if (*offset + 6 >= cap) return false;
                    buffer[(*offset)++] = '\\';
                    buffer[(*offset)++] = 'u';
                    buffer[(*offset)++] = '0';
                    buffer[(*offset)++] = '0';
                    buffer[(*offset)++] = "0123456789abcdef"[(ch >> 4) & 0xF];
                    buffer[(*offset)++] = "0123456789abcdef"[ch & 0xF];
                } else {
                    if (*offset + 1 >= cap) return false;
                    buffer[(*offset)++] = (char) ch;
                }
                break;
        }
    }

    if (*offset >= cap) return false;
    buffer[(*offset)++] = '"';
    if (*offset >= cap) return false;
    buffer[*offset] = '\0';
    return true;
}

static void ws_send_json_message(struct mg_connection *c, const char *type, const char *message) {
    char buffer[1024];
    u64 off = 0;
    Str8 message_str = { (u8 *) message, (u64) strlen(message) };

    if (!json_append(buffer, sizeof(buffer), &off, "{\"type\":\"%s\",\"message\":", type)) return;
    if (!json_append_escaped_str(buffer, sizeof(buffer), &off, message_str)) return;
    if (!json_append(buffer, sizeof(buffer), &off, "}")) return;

    mg_ws_send(c, buffer, off, WEBSOCKET_OP_TEXT);
}

static void ws_send_error(struct mg_connection *c, const char *message) {
    ws_send_json_message(c, "error", message);
}

static void ws_send_status(struct mg_connection *c, const char *message) {
    ws_send_json_message(c, "status", message);
}

static u32 get_geometry_time_count(const Ensight_Case *encase) {
    if (encase == NULL || encase->geometry == NULL || encase->geometry->model == NULL) return 1;
    if (encase->geometry->model->ts == -1 || encase->time == NULL) return 1;

    s32 time_set_idx = ensight_get_time_set_index(encase, encase->geometry->model->ts);
    if (time_set_idx < 0) return 1;

    return encase->time->elems[time_set_idx].number_of_steps;
}

static u32 get_variable_time_count(const Ensight_Case *encase, u32 variable_idx) {
    if (encase == NULL || encase->variable == NULL || variable_idx >= encase->variable->len) return 1;

    Ensight_Variable *var = &encase->variable->elems[variable_idx];
    if (var->ts == -1 || encase->time == NULL) return 1;

    s32 time_set_idx = ensight_get_time_set_index(encase, var->ts);
    if (time_set_idx < 0) return 1;

    return encase->time->elems[time_set_idx].number_of_steps;
}

static b32 parse_slice_axis(const char *axis_str, Slice_Axis *out_axis) {
    if (axis_str == NULL || out_axis == NULL) return false;

    if (strcmp(axis_str, "x") == 0 || strcmp(axis_str, "X") == 0) {
        *out_axis = SLICE_AXIS_X;
        return true;
    }

    if (strcmp(axis_str, "y") == 0 || strcmp(axis_str, "Y") == 0) {
        *out_axis = SLICE_AXIS_Y;
        return true;
    }

    if (strcmp(axis_str, "z") == 0 || strcmp(axis_str, "Z") == 0) {
        *out_axis = SLICE_AXIS_Z;
        return true;
    }

    return false;
}

static f32 scalar_from_values(const f32 *values, u64 idx, u8 dim) {
    if (dim == 1) return values[idx];

    const f32 x = values[idx * 3 + 0];
    const f32 y = values[idx * 3 + 1];
    const f32 z = values[idx * 3 + 2];
    return sqrtf(x * x + y * y + z * z);
}

static void compute_face_normals(const f32 *positions, const u32 *indices, u32 triangle_count, f32 *out_normals) {
    for (u32 tri_idx = 0; tri_idx < triangle_count; ++tri_idx) {
        const u32 i0 = indices[tri_idx * 3 + 0];
        const u32 i1 = indices[tri_idx * 3 + 1];
        const u32 i2 = indices[tri_idx * 3 + 2];

        V3 v0 = {
            positions[i0 * 3 + 0],
            positions[i0 * 3 + 1],
            positions[i0 * 3 + 2],
        };
        V3 v1 = {
            positions[i1 * 3 + 0],
            positions[i1 * 3 + 1],
            positions[i1 * 3 + 2],
        };
        V3 v2 = {
            positions[i2 * 3 + 0],
            positions[i2 * 3 + 1],
            positions[i2 * 3 + 2],
        };

        V3 e0 = v3_sub(v1, v0);
        V3 e1 = v3_sub(v2, v0);
        V3 n = v3_cross(e0, e1);

        const f32 len_sq = v3_length_sq(n);
        if (len_sq > 0.0f) {
            const f32 inv_len = 1.0f / sqrtf(len_sq);
            n = v3_scale(n, inv_len);
        } else {
            n = v3_zero();
        }

        out_normals[tri_idx * 3 + 0] = n.x;
        out_normals[tri_idx * 3 + 1] = n.y;
        out_normals[tri_idx * 3 + 2] = n.z;
    }
}

static void compute_scalar_range(const f32 *scalars, u32 scalar_count, f32 *out_min, f32 *out_max) {
    f32 min_value = FLT_MAX;
    f32 max_value = -FLT_MAX;

    for (u32 i = 0; i < scalar_count; ++i) {
        const f32 value = scalars[i];
        if (value < min_value) min_value = value;
        if (value > max_value) max_value = value;
    }

    if (scalar_count == 0) {
        min_value = 0.0f;
        max_value = 1.0f;
    } else if (min_value == max_value) {
        max_value = min_value + 1.0f;
    }

    *out_min = min_value;
    *out_max = max_value;
}

static void set_error_message(char *buffer, u64 buffer_size, const char *message) {
    if (buffer == NULL || buffer_size == 0) return;
    snprintf(buffer, buffer_size, "%s", message != NULL ? message : "Unknown error.");
}

static b32 ensure_model_info(WS_Client *cli, u32 request_time_idx) {
    Ensight_GeometryElem *model = cli->encase.geometry->model;
    const u32 model_info_idx = (model->ts == -1) ? 0 : request_time_idx;
    if (model->model_info_array[model_info_idx] != NULL) return true;

    u8 filename_buffer[PATH_MAX];
    if (unlikely(ensight_get_geometry_model_filename(&cli->encase, request_time_idx, filename_buffer, PATH_MAX) == 0)) {
        return false;
    }

    CFD_File geo_file;
    if (unlikely(!cfd_file_slurp((char *) filename_buffer, &geo_file))) return false;

    cfd_arena_reset(&cli->scratch_arena);

    Ensight_Model_Info *model_info = cfd_arena_push_type_zero(&cli->case_arena, Ensight_Model_Info);
    if (unlikely(model_info == NULL)) {
        cfd_file_free(&geo_file);
        return false;
    }

    const b32 ok = ensight_parse_geometry_model_info(&cli->case_arena, &cli->scratch_arena, &cli->encase, model_info, &geo_file);
    cfd_file_free(&geo_file);
    if (!ok) return false;

    model->model_info_array[model_info_idx] = model_info;
    return true;
}

static b32 load_mesh_for_time(WS_Client *cli, u32 request_time_idx) {
    Ensight_GeometryElem *model = cli->encase.geometry->model;
    const u32 mesh_time_idx = (model->ts == -1) ? 0 : request_time_idx;
    if (cli->has_mesh && cli->loaded_mesh_time_idx == mesh_time_idx) return true;

    if (unlikely(!ensure_model_info(cli, request_time_idx))) return false;

    u8 filename_buffer[PATH_MAX];
    if (unlikely(ensight_get_geometry_model_filename(&cli->encase, request_time_idx, filename_buffer, PATH_MAX) == 0)) {
        return false;
    }

    CFD_File geo_file;
    if (unlikely(!cfd_file_slurp((char *) filename_buffer, &geo_file))) return false;

    cfd_arena_reset(&cli->mesh_arena);
    cfd_arena_reset(&cli->scratch_arena);

    const u32 model_info_idx = (model->ts == -1) ? 0 : request_time_idx;
    Ensight_Model_Info *model_info = model->model_info_array[model_info_idx];

    const b32 ok = ensight_parse_model_merge_parts(&cli->mesh_arena, &cli->encase, model_info, &geo_file, &cli->mesh);
    cfd_file_free(&geo_file);
    if (!ok) return false;

    cli->bvh = cfd_build_grid_bvh8(&cli->mesh_arena, &cli->scratch_arena, &cli->mesh);
    cli->loaded_mesh_time_idx = mesh_time_idx;
    cli->has_mesh = true;
    return true;
}

static b32 load_node_data(WS_Client *cli, s32 variable_idx, u32 request_time_idx, u8 *out_dim, f32 **out_node_data) {
    if (out_dim == NULL || out_node_data == NULL) return false;

    cfd_arena_reset(&cli->scratch_arena);

    if (variable_idx < 0 || cli->encase.variable == NULL || (u32) variable_idx >= cli->encase.variable->len) {
        f32 *zeros = cfd_arena_push_array_zero(&cli->response_arena, f32, cli->mesh.num_vertices);
        if (unlikely(zeros == NULL)) return false;

        *out_dim = 1;
        *out_node_data = zeros;
        return true;
    }

    Ensight_Variable *var = &cli->encase.variable->elems[variable_idx];
    const u8 dim = (var->type == ENSIGHT_VARIABLE_SCALAR_PER_NODE || var->type == ENSIGHT_VARIABLE_SCALAR_PER_ELEMENT) ? 1 : 3;
    const u32 variable_time_idx = (var->ts == -1) ? 0 : request_time_idx;

    f32 *var_data = ensight_read_variable(&cli->response_arena, &cli->encase, (u32) variable_idx, variable_time_idx);
    if (unlikely(var_data == NULL)) return false;

    f32 *node_data = var_data;
    if (var->type == ENSIGHT_VARIABLE_SCALAR_PER_ELEMENT || var->type == ENSIGHT_VARIABLE_VECTOR_PER_ELEMENT) {
        node_data = cfd_cell_data_to_node_data(&cli->response_arena, &cli->scratch_arena, &cli->mesh, var_data, dim);
        if (unlikely(node_data == NULL)) return false;
    }

    *out_dim = dim;
    *out_node_data = node_data;
    return true;
}

static b32 generate_axis_aligned_plane_mesh(
    Slice_Axis axis,
    V3 aabb_min,
    V3 aabb_max,
    f32 value,
    u32 res_u,
    u32 res_v,
    V3 *out_vertices,
    u32 *out_indices
) {
    if (unlikely(res_u < 2 || res_v < 2)) return false;

    f32 min_u = 0.0f, max_u = 0.0f;
    f32 min_v = 0.0f, max_v = 0.0f;

    switch (axis) {
        case SLICE_AXIS_X:
            min_u = aabb_min.y; max_u = aabb_max.y;
            min_v = aabb_min.z; max_v = aabb_max.z;
            break;
        case SLICE_AXIS_Y:
            min_u = aabb_min.x; max_u = aabb_max.x;
            min_v = aabb_min.z; max_v = aabb_max.z;
            break;
        case SLICE_AXIS_Z:
            min_u = aabb_min.x; max_u = aabb_max.x;
            min_v = aabb_min.y; max_v = aabb_max.y;
            break;
        default:
            return false;
    }

    const f32 du = (max_u - min_u) / (f32) (res_u - 1);
    const f32 dv = (max_v - min_v) / (f32) (res_v - 1);

    for (u32 v = 0; v < res_v; ++v) {
        const f32 pv = min_v + (f32) v * dv;

        for (u32 u = 0; u < res_u; ++u) {
            const f32 pu = min_u + (f32) u * du;
            const u64 idx = (u64) v * (u64) res_u + (u64) u;

            switch (axis) {
                case SLICE_AXIS_X:
                    out_vertices[idx].x = value;
                    out_vertices[idx].y = pu;
                    out_vertices[idx].z = pv;
                    break;
                case SLICE_AXIS_Y:
                    out_vertices[idx].x = pu;
                    out_vertices[idx].y = value;
                    out_vertices[idx].z = pv;
                    break;
                case SLICE_AXIS_Z:
                    out_vertices[idx].x = pu;
                    out_vertices[idx].y = pv;
                    out_vertices[idx].z = value;
                    break;
                default:
                    return false;
            }
        }
    }

    u64 conn_idx = 0;
    for (u32 v = 0; v < res_v - 1; ++v) {
        for (u32 u = 0; u < res_u - 1; ++u) {
            const u32 v00 = v * res_u + u;
            const u32 v10 = v * res_u + (u + 1);
            const u32 v01 = (v + 1) * res_u + u;
            const u32 v11 = (v + 1) * res_u + (u + 1);

            out_indices[conn_idx++] = v00;
            out_indices[conn_idx++] = v10;
            out_indices[conn_idx++] = v11;

            out_indices[conn_idx++] = v00;
            out_indices[conn_idx++] = v11;
            out_indices[conn_idx++] = v01;
        }
    }

    return true;
}

static b32 build_case_info_payload(WS_Client *cli, char **out_json, u64 *out_len) {
    const u32 geometry_time_count = get_geometry_time_count(&cli->encase);
    const u32 variable_count = cli->encase.variable ? cli->encase.variable->len : 0;

    u64 json_cap = 512;
    for (u32 i = 0; i < variable_count; ++i) {
        json_cap += cli->encase.variable->elems[i].description.len * 6 + 128;
    }

    char *json = (char *) malloc((size_t) json_cap);
    if (unlikely(json == NULL)) return false;

    u64 off = 0;
    if (!json_append(
        json,
        json_cap,
        &off,
        "{\"type\":\"case_info\",\"geometryTimeCount\":%u,\"aabbMin\":[%.9g,%.9g,%.9g],\"aabbMax\":[%.9g,%.9g,%.9g],\"variables\":[",
        geometry_time_count,
        cli->mesh.aabb_min.x, cli->mesh.aabb_min.y, cli->mesh.aabb_min.z,
        cli->mesh.aabb_max.x, cli->mesh.aabb_max.y, cli->mesh.aabb_max.z
    )) {
        free(json);
        return false;
    }

    for (u32 i = 0; i < variable_count; ++i) {
        Ensight_Variable *var = &cli->encase.variable->elems[i];
        if (!json_append(json, json_cap, &off, "%s{\"index\":%u,\"name\":", i == 0 ? "" : ",", i) ||
            !json_append_escaped_str(json, json_cap, &off, var->description) ||
            !json_append(json, json_cap, &off, ",\"timeCount\":%u}", get_variable_time_count(&cli->encase, i))) {
            free(json);
            return false;
        }
    }

    if (!json_append(json, json_cap, &off, "]}")) {
        free(json);
        return false;
    }

    *out_json = json;
    *out_len = off;
    return true;
}

static b32 build_surface_packet(WS_Client *cli, s32 variable_idx, u32 request_time_idx, u8 **out_packet, u64 *out_packet_size, char *error_message, u64 error_message_size) {
    if (unlikely(!load_mesh_for_time(cli, request_time_idx))) {
        set_error_message(error_message, error_message_size, "Could not load geometry for surface.");
        return false;
    }

    cfd_arena_reset(&cli->response_arena);
    cfd_arena_reset(&cli->scratch_arena);

    u8 node_dim = 1;
    f32 *node_data = NULL;
    if (unlikely(!load_node_data(cli, variable_idx, request_time_idx, &node_dim, &node_data))) {
        set_error_message(error_message, error_message_size, "Could not load variable data for surface.");
        return false;
    }

    CFD_Surface_Mesh surface;
    if (unlikely(!cfd_extract_surface(&cli->response_arena, &cli->scratch_arena, &cli->mesh, &surface))) {
        set_error_message(error_message, error_message_size, "Surface extraction failed.");
        return false;
    }

    if (surface.num_vertices > UINT32_MAX || surface.num_triangles > UINT32_MAX) {
        cfd_error("Surface mesh exceeds u32 limits.");
        set_error_message(error_message, error_message_size, "Surface mesh exceeds u32 limits.");
        return false;
    }

    const u32 vertex_count = (u32) surface.num_vertices;
    const u32 triangle_count = (u32) surface.num_triangles;
    const u32 index_count = triangle_count * 3;

    const u64 total_size = sizeof(Mesh_Packet_Header)
        + (u64) vertex_count * 3ULL * sizeof(f32)
        + (u64) index_count * sizeof(u32)
        + (u64) triangle_count * 3ULL * sizeof(f32)
        + (u64) vertex_count * sizeof(f32);

    u8 *packet = (u8 *) malloc((size_t) total_size);
    if (unlikely(packet == NULL)) {
        set_error_message(error_message, error_message_size, "Out of memory while building surface packet.");
        return false;
    }

    Mesh_Packet_Header *header = (Mesh_Packet_Header *) packet;
    f32 *positions = (f32 *) (packet + sizeof(*header));
    u32 *indices = (u32 *) (positions + vertex_count * 3);
    f32 *normals = (f32 *) (indices + index_count);
    f32 *scalars = (f32 *) (normals + triangle_count * 3);

    for (u32 vertex_idx = 0; vertex_idx < vertex_count; ++vertex_idx) {
        const V3 v = surface.vertices[vertex_idx];
        positions[vertex_idx * 3 + 0] = v.x;
        positions[vertex_idx * 3 + 1] = v.y;
        positions[vertex_idx * 3 + 2] = v.z;

        const u64 old_idx = surface.vertex_new_to_old[vertex_idx];
        scalars[vertex_idx] = scalar_from_values(node_data, old_idx, node_dim);
    }

    for (u32 idx = 0; idx < index_count; ++idx) {
        const u64 index_value = surface.indices[idx];
        if (unlikely(index_value > UINT32_MAX)) {
            free(packet);
            set_error_message(error_message, error_message_size, "Surface index exceeds u32 limits.");
            return false;
        }
        indices[idx] = (u32) index_value;
    }

    compute_face_normals(positions, indices, triangle_count, normals);

    f32 scalar_min = 0.0f;
    f32 scalar_max = 1.0f;
    compute_scalar_range(scalars, vertex_count, &scalar_min, &scalar_max);

    header->magic = MESH_PACKET_MAGIC;
    header->version = MESH_PACKET_VERSION;
    header->payload_type = MESH_PACKET_SURFACE;
    header->vertex_count = vertex_count;
    header->index_count = index_count;
    header->triangle_count = triangle_count;
    header->bounds_min[0] = cli->mesh.aabb_min.x;
    header->bounds_min[1] = cli->mesh.aabb_min.y;
    header->bounds_min[2] = cli->mesh.aabb_min.z;
    header->bounds_max[0] = cli->mesh.aabb_max.x;
    header->bounds_max[1] = cli->mesh.aabb_max.y;
    header->bounds_max[2] = cli->mesh.aabb_max.z;
    header->scalar_min = scalar_min;
    header->scalar_max = scalar_max;

    *out_packet = packet;
    *out_packet_size = total_size;
    return true;
}

static b32 build_slice_packet(
    WS_Client *cli,
    s32 variable_idx,
    u32 request_time_idx,
    Slice_Axis axis,
    f32 slice_value,
    u32 resolution,
    u8 **out_packet,
    u64 *out_packet_size,
    char *error_message,
    u64 error_message_size
) {
    if (unlikely(!load_mesh_for_time(cli, request_time_idx))) {
        set_error_message(error_message, error_message_size, "Could not load geometry for slice.");
        return false;
    }

    cfd_arena_reset(&cli->response_arena);
    cfd_arena_reset(&cli->scratch_arena);

    u8 node_dim = 1;
    f32 *node_data = NULL;
    if (unlikely(!load_node_data(cli, variable_idx, request_time_idx, &node_dim, &node_data))) {
        set_error_message(error_message, error_message_size, "Could not load variable data for slice.");
        return false;
    }

    if (resolution < 2) resolution = DEFAULT_SLICE_RESOLUTION;

    switch (axis) {
        case SLICE_AXIS_X:
            if (slice_value < cli->mesh.aabb_min.x || slice_value > cli->mesh.aabb_max.x) {
                set_error_message(error_message, error_message_size, "Slice x is outside the mesh bounds.");
                return false;
            }
            break;
        case SLICE_AXIS_Y:
            if (slice_value < cli->mesh.aabb_min.y || slice_value > cli->mesh.aabb_max.y) {
                set_error_message(error_message, error_message_size, "Slice y is outside the mesh bounds.");
                return false;
            }
            break;
        case SLICE_AXIS_Z:
            if (slice_value < cli->mesh.aabb_min.z || slice_value > cli->mesh.aabb_max.z) {
                set_error_message(error_message, error_message_size, "Slice z is outside the mesh bounds.");
                return false;
            }
            break;
        default:
            set_error_message(error_message, error_message_size, "Unsupported slice axis.");
            return false;
    }

    const u32 res_u = resolution;
    const u32 res_v = resolution;
    const u32 vertex_count = res_u * res_v;
    const u32 index_count = (res_u - 1) * (res_v - 1) * 6;
    const u32 triangle_count = index_count / 3;

    V3 *plane_vertices = cfd_arena_push_array(&cli->response_arena, V3, vertex_count);
    u32 *plane_indices = cfd_arena_push_array(&cli->response_arena, u32, index_count);
    f32 *sample_values = cfd_arena_push_array(&cli->response_arena, f32, (u64) vertex_count * node_dim);
    if (unlikely(plane_vertices == NULL || plane_indices == NULL || sample_values == NULL)) {
        set_error_message(error_message, error_message_size, "Out of memory while preparing slice mesh.");
        return false;
    }

    if (unlikely(!generate_axis_aligned_plane_mesh(axis, cli->mesh.aabb_min, cli->mesh.aabb_max, slice_value, res_u, res_v, plane_vertices, plane_indices))) {
        set_error_message(error_message, error_message_size, "Could not generate slice plane mesh.");
        return false;
    }

    if (unlikely(!cfd_sample_points_tetra_node_data_parallel(
        &cli->bvh,
        &cli->mesh,
        node_data,
        node_dim,
        plane_vertices,
        vertex_count,
        1e-6f,
        sample_values
    ))) {
        set_error_message(error_message, error_message_size, "Could not sample slice values.");
        return false;
    }

    const u64 total_size = sizeof(Mesh_Packet_Header)
        + (u64) vertex_count * 3ULL * sizeof(f32)
        + (u64) index_count * sizeof(u32)
        + (u64) triangle_count * 3ULL * sizeof(f32)
        + (u64) vertex_count * sizeof(f32);

    u8 *packet = (u8 *) malloc((size_t) total_size);
    if (unlikely(packet == NULL)) {
        set_error_message(error_message, error_message_size, "Out of memory while building slice packet.");
        return false;
    }

    Mesh_Packet_Header *header = (Mesh_Packet_Header *) packet;
    f32 *positions = (f32 *) (packet + sizeof(*header));
    u32 *indices = (u32 *) (positions + vertex_count * 3);
    f32 *normals = (f32 *) (indices + index_count);
    f32 *scalars = (f32 *) (normals + triangle_count * 3);

    V3 bounds_min = plane_vertices[0];
    V3 bounds_max = plane_vertices[0];

    for (u32 vertex_idx = 0; vertex_idx < vertex_count; ++vertex_idx) {
        const V3 v = plane_vertices[vertex_idx];
        positions[vertex_idx * 3 + 0] = v.x;
        positions[vertex_idx * 3 + 1] = v.y;
        positions[vertex_idx * 3 + 2] = v.z;
        scalars[vertex_idx] = scalar_from_values(sample_values, vertex_idx, node_dim);

        if (v.x < bounds_min.x) bounds_min.x = v.x;
        if (v.y < bounds_min.y) bounds_min.y = v.y;
        if (v.z < bounds_min.z) bounds_min.z = v.z;
        if (v.x > bounds_max.x) bounds_max.x = v.x;
        if (v.y > bounds_max.y) bounds_max.y = v.y;
        if (v.z > bounds_max.z) bounds_max.z = v.z;
    }

    memcpy(indices, plane_indices, (u64) index_count * sizeof(u32));
    compute_face_normals(positions, indices, triangle_count, normals);

    f32 scalar_min = 0.0f;
    f32 scalar_max = 1.0f;
    compute_scalar_range(scalars, vertex_count, &scalar_min, &scalar_max);

    header->magic = MESH_PACKET_MAGIC;
    header->version = MESH_PACKET_VERSION;
    header->payload_type = MESH_PACKET_SLICE;
    header->vertex_count = vertex_count;
    header->index_count = index_count;
    header->triangle_count = triangle_count;
    header->bounds_min[0] = bounds_min.x;
    header->bounds_min[1] = bounds_min.y;
    header->bounds_min[2] = bounds_min.z;
    header->bounds_max[0] = bounds_max.x;
    header->bounds_max[1] = bounds_max.y;
    header->bounds_max[2] = bounds_max.z;
    header->scalar_min = scalar_min;
    header->scalar_max = scalar_max;

    *out_packet = packet;
    *out_packet_size = total_size;
    return true;
}

static struct mg_connection *find_client_connection(struct mg_mgr *mgr, WS_Client *cli) {
    for (struct mg_connection *it = mgr->conns; it != NULL; it = it->next) {
        if (it->id == cli->user_id && ws_client_get(it) == cli) return it;
    }
    return NULL;
}

static void free_async_job_result(Async_Job_Result *result) {
    if (result == NULL) return;
    free(result->text_payload);
    free(result->binary_payload);
    ws_client_release(result->cli);
    free(result);
}

static void process_async_job_result(struct mg_mgr *mgr, Async_Job_Result *result) {
    WS_Client *cli = result->cli;
    ws_client_finish_job(cli);

    struct mg_connection *target = find_client_connection(mgr, cli);
    if (target != NULL) {
        if (result->success) {
            if (result->text_payload != NULL) {
                mg_ws_send(target, result->text_payload, result->text_payload_len, WEBSOCKET_OP_TEXT);
            }
            if (result->binary_payload != NULL) {
                mg_ws_send(target, result->binary_payload, result->binary_payload_len, WEBSOCKET_OP_BINARY);
            }
        } else {
            ws_send_error(target, result->error_message);
        }
    }

    free_async_job_result(result);
}

static void *async_job_main(void *arg) {
    Async_Job_Request *request = (Async_Job_Request *) arg;
    WS_Client *cli = request->cli;
    Async_Job_Result *result = (Async_Job_Result *) calloc(1, sizeof(*result));

    if (result == NULL) {
        ws_client_finish_job(cli);
        ws_client_release(cli);
        free(request);
        return NULL;
    }

    result->cli = cli;
    result->type = request->type;

    switch (request->type) {
        case ASYNC_JOB_OPEN_CASE:
            ws_client_reset_state(cli);

            if (!ensight_read_case(&cli->case_arena, &cli->encase, request->case_path)) {
                set_error_message(result->error_message, sizeof(result->error_message), "Could not read case file.");
                break;
            }

            if (cli->encase.geometry == NULL || cli->encase.geometry->model == NULL) {
                set_error_message(result->error_message, sizeof(result->error_message), "Case does not contain a geometry model.");
                break;
            }

            if (!load_mesh_for_time(cli, 0)) {
                set_error_message(result->error_message, sizeof(result->error_message), "Could not load geometry.");
                break;
            }

            cli->has_case = true;
            snprintf(cli->case_path, sizeof(cli->case_path), "%s", request->case_path);

            if (!build_case_info_payload(cli, &result->text_payload, &result->text_payload_len)) {
                set_error_message(result->error_message, sizeof(result->error_message), "Could not build case metadata payload.");
                break;
            }

            result->success = true;
            break;

        case ASYNC_JOB_SURFACE:
            result->success = build_surface_packet(
                cli,
                request->variable_idx,
                request->request_time_idx,
                &result->binary_payload,
                &result->binary_payload_len,
                result->error_message,
                sizeof(result->error_message)
            );
            break;

        case ASYNC_JOB_SLICE:
            result->success = build_slice_packet(
                cli,
                request->variable_idx,
                request->request_time_idx,
                request->slice_axis,
                request->slice_value,
                request->resolution,
                &result->binary_payload,
                &result->binary_payload_len,
                result->error_message,
                sizeof(result->error_message)
            );
            break;

        default:
            set_error_message(result->error_message, sizeof(result->error_message), "Unsupported async job type.");
            break;
    }

    free(request);

    ws_client_retain(cli);
    if (!mg_wakeup(cli->mgr, g_wakeup_conn_id, &result, sizeof(result))) {
        ws_client_release(cli);
        ws_client_finish_job(cli);
        free_async_job_result(result);
    }

    ws_client_release(cli);
    return NULL;
}

static b32 launch_async_job(struct mg_connection *c, Async_Job_Request *request, const char *status_message) {
    WS_Client *cli = request->cli;
    if (!ws_client_begin_job(cli)) {
        ws_send_error(c, "A previous request is still running for this client.");
        free(request);
        return false;
    }

    ws_send_status(c, status_message);

    pthread_t thread;
    ws_client_retain(cli);
    if (pthread_create(&thread, NULL, async_job_main, request) != 0) {
        ws_client_release(cli);
        ws_client_finish_job(cli);
        ws_send_error(c, "Could not start worker thread.");
        free(request);
        return false;
    }

    pthread_detach(thread);
    return true;
}

static b32 ensure_case_is_loaded(struct mg_connection *c, WS_Client *cli) {
    if (cli != NULL && ws_client_has_case(cli)) return true;
    ws_send_error(c, "Load an Ensight Gold case first.");
    return false;
}

static void handle_open_case(struct mg_connection *c, WS_Client *cli, struct mg_str json) {
    char *case_path = mg_json_get_str(json, "$.casePath");
    if (case_path == NULL || case_path[0] == '\0') {
        free(case_path);
        ws_send_error(c, "Missing casePath.");
        return;
    }

    Async_Job_Request *request = (Async_Job_Request *) calloc(1, sizeof(*request));
    if (request == NULL) {
        free(case_path);
        ws_send_error(c, "Could not allocate open_case request.");
        return;
    }

    request->cli = cli;
    request->type = ASYNC_JOB_OPEN_CASE;
    snprintf(request->case_path, sizeof(request->case_path), "%s", case_path);
    free(case_path);

    launch_async_job(c, request, "Loading case...");
}

static void handle_surface_request(struct mg_connection *c, WS_Client *cli, struct mg_str json) {
    if (!ensure_case_is_loaded(c, cli)) return;

    const s32 variable_idx = (s32) mg_json_get_long(json, "$.variableIndex", -1);
    const u32 request_time_idx = (u32) mg_json_get_long(json, "$.timeIndex", 0);

    Async_Job_Request *request = (Async_Job_Request *) calloc(1, sizeof(*request));
    if (request == NULL) {
        ws_send_error(c, "Could not allocate surface request.");
        return;
    }

    request->cli = cli;
    request->type = ASYNC_JOB_SURFACE;
    request->variable_idx = variable_idx;
    request->request_time_idx = request_time_idx;
    launch_async_job(c, request, "Building surface...");
}

static void handle_slice_request(struct mg_connection *c, WS_Client *cli, struct mg_str json) {
    if (!ensure_case_is_loaded(c, cli)) return;

    const s32 variable_idx = (s32) mg_json_get_long(json, "$.variableIndex", -1);
    const u32 request_time_idx = (u32) mg_json_get_long(json, "$.timeIndex", 0);
    const u32 resolution = (u32) mg_json_get_long(json, "$.resolution", DEFAULT_SLICE_RESOLUTION);

    double slice_value_f64 = 0.0;
    if (!mg_json_get_num(json, "$.value", &slice_value_f64)) {
        ws_send_error(c, "Missing slice value.");
        return;
    }

    char *axis_str = mg_json_get_str(json, "$.axis");
    Slice_Axis axis;
    if (axis_str == NULL || !parse_slice_axis(axis_str, &axis)) {
        free(axis_str);
        ws_send_error(c, "Slice axis must be x, y or z.");
        return;
    }

    Async_Job_Request *request = (Async_Job_Request *) calloc(1, sizeof(*request));
    if (request == NULL) {
        free(axis_str);
        ws_send_error(c, "Could not allocate slice request.");
        return;
    }

    request->cli = cli;
    request->type = ASYNC_JOB_SLICE;
    request->variable_idx = variable_idx;
    request->request_time_idx = request_time_idx;
    request->slice_axis = axis;
    request->slice_value = (f32) slice_value_f64;
    request->resolution = resolution;
    free(axis_str);

    launch_async_job(c, request, "Building slice...");
}

static void handle_ws_message(struct mg_connection *c, struct mg_ws_message *wm) {
    WS_Client *cli = ws_client_get(c);
    if (cli == NULL) {
        ws_send_error(c, "Internal client state error.");
        c->is_draining = 1;
        return;
    }

    if (wm->flags & WEBSOCKET_OP_BINARY) {
        ws_send_error(c, "Binary client messages are not supported.");
        return;
    }

    struct mg_str json = wm->data;
    char *type = mg_json_get_str(json, "$.type");
    if (type == NULL) {
        ws_send_error(c, "Missing message type.");
        return;
    }

    if (strcmp(type, "open_case") == 0) {
        handle_open_case(c, cli, json);
    } else if (strcmp(type, "request_surface") == 0) {
        handle_surface_request(c, cli, json);
    } else if (strcmp(type, "request_slice") == 0) {
        handle_slice_request(c, cli, json);
    } else {
        ws_send_error(c, "Unknown websocket command.");
    }

    free(type);
}

static void ev_handler(struct mg_connection *c, int ev, void *ev_data) {
    switch (ev) {
        case MG_EV_WAKEUP: {
            if (c->id == g_wakeup_conn_id) {
                struct mg_str *data = (struct mg_str *) ev_data;
                if (data->len == sizeof(Async_Job_Result *)) {
                    Async_Job_Result *result = NULL;
                    memcpy(&result, data->buf, sizeof(result));
                    process_async_job_result(c->mgr, result);
                }
            }
        } break;

        case MG_EV_HTTP_MSG: {
            struct mg_http_message *hm = (struct mg_http_message *) ev_data;

            if (mg_match(hm->uri, mg_str("/ws"), NULL)) {
                mg_ws_upgrade(c, hm, NULL);
            } else {
                struct mg_http_serve_opts opts = {
                    .root_dir = PUBLIC_DIR,
                };
                mg_http_serve_dir(c, hm, &opts);
            }
        } break;

        case MG_EV_WS_OPEN: {
            WS_Client *cli = (WS_Client *) calloc(1, sizeof(*cli));
            if (cli == NULL || !ws_client_init(cli)) {
                free(cli);
                c->is_draining = 1;
                return;
            }

            cli->user_id = c->id;
            cli->mgr = c->mgr;
            c->fn_data = cli;
            mg_ws_printf(c, WEBSOCKET_OP_TEXT,
                         "{\"type\":\"welcome\",\"connId\":%lu}",
                         c->id);
        } break;

        case MG_EV_WS_MSG:
            handle_ws_message(c, (struct mg_ws_message *) ev_data);
            break;

        case MG_EV_CLOSE:
            ws_client_destroy(c);
            break;

        default:
            break;
    }
}

int main(void) {
    struct mg_mgr mgr;
    mg_mgr_init(&mgr);
    mg_wakeup_init(&mgr);

    struct mg_connection *listener = mg_http_listen(&mgr, LISTEN_URL, ev_handler, NULL);
    if (listener == NULL) {
        fprintf(stderr, "Failed to listen on %s\n", LISTEN_URL);
        mg_mgr_free(&mgr);
        return 1;
    }

    g_wakeup_conn_id = listener->id;

    for (;;) {
        mg_mgr_poll(&mgr, 1000);
    }

    mg_mgr_free(&mgr);
    return 0;
}
