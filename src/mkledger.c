#include "mkledger.h"
#include "ledger.h"
#include "tiny-json.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VERSION "0.1.0"

typedef struct {
  TrieNode *v;
  size_t n;
  size_t cap;
} Trie;

typedef struct {
  size_t left;
  size_t right;
  size_t freq;
} PairID;

typedef struct {
  PairID *v;
  size_t n;
  size_t unique_pairs;
  size_t max_freq;
} PairTable;

char *parse_integer_string(tjson_t *json) {
  size_t start = json->pos;
  size_t end;
  const char *p;

  (void)tjson_parse_number(json);
  end = json->pos;

  for (p = json->text + start; p < json->text + end; p++)
    if (*p == '.' || *p == 'e' || *p == 'E')
      return NULL;

  return strndup(json->text + start, end - start);
}

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

static void trie_free(Trie *trie) {
  free(trie->v);
  trie->v = NULL;
  trie->n = 0;
  trie->cap = 0;
}

static void trie_init(Trie *trie) {
  trie->v = NULL;
  trie->n = 0;
  trie->cap = 0;
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

  free(config->version);
  free(config->filename);
  free(config->ledger.sequence);
  free(config->ledger.trie);

  for (p = config->ledger.ignore;
       p < config->ledger.ignore + config->ledger.nignore; p++)
    free(*p);

  free(config->ledger.ignore);

  {
    SchemaField *field;

    for (field = config->schema.v; field < config->schema.v + config->schema.n;
         field++) {
      char **type;

      free(field->field);

      for (type = field->types; type < field->types + field->ntypes; type++)
        free(*type);

      free(field->types);
    }
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

static int trie_add_node(Trie *trie, uint32_t token) {
  TrieNode *tmp;

  if (trie->n == trie->cap) {
    size_t newcap = (trie->cap == 0) ? 1024 : trie->cap * 2;

    tmp = realloc(trie->v, newcap * sizeof *trie->v);
    if (tmp == NULL)
      return -1;

    trie->v = tmp;
    trie->cap = newcap;
  }

  trie->v[trie->n].token = token;
  trie->v[trie->n].child = TRIE_NONE;
  trie->v[trie->n].sibling = TRIE_NONE;
  trie->v[trie->n].freq = 0;

  trie->n++;

  return 0;
}

static int add_pair(Pair **pairv, size_t *npairs, size_t *cap, const char *left,
                    const char *right) {
  Pair *tmp;

  if (*npairs == *cap) {
    size_t newcap = (*cap == 0) ? 1024 : *cap * 2;

    tmp = realloc(*pairv, newcap * sizeof **pairv);
    if (tmp == NULL)
      return -1;

    *pairv = tmp;
    *cap = newcap;
  }

  if (strlen(left) >= sizeof(*pairv)[*npairs].left ||
      strlen(right) >= sizeof(*pairv)[*npairs].right)
    return -1;

  strcpy((*pairv)[*npairs].left, left);
  strcpy((*pairv)[*npairs].right, right);

  (*npairs)++;
  return 0;
}

static int compare_pair(const void *a, const void *b) {
  const Pair *pa = a;
  const Pair *pb = b;
  int c;

  c = strcmp(pa->left, pb->left);
  if (c != 0)
    return c;

  return strcmp(pa->right, pb->right);
}

static int add_surface(char ***surfacev, size_t *n, size_t *cap,
                       const char *surface) {
  char **tmp;
  char *s;

  if (*n == *cap) {
    size_t newcap = (*cap == 0) ? 1024 : *cap * 2;

    tmp = realloc(*surfacev, newcap * sizeof **surfacev);
    if (tmp == NULL)
      return -1;

    *surfacev = tmp;
    *cap = newcap;
  }

  s = strdup(surface);
  if (s == NULL)
    return -1;

  (*surfacev)[*n] = s;
  (*n)++;

  return 0;
}

static int add_token(Token **tokenv, size_t *n, size_t *cap,
                     const char *surface, size_t sequence) {
  Token *tmp;
  char *s;

  if (*n == *cap) {
    size_t newcap = (*cap == 0) ? 1024 : *cap * 2;

    tmp = realloc(*tokenv, newcap * sizeof **tokenv);
    if (tmp == NULL)
      return -1;

    *tokenv = tmp;
    *cap = newcap;
  }

  s = strdup(surface);
  if (s == NULL)
    return -1;

  (*tokenv)[*n].surface = s;
  (*tokenv)[*n].sequence = sequence;
  (*n)++;

  return 0;
}

static int compare_surface(const void *a, const void *b) {
  const char *const *sa = a;
  const char *const *sb = b;

  return strcmp(*sa, *sb);
}

static size_t find_surface(char **surfacev, size_t n, const char *surface) {
  char **found;

  found = bsearch(&surface, surfacev, n, sizeof *surfacev, compare_surface);

  if (found == NULL)
    return SIZE_MAX;

  return (size_t)(found - surfacev);
}

void free_json_record(JsonRecord *record) {
  JsonField *field;

  for (field = record->v; field < record->v + record->n; field++) {
    free(field->name);
    free(field->value);
  }

  free(record->v);
  record->v = NULL;
  record->n = 0;
}

JsonField *find_json_field(JsonRecord *record, const char *name) {
  JsonField *field;

  for (field = record->v; field < record->v + record->n; field++)
    if (strcmp(field->name, name) == 0)
      return field;

  return NULL;
}

const JsonField *find_json_field_const(const JsonRecord *record,
                                       const char *name) {
  const JsonField *field;

  for (field = record->v; field < record->v + record->n; field++)
    if (strcmp(field->name, name) == 0)
      return field;

  return NULL;
}

static const SchemaField *find_schema_field(const Schema *schema,
                                            const char *name) {
  const SchemaField *field;

  for (field = schema->v; field < schema->v + schema->n; field++)
    if (strcmp(field->field, name) == 0)
      return field;

  return NULL;
}

static const char *json_type_name(JsonType type) {
  switch (type) {
  case JSON_STRING:
    return "string";
  case JSON_INTEGER:
    return "integer";
  }

  return NULL;
}

static bool schema_accepts_type(const SchemaField *field, JsonType type) {
  char **p;
  const char *name = json_type_name(type);

  if (name == NULL)
    return false;

  for (p = field->types; p < field->types + field->ntypes; p++)
    if (strcmp(*p, name) == 0)
      return true;

  return false;
}

static int add_json_field(tjson_t *json, const char *name, size_t lineno,
                          const SchemaField *schema, JsonRecord *record) {
  JsonField *tmp;
  JsonField *field;
  JsonType type;
  char *value;

  if (find_json_field(record, name) != NULL) {
    fprintf(stderr, "duplicate %s at line %zu\n", name, lineno);
    return -1;
  }

  if (tjson_peek(json) == '"') {
    type = JSON_STRING;
    value = tjson_parse_string(json);
  } else {
    type = JSON_INTEGER;
    value = parse_integer_string(json);
  }

  if (value == NULL) {
    fprintf(stderr, "invalid integer for %s at line %zu\n", name, lineno);
    return -1;
  }

  if (!schema_accepts_type(schema, type)) {
    fprintf(stderr, "invalid type for %s at line %zu\n", name, lineno);
    free(value);
    return -1;
  }

  tmp = realloc(record->v, (record->n + 1) * sizeof *record->v);
  if (tmp == NULL) {
    free(value);
    return -1;
  }

  record->v = tmp;
  field = record->v + record->n;
  field->name = strdup(name);
  if (field->name == NULL) {
    free(value);
    return -1;
  }

  field->type = type;
  field->value = value;
  record->n++;

  return 0;
}

int parse_json_record(const char *line, size_t lineno,
                      const MkledgerConfig *config, JsonRecord *record) {
  tjson_t json;
  char path[64];

  memset(record, 0, sizeof *record);

  snprintf(path, sizeof path, "line %zu", lineno);
  tjson_init(&json, path, line);

  tjson_skip_ws(&json);
  tjson_expect(&json, '{');

  for (;;) {
    char *key;
    const SchemaField *schema;

    tjson_skip_ws(&json);

    if (tjson_peek(&json) == '}') {
      tjson_expect(&json, '}');
      break;
    }

    key = tjson_parse_string(&json);

    tjson_skip_ws(&json);
    tjson_expect(&json, ':');
    tjson_skip_ws(&json);

    schema = find_schema_field(&config->schema, key);

    if (schema == NULL) {
      tjson_skip_value(&json);
    } else if (add_json_field(&json, key, lineno, schema, record) != 0) {
      free(key);
      free_json_record(record);
      return -1;
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

    fprintf(stderr, "invalid JSON object at line %zu\n", lineno);
    free_json_record(record);
    return -1;
  }

  tjson_skip_ws(&json);

  if (json.pos != json.length) {
    fprintf(stderr, "trailing content at line %zu\n", lineno);
    free_json_record(record);
    return -1;
  }

  {
    const SchemaField *field;

    for (field = config->schema.v; field < config->schema.v + config->schema.n;
         field++) {
      if (find_json_field_const(record, field->field) == NULL) {
        fprintf(stderr, "missing %s at line %zu\n", field->field, lineno);
        free_json_record(record);
        return -1;
      }
    }
  }

  return 0;
}

static bool is_provenance_field(const MkledgerConfig *config,
                                const char *name) {
  char **field;

  for (field = config->provenance;
       field < config->provenance + config->nprovenance; field++)
    if (strcmp(*field, name) == 0)
      return true;

  return false;
}

static void free_provenance(Provenance *provenance) {
  JsonField *field;

  for (field = provenance->v; field < provenance->v + provenance->n; field++) {
    free(field->name);
    free(field->value);
  }

  free(provenance->v);
  provenance->v = NULL;
  provenance->n = 0;
}

static void free_combine(Combine *combine) {
  JsonField *field;
  Provenance *provenance;

  for (field = combine->v; field < combine->v + combine->n; field++) {
    free(field->name);
    free(field->value);
  }

  for (provenance = combine->provenance;
       provenance < combine->provenance + combine->nprovenance; provenance++)
    free_provenance(provenance);

  free(combine->v);
  free(combine->provenance);

  combine->v = NULL;
  combine->n = 0;
  combine->provenance = NULL;
  combine->nprovenance = 0;
}

static int copy_json_field(JsonField *dst, const JsonField *src) {
  dst->name = strdup(src->name);
  dst->value = strdup(src->value);
  dst->type = src->type;

  if (dst->name == NULL || dst->value == NULL) {
    free(dst->name);
    free(dst->value);
    dst->name = NULL;
    dst->value = NULL;
    return -1;
  }

  return 0;
}

static int make_provenance(const JsonRecord *record,
                           const MkledgerConfig *config,
                           Provenance *provenance) {
  char **name;

  memset(provenance, 0, sizeof *provenance);

  if (config->nprovenance == 0)
    return 0;

  provenance->v = calloc(config->nprovenance, sizeof *provenance->v);
  if (provenance->v == NULL)
    return -1;

  for (name = config->provenance;
       name < config->provenance + config->nprovenance; name++) {
    const JsonField *field = find_json_field_const(record, *name);

    if (field == NULL) {
      fprintf(stderr, "provenance field %s is not in record\n", *name);
      free_provenance(provenance);
      return -1;
    }

    if (copy_json_field(provenance->v + provenance->n, field) != 0) {
      free_provenance(provenance);
      return -1;
    }

    provenance->n++;
  }

  return 0;
}

static int make_combine_fields(const JsonRecord *record,
                               const MkledgerConfig *config,
                               Combine *combine) {
  const SchemaField *schema;
  size_t n = 0;

  memset(combine, 0, sizeof *combine);

  for (schema = config->schema.v; schema < config->schema.v + config->schema.n;
       schema++)
    if (!is_provenance_field(config, schema->field))
      n++;

  if (n == 0)
    return 0;

  combine->v = calloc(n, sizeof *combine->v);
  if (combine->v == NULL)
    return -1;

  for (schema = config->schema.v; schema < config->schema.v + config->schema.n;
       schema++) {
    const JsonField *field;

    if (is_provenance_field(config, schema->field))
      continue;

    field = find_json_field_const(record, schema->field);
    if (field == NULL) {
      fprintf(stderr, "combine field %s is not in record\n", schema->field);
      free_combine(combine);
      return -1;
    }

    if (copy_json_field(combine->v + combine->n, field) != 0) {
      free_combine(combine);
      return -1;
    }

    combine->n++;
  }

  return 0;
}

static bool same_combine_fields(const Combine *left, const Combine *right) {
  const JsonField *a;
  const JsonField *b;

  if (left->n != right->n)
    return false;

  a = left->v;
  b = right->v;

  while (a < left->v + left->n) {
    if (a->type != b->type || strcmp(a->name, b->name) != 0 ||
        strcmp(a->value, b->value) != 0)
      return false;

    a++;
    b++;
  }

  return true;
}

static int append_provenance(Combine *combine, Provenance *provenance) {
  Provenance *tmp;

  tmp = realloc(combine->provenance,
                (combine->nprovenance + 1) * sizeof *combine->provenance);
  if (tmp == NULL)
    return -1;

  combine->provenance = tmp;
  combine->provenance[combine->nprovenance] = *provenance;
  combine->nprovenance++;

  provenance->v = NULL;
  provenance->n = 0;

  return 0;
}

static int add_combine(LedgerInput *in, const JsonRecord *record,
                       const MkledgerConfig *config) {
  Combine candidate = {0};
  Provenance provenance = {0};
  Combine *combine;
  Combine *tmp;

  if (make_combine_fields(record, config, &candidate) != 0)
    return -1;

  if (make_provenance(record, config, &provenance) != 0) {
    free_combine(&candidate);
    return -1;
  }

  for (combine = in->combinev; combine < in->combinev + in->ncombines;
       combine++) {
    if (!same_combine_fields(combine, &candidate))
      continue;

    free_combine(&candidate);

    if (append_provenance(combine, &provenance) != 0) {
      free_provenance(&provenance);
      return -1;
    }

    return 0;
  }

  tmp = realloc(in->combinev, (in->ncombines + 1) * sizeof *in->combinev);
  if (tmp == NULL) {
    free_combine(&candidate);
    free_provenance(&provenance);
    return -1;
  }

  in->combinev = tmp;
  combine = in->combinev + in->ncombines;
  *combine = candidate;
  in->ncombines++;

  if (append_provenance(combine, &provenance) != 0) {
    in->ncombines--;
    free_combine(combine);
    return -1;
  }

  return 0;
}

bool same_sequence(const JsonRecord *a, const JsonRecord *b,
                   const MkledgerConfig *config) {
  const JsonField *left;
  const JsonField *right;

  left = find_json_field_const(a, config->ledger.sequence);
  right = find_json_field_const(b, config->ledger.sequence);

  if (left == NULL || right == NULL || left->type != right->type)
    return false;

  return strcmp(left->value, right->value) == 0;
}

int add_record_pair(LedgerInput *in, JsonRecord *record, JsonRecord *prev,
                    const MkledgerConfig *config, char *prev_word,
                    bool *have_prev, size_t *paircap) {
  const JsonField *sequence;
  const JsonField *trie;

  sequence = find_json_field_const(record, config->ledger.sequence);
  trie = find_json_field_const(record, config->ledger.trie);

  if (sequence == NULL || trie == NULL)
    return -1;

  if (!*have_prev || !same_sequence(record, prev, config)) {
    in->sequences++;

    if (*have_prev &&
        add_pair(&in->pairv, &in->pairs, paircap, prev_word, "EOS") != 0)
      return -1;

    if (add_pair(&in->pairv, &in->pairs, paircap, "BOS", trie->value) != 0)
      return -1;

    free_json_record(prev);

    prev->v = calloc(1, sizeof *prev->v);
    if (prev->v == NULL)
      return -1;

    prev->v->name = strdup(config->ledger.sequence);
    prev->v->value = strdup(sequence->value);
    prev->v->type = sequence->type;

    if (prev->v->name == NULL || prev->v->value == NULL) {
      prev->n = 1;
      free_json_record(prev);
      return -1;
    }

    prev->n = 1;
    *have_prev = true;
    return 0;
  }

  if (add_pair(&in->pairv, &in->pairs, paircap, prev_word, trie->value) != 0)
    return -1;

  return 0;
}

static int read_ledger(FILE *fp, const MkledgerConfig *config,
                       LedgerInput *in) {
  char line[LINE_SIZE];
  char prev_word[LINE_SIZE] = "";
  JsonRecord prev = {0};
  bool have_prev = false;
  size_t paircap = 0;
  size_t surfacecap = 0;
  size_t tokencap = 0;

  memset(in, 0, sizeof *in);

  while (fgets(line, sizeof line, fp) != NULL) {
    JsonRecord record = {0};
    const JsonField *trie;
    size_t lineno = in->records + 1;
    size_t word_len;

    line[strcspn(line, "\n")] = '\0';

    if (parse_json_record(line, lineno, config, &record) != 0) {
      free_json_record(&prev);
      return -1;
    }

    if (add_combine(in, &record, config) != 0) {
      fprintf(stderr, "cannot add combine at line %zu\n", lineno);
      free_json_record(&record);
      free_json_record(&prev);
      return -1;
    }

    trie = find_json_field_const(&record, config->ledger.trie);
    if (trie == NULL) {
      fprintf(stderr, "missing %s at line %zu\n", config->ledger.trie, lineno);
      free_json_record(&record);
      free_json_record(&prev);
      return -1;
    }

    word_len = strlen(trie->value);

    if (word_len > in->max_word_len)
      in->max_word_len = word_len;

    if (add_surface(&in->surfacev, &in->nsurfaces, &surfacecap, trie->value) !=
        0) {
      fprintf(stderr, "cannot add surface at line %zu\n", lineno);
      free_json_record(&record);
      free_json_record(&prev);
      return -1;
    }

    if (add_record_pair(in, &record, &prev, config, prev_word, &have_prev,
                        &paircap) != 0) {
      free_json_record(&record);
      free_json_record(&prev);
      return -1;
    }

    if (strlen(trie->value) >= sizeof prev_word) {
      fprintf(stderr, "%s too long at line %zu\n", config->ledger.trie,
              lineno);
      free_json_record(&record);
      free_json_record(&prev);
      return -1;
    }

    if (add_token(&in->tokenv, &in->ntokens, &tokencap, trie->value,
                  in->sequences - 1) != 0) {
      fprintf(stderr, "cannot add token at line %zu\n", lineno);
      free_json_record(&record);
      free_json_record(&prev);
      return -1;
    }

    strcpy(prev_word, trie->value);

    free_json_record(&record);
    in->records++;
  }

  if (have_prev &&
      add_pair(&in->pairv, &in->pairs, &paircap, prev_word, "EOS") != 0) {
    free_json_record(&prev);
    return -1;
  }

  free_json_record(&prev);

  if (ferror(fp))
    return -1;

  return 0;
}

static size_t make_surface_table(char **surfacev, size_t nsurfaces) {
  size_t unique_surfaces = 0;

  qsort(surfacev, nsurfaces, sizeof *surfacev, compare_surface);

  for (size_t i = 0; i < nsurfaces;) {
    size_t j = i + 1;

    while (j < nsurfaces && strcmp(surfacev[i], surfacev[j]) == 0) {
      free(surfacev[j]);
      j++;
    }

    surfacev[unique_surfaces++] = surfacev[i];
    i = j;
  }

  return unique_surfaces;
}

static PairTable make_pair_table(Pair *pairv, size_t pairs, char **surfacev,
                                 size_t unique_surfaces) {
  PairTable table = {0};
  size_t id_bos = unique_surfaces;
  size_t id_eos = unique_surfaces + 1;

  table.v = malloc(pairs * sizeof *table.v);
  if (table.v == NULL)
    return table;

  qsort(pairv, pairs, sizeof *pairv, compare_pair);

  for (size_t i = 0; i < pairs;) {
    size_t j = i + 1;

    while (j < pairs && strcmp(pairv[i].left, pairv[j].left) == 0 &&
           strcmp(pairv[i].right, pairv[j].right) == 0) {
      j++;
    }

    size_t freq = j - i;
    size_t left_id;
    size_t right_id;

    if (strcmp(pairv[i].left, "BOS") == 0)
      left_id = id_bos;
    else
      left_id = find_surface(surfacev, unique_surfaces, pairv[i].left);

    if (strcmp(pairv[i].right, "EOS") == 0)
      right_id = id_eos;
    else
      right_id = find_surface(surfacev, unique_surfaces, pairv[i].right);

    if (left_id == SIZE_MAX || right_id == SIZE_MAX) {
      free(table.v);
      table.v = NULL;
      table.n = 0;
      return table;
    }

    table.v[table.n].left = left_id;
    table.v[table.n].right = right_id;
    table.v[table.n].freq = freq;
    table.n++;

    table.unique_pairs++;

    if (freq > table.max_freq)
      table.max_freq = freq;

    i = j;
  }

  return table;
}

static void show_pair_freq(const PairTable *table) {
  for (size_t i = 0; i < table->n; i++)
    printf("%zu\n", table->v[i].freq);
}

static void show_combine(const LedgerInput *in) {
  const Combine *combine;
  const JsonField *field;
  const Provenance *provenance;
  size_t n;

  for (combine = in->combinev; combine < in->combinev + in->ncombines;
       combine++) {
    if (combine->nprovenance < 2)
      continue;

    printf("combine:\n");
    for (field = combine->v; field < combine->v + combine->n; field++)
      printf("  %s: %s\n", field->name, field->value);

    printf("provenance: %zu\n", combine->nprovenance);
    n = 0;
    for (provenance = combine->provenance;
         provenance < combine->provenance + combine->nprovenance;
         provenance++) {
      printf("  [%zu]\n", n++);
      for (field = provenance->v; field < provenance->v + provenance->n;
           field++)
        printf("    %s: %s\n", field->name, field->value);
    }

    return;
  }

  printf("no combine with multiple provenance\n");
}

static size_t count_provenance(const LedgerInput *in) {
  const Combine *combine;
  size_t nprovenance = 0;

  for (combine = in->combinev; combine < in->combinev + in->ncombines;
       combine++)
    nprovenance += combine->nprovenance;

  return nprovenance;
}

static void show_stat(const LedgerInput *in, const PairTable *pair_table,
                      size_t unique_surfaces) {
  printf("records:   %zu\n", in->records);
  printf("combines:  %zu\n", in->ncombines);
  printf("provenance: %zu\n", count_provenance(in));
  printf("sequences: %zu\n", in->sequences);
  printf("pairs:     %zu\n", in->pairs);
  printf("unique:    %zu\n", pair_table->unique_pairs);
  printf("max freq:  %zu\n", pair_table->max_freq);
  printf("max word bytes: %zu\n", in->max_word_len);
  printf("surfaces:   %zu\n", in->nsurfaces);
  printf("unique surfaces: %zu\n", unique_surfaces);
  printf("pair IDs:   %zu\n", pair_table->n);
  printf("BOS id:     %zu\n", unique_surfaces);
  printf("EOS id:     %zu\n", unique_surfaces + 1);

  for (size_t i = 0; i < unique_surfaces && i < 10; i++)
    printf("surface[%zu]: %s\n", i, in->surfacev[i]);
}

static void free_mem(LedgerInput *in, PairTable *pair_table,
                     size_t unique_surfaces) {
  Combine *combine;

  for (combine = in->combinev; combine < in->combinev + in->ncombines;
       combine++)
    free_combine(combine);

  for (size_t i = 0; i < unique_surfaces; i++)
    free(in->surfacev[i]);

  for (size_t i = 0; i < in->ntokens; i++)
    free(in->tokenv[i].surface);

  free(in->combinev);
  free(in->tokenv);
  free(in->surfacev);
  free(in->pairv);
  free(pair_table->v);
}

static uint32_t trie_find_child(const Trie *trie, uint32_t parent,
                                uint32_t token) {
  uint32_t child = trie->v[parent].child;

  while (child != TRIE_NONE) {
    if (trie->v[child].token == token)
      return child;

    child = trie->v[child].sibling;
  }

  return TRIE_NONE;
}

static uint32_t trie_add_child(Trie *trie, uint32_t parent, uint32_t token) {
  uint32_t child;

  child = trie_find_child(trie, parent, token);
  if (child != TRIE_NONE)
    return child;

  if (trie->n >= UINT32_MAX)
    return TRIE_NONE;

  child = (uint32_t)trie->n;

  if (trie_add_node(trie, token) != 0)
    return TRIE_NONE;

  trie->v[child].sibling = trie->v[parent].child;
  trie->v[parent].child = child;

  return child;
}

static int trie_add_sequence(Trie *trie, const uint32_t *tokens,
                             size_t ntokens) {
  uint32_t parent = 0;

  for (size_t i = 0; i < ntokens; i++) {
    uint32_t child = trie_add_child(trie, parent, tokens[i]);

    if (child == TRIE_NONE)
      return -1;

    trie->v[child].freq++;
    parent = child;
  }

  return 0;
}

static int trie_add_suffixes(Trie *trie, const uint32_t *tokens,
                             size_t ntokens) {
  for (size_t i = 0; i < ntokens; i++) {
    if (trie_add_sequence(trie, tokens + i, ntokens - i) != 0)
      return -1;
  }

  return 0;
}

static int make_trie(Trie *trie, const LedgerInput *in,
                     size_t unique_surfaces) {
  uint32_t *tokens = NULL;
  size_t ntokens = 0;
  size_t cap = 0;
  size_t sequence = SIZE_MAX;

  trie_init(trie);

  if (trie_add_node(trie, TRIE_NONE) != 0)
    return -1;

  for (size_t i = 0; i < in->ntokens; i++) {
    if (sequence != SIZE_MAX && in->tokenv[i].sequence != sequence) {
      if (trie_add_suffixes(trie, tokens, ntokens) != 0) {
        free(tokens);
        trie_free(trie);
        return -1;
      }
      ntokens = 0;
    }

    sequence = in->tokenv[i].sequence;

    if (ntokens == cap) {
      size_t newcap = (cap == 0) ? 64 : cap * 2;
      uint32_t *tmp = realloc(tokens, newcap * sizeof *tokens);

      if (tmp == NULL) {
        free(tokens);
        trie_free(trie);
        return -1;
      }

      tokens = tmp;
      cap = newcap;
    }

    size_t id =
        find_surface(in->surfacev, unique_surfaces, in->tokenv[i].surface);

    if (id == SIZE_MAX || id >= UINT32_MAX) {
      free(tokens);
      trie_free(trie);
      return -1;
    }

    tokens[ntokens++] = (uint32_t)id;
  }

  if (ntokens > 0 && trie_add_suffixes(trie, tokens, ntokens) != 0) {
    free(tokens);
    trie_free(trie);
    return -1;
  }

  free(tokens);
  return 0;
}
static int write_field(FILE *fp, const JsonField *field) {
  LedgerFieldHeader header;
  size_t name_bytes = strlen(field->name) + 1;
  size_t value_bytes = strlen(field->value) + 1;

  if (name_bytes > UINT32_MAX || value_bytes > UINT32_MAX)
    return -1;

  header.type = (uint32_t)field->type;
  header.name_bytes = (uint32_t)name_bytes;
  header.value_bytes = (uint32_t)value_bytes;

  if (fwrite(&header, sizeof header, 1, fp) != 1 ||
      fwrite(field->name, 1, name_bytes, fp) != name_bytes ||
      fwrite(field->value, 1, value_bytes, fp) != value_bytes)
    return -1;

  return 0;
}

static int write_combine_section(FILE *fp, const LedgerInput *in,
                                 const MkledgerConfig *config,
                                 char **surfacev, size_t unique_surfaces) {
  CombineSectionHeader section;
  const Combine *combine;

  if (in->ncombines > UINT32_MAX)
    return -1;

  section.magic = COMBINE_MAGIC;
  section.version = 1;
  section.ncombines = (uint32_t)in->ncombines;

  if (fwrite(&section, sizeof section, 1, fp) != 1)
    return -1;

  for (combine = in->combinev; combine < in->combinev + in->ncombines;
       combine++) {
    CombineRecordHeader record;
    const JsonField *trie;
    const JsonField *field;
    const Provenance *provenance;
    size_t surface;

    trie = find_json_field_const((const JsonRecord *)combine,
                                 config->ledger.trie);
    if (trie == NULL)
      return -1;

    surface = find_surface(surfacev, unique_surfaces, trie->value);
    if (surface == SIZE_MAX || surface > UINT32_MAX ||
        combine->n > UINT32_MAX || combine->nprovenance > UINT32_MAX)
      return -1;

    record.surface = (uint32_t)surface;
    record.nfields = (uint32_t)combine->n;
    record.nprovenance = (uint32_t)combine->nprovenance;

    if (fwrite(&record, sizeof record, 1, fp) != 1)
      return -1;

    for (field = combine->v; field < combine->v + combine->n; field++)
      if (write_field(fp, field) != 0)
        return -1;

    for (provenance = combine->provenance;
         provenance < combine->provenance + combine->nprovenance;
         provenance++) {
      ProvenanceRecordHeader provenance_header;

      if (provenance->n > UINT32_MAX)
        return -1;

      provenance_header.nfields = (uint32_t)provenance->n;

      if (fwrite(&provenance_header, sizeof provenance_header, 1, fp) != 1)
        return -1;

      for (field = provenance->v; field < provenance->v + provenance->n;
           field++)
        if (write_field(fp, field) != 0)
          return -1;
    }
  }

  return 0;
}

static int write_ledger(const char *path, const Trie *trie,
                        const LedgerInput *in, const MkledgerConfig *config,
                        char **surfacev, size_t unique_surfaces) {
  FILE *fp;
  LedgerHeader header;

  if (trie->n > UINT32_MAX || unique_surfaces > UINT32_MAX)
    return -1;

  header.magic = LEDGER_MAGIC;
  header.version = 1;
  header.trie_nodes = (uint32_t)trie->n;
  header.unique_surfaces = (uint32_t)unique_surfaces;

  fp = fopen(path, "wb");
  if (fp == NULL) {
    perror(path);
    return -1;
  }

  if (fwrite(&header, sizeof header, 1, fp) != 1) {
    perror(path);
    fclose(fp);
    return -1;
  }

  if (fwrite(trie->v, sizeof *trie->v, trie->n, fp) != trie->n) {
    perror(path);
    fclose(fp);
    return -1;
  }

  uint32_t offset = 0;

  for (size_t i = 0; i < unique_surfaces; i++) {
    if (fwrite(&offset, sizeof offset, 1, fp) != 1) {
      perror(path);
      fclose(fp);
      return -1;
    }

    size_t len = strlen(surfacev[i]) + 1;

    if (len > UINT32_MAX - offset) {
      fprintf(stderr, "surface table too large\n");
      fclose(fp);
      return -1;
    }

    offset += (uint32_t)len;
  }

  for (size_t i = 0; i < unique_surfaces; i++) {
    size_t len = strlen(surfacev[i]) + 1;

    if (fwrite(surfacev[i], 1, len, fp) != len) {
      perror(path);
      fclose(fp);
      return -1;
    }
  }

  if (write_combine_section(fp, in, config, surfacev, unique_surfaces) != 0) {
    fprintf(stderr, "cannot write combine section\n");
    fclose(fp);
    return -1;
  }

  if (fclose(fp) != 0) {
    perror(path);
    return -1;
  }

  return 0;
}

static int mkledger(LedgerInput *in, const MkledgerConfig *config,
                    int show_stats, int show_freq, int show_combine_record) {
  size_t unique_surfaces = make_surface_table(in->surfacev, in->nsurfaces);

  PairTable pair_table =
      make_pair_table(in->pairv, in->pairs, in->surfacev, unique_surfaces);

  // trie starts
  Trie trie;

  if (make_trie(&trie, in, unique_surfaces) != 0) {
    fprintf(stderr, "cannot build trie\n");
    free_mem(in, &pair_table, unique_surfaces);
    return -1;
  }

  if (write_ledger("ledger.dat", &trie, in, config, in->surfacev,
                   unique_surfaces) != 0) {
    fprintf(stderr, "cannot write ledger.dat\n");
    trie_free(&trie);
    free_mem(in, &pair_table, unique_surfaces);
    return -1;
  }

  if (show_freq)
    show_pair_freq(&pair_table);

  if (show_stats) {
    show_stat(in, &pair_table, unique_surfaces);
    printf("trie nodes: %zu\n", trie.n);
  }

  if (show_combine_record)
    show_combine(in);

  trie_free(&trie);
  // trie ends

  if (pair_table.v == NULL) {
    fprintf(stderr, "cannot build pair table\n");
    return -1;
  }

  free_mem(in, &pair_table, unique_surfaces);

  return 0;
}

static void usage(const char *prog) {
  printf("Usage: %s [options] [ledger.in]\n", prog);
  printf("Compile ledger input into ledger.dat\n");
  printf("Options:\n");
  printf("  -c  set config file\n");
  printf("  -f  output pair frequencies\n");
  printf("  -m  monitor config\n");
  printf("  -p  show one combine with multiple provenance\n");
  printf("  -s  show statistics\n");
  printf("  -h  show this help\n");
  printf("  -v  show version\n");
}

int main(int argc, char *argv[]) {
  FILE *fp;
  int show_stats = 0;
  int show_freq = 0;
  int show_combine_record = 0;
  int opt;
  MkledgerConfig config;
  int monitor = 0;
  const char *config_path = "ledger-config.json";

  while ((opt = getopt(argc, argv, "c:fmpshv")) != -1) {
    switch (opt) {
    case 'f':
      show_freq = 1;
      break;
    case 'c':
      config_path = optarg;
      break;
    case 'm':
      monitor = 1;
      break;
    case 'p':
      show_combine_record = 1;
      break;
    case 's':
      show_stats = 1;
      break;
    case 'h':
      usage(argv[0]);
      return EXIT_SUCCESS;
    case 'v':
      printf("mkledger %s\n", VERSION);
      return EXIT_SUCCESS;
    default:
      usage(argv[0]);
      return EXIT_FAILURE;
    }
  }

  if (optind == argc) {
    fp = stdin;
  } else if (optind + 1 == argc) {
    fp = fopen(argv[optind], "r");
    if (fp == NULL) {
      perror(argv[optind]);
      usage(argv[0]);
      return EXIT_FAILURE;
    }
  } else {
    usage(argv[0]);
    return EXIT_FAILURE;
  }

  if (load_mkledger_config(config_path, &config) != 0)
    return EXIT_FAILURE;

  if (monitor)
    monitor_config(&config);

  LedgerInput in;

  if (read_ledger(fp, &config, &in) != 0) {
    fprintf(stderr, "cannot read ledger\n");
    fclose(fp);
    return EXIT_FAILURE;
  }

  if (fp != stdin)
    fclose(fp);

  if (mkledger(&in, &config, show_stats, show_freq, show_combine_record) != 0) {
    free_mkledger_config(&config);
    return EXIT_FAILURE;
  }

  free_mkledger_config(&config);
  return EXIT_SUCCESS;
}