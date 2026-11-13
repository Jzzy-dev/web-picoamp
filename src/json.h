// json.h - a streaming JSON reader (see json.c)
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define JSON_DEPTH  8
#define JSON_VALUE  1       // a value: path, its text, index in the innermost array
#define JSON_END    2       // an object ended (path = where it was)

typedef struct json json_t;
typedef void (*json_cb)(json_t *j, int ev, const char *path, const char *val, uint16_t idx);
struct json
{
    json_cb  cb;
    void    *user;
    uint8_t  depth;
    bool     is_arr[JSON_DEPTH];
    uint16_t idx[JSON_DEPTH];
    char     name[JSON_DEPTH][32];
    bool     in_str, esc, is_key, expect_key, have_val, seen, done;
    uint8_t  uhex, klen;
    uint32_t ucp;
    char     key[32];
    char     val[2048];
    uint16_t vlen;
    char     pbuf[160];
};
void json_reset(json_t *j);                 // keeps cb and user
void json_byte(json_t *j, char c);
void json_feed(json_t *j, const char *p, size_t n);
