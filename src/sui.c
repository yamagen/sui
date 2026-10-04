#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
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
  uint32_t null_surface;
  uint32_t current_max_surface;
  char **runtime_surface_strings;
  size_t runtime_surface_count;
  size_t runtime_surface_cap;
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

typedef enum {
  CONTEXT_STRING,
  CONTEXT_INTEGER
} ContextType;

typedef struct {
  char *name;
  ContextType type;
  char *value;
} ContextField;

typedef struct {
  char *text;
  ContextField *provenance;
  size_t nprovenance;
} SuiInput;

static bool reach_covers_adjacency(const LongestPathList *list,
                                   const LatticeEdge *a, const LatticeEdge *b);
static int emit_adjacency_work_row(FILE *fp, const MkledgerConfig *config,
                                   const SuiInput *input,
                                   const LatticeEdge *edge,
                                   const char *word) {
  const SchemaField *field;

  fprintf(fp, "!{\"start\":%zu,\"end\":%zu,\"text\":", edge->start,
          edge->end);
  fprintf(fp, "\"%.*s\"", (int)(edge->end - edge->start),
          input->text + edge->start);

  if (input->nprovenance != 0)
    print_input_provenance(fp, input);

  if (config != NULL) {
    for (field = config->schema.v; field < config->schema.v + config->schema.n;
         field++) {
      if (is_provenance_name(config, field->field))
        continue;
      if (!schema_allows_type(field, "string"))
        continue;

      fputc(',', fp);
      print_json_string_to(fp, field->field);
      fputc(':', fp);
      if (strcmp(field->field, "word") == 0)
        print_json_string_to(fp, word);
      else
        fputs("\"\"", fp);
    }
  }

  fputs("}\n", fp);
  return ferror(fp) ? -1 : 0;
}

static int append_uncovered_adjacencies(const Ledger *ledger,
                                        const MkledgerConfig *config,
                                        const SuiInput *input,
                                        const Lattice *lat,
                                        const LongestPathList *list) {
  const LatticeEdge *a;
  const LatticeEdge *b;
  FILE *fp;

  if (config == NULL || config->candy.filename == NULL)
    return 0;

  fp = fopen(config->candy.filename, "a");
  if (fp == NULL)
    return -1;

  for (a = lat->v; a < lat->v + lat->n; a++) {
    for (b = lat->v; b < lat->v + lat->n; b++) {
      const char *as;
      const char *bs;

      if (!lattice_edges_connect(input->text, a, b))
        continue;
      if (reach_covers_adjacency(list, a, b))
        continue;

      as = a->surface > ledger->null_surface
               ? runtime_surface_string(ledger, a->surface)
               : surface_string(ledger, a->surface);
      bs = b->surface > ledger->null_surface
               ? runtime_surface_string(ledger, b->surface)
               : surface_string(ledger, b->surface);

      if (as == NULL || bs == NULL)
        continue;

      if (emit_adjacency_work_row(fp, config, input, a, as) != 0 ||
          emit_adjacency_work_row(fp, config, input, b, bs) != 0) {
        fclose(fp);
        return -1;
      }
    }
  }

  return fclose(fp) == 0 ? 0 : -1;
}

static void show_uncovered_adjacencies(const Ledger *ledger,
                                       const MkledgerConfig *config,
                                       const char *input, const Lattice *lat,
                                       const LongestPathList *list);
static int append_uncovered_adjacencies(const Ledger *ledger,
                                        const MkledgerConfig *config,
                                        const SuiInput *input,
                                        const Lattice *lat,
                                        const LongestPathList *list);

static int schema_allows_type(const SchemaField *field, const char *type);
static int is_provenance_name(const MkledgerConfig *config, const char *name);
static void print_json_string_to(FILE *fp, const char *s);

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
static const char *surface_string(const Ledger *ledger, uint32_t surface);
static const char *runtime_surface_string(const Ledger *ledger,
                                          uint32_t surface);
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
static void free_sui_input(SuiInput *input);
static int parse_sui_input(const char *line, const MkledgerConfig *config,
                           SuiInput *input);
static void print_input_provenance(FILE *fp, const SuiInput *input);
static int emit_unresolved(FILE *fp, const MkledgerConfig *config,
                           const SuiInput *input, size_t start, size_t end);
