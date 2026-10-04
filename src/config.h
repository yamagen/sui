#ifndef CONFIG_H
#define CONFIG_H

#include "tiny-json.h"
#include <stddef.h>

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
  char *filename;
} CandyConfig;

typedef struct {
  char *version;
  char *filename;
  LedgerConfig ledger;
  Schema schema;
  char **provenance;
  size_t nprovenance;
  CandyConfig candy;
} MkledgerConfig;

typedef enum {
  CONFIG_VERSION,
  CONFIG_FILENAME,
  CONFIG_LEDGER,
  CONFIG_SCHEMA,
  CONFIG_PROVENANCE,
  CONFIG_CANDY,
  CONFIG_UNKNOWN
} ConfigKey;

typedef struct {
  const char *name;
  ConfigKey key;
} ConfigKeyMap;

ConfigKey get_config_key(const char *name);
void monitor_config(const MkledgerConfig *config);
void parse_string_array(tjson_t *json, char ***values, size_t *nvalues);
char *read_file(const char *path);
void free_mkledger_config(MkledgerConfig *config);
int load_mkledger_config(const char *path, MkledgerConfig *config);
void parse_ledger_config(tjson_t *json, LedgerConfig *ledger);
void parse_schema_config(tjson_t *json, Schema *schema);
void parse_candy_config(tjson_t *json, CandyConfig *candy);

#endif
