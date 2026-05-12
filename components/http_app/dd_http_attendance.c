// HTTP handlers: attendance — events log, today summary, calendar, paired
// (in/out) view, CSV/JSONL export, wipe.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"

#include "dd_ble.h"
#include "dd_event.h"
#include "dd_storage.h"
#include "dd_time.h"
#include "dd_util.h"
#include "dd_worker.h"

#include "http_internal.h"

// ---------- raw event iteration ----------

// Count-only callback used to compute tail offset when a worker filter is
// active (the cached storage count is across all workers, so we need a
// real walk to know how many match).
typedef struct {
    uint16_t worker_id;
    int      count;
} count_ctx_t;

static esp_err_t count_iter_cb(const char *line, void *arg)
{
    count_ctx_t *c = arg;
    cJSON *j = cJSON_Parse(line);
    if (!j) return ESP_OK;
    const cJSON *wid_j = cJSON_GetObjectItem(j, "worker_id");
    int wid = cJSON_IsNumber(wid_j) ? (int)wid_j->valuedouble : 0;
    if (wid == c->worker_id) c->count++;
    cJSON_Delete(j);
    return ESP_OK;
}

typedef struct {
    httpd_req_t *req;
    bool        first;
    int         skipped;
    int         emitted;
    int         offset;
    int         limit;
    int64_t     from_ts;   // 0 = no lower bound
    int64_t     to_ts;     // 0 = no upper bound
    uint16_t    worker_id; // 0 = no filter
} events_iter_ctx_t;

static esp_err_t events_iter_cb(const char *line, void *arg)
{
    events_iter_ctx_t *ctx = arg;
    if (ctx->from_ts != 0 || ctx->to_ts != 0 || ctx->worker_id != 0) {
        cJSON *j = cJSON_Parse(line);
        if (!j) return ESP_OK;
        if (ctx->from_ts != 0 || ctx->to_ts != 0) {
            const cJSON *ts_j = cJSON_GetObjectItem(j, "ts");
            int64_t ts = cJSON_IsNumber(ts_j) ? (int64_t)ts_j->valuedouble : 0;
            if (ctx->from_ts != 0 && ts < ctx->from_ts) { cJSON_Delete(j); return ESP_OK; }
            if (ctx->to_ts   != 0 && ts >= ctx->to_ts) { cJSON_Delete(j); return ESP_OK; }
        }
        if (ctx->worker_id != 0) {
            const cJSON *wid_j = cJSON_GetObjectItem(j, "worker_id");
            int wid = cJSON_IsNumber(wid_j) ? (int)wid_j->valuedouble : 0;
            if (wid != ctx->worker_id) { cJSON_Delete(j); return ESP_OK; }
        }
        cJSON_Delete(j);
    }
    if (ctx->skipped < ctx->offset) {
        ctx->skipped++;
        return ESP_OK;
    }
    if (ctx->limit > 0 && ctx->emitted >= ctx->limit) {
        return ESP_FAIL;
    }
    if (!ctx->first) httpd_resp_sendstr_chunk(ctx->req, ",");
    ctx->first = false;
    httpd_resp_sendstr_chunk(ctx->req, line);
    ctx->emitted++;
    return ESP_OK;
}

