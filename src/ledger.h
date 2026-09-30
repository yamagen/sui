#ifndef LEDGER_H
#define LEDGER_H

#include <stdint.h>

#define TRIE_NONE UINT32_MAX
#define LEDGER_MAGIC 0x53554932u /* "SUI2" */

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
