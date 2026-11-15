/*
gcc mini_zip.c -o mz
./mz -c entrada.txt compactado.mzp     # compactar
./mz -d compactado.mzp saida.txt       # descompactar
*/
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>

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

// O que fica na memória compartilhada entre processos (compactadora e gravadora)
typedef struct {
  pthread_mutex_t mutex;         // protege 'st' (funciona entre processos)
  Stats st;
} Shm;

// Soma 'v' a um contador, com o mutex travado
static void stats_add(Shm *sh, long *campo, long v){
  pthread_mutex_lock(&sh->mutex);
  *campo += v;
  pthread_mutex_unlock(&sh->mutex);
}

typedef struct {                 // o que a LEITORA produz
  int  eh_palavra;               // 1 = palavra, 0 = separador
  char texto[MAX_WORD];          // a palavra (se eh_palavra)
  int  sep;                      // o caractere (se separador)
} Token;

typedef struct {                 // o que a COMPACTADORA produz
  unsigned char dados[MAX_WORD + 2];
  int n;                         // quantos bytes são válidos
} Saida;

typedef struct {
  unsigned char *dados;      // área: capacidade * tam_item bytes
  size_t tam_item;           // tamanho de cada item
  int capacidade;            // quantos itens cabem
  int ini;                   // posição do próximo item a retirar
  int fim;                   // posição onde o próximo item será inserido
  int qtd;                   // quantos itens há agora
  int fechado;               // 1 = o produtor terminou
  pthread_mutex_t mutex;
  pthread_cond_t  nao_cheio;
  pthread_cond_t  nao_vazio;
} Buffer;

static int buffer_init(Buffer *b, int capacidade, size_t tam_item){
  b->dados = malloc((size_t)capacidade * tam_item);
  if (!b->dados) return -1;
  b->tam_item = tam_item; b->capacidade = capacidade;
  b->ini = b->fim = b->qtd = b->fechado = 0;
  pthread_mutex_init(&b->mutex, NULL);
  pthread_cond_init(&b->nao_cheio, NULL);
  pthread_cond_init(&b->nao_vazio, NULL);
  return 0;
}

static void buffer_destroy(Buffer *b){
  pthread_mutex_destroy(&b->mutex);
  pthread_cond_destroy(&b->nao_cheio);
  pthread_cond_destroy(&b->nao_vazio);
  free(b->dados);
}

// Produtor: coloca um item. Espera se estiver cheio.
static void buffer_put(Buffer *b, const void *item){
  pthread_mutex_lock(&b->mutex);
  while (b->qtd == b->capacidade)
    pthread_cond_wait(&b->nao_cheio, &b->mutex);
  memcpy(b->dados + (size_t)b->fim * b->tam_item, item, b->tam_item);
  b->fim = (b->fim + 1) % b->capacidade;
  b->qtd++;
  pthread_cond_signal(&b->nao_vazio);
  pthread_mutex_unlock(&b->mutex);
}

// Consumidor: retira um item. Devolve 0 se acabou (fechado e vazio).
static int buffer_get(Buffer *b, void *item){
  pthread_mutex_lock(&b->mutex);
  while (b->qtd == 0 && !b->fechado)
    pthread_cond_wait(&b->nao_vazio, &b->mutex);
  if (b->qtd == 0){ pthread_mutex_unlock(&b->mutex); return 0; }
  memcpy(item, b->dados + (size_t)b->ini * b->tam_item, b->tam_item);
  b->ini = (b->ini + 1) % b->capacidade;
  b->qtd--;
  pthread_cond_signal(&b->nao_cheio);
  pthread_mutex_unlock(&b->mutex);
  return 1;
}

// Produtor avisa que não vai mais inserir nada.
static void buffer_close(Buffer *b){
  pthread_mutex_lock(&b->mutex);
  b->fechado = 1;
  pthread_cond_broadcast(&b->nao_vazio);   // acorda consumidor que esteja esperando
  pthread_mutex_unlock(&b->mutex);
}

static int word_to_code(const char *w){          
  for (int i = 0; i < N; i++)
    if (!strcmp(WORD[i], w)) return BASE + i;
  return -1;
}

static const char* code_to_word(int c){             
  if (c >= BASE && c < BASE + N) return WORD[c - BASE];
  return NULL;
}

