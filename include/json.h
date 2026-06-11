/*
 * json.h - tiny read-only JSON value extractor for flat/shallow objects.
 *
 * Not a full parser: it locates "key" and returns the following scalar,
 * string (unquoted), or balanced {..}/[..] substring. Sufficient for the
 * flat objects ubus returns.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef U60_JSON_H
#define U60_JSON_H

#include <stddef.h>

/* Copy the value of `key` into out (NUL-terminated). Returns 1 if found. */
int  json_get(const char *json, const char *key, char *out, size_t outlen);

/* Integer value of `key`, or `def` if missing/unparseable. */
long json_get_int(const char *json, const char *key, long def);

#endif /* U60_JSON_H */