static char *parse_candy_text(const char *line);
static int index_candy_surfaces(Ledger *ledger, const MkledgerConfig *config);
static int scan_candy_surface(const Ledger *ledger,
                              const MkledgerConfig *config,
                              const char *input, size_t start,
                              size_t *matched_end, uint32_t *matched_surface);
static int process_input(const Ledger *ledger, const MkledgerConfig *config,
                         int monitor, int unresolved, int append_candy);

static bool reach_covers_adjacency(const LongestPathList *list,
                                   const LatticeEdge *a, const LatticeEdge *b) {
  const LongestPath *path;

  for (path = list->v; path < list->v + list->n; path++) {
    if (path->start <= a->start && path->end >= b->end)
      return true;
  }

  return false;
}

static void print_adjacency_work_row(const MkledgerConfig *config,
                                     const char *word) {
  const SchemaField *field;
  int wrote_field = 0;
  int wrote_word = 0;

  fputs("!{", stdout);

  if (config != NULL) {
    for (field = config->schema.v; field < config->schema.v + config->schema.n;
         field++) {
      if (is_provenance_name(config, field->field))
        continue;
      if (!schema_allows_type(field, "string"))
        continue;

      if (wrote_field)
        putchar(',');

      print_json_string_to(stdout, field->field);
      putchar(':');

      if (strcmp(field->field, "word") == 0) {
        print_json_string_to(stdout, word);
        wrote_word = 1;
      } else {
        fputs("\"\"", stdout);
      }
      wrote_field = 1;
    }
  }

  if (!wrote_word) {
    if (wrote_field)
      putchar(',');
    fputs("\"word\":", stdout);
    print_json_string_to(stdout, word);
  }

  fputs("}\n", stdout);
}

