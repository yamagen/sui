#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tiny-json.h"

static void tjson_die(const tjson_t *json, const char *message);
static void *tjson_realloc(void *ptr, size_t size);
static int hex_value(char c);
static void append_utf8(char **buffer, size_t *length, size_t *capacity,
                        unsigned codepoint);
static unsigned tjson_parse_hex4(tjson_t *json);

static void tjson_die(const tjson_t *json, const char *message) {
  fprintf(stderr, "tiny-json: %s:%zu: %s\n", json->path ? json->path : "-",
          json->line, message);
  exit(EXIT_FAILURE);
}

static void *tjson_realloc(void *ptr, size_t size) {
  void *p = realloc(ptr, size);

  if (!p) {
    fprintf(stderr, "tiny-json: realloc failed\n");
    exit(EXIT_FAILURE);
  }

  return p;
}

void tjson_init(tjson_t *json, const char *path, const char *text) {
  json->path = path;
  json->text = text;
  json->length = strlen(text);
  json->pos = 0;
  json->line = 1;
}

void tjson_skip_ws(tjson_t *json) {
  while (json->pos < json->length) {
    unsigned char c = (unsigned char)json->text[json->pos];

    if (!isspace(c))
      break;

    if (c == '\n')
      json->line++;

    json->pos++;
  }
}

char tjson_peek(tjson_t *json) {
  tjson_skip_ws(json);

  if (json->pos >= json->length)
    return '\0';

  return json->text[json->pos];
}

void tjson_expect(tjson_t *json, char expected) {
  char message[96];

  tjson_skip_ws(json);

  if (json->pos >= json->length || json->text[json->pos] != expected) {
    snprintf(message, sizeof(message), "expected '%c'", expected);
    tjson_die(json, message);
  }

  json->pos++;
}

static int hex_value(char c) {
  if ('0' <= c && c <= '9')
    return c - '0';

  if ('a' <= c && c <= 'f')
    return c - 'a' + 10;

  if ('A' <= c && c <= 'F')
    return c - 'A' + 10;

  return -1;
}

static void append_utf8(char **buffer, size_t *length, size_t *capacity,
                        unsigned codepoint) {
  unsigned char bytes[4];
  size_t count;
  size_t new_capacity;
  size_t i;

  if (codepoint <= 0x7f) {
    bytes[0] = (unsigned char)codepoint;
    count = 1;
  } else if (codepoint <= 0x7ff) {
    bytes[0] = (unsigned char)(0xc0 | (codepoint >> 6));
    bytes[1] = (unsigned char)(0x80 | (codepoint & 0x3f));
    count = 2;
  } else if (codepoint <= 0xffff) {
    bytes[0] = (unsigned char)(0xe0 | (codepoint >> 12));
    bytes[1] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3f));
    bytes[2] = (unsigned char)(0x80 | (codepoint & 0x3f));
    count = 3;
  } else {
    bytes[0] = (unsigned char)(0xf0 | (codepoint >> 18));
    bytes[1] = (unsigned char)(0x80 | ((codepoint >> 12) & 0x3f));
    bytes[2] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3f));
    bytes[3] = (unsigned char)(0x80 | (codepoint & 0x3f));
    count = 4;
  }

  if (*length + count + 1 > *capacity) {
    new_capacity = *capacity == 0 ? 32 : *capacity * 2;

    while (new_capacity < *length + count + 1)
      new_capacity *= 2;

    *buffer = tjson_realloc(*buffer, new_capacity);
    *capacity = new_capacity;
  }

  for (i = 0; i < count; i++)
    (*buffer)[(*length)++] = (char)bytes[i];
}

static unsigned tjson_parse_hex4(tjson_t *json) {
  unsigned value = 0;
  int i;
  int digit;

  for (i = 0; i < 4; i++) {
    if (json->pos >= json->length)
      tjson_die(json, "incomplete Unicode escape");

    digit = hex_value(json->text[json->pos++]);

    if (digit < 0)
      tjson_die(json, "invalid Unicode escape");

    value = (value << 4) | (unsigned)digit;
  }

  return value;
}