esp_err_t events_get(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;

    char qbuf[128];
    int offset = 0;
    int limit  = 200;
    int64_t from_ts = 0, to_ts = 0;
    uint16_t worker_id = 0;
    bool offset_set = false;
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
        char val[24];
        if (httpd_query_key_value(qbuf, "offset", val, sizeof(val)) == ESP_OK) {
            offset = atoi(val);
            if (offset < 0) offset = 0;
            offset_set = true;
        }
        if (httpd_query_key_value(qbuf, "limit", val, sizeof(val)) == ESP_OK) {
            limit = atoi(val);
            if (limit <= 0 || limit > 5000) limit = 200;
        }
        if (httpd_query_key_value(qbuf, "worker_id", val, sizeof(val)) == ESP_OK) {
            worker_id = (uint16_t)atoi(val);
        }
        // ?date=YYYY-MM-DD → events between local 00:00 and 24:00
        if (httpd_query_key_value(qbuf, "date", val, sizeof(val)) == ESP_OK) {
            int y, m, d;
            if (sscanf(val, "%d-%d-%d", &y, &m, &d) == 3) {
                struct tm tm = { 0 };
                tm.tm_year = y - 1900;
                tm.tm_mon  = m - 1;
                tm.tm_mday = d;
                from_ts = (int64_t)mktime(&tm);
                to_ts   = from_ts + 86400;
                limit   = 0;
            }
        }
    }

    // Tail mode: when no time-range filter and no explicit offset, treat
    // ?limit=N as "the most recent N" instead of "the oldest N". Without
    // this the events tab silently drops the freshest entries once the
    // log grows past `limit` — exactly opposite of what the UI wants.
    // For worker_id-filtered queries we have to walk the file once to
    // count matches; otherwise the cached storage count is enough.
    if (limit > 0 && !offset_set && from_ts == 0 && to_ts == 0) {
        int matching;
        if (worker_id == 0) {
            matching = (int)dd_storage_event_count();
        } else {
            count_ctx_t cc = { .worker_id = worker_id, .count = 0 };
            dd_storage_event_iter(count_iter_cb, &cc);
            matching = cc.count;
        }
        if (matching > limit) offset = matching - limit;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"events\":[");

    events_iter_ctx_t ctx = {
        .req = req, .first = true,
        .offset = offset, .limit = limit,
        .from_ts = from_ts, .to_ts = to_ts,
        .worker_id = worker_id,
    };
    dd_storage_event_iter(events_iter_cb, &ctx);

    char tail[200];
    snprintf(tail, sizeof(tail),
             "],\"count\":%u,\"emitted\":%d,\"offset\":%d,\"limit\":%d,"
             "\"from_ts\":%lld,\"to_ts\":%lld,\"worker_id\":%u}",
             (unsigned)dd_storage_event_count(), ctx.emitted, offset, limit,
             (long long)from_ts, (long long)to_ts, worker_id);
    httpd_resp_sendstr_chunk(req, tail);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

// ---------- raw CSV export ----------

typedef struct {
    httpd_req_t *req;
    int64_t      from_ts;
    int64_t      to_ts;
} csv_ctx_t;

static void csv_field_quote(httpd_req_t *req, const char *s)
{
    if (!s || !s[0]) return;
    // CSV quoting per RFC 4180: any of comma / double-quote / CR / LF triggers
    // quoting, and embedded double-quotes are doubled. CR is easy to forget but
    // worker names and notes go through user input — must be safe.
    if (strchr(s, ',') || strchr(s, '"') || strchr(s, '\n') || strchr(s, '\r')) {
        httpd_resp_send_chunk(req, "\"", 1);
        const char *p = s;
        while (*p) {
            if (*p == '"') httpd_resp_send_chunk(req, "\"\"", 2);
            else           httpd_resp_send_chunk(req, p, 1);
            p++;
        }
        httpd_resp_send_chunk(req, "\"", 1);
    } else {
        httpd_resp_sendstr_chunk(req, s);
    }
}