static void show_uncovered_adjacencies(const Ledger *ledger,
                                       const MkledgerConfig *config,
                                       const char *input, const Lattice *lat,
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

      if (a->surface > ledger->null_surface)
        as = runtime_surface_string(ledger, a->surface);
      else
        as = surface_string(ledger, a->surface);

      if (b->surface > ledger->null_surface)
        bs = runtime_surface_string(ledger, b->surface);
      else
        bs = surface_string(ledger, b->surface);

      if (as == NULL || bs == NULL)
        continue;

      printf("add an adjacency of A with B: %.*s[%.*s]%s\n", (int)a->start,
             input, (int)(b->end - a->start), input + a->start, input + b->end);

      print_adjacency_work_row(config, as);
      print_adjacency_work_row(config, bs);
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

static const char *surface_string(const Ledger *ledger, uint32_t surface) {
  if (surface >= ledger->header->unique_surfaces)
    return NULL;

  return ledger->surface_strings + ledger->surface_offset[surface];
}

static const char *runtime_surface_string(const Ledger *ledger,
                                          uint32_t surface) {
  size_t index;

  if (surface <= ledger->null_surface ||
      surface > ledger->current_max_surface)
    return NULL;

  index = (size_t)(surface - ledger->null_surface - 1);
  if (index >= ledger->runtime_surface_count)
    return NULL;

  return ledger->runtime_surface_strings[index];
}

static void unload_ledger(Ledger *ledger) {
  size_t i;

  for (i = 0; i < ledger->runtime_surface_count; i++)
    free(ledger->runtime_surface_strings[i]);
  free(ledger->runtime_surface_strings);

  munmap(ledger->map, ledger->size);
}

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

  if (ledger->header->unique_surfaces > UINT32_MAX - 2) {
    fprintf(stderr, "surface id space exhausted\n");
    munmap(ledger->map, ledger->size);
    return -1;
  }

  ledger->null_surface = ledger->header->unique_surfaces + 2;
  ledger->current_max_surface = ledger->null_surface;
  ledger->runtime_surface_strings = NULL;
  ledger->runtime_surface_count = 0;
  ledger->runtime_surface_cap = 0;

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
      const char *surface = surface_string(ledger, id);
      size_t len = strlen(surface);

      if (start + len <= input_len &&
          memcmp(input + start, surface, len) == 0) {
        if (lattice_add(lat, start, start + len, id) != 0)
          return -1;
      }
    }

    for (id = ledger->null_surface + 1;
         id <= ledger->current_max_surface; id++) {
      const char *surface = runtime_surface_string(ledger, id);
      size_t len;

      if (surface == NULL)
        continue;

      len = strlen(surface);
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
  uint32_t p;

  if (surface >= ledger->header->unique_surfaces)
    return TRIE_NONE;

  p = ledger->trie[node].child;

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
  const char *surface = surface_string(ledger, edge->surface);
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


static void free_sui_input(SuiInput *input) {
  ContextField *field;

  free(input->text);

  for (field = input->provenance;
       field < input->provenance + input->nprovenance; field++) {
    free(field->name);
    free(field->value);
  }

  free(input->provenance);
  memset(input, 0, sizeof(*input));
}

static const SchemaField *find_schema_field(const MkledgerConfig *config,
                                            const char *name) {
  const SchemaField *field;

  for (field = config->schema.v;
       field < config->schema.v + config->schema.n; field++)
    if (strcmp(field->field, name) == 0)
      return field;

  return NULL;
}

static int schema_allows_type(const SchemaField *field, const char *type) {
  char **p;

  if (field == NULL)
    return 0;

  for (p = field->types; p < field->types + field->ntypes; p++)
    if (strcmp(*p, type) == 0)
      return 1;

  return 0;
}

static char *parse_context_integer(tjson_t *json) {
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

static int add_context_field(SuiInput *input, const char *name,
                             ContextType type, char *value) {
  ContextField *tmp;
  ContextField *field;

  tmp = realloc(input->provenance,
                (input->nprovenance + 1) * sizeof(*input->provenance));
  if (tmp == NULL) {
    free(value);
    return -1;
  }

  input->provenance = tmp;
  field = input->provenance + input->nprovenance;
  field->name = strdup(name);
  if (field->name == NULL) {
    free(value);
    return -1;
  }

  field->type = type;
  field->value = value;
  input->nprovenance++;
  return 0;
}

static int is_provenance_name(const MkledgerConfig *config, const char *name) {
  char **p;

  for (p = config->provenance;
       p < config->provenance + config->nprovenance; p++)
    if (strcmp(*p, name) == 0)
      return 1;

  return 0;
}

static int parse_input_provenance(tjson_t *json, const MkledgerConfig *config,
                                  SuiInput *input) {
  tjson_expect(json, '{');

  for (;;) {
    char *name;
    const SchemaField *schema;
    char *value = NULL;
    ContextType type;

    tjson_skip_ws(json);
    if (tjson_peek(json) == '}') {
      tjson_expect(json, '}');
      return 0;
    }

    name = tjson_parse_string(json);
    tjson_skip_ws(json);
    tjson_expect(json, ':');
    tjson_skip_ws(json);

    if (!is_provenance_name(config, name)) {
      tjson_skip_value(json);
      free(name);
    } else {
      schema = find_schema_field(config, name);

      if (tjson_peek(json) == '"') {
        if (!schema_allows_type(schema, "string")) {
          free(name);
          return -1;
        }
        type = CONTEXT_STRING;
        value = tjson_parse_string(json);
      } else {
        if (!schema_allows_type(schema, "integer")) {
          free(name);
          return -1;
        }
        type = CONTEXT_INTEGER;
        value = parse_context_integer(json);
        if (value == NULL) {
          free(name);
          return -1;
        }
      }

      if (add_context_field(input, name, type, value) != 0) {
        free(name);
        return -1;
      }
      free(name);
    }

    tjson_skip_ws(json);
    if (tjson_peek(json) == ',') {
      tjson_expect(json, ',');
      continue;
    }
    if (tjson_peek(json) == '}') {
      tjson_expect(json, '}');
      return 0;
    }
    return -1;
  }
}

static int has_input_provenance(const SuiInput *input, const char *name) {
  const ContextField *field;

  for (field = input->provenance;
       field < input->provenance + input->nprovenance; field++)
    if (strcmp(field->name, name) == 0)
      return 1;

  return 0;
}

static int parse_sui_input(const char *line, const MkledgerConfig *config,
                           SuiInput *input) {
  tjson_t json;
  char **required;

  memset(input, 0, sizeof(*input));

  if (line[0] != '{') {
    input->text = strdup(line);
    return input->text == NULL ? -1 : 0;
  }

  if (config == NULL || config->nprovenance == 0)
    return -1;

  tjson_init(&json, "stdin", line);
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
    tjson_skip_ws(&json);

    if (strcmp(key, "text") == 0)
      input->text = tjson_parse_string(&json);
    else if (strcmp(key, "provenance") == 0) {
      if (parse_input_provenance(&json, config, input) != 0) {
        free(key);
        free_sui_input(input);
        return -1;
      }
    } else
      tjson_skip_value(&json);

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

    free_sui_input(input);
    return -1;
  }

  tjson_skip_ws(&json);
  if (json.pos != json.length || input->text == NULL) {
    free_sui_input(input);
    return -1;
  }

  for (required = config->provenance;
       required < config->provenance + config->nprovenance; required++)
    if (!has_input_provenance(input, *required)) {
      free_sui_input(input);
      return -1;
    }

  return 0;
}

static void print_json_string_to(FILE *fp, const char *s) {
  const unsigned char *p;

  fputc('"', fp);

  for (p = (const unsigned char *)s; *p != '\0'; p++) {
    switch (*p) {
    case '"':
      fputs("\\\"", fp);
      break;
    case '\\':
      fputs("\\\\", fp);
      break;
    case '\b':
      fputs("\\b", fp);
      break;
    case '\f':
      fputs("\\f", fp);
      break;
    case '\n':
      fputs("\\n", fp);
      break;
    case '\r':
      fputs("\\r", fp);
      break;
    case '\t':
      fputs("\\t", fp);
      break;
    default:
      if (*p < 0x20)
        fprintf(fp, "\\u%04x", *p);
      else
        fputc(*p, fp);
    }
  }

  fputc('"', fp);
}

static void print_input_provenance(FILE *fp, const SuiInput *input) {
  const ContextField *field;

  fputs(",\"provenance\":{", fp);

  for (field = input->provenance;
       field < input->provenance + input->nprovenance; field++) {
    if (field != input->provenance)
      fputc(',', fp);

    print_json_string_to(fp, field->name);
    fputc(':', fp);

    if (field->type == CONTEXT_STRING)
      print_json_string_to(fp, field->value);
    else
      fputs(field->value, fp);
  }

  fputc('}', fp);
}

static int emit_unresolved(FILE *fp, const MkledgerConfig *config,
                           const SuiInput *input, size_t start, size_t end) {
  const SchemaField *field;

  fprintf(fp, "!{\"start\":%zu,\"end\":%zu,\"text\":", start, end);
  print_json_string_to(fp, input->text + start);

  if (input->nprovenance != 0)
    print_input_provenance(fp, input);

  if (config != NULL) {
    for (field = config->schema.v; field < config->schema.v + config->schema.n;
         field++) {
      if (is_provenance_name(config, field->field))
        continue;

      if (!schema_allows_type(field, "string"))
        continue;

      fputc(',', fp);
      print_json_string_to(fp, field->field);
      fputs(":\"\"", fp);
    }
  }

  fputs("}\n", fp);
  return ferror(fp) ? -1 : 0;
}

static char *parse_candy_text(const char *line) {
  tjson_t json;
  char *text = NULL;

  if (line[0] != '!')
    return NULL;

  tjson_init(&json, "candy", line + 1);
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
    tjson_skip_ws(&json);

    if (strcmp(key, "text") == 0) {
      free(text);
      text = tjson_parse_string(&json);
    } else
      tjson_skip_value(&json);

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

    free(text);
    return NULL;
  }

  tjson_skip_ws(&json);
  if (json.pos != json.length) {
    free(text);
    return NULL;
  }

  return text;
}

