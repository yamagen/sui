#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

ConfigKey get_config_key(const char *name) {
  static const ConfigKeyMap map[] = {
      {"version", CONFIG_VERSION},       {"filename", CONFIG_FILENAME},
      {"ledger", CONFIG_LEDGER},         {"schema", CONFIG_SCHEMA},
      {"provenance", CONFIG_PROVENANCE},
  };
  const ConfigKeyMap *p;

  for (p = map; p < map + sizeof(map) / sizeof(*map); p++)
    if (strcmp(name, p->name) == 0)
      return p->key;

  return CONFIG_UNKNOWN;
}

void monitor_config(const MkledgerConfig *config) {
  const SchemaField *field;
  char **type;
  char **ignore;

  printf("version: %s\n", config->version);
  printf("filename: %s\n", config->filename);
  printf("ledger.sequence: %s\n", config->ledger.sequence);
  printf("ledger.trie: %s\n", config->ledger.trie);

  printf("ledger.ignore:");
  for (ignore = config->ledger.ignore;
       ignore < config->ledger.ignore + config->ledger.nignore; ignore++)
    printf(" %s", *ignore);
  putchar('\n');

  printf("schema: %zu\n", config->schema.n);

  for (field = config->schema.v; field < config->schema.v + config->schema.n;
       field++) {
    printf("%s:", field->field);
    for (type = field->types; type < field->types + field->ntypes; type++)
      printf(" %s", *type);
    putchar('\n');
  }

  printf("provenance:");
  for (type = config->provenance;
       type < config->provenance + config->nprovenance; type++)
    printf(" %s", *type);
  putchar('\n');
}

void parse_string_array(tjson_t *json, char ***values, size_t *nvalues) {
  char **v = NULL;
  size_t n = 0;

  tjson_expect(json, '[');

  for (;;) {
    char **p;

    tjson_skip_ws(json);
    if (tjson_peek(json) == ']') {
      tjson_expect(json, ']');
      break;
    }

    p = realloc(v, (n + 1) * sizeof(*v));
    if (p == NULL) {
      free(v);
      return;
    }

    v = p;
    v[n++] = tjson_parse_string(json);

    tjson_skip_ws(json);
    if (tjson_peek(json) == ',') {
      tjson_expect(json, ',');
      continue;
    }
    if (tjson_peek(json) == ']') {
      tjson_expect(json, ']');
      break;
    }
  }

  *values = v;
  *nvalues = n;
}

char *read_file(const char *path) {
  FILE *fp;
  long size;
  char *text;

  fp = fopen(path, "rb");
  if (fp == NULL) {
    perror(path);
    return NULL;
  }
  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    return NULL;
  }
  size = ftell(fp);
  if (size < 0) {
    fclose(fp);
    return NULL;
  }
  rewind(fp);

  text = malloc((size_t)size + 1);
  if (text == NULL) {
    fclose(fp);
    return NULL;
  }
  if (fread(text, 1, (size_t)size, fp) != (size_t)size) {
    free(text);
    fclose(fp);
    return NULL;
  }

  text[size] = '\0';
  fclose(fp);
  return text;
}

void free_mkledger_config(MkledgerConfig *config) {
  char **p;
  SchemaField *field;

  free(config->version);
  free(config->filename);
  free(config->ledger.sequence);
  free(config->ledger.trie);

  for (p = config->ledger.ignore;
       p < config->ledger.ignore + config->ledger.nignore; p++)
    free(*p);
  free(config->ledger.ignore);

  for (field = config->schema.v; field < config->schema.v + config->schema.n;
       field++) {
    char **type;
    free(field->field);
    for (type = field->types; type < field->types + field->ntypes; type++)
      free(*type);
    free(field->types);
  }
  free(config->schema.v);

  for (p = config->provenance; p < config->provenance + config->nprovenance; p++)
    free(*p);
  free(config->provenance);

  memset(config, 0, sizeof(*config));
}

