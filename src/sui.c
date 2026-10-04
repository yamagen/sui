#include <fcntl.h>
#include <stdbool.h>
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
  uint32_t surface;
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
  const CombineSectionHeader *combine;
  const OccurrenceSectionHeader *occurrence;
  const OccurrenceRecord *occurrencev;
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

static bool reach_covers_adjacency(const LongestPathList *list,
                                   const LatticeEdge *a, const LatticeEdge *b);
static void show_uncovered_adjacencies(const Ledger *ledger, const char *input,
                                       const Lattice *lat,
                                       const LongestPathList *list);

static void longest_path_list_init(LongestPathList *list);
static void longest_path_list_free(LongestPathList *list);
static int longest_path_list_add(LongestPathList *list,
                                 const LongestPath *path);

static int is_separator(const char *input, size_t start, size_t end);
static int lattice_edges_connect(const char *input, const LatticeEdge *a,
                                 const LatticeEdge *b);

static void lattice_init(Lattice *lat);
static void lattice_free(Lattice *lat);
static int lattice_add(Lattice *lat, size_t start, size_t end,
                       uint32_t surface);

static void gaplist_init(GapList *gaps);
static void gaplist_free(GapList *gaps);
static int gaplist_add(GapList *gaps, size_t start, size_t end);
static size_t utf8_next(const char *s, size_t pos);
static int find_lattice_gaps(const char *input, const Lattice *lat,
                             GapList *gaps);
static int show_lattice_gaps(const char *input, const Lattice *lat);
static void unload_ledger(Ledger *ledger);
static int load_ledger(const char *path, Ledger *ledger);
static int make_lattice(const Ledger *ledger, const char *input, Lattice *lat);
static uint32_t trie_find(const Ledger *ledger, uint32_t node,
                          uint32_t surface);
static void follow_path(const Ledger *ledger, const char *input,
                        const Lattice *lat, size_t edge_index, uint32_t node,
                        size_t depth);
static void show_paths(const Ledger *ledger, const char *input,
                       const Lattice *lat);
static void find_longest_from(const Ledger *ledger, const char *input,
                              const Lattice *lat, size_t edge_index,
                              uint32_t node, size_t depth, LongestPath *best);
static int make_longest_paths(const Ledger *ledger, const char *input,
                              const Lattice *lat, LongestPathList *list);
static void show_longest_paths(const char *input, const LongestPathList *list);
static void show_reach(const LongestPathList *list);
static void show_occurrence_adjacencies(const Ledger *ledger,
                                         const char *input,
                                         const Lattice *lat);
static void show_observed_paths(const Ledger *ledger, const char *input,
                                const Lattice *lat);
static int process_input(const Ledger *ledger, int monitor);

static bool reach_covers_adjacency(const LongestPathList *list,
                                   const LatticeEdge *a, const LatticeEdge *b) {
  const LongestPath *path;

  for (path = list->v; path < list->v + list->n; path++) {
    if (path->start <= a->start && path->end >= b->end)
      return true;
  }

  return false;
}

static void show_uncovered_adjacencies(const Ledger *ledger, const char *input,
                                       const Lattice *lat,
                                       const LongestPathList *list) {
  const LatticeEdge *a;
  const LatticeEdge *b;
  const char *as;
  const char *bs;

  for (a = lat->v; a < lat->v + lat->n; a++) {
    for (b = lat->v; b < lat->v + lat->n; b++) {
      if (!lattice_edges_connect(input, a, b))
        continue;

      if (reach_covers_adjacency(list, a, b))
        continue;

      as = ledger->surface_strings + ledger->surface_offset[a->surface];
      bs = ledger->surface_strings + ledger->surface_offset[b->surface];

      printf("add an adjacency of A with B: %.*s[%.*s]%s\n", (int)a->start,
             input, (int)(b->end - a->start), input + a->start, input + b->end);

      printf("{\"word\":\"%s\"}\n", as);
      printf("{\"word\":\"%s\"}\n", bs);
    }
  }
}

static void show_reach(const LongestPathList *list) {
  const LongestPath *path;
  const LongestPath *other;
  bool is_longest;

  for (path = list->v; path < list->v + list->n; path++) {
    is_longest = true;

    for (other = list->v; other < list->v + list->n; other++) {
      if (other->start == path->start && other->end > path->end) {
        is_longest = false;
        break;
      }
    }

    if (is_longest)
      printf("reach\t%zu\t%zu\tdepth=%zu\tfreq=%u\n", path->start, path->end,
             path->depth, path->freq);
  }
}

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