static int index_candy_surfaces(Ledger *ledger,
                                const MkledgerConfig *config) {
  FILE *fp;
  char line[4096];
  uint32_t surface = ledger->null_surface;

  if (config == NULL || config->candy.filename == NULL)
    return 0;

  fp = fopen(config->candy.filename, "r");
  if (fp == NULL)
    return 0;

  while (fgets(line, sizeof line, fp) != NULL) {
    char *text;

    line[strcspn(line, "\r\n")] = '\0';
    text = parse_candy_text(line);
    if (text == NULL)
      continue;

    if (surface == UINT32_MAX) {
      free(text);
      fclose(fp);
      return -1;
    }

    if (ledger->runtime_surface_count == ledger->runtime_surface_cap) {
      size_t new_cap =
          ledger->runtime_surface_cap == 0 ? 16 : ledger->runtime_surface_cap * 2;
      char **new_strings =
          realloc(ledger->runtime_surface_strings, new_cap * sizeof(*new_strings));

      if (new_strings == NULL) {
        free(text);
        fclose(fp);
        return -1;
      }

      ledger->runtime_surface_strings = new_strings;
      ledger->runtime_surface_cap = new_cap;
    }

    ledger->runtime_surface_strings[ledger->runtime_surface_count++] = text;
    surface++;
  }

  if (fclose(fp) != 0)
    return -1;

  ledger->current_max_surface = surface;

  if (ledger->runtime_surface_count != 0 &&
      runtime_surface_string(ledger, ledger->current_max_surface) == NULL)
    return -1;

  return 0;
}

