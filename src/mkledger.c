#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_SIZE 4096
#define NFIELDS 5

int main(int argc, char *argv[]) {
  FILE *fp;
  char line[LINE_SIZE];
  size_t records = 0;
  size_t sequences = 0;
  char prev_id[256] = "";
  size_t pairs = 0;
  char prev_word[LINE_SIZE] = "";

  if (argc != 2) {
    fprintf(stderr, "usage: %s ledger.in\n", argv[0]);
    return EXIT_FAILURE;
  }

  fp = fopen(argv[1], "r");
  if (fp == NULL) {
    perror(argv[1]);
    return EXIT_FAILURE;
  }

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
      fprintf(stderr, "invalid record at line %zu\n", records + 1);
      fclose(fp);
      return EXIT_FAILURE;
    }

    if (strcmp(field[0], prev_id) != 0) {
      sequences++;

      /* previous sequence -> EOS */
      if (records > 0) {
        pairs++;

        if (pairs <= 10) {
          printf("pair: %s + EOS -> %sEOS\n", prev_word, prev_word);
        }
      }

      /* BOS -> first word of new sequence */
      pairs++;

      if (pairs <= 10) {
        printf("pair: BOS + %s -> BOS%s\n", field[2], field[2]);
      }

      if (strlen(field[0]) >= sizeof prev_id) {
        fprintf(stderr, "id too long at line %zu\n", records + 1);
        fclose(fp);
        return EXIT_FAILURE;
      }

      strcpy(prev_id, field[0]);

    } else {
      pairs++;

      if (pairs <= 10) {
        printf("pair: %s + %s -> %s%s\n", prev_word, field[2], prev_word,
               field[2]);
      }
    }

    if (strlen(field[2]) >= sizeof prev_word) {
      fprintf(stderr, "word too long at line %zu\n", records + 1);
      fclose(fp);
      return EXIT_FAILURE;
    }

    strcpy(prev_word, field[2]);

    records++;
  }

  /* EOS for the final sequence */
  if (records > 0) {
    pairs++;
  }

  if (ferror(fp)) {
    perror(argv[1]);
    fclose(fp);
    return EXIT_FAILURE;
  }

  fclose(fp);

  printf("records:   %zu\n", records);
  printf("sequences: %zu\n", sequences);
  printf("pairs:     %zu\n", pairs);

  return EXIT_SUCCESS;
}