static void compactar_token(const Token *t, Saida *s, Shm *sh){
  s->n = 0;
  if (!t->eh_palavra){ s->dados[s->n++] = (unsigned char)t->sep; return; }

  const char *w = t->texto;
  char lw[MAX_WORD];
  size_t len = strlen(w);
  for (size_t i = 0; i <= len; i++) lw[i] = (char)tolower((unsigned char)w[i]);

  int code = word_to_code(lw);
  if (code < 0) stats_add(sh, &sh->st.palavras_nao_encontradas, 1);
  else if (strcmp(w, lw) == 0){                       // toda minúscula
    s->dados[s->n++] = (unsigned char)code;
    stats_add(sh, &sh->st.palavras_compactadas, 1);
    return;
  } else if (len > 1 && isupper((unsigned char)w[0]) && strcmp(w+1, lw+1) == 0){
    s->dados[s->n++] = MARK_CAP;                      // 1ª letra maiúscula
    s->dados[s->n++] = (unsigned char)code;
    stats_add(sh, &sh->st.palavras_compactadas, 1);
    return;
  }
  memcpy(s->dados, w, len);                           // grava a palavra como está
  s->n = (int)len;
}

static void gravar_saida(FILE *out, const Saida *s, Shm *sh){
  fwrite(s->dados, 1, (size_t)s->n, out);
  stats_add(sh, &sh->st.bytes_compactados, s->n);
}

static int ler_token(FILE *in, Token *t, Shm *sh){
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
    stats_add(sh, &sh->st.palavras_lidas, 1);
    stats_add(sh, &sh->st.bytes_originais, n);
  } else {
    t->eh_palavra = 0;
    t->sep = c;
    stats_add(sh, &sh->st.bytes_originais, 1);
  }
  return 1;
}

// Tudo que as três threads compartilham
typedef struct {
  FILE *in, *out;
  Shm *sh;
  Buffer buf1;                   // leitora -> compactadora (Token)
  Buffer buf2;                   // compactadora -> gravadora (Saida)
} Contexto;

static void *thread_leitora(void *arg){
  Contexto *ctx = arg;
  Token t;
  while (ler_token(ctx->in, &t, ctx->sh)) buffer_put(&ctx->buf1, &t);
  buffer_close(&ctx->buf1);                 // fim do arquivo: avisa a compactadora
  return NULL;
}

static void *thread_compactadora(void *arg){
  Contexto *ctx = arg;
  Token t; Saida s;
  while (buffer_get(&ctx->buf1, &t)){
    compactar_token(&t, &s, ctx->sh);
    buffer_put(&ctx->buf2, &s);
  }
  buffer_close(&ctx->buf2);                 // repassa o fim: avisa a gravadora
  return NULL;
}

static void *thread_gravadora(void *arg){
  Contexto *ctx = arg;
  Saida s;
  while (buffer_get(&ctx->buf2, &s)) gravar_saida(ctx->out, &s, ctx->sh);
  return NULL;
}

static void compress(FILE *in, FILE *out, Shm *sh){
  Contexto ctx = { .in = in, .out = out, .sh = sh };
  buffer_init(&ctx.buf1, 8, sizeof(Token));
  buffer_init(&ctx.buf2, 8, sizeof(Saida));

  pthread_t tl, tc, tg;
  pthread_create(&tl, NULL, thread_leitora,      &ctx);
  pthread_create(&tc, NULL, thread_compactadora, &ctx);
  pthread_create(&tg, NULL, thread_gravadora,    &ctx);

  pthread_join(tl, NULL);
  pthread_join(tc, NULL);
  pthread_join(tg, NULL);

  buffer_destroy(&ctx.buf1);
  buffer_destroy(&ctx.buf2);
}

static void decompress(FILE *in, FILE *out, Shm *sh){
  int c;
  while ((c = fgetc(in)) != EOF){
    stats_add(sh, &sh->st.bytes_compactados, 1);
    if (c == MARK_CAP){                       // próxima palavra começa com maiúscula
      int d = fgetc(in);
      if (d == EOF) break;                    // arquivo truncado
      stats_add(sh, &sh->st.bytes_compactados, 1);
      const char *w = code_to_word(d);
      if (w){
        fputc(toupper((unsigned char)w[0]), out);
        fputs(w + 1, out);
        stats_add(sh, &sh->st.palavras_compactadas, 1);
        stats_add(sh, &sh->st.bytes_originais, (long)strlen(w));
      }
      continue;
    }
    const char *w = code_to_word(c);
    if (w){
      fputs(w, out);
      stats_add(sh, &sh->st.palavras_compactadas, 1);
      stats_add(sh, &sh->st.bytes_originais, (long)strlen(w));
    } else {
      fputc(c, out);
      stats_add(sh, &sh->st.bytes_originais, 1);
    }
  }
}