static const char *skip_ledger_field(const char *p, const char *end) {
  const LedgerFieldHeader *field;

  if ((size_t)(end - p) < sizeof *field)
    return NULL;

  field = (const LedgerFieldHeader *)p;
  p += sizeof *field;

  if (field->name_bytes > (size_t)(end - p))
    return NULL;
  p += field->name_bytes;

  if (field->value_bytes > (size_t)(end - p))
    return NULL;

  return p + field->value_bytes;
}

static int find_combine_section(Ledger *ledger) {
  const char *p;
  const char *end;
  uint32_t i;

  end = (const char *)ledger->map + ledger->size;
  p = ledger->surface_strings;

  for (i = 0; i < ledger->header->unique_surfaces; i++) {
    size_t remaining = (size_t)(end - p);
    const char *nul = memchr(p, '\0', remaining);

    if (nul == NULL)
      return -1;

    p = nul + 1;
  }

  if ((size_t)(end - p) < sizeof *ledger->combine)
    return -1;

  ledger->combine = (const CombineSectionHeader *)p;

  if (ledger->combine->magic != COMBINE_MAGIC || ledger->combine->version != 1)
    return -1;

  return 0;
}

static int find_occurrence_section(Ledger *ledger) {
  const char *p = (const char *)(ledger->combine + 1);
  const char *end = (const char *)ledger->map + ledger->size;
  uint32_t i;

  for (i = 0; i < ledger->combine->ncombines; i++) {
    const CombineRecordHeader *combine;
    uint32_t j;

    if ((size_t)(end - p) < sizeof *combine)
      return -1;

    combine = (const CombineRecordHeader *)p;
    p += sizeof *combine;

    for (j = 0; j < combine->nfields; j++) {
      p = skip_ledger_field(p, end);
      if (p == NULL)
        return -1;
    }

    for (j = 0; j < combine->nprovenance; j++) {
      const ProvenanceRecordHeader *provenance;
      uint32_t k;

      if ((size_t)(end - p) < sizeof *provenance)
        return -1;

      provenance = (const ProvenanceRecordHeader *)p;
      p += sizeof *provenance;

      for (k = 0; k < provenance->nfields; k++) {
        p = skip_ledger_field(p, end);
        if (p == NULL)
          return -1;
      }
    }
  }

  if ((size_t)(end - p) < sizeof *ledger->occurrence)
    return -1;

  ledger->occurrence = (const OccurrenceSectionHeader *)p;

  if (ledger->occurrence->magic != OCCURRENCE_MAGIC ||
      ledger->occurrence->version != 2)
    return -1;

  p += sizeof *ledger->occurrence;

  if (ledger->occurrence->noccurrences >
      (size_t)(end - p) / sizeof *ledger->occurrencev)
    return -1;

  ledger->occurrencev = (const OccurrenceRecord *)p;
  p += (size_t)ledger->occurrence->noccurrences * sizeof *ledger->occurrencev;

  return p == end ? 0 : -1;
}

static int count_ledger_provenance(const Ledger *ledger,
                                   uint32_t *nprovenance) {
  const char *p = (const char *)(ledger->combine + 1);
  const char *end = (const char *)ledger->occurrence;
  uint32_t total = 0;
  uint32_t i;

  for (i = 0; i < ledger->combine->ncombines; i++) {
    const CombineRecordHeader *combine;
    uint32_t j;

    if ((size_t)(end - p) < sizeof *combine)
      return -1;

    combine = (const CombineRecordHeader *)p;
    p += sizeof *combine;

    for (j = 0; j < combine->nfields; j++) {
      p = skip_ledger_field(p, end);
      if (p == NULL)
        return -1;
    }

    if (combine->nprovenance > UINT32_MAX - total)
      return -1;
    total += combine->nprovenance;

    for (j = 0; j < combine->nprovenance; j++) {
      const ProvenanceRecordHeader *provenance;
      uint32_t k;

      if ((size_t)(end - p) < sizeof *provenance)
        return -1;

      provenance = (const ProvenanceRecordHeader *)p;
      p += sizeof *provenance;

      for (k = 0; k < provenance->nfields; k++) {
        p = skip_ledger_field(p, end);
        if (p == NULL)
          return -1;
      }
    }
  }

  if (p != end)
    return -1;

  *nprovenance = total;
  return 0;
}

