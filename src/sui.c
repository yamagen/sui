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
  uint32_t surface;
} LatticeEdge;

typedef struct {
  size_t start;
  size_t end;
  size_t depth;
  uint32_t freq;
  uint32_t surface;
  LatticeEdge *route;
  size_t route_n;
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
  size_t *runtime_surface_offsets;
  size_t runtime_surface_count;
  size_t runtime_surface_cap;
  void *candy_map;
  size_t candy_size;
} Ledger;

typedef struct {
  LatticeEdge *v;
  size_t n;
  size_t cap;
} Lattice;

/*
 * The selected route uses the same surface namespace and span representation
 * as the lattice. The lattice holds all candidates; this list will hold only
 * the route actually accepted by SUI.
 */
typedef struct {
  LatticeEdge *v;
  size_t n;
  size_t cap;
} SelectedRoute;

typedef struct {
  size_t first;
  size_t count;
} RouteRun;

typedef struct {
  RouteRun *v;
  size_t n;
  size_t cap;
} RouteRunList;

typedef struct {
  size_t transfers;
  size_t longest_surface;
} RouteEvaluation;

typedef struct {
  const OccurrenceRecord **v;
  size_t n;
  size_t cap;
} OccurrenceMatchList;


typedef struct {
  OccurrenceMatchList *v;
  size_t n;
} RouteOccurrenceMatches;

typedef struct {
  size_t end;
  char *text;
  uint32_t surface;
} CandyCandidate;

typedef struct {
  CandyCandidate *v;
  size_t n;
  size_t cap;
} CandyCandidateList;

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

typedef struct {
  const char *name;
  const char *value;
  uint32_t type;
} LedgerFieldView;

typedef struct {
  const char **v;
  size_t n;
  size_t cap;
} FieldValueList;

typedef struct {
  const char *name;
  uint32_t type;
  FieldValueList values;
} AggregatedField;

typedef struct {
  AggregatedField *v;
  size_t n;
} AggregatedRecord;

typedef struct {
  const CombineRecordHeader *combine;
  const ProvenanceRecordHeader *provenance;
} ProvenanceView;

typedef struct {
  ProvenanceView *v;
  size_t n;
  size_t cap;
} ProvenanceViewList;


static bool reach_covers_adjacency(const LongestPathList *list,
                                   const LatticeEdge *a, const LatticeEdge *b);
static void show_uncovered_adjacencies(const Ledger *ledger,
                                       const MkledgerConfig *config,
                                       const char *input, const Lattice *lat,
                                       const LongestPathList *list);
static int emit_adjacency_work_row(FILE *fp, const MkledgerConfig *config,
                                   const SuiInput *input,
                                   const LatticeEdge *edge,
                                   const char *word);

static int candy_has_work_pair(const Ledger *ledger,
                               const MkledgerConfig *config,
                               const SuiInput *input,
                               const LatticeEdge *a, const char *as,
                               const LatticeEdge *b, const char *bs) {
  char *pair = NULL;
  size_t pair_len = 0;
  FILE *fp;
  const char *base;
  size_t offset = 0;

  if (ledger->candy_map == NULL || ledger->candy_size == 0)
    return 0;

  fp = open_memstream(&pair, &pair_len);
  if (fp == NULL)
    return -1;

  if (emit_adjacency_work_row(fp, config, input, a, as) != 0 ||
      emit_adjacency_work_row(fp, config, input, b, bs) != 0 ||
      fclose(fp) != 0) {
    free(pair);
    return -1;
  }

  base = (const char *)ledger->candy_map;

  while (offset + pair_len <= ledger->candy_size) {
    if ((offset == 0 || base[offset - 1] == '\n') &&
        memcmp(base + offset, pair, pair_len) == 0) {
      free(pair);
      return 1;
    }

    const char *nl = memchr(base + offset, '\n', ledger->candy_size - offset);
    if (nl == NULL)
      break;
    offset = (size_t)(nl - base) + 1;
  }

  free(pair);
  return 0;
}

static int append_uncovered_adjacencies(const Ledger *ledger,
                                        const MkledgerConfig *config,
                                        const SuiInput *input,
                                        const Lattice *lat,
                                        const LongestPathList *list,
                                        size_t resolved_end);

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

static void selected_route_init(SelectedRoute *route);
static void selected_route_free(SelectedRoute *route);
static int selected_route_add(SelectedRoute *route, size_t start, size_t end,
                              uint32_t surface);
static void route_run_list_init(RouteRunList *runs);
static void route_run_list_free(RouteRunList *runs);
static int split_selected_route(const Ledger *ledger,
                                const SelectedRoute *route,
                                RouteRunList *runs);
static size_t route_transfer_count(const RouteRunList *runs);
static void occurrence_match_list_init(OccurrenceMatchList *list);
static void occurrence_match_list_free(OccurrenceMatchList *list);
static int occurrence_match_list_add(OccurrenceMatchList *list,
                                     const OccurrenceRecord *occurrence);
static int collect_ledger_run_matches(const Ledger *ledger,
                                      const SelectedRoute *route,
                                      const RouteRun *run,
                                      OccurrenceMatchList *matches);
static void route_occurrence_matches_free(RouteOccurrenceMatches *matches);
static int collect_route_occurrence_matches(const Ledger *ledger,
                                            const SelectedRoute *route,
                                            const RouteRunList *runs,
                                            RouteOccurrenceMatches *matches);
static int collect_route_position_occurrences(
    const Ledger *ledger, const SelectedRoute *route,
    const RouteRunList *runs, const RouteOccurrenceMatches *matches,
    size_t route_index, OccurrenceMatchList *occurrences);
static int evaluate_selected_route(const Ledger *ledger,
                                   const SelectedRoute *route,
                                   RouteEvaluation *evaluation);
static int route_evaluation_compare(const RouteEvaluation *a,
                                    const RouteEvaluation *b);
static int evaluate_route_continuation(
    const Ledger *ledger, const SelectedRoute *accepted,
    const LatticeEdge *route, size_t route_n, RouteEvaluation *evaluation);
static int evaluate_route_continuation(
    const Ledger *ledger, const SelectedRoute *accepted,
    const LatticeEdge *route, size_t route_n, RouteEvaluation *evaluation) {
  SelectedRoute combined;
  size_t i;

  selected_route_init(&combined);

  if (accepted != NULL)
    for (i = 0; i < accepted->n; i++)
      if (selected_route_add(&combined, accepted->v[i].start,
                             accepted->v[i].end,
                             accepted->v[i].surface) != 0) {
        selected_route_free(&combined);
        return -1;
      }

  for (i = 1; i < route_n; i++)
    if (selected_route_add(&combined, route[i].start, route[i].end,
                           route[i].surface) != 0) {
      selected_route_free(&combined);
      return -1;
    }

  if (evaluate_selected_route(ledger, &combined, evaluation) != 0) {
    selected_route_free(&combined);
    return -1;
  }

  selected_route_free(&combined);
  return 0;
}

static size_t longest_path_candidate_count(const LongestPathList *paths,
                                           size_t start, size_t end);
static size_t longest_path_candidate_count(const LongestPathList *paths,
                                           size_t start, size_t end) {
  size_t count = 0;
  const LongestPath *path;

  for (path = paths->v; path < paths->v + paths->n; path++)
    if (path->start == start && path->end == end)
      count++;

  return count;
}

static int ledger_run_matches(const Ledger *ledger,
                              const SelectedRoute *route,
                              const RouteRun *run,
                              const OccurrenceRecord *first);
static size_t runtime_surface_offset(const Ledger *ledger, uint32_t surface);
static int selected_route_has_runtime(const Ledger *ledger,
                                      const SelectedRoute *route);
static int emit_mixed_route(const Ledger *ledger,
                            const SelectedRoute *route, size_t target_end,
                            int best_mode);
static int emit_occurrence(const Ledger *ledger,
                           const OccurrenceRecord *occurrence);
static const char *print_ledger_field_json(const char *p,
                                           const char *end);
static int occurrence_combine_fields(
    const Ledger *ledger, const OccurrenceRecord *occurrence,
    const CombineRecordHeader **combine_out, const char **fields_out);
static const char *ledger_field_view(const char *p, const char *end,
                                     LedgerFieldView *view);