static esp_err_t events_csv_iter_cb(const char *line, void *arg)
{
    csv_ctx_t *ctx = arg;
    cJSON *j = cJSON_Parse(line);
    if (!j) return ESP_OK;

    const cJSON *ts_j   = cJSON_GetObjectItem(j, "ts");
    const cJSON *type_j = cJSON_GetObjectItem(j, "type");
    const cJSON *src_j  = cJSON_GetObjectItem(j, "src");
    const cJSON *wid_j  = cJSON_GetObjectItem(j, "worker_id");
    const cJSON *peer_j = cJSON_GetObjectItem(j, "peer");
    const cJSON *rsn_j  = cJSON_GetObjectItem(j, "reason");
    const cJSON *note_j = cJSON_GetObjectItem(j, "note");

    int64_t ts = cJSON_IsNumber(ts_j) ? (int64_t)ts_j->valuedouble : 0;
    if (ctx->from_ts != 0 && ts < ctx->from_ts) { cJSON_Delete(j); return ESP_OK; }
    if (ctx->to_ts   != 0 && ts >= ctx->to_ts) { cJSON_Delete(j); return ESP_OK; }

    char buf[64];
    snprintf(buf, sizeof(buf), "%lld", (long long)ts);
    httpd_resp_sendstr_chunk(ctx->req, buf);
    httpd_resp_send_chunk(ctx->req, ",", 1);

    if (ts > 0) {
        time_t t = (time_t)ts;
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
        httpd_resp_sendstr_chunk(ctx->req, buf);
    }
    httpd_resp_send_chunk(ctx->req, ",", 1);

    csv_field_quote(ctx->req, cJSON_IsString(type_j) ? type_j->valuestring : "");
    httpd_resp_send_chunk(ctx->req, ",", 1);
    csv_field_quote(ctx->req, cJSON_IsString(src_j) ? src_j->valuestring : "");
    httpd_resp_send_chunk(ctx->req, ",", 1);

    int wid = cJSON_IsNumber(wid_j) ? (int)wid_j->valuedouble : 0;
    if (wid > 0) {
        snprintf(buf, sizeof(buf), "%d", wid);
        httpd_resp_sendstr_chunk(ctx->req, buf);
        dd_worker_t w;
        if (dd_worker_get((uint16_t)wid, &w) == ESP_OK) {
            httpd_resp_send_chunk(ctx->req, ",", 1);
            csv_field_quote(ctx->req, w.name);
        } else {
            httpd_resp_send_chunk(ctx->req, ",", 1);
        }
    } else {
        httpd_resp_send_chunk(ctx->req, ",", 1);
    }
    httpd_resp_send_chunk(ctx->req, ",", 1);

    csv_field_quote(ctx->req, cJSON_IsString(peer_j) ? peer_j->valuestring : "");
    httpd_resp_send_chunk(ctx->req, ",", 1);

    if (cJSON_IsNumber(rsn_j)) {
        snprintf(buf, sizeof(buf), "%d", (int)rsn_j->valuedouble);
        httpd_resp_sendstr_chunk(ctx->req, buf);
    }
    httpd_resp_send_chunk(ctx->req, ",", 1);

    csv_field_quote(ctx->req, cJSON_IsString(note_j) ? note_j->valuestring : "");
    httpd_resp_send_chunk(ctx->req, "\n", 1);

    cJSON_Delete(j);
    return ESP_OK;
}

// ---------- view-layer pairing (in/out → segments) ----------

#define PAIR_MAX_OPEN 16

typedef struct {
    uint16_t worker_id;
    uint8_t  addr[6];
    bool     has_addr;
    int64_t  in_ts;
    int64_t  in_mono;
    char     in_src[16];
} pair_open_t;

typedef struct {
    httpd_req_t *req;
    bool         first;
    int          emitted;
    bool         csv_mode;
    int64_t      from_ts;
    int64_t      to_ts;
    pair_open_t  opens[PAIR_MAX_OPEN];
} pair_ctx_t;

static int find_open_slot(pair_ctx_t *ctx, uint16_t worker_id,
                          const uint8_t *addr, bool has_addr)
{
    for (int i = 0; i < PAIR_MAX_OPEN; i++) {
        pair_open_t *o = &ctx->opens[i];
        if (o->in_ts == 0) continue;
        if (worker_id != 0 && o->worker_id == worker_id) return i;
        if (worker_id == 0 && has_addr && o->has_addr &&
            memcmp(o->addr, addr, 6) == 0) return i;
    }
    return -1;
}

static int find_free_slot(pair_ctx_t *ctx)
{
    for (int i = 0; i < PAIR_MAX_OPEN; i++) {
        if (ctx->opens[i].in_ts == 0) return i;
    }
    return -1;
}

