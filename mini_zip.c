/*
gcc -pthread mini_zip.c -o mz
./mz -c entrada.txt compactado.mzp     # compactar
./mz -d compactado.mzp saida.txt       # descompactar
*/

/*
testar no modo detalhado:
./mz -v -s 5 -c entrada2.txt entrada2.mzp > log.txt
less log.txt

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
#include <sys/syscall.h>
#include <stdarg.h>

// ---------- Modo demonstração ----------
static int verbose   = 0;    // -v: cada thread conta o que está fazendo
static int atraso_us = 0;    // -s ms: pausa artificial por item (só para a demo)
static struct timespec t0;   // instante em que o programa começou

// Imprime "[tempo] [PID x | TID y] PAPEL mensagem" (só no modo -v)
static void logv(const char *papel, const char *fmt, ...){
  if (!verbose) return;
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  double ms = (t.tv_sec - t0.tv_sec) * 1000.0 + (t.tv_nsec - t0.tv_nsec) / 1e6;
  char msg[512];
  va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
  // um único printf por linha: as linhas das threads não se misturam
  printf("[%8.2f ms] [PID %d | TID %ld] %-13s %s\n",
         ms, (int)getpid(), (long)syscall(SYS_gettid), papel, msg);
}

static void pausa_demo(void){ if (atraso_us > 0) usleep((useconds_t)atraso_us); }

#define BASE 0x80   // primeiro código de palavra
#define MARK_CAP 0xFF   // "próxima palavra começa com maiúscula"
#define PREFIXO_LONGO 0xFE  // avisa: o PRÓXIMO byte completa um código de 2 bytes
#define LIMITE_1B (PREFIXO_LONGO - BASE)  // 126 = quantas palavras cabem em 1 byte
#define MAX_PALAVRAS (LIMITE_1B + 256)  // 126 de 1 byte + 256 de 2 bytes = 382
#define MAX_WORD 256

// Dicionário ampliado, ordenado por frequência nos textos de teste
// índices 0..125  -> código de 1 byte  (BASE + índice)
// índices 126..   -> código de 2 bytes (PREFIXO_LONGO, índice - LIMITE_1B)
static const char* WORD[] = {
  // ---- 1 byte (126 palavras) ----
  "e","o","de","compressao","programa","dicionario","do","texto","a","para",
  "com","arquivos","em","palavras","arquivo","quando","que","um","exemplo","como",
  "este","muitas","apenas","assim","foi","nao","palavra","threads","uma","vezes",
  "ainda","aparecem","cada","curtas","da","diferentes","escolhemos","final","frases","grava",
  "mesmo","mostra","muito","no","ocorrencias","original","os","ou","pequeno","pode",
  "por","processo","processos","repeticoes","se","simbolo","simples","util","varias","academicos",
  "ajuda","aluno","ao","aprendizado","aqui","as","ate","atividade","avaliar","bastante",
  "c","caso","combinacoes","compactacao","comprimir","comprova","comum","comuns","concluir","contem",
  "conter","contextos","continua","correspondente","criado","criados","demonstra","descompactado","deve","diferenca",
  "direto","duas","efeito","eficiente","ele","encontra","encontre","entender","escrever","espacos",
  "estatisticas","estimular","estruturas","exatamente","exibe","facil","falamos","fato","faz","fica",
  "filho","foco","foque","formato","frequentes","ganhar","ganhos","gravar","ideia","igual",
  "inclui","isso","juntos","leitura","ler","linha",
  // ---- 2 bytes (55 palavras) ----
  "linhas","logica","longo","maiores","mais","mas","material","maxima","menor","metodo",
  "modo","mostrar","muitos","normal","objetivo","observar","organizado","pai","paragrafos","pensamos",
  "pontuacao","porque","portugues","possivel","possui","preparado","preservar","reduz","relatorio","repete",
  "repetem","repetidas","saida","seja","sejam","semelhantes","ser","sobre","solucao","tamanho",
  "tambem","tantas","taxa","tecnicos","tem","termos","testes","textos","trabalha","trabalham",
  "trabalhar","uso","utiliza","varios","ver"
};

static const int N = sizeof(WORD)/sizeof(WORD[0]);

// 126 códigos de 1 byte + 256 de 2 bytes = 382 palavras no máximo
_Static_assert(sizeof(WORD)/sizeof(WORD[0]) <= MAX_PALAVRAS, "dicionario grande demais");

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

// Devolve o índice da palavra no dicionário (ou -1 se não existir)
// O índice ainda não é o código: quem converte em bytes é gravar_codigo()
static int word_to_index(const char *w) {
  for (int i = 0; i < N; i++) {
    if (!strcmp(WORD[i], w)) return i;
  }
  return -1;
}

// Devolve a palavra de um índice (ou NULL se o índice for inválido)
static const char* index_to_word(int idx) {
  if (idx >= 0 && idx < N) return WORD[idx];
  return NULL;
}

// Escreve em 'dst' o código do índice: 1 byte (índices 0..125) ou 2 bytes
// (PREFIXO_LONGO + índice - LIMITE_1B). Devolve quantos bytes escreveu.
static int gravar_codigo(unsigned char *dst, int idx) {
  if (idx < LIMITE_1B) {
    dst[0] = (unsigned char)(BASE + idx);
    return 1;
  }
  dst[0] = PREFIXO_LONGO;
  dst[1] = (unsigned char)(idx - LIMITE_1B);
  return 2;
}

static void compactar_token(const Token *t, Saida *s, Shm *sh){
  s->n = 0;
  if (!t->eh_palavra){ s->dados[s->n++] = (unsigned char)t->sep; return; }

  const char *w = t->texto;
  char lw[MAX_WORD];
  size_t len = strlen(w);
  for (size_t i = 0; i <= len; i++) lw[i] = (char)tolower((unsigned char)w[i]);

  int idx = word_to_index(lw);
  if (idx < 0) stats_add(sh, &sh->st.palavras_nao_encontradas, 1);
  else if (strcmp(w, lw) == 0){                       // toda minúscula
    s->n += gravar_codigo(s->dados + s->n, idx);
    stats_add(sh, &sh->st.palavras_compactadas, 1);
    return;
  } else if (isupper((unsigned char)w[0]) && strcmp(w+1, lw+1) == 0) {
    s->dados[s->n++] = MARK_CAP;                      // 1ª letra maiúscula
    s->n += gravar_codigo(s->dados + s->n, idx);       // vale também para 1 letra, ex.: "O"
    stats_add(sh, &sh->st.palavras_compactadas, 1);
    return;
  } else {
    // está no dicionário, mas com maiúsculas que não sabemos representar
    // (ex.: "DE"): vai como está e conta como não compactada
    stats_add(sh, &sh->st.palavras_nao_encontradas, 1);
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
  logv("LEITORA", "iniciou");
  while (ler_token(ctx->in, &t, ctx->sh)){
    if (t.eh_palavra) logv("LEITORA", "leu \"%s\"", t.texto);
    buffer_put(&ctx->buf1, &t);
    pausa_demo();
  }
  logv("LEITORA", "fim do arquivo");
  buffer_close(&ctx->buf1);                 // fim do arquivo: avisa a compactadora
  return NULL;
}

static void *thread_compactadora(void *arg){
  Contexto *ctx = arg;
  Token t; Saida s;
  logv("COMPACTADORA", "iniciou");
  while (buffer_get(&ctx->buf1, &t)){
    compactar_token(&t, &s, ctx->sh);
    if (t.eh_palavra) logv("COMPACTADORA", "\"%s\" -> %d byte(s)", t.texto, s.n);
    buffer_put(&ctx->buf2, &s);
    pausa_demo();
  }
  logv("COMPACTADORA", "terminou");
  buffer_close(&ctx->buf2);                 // repassa o fim: avisa a gravadora
  return NULL;
}

static void *thread_gravadora(void *arg){
  Contexto *ctx = arg;
  Saida s;
  logv("GRAVADORA", "iniciou");
  while (buffer_get(&ctx->buf2, &s)){
    gravar_saida(ctx->out, &s, ctx->sh);
    if (s.n > 1 || s.dados[0] >= BASE) logv("GRAVADORA", "gravou %d byte(s)", s.n);
    pausa_demo();
  }
  logv("GRAVADORA", "terminou");
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

// Lê um código de palavra cujo 1º byte 'c' já foi lido e devolve o ÍNDICE no dicionário
// Se 'c' for o prefixo longo, lê (e conta) o 2º byte
// Devolve -1 se 'c' não é código ou se o arquivo terminou no meio do código
static int ler_indice(FILE *in, int c, Shm *sh) {
  if (c < BASE || c == MARK_CAP) return -1;
  if (c == PREFIXO_LONGO) {
    int d = fgetc(in);            // 2º byte: é só um número, não interpretar
    if (d == EOF) return -1;      // arquivo truncado
    stats_add(sh, &sh->st.bytes_compactados, 1);
    return LIMITE_1B + d;
  }
  return c - BASE;
}

static void decompress(FILE *in, FILE *out, Shm *sh){
  int c;
  while ((c = fgetc(in)) != EOF){
    stats_add(sh, &sh->st.bytes_compactados, 1);
    if (c == MARK_CAP){                       // próxima palavra começa com maiúscula
      int d = fgetc(in);
      if (d == EOF) break;                    // arquivo truncado
      stats_add(sh, &sh->st.bytes_compactados, 1);
      int idx = ler_indice(in, d, sh);
      if (idx < 0) break;                     // truncado ou inválido
      const char *w = index_to_word(idx);
      if (w){
        fputc(toupper((unsigned char)w[0]), out);
        fputs(w + 1, out);
        logv("DESCOMPACTADOR", "restaurou \"%c%s\"", toupper((unsigned char)w[0]), w + 1);
        pausa_demo();
        stats_add(sh, &sh->st.palavras_compactadas, 1);
        stats_add(sh, &sh->st.bytes_originais, (long)strlen(w));
      }
      continue;
    }
    if (c >= BASE) {                          // código de palavra (1 ou 2 bytes)
      int idx = ler_indice(in, c, sh);
      if (idx < 0) break;                     // truncado
      const char *w = index_to_word(idx);
      if (w) {
        fputs(w, out);
        logv("DESCOMPACTADOR", "restaurou \"%s\"", w);
        pausa_demo();
        stats_add(sh, &sh->st.palavras_compactadas, 1);
        stats_add(sh, &sh->st.bytes_originais, (long)strlen(w));
      }
    } else {                                  // caractere normal: copia
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
  clock_gettime(CLOCK_MONOTONIC, &t0);
  setvbuf(stdout, NULL, _IOLBF, 0);   // linha a linha, mesmo redirecionado p/ arquivo

  int compactar = -1, opt;
  while ((opt = getopt(argc, argv, "cdvs:")) != -1){
    switch (opt){
      case 'c': compactar = 1; break;
      case 'd': compactar = 0; break;
      case 'v': verbose = 1; break;
      case 's': atraso_us = atoi(optarg) * 1000; break;   // ms -> us
      default:
        fprintf(stderr, "uso: %s [-v] [-s ms] -c|-d entrada saida\n", argv[0]);
        return 1;
    }
  }
  if (compactar < 0 || argc - optind != 2){
    fprintf(stderr, "uso: %s [-v] [-s ms] -c|-d entrada saida\n", argv[0]);
    return 1;
  }
  const char *entrada = argv[optind], *saida = argv[optind + 1];

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
  fflush(stdout);                      // evita que o filho herde texto ainda no buffer
  pid_t pid = fork();
  if (pid < 0){ perror("fork"); munmap(sh, sizeof(Shm)); shm_unlink(nome); return 1; }
  if (pid == 0){
    printf("[%s] processo filho iniciado: PID %d (pai %d)\n",
           compactar ? "compactador" : "descompactador", (int)getpid(), (int)getppid());
    return executar_trabalho(compactar, entrada, saida, sh);
  }
  printf("[monitor] processo pai: PID %d | filho %s: PID %d\n",
         (int)getpid(), compactar ? "Compactador" : "Descompactador", (int)pid);

  int rc = executar_monitor(sh, pid, compactar);

  // 4) Limpeza
  pthread_mutex_destroy(&sh->mutex);
  munmap(sh, sizeof(Shm));
  shm_unlink(nome);
  return rc;
}