#define WIN32_LEAN_AND_MEAN

#define CFD_LIB_IMPLEMENTATION
#include "ensight_gold.h"
#include "cfd_render.h"

#include "mongoose.h"

#define PUBLIC_DIR "frontend"

typedef struct WS_Client {
    CFD_Arena arena;
    u64 user_id;

    // ...
} WS_Client;

static WS_Client *ws_client_get(struct mg_connection *c) {
    return (WS_Client *)c->fn_data;
}

static void ws_client_destroy(struct mg_connection *c) {
    WS_Client *cli = ws_client_get(c);
    if (cli != NULL) {
        cfd_arena_destroy(&cli->arena);
        free(cli);
        c->fn_data = NULL;
    }
}

static void ev_handler(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;

        if (mg_match(hm->uri, mg_str("/ws"), NULL)) {
            mg_ws_upgrade(c, hm, NULL);
        } else if (mg_match(hm->uri, mg_str("/api/"), NULL)) {
            char case_path[PATH_MAX];
            struct mg_str case_path_mg = mg_http_var(hm->query, mg_str("case_path"));
            if (case_path_mg.buf == NULL) {
                mg_http_reply(c, 200, NULL, "case_path parameter is not provided!\n");
                return;
            }

            memcpy(case_path, case_path_mg.buf, case_path_mg.len);
            case_path[case_path_mg.len] = '\0';

            CFD_Arena arena;
            if (unlikely(!cfd_arena_init(&arena, GB(16)))) {
                mg_http_reply(c, 200, NULL, "Couldn't create arena!\n");
                return;
            }

            Ensight_Case encase;

            CFD_File case_file;
            if (unlikely(!cfd_file_slurp(case_path, &case_file))) {
                mg_http_reply(c, 200, NULL, "Couldn't open case file!\n");
                return;
            }

            if (!ensight_parse_case(&arena, &encase, case_path, &case_file)) {
                mg_http_reply(c, 200, NULL, "Couldn't parse case file!\n");
                return;
            }

            mg_printf(c, "HTTP/1.1 200 OK\r\n"
                         "Content-Type: application/json\r\n"
                         "Transfer-Encoding: chunked\r\n\r\n");
            mg_http_printf_chunk(c, "{\"variable_names\":[");
            for (u32 var_idx = 0; var_idx < encase.variable->len - 1; ++var_idx) {
                Str8 description = encase.variable->elems[var_idx].description;
                mg_http_printf_chunk(c, "\"%.*s\",", (int)description.len, description.buffer);
            }
            if (encase.variable->len > 0) {
                Str8 description = encase.variable->elems[encase.variable->len - 1].description;
                mg_http_printf_chunk(c, "\"%.*s\"", (int)description.len, description.buffer);
            }
            mg_http_printf_chunk(c, "]}");
            mg_http_printf_chunk(c, "");

            cfd_file_free(&case_file);

            cfd_arena_destroy(&arena);
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{%m:%d}", mg_print_esc, 0, "success", 1);
        } else {
            struct mg_http_serve_opts opts = {
                .root_dir = PUBLIC_DIR,
            };

            mg_http_serve_dir(c, hm, &opts);
        }
    } else if (ev == MG_EV_WS_MSG) {
        struct mg_ws_message *wm = (struct mg_ws_message *)ev_data;
        WS_Client *cli = ws_client_get(c);

        if (cli == NULL) {
            mg_ws_send(c, "internal error", 14, WEBSOCKET_OP_TEXT);
            c->is_draining = 1;
            return;
        }

        if (wm->flags & WEBSOCKET_OP_BINARY) {
            const u8 *data = (u8 *)wm->data.buf;
            u64 len = (u64)wm->data.len;
            (void)data;
            (void)len;

            // itt feldolgozom
        }
    } else if (ev == MG_EV_WS_OPEN) {
        WS_Client *cli = (WS_Client *)calloc(1, sizeof(*cli));
        if (cli == NULL) {
            c->is_draining = 1;
            return;
        }

        cfd_arena_init(&cli->arena, GB(4));
        cli->user_id = c->id;

        c->fn_data = cli;
        mg_ws_printf(c, WEBSOCKET_OP_TEXT,
                     "{\"type\":\"welcome\",\"conn_id\":%lu}",
                     c->id);
        return;
    } else if (ev == MG_EV_CLOSE) {
        ws_client_destroy(c);
        return;
    }
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    struct mg_mgr mgr;
    mg_mgr_init(&mgr);

    mg_http_listen(&mgr, "http://localhost:8000", ev_handler, NULL);

    for (;;) {
        mg_mgr_poll(&mgr, 1000);
    }

    mg_mgr_free(&mgr);
    return 0;
}