static void emit_segment(pair_ctx_t *ctx, const pair_open_t *open,
                         int64_t out_ts, const char *out_src,
                         bool open_segment)
{
    if (ctx->csv_mode) {
        char buf[64];

        snprintf(buf, sizeof(buf), "%u,", open->worker_id);
        httpd_resp_sendstr_chunk(ctx->req, buf);
        if (open->worker_id != 0) {
            dd_worker_t info;
            if (dd_worker_get(open->worker_id, &info) == ESP_OK) {
                csv_field_quote(ctx->req, info.name);
            }
        }
        httpd_resp_send_chunk(ctx->req, ",", 1);

        if (open->in_ts > 0) {
            time_t t = (time_t)open->in_ts;
            struct tm tm; localtime_r(&t, &tm);
            strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
            httpd_resp_sendstr_chunk(ctx->req, buf);
            httpd_resp_send_chunk(ctx->req, ",", 1);
            strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
            httpd_resp_sendstr_chunk(ctx->req, buf);
        } else {
            httpd_resp_send_chunk(ctx->req, ",", 1);
        }
        httpd_resp_send_chunk(ctx->req, ",", 1);

        if (!open_segment && out_ts > 0) {
            time_t t = (time_t)out_ts;
            struct tm tm; localtime_r(&t, &tm);
            strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
            httpd_resp_sendstr_chunk(ctx->req, buf);
            httpd_resp_send_chunk(ctx->req, ",", 1);
            int dur_min = (int)((out_ts - open->in_ts) / 60);
            snprintf(buf, sizeof(buf), "%d", dur_min);
            httpd_resp_sendstr_chunk(ctx->req, buf);
        } else {
            httpd_resp_send_chunk(ctx->req, ",", 1);
            httpd_resp_sendstr_chunk(ctx->req, "open");
        }
        httpd_resp_send_chunk(ctx->req, ",", 1);

        csv_field_quote(ctx->req, open->in_src);
        httpd_resp_send_chunk(ctx->req, ",", 1);
        if (!open_segment) csv_field_quote(ctx->req, out_src);
        httpd_resp_send_chunk(ctx->req, "\n", 1);
        ctx->emitted++;
        return;
    }

    if (!ctx->first) httpd_resp_sendstr_chunk(ctx->req, ",");
    ctx->first = false;

    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "worker_id", open->worker_id);
    if (open->has_addr) {
        char addr_str[18];
        dd_format_mac(open->addr, addr_str);
        cJSON_AddStringToObject(j, "peer", addr_str);
    }
    cJSON_AddNumberToObject(j, "in_ts",  (double)open->in_ts);
    cJSON_AddStringToObject(j, "in_src", open->in_src);
    if (open_segment) {
        cJSON_AddBoolToObject(j, "open", true);
    } else {
        cJSON_AddNumberToObject(j, "out_ts",   (double)out_ts);
        cJSON_AddStringToObject(j, "out_src",  out_src);
        cJSON_AddNumberToObject(j, "duration_s", (double)(out_ts - open->in_ts));
    }
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (s) {
        httpd_resp_sendstr_chunk(ctx->req, s);
        free(s);
        ctx->emitted++;
    }
}

static esp_err_t paired_iter_cb(const char *line, void *arg)
{
    pair_ctx_t *ctx = arg;
    cJSON *j = cJSON_Parse(line);
    if (!j) return ESP_OK;

    const cJSON *type_j = cJSON_GetObjectItem(j, "type");
    const cJSON *ts_j   = cJSON_GetObjectItem(j, "ts");
    const cJSON *wid_j  = cJSON_GetObjectItem(j, "worker_id");
    const cJSON *peer_j = cJSON_GetObjectItem(j, "peer");
    const cJSON *src_j  = cJSON_GetObjectItem(j, "src");
    const cJSON *mono_j = cJSON_GetObjectItem(j, "mono_us");

    if (!cJSON_IsString(type_j) || !cJSON_IsNumber(ts_j)) {
        cJSON_Delete(j); return ESP_OK;
    }

    bool is_in = strcmp(type_j->valuestring, "in") == 0;
    int64_t ts = (int64_t)ts_j->valuedouble;
    int64_t mono = cJSON_IsNumber(mono_j) ? (int64_t)mono_j->valuedouble : 0;
    uint16_t wid = cJSON_IsNumber(wid_j) ? (uint16_t)wid_j->valuedouble : 0;

    if ((ctx->from_ts != 0 && ts < ctx->from_ts) ||
        (ctx->to_ts   != 0 && ts >= ctx->to_ts)) {
        cJSON_Delete(j); return ESP_OK;
    }

    uint8_t addr[6] = {0};
    bool has_addr = false;
    if (cJSON_IsString(peer_j)) {
        unsigned int a[6];
        if (sscanf(peer_j->valuestring, "%02x:%02x:%02x:%02x:%02x:%02x",
                   &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) == 6) {
            for (int i = 0; i < 6; i++) addr[i] = (uint8_t)a[i];
            has_addr = true;
        }
    }
    const char *src = cJSON_IsString(src_j) ? src_j->valuestring : "?";

    if (is_in) {
        int existing = find_open_slot(ctx, wid, addr, has_addr);
        if (existing >= 0) {
            emit_segment(ctx, &ctx->opens[existing], 0, "", true);
            memset(&ctx->opens[existing], 0, sizeof(ctx->opens[existing]));
        }
        int slot = find_free_slot(ctx);
        if (slot < 0) {
            slot = 0;
            for (int i = 1; i < PAIR_MAX_OPEN; i++) {
                if (ctx->opens[i].in_ts < ctx->opens[slot].in_ts) slot = i;
            }
        }
        pair_open_t *o = &ctx->opens[slot];
        o->worker_id = wid;
        memcpy(o->addr, addr, 6);
        o->has_addr = has_addr;
        o->in_ts = ts > 0 ? ts : mono / 1000000;
        o->in_mono = mono;
        strncpy(o->in_src, src, sizeof(o->in_src) - 1);
        o->in_src[sizeof(o->in_src) - 1] = 0;
    } else {
        int idx = find_open_slot(ctx, wid, addr, has_addr);
        if (idx >= 0) {
            int64_t out_ts = ts > 0 ? ts : mono / 1000000;
            emit_segment(ctx, &ctx->opens[idx], out_ts, src, false);
            memset(&ctx->opens[idx], 0, sizeof(ctx->opens[idx]));
        }
    }

    cJSON_Delete(j);
    return ESP_OK;
}