static void print_json_string(const char *s) {
  const unsigned char *p;

  putchar('"');

  for (p = (const unsigned char *)s; *p != '\0'; p++) {
    switch (*p) {
    case '"':
      fputs("\\\"", stdout);
      break;
    case '\\':
      fputs("\\\\", stdout);
      break;
    case '\b':
      fputs("\\b", stdout);
      break;
    case '\f':
      fputs("\\f", stdout);
      break;
    case '\n':
      fputs("\\n", stdout);
      break;
    case '\r':
      fputs("\\r", stdout);
      break;
    case '\t':
      fputs("\\t", stdout);
      break;
    default:
      if (*p < 0x20)
        printf("\\u%04x", *p);
      else
        putchar(*p);
    }
  }

  putchar('"');
}

static const char *print_ledger_field_json(const char *p, const char *end) {
  const LedgerFieldHeader *field;
  const char *name;
  const char *value;

  if ((size_t)(end - p) < sizeof *field)
    return NULL;

  field = (const LedgerFieldHeader *)p;
  p += sizeof *field;

  if (field->name_bytes == 0 || field->name_bytes > (size_t)(end - p))
    return NULL;
  name = p;
  p += field->name_bytes;

  if (field->value_bytes == 0 || field->value_bytes > (size_t)(end - p))
    return NULL;
  value = p;
  p += field->value_bytes;

  if (name[field->name_bytes - 1] != '\0' ||
      value[field->value_bytes - 1] != '\0')
    return NULL;

  print_json_string(name);
  putchar(':');

  if (field->type == 0)
    print_json_string(value);
  else if (field->type == 1)
    fputs(value, stdout);
  else
    return NULL;

  return p;
}

static const char *show_ledger_field(const char *p, const char *end,
                                     const char *indent) {
  const LedgerFieldHeader *field;
  const char *name;
  const char *value;

  if ((size_t)(end - p) < sizeof *field)
    return NULL;

  field = (const LedgerFieldHeader *)p;
  p += sizeof *field;

  if (field->name_bytes == 0 || field->name_bytes > (size_t)(end - p))
    return NULL;
  name = p;
  p += field->name_bytes;

  if (field->value_bytes == 0 || field->value_bytes > (size_t)(end - p))
    return NULL;
  value = p;
  p += field->value_bytes;

  if (name[field->name_bytes - 1] != '\0' ||
      value[field->value_bytes - 1] != '\0')
    return NULL;

  printf("%s%s: %s\n", indent, name, value);
  return p;
}

static int emit_occurrence(const Ledger *ledger,
                           const OccurrenceRecord *occurrence) {
  const char *p = (const char *)(ledger->combine + 1);
  const char *end = (const char *)ledger->occurrence;
  uint32_t i;

  if (occurrence->combine >= ledger->combine->ncombines)
    return -1;

  for (i = 0; i < ledger->combine->ncombines; i++) {
    const CombineRecordHeader *combine;
    const char *fields;
    uint32_t j;

    if ((size_t)(end - p) < sizeof *combine)
      return -1;

    combine = (const CombineRecordHeader *)p;
    p += sizeof *combine;
    fields = p;

    for (j = 0; j < combine->nfields; j++) {
      p = skip_ledger_field(p, end);
      if (p == NULL)
        return -1;
    }

    if (i == occurrence->combine) {
      const char *provenancep = p;

      if (occurrence->provenance >= combine->nprovenance)
        return -1;

      for (j = 0; j < occurrence->provenance; j++) {
        const ProvenanceRecordHeader *provenance;
        uint32_t k;

        if ((size_t)(end - provenancep) < sizeof *provenance)
          return -1;

        provenance = (const ProvenanceRecordHeader *)provenancep;
        provenancep += sizeof *provenance;

        for (k = 0; k < provenance->nfields; k++) {
          provenancep = skip_ledger_field(provenancep, end);
          if (provenancep == NULL)
            return -1;
        }
      }

      putchar('{');

      {
        const char *q = fields;
        const ProvenanceRecordHeader *provenance;
        uint32_t k;

        for (j = 0; j < combine->nfields; j++) {
          if (j != 0)
            putchar(',');

          q = print_ledger_field_json(q, end);
          if (q == NULL)
            return -1;
        }

        if ((size_t)(end - provenancep) < sizeof *provenance)
          return -1;

        provenance = (const ProvenanceRecordHeader *)provenancep;
        provenancep += sizeof *provenance;

        fputs(",\"provenance\":{", stdout);

        for (k = 0; k < provenance->nfields; k++) {
          if (k != 0)
            putchar(',');

          provenancep = print_ledger_field_json(provenancep, end);
          if (provenancep == NULL)
            return -1;
        }

        fputs("}}", stdout);
      }

      return 0;
    }

    for (j = 0; j < combine->nprovenance; j++) {
      const ProvenanceRecordHeader *provenance;
      uint32_t k;

      if ((size_t)(end - p) < sizeof *provenance)
        return -1;

      provenance = (const ProvenanceRecordHeader *)p;
      p += sizeof *provenance;

      for (k = 0; k < provenance->nfields; k++) {
        p = skip_ledger_field(p, end);
        if (p == NULL)
          return -1;
      }
    }
  }

  return -1;
}

