#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_SIZE 4096
#define NFIELDS 5
#define TRIE_NONE UINT32_MAX

typedef struct {
  uint32_t token;
  uint32_t child;
  uint32_t sibling;
  uint32_t freq;
} TrieNode;

typedef struct {
  TrieNode *v;
  size_t n;
  size_t cap;
} Trie;

#define LEDGER_MAGIC 0x53554932u /* "SUI2" */

typedef struct {
  uint32_t magic;
  uint32_t version;
  uint32_t trie_nodes;
  uint32_t unique_surfaces;
} LedgerHeader;

typedef struct {
  char left[LINE_SIZE];
  char right[LINE_SIZE];
} Pair;

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
  size_t freq_dist[100];
} PairTable;

typedef struct {
  char *surface;
  size_t sequence;
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
} LedgerInput;

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

static int read_ledger(FILE *fp, LedgerInput *in) {
  char line[LINE_SIZE];
  char prev_id[256] = "";
  char prev_word[LINE_SIZE] = "";
  size_t paircap = 0;
  size_t surfacecap = 0;
  size_t tokencap = 0;

  memset(in, 0, sizeof *in);

  while (fgets(line, sizeof line, fp) != NULL) {
    char *field[NFIELDS];
    char *p = line;
    int n = 0;

    line[strcspn(line, "\n")] = '\0';

    field[n++] = p;

    while (n < NFIELDS && (p = strchr(p, '\t')) != NULL) {
      *p++ = '\0';
      field[n++] = p;
    }

    if (n != NFIELDS) {
      fprintf(stderr, "invalid record at line %zu\n", in->records + 1);
      return -1;
    }

    size_t word_len = strlen(field[2]);

    if (word_len > in->max_word_len)
      in->max_word_len = word_len;

    if (add_surface(&in->surfacev, &in->nsurfaces, &surfacecap, field[2]) !=
        0) {
      fprintf(stderr, "cannot add surface at line %zu\n", in->records + 1);
      return -1;
    }

    if (strcmp(field[0], prev_id) != 0) {
      in->sequences++;

      if (in->records > 0) {
        if (add_pair(&in->pairv, &in->pairs, &paircap, prev_word, "EOS") != 0)
          return -1;
      }

      if (add_pair(&in->pairv, &in->pairs, &paircap, "BOS", field[2]) != 0)
        return -1;

      if (strlen(field[0]) >= sizeof prev_id) {
        fprintf(stderr, "id too long at line %zu\n", in->records + 1);
        return -1;
      }

      strcpy(prev_id, field[0]);

    } else {
      if (add_pair(&in->pairv, &in->pairs, &paircap, prev_word, field[2]) != 0)
        return -1;
    }

    if (strlen(field[2]) >= sizeof prev_word) {
      fprintf(stderr, "word too long at line %zu\n", in->records + 1);
      return -1;
    }

    if (add_token(&in->tokenv, &in->ntokens, &tokencap, field[2],
                  in->sequences - 1) != 0) {
      fprintf(stderr, "cannot add token at line %zu\n", in->records + 1);
      return -1;
    }

    strcpy(prev_word, field[2]);
    in->records++;
  }

  if (in->records > 0) {
    if (add_pair(&in->pairv, &in->pairs, &paircap, prev_word, "EOS") != 0)
      return -1;
  }

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

    if (freq < 100)
      table.freq_dist[freq]++;

    i = j;
  }

  return table;
}

static void show_stat(const LedgerInput *in, const PairTable *pair_table,
                      size_t unique_surfaces) {
  printf("records:   %zu\n", in->records);
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

  printf("\nfrequency distribution:\n");

  for (size_t i = 1; i < 100; i++) {
    if (pair_table->freq_dist[i] > 0)
      printf("%2zu: %zu\n", i, pair_table->freq_dist[i]);
  }
}

static void free_mem(LedgerInput *in, PairTable *pair_table,
                     size_t unique_surfaces) {
  for (size_t i = 0; i < unique_surfaces; i++)
    free(in->surfacev[i]);

  for (size_t i = 0; i < in->ntokens; i++)
    free(in->tokenv[i].surface);

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

static int write_ledger(const char *path, const Trie *trie,
                        size_t unique_surfaces) {
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

  if (fclose(fp) != 0) {
    perror(path);
    return -1;
  }

  return 0;
}

static int mkledger(LedgerInput *in) {
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

  printf("trie nodes: %zu\n", trie.n);

  if (write_ledger("ledger.dat", &trie, unique_surfaces) != 0) {
    fprintf(stderr, "cannot write ledger.dat\n");
    trie_free(&trie);
    free_mem(in, &pair_table, unique_surfaces);
    return -1;
  }

  trie_free(&trie);
  // trie ends

  if (pair_table.v == NULL) {
    fprintf(stderr, "cannot build pair table\n");
    return -1;
  }

  show_stat(in, &pair_table, unique_surfaces);

  free_mem(in, &pair_table, unique_surfaces);

  return 0;
}

int main(int argc, char *argv[]) {
  FILE *fp;

  if (argc != 2) {
    fprintf(stderr, "usage: %s ledger.in\n", argv[0]);
    return EXIT_FAILURE;
  }

  fp = fopen(argv[1], "r");
  if (fp == NULL) {
    perror(argv[1]);
    return EXIT_FAILURE;
  }

  LedgerInput in;

  if (read_ledger(fp, &in) != 0) {
    fprintf(stderr, "cannot read ledger\n");
    fclose(fp);
    return EXIT_FAILURE;
  }

  fclose(fp);

  if (mkledger(&in) != 0)
    return EXIT_FAILURE;

  return EXIT_SUCCESS;
}