static void field_value_list_init(FieldValueList *list);
static void field_value_list_free(FieldValueList *list);
static int field_value_list_add_unique(FieldValueList *list,
                                       const char *value);
static int collect_occurrence_field_values(
    const Ledger *ledger, const OccurrenceMatchList *occurrences,
    const char *field_name, uint32_t field_type, FieldValueList *values);
static void aggregated_record_free(AggregatedRecord *record);
static int aggregate_occurrence_record(
    const Ledger *ledger, const OccurrenceMatchList *occurrences,
    AggregatedRecord *record);
static int emit_aggregated_record_fields(const AggregatedRecord *record);
static void provenance_view_list_init(ProvenanceViewList *list);
static void provenance_view_list_free(ProvenanceViewList *list);
static int collect_occurrence_provenance(
    const Ledger *ledger, const OccurrenceMatchList *occurrences,
    ProvenanceViewList *list);
static int emit_provenance_list(const Ledger *ledger,
                                const ProvenanceViewList *list);
static int emit_best_mixed_route(const Ledger *ledger,
                                 const SelectedRoute *route,
                                 const RouteRunList *runs,
                                 size_t target_end);

static void gaplist_init(GapList *gaps);
static void gaplist_free(GapList *gaps);
static int gaplist_add(GapList *gaps, size_t start, size_t end);
static size_t utf8_next(const char *s, size_t pos);
static int find_lattice_gaps(const char *input, const Lattice *lat,
                             GapList *gaps);
static int show_lattice_gaps(const char *input, const Lattice *lat);
static void unload_ledger(Ledger *ledger);
static const char *surface_string(const Ledger *ledger, uint32_t surface);
static char *runtime_surface_string(const Ledger *ledger,
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
static int find_longest_from(const Ledger *ledger, const char *input,
                             const Lattice *lat, size_t edge_index,
                             uint32_t node, size_t depth, LongestPath *best,
                             SelectedRoute *route);
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
static int index_candy_surfaces(Ledger *ledger,
                                const MkledgerConfig *config);
static void candy_candidate_list_init(CandyCandidateList *list);
static void candy_candidate_list_free(CandyCandidateList *list);
static int candy_candidate_list_add(CandyCandidateList *list, size_t end,
                                    const char *text, uint32_t surface);
static int process_input(const Ledger *ledger, const MkledgerConfig *config,
                         int monitor, int unresolved, int append_candy,
                         int best);

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
                                        const LongestPathList *list,
                                        size_t resolved_end) {
  const LatticeEdge *a;
  const LatticeEdge *b;
  const LatticeEdge *best_a = NULL;
  const LatticeEdge *best_b = NULL;

  FILE *fp;

  if (config == NULL || config->candy.filename == NULL)
    return 0;

  for (a = lat->v; a < lat->v + lat->n; a++) {
    for (b = lat->v; b < lat->v + lat->n; b++) {
      const char *as;
      const char *bs;

      if (!lattice_edges_connect(input->text, a, b))
        continue;
      if (b->end <= resolved_end)
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

      if (best_a == NULL || a->end - a->start > best_a->end - best_a->start) {
        best_a = a;
        best_b = b;
      }

      if (a->surface > ledger->null_surface)
        free((char *)as);
      if (b->surface > ledger->null_surface)
        free((char *)bs);
    }
  }

  if (best_a == NULL)
    return 0;

  const char *best_as =
      best_a->surface > ledger->null_surface
          ? runtime_surface_string(ledger, best_a->surface)
          : surface_string(ledger, best_a->surface);
  const char *best_bs =
      best_b->surface > ledger->null_surface
          ? runtime_surface_string(ledger, best_b->surface)
          : surface_string(ledger, best_b->surface);

  if (best_as == NULL || best_bs == NULL) {
    if (best_a->surface > ledger->null_surface)
      free((char *)best_as);
    if (best_b->surface > ledger->null_surface)
      free((char *)best_bs);
    return -1;
  }

  int already_present =
      candy_has_work_pair(ledger, config, input, best_a, best_as,
                          best_b, best_bs);

  if (already_present < 0) {
    if (best_a->surface > ledger->null_surface)
      free((char *)best_as);
    if (best_b->surface > ledger->null_surface)
      free((char *)best_bs);
    return -1;
  }

  if (already_present) {
    if (best_a->surface > ledger->null_surface)
      free((char *)best_as);
    if (best_b->surface > ledger->null_surface)
      free((char *)best_bs);
    return 1;
  }

  fp = fopen(config->candy.filename, "a");
  if (fp == NULL) {
    if (best_a->surface > ledger->null_surface)
      free((char *)best_as);
    if (best_b->surface > ledger->null_surface)
      free((char *)best_bs);
    return -1;
  }

  int status =
      emit_adjacency_work_row(fp, config, input, best_a, best_as) != 0 ||
      emit_adjacency_work_row(fp, config, input, best_b, best_bs) != 0
          ? -1
          : 1;

  if (fclose(fp) != 0)
    status = -1;

  if (best_a->surface > ledger->null_surface)
    free((char *)best_as);
  if (best_b->surface > ledger->null_surface)
    free((char *)best_bs);

  return status;
}

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

      if (a->surface > ledger->null_surface)
        free((char *)as);
      if (b->surface > ledger->null_surface)
        free((char *)bs);
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
  size_t i;

  for (i = 0; i < list->n; i++)
    free(list->v[i].route);
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

  list->v[list->n] = *path;
  list->v[list->n].route = NULL;

  if (path->route_n != 0) {
    list->v[list->n].route =
        malloc(path->route_n * sizeof *list->v[list->n].route);
    if (list->v[list->n].route == NULL)
      return -1;
    memcpy(list->v[list->n].route, path->route,
           path->route_n * sizeof *path->route);
  }

  list->n++;
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

static void selected_route_init(SelectedRoute *route) {
  route->v = NULL;
  route->n = 0;
  route->cap = 0;
}

static void selected_route_free(SelectedRoute *route) {
  free(route->v);
  route->v = NULL;
  route->n = 0;
  route->cap = 0;
}

static int selected_route_add(SelectedRoute *route, size_t start, size_t end,
                              uint32_t surface) {
  LatticeEdge *tmp;

  if (route->n == route->cap) {
    size_t newcap = (route->cap == 0) ? 16 : route->cap * 2;

    tmp = realloc(route->v, newcap * sizeof *route->v);
    if (tmp == NULL)
      return -1;

    route->v = tmp;
    route->cap = newcap;
  }

  route->v[route->n].start = start;
  route->v[route->n].end = end;
  route->v[route->n].surface = surface;
  route->n++;

  return 0;
}

static void route_run_list_init(RouteRunList *runs) {
  runs->v = NULL;
  runs->n = 0;
  runs->cap = 0;
}

static void route_run_list_free(RouteRunList *runs) {
  free(runs->v);
  runs->v = NULL;
  runs->n = 0;
  runs->cap = 0;
}

static int route_run_list_add(RouteRunList *runs, size_t first,
                              size_t count) {
  RouteRun *tmp;

  if (runs->n == runs->cap) {
    size_t newcap = runs->cap == 0 ? 8 : runs->cap * 2;

    tmp = realloc(runs->v, newcap * sizeof *runs->v);
    if (tmp == NULL)
      return -1;

    runs->v = tmp;
    runs->cap = newcap;
  }

  runs->v[runs->n].first = first;
  runs->v[runs->n].count = count;
  runs->n++;
  return 0;
}

static int split_selected_route(const Ledger *ledger,
                                const SelectedRoute *route,
                                RouteRunList *runs) {
  size_t first;

  if (route->n == 0)
    return 0;

  first = 0;

  while (first < route->n) {
    bool runtime = route->v[first].surface > ledger->null_surface;
    size_t end = first + 1;

    while (end < route->n &&
           (route->v[end].surface > ledger->null_surface) == runtime)
      end++;

    if (route_run_list_add(runs, first, end - first) != 0)
      return -1;

    first = end;
  }

  return 0;
}

static size_t route_transfer_count(const RouteRunList *runs) {
  return runs->n == 0 ? 0 : runs->n - 1;
}