int load_mkledger_config(const char *path, MkledgerConfig *config) {
  char *text;
  tjson_t json;

  memset(config, 0, sizeof(*config));
  text = read_file(path);
  if (text == NULL)
    return -1;

  tjson_init(&json, path, text);
  tjson_skip_ws(&json);
  tjson_expect(&json, '{');

  for (;;) {
    char *key;

    tjson_skip_ws(&json);
    if (tjson_peek(&json) == '}') {
      tjson_expect(&json, '}');
      break;
    }

    key = tjson_parse_string(&json);
    tjson_skip_ws(&json);
    tjson_expect(&json, ':');

    switch (get_config_key(key)) {
    case CONFIG_VERSION:
      config->version = tjson_parse_string(&json);
      break;
    case CONFIG_FILENAME:
      config->filename = tjson_parse_string(&json);
      break;
    case CONFIG_LEDGER:
      parse_ledger_config(&json, &config->ledger);
      break;
    case CONFIG_SCHEMA:
      parse_schema_config(&json, &config->schema);
      break;
    case CONFIG_PROVENANCE:
      parse_string_array(&json, &config->provenance, &config->nprovenance);
      break;
    case CONFIG_UNKNOWN:
      tjson_skip_value(&json);
      break;
    }

    free(key);
    tjson_skip_ws(&json);
    if (tjson_peek(&json) == ',') {
      tjson_expect(&json, ',');
      continue;
    }
    if (tjson_peek(&json) == '}') {
      tjson_expect(&json, '}');
      break;
    }
  }

  tjson_skip_ws(&json);
  if (json.pos != json.length) {
    free(text);
    return -1;
  }

  free(text);
  return 0;
}

void parse_ledger_config(tjson_t *json, LedgerConfig *ledger) {
  tjson_expect(json, '{');

  for (;;) {
    char *key;

    tjson_skip_ws(json);
    if (tjson_peek(json) == '}') {
      tjson_expect(json, '}');
      return;
    }

    key = tjson_parse_string(json);
    tjson_skip_ws(json);
    tjson_expect(json, ':');

    if (strcmp(key, "sequence") == 0)
      ledger->sequence = tjson_parse_string(json);
    else if (strcmp(key, "trie") == 0)
      ledger->trie = tjson_parse_string(json);
    else if (strcmp(key, "ignore") == 0)
      parse_string_array(json, &ledger->ignore, &ledger->nignore);
    else
      tjson_skip_value(json);

    free(key);
    tjson_skip_ws(json);
    if (tjson_peek(json) == ',') {
      tjson_expect(json, ',');
      continue;
    }
    if (tjson_peek(json) == '}') {
      tjson_expect(json, '}');
      return;
    }
  }
}

void parse_schema_config(tjson_t *json, Schema *schema) {
  tjson_expect(json, '{');

  for (;;) {
    SchemaField *field;
    SchemaField *p;

    tjson_skip_ws(json);
    if (tjson_peek(json) == '}') {
      tjson_expect(json, '}');
      return;
    }

    p = realloc(schema->v, (schema->n + 1) * sizeof *schema->v);
    if (p == NULL)
      return;

    schema->v = p;
    field = schema->v + schema->n;
    memset(field, 0, sizeof *field);
    field->field = tjson_parse_string(json);

    tjson_skip_ws(json);
    tjson_expect(json, ':');
    tjson_skip_ws(json);

    if (tjson_peek(json) == '[') {
      parse_string_array(json, &field->types, &field->ntypes);
    } else {
      field->types = malloc(sizeof *field->types);
      if (field->types == NULL) {
        free(field->field);
        return;
      }
      field->types[0] = tjson_parse_string(json);
      field->ntypes = 1;
    }

    schema->n++;
    tjson_skip_ws(json);
    if (tjson_peek(json) == ',') {
      tjson_expect(json, ',');
      continue;
    }
    if (tjson_peek(json) == '}') {
      tjson_expect(json, '}');
      return;
    }
  }
}
