#ifndef MKLEDGER_H
#define MKLEDGER_H

#include "config.h"
#include "tiny-json.h"
#include <stdbool.h>
#include <stddef.h>

#define LINE_SIZE 4096

typedef enum {
  JSON_STRING,
  JSON_INTEGER
} JsonType;

typedef struct {
  char *name;
  JsonType type;
  char *value;
} JsonField;

typedef struct {
  JsonField *v;
  size_t n;
} JsonRecord;

typedef struct {
  JsonField *v;
  size_t n;
} Provenance;

typedef struct {
  JsonField *v;
  size_t n;
  Provenance *provenance;
  size_t nprovenance;
} Combine;

typedef struct {
  char left[LINE_SIZE];
  char right[LINE_SIZE];
} Pair;

typedef struct {
  char *surface;
  size_t sequence;
  size_t combine;
  size_t provenance;
} Token;

typedef struct {
  size_t records;
  size_t sequences;
  size_t pairs;
  Pair *pairv;
  size_t max_word_len;
  char **surfacev;
  size_t nsurfaces;
  Token *tokenv;
  size_t ntokens;
  Combine *combinev;
  size_t ncombines;
} LedgerInput;

int add_record_pair(LedgerInput *in, JsonRecord *record, JsonRecord *prev,
                    const MkledgerConfig *config, char *prev_word,
                    bool *have_prev, size_t *paircap);
char *parse_integer_string(tjson_t *json);
void free_json_record(JsonRecord *record);
JsonField *find_json_field(JsonRecord *record, const char *name);
const JsonField *find_json_field_const(const JsonRecord *record,
                                       const char *name);
int parse_json_record(const char *line, size_t lineno,
                      const MkledgerConfig *config, JsonRecord *record);
bool same_sequence(const JsonRecord *a, const JsonRecord *b,
                   const MkledgerConfig *config);

#endif