static void occurrence_match_list_init(OccurrenceMatchList *list) {
  list->v = NULL;
  list->n = 0;
  list->cap = 0;
}

static void occurrence_match_list_free(OccurrenceMatchList *list) {
  free(list->v);
  list->v = NULL;
  list->n = 0;
  list->cap = 0;
}

static int occurrence_match_list_add(OccurrenceMatchList *list,
                                     const OccurrenceRecord *occurrence) {
  const OccurrenceRecord **tmp;

  if (list->n == list->cap) {
    size_t newcap = list->cap == 0 ? 8 : list->cap * 2;

    tmp = realloc(list->v, newcap * sizeof *list->v);
    if (tmp == NULL)
      return -1;

    list->v = tmp;
    list->cap = newcap;
  }

  list->v[list->n++] = occurrence;
  return 0;
}

static int evaluate_selected_route(const Ledger *ledger,
                                   const SelectedRoute *route,
                                   RouteEvaluation *evaluation) {
  RouteRunList runs;

  route_run_list_init(&runs);
  if (split_selected_route(ledger, route, &runs) != 0) {
    route_run_list_free(&runs);
    return -1;
  }

  evaluation->transfers = route_transfer_count(&runs);
  evaluation->longest_surface = 0;
  {
    size_t i;

    for (i = 0; i < route->n; i++) {
      size_t span = route->v[i].end - route->v[i].start;

      if (span > evaluation->longest_surface)
        evaluation->longest_surface = span;
    }
  }
  route_run_list_free(&runs);
  return 0;
}

static int route_evaluation_compare(const RouteEvaluation *a,
                                    const RouteEvaluation *b) {
  if (a->transfers < b->transfers)
    return -1;
  if (a->transfers > b->transfers)
    return 1;
  if (a->longest_surface > b->longest_surface)
    return -1;
  if (a->longest_surface < b->longest_surface)
    return 1;
  return 0;
}

static int ledger_run_matches(const Ledger *ledger,
                              const SelectedRoute *route,
                              const RouteRun *run,
                              const OccurrenceRecord *first) {
  const OccurrenceRecord *end =
      ledger->occurrencev + ledger->occurrence->noccurrences;
  size_t i;

  if (run->count == 0 || first < ledger->occurrencev || first >= end)
    return 0;

  for (i = 0; i < run->count; i++) {
    const OccurrenceRecord *occurrence = first + i;
    const LatticeEdge *edge = &route->v[run->first + i];

    if (occurrence >= end)
      return 0;
    if (i != 0 && occurrence->sequence != first->sequence)
      return 0;
    if (occurrence->surface != edge->surface)
      return 0;
  }

  return 1;
}

static int collect_ledger_run_matches(const Ledger *ledger,
                                      const SelectedRoute *route,
                                      const RouteRun *run,
                                      OccurrenceMatchList *matches) {
  const OccurrenceRecord *occurrence;
  const OccurrenceRecord *end =
      ledger->occurrencev + ledger->occurrence->noccurrences;

  occurrence_match_list_init(matches);

  for (occurrence = ledger->occurrencev; occurrence < end; occurrence++) {
    if (!ledger_run_matches(ledger, route, run, occurrence))
      continue;

    if (occurrence_match_list_add(matches, occurrence) != 0) {
      occurrence_match_list_free(matches);
      return -1;
    }
  }

  return 0;
}

static void route_occurrence_matches_free(RouteOccurrenceMatches *matches) {
  size_t i;

  if (matches->v != NULL) {
    for (i = 0; i < matches->n; i++)
      occurrence_match_list_free(&matches->v[i]);
  }

  free(matches->v);
  matches->v = NULL;
  matches->n = 0;
}

static int collect_route_occurrence_matches(const Ledger *ledger,
                                            const SelectedRoute *route,
                                            const RouteRunList *runs,
                                            RouteOccurrenceMatches *matches) {
  size_t r;

  matches->v = calloc(runs->n, sizeof *matches->v);
  matches->n = runs->n;

  if (matches->v == NULL && runs->n != 0) {
    matches->n = 0;
    return -1;
  }

  for (r = 0; r < runs->n; r++) {
    const RouteRun *run = &runs->v[r];

    if (route->v[run->first].surface > ledger->null_surface)
      continue;

    if (collect_ledger_run_matches(ledger, route, run, &matches->v[r]) != 0) {
      route_occurrence_matches_free(matches);
      return -1;
    }
  }

  return 0;
}

static int collect_route_position_occurrences(
    const Ledger *ledger, const SelectedRoute *route,
    const RouteRunList *runs, const RouteOccurrenceMatches *matches,
    size_t route_index, OccurrenceMatchList *occurrences) {
  size_t r;

  occurrence_match_list_init(occurrences);

  if (route_index >= route->n)
    return -1;

  if (route->v[route_index].surface > ledger->null_surface)
    return 0;

  for (r = 0; r < runs->n; r++) {
    const RouteRun *run = &runs->v[r];
    size_t j;
    size_t m;

    if (route_index < run->first || route_index >= run->first + run->count)
      continue;

    if (r >= matches->n)
      return -1;

    j = route_index - run->first;

    for (m = 0; m < matches->v[r].n; m++) {
      if (occurrence_match_list_add(
              occurrences, matches->v[r].v[m] + j) != 0) {
        occurrence_match_list_free(occurrences);
        return -1;
      }
    }

    return 0;
  }

  return -1;
}

static size_t runtime_surface_offset(const Ledger *ledger, uint32_t surface) {
  size_t index;

  if (surface <= ledger->null_surface || surface > ledger->current_max_surface)
    return SIZE_MAX;

  index = (size_t)(surface - ledger->null_surface - 1);
  if (index >= ledger->runtime_surface_count)
    return SIZE_MAX;

  return ledger->runtime_surface_offsets[index];
}


static int selected_route_has_runtime(const Ledger *ledger,
                                      const SelectedRoute *route) {
  size_t i;

  for (i = 0; i < route->n; i++)
    if (route->v[i].surface > ledger->null_surface)
      return 1;

  return 0;
}

static int emit_runtime_record(const Ledger *ledger,
                               const LatticeEdge *edge) {
  size_t offset = runtime_surface_offset(ledger, edge->surface);
  const char *line;
  const char *nl;
  size_t line_len;
  char *copy;
  tjson_t json;

  if (offset == SIZE_MAX || offset >= ledger->candy_size ||
      ledger->candy_map == NULL)
    return -1;

  line = (const char *)ledger->candy_map + offset;
  nl = memchr(line, '\n', ledger->candy_size - offset);
  line_len = nl == NULL ? ledger->candy_size - offset : (size_t)(nl - line);

  if (line_len != 0 && line[line_len - 1] == '\r')
    line_len--;

  if (line_len == 0 || line[0] == '!')
    return -1;

  copy = strndup(line, line_len);
  if (copy == NULL)
    return -1;

  int first_field = 1;

  tjson_init(&json, "candy", copy);
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
    free(key);
    tjson_skip_ws(&json);
    tjson_expect(&json, ':');
    tjson_skip_ws(&json);
    tjson_skip_value(&json);
    tjson_skip_ws(&json);

    if (tjson_peek(&json) == ',') {
      tjson_expect(&json, ',');
      continue;
    }
    if (tjson_peek(&json) == '}') {
      tjson_expect(&json, '}');
      break;
    }

    free(copy);
    return -1;
  }

  tjson_skip_ws(&json);
  if (json.pos != json.length) {
    free(copy);
    return -1;
  }

  putchar('{');

  tjson_init(&json, "candy", copy);
  tjson_skip_ws(&json);
  tjson_expect(&json, '{');

  for (;;) {
    char *key;
    size_t value_start;
    size_t value_end;

    tjson_skip_ws(&json);
    if (tjson_peek(&json) == '}') {
      tjson_expect(&json, '}');
      break;
    }

    key = tjson_parse_string(&json);
    tjson_skip_ws(&json);
    tjson_expect(&json, ':');
    tjson_skip_ws(&json);

    value_start = json.pos;
    tjson_skip_value(&json);
    value_end = json.pos;

    if (strcmp(key, "start") != 0 && strcmp(key, "end") != 0 &&
        strcmp(key, "text") != 0) {
      if (!first_field)
        putchar(',');
      print_json_string_to(stdout, key);
      putchar(':');
      if (fwrite(copy + value_start, 1, value_end - value_start, stdout) !=
          value_end - value_start) {
        free(key);
        free(copy);
        return -1;
      }
      first_field = 0;
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

    free(copy);
    return -1;
  }

  free(copy);
  return putchar('}') == EOF ? -1 : 0;
}

