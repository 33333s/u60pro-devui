/*
 * json.c - tiny read-only JSON value extractor. See json.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "json.h"
#include <stdlib.h>
#include <string.h>

static int is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

/* Return a pointer just past the ':' of "key": , or NULL. */
static const char *find_key(const char *json, const char *key)
{
    size_t klen = strlen(key);
    const char *p = json;
    while ((p = strchr(p, '"')) != NULL) {
        if (strncmp(p + 1, key, klen) == 0 && p[1 + klen] == '"') {
            const char *q = p + 1 + klen + 1;
            while (is_ws(*q)) q++;
            if (*q == ':') return q + 1;
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
            if (*v == '\\' && v[1]) v++;   /* keep escaped char literally */
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
