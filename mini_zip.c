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

static const char* WORD[] =
  {"de","que","para","com","nao","por","em","uma","quando","muito","o","as",
   "ao","mais","como","se","e","foi","tem","tambem","programa","processos","threads","arquivo",
   "arquivos","texto","dicionario","palavras","compressao","exemplo","objetivo","leitura",
   "gravar","relatorio","entrada","saida","aluno","testes","atividade"};

static const int N = sizeof(WORD)/sizeof(WORD[0]);

 // 0x80..0xFE = 127 códigos (0xFF é o marcador)
_Static_assert(sizeof(WORD)/sizeof(WORD[0]) <= 127, "dicionario grande demais para 1 byte");

static int word_to_code(const char *w){          
  for (int i = 0; i < N; i++)
    if (!strcmp(WORD[i], w)) return BASE + i;
  return -1;
}

static const char* code_to_word(int c){             
  if (c >= BASE && c < BASE + N) return WORD[c - BASE];
  return NULL;
}

static void emit_word(const char *w, FILE *out){
  char lw[256];
  size_t n = strlen(w);
  for (size_t i = 0; i <= n; i++)            // cópia em minúsculas (inclui o '\0')
    lw[i] = (char)tolower((unsigned char)w[i]);

  int code = word_to_code(lw);               // busca sempre pela versão minúscula
  if (code >= 0){
    if (strcmp(w, lw) == 0){                 // toda minúscula
      fputc(code, out); return;
    }
    if (n > 1 && isupper((unsigned char)w[0]) && strcmp(w+1, lw+1) == 0){
      fputc(MARK_CAP, out);                  // só a 1ª letra maiúscula
      fputc(code, out); return;
    }
  }
  fputs(w, out);                             // fora do dicionário ou caixa mista
}


static void compress(FILE *in, FILE *out){
  int c;
  int wl = 0;
  char w[256];
  while ((c = fgetc(in)) != EOF){
    if ((isalnum(c) || c == '_') && wl < 255) w[wl++] = (char)c;   // mantém a caixa
    else {
      if (wl){ w[wl] = 0; wl = 0; emit_word(w, out); }
      fputc(c, out);
    }
  }
  if (wl){ w[wl] = 0; emit_word(w, out); }
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
  if (argc != 4) { printf("uso: %s -c|-d entrada.txt saida.txt\n", argv[0]); return 1; }
  FILE *in = fopen(argv[2],"rb");                   // abre arquivo de entrada
  FILE *out = fopen(argv[3],"wb");                  // abre arquivo de saída
  if (argv[1][1]=='c') compress(in,out);            // modo compactar
  else decompress(in,out);                          // modo descompactar
  fclose(in); fclose(out);
  return 0;
}