static int emit_mixed_route_from(const Ledger *ledger,
                                 const SelectedRoute *route,
                                 const RouteRunList *runs, size_t run_index,
                                 const OccurrenceRecord **ledger_first,
                                 size_t target_end) {
  const RouteRun *run;

  if (run_index == runs->n) {
    size_t r;
    int first_record = 1;

    printf("{\"start\":%zu,\"end\":%zu,\"records\":[",
           route->v[0].start, target_end);

    for (r = 0; r < runs->n; r++) {
      const RouteRun *out_run = &runs->v[r];
      const LatticeEdge *first_edge = &route->v[out_run->first];
      size_t j;

      if (first_edge->surface > ledger->null_surface) {
        for (j = 0; j < out_run->count; j++) {
          if (!first_record)
            putchar(',');
          if (emit_runtime_record(
                  ledger, &route->v[out_run->first + j]) != 0)
            return -1;
          first_record = 0;
        }
      } else {
        for (j = 0; j < out_run->count; j++) {
          if (!first_record)
            putchar(',');
          if (emit_occurrence(ledger, ledger_first[r] + j) != 0)
            return -1;
          first_record = 0;
        }
      }
    }

    fputs("]}\n", stdout);
    return 0;
  }

  run = &runs->v[run_index];

  if (route->v[run->first].surface > ledger->null_surface)
    return emit_mixed_route_from(ledger, route, runs, run_index + 1,
                                 ledger_first, target_end);

  {
    const OccurrenceRecord *occurrence;
    const OccurrenceRecord *end =
        ledger->occurrencev + ledger->occurrence->noccurrences;

    for (occurrence = ledger->occurrencev; occurrence < end; occurrence++) {
      if (!ledger_run_matches(ledger, route, run, occurrence))
        continue;

      ledger_first[run_index] = occurrence;
      if (emit_mixed_route_from(ledger, route, runs, run_index + 1,
                                ledger_first, target_end) != 0)
        return -1;
    }
  }

  return 0;
}

static int emit_best_mixed_route(const Ledger *ledger,
                                 const SelectedRoute *route,
                                 const RouteRunList *runs,
                                 size_t target_end) {
  RouteOccurrenceMatches matches;
  size_t i;
  int first_record = 1;

  if (collect_route_occurrence_matches(ledger, route, runs, &matches) != 0)
    return -1;

  printf("{\"start\":%zu,\"end\":%zu,\"records\":[",
         route->v[0].start, target_end);

  for (i = 0; i < route->n; i++) {
    if (!first_record)
      putchar(',');

    if (route->v[i].surface > ledger->null_surface) {
      if (emit_runtime_record(ledger, &route->v[i]) != 0) {
        route_occurrence_matches_free(&matches);
        return -1;
      }
    } else {
      OccurrenceMatchList occurrences;
      AggregatedRecord record;
      ProvenanceViewList provenance;

      if (collect_route_position_occurrences(
              ledger, route, runs, &matches, i, &occurrences) != 0) {
        route_occurrence_matches_free(&matches);
        return -1;
      }

      if (aggregate_occurrence_record(ledger, &occurrences, &record) != 0) {
        occurrence_match_list_free(&occurrences);
        route_occurrence_matches_free(&matches);
        return -1;
      }

      if (collect_occurrence_provenance(
              ledger, &occurrences, &provenance) != 0) {
        aggregated_record_free(&record);
        occurrence_match_list_free(&occurrences);
        route_occurrence_matches_free(&matches);
        return -1;
      }

      putchar('{');
      if (emit_aggregated_record_fields(&record) != 0) {
        provenance_view_list_free(&provenance);
        aggregated_record_free(&record);
        occurrence_match_list_free(&occurrences);
        route_occurrence_matches_free(&matches);
        return -1;
      }
      fputs(",\"provenance\":", stdout);
      if (emit_provenance_list(ledger, &provenance) != 0) {
        provenance_view_list_free(&provenance);
        aggregated_record_free(&record);
        occurrence_match_list_free(&occurrences);
        route_occurrence_matches_free(&matches);
        return -1;
      }
      putchar('}');

      provenance_view_list_free(&provenance);
      aggregated_record_free(&record);
      occurrence_match_list_free(&occurrences);
    }

    first_record = 0;
  }

  fputs("]}\n", stdout);
  route_occurrence_matches_free(&matches);
  return ferror(stdout) ? -1 : 0;
}

static int emit_mixed_route(const Ledger *ledger,
                            const SelectedRoute *route, size_t target_end,
                            int best_mode) {
  RouteRunList runs;
  const OccurrenceRecord **ledger_first;
  int status;

  route_run_list_init(&runs);

  if (split_selected_route(ledger, route, &runs) != 0) {
    route_run_list_free(&runs);
    return -1;
  }

  if (runs.n == 0) {
    route_run_list_free(&runs);
    return 0;
  }

  if (best_mode) {
    status = emit_best_mixed_route(ledger, route, &runs, target_end);
    route_run_list_free(&runs);
    return status;
  }

  ledger_first = calloc(runs.n, sizeof *ledger_first);
  if (ledger_first == NULL) {
    route_run_list_free(&runs);
    return -1;
  }

  status = emit_mixed_route_from(ledger, route, &runs, 0, ledger_first,
                                 target_end);

  free(ledger_first);
  route_run_list_free(&runs);
  return status;
}