esp_err_t events_export_get(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;

    char qbuf[160];
    char fmt[8] = "jsonl";
    char mode[8] = "raw";
    int64_t from_ts = 0, to_ts = 0;
    uint16_t worker_id = 0;
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
        char val[24];
        httpd_query_key_value(qbuf, "format", fmt, sizeof(fmt));
        httpd_query_key_value(qbuf, "mode",   mode, sizeof(mode));
        if (httpd_query_key_value(qbuf, "from", val, sizeof(val)) == ESP_OK) {
            int y, m, d;
            if (sscanf(val, "%d-%d-%d", &y, &m, &d) == 3) {
                struct tm tm = { 0 };
                tm.tm_year = y - 1900; tm.tm_mon = m - 1; tm.tm_mday = d;
                from_ts = (int64_t)mktime(&tm);
            }
        }
        if (httpd_query_key_value(qbuf, "to", val, sizeof(val)) == ESP_OK) {
            int y, m, d;
            if (sscanf(val, "%d-%d-%d", &y, &m, &d) == 3) {
                struct tm tm = { 0 };
                tm.tm_year = y - 1900; tm.tm_mon = m - 1; tm.tm_mday = d;
                to_ts = (int64_t)mktime(&tm) + 86400;
            }
        }
        if (httpd_query_key_value(qbuf, "worker_id", val, sizeof(val)) == ESP_OK) {
            worker_id = (uint16_t)atoi(val);
        }
    }

    bool csv = strcmp(fmt, "csv") == 0;
    bool paired = strcmp(mode, "paired") == 0;

    httpd_resp_set_type(req, csv ? "text/csv; charset=utf-8"
                                 : "application/x-ndjson; charset=utf-8");

    char fname[96];
    if (from_ts > 0 || to_ts > 0) {
        snprintf(fname, sizeof(fname),
                 "attachment; filename=\"dingdong-%s-%lld-%lld.%s\"",
                 paired ? "segments" : "events",
                 (long long)from_ts, (long long)to_ts,
                 csv ? "csv" : "jsonl");
    } else {
        snprintf(fname, sizeof(fname), "attachment; filename=\"dingdong-%s.%s\"",
                 paired ? "segments" : "events", csv ? "csv" : "jsonl");
    }
    httpd_resp_set_hdr(req, "Content-Disposition", fname);

    if (paired) {
        if (csv) {
            httpd_resp_sendstr_chunk(req,
                "worker_id,worker_name,date,in,out,duration_min,in_src,out_src\n");
        }
        pair_ctx_t pctx = {
            .req = req, .first = true, .csv_mode = csv,
            .from_ts = from_ts, .to_ts = to_ts,
        };
        dd_storage_event_iter(paired_iter_cb, &pctx);
        for (int i = 0; i < PAIR_MAX_OPEN; i++) {
            if (pctx.opens[i].in_ts != 0) {
                emit_segment(&pctx, &pctx.opens[i], 0, "", true);
            }
        }
        if (!csv) httpd_resp_send_chunk(req, "\n", 1);
    } else if (csv) {
        httpd_resp_sendstr_chunk(req,
            "ts,iso_local,type,src,worker_id,worker_name,peer,reason,note\n");
        csv_ctx_t cctx = { .req = req, .from_ts = from_ts, .to_ts = to_ts };
        dd_storage_event_iter(events_csv_iter_cb, &cctx);
    } else {
        events_iter_ctx_t ctx = {
            .req = req, .first = true, .offset = 0, .limit = 0,
            .from_ts = from_ts, .to_ts = to_ts, .worker_id = worker_id,
        };
        dd_storage_event_iter(events_iter_cb, &ctx);
        httpd_resp_send_chunk(req, "\n", 1);
    }
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

