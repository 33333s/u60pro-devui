/*
 * json.c - tiny read-only JSON value extractor. See json.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "json.h"
#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

static const char *skip_json_string(const char *p)
{
    if (!p || *p != '"') return NULL;

    int esc = 0;
    for (p++; *p; p++) {
        if (esc) { esc = 0; continue; }
        if (*p == '\\') { esc = 1; continue; }
        if (*p == '"') return p + 1;
    }
    return NULL;
}

static int hex4(const char **pp, uint32_t *out)
{
    const char *p = *pp;
    uint32_t v = 0;
    int d;
    for (int i = 0; i < 4; i++) {
        if (!p[i]) return 0;
        unsigned char c = (unsigned char)p[i];
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = 10 + (c - 'a');
        else if (c >= 'A' && c <= 'F') d = 10 + (c - 'A');
        else return 0;
        v = (v << 4) | (uint32_t)d;
    }
    *out = v;
    *pp = p + 4;
    return 1;
}

static size_t append_utf8_codepoint_json(char *out, size_t outlen, size_t pos, uint32_t cp)
{
    if (cp <= 0x7F) {
        if (pos + 1 >= outlen) return pos;
        out[pos++] = (char)cp;
        return pos;
    }
    if (cp <= 0x7FF) {
        if (pos + 2 >= outlen) return pos;
        out[pos++] = (char)(0xC0 | (cp >> 6));
        out[pos++] = (char)(0x80 | (cp & 0x3F));
        return pos;
    }
    if (cp <= 0xFFFF) {
        if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFDu;
        if (pos + 3 >= outlen) return pos;
        out[pos++] = (char)(0xE0 | (cp >> 12));
        out[pos++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[pos++] = (char)(0x80 | (cp & 0x3F));
        return pos;
    }
    if (cp <= 0x10FFFF) {
        if (pos + 4 >= outlen) return pos;
        out[pos++] = (char)(0xF0 | (cp >> 18));
        out[pos++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[pos++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[pos++] = (char)(0x80 | (cp & 0x3F));
        return pos;
    }
    return pos;
}

/* Return a pointer just past the ':' of "key": , or NULL. */
static const char *find_key(const char *json, const char *key)
{
    size_t klen = strlen(key);
    int obj_depth = 0;
    int arr_depth = 0;
    int expect_key = 0;
    const char *p = json;

    while (*p) {
        char c = *p;
        if (c == '"') {
            const char *ks = p + 1;
            const char *kp = skip_json_string(p);
            if (!kp) return NULL;
            const char *ke = kp - 1;
            if (kp > ks && expect_key && obj_depth == 1) {
                if ((size_t)(ke - ks) == klen && strncmp(ks, key, klen) == 0) {
                    const char *q = kp;
                    while (is_ws(*q)) q++;
                    if (*q == ':') return q + 1;
                }
            }
            p = kp;
            if (obj_depth == 1 && expect_key) expect_key = 0;
            continue;
        }
        if (c == '{') {
            obj_depth++;
            expect_key = (obj_depth == 1);
            p++;
            continue;
        }
        if (c == '}') {
            if (obj_depth > 0) obj_depth--;
            expect_key = (obj_depth == 1);
            p++;
            continue;
        }
        if (c == '[') {
            arr_depth++;
            expect_key = 0;
            p++;
            continue;
        }
        if (c == ']') {
            if (arr_depth > 0) arr_depth--;
            p++;
            continue;
        }
        if (c == ',') {
            if (obj_depth == 1 && arr_depth == 0) expect_key = 1;
            p++;
            continue;
        }
        if (c == ':') {
            expect_key = 0;
            p++;
            continue;
        }
        p++;
    }
    return NULL;
}

int json_get(const char *json, const char *key, char *out, size_t outlen)
{
    if (!json || !key || outlen == 0) return 0;
    const char *v = find_key(json, key);
    if (!v) return 0;
    while (is_ws(*v)) v++;

    size_t n = 0;
    if (*v == '"') {
        v++;
        while (*v && *v != '"' && n < outlen - 1) {
            if (*v == '\\' && v[1]) {
                v++;
                if (*v == '"') out[n++] = '"';
                else if (*v == '\\') out[n++] = '\\';
                else if (*v == '/') out[n++] = '/';
                else if (*v == 'b') out[n++] = '\b';
                else if (*v == 'f') out[n++] = '\f';
                else if (*v == 'n') out[n++] = '\n';
                else if (*v == 'r') out[n++] = '\r';
                else if (*v == 't') out[n++] = '\t';
                else if (*v == 'u') {
                    v++;
                    uint32_t cp = 0;
                    const char *q = v;
                    if (!hex4(&q, &cp)) break;
                    if (cp >= 0xD800 && cp <= 0xDBFF &&
                        q[0] == '\\' && q[1] == 'u') {
                        const char *q2 = q + 2;
                        uint32_t lo;
                        if (hex4(&q2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                            q = q2;
                        }
                    }
                    n = append_utf8_codepoint_json(out, outlen, n, cp);
                    v = q;
                    continue;
                } else {
                    out[n++] = *v;
                }
                v++;
                continue;
            }
            out[n++] = *v++;
        }
    } else if (*v == '{' || *v == '[') {
        char open = *v, close = (open == '{') ? '}' : ']';
        int depth = 0;
        int in_str = 0;
        while (*v && n < outlen - 1) {
            char c = *v;
            if (in_str) {
                if (c == '\\' && v[1]) { out[n++] = *v++; if (n < outlen-1) out[n++] = *v++; continue; }
                if (c == '"') in_str = 0;
            } else {
                if (c == '"') in_str = 1;
                else if (c == open) depth++;
                else if (c == close) depth--;
            }
            out[n++] = *v++;
            if (!in_str && depth == 0) break;
        }
    } else {
        while (*v && *v != ',' && *v != '}' && *v != ']' && !is_ws(*v) && n < outlen - 1)
            out[n++] = *v++;
    }
    out[n] = 0;
    return 1;
}

long json_get_int(const char *json, const char *key, long def)
{
    char buf[64];
    if (!json_get(json, key, buf, sizeof buf)) return def;
    char *end;
    long v = strtol(buf, &end, 10);
    return (end == buf) ? def : v;
}