static int scan_candy_surface(const Ledger *ledger,
                              const MkledgerConfig *config,
                              const char *input, size_t start,
                              size_t *matched_end, uint32_t *matched_surface) {
  FILE *fp;
  char line[4096];
  size_t input_end = strlen(input);
  uint32_t surface = ledger->null_surface;
  int found = 0;

  *matched_end = start;
  *matched_surface = ledger->null_surface;

  if (config == NULL || config->candy.filename == NULL)
    return 0;

  fp = fopen(config->candy.filename, "r");
  if (fp == NULL)
    return 0;

  while (fgets(line, sizeof line, fp) != NULL) {
    char *text;
    size_t len;

    line[strcspn(line, "\r\n")] = '\0';
    text = parse_candy_text(line);
    if (text == NULL)
      continue;

    surface++;
    len = strlen(text);

    if (len != 0 && start + len <= input_end &&
        memcmp(input + start, text, len) == 0 &&
        start + len > *matched_end) {
      *matched_end = start + len;
      *matched_surface = surface;
      found = 1;
    }

    free(text);
  }

  if (fclose(fp) != 0)
    return -1;

  return found;
}

static int process_input(const Ledger *ledger, const MkledgerConfig *config,
                         int monitor, int unresolved, int append_candy) {
  char line[4096];
  SuiInput parsed;
  const char *input;
  Lattice lat;

  if (fgets(line, sizeof line, stdin) == NULL)
    return -1;

  line[strcspn(line, "\r\n")] = '\0';

  if (parse_sui_input(line, config, &parsed) != 0)
    return -1;

  input = parsed.text;
  lattice_init(&lat);

  if (make_lattice(ledger, input, &lat) != 0) {
    free_sui_input(&parsed);
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
      free_sui_input(&parsed);
      return -1;
    }

    for (path = paths.v; path < paths.v + paths.n; path++)
      if (path->start == 0 && path->end > target_end)
        target_end = path->end;

    if (config != NULL && config->candy.filename != NULL) {
      size_t input_end = strlen(input);

      while (target_end < input_end) {
        size_t candy_end;
        size_t known_end;
        uint32_t candy_surface;
        int candy_status = scan_candy_surface(
            ledger, config, input, target_end, &candy_end, &candy_surface);

        if (candy_status < 0) {
          longest_path_list_free(&paths);
          lattice_free(&lat);
          free_sui_input(&parsed);
          return -1;
        }

        if (candy_status == 0)
          break;

        (void)candy_surface;
        target_end = candy_end;
        known_end = target_end;

        for (path = paths.v; path < paths.v + paths.n; path++)
          if (path->start == target_end && path->end > known_end)
            known_end = path->end;

        target_end = known_end;
      }
    }

    if (append_candy &&
        append_uncovered_adjacencies(ledger, config, &parsed, &lat, &paths) !=
            0) {
      longest_path_list_free(&paths);
      lattice_free(&lat);
      free_sui_input(&parsed);
      return -1;
    }

    if (unresolved) {
      size_t input_end = strlen(input);

      if (target_end < input_end) {
        if (emit_unresolved(stdout, config, &parsed, target_end, input_end) != 0) {
          longest_path_list_free(&paths);
          lattice_free(&lat);
          free_sui_input(&parsed);
          return -1;
        }

        if (append_candy) {
          FILE *fp = fopen(config->candy.filename, "a");
          int status;

          if (fp == NULL) {
            longest_path_list_free(&paths);
            lattice_free(&lat);
            free_sui_input(&parsed);
            return -1;
          }

          status = emit_unresolved(fp, config, &parsed, target_end, input_end);
          if (fclose(fp) != 0)
            status = -1;

          if (status != 0) {
            longest_path_list_free(&paths);
            lattice_free(&lat);
            free_sui_input(&parsed);
            return -1;
          }
        }
      }
    } else if (emit_observed_paths(ledger, input, &lat, target_end) != 0) {
      longest_path_list_free(&paths);
      lattice_free(&lat);
      free_sui_input(&parsed);
      return -1;
    }

    longest_path_list_free(&paths);
    lattice_free(&lat);
    free_sui_input(&parsed);
    return 0;
  }

  if (show_lattice_gaps(input, &lat) != 0) {
    lattice_free(&lat);
    free_sui_input(&parsed);
    return -1;
  }

  printf("lattice edges: %zu\n", lat.n);

  for (size_t i = 0; i < lat.n; i++) {
    const LatticeEdge *e = &lat.v[i];
    const char *surface;

    if (e->surface > ledger->null_surface)
      surface = runtime_surface_string(ledger, e->surface);
    else
      surface = surface_string(ledger, e->surface);

    if (surface == NULL)
      continue;

    printf("%zu\t%zu\t%u\t%s\n", e->start, e->end, e->surface, surface);

    if (e->surface < ledger->header->unique_surfaces &&
        show_surface_combines(ledger, e->surface) != 0) {
      lattice_free(&lat);
      free_sui_input(&parsed);
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
    free_sui_input(&parsed);
    return -1;
  }

  show_longest_paths(input, &paths);
  show_reach(&paths);
  show_uncovered_adjacencies(ledger, config, input, &lat, &paths);

  longest_path_list_free(&paths);

  lattice_free(&lat);
  free_sui_input(&parsed);
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
  MkledgerConfig config;
  const char *config_path = NULL;
  const char *path;
  int monitor = 0;
  int unresolved = 0;
  int append_candy = 0;
  int opt;

  memset(&config, 0, sizeof(config));

  while ((opt = getopt(argc, argv, "ac:muh")) != -1) {
    switch (opt) {
    case 'a':
      append_candy = 1;
      unresolved = 1;
      break;
    case 'c':
      config_path = optarg;
      break;
    case 'm':
      monitor = 1;
      break;
    case 'u':
      unresolved = 1;
      break;
    case 'h':
      printf("usage: %s [-c config] [-m | -u | -a] ledger.dat\n", argv[0]);
      return EXIT_SUCCESS;
    default:
      fprintf(stderr, "usage: %s [-c config] [-m | -u | -a] ledger.dat\n", argv[0]);
      return EXIT_FAILURE;
    }
  }

  if (monitor && unresolved) {
    fprintf(stderr, "usage: %s [-c config] [-m | -u | -a] ledger.dat\n", argv[0]);
    return EXIT_FAILURE;
  }

  if (optind + 1 != argc) {
    fprintf(stderr, "usage: %s [-c config] [-m | -u | -a] ledger.dat\n", argv[0]);
    return EXIT_FAILURE;
  }

  path = argv[optind];

  if (append_candy && config_path == NULL) {
    fprintf(stderr, "-a requires -c config\n");
    return EXIT_FAILURE;
  }

  if (config_path != NULL && load_mkledger_config(config_path, &config) != 0)
    return EXIT_FAILURE;

  if (append_candy && config.candy.filename == NULL) {
    fprintf(stderr, "-a requires candy.filename in config\n");
    free_mkledger_config(&config);
    return EXIT_FAILURE;
  }

  if (load_ledger(path, &ledger) != 0) {
    free_mkledger_config(&config);
    return EXIT_FAILURE;
  }

  if (index_candy_surfaces(&ledger,
                           config_path == NULL ? NULL : &config) != 0) {
    fprintf(stderr, "cannot index candy surfaces\n");
    unload_ledger(&ledger);
    free_mkledger_config(&config);
    return EXIT_FAILURE;
  }

  if (monitor && monitor_ledger(&ledger) != 0) {
    unload_ledger(&ledger);
    free_mkledger_config(&config);
    return EXIT_FAILURE;
  }

  if (process_input(&ledger, config_path == NULL ? NULL : &config, monitor,
                    unresolved, append_candy) != 0) {
    unload_ledger(&ledger);
    free_mkledger_config(&config);
    return EXIT_FAILURE;
  }

  unload_ledger(&ledger);
  free_mkledger_config(&config);
  return EXIT_SUCCESS;
}