esp_err_t events_paired_get(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"segments\":[");

    pair_ctx_t ctx = { .req = req, .first = true };
    dd_storage_event_iter(paired_iter_cb, &ctx);

    for (int i = 0; i < PAIR_MAX_OPEN; i++) {
        if (ctx.opens[i].in_ts != 0) {
            emit_segment(&ctx, &ctx.opens[i], 0, "", true);
        }
    }

    char tail[64];
    snprintf(tail, sizeof(tail), "],\"count\":%d}", ctx.emitted);
    httpd_resp_sendstr_chunk(req, tail);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

// ---------- POST event / wipe ----------

esp_err_t events_post(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;

    cJSON *j = recv_json_body(req);
    if (!j) return reply_text(req, "400 Bad Request", "bad json");

    const cJSON *type_j = cJSON_GetObjectItem(j, "type");
    const cJSON *note_j = cJSON_GetObjectItem(j, "note");
    const cJSON *wid_j  = cJSON_GetObjectItem(j, "worker_id");
    if (!cJSON_IsString(type_j)) {
        cJSON_Delete(j);
        return reply_text(req, "400 Bad Request", "type required");
    }
    dd_event_type_t t;
    if (strcmp(type_j->valuestring, "in") == 0)       t = DD_EV_IN;
    else if (strcmp(type_j->valuestring, "out") == 0) t = DD_EV_OUT;
    else { cJSON_Delete(j); return reply_text(req, "400 Bad Request", "type must be in|out"); }

    const char *note = cJSON_IsString(note_j) ? note_j->valuestring : NULL;

    const uint8_t *peer = NULL;
    dd_worker_t winfo;
    if (cJSON_IsNumber(wid_j)) {
        uint16_t id = (uint16_t)wid_j->valuedouble;
        if (dd_worker_get(id, &winfo) == ESP_OK) {
            peer = winfo.addr;
        }
    }

    esp_err_t err = dd_event_record(t, DD_SRC_MANUAL_WEB, peer, 0, note);
    cJSON_Delete(j);

    if (err != ESP_OK) return reply_text(req, "500 Internal Server Error", "record failed");
    return reply_text(req, "201 Created", "ok");
}

esp_err_t events_wipe_post(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;
    esp_err_t err = dd_storage_event_wipe();
    if (err != ESP_OK) return reply_text(req, "500 Internal Server Error", "wipe failed");
    // Tell the presence machine to forget which IN events it has emitted, so
    // the next 1Hz tick re-fires IN for everyone currently in proximity.
    dd_ble_presence_reset_events();
    return reply_text(req, "200 OK", "wiped");
}

esp_err_t events_delete_post(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;
    cJSON *j = recv_json_body(req);
    if (!j) return reply_text(req, "400 Bad Request", "bad json");
    const cJSON *ts_j   = cJSON_GetObjectItem(j, "ts");
    const cJSON *mono_j = cJSON_GetObjectItem(j, "mono_us");
    if (!cJSON_IsNumber(ts_j) || !cJSON_IsNumber(mono_j)) {
        cJSON_Delete(j);
        return reply_text(req, "400 Bad Request", "ts and mono_us required");
    }
    int64_t ts   = (int64_t)ts_j->valuedouble;
    int64_t mono = (int64_t)mono_j->valuedouble;
    cJSON_Delete(j);
    esp_err_t err = dd_storage_event_delete_one(ts, mono);
    if (err != ESP_OK) return reply_text(req, "500 Internal Server Error", "delete failed");
    return reply_text(req, "200 OK", "ok");
}

// ---------- today's summary ----------
//
// Per-worker today (since local 00:00) total in-time and segments. Streams a
// single JSON object grouped by worker_id.

typedef struct {
    uint16_t worker_id;
    uint8_t  addr[6];
    bool     has_addr;
    int64_t  in_ts;
    char     in_src[16];
    int      total_seconds;
    int      segments;
    bool     currently_in;
} today_worker_t;