char *tjson_parse_string(tjson_t *json) {
  char *result = NULL;
  size_t length = 0;
  size_t capacity = 0;

  tjson_skip_ws(json);

  if (json->pos >= json->length || json->text[json->pos] != '"')
    tjson_die(json, "expected string");

  json->pos++;

  while (json->pos < json->length) {
    unsigned char c = (unsigned char)json->text[json->pos++];

    if (c == '"') {
      if (length + 1 > capacity) {
        capacity = length + 1;
        result = tjson_realloc(result, capacity);
      }

      result[length] = '\0';
      return result;
    }

    if (c < 0x20)
      tjson_die(json, "control character in string");

    if (c == '\\') {
      char escape;

      if (json->pos >= json->length)
        tjson_die(json, "incomplete escape sequence");

      escape = json->text[json->pos++];

      switch (escape) {
      case '"':
        c = '"';
        break;

      case '\\':
        c = '\\';
        break;

      case '/':
        c = '/';
        break;

      case 'b':
        c = '\b';
        break;

      case 'f':
        c = '\f';
        break;

      case 'n':
        c = '\n';
        break;

      case 'r':
        c = '\r';
        break;

      case 't':
        c = '\t';
        break;

      case 'u': {
        unsigned codepoint = tjson_parse_hex4(json);

        if (0xd800 <= codepoint && codepoint <= 0xdbff) {
          unsigned low;

          if (json->pos + 2 > json->length || json->text[json->pos] != '\\' ||
              json->text[json->pos + 1] != 'u')
            tjson_die(json, "missing low surrogate");

          json->pos += 2;
          low = tjson_parse_hex4(json);

          if (low < 0xdc00 || low > 0xdfff)
            tjson_die(json, "invalid low surrogate");

          codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
        } else if (0xdc00 <= codepoint && codepoint <= 0xdfff) {
          tjson_die(json, "unexpected low surrogate");
        }

        append_utf8(&result, &length, &capacity, codepoint);
        continue;
      }

      default:
        tjson_die(json, "invalid escape sequence");
      }
    }

    if (length + 2 > capacity) {
      size_t new_capacity = capacity == 0 ? 32 : capacity * 2;

      result = tjson_realloc(result, new_capacity);
      capacity = new_capacity;
    }

    result[length++] = (char)c;
  }

  free(result);
  tjson_die(json, "unterminated string");

  return NULL;
}

bool tjson_consume_literal(tjson_t *json, const char *literal) {
  size_t length;

  tjson_skip_ws(json);

  length = strlen(literal);

  if (json->length - json->pos < length ||
      strncmp(json->text + json->pos, literal, length) != 0)
    return false;

  json->pos += length;

  return true;
}

double tjson_parse_number(tjson_t *json) {
  const char *start;
  char *end = NULL;
  double value;

  tjson_skip_ws(json);

  start = json->text + json->pos;

  errno = 0;
  value = strtod(start, &end);

  if (errno != 0 || end == start || !isfinite(value))
    tjson_die(json, "expected finite number");

  json->pos += (size_t)(end - start);

  return value;
}

bool tjson_parse_bool(tjson_t *json) {
  if (tjson_consume_literal(json, "true"))
    return true;

  if (tjson_consume_literal(json, "false"))
    return false;

  tjson_die(json, "expected true or false");

  return false;
}

void tjson_skip_array(tjson_t *json) {
  tjson_expect(json, '[');

  if (tjson_peek(json) == ']') {
    json->pos++;
    return;
  }

  for (;;) {
    tjson_skip_value(json);

    if (tjson_peek(json) == ']') {
      json->pos++;
      return;
    }

    tjson_expect(json, ',');
  }
}

void tjson_skip_object(tjson_t *json) {
  tjson_expect(json, '{');

  if (tjson_peek(json) == '}') {
    json->pos++;
    return;
  }

  for (;;) {
    char *key = tjson_parse_string(json);

    free(key);

    tjson_expect(json, ':');
    tjson_skip_value(json);

    if (tjson_peek(json) == '}') {
      json->pos++;
      return;
    }

    tjson_expect(json, ',');
  }
}

void tjson_skip_value(tjson_t *json) {
  char c = tjson_peek(json);

  if (c == '{') {
    tjson_skip_object(json);
  } else if (c == '[') {
    tjson_skip_array(json);
  } else if (c == '"') {
    char *s = tjson_parse_string(json);
    free(s);
  } else if (c == '-' || isdigit((unsigned char)c)) {
    (void)tjson_parse_number(json);
  } else if (tjson_consume_literal(json, "true") ||
             tjson_consume_literal(json, "false") ||
             tjson_consume_literal(json, "null")) {
    return;
  } else {
    tjson_die(json, "invalid JSON value");
  }
}

char *tjson_parse_nullable_string(tjson_t *json) {
  if (tjson_consume_literal(json, "null"))
    return NULL;

  return tjson_parse_string(json);
}

#ifdef TJTEST

int main(int argc, char **argv) {
  FILE *fp;
  char *text;
  long size;
  tjson_t json;

  if (argc != 2) {
    fprintf(stderr, "usage: tiny-json FILE\n");
    return EXIT_FAILURE;
  }

  fp = fopen(argv[1], "rb");
  if (!fp) {
    perror(argv[1]);
    return EXIT_FAILURE;
  }

  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    return EXIT_FAILURE;
  }

  size = ftell(fp);

  if (size < 0) {
    fclose(fp);
    return EXIT_FAILURE;
  }

  rewind(fp);

  text = malloc((size_t)size + 1);

  if (!text) {
    fclose(fp);
    return EXIT_FAILURE;
  }

  if (fread(text, 1, (size_t)size, fp) != (size_t)size) {
    free(text);
    fclose(fp);
    return EXIT_FAILURE;
  }

  text[size] = '\0';
  fclose(fp);

  tjson_init(&json, argv[1], text);
  tjson_skip_value(&json);
  tjson_skip_ws(&json);

  if (json.pos != json.length) {
    fprintf(stderr, "tiny-json: %s:%zu: trailing content\n", json.path,
            json.line);
    free(text);
    return EXIT_FAILURE;
  }

  free(text);

  return EXIT_SUCCESS;
}

#endif