static int longest_path_set_route(LongestPath *path,
                                  const SelectedRoute *route) {
  LatticeEdge *copy = NULL;

  if (route->n != 0) {
    copy = malloc(route->n * sizeof *copy);
    if (copy == NULL)
      return -1;
    memcpy(copy, route->v, route->n * sizeof *copy);
  }

  free(path->route);
  path->route = copy;
  path->route_n = route->n;
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

static void field_value_list_init(FieldValueList *list) {
  list->v = NULL;
  list->n = 0;
  list->cap = 0;
}

static void field_value_list_free(FieldValueList *list) {
  free(list->v);
  list->v = NULL;
  list->n = 0;
  list->cap = 0;
}

static int field_value_list_add_unique(FieldValueList *list,
                                       const char *value) {
  const char **tmp;
  size_t i;

  for (i = 0; i < list->n; i++)
    if (strcmp(list->v[i], value) == 0)
      return 0;

  if (list->n == list->cap) {
    size_t newcap = list->cap == 0 ? 4 : list->cap * 2;

    tmp = realloc(list->v, newcap * sizeof *list->v);
    if (tmp == NULL)
      return -1;

    list->v = tmp;
    list->cap = newcap;
  }

  list->v[list->n++] = value;
  return 0;
}

static int collect_occurrence_field_values(
    const Ledger *ledger, const OccurrenceMatchList *occurrences,
    const char *field_name, uint32_t field_type, FieldValueList *values) {
  const char *end = (const char *)ledger->occurrence;
  size_t i;

  field_value_list_init(values);

  for (i = 0; i < occurrences->n; i++) {
    const CombineRecordHeader *combine;
    const char *fieldp;
    uint32_t j;

    if (occurrence_combine_fields(ledger, occurrences->v[i],
                                  &combine, &fieldp) != 0) {
      field_value_list_free(values);
      return -1;
    }

    for (j = 0; j < combine->nfields; j++) {
      LedgerFieldView view;
      const char *next = ledger_field_view(fieldp, end, &view);

      if (next == NULL) {
        field_value_list_free(values);
        return -1;
      }

      if (view.type == field_type && strcmp(view.name, field_name) == 0) {
        if (field_value_list_add_unique(values, view.value) != 0) {
          field_value_list_free(values);
          return -1;
        }
        break;
      }

      fieldp = next;
    }
  }

  return 0;
}

static void aggregated_record_free(AggregatedRecord *record) {
  size_t i;

  if (record->v != NULL) {
    for (i = 0; i < record->n; i++)
      field_value_list_free(&record->v[i].values);
  }

  free(record->v);
  record->v = NULL;
  record->n = 0;
}

static int aggregate_occurrence_record(
    const Ledger *ledger, const OccurrenceMatchList *occurrences,
    AggregatedRecord *record) {
  const CombineRecordHeader *combine;
  const char *p;
  const char *end = (const char *)ledger->occurrence;
  uint32_t j;

  record->v = NULL;
  record->n = 0;

  if (occurrences->n == 0)
    return 0;

  if (occurrence_combine_fields(
          ledger, occurrences->v[0], &combine, &p) != 0)
    return -1;

  record->v = calloc(combine->nfields, sizeof *record->v);
  if (record->v == NULL && combine->nfields != 0)
    return -1;
  record->n = combine->nfields;

  for (j = 0; j < combine->nfields; j++) {
    LedgerFieldView view;
    const char *next = ledger_field_view(p, end, &view);

    if (next == NULL) {
      aggregated_record_free(record);
      return -1;
    }

    record->v[j].name = view.name;
    record->v[j].type = view.type;

    if (collect_occurrence_field_values(
            ledger, occurrences, view.name, view.type,
            &record->v[j].values) != 0) {
      aggregated_record_free(record);
      return -1;
    }

    p = next;
  }

  return 0;
}

static void provenance_view_list_init(ProvenanceViewList *list) {
  list->v = NULL;
  list->n = 0;
  list->cap = 0;
}

static void provenance_view_list_free(ProvenanceViewList *list) {
  free(list->v);
  list->v = NULL;
  list->n = 0;
  list->cap = 0;
}

static int collect_occurrence_provenance(
    const Ledger *ledger, const OccurrenceMatchList *occurrences,
    ProvenanceViewList *list) {
  const char *end = (const char *)ledger->occurrence;
  size_t i;

  provenance_view_list_init(list);

  for (i = 0; i < occurrences->n; i++) {
    const OccurrenceRecord *occurrence = occurrences->v[i];
    const CombineRecordHeader *combine;
    const char *p;
    uint32_t j;

    if (occurrence_combine_fields(ledger, occurrence, &combine, &p) != 0)
      goto fail;

    for (j = 0; j < combine->nfields; j++) {
      p = skip_ledger_field(p, end);
      if (p == NULL)
        goto fail;
    }

    if (occurrence->provenance >= combine->nprovenance)
      goto fail;

    for (j = 0; j < occurrence->provenance; j++) {
      const ProvenanceRecordHeader *provenance;
      uint32_t k;

      if ((size_t)(end - p) < sizeof *provenance)
        goto fail;

      provenance = (const ProvenanceRecordHeader *)p;
      p += sizeof *provenance;

      for (k = 0; k < provenance->nfields; k++) {
        p = skip_ledger_field(p, end);
        if (p == NULL)
          goto fail;
      }
    }

    {
      const ProvenanceRecordHeader *provenance;
      ProvenanceView *tmp;

      if ((size_t)(end - p) < sizeof *provenance)
        goto fail;

      provenance = (const ProvenanceRecordHeader *)p;

      if (list->n == list->cap) {
        size_t newcap = list->cap == 0 ? 4 : list->cap * 2;

        tmp = realloc(list->v, newcap * sizeof *list->v);
        if (tmp == NULL)
          goto fail;

        list->v = tmp;
        list->cap = newcap;
      }

      list->v[list->n].combine = combine;
      list->v[list->n].provenance = provenance;
      list->n++;
    }
  }

  return 0;

fail:
  provenance_view_list_free(list);
  return -1;
}

static int emit_provenance_list(const Ledger *ledger,
                                const ProvenanceViewList *list) {
  const char *end = (const char *)ledger->occurrence;
  size_t i;

  putchar('[');

  for (i = 0; i < list->n; i++) {
    const ProvenanceRecordHeader *provenance = list->v[i].provenance;
    const char *p = (const char *)(provenance + 1);
    uint32_t j;

    if (i != 0)
      putchar(',');

    putchar('{');

    for (j = 0; j < provenance->nfields; j++) {
      if (j != 0)
        putchar(',');

      p = print_ledger_field_json(p, end);
      if (p == NULL)
        return -1;
    }

    putchar('}');
  }

  putchar(']');
  return ferror(stdout) ? -1 : 0;
}

static int emit_aggregated_record_fields(const AggregatedRecord *record) {
  size_t i;

  for (i = 0; i < record->n; i++) {
    const AggregatedField *field = &record->v[i];
    size_t j;

    if (i != 0)
      putchar(',');

    print_json_string(field->name);
    putchar(':');

    if (field->values.n == 0) {
      if (field->type == 0)
        print_json_string("");
      else
        fputs("0", stdout);
      continue;
    }

    if (field->values.n == 1) {
      if (field->type == 0)
        print_json_string(field->values.v[0]);
      else
        fputs(field->values.v[0], stdout);
      continue;
    }

    if (field->type == 0) {
      putchar('"');
      for (j = 0; j < field->values.n; j++) {
        if (j != 0)
          putchar(',');
        fputs(field->values.v[j], stdout);
      }
      putchar('"');
    } else {
      fputs(field->values.v[0], stdout);
    }
  }

  return ferror(stdout) ? -1 : 0;
}

static const char *ledger_field_view(const char *p, const char *end,
                                     LedgerFieldView *view) {
  const LedgerFieldHeader *field;

  if ((size_t)(end - p) < sizeof *field)
    return NULL;

  field = (const LedgerFieldHeader *)p;
  p += sizeof *field;

  if (field->name_bytes == 0 || field->name_bytes > (size_t)(end - p))
    return NULL;
  view->name = p;
  p += field->name_bytes;

  if (field->value_bytes == 0 || field->value_bytes > (size_t)(end - p))
    return NULL;
  view->value = p;
  p += field->value_bytes;

  if (view->name[field->name_bytes - 1] != '\0' ||
      view->value[field->value_bytes - 1] != '\0')
    return NULL;

  if (field->type != 0 && field->type != 1)
    return NULL;

  view->type = field->type;
  return p;
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

static int occurrence_combine_fields(
    const Ledger *ledger, const OccurrenceRecord *occurrence,
    const CombineRecordHeader **combine_out, const char **fields_out) {
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
      *combine_out = combine;
      *fields_out = fields;
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

static char *runtime_surface_string(const Ledger *ledger,
                                    uint32_t surface) {
  size_t index;
  size_t offset;
  const char *line;
  const char *nl;
  size_t line_len;
  char *copy;
  char *text;

  if (surface <= ledger->null_surface ||
      surface > ledger->current_max_surface)
    return NULL;

  index = (size_t)(surface - ledger->null_surface - 1);
  if (index >= ledger->runtime_surface_count || ledger->candy_map == NULL)
    return NULL;

  offset = ledger->runtime_surface_offsets[index];
  if (offset >= ledger->candy_size)
    return NULL;

  line = (const char *)ledger->candy_map + offset;
  nl = memchr(line, '\n', ledger->candy_size - offset);
  line_len = nl == NULL ? ledger->candy_size - offset : (size_t)(nl - line);

  if (line_len != 0 && line[line_len - 1] == '\r')
    line_len--;

  copy = strndup(line, line_len);
  if (copy == NULL)
    return NULL;

  text = parse_candy_text(copy);
  free(copy);
  return text;
}

static uint32_t runtime_surface_at_offset(const Ledger *ledger,
                                          size_t offset) {
  size_t i;

  for (i = 0; i < ledger->runtime_surface_count; i++)
    if (ledger->runtime_surface_offsets[i] == offset)
      return ledger->null_surface + 1 + (uint32_t)i;

  return ledger->null_surface;
}

static void unload_ledger(Ledger *ledger) {
  free(ledger->runtime_surface_offsets);

  if (ledger->candy_map != NULL)
    munmap(ledger->candy_map, ledger->candy_size);

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
  ledger->runtime_surface_offsets = NULL;
  ledger->runtime_surface_count = 0;
  ledger->runtime_surface_cap = 0;
  ledger->candy_map = NULL;
  ledger->candy_size = 0;

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

static int find_longest_from(const Ledger *ledger, const char *input,
                             const Lattice *lat, size_t edge_index,
                             uint32_t node, size_t depth, LongestPath *best,
                             SelectedRoute *route) {
  const LatticeEdge *edge = &lat->v[edge_index];
  size_t i;

  if (selected_route_add(route, edge->start, edge->end, edge->surface) != 0)
    return -1;

  if (best->route_n == 0 || edge->end > best->end ||
      (edge->end == best->end && depth > best->depth)) {
    best->end = edge->end;
    best->depth = depth;
    best->freq = ledger->trie[node].freq;
    best->surface = edge->surface;
    if (longest_path_set_route(best, route) != 0) {
      route->n--;
      return -1;
    }
  }

  for (i = 0; i < lat->n; i++) {
    const LatticeEdge *next = &lat->v[i];
    uint32_t child;

    if (!lattice_edges_connect(input, edge, next))
      continue;

    child = trie_find(ledger, node, next->surface);
    if (child == TRIE_NONE)
      continue;

    if (find_longest_from(ledger, input, lat, i, child, depth + 1, best,
                          route) != 0) {
      route->n--;
      return -1;
    }
  }

  route->n--;
  return 0;
}

static int make_longest_paths(const Ledger *ledger, const char *input,
                              const Lattice *lat, LongestPathList *list) {
  size_t i;

  for (i = 0; i < lat->n; i++) {
    uint32_t node;
    LongestPath best;
    SelectedRoute route;

    node = trie_find(ledger, 0, lat->v[i].surface);
    if (node == TRIE_NONE)
      continue;

    best.start = lat->v[i].start;
    best.end = lat->v[i].end;
    best.depth = 1;
    best.freq = ledger->trie[node].freq;
    best.surface = lat->v[i].surface;
    best.route = NULL;
    best.route_n = 0;
    selected_route_init(&route);

    if (find_longest_from(ledger, input, lat, i, node, 1, &best, &route) != 0) {
      selected_route_free(&route);
      free(best.route);
      return -1;
    }
    selected_route_free(&route);

    if (longest_path_list_add(list, &best) != 0) {
      free(best.route);
      return -1;
    }
    free(best.route);
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

static int candy_has_unresolved(const Ledger *ledger,
                                const MkledgerConfig *config,
                                const SuiInput *input, size_t start,
                                size_t end) {
  char *expected = NULL;
  size_t expected_len = 0;
  FILE *fp;
  const char *base;
  size_t offset = 0;
  int found = 0;

  fp = open_memstream(&expected, &expected_len);
  if (fp == NULL)
    return 0;

  if (emit_unresolved(fp, config, input, start, end) != 0 ||
      fclose(fp) != 0) {
    free(expected);
    return 0;
  }

  if (ledger->candy_map == NULL || ledger->candy_size == 0) {
    free(expected);
    return 0;
  }

  base = (const char *)ledger->candy_map;

  while (offset < ledger->candy_size) {
    const char *line = base + offset;
    const char *nl = memchr(line, '\n', ledger->candy_size - offset);
    size_t line_len = nl == NULL ? ledger->candy_size - offset
                                 : (size_t)(nl - line);

    if (line_len != 0 && line[line_len - 1] == '\r')
      line_len--;

    if (expected_len != 0 && expected[expected_len - 1] == '\n' &&
        line_len == expected_len - 1 &&
        memcmp(line, expected, line_len) == 0) {
      found = 1;
      break;
    }

    if (nl == NULL)
      break;
    offset = (size_t)(nl - base) + 1;
  }

  free(expected);
  return found;
}

static char *parse_candy_text(const char *line) {
  tjson_t json;
  char *text = NULL;

  tjson_init(&json, "candy", line[0] == '!' ? line + 1 : line);
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
  int fd;
  struct stat st;
  const char *base;
  size_t offset = 0;
  uint32_t surface = ledger->null_surface;

  if (config == NULL || config->candy.filename == NULL)
    return 0;

  fd = open(config->candy.filename, O_RDONLY);
  if (fd < 0)
    return 0;

  if (fstat(fd, &st) != 0) {
    close(fd);
    return -1;
  }

  ledger->candy_size = (size_t)st.st_size;
  if (ledger->candy_size == 0) {
    close(fd);
    return 0;
  }

  ledger->candy_map =
      mmap(NULL, ledger->candy_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);

  if (ledger->candy_map == MAP_FAILED) {
    ledger->candy_map = NULL;
    ledger->candy_size = 0;
    return -1;
  }

  base = (const char *)ledger->candy_map;

  while (offset < ledger->candy_size) {
    const char *line = base + offset;
    const char *nl = memchr(line, '\n', ledger->candy_size - offset);
    size_t line_len = nl == NULL ? ledger->candy_size - offset
                                 : (size_t)(nl - line);
    char *copy;
    char *text;

    if (line_len != 0 && line[line_len - 1] == '\r')
      line_len--;

    copy = strndup(line, line_len);
    if (copy == NULL)
      return -1;

    if (copy[0] == '!') {
      free(copy);
      goto next_line;
    }

    text = parse_candy_text(copy);
    free(copy);

    if (text != NULL) {
      size_t *new_offsets;

      free(text);

      if (surface == UINT32_MAX)
        return -1;

      if (ledger->runtime_surface_count == ledger->runtime_surface_cap) {
        size_t new_cap = ledger->runtime_surface_cap == 0
                             ? 16
                             : ledger->runtime_surface_cap * 2;

        new_offsets = realloc(ledger->runtime_surface_offsets,
                              new_cap * sizeof(*new_offsets));
        if (new_offsets == NULL)
          return -1;

        ledger->runtime_surface_offsets = new_offsets;
        ledger->runtime_surface_cap = new_cap;
      }

      ledger->runtime_surface_offsets[ledger->runtime_surface_count++] = offset;
      surface++;
    }

next_line:
    if (nl == NULL)
      break;
    offset = (size_t)(nl - base) + 1;
  }

  ledger->current_max_surface = surface;
  return 0;
}

static size_t ignored_advance(const MkledgerConfig *config,
                              const char *input, size_t target_end) {
  size_t i;
  size_t best_end = target_end;

  if (config == NULL)
    return target_end;

  for (i = 0; i < config->ledger.nignore; i++) {
    const char *ignored = config->ledger.ignore[i];
    size_t len = strlen(ignored);

    if (len == 0)
      continue;

    if (strncmp(input + target_end, ignored, len) == 0 &&
        target_end + len > best_end)
      best_end = target_end + len;
  }

  return best_end;
}

static void candy_candidate_list_init(CandyCandidateList *list) {
  list->v = NULL;
  list->n = 0;
  list->cap = 0;
}

static void candy_candidate_list_free(CandyCandidateList *list) {
  size_t i;

  for (i = 0; i < list->n; i++)
    free(list->v[i].text);
  free(list->v);
  list->v = NULL;
  list->n = 0;
  list->cap = 0;
}

static int candy_candidate_list_add(CandyCandidateList *list, size_t end,
                                    const char *text, uint32_t surface) {
  CandyCandidate *tmp;

  if (list->n == list->cap) {
    size_t newcap = list->cap == 0 ? 8 : list->cap * 2;

    tmp = realloc(list->v, newcap * sizeof *list->v);
    if (tmp == NULL)
      return -1;
    list->v = tmp;
    list->cap = newcap;
  }

  list->v[list->n].text = strdup(text);
  if (list->v[list->n].text == NULL)
    return -1;
  list->v[list->n].end = end;
  list->v[list->n].surface = surface;
  list->n++;
  return 0;
}

static int candy_approved_candidates(const Ledger *ledger,
                                     const char *input, size_t target_end,
                                     CandyCandidateList *list) {
  const char *base;
  size_t offset = 0;
  char *previous = NULL;

  if (ledger->candy_map == NULL || ledger->candy_size == 0)
    return 0;

  base = (const char *)ledger->candy_map;

  while (offset < ledger->candy_size) {
    const char *line = base + offset;
    const char *nl = memchr(line, '\n', ledger->candy_size - offset);
    size_t line_len = nl == NULL ? ledger->candy_size - offset
                                 : (size_t)(nl - line);
    char *copy;
    char *text = NULL;

    if (line_len != 0 && line[line_len - 1] == '\r')
      line_len--;

    copy = strndup(line, line_len);
    if (copy == NULL) {
      free(previous);
      return -1;
    }

    if (copy[0] != '!')
      text = parse_candy_text(copy);
    free(copy);

    if (text != NULL && previous != NULL) {
      size_t previous_len = strlen(previous);
      size_t text_len = strlen(text);

      if (previous_len <= target_end &&
          memcmp(input + target_end - previous_len, previous,
                 previous_len) == 0 &&
          memcmp(input + target_end, text, text_len) == 0 &&
          candy_candidate_list_add(
              list, target_end + text_len, text,
              runtime_surface_at_offset(ledger, offset)) != 0) {
        free(previous);
        free(text);
        return -1;
      }
    }

    free(previous);
    previous = text;

    if (nl == NULL)
      break;
    offset = (size_t)(nl - base) + 1;
  }

  free(previous);
  return 0;
}

static size_t candy_approved_advance(const Ledger *ledger,
                                     const char *input, size_t target_end,
                                     char **accepted_text,
                                     uint32_t *accepted_surface) {
  CandyCandidateList candidates;
  size_t result = target_end;

  if (accepted_text != NULL)
    *accepted_text = NULL;
  if (accepted_surface != NULL)
    *accepted_surface = ledger->null_surface;

  candy_candidate_list_init(&candidates);
  if (candy_approved_candidates(ledger, input, target_end, &candidates) != 0) {
    candy_candidate_list_free(&candidates);
    return target_end;
  }

  if (candidates.n != 0) {
    result = candidates.v[0].end;
    if (accepted_text != NULL)
      *accepted_text = strdup(candidates.v[0].text);
    if (accepted_surface != NULL)
      *accepted_surface = candidates.v[0].surface;
  }

  candy_candidate_list_free(&candidates);
  return result;
}

static size_t resume_from_confirmed_surface(
    const Ledger *ledger, const char *input, const Lattice *lat,
    size_t target_end, const char *surface_text, SelectedRoute *accepted,
    int best_mode) {
  size_t i;
  size_t best_end = target_end;
  size_t text_len;
  LatticeEdge *best_route = NULL;
  size_t best_route_n = 0;

  if (surface_text == NULL)
    return target_end;

  text_len = strlen(surface_text);

  for (i = 0; i < lat->n; i++) {
    const LatticeEdge *edge = &lat->v[i];
    uint32_t node;
    LongestPath best;

    if (edge->end != target_end || edge->end - edge->start != text_len)
      continue;
    if (memcmp(input + edge->start, surface_text, text_len) != 0)
      continue;

    node = trie_find(ledger, 0, edge->surface);
    if (node == TRIE_NONE)
      continue;

    best.start = edge->start;
    best.end = edge->end;
    best.depth = 1;
    best.freq = ledger->trie[node].freq;
    best.surface = edge->surface;
    best.route = NULL;
    best.route_n = 0;

    {
      SelectedRoute route;
      selected_route_init(&route);
      if (find_longest_from(ledger, input, lat, i, node, 1, &best, &route) != 0) {
        selected_route_free(&route);
        free(best.route);
        free(best_route);
        return best_end;
      }
      selected_route_free(&route);
    }

    if (best.end > best_end) {
      free(best_route);
      best_route = best.route;
      best_route_n = best.route_n;
      best.route = NULL;
      best.route_n = 0;
      best_end = best.end;
    } else if (best_mode && best.end == best_end && best_route != NULL) {
      RouteEvaluation candidate_evaluation;
      RouteEvaluation best_evaluation;

      if (evaluate_route_continuation(ledger, accepted, best.route,
                                      best.route_n,
                                      &candidate_evaluation) == 0 &&
          evaluate_route_continuation(ledger, accepted, best_route,
                                      best_route_n,
                                      &best_evaluation) == 0 &&
          route_evaluation_compare(&candidate_evaluation,
                                   &best_evaluation) < 0) {
        free(best_route);
        best_route = best.route;
        best_route_n = best.route_n;
        best.route = NULL;
        best.route_n = 0;
      }
    }

    free(best.route);
  }

  if (accepted != NULL && best_route != NULL) {
    size_t j;

    for (j = 1; j < best_route_n; j++)
      if (selected_route_add(accepted, best_route[j].start,
                             best_route[j].end,
                             best_route[j].surface) != 0) {
        free(best_route);
        return best_end;
      }
  }

  free(best_route);
  return best_end;
}

static int process_input(const Ledger *ledger, const MkledgerConfig *config,
                         int monitor, int unresolved, int append_candy,
                         int best) {
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

  (void)best; /* best-output evaluation is added independently of routing. */

  if (make_lattice(ledger, input, &lat) != 0) {
    free_sui_input(&parsed);
    lattice_free(&lat);
    return -1;
  }

  if (!monitor) {
    LongestPathList paths;
    const LongestPath *path;
    const LongestPath *initial = NULL;
    SelectedRoute accepted;
    size_t target_end = 0;

    longest_path_list_init(&paths);
    selected_route_init(&accepted);

    if (make_longest_paths(ledger, input, &lat, &paths) != 0) {
      longest_path_list_free(&paths);
      lattice_free(&lat);
      free_sui_input(&parsed);
      return -1;
    }

    for (path = paths.v; path < paths.v + paths.n; path++)
      if (path->start == 0 && path->end > target_end) {
        target_end = path->end;
        initial = path;
      }

    if (initial != NULL) {
      size_t i;
      size_t candidate_count =
          longest_path_candidate_count(&paths, initial->start, initial->end);

      if (best && candidate_count > 1) {
        const LongestPath *candidate;
        const LongestPath *best_candidate = initial;
        RouteEvaluation best_evaluation;
        SelectedRoute candidate_route;

        selected_route_init(&candidate_route);
        for (i = 0; i < best_candidate->route_n; i++)
          if (selected_route_add(&candidate_route,
                                 best_candidate->route[i].start,
                                 best_candidate->route[i].end,
                                 best_candidate->route[i].surface) != 0) {
            selected_route_free(&candidate_route);
            selected_route_free(&accepted);
            longest_path_list_free(&paths);
            lattice_free(&lat);
            free_sui_input(&parsed);
            return -1;
          }

        if (evaluate_selected_route(ledger, &candidate_route,
                                    &best_evaluation) != 0) {
          selected_route_free(&candidate_route);
          selected_route_free(&accepted);
          longest_path_list_free(&paths);
          lattice_free(&lat);
          free_sui_input(&parsed);
          return -1;
        }
        selected_route_free(&candidate_route);

        for (candidate = paths.v; candidate < paths.v + paths.n;
             candidate++) {
          RouteEvaluation evaluation;

          if (candidate == best_candidate ||
              candidate->start != initial->start ||
              candidate->end != initial->end)
            continue;

          selected_route_init(&candidate_route);
          for (i = 0; i < candidate->route_n; i++)
            if (selected_route_add(&candidate_route,
                                   candidate->route[i].start,
                                   candidate->route[i].end,
                                   candidate->route[i].surface) != 0) {
              selected_route_free(&candidate_route);
              selected_route_free(&accepted);
              longest_path_list_free(&paths);
              lattice_free(&lat);
              free_sui_input(&parsed);
              return -1;
            }

          if (evaluate_selected_route(ledger, &candidate_route,
                                      &evaluation) != 0) {
            selected_route_free(&candidate_route);
            selected_route_free(&accepted);
            longest_path_list_free(&paths);
            lattice_free(&lat);
            free_sui_input(&parsed);
            return -1;
          }
          selected_route_free(&candidate_route);

          if (route_evaluation_compare(&evaluation, &best_evaluation) < 0) {
            best_candidate = candidate;
            best_evaluation = evaluation;
          }
        }

        initial = best_candidate;
      }

      for (i = 0; i < initial->route_n; i++)
        if (selected_route_add(&accepted, initial->route[i].start,
                               initial->route[i].end,
                               initial->route[i].surface) != 0) {
          selected_route_free(&accepted);
          longest_path_list_free(&paths);
          lattice_free(&lat);
          free_sui_input(&parsed);
          return -1;
        }
    }

    for (;;) {
      size_t previous_end = target_end;
      char *accepted_text = NULL;
      uint32_t accepted_surface = ledger->null_surface;
      size_t candy_start = target_end;
      size_t candy_end;

      if (best) {
        CandyCandidateList candidates;
        SelectedRoute best_route;
        RouteEvaluation best_evaluation = {0};
        size_t best_end = target_end;
        int have_best = 0;
        size_t ci;

        candy_candidate_list_init(&candidates);
        selected_route_init(&best_route);

        if (candy_approved_candidates(ledger, input, target_end,
                                      &candidates) != 0) {
          candy_candidate_list_free(&candidates);
          selected_route_free(&best_route);
          selected_route_free(&accepted);
          longest_path_list_free(&paths);
          lattice_free(&lat);
          free_sui_input(&parsed);
          return -1;
        }

        for (ci = 0; ci < candidates.n; ci++) {
          SelectedRoute candidate_route;
          RouteEvaluation evaluation;
          size_t candidate_end;
          size_t j;

          selected_route_init(&candidate_route);
          for (j = 0; j < accepted.n; j++)
            if (selected_route_add(&candidate_route,
                                   accepted.v[j].start,
                                   accepted.v[j].end,
                                   accepted.v[j].surface) != 0) {
              selected_route_free(&candidate_route);
              candy_candidate_list_free(&candidates);
              selected_route_free(&best_route);
              selected_route_free(&accepted);
              longest_path_list_free(&paths);
              lattice_free(&lat);
              free_sui_input(&parsed);
              return -1;
            }

          if (selected_route_add(&candidate_route, candy_start,
                                 candidates.v[ci].end,
                                 candidates.v[ci].surface) != 0) {
            selected_route_free(&candidate_route);
            candy_candidate_list_free(&candidates);
            selected_route_free(&best_route);
            selected_route_free(&accepted);
            longest_path_list_free(&paths);
            lattice_free(&lat);
            free_sui_input(&parsed);
            return -1;
          }

          candidate_end = resume_from_confirmed_surface(
              ledger, input, &lat, candidates.v[ci].end,
              candidates.v[ci].text, &candidate_route, 1);

          if (evaluate_selected_route(ledger, &candidate_route,
                                      &evaluation) != 0) {
            selected_route_free(&candidate_route);
            candy_candidate_list_free(&candidates);
            selected_route_free(&best_route);
            selected_route_free(&accepted);
            longest_path_list_free(&paths);
            lattice_free(&lat);
            free_sui_input(&parsed);
            return -1;
          }

          if (!have_best || candidate_end > best_end ||
              (candidate_end == best_end &&
               route_evaluation_compare(&evaluation,
                                        &best_evaluation) < 0)) {
            selected_route_free(&best_route);
            best_route = candidate_route;
            selected_route_init(&candidate_route);
            best_end = candidate_end;
            best_evaluation = evaluation;
            have_best = 1;
          }

          selected_route_free(&candidate_route);
        }

        if (have_best) {
          selected_route_free(&accepted);
          accepted = best_route;
          selected_route_init(&best_route);
          target_end = best_end;
        }

        selected_route_free(&best_route);
        candy_candidate_list_free(&candidates);
        candy_end = target_end;
      } else {
        candy_end = candy_approved_advance(
            ledger, input, target_end, &accepted_text, &accepted_surface);

        if (candy_end > target_end) {
          if (accepted_surface > ledger->null_surface &&
              selected_route_add(&accepted, candy_start, candy_end,
                                 accepted_surface) != 0) {
            free(accepted_text);
            selected_route_free(&accepted);
            longest_path_list_free(&paths);
            lattice_free(&lat);
            free_sui_input(&parsed);
            return -1;
          }
          target_end = candy_end;
          target_end = resume_from_confirmed_surface(
              ledger, input, &lat, target_end, accepted_text, &accepted, 0);
        }
        free(accepted_text);
      }

      for (;;) {
        size_t ignored_end = ignored_advance(config, input, target_end);

        if (ignored_end <= target_end)
          break;
        target_end = ignored_end;
      }

      if (target_end <= previous_end)
        break;
    }

    {
      RouteRunList runs;
      RouteEvaluation evaluation;

      if (evaluate_selected_route(ledger, &accepted, &evaluation) != 0) {
        selected_route_free(&accepted);
        longest_path_list_free(&paths);
        lattice_free(&lat);
        free_sui_input(&parsed);
        return -1;
      }
      (void)evaluation; /* Candidate comparison will consume this next. */

      route_run_list_init(&runs);
      if (split_selected_route(ledger, &accepted, &runs) != 0) {
        route_run_list_free(&runs);
        selected_route_free(&accepted);
        longest_path_list_free(&paths);
        lattice_free(&lat);
        free_sui_input(&parsed);
        return -1;
      }

      for (size_t r = 0; r < runs.n; r++) {
        const RouteRun *run = &runs.v[r];
        const LatticeEdge *first_edge = &accepted.v[run->first];

        if (first_edge->surface > ledger->null_surface) {
          for (size_t j = 0; j < run->count; j++)
            if (runtime_surface_offset(
                    ledger, accepted.v[run->first + j].surface) == SIZE_MAX) {
              route_run_list_free(&runs);
              selected_route_free(&accepted);
              longest_path_list_free(&paths);
              lattice_free(&lat);
              free_sui_input(&parsed);
              return -1;
            }
        } else {
          const OccurrenceRecord *occurrence;
          const OccurrenceRecord *end =
              ledger->occurrencev + ledger->occurrence->noccurrences;
          int matched = 0;

          for (occurrence = ledger->occurrencev; occurrence < end;
               occurrence++)
            if (ledger_run_matches(ledger, &accepted, run, occurrence)) {
              matched = 1;
              break;
            }

          if (!matched) {
            route_run_list_free(&runs);
            selected_route_free(&accepted);
            longest_path_list_free(&paths);
            lattice_free(&lat);
            free_sui_input(&parsed);
            return -1;
          }
        }
      }

      route_run_list_free(&runs);
    }

    int appended_adjacency = 0;

    if (append_candy) {
      appended_adjacency =
          append_uncovered_adjacencies(ledger, config, &parsed, &lat, &paths,
                                       target_end);
      if (appended_adjacency < 0) {
        longest_path_list_free(&paths);
        lattice_free(&lat);
        free_sui_input(&parsed);
        return -1;
      }
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

        if (append_candy && !appended_adjacency &&
            !candy_has_unresolved(ledger, config, &parsed, target_end,
                                  input_end)) {
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
    } else {
      int emit_status;

      if (selected_route_has_runtime(ledger, &accepted))
        emit_status = emit_mixed_route(ledger, &accepted, target_end, best);
      else
        emit_status = emit_observed_paths(ledger, input, &lat, target_end);

      if (emit_status != 0) {
        selected_route_free(&accepted);
        longest_path_list_free(&paths);
        lattice_free(&lat);
        free_sui_input(&parsed);
        return -1;
      }
    }

    selected_route_free(&accepted);
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

    if (e->surface > ledger->null_surface)
      free((char *)surface);

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
  int best = 0;
  int opt;

  memset(&config, 0, sizeof(config));

  while ((opt = getopt(argc, argv, "abc:muh")) != -1) {
    switch (opt) {
    case 'a':
      append_candy = 1;
      unresolved = 1;
      break;
    case 'b':
      best = 1;
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
      printf("usage: %s [-b] [-c config] [-m | -u | -a] ledger.dat\n", argv[0]);
      return EXIT_SUCCESS;
    default:
      fprintf(stderr, "usage: %s [-b] [-c config] [-m | -u | -a] ledger.dat\n", argv[0]);
      return EXIT_FAILURE;
    }
  }

  if (monitor && unresolved) {
    fprintf(stderr, "usage: %s [-b] [-c config] [-m | -u | -a] ledger.dat\n", argv[0]);
    return EXIT_FAILURE;
  }

  if (optind + 1 != argc) {
    fprintf(stderr, "usage: %s [-b] [-c config] [-m | -u | -a] ledger.dat\n", argv[0]);
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
                    unresolved, append_candy, best) != 0) {
    unload_ledger(&ledger);
    free_mkledger_config(&config);
    return EXIT_FAILURE;
  }

  unload_ledger(&ledger);
  free_mkledger_config(&config);
  return EXIT_SUCCESS;
}