#define TODAY_MAX_WORKERS 16

typedef struct {
    today_worker_t list[TODAY_MAX_WORKERS];
    int            count;
    int64_t        cutoff_ts;
} today_ctx_t;

static today_worker_t *today_get_or_create(today_ctx_t *ctx, uint16_t wid,
                                            const uint8_t addr[6], bool has_addr)
{
    for (int i = 0; i < ctx->count; i++) {
        if (wid != 0 && ctx->list[i].worker_id == wid) return &ctx->list[i];
        if (wid == 0 && has_addr && ctx->list[i].has_addr &&
            memcmp(ctx->list[i].addr, addr, 6) == 0) return &ctx->list[i];
    }
    if (ctx->count >= TODAY_MAX_WORKERS) return NULL;
    today_worker_t *w = &ctx->list[ctx->count++];
    memset(w, 0, sizeof(*w));
    w->worker_id = wid;
    if (has_addr) { memcpy(w->addr, addr, 6); w->has_addr = true; }
    return w;
}

static esp_err_t today_iter_cb(const char *line, void *arg)
{
    today_ctx_t *ctx = arg;
    cJSON *j = cJSON_Parse(line);
    if (!j) return ESP_OK;

    const cJSON *type_j = cJSON_GetObjectItem(j, "type");
    const cJSON *ts_j   = cJSON_GetObjectItem(j, "ts");
    const cJSON *wid_j  = cJSON_GetObjectItem(j, "worker_id");
    const cJSON *peer_j = cJSON_GetObjectItem(j, "peer");
    const cJSON *src_j  = cJSON_GetObjectItem(j, "src");

    if (!cJSON_IsString(type_j) || !cJSON_IsNumber(ts_j)) {
        cJSON_Delete(j); return ESP_OK;
    }

    int64_t ts = (int64_t)ts_j->valuedouble;
    if (ts < ctx->cutoff_ts) {
        cJSON_Delete(j); return ESP_OK;
    }

    uint16_t wid = cJSON_IsNumber(wid_j) ? (uint16_t)wid_j->valuedouble : 0;
    uint8_t addr[6] = {0};
    bool has_addr = false;
    if (cJSON_IsString(peer_j)) {
        unsigned int a[6];
        if (sscanf(peer_j->valuestring, "%02x:%02x:%02x:%02x:%02x:%02x",
                   &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) == 6) {
            for (int i = 0; i < 6; i++) addr[i] = (uint8_t)a[i];
            has_addr = true;
        }
    }
    today_worker_t *w = today_get_or_create(ctx, wid, addr, has_addr);
    if (!w) { cJSON_Delete(j); return ESP_OK; }

    bool is_in = strcmp(type_j->valuestring, "in") == 0;
    if (is_in) {
        w->in_ts = ts;
        w->currently_in = true;
        if (cJSON_IsString(src_j)) {
            strncpy(w->in_src, src_j->valuestring, sizeof(w->in_src) - 1);
        }
    } else {
        if (w->in_ts > 0) {
            int dur = (int)(ts - w->in_ts);
            if (dur > 0 && dur < 86400) w->total_seconds += dur;
            w->segments++;
            w->in_ts = 0;
            w->currently_in = false;
        }
    }
    cJSON_Delete(j);
    return ESP_OK;
}

esp_err_t today_get(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0;
    int64_t cutoff = (int64_t)mktime(&tm);

    today_ctx_t tctx = { .count = 0, .cutoff_ts = cutoff };
    dd_storage_event_iter(today_iter_cb, &tctx);

    int64_t now_ts = (int64_t)now;
    for (int i = 0; i < tctx.count; i++) {
        if (tctx.list[i].in_ts > 0) {
            int dur = (int)(now_ts - tctx.list[i].in_ts);
            if (dur > 0 && dur < 86400) tctx.list[i].total_seconds += dur;
        }
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "date_unix", (double)cutoff);
    cJSON_AddNumberToObject(root, "now", (double)now_ts);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < tctx.count; i++) {
        today_worker_t *w = &tctx.list[i];
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "worker_id", w->worker_id);
        if (w->worker_id != 0) {
            dd_worker_t info;
            if (dd_worker_get(w->worker_id, &info) == ESP_OK) {
                cJSON_AddStringToObject(e, "name", info.name);
                cJSON_AddStringToObject(e, "category", info.category);
            }
        }
        if (w->has_addr) {
            char addr_str[18];
            dd_format_mac(w->addr, addr_str);
            cJSON_AddStringToObject(e, "peer", addr_str);
        }
        cJSON_AddNumberToObject(e, "total_seconds", w->total_seconds);
        cJSON_AddNumberToObject(e, "segments", w->segments);
        cJSON_AddBoolToObject  (e, "currently_in", w->currently_in);
        cJSON_AddItemToArray(arr, e);
    }
    cJSON_AddItemToObject(root, "workers", arr);
    return reply_json_status(req, "200 OK", root);
}