// ---------- Processo Compactador / Descompactador (filho) ----------
static int executar_trabalho(int compactar, const char *ent, const char *sai, Shm *sh){
  FILE *in = fopen(ent, "rb");
  if (!in){ perror(ent); return 1; }
  FILE *out = fopen(sai, "wb");
  if (!out){ perror(sai); fclose(in); return 1; }

  if (compactar) compress(in, out, sh);
  else           decompress(in, out, sh);

  fclose(in);
  if (fclose(out) != 0){ perror("fechar saida"); return 1; }
  return 0;
}

// ---------- Processo Monitor (pai) ----------
static void exibir(const Stats *st, int compactar, int final){
  double pct = st->bytes_originais
    ? 100.0 * (1.0 - (double)st->bytes_compactados / st->bytes_originais) : 0.0;
  if (!final){
    printf("[monitor] lidas=%ld compactadas=%ld nao_encontradas=%ld orig=%ld comp=%ld\n",
           st->palavras_lidas, st->palavras_compactadas, st->palavras_nao_encontradas,
           st->bytes_originais, st->bytes_compactados);
    return;
  }
  printf("\n===== Resumo (%s) =====\n", compactar ? "compactacao" : "descompactacao");
  if (compactar){
    printf("palavras lidas ........ %ld\n", st->palavras_lidas);
    printf("nao encontradas ....... %ld\n", st->palavras_nao_encontradas);
  }
  printf("palavras compactadas .. %ld\n", st->palavras_compactadas);
  printf("tamanho original ...... %ld bytes\n", st->bytes_originais);
  printf("tamanho compactado .... %ld bytes\n", st->bytes_compactados);
  printf("compressao ............ %.1f%%\n", pct);
}

static Stats ler_stats(Shm *sh){             // cópia consistente dos contadores
  Stats snap;
  pthread_mutex_lock(&sh->mutex);
  snap = sh->st;
  pthread_mutex_unlock(&sh->mutex);
  return snap;
}

static int executar_monitor(Shm *sh, pid_t filho, int compactar){
  int status = 0;
  struct timespec pausa = {0, 100 * 1000 * 1000};     // 100 ms
  for (;;){
    Stats snap = ler_stats(sh);
    exibir(&snap, compactar, 0);
    if (waitpid(filho, &status, WNOHANG) == filho) break;   // filho terminou
    nanosleep(&pausa, NULL);
  }
  Stats fim = ler_stats(sh);                           // valores finais, já exatos
  exibir(&fim, compactar, 1);
  return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : 1;
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

    // 1) Memória compartilhada
  char nome[64];
  snprintf(nome, sizeof nome, "/mini_zip_%d", (int)getpid());
  int fd = shm_open(nome, O_CREAT | O_RDWR, 0600);
  if (fd < 0){ perror("shm_open"); return 1; }
  if (ftruncate(fd, sizeof(Shm)) < 0){ perror("ftruncate"); shm_unlink(nome); return 1; }
  Shm *sh = mmap(NULL, sizeof(Shm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (sh == MAP_FAILED){ perror("mmap"); shm_unlink(nome); return 1; }
  close(fd);

  // 2) Mutex que vale entre processos
  pthread_mutexattr_t at;
  pthread_mutexattr_init(&at);
  pthread_mutexattr_setpshared(&at, PTHREAD_PROCESS_SHARED);
  pthread_mutex_init(&sh->mutex, &at);
  pthread_mutexattr_destroy(&at);
  memset(&sh->st, 0, sizeof sh->st);

  // 3) fork: filho = Compactador/Descompactador, pai = Monitor
  pid_t pid = fork();
  if (pid < 0){ perror("fork"); munmap(sh, sizeof(Shm)); shm_unlink(nome); return 1; }
  if (pid == 0) return executar_trabalho(compactar, argv[2], argv[3], sh);

  int rc = executar_monitor(sh, pid, compactar);

  // 4) Limpeza
  pthread_mutex_destroy(&sh->mutex);
  munmap(sh, sizeof(Shm));
  shm_unlink(nome);
  return rc;
}