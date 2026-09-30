#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ledger.h"

typedef struct {
  size_t start;
  size_t end;
  size_t depth;
  uint32_t freq;
} LongestPath;

typedef struct {
  LongestPath *v;
  size_t n;
  size_t cap;
} LongestPathList;

typedef struct {
  void *map;
  size_t size;
  const LedgerHeader *header;
  const TrieNode *trie;
  const uint32_t *surface_offset;
  const char *surface_strings;
} Ledger;

typedef struct {
  size_t start;
  size_t end;
  uint32_t surface;
} LatticeEdge;

typedef struct {
  LatticeEdge *v;
  size_t n;
  size_t cap;
} Lattice;

typedef struct {
  size_t start;
  size_t end;
} Gap;

typedef struct {
  Gap *v;
  size_t n;
  size_t cap;
} GapList;

static void longest_path_list_init(LongestPathList *list) {
  list->v = NULL;
  list->n = 0;
  list->cap = 0;
}

static void longest_path_list_free(LongestPathList *list) {
  free(list->v);
  list->v = NULL;
  list->n = 0;
  list->cap = 0;
}

static int longest_path_list_add(LongestPathList *list,
                                 const LongestPath *path) {
  LongestPath *tmp;

  if (list->n == list->cap) {
    size_t newcap = (list->cap == 0) ? 16 : list->cap * 2;

    tmp = realloc(list->v, newcap * sizeof *list->v);
    if (tmp == NULL)
      return -1;

    list->v = tmp;
    list->cap = newcap;
  }

  list->v[list->n++] = *path;
  return 0;
}

static int is_separator(const char *input, size_t start, size_t end) {
  size_t len = end - start;

  if (len == 3 && (memcmp(input + start, "、", 3) == 0 ||
                   memcmp(input + start, "。", 3) == 0))
    return 1;

  return 0;
}

static int lattice_edges_connect(const char *input, const LatticeEdge *a,
                                 const LatticeEdge *b) {
  if (a->end == b->start)
    return 1;

  if (a->end < b->start && is_separator(input, a->end, b->start))
    return 1;

  return 0;
}

static void lattice_init(Lattice *lat) {
  lat->v = NULL;
  lat->n = 0;
  lat->cap = 0;
}

static void lattice_free(Lattice *lat) {
  free(lat->v);
  lat->v = NULL;
  lat->n = 0;
  lat->cap = 0;
}

static int lattice_add(Lattice *lat, size_t start, size_t end,
                       uint32_t surface) {
  LatticeEdge *tmp;

  if (lat->n == lat->cap) {
    size_t newcap = (lat->cap == 0) ? 256 : lat->cap * 2;

    tmp = realloc(lat->v, newcap * sizeof *lat->v);
    if (tmp == NULL)
      return -1;

    lat->v = tmp;
    lat->cap = newcap;
  }

  lat->v[lat->n].start = start;
  lat->v[lat->n].end = end;
  lat->v[lat->n].surface = surface;
  lat->n++;

  return 0;
}

static void gaplist_init(GapList *gaps) {
  gaps->v = NULL;
  gaps->n = 0;
  gaps->cap = 0;
}

static void gaplist_free(GapList *gaps) {
  free(gaps->v);
  gaps->v = NULL;
  gaps->n = 0;
  gaps->cap = 0;
}

static int gaplist_add(GapList *gaps, size_t start, size_t end) {
  Gap *tmp;

  if (gaps->n == gaps->cap) {
    size_t newcap = (gaps->cap == 0) ? 16 : gaps->cap * 2;

    tmp = realloc(gaps->v, newcap * sizeof *gaps->v);
    if (tmp == NULL)
      return -1;

    gaps->v = tmp;
    gaps->cap = newcap;
  }

  gaps->v[gaps->n].start = start;
  gaps->v[gaps->n].end = end;
  gaps->n++;

  return 0;
}

static size_t utf8_next(const char *s, size_t pos) {
  unsigned char c = (unsigned char)s[pos];

  if (c < 0x80)
    return pos + 1;
  if ((c & 0xe0) == 0xc0)
    return pos + 2;
  if ((c & 0xf0) == 0xe0)
    return pos + 3;
  if ((c & 0xf8) == 0xf0)
    return pos + 4;

  return pos + 1;
}

static int find_lattice_gaps(const char *input, const Lattice *lat,
                             GapList *gaps) {
  size_t input_len = strlen(input);
  size_t pos = 0;

  while (pos < input_len) {
    size_t i;
    size_t covered_end = pos;

    for (i = 0; i < lat->n; i++) {
      if (lat->v[i].start <= pos && lat->v[i].end > covered_end)
        covered_end = lat->v[i].end;
    }

    if (covered_end > pos) {
      pos = covered_end;
      continue;
    }

    {
      size_t start = pos;

      pos = utf8_next(input, pos);

      while (pos < input_len) {
        int covered = 0;

        for (i = 0; i < lat->n; i++) {
          if (lat->v[i].start <= pos && lat->v[i].end > pos) {
            covered = 1;
            break;
          }
        }

        if (covered)
          break;

        pos = utf8_next(input, pos);
      }

      if (gaplist_add(gaps, start, pos) != 0)
        return -1;
    }
  }

  return 0;
}

static int show_lattice_gaps(const char *input, const Lattice *lat) {
  GapList gaps;
  size_t i;

  gaplist_init(&gaps);

  if (find_lattice_gaps(input, lat, &gaps) != 0) {
    gaplist_free(&gaps);
    return -1;
  }

  for (i = 0; i < gaps.n; i++) {
    size_t len = gaps.v[i].end - gaps.v[i].start;

    printf("gap\t%zu\t%zu\t%.*s\n", gaps.v[i].start, gaps.v[i].end, (int)len,
           input + gaps.v[i].start);
  }

  gaplist_free(&gaps);
  return 0;
}

static void unload_ledger(Ledger *ledger) { munmap(ledger->map, ledger->size); }

static int load_ledger(const char *path, Ledger *ledger) {
  int fd;
  struct stat st;

  fd = open(path, O_RDONLY);
  if (fd < 0) {
    perror(path);
    return -1;
  }

  if (fstat(fd, &st) != 0) {
    perror(path);
    close(fd);
    return -1;
  }

  ledger->size = (size_t)st.st_size;

  if (ledger->size < sizeof(LedgerHeader)) {
    fprintf(stderr, "ledger too small\n");
    close(fd);
    return -1;
  }

  ledger->map = mmap(NULL, ledger->size, PROT_READ, MAP_PRIVATE, fd, 0);

  close(fd);

  if (ledger->map == MAP_FAILED) {
    perror("mmap");
    return -1;
  }

  ledger->header = (const LedgerHeader *)ledger->map;

  if (ledger->header->magic != LEDGER_MAGIC) {
    fprintf(stderr, "invalid ledger magic\n");
    munmap(ledger->map, ledger->size);
    return -1;
  }

  ledger->trie =
      (const TrieNode *)((const char *)ledger->map + sizeof(LedgerHeader));

  ledger->surface_offset =
      (const uint32_t *)(ledger->trie + ledger->header->trie_nodes);

  ledger->surface_strings =
      (const char *)(ledger->surface_offset + ledger->header->unique_surfaces);

  return 0;
}

static int make_lattice(const Ledger *ledger, const char *input, Lattice *lat) {
  size_t input_len = strlen(input);
  size_t start;

  for (start = 0; start < input_len; start = utf8_next(input, start)) {
    uint32_t id;

    for (id = 0; id < ledger->header->unique_surfaces; id++) {
      const char *surface =
          ledger->surface_strings + ledger->surface_offset[id];
      size_t len = strlen(surface);

      if (start + len <= input_len &&
          memcmp(input + start, surface, len) == 0) {
        if (lattice_add(lat, start, start + len, id) != 0)
          return -1;
      }
    }
  }

  return 0;
}

static uint32_t trie_find(const Ledger *ledger, uint32_t node,
                          uint32_t surface) {
  uint32_t p = ledger->trie[node].child;

  while (p != TRIE_NONE) {
    if (ledger->trie[p].token == surface)
      return p;

    p = ledger->trie[p].sibling;
  }

  return TRIE_NONE;
}

static void follow_path(const Ledger *ledger, const char *input,
                        const Lattice *lat, size_t edge_index, uint32_t node,
                        size_t depth) {
  const LatticeEdge *edge = &lat->v[edge_index];
  const char *surface =
      ledger->surface_strings + ledger->surface_offset[edge->surface];
  size_t i;

  printf("%*s%s\tfreq=%u\n", (int)(depth * 2), "", surface,
         ledger->trie[node].freq);

  for (i = 0; i < lat->n; i++) {
    const LatticeEdge *next = &lat->v[i];
    uint32_t child;

    if (!lattice_edges_connect(input, edge, next))
      continue;

    child = trie_find(ledger, node, next->surface);
    if (child == TRIE_NONE)
      continue;

    follow_path(ledger, input, lat, i, child, depth + 1);
  }
}

static void show_paths(const Ledger *ledger, const char *input,
                       const Lattice *lat) {
  size_t i;

  for (i = 0; i < lat->n; i++) {
    uint32_t node;

    node = trie_find(ledger, 0, lat->v[i].surface);
    if (node == TRIE_NONE)
      continue;

    follow_path(ledger, input, lat, i, node, 0);
  }
}

static void find_longest_from(const Ledger *ledger, const char *input,
                              const Lattice *lat, size_t edge_index,
                              uint32_t node, size_t depth, LongestPath *best) {
  const LatticeEdge *edge = &lat->v[edge_index];
  size_t i;

  if (edge->end > best->end ||
      (edge->end == best->end && depth > best->depth)) {
    best->end = edge->end;
    best->depth = depth;
    best->freq = ledger->trie[node].freq;
  }

  for (i = 0; i < lat->n; i++) {
    const LatticeEdge *next = &lat->v[i];
    uint32_t child;

    if (!lattice_edges_connect(input, edge, next))
      continue;

    child = trie_find(ledger, node, next->surface);
    if (child == TRIE_NONE)
      continue;

    find_longest_from(ledger, input, lat, i, child, depth + 1, best);
  }
}

static int make_longest_paths(const Ledger *ledger, const char *input,
                              const Lattice *lat, LongestPathList *list) {
  size_t i;

  for (i = 0; i < lat->n; i++) {
    uint32_t node;
    LongestPath best;

    node = trie_find(ledger, 0, lat->v[i].surface);
    if (node == TRIE_NONE)
      continue;

    best.start = lat->v[i].start;
    best.end = lat->v[i].end;
    best.depth = 1;
    best.freq = ledger->trie[node].freq;

    find_longest_from(ledger, input, lat, i, node, 1, &best);

    if (longest_path_list_add(list, &best) != 0)
      return -1;
  }

  return 0;
}

static void show_longest_paths(const char *input, const LongestPathList *list) {
  size_t i;

  for (i = 0; i < list->n; i++) {
    const LongestPath *p = &list->v[i];

    printf("longest\t%zu\t%zu\tdepth=%zu\tfreq=%u\t%.*s\n", p->start, p->end,
           p->depth, p->freq, (int)(p->end - p->start), input + p->start);
  }
}

static int process_input(const Ledger *ledger) {
  char input[4096];
  Lattice lat;

  if (fgets(input, sizeof input, stdin) == NULL)
    return -1;

  input[strcspn(input, "\r\n")] = '\0';

  lattice_init(&lat);

  if (make_lattice(ledger, input, &lat) != 0) {
    lattice_free(&lat);
    return -1;
  }

  if (show_lattice_gaps(input, &lat) != 0) {
    lattice_free(&lat);
    return -1;
  }

  printf("lattice edges: %zu\n", lat.n);

  for (size_t i = 0; i < lat.n; i++) {
    const LatticeEdge *e = &lat.v[i];
    const char *surface =
        ledger->surface_strings + ledger->surface_offset[e->surface];

    printf("%zu\t%zu\t%u\t%s\n", e->start, e->end, e->surface, surface);
  }

  show_paths(ledger, input, &lat);

  LongestPathList paths;

  longest_path_list_init(&paths);

  if (make_longest_paths(ledger, input, &lat, &paths) != 0) {
    longest_path_list_free(&paths);
    lattice_free(&lat);
    return -1;
  }

  show_longest_paths(input, &paths);

  longest_path_list_free(&paths);

  lattice_free(&lat);
  return 0;
}

int main(int argc, char *argv[]) {
  Ledger ledger;

  if (argc != 2) {
    fprintf(stderr, "usage: %s ledger.dat\n", argv[0]);
    return EXIT_FAILURE;
  }

  if (load_ledger(argv[1], &ledger) != 0)
    return EXIT_FAILURE;

  printf("version:         %u\n", ledger.header->version);
  printf("trie nodes:      %u\n", ledger.header->trie_nodes);
  printf("unique surfaces: %u\n", ledger.header->unique_surfaces);

  printf("root token:      %u\n", ledger.trie[0].token);
  printf("root child:      %u\n", ledger.trie[0].child);
  printf("root sibling:    %u\n", ledger.trie[0].sibling);
  printf("root freq:       %u\n", ledger.trie[0].freq);

  if (process_input(&ledger) != 0) {
    unload_ledger(&ledger);
    return EXIT_FAILURE;
  }

  unload_ledger(&ledger);
  return EXIT_SUCCESS;
}