// ---------- calendar ----------
//
// Returns event counts grouped by local date for the past N days.

#define CAL_MAX_DAYS 62

typedef struct {
    int64_t  date_unix;
    int      events;
    uint32_t worker_mask;
} cal_day_t;

typedef struct {
    cal_day_t days[CAL_MAX_DAYS];
    int       count;
    int64_t   from_ts;
    int64_t   to_ts;
} cal_ctx_t;

static cal_day_t *cal_get_day(cal_ctx_t *ctx, int64_t day_midnight)
{
    for (int i = 0; i < ctx->count; i++) {
        if (ctx->days[i].date_unix == day_midnight) return &ctx->days[i];
    }
    if (ctx->count >= CAL_MAX_DAYS) return NULL;
    cal_day_t *d = &ctx->days[ctx->count++];
    d->date_unix = day_midnight;
    d->events = 0;
    d->worker_mask = 0;
    return d;
}

static esp_err_t cal_iter_cb(const char *line, void *arg)
{
    cal_ctx_t *ctx = arg;
    cJSON *j = cJSON_Parse(line);
    if (!j) return ESP_OK;

    const cJSON *ts_j = cJSON_GetObjectItem(j, "ts");
    const cJSON *wid_j = cJSON_GetObjectItem(j, "worker_id");
    if (!cJSON_IsNumber(ts_j)) { cJSON_Delete(j); return ESP_OK; }

    int64_t ts = (int64_t)ts_j->valuedouble;
    if (ts < ctx->from_ts || ts >= ctx->to_ts) { cJSON_Delete(j); return ESP_OK; }

    time_t t = (time_t)ts;
    struct tm tm; localtime_r(&t, &tm);
    tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0;
    int64_t day = (int64_t)mktime(&tm);

    cal_day_t *d = cal_get_day(ctx, day);
    if (!d) { cJSON_Delete(j); return ESP_OK; }
    d->events++;
    if (cJSON_IsNumber(wid_j)) {
        int wid = (int)wid_j->valuedouble;
        if (wid > 0 && wid < 32) d->worker_mask |= (1U << wid);
    }
    cJSON_Delete(j);
    return ESP_OK;
}

esp_err_t calendar_get(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;

    int days = 30;
    char qbuf[64];
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
        char val[16];
        if (httpd_query_key_value(qbuf, "days", val, sizeof(val)) == ESP_OK) {
            days = atoi(val);
            if (days <= 0 || days > CAL_MAX_DAYS) days = 30;
        }
    }

    time_t now = time(NULL);
    struct tm tm; localtime_r(&now, &tm);
    tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0;
    int64_t today_midnight = (int64_t)mktime(&tm);
    int64_t to_ts = today_midnight + 86400;
    int64_t from_ts = today_midnight - (int64_t)(days - 1) * 86400;

    cal_ctx_t cctx = { .count = 0, .from_ts = from_ts, .to_ts = to_ts };
    dd_storage_event_iter(cal_iter_cb, &cctx);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "from_unix", (double)from_ts);
    cJSON_AddNumberToObject(root, "to_unix",   (double)to_ts);
    cJSON_AddNumberToObject(root, "days",      days);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < cctx.count; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "date_unix", (double)cctx.days[i].date_unix);
        cJSON_AddNumberToObject(e, "events",    cctx.days[i].events);
        uint32_t m = cctx.days[i].worker_mask;
        int n = 0;
        while (m) { n += m & 1; m >>= 1; }
        cJSON_AddNumberToObject(e, "workers", n);
        cJSON_AddItemToArray(arr, e);
    }
    cJSON_AddItemToObject(root, "buckets", arr);
    return reply_json_status(req, "200 OK", root);
}
