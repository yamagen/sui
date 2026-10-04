#ifndef LEDGER_H
#define LEDGER_H

#include <stdint.h>

#define TRIE_NONE UINT32_MAX
#define LEDGER_MAGIC 0x53554932u /* "SUI2" */
#define COMBINE_MAGIC 0x434d4231u /* "CMB1" */
#define OCCURRENCE_MAGIC 0x4f434331u /* "OCC1" */

typedef struct {
  uint32_t magic;
  uint32_t version;
  uint32_t ncombines;
} CombineSectionHeader;

typedef struct {
  uint32_t surface;
  uint32_t nfields;
  uint32_t nprovenance;
} CombineRecordHeader;

typedef struct {
  uint32_t type;
  uint32_t name_bytes;
  uint32_t value_bytes;
} LedgerFieldHeader;

typedef struct {
  uint32_t nfields;
} ProvenanceRecordHeader;

typedef struct {
  uint32_t magic;
  uint32_t version;
  uint32_t noccurrences;
} OccurrenceSectionHeader;

typedef struct {
  uint32_t surface;
  uint32_t sequence;
  uint32_t combine;
} OccurrenceRecord;

typedef struct {
  uint32_t token;
  uint32_t child;
  uint32_t sibling;
  uint32_t freq;
} TrieNode;

typedef struct {
  uint32_t magic;
  uint32_t version;
  uint32_t trie_nodes;
  uint32_t unique_surfaces;
} LedgerHeader;

#endif
