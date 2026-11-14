/*
gcc mini_zip.c -o mz
./mz -c entrada.txt compactado.mzp     # compactar
./mz -d compactado.mzp saida.txt       # descompactar
*/
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#define BASE 0x80   // primeiro código de palavra
#define MARK_CAP 0xFF   // "próxima palavra começa com maiúscula"
#define MAX_WORD 256

static const char* WORD[] =
  {"de","que","para","com","nao","por","em","uma","quando","muito","o","as",
   "ao","mais","como","se","e","foi","tem","tambem","programa","processos","threads","arquivo",
   "arquivos","texto","dicionario","palavras","compressao","exemplo","objetivo","leitura",
   "gravar","relatorio","entrada","saida","aluno","testes","atividade"};

static const int N = sizeof(WORD)/sizeof(WORD[0]);

 // 0x80..0xFE = 127 códigos (0xFF é o marcador)
_Static_assert(sizeof(WORD)/sizeof(WORD[0]) <= 127, "dicionario grande demais para 1 byte");

typedef struct {                 // só números: depois vai para memória compartilhada
  long palavras_lidas;
  long palavras_compactadas;
  long palavras_nao_encontradas;
  long bytes_originais;
  long bytes_compactados;
} Stats;

typedef struct {                 // o que a LEITORA produz
  int  eh_palavra;               // 1 = palavra, 0 = separador
  char texto[MAX_WORD];          // a palavra (se eh_palavra)
  int  sep;                      // o caractere (se separador)
} Token;

typedef struct {                 // o que a COMPACTADORA produz
  unsigned char dados[MAX_WORD + 2];
  int n;                         // quantos bytes são válidos
} Saida;

static int word_to_code(const char *w){          
  for (int i = 0; i < N; i++)
    if (!strcmp(WORD[i], w)) return BASE + i;
  return -1;
}

static const char* code_to_word(int c){             
  if (c >= BASE && c < BASE + N) return WORD[c - BASE];
  return NULL;
}

static void compactar_token(const Token *t, Saida *s, Stats *st){
  s->n = 0;
  if (!t->eh_palavra){ s->dados[s->n++] = (unsigned char)t->sep; return; }

  const char *w = t->texto;
  char lw[MAX_WORD];
  size_t len = strlen(w);
  for (size_t i = 0; i <= len; i++) lw[i] = (char)tolower((unsigned char)w[i]);

  int code = word_to_code(lw);
  if (code < 0) st->palavras_nao_encontradas++;
  else if (strcmp(w, lw) == 0){                       // toda minúscula
    s->dados[s->n++] = (unsigned char)code;
    st->palavras_compactadas++;
    return;
  } else if (len > 1 && isupper((unsigned char)w[0]) && strcmp(w+1, lw+1) == 0){
    s->dados[s->n++] = MARK_CAP;                      // 1ª letra maiúscula
    s->dados[s->n++] = (unsigned char)code;
    st->palavras_compactadas++;
    return;
  }
  memcpy(s->dados, w, len);                           // grava a palavra como está
  s->n = (int)len;
}

static void gravar_saida(FILE *out, const Saida *s, Stats *st){
  fwrite(s->dados, 1, (size_t)s->n, out);
  st->bytes_compactados += s->n;
}

static int ler_token(FILE *in, Token *t, Stats *st){
  int c = fgetc(in);
  if (c == EOF) return 0;

  if (isalnum(c) || c == '_'){
    int n = 0;
    while (c != EOF && (isalnum(c) || c == '_') && n < MAX_WORD - 1){
      t->texto[n++] = (char)c;
      c = fgetc(in);
    }
    if (c != EOF) ungetc(c, in);     // devolve o separador
    t->texto[n] = '\0';
    t->eh_palavra = 1;
    st->palavras_lidas++;
    st->bytes_originais += n;
  } else {
    t->eh_palavra = 0;
    t->sep = c;
    st->bytes_originais++;
  }
  return 1;
}

static void imprimir_stats(const Stats *st){
  double pct = st->bytes_originais
    ? 100.0 * (1.0 - (double)st->bytes_compactados / st->bytes_originais) : 0.0;
  printf("palavras lidas ........ %ld\n", st->palavras_lidas);
  printf("palavras compactadas .. %ld\n", st->palavras_compactadas);
  printf("nao encontradas ....... %ld\n", st->palavras_nao_encontradas);
  printf("tamanho original ...... %ld bytes\n", st->bytes_originais);
  printf("tamanho compactado .... %ld bytes\n", st->bytes_compactados);
  printf("compressao ............ %.1f%%\n", pct);
}

static void compress(FILE *in, FILE *out, Stats *st){
  Token t; Saida s;
  while (ler_token(in, &t, st)){
    compactar_token(&t, &s, st);
    gravar_saida(out, &s, st);
  }
}

static void decompress(FILE *in, FILE *out){
  int c;
  while ((c = fgetc(in)) != EOF){
    if (c == MARK_CAP){                       // próxima palavra começa com maiúscula
      int d = fgetc(in);
      const char *w = (d == EOF) ? NULL : code_to_word(d);
      if (w){
        fputc(toupper((unsigned char)w[0]), out);
        fputs(w + 1, out);
      }
      continue;
    }
    const char *w = code_to_word(c);
    if (w) fputs(w, out); else fputc(c, out);
  }
}

int main(int argc, char **argv){
  if (argc != 4){
    fprintf(stderr, "uso: %s -c|-d entrada saida\n", argv[0]);
    return 1;
  }

  int compactar;
  if      (!strcmp(argv[1], "-c")) compactar = 1;
  else if (!strcmp(argv[1], "-d")) compactar = 0;
  else { fprintf(stderr, "modo invalido: %s\n", argv[1]); return 1; }

  FILE *in = fopen(argv[2], "rb");
  if (!in){ perror(argv[2]); return 1; }

  FILE *out = fopen(argv[3], "wb");
  if (!out){ perror(argv[3]); fclose(in); return 1; }

  Stats st = {0};
  if (compactar){ compress(in, out, &st); imprimir_stats(&st); }
  else          decompress(in, out);

  fclose(in);
  if (fclose(out) != 0){ perror("fechar saida"); return 1; }
  return 0;
}