static int show_surface_combines(const Ledger *ledger, uint32_t surface) {
  const char *p = (const char *)(ledger->combine + 1);
  const char *end = (const char *)ledger->map + ledger->size;
  uint32_t matched = 0;
  uint32_t i;

  for (i = 0; i < ledger->combine->ncombines; i++) {
    const CombineRecordHeader *combine;
    const char *fields;
    const char *provenances;
    uint32_t j;

    if ((size_t)(end - p) < sizeof *combine)
      return -1;

    combine = (const CombineRecordHeader *)p;
    p += sizeof *combine;
    fields = p;

    for (j = 0; j < combine->nfields; j++) {
      p = skip_ledger_field(p, end);
      if (p == NULL)
        return -1;
    }

    provenances = p;

    for (j = 0; j < combine->nprovenance; j++) {
      const ProvenanceRecordHeader *provenance;
      uint32_t k;

      if ((size_t)(end - p) < sizeof *provenance)
        return -1;

      provenance = (const ProvenanceRecordHeader *)p;
      p += sizeof *provenance;

      for (k = 0; k < provenance->nfields; k++) {
        p = skip_ledger_field(p, end);
        if (p == NULL)
          return -1;
      }
    }

    if (combine->surface != surface)
      continue;

    matched++;
    printf("combine %u: provenance=%u\n", matched, combine->nprovenance);

    {
      const char *q = fields;

      for (j = 0; j < combine->nfields; j++) {
        q = show_ledger_field(q, end, "  ");
        if (q == NULL)
          return -1;
      }

      q = provenances;

      for (j = 0; j < combine->nprovenance; j++) {
        const ProvenanceRecordHeader *provenance;
        uint32_t k;

        if ((size_t)(end - q) < sizeof *provenance)
          return -1;

        provenance = (const ProvenanceRecordHeader *)q;
        q += sizeof *provenance;

        printf("  provenance[%u]\n", j);

        for (k = 0; k < provenance->nfields; k++) {
          q = show_ledger_field(q, end, "    ");
          if (q == NULL)
            return -1;
        }
      }
    }
  }

  printf("surface combines: %u\n", matched);
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

  if (find_combine_section(ledger) != 0) {
    fprintf(stderr, "invalid combine section\n");
    munmap(ledger->map, ledger->size);
    return -1;
  }

  if (find_occurrence_section(ledger) != 0) {
    fprintf(stderr, "invalid occurrence section\n");
    munmap(ledger->map, ledger->size);
    return -1;
  }

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
    best->surface = edge->surface;
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
    best.surface = lat->v[i].surface;

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

static void show_occurrence_adjacencies(const Ledger *ledger,
                                         const char *input,
                                         const Lattice *lat) {
  const LatticeEdge *left;
  const LatticeEdge *right;
  const OccurrenceRecord *occurrence;
  const OccurrenceRecord *end;

  end = ledger->occurrencev + ledger->occurrence->noccurrences;

  for (left = lat->v; left < lat->v + lat->n; left++) {
    for (right = lat->v; right < lat->v + lat->n; right++) {
      if (!lattice_edges_connect(input, left, right))
        continue;

      for (occurrence = ledger->occurrencev; occurrence + 1 < end;
           occurrence++) {
        const OccurrenceRecord *next = occurrence + 1;

        if (occurrence->sequence != next->sequence)
          continue;

        if (occurrence->surface != left->surface ||
            next->surface != right->surface)
          continue;

        printf("occurrence adjacency\tsequence=%u\tcombine=%u→%u\t%s→%s\n",
               occurrence->sequence, occurrence->combine, next->combine,
               ledger->surface_strings +
                   ledger->surface_offset[occurrence->surface],
               ledger->surface_strings + ledger->surface_offset[next->surface]);
      }
    }
  }
}

static void show_observed_path_from(const Ledger *ledger,
                                    const char *input, const Lattice *lat,
                                    const LatticeEdge *edge,
                                    const OccurrenceRecord *occurrence,
                                    size_t start, uint32_t *combinev,
                                    size_t depth) {
  const OccurrenceRecord *end;
  const OccurrenceRecord *next_occurrence;
  const LatticeEdge *next_edge;
  bool extended = false;
  size_t i;

  combinev[depth - 1] = occurrence->combine;
  end = ledger->occurrencev + ledger->occurrence->noccurrences;

  if (occurrence + 1 < end && depth < lat->n) {
    next_occurrence = occurrence + 1;

    if (next_occurrence->sequence == occurrence->sequence) {
      for (next_edge = lat->v; next_edge < lat->v + lat->n; next_edge++) {
        if (!lattice_edges_connect(input, edge, next_edge))
          continue;

        if (next_edge->surface != next_occurrence->surface)
          continue;

        show_observed_path_from(ledger, input, lat, next_edge, next_occurrence,
                                start, combinev, depth + 1);
        extended = true;
      }
    }
  }

  if (!extended && depth > 1) {
    printf("observed path\tsequence=%u\tdepth=%zu\tcombine=",
           occurrence->sequence, depth);

    for (i = 0; i < depth; i++) {
      if (i != 0)
        fputs("→", stdout);
      printf("%u", combinev[i]);
    }

    printf("\t%.*s\n", (int)(edge->end - start), input + start);
  }
}

static void show_observed_paths(const Ledger *ledger, const char *input,
                                const Lattice *lat) {
  const LatticeEdge *edge;
  const OccurrenceRecord *occurrence;
  const OccurrenceRecord *end;
  uint32_t *combinev;

  if (lat->n == 0)
    return;

  combinev = malloc(lat->n * sizeof *combinev);
  if (combinev == NULL)
    return;

  end = ledger->occurrencev + ledger->occurrence->noccurrences;

  for (edge = lat->v; edge < lat->v + lat->n; edge++) {
    for (occurrence = ledger->occurrencev; occurrence < end; occurrence++) {
      if (occurrence->surface != edge->surface)
        continue;

      show_observed_path_from(ledger, input, lat, edge, occurrence, edge->start,
                              combinev, 1);
    }
  }

  free(combinev);
}

static int emit_observed_path_from(
    const Ledger *ledger, const char *input, const Lattice *lat,
    const LatticeEdge *edge, const OccurrenceRecord *occurrence,
    const OccurrenceRecord **path, size_t depth, size_t target_end) {
  const OccurrenceRecord *end;
  const OccurrenceRecord *next_occurrence;
  const LatticeEdge *next_edge;
  bool extended = false;
  const OccurrenceRecord **p;

  path[depth - 1] = occurrence;
  end = ledger->occurrencev + ledger->occurrence->noccurrences;

  if (occurrence + 1 < end && depth < lat->n) {
    next_occurrence = occurrence + 1;

    if (next_occurrence->sequence == occurrence->sequence) {
      for (next_edge = lat->v; next_edge < lat->v + lat->n; next_edge++) {
        if (!lattice_edges_connect(input, edge, next_edge))
          continue;

        if (next_edge->surface != next_occurrence->surface)
          continue;

        if (emit_observed_path_from(ledger, input, lat, next_edge,
                                    next_occurrence, path, depth + 1,
                                    target_end) != 0)
          return -1;
        extended = true;
      }
    }
  }

  if (!extended && edge->end == target_end) {
    printf("{\"start\":0,\"end\":%zu,\"records\":[", target_end);

    for (p = path; p < path + depth; p++) {
      if (p != path)
        putchar(',');

      if (emit_occurrence(ledger, *p) != 0)
        return -1;
    }

    fputs("]}\n", stdout);
  }

  return 0;
}

static int emit_observed_paths(const Ledger *ledger, const char *input,
                               const Lattice *lat, size_t target_end) {
  const LatticeEdge *edge;
  const OccurrenceRecord *occurrence;
  const OccurrenceRecord *end;
  const OccurrenceRecord **path;
  if (lat->n == 0 || target_end == 0)
    return 0;

  path = malloc(lat->n * sizeof *path);
  if (path == NULL)
    return -1;

  end = ledger->occurrencev + ledger->occurrence->noccurrences;

  for (edge = lat->v; edge < lat->v + lat->n; edge++) {
    if (edge->start != 0)
      continue;

    for (occurrence = ledger->occurrencev; occurrence < end; occurrence++) {
      if (occurrence->surface != edge->surface)
        continue;

      if (emit_observed_path_from(ledger, input, lat, edge, occurrence, path,
                                  1, target_end) != 0) {
        free(path);
        return -1;
      }
    }
  }

  free(path);
  return 0;
}

static int process_input(const Ledger *ledger, int monitor) {
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

  if (!monitor) {
    LongestPathList paths;
    const LongestPath *path;
    size_t target_end = 0;

    longest_path_list_init(&paths);

    if (make_longest_paths(ledger, input, &lat, &paths) != 0) {
      longest_path_list_free(&paths);
      lattice_free(&lat);
      return -1;
    }

    for (path = paths.v; path < paths.v + paths.n; path++)
      if (path->start == 0 && path->end > target_end)
        target_end = path->end;

    if (emit_observed_paths(ledger, input, &lat, target_end) != 0) {
      longest_path_list_free(&paths);
      lattice_free(&lat);
      return -1;
    }

    longest_path_list_free(&paths);
    lattice_free(&lat);
    return 0;
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

    if (show_surface_combines(ledger, e->surface) != 0) {
      lattice_free(&lat);
      return -1;
    }
  }

  show_paths(ledger, input, &lat);
  show_occurrence_adjacencies(ledger, input, &lat);
  show_observed_paths(ledger, input, &lat);

  LongestPathList paths;

  longest_path_list_init(&paths);

  if (make_longest_paths(ledger, input, &lat, &paths) != 0) {
    longest_path_list_free(&paths);
    lattice_free(&lat);
    return -1;
  }

  show_longest_paths(input, &paths);
  show_reach(&paths);
  show_uncovered_adjacencies(ledger, input, &lat, &paths);

  longest_path_list_free(&paths);

  lattice_free(&lat);
  return 0;
}

static int monitor_ledger(const Ledger *ledger) {
  uint32_t nprovenance;

  printf("version:         %u\n", ledger->header->version);
  printf("trie nodes:      %u\n", ledger->header->trie_nodes);
  printf("unique surfaces: %u\n", ledger->header->unique_surfaces);
  printf("combines:        %u\n", ledger->combine->ncombines);

  if (count_ledger_provenance(ledger, &nprovenance) != 0) {
    fprintf(stderr, "invalid combine records\n");
    return -1;
  }

  printf("provenance:      %u\n", nprovenance);
  printf("occurrences:     %u\n", ledger->occurrence->noccurrences);
  printf("root token:      %u\n", ledger->trie[0].token);
  printf("root child:      %u\n", ledger->trie[0].child);
  printf("root sibling:    %u\n", ledger->trie[0].sibling);
  printf("root freq:       %u\n", ledger->trie[0].freq);

  return 0;
}

int main(int argc, char *argv[]) {
  Ledger ledger;
  const char *path;
  int monitor = 0;
  int opt;

  while ((opt = getopt(argc, argv, "mh")) != -1) {
    switch (opt) {
    case 'm':
      monitor = 1;
      break;
    case 'h':
      printf("usage: %s [-m] ledger.dat\n", argv[0]);
      return EXIT_SUCCESS;
    default:
      fprintf(stderr, "usage: %s [-m] ledger.dat\n", argv[0]);
      return EXIT_FAILURE;
    }
  }

  if (optind + 1 != argc) {
    fprintf(stderr, "usage: %s [-m] ledger.dat\n", argv[0]);
    return EXIT_FAILURE;
  }

  path = argv[optind];

  if (load_ledger(path, &ledger) != 0)
    return EXIT_FAILURE;

  if (monitor && monitor_ledger(&ledger) != 0) {
    unload_ledger(&ledger);
    return EXIT_FAILURE;
  }

  if (process_input(&ledger, monitor) != 0) {
    unload_ledger(&ledger);
    return EXIT_FAILURE;
  }

  unload_ledger(&ledger);
  return EXIT_SUCCESS;
}
