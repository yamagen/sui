#ifndef TINY_JSON_H
#define TINY_JSON_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
  const char *path;
  const char *text;
  size_t length;
  size_t pos;
  size_t line;
} tjson_t;

void tjson_init(tjson_t *json, const char *path, const char *text);

void tjson_skip_ws(tjson_t *json);
char tjson_peek(tjson_t *json);
void tjson_expect(tjson_t *json, char expected);

char *tjson_parse_string(tjson_t *json);
double tjson_parse_number(tjson_t *json);
bool tjson_parse_bool(tjson_t *json);
bool tjson_consume_literal(tjson_t *json, const char *literal);

void tjson_skip_value(tjson_t *json);
void tjson_skip_array(tjson_t *json);
void tjson_skip_object(tjson_t *json);

char *tjson_parse_nullable_string(tjson_t *json);

#endif
