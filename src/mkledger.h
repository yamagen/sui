#ifndef MKLEDGER_H
#define MKLEDGER_H

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

typedef struct {
  char *sequence;
  char *trie;
  char **ignore;
  size_t nignore;
} LedgerConfig;

typedef struct {
  char *field;
  char **types;
  size_t ntypes;
} SchemaField;

typedef struct {
  SchemaField *v;
  size_t n;
} Schema;

typedef struct {
  char *version;
  char *filename;
  LedgerConfig ledger;
  Schema schema;
  char **provenance;
  size_t nprovenance;
} MkledgerConfig;

typedef enum {
  CONFIG_VERSION,
  CONFIG_FILENAME,
  CONFIG_LEDGER,
  CONFIG_SCHEMA,
  CONFIG_PROVENANCE,
  CONFIG_UNKNOWN
} ConfigKey;

typedef struct {
  const char *name;
  ConfigKey key;
} ConfigKeyMap;

int add_record_pair(LedgerInput *in, JsonRecord *record, JsonRecord *prev,
                    const MkledgerConfig *config, char *prev_word,
                    bool *have_prev, size_t *paircap);
char *parse_integer_string(tjson_t *json);
ConfigKey get_config_key(const char *name);
void monitor_config(const MkledgerConfig *config);
void parse_string_array(tjson_t *json, char ***values, size_t *nvalues);
char *read_file(const char *path);
void free_mkledger_config(MkledgerConfig *config);
int load_mkledger_config(const char *path, MkledgerConfig *config);
void parse_ledger_config(tjson_t *json, LedgerConfig *ledger);
void parse_schema_config(tjson_t *json, Schema *schema);
void free_json_record(JsonRecord *record);
JsonField *find_json_field(JsonRecord *record, const char *name);
const JsonField *find_json_field_const(const JsonRecord *record,
                                       const char *name);
int parse_json_record(const char *line, size_t lineno,
                      const MkledgerConfig *config, JsonRecord *record);
bool same_sequence(const JsonRecord *a, const JsonRecord *b,
                   const MkledgerConfig *config);

#endif