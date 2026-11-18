# Compactador Paralelo com Processos e Threads

Trabalho do Grau B - compactador e descompactador de texto em **C (Linux)** que combina 
**processos**, **threads**, **memória compartilhada** e **sincronização**.

O programa troca as palavras de um dicionário por códigos de 1 ou 2 bytes 
(compactação) e faz o caminho inverso (descompactação), enquanto um processo Monitor 
acompanha a execução.

## Integrantes

- Luiza Klein
- João Pedro Büttenbender da Silva

## Requisitos

- Linux (testado no Ubuntu, em uma VM VirtualBox)
- `gcc` e as bibliotecas básicas de C: `sudo apt install build-essential`

O código usa `fork`, `shm_open` e `mmap`, que não existem no Windows. Para 
compilar e rodar no Windows é necessário usar WSL ou uma máquina virtual Linux.

## Arquivos

| Arquivo | Descrição |
|---|---|
| `mini_zip.c` | Código-fonte completo (compactador, descompactador e monitor) |
| `entrada.txt` | Texto de teste 1 |
| `entrada2.txt` | Texto de teste 2 |
| `Compactador-v2.pdf` | PDF disponibilizado pela professora |

## Compilação

```bash
gcc-Wall -Wextra -Wpedantic -g -pthread mini_zip.c -o mz
```

Compila sem nenhum aviso.

## Uso

```bash
./mz -c entrada.txt compactado.mzp     # compactar
./mz -d compactado.mzp saida.txt       # descompactar
```

Opções:

| Opção | Efeito |
|---|---|
| `-c` | Modo compactar (processos Compactador + Monitor) |
| `-d` | Modo descompactar (processos Descompactador + Monitor) |
| `-v` | Modo detalhado: cada thread imprime o que está fazendo, com PID, TID e tempo |
| `-s ms` | Pausa artificial de `ms` milissegundos por item, **só para demonstração** do paralelismo |
 
Sem `-v` e `-s`, o programa roda na velocidade normal. Os percentuais de compressão
da seção de resultados foram medidos sem essas opções.
 
Uma opção inválida ou argumentos faltando mostram a mensagem de uso e encerram o programa:
 
```
uso: ./mz [-v] [-s ms] -c|-d entrada saida
```

## Como funciona
 
### Processos
 
```
                 fork()
  ┌──────────────────┴──────────────────┐
  │                                     │
Processo PAI                        Processo FILHO
MONITOR                             COMPACTADOR (-c) ou DESCOMPACTADOR (-d)
  │                                     │
  │  lê os contadores a cada 100 ms     │  atualiza os contadores
  └──────────── memória compartilhada ──┘
                (shm_open + mmap + mutex PROCESS_SHARED)
```
 
- O **Monitor** (pai) imprime periodicamente as palavras lidas, compactadas, não
  encontradas e os tamanhos. Quando o filho termina (`waitpid`), lê os valores finais e
  imprime o resumo, com o percentual de compressão.
- O **Compactador** ou **Descompactador** (filho) faz o trabalho de fato.
- Os dois processos se comunicam por uma região de **memória compartilhada**
  (`shm_open`, `ftruncate`, `mmap`) que guarda os contadores. Um **mutex com
  `PTHREAD_PROCESS_SHARED`** protege esses contadores entre processos.
### Threads do compactador
 
No modo `-c`, o processo filho cria três threads que trabalham em pipeline:
 
```
LEITORA ──[buffer 1]──▶ COMPACTADORA ──[buffer 2]──▶ GRAVADORA
 (lê o        Token        (transforma em      Saida     (grava em
 arquivo)                   bytes)                        disco)
```
 
- Os buffers são **circulares**, com o padrão produtor/consumidor.
- A sincronização usa **mutex + variáveis de condição** (`nao_cheio` e `nao_vazio`).
- O `cond_wait` fica dentro de um `while`, para tratar despertares falsos.
- Quando a leitora termina, `buffer_close` avisa a thread seguinte (com `broadcast`), que
  repassa o aviso adiante. Assim nenhuma thread fica esperando para sempre.
## Formato da compactação
 
Cada palavra do dicionário vira um **índice** (a posição dela no array `WORD[]`), e o
índice vira bytes. Como o texto original é ASCII (bytes abaixo de 0x80), os códigos usam
apenas bytes a partir de 0x80, e o descompactador distingue código de caractere normal.
 
| Situação | O que é gravado |
|---|---|
| Palavra com índice 0 a 125 | 1 byte: `0x80 + índice` |
| Palavra com índice 126 em diante | 2 bytes: `0xFE` e depois `índice - 126` |
| Palavra com inicial maiúscula | `0xFF` antes do código (1 ou 2 bytes) |
| Palavra fora do dicionário (ou em caixa irregular, como `DE`) | A palavra como está, e conta como "não encontrada" |
| Espaços, pontuação e quebras de linha | Gravados como estão |
 
Exemplos reais, no começo do `entrada.txt` ("este material de exemplo..."): o `xxd` do
arquivo compactado começa com `94 20 fe 06 20 82 20 92`, em que `94` é "este", `20` é o
espaço, `fe 06` é "material" (2 bytes), `82` é "de" e `92` é "exemplo".
 
### Dicionário ampliado (desafio)
 
O dicionário original tinha 39 palavras e só comportava 127 códigos de 1 byte. Os dois
textos de teste têm **181 palavras distintas**, então o formato passou a ter códigos de
2 bytes:
 
- 126 palavras mais frequentes: código de 1 byte;
- 55 palavras restantes: código de 2 bytes;
- capacidade total: 126 + 256 = **382 palavras**.
O array `WORD[]` está ordenado por frequência nos textos de teste, para que as palavras
mais usadas fiquem com 1 byte. A ordem do array é o próprio código: o compactador e o
descompactador usam o mesmo array.
 
Para conferir a quantidade de palavras distintas dos textos:
 
```bash
cat entrada.txt entrada2.txt | tr 'A-Z' 'a-z' | tr -c 'a-z0-9_' '\n' | grep -v '^$' | sort -u | wc -l
```
 
## Testes
 
```bash
# Teste 1
./mz -c entrada.txt comp1.mzp
./mz -d comp1.mzp saida1.txt
cmp entrada.txt saida1.txt && echo "IDENTICO"
 
# Teste 2
./mz -c entrada2.txt comp2.mzp
./mz -d comp2.mzp saida2.txt
cmp entrada2.txt saida2.txt && echo "IDENTICO"
```
 
### Resultados
 
| | Palavras | Não encontradas | Original | Compactado | Compressão |
|---|---|---|---|---|---|
| `entrada.txt` | 315 | 0 | 1921 bytes | 722 bytes | 62,4% |
| `entrada2.txt` | 139 | 0 | 886 bytes | 307 bytes | 65,3% |
 
Antes do dicionário ampliado, as "não encontradas" eram 170 (texto 1) e 39 (texto 2),
com compressão de 22,0% e 49,5%. Os tamanhos podem variar em 1 byte conforme o arquivo
termine ou não com uma quebra de linha.
 
## Demonstração do paralelismo
 
Os textos de teste terminam em milissegundos. Para ver a execução paralela, use um arquivo
maior e o modo detalhado.
 
**1. Monitor e processos trabalhando ao mesmo tempo (arquivo grande):**
 
```bash
for i in $(seq 1 3000); do cat entrada.txt; done > grande.txt
./mz -c grande.txt grande.mzp
```
 
O Monitor imprime os contadores subindo enquanto o filho ainda compacta.
 
**2. Processos e threads vistos pelo sistema operacional:**
 
```bash
./mz -v -s 40 -c entrada2.txt paralelo.mzp > log.txt &
ps -L -o pid,ppid,lwp,nlwp,stat,pcpu,comm -C mz
```
 
Aparecem dois processos (pai e filho, com `PPID` apontando para o pai). O filho tem
4 threads (`NLWP = 4`): a principal, que espera nos `pthread_join`, e as três de trabalho.
Também é possível usar o `htop` (`F5` para árvore, `Shift+H` para mostrar threads).
 
**3. O log do modo detalhado:**
 
```bash
head -n 20 log.txt
```
 
Todas as linhas do compactador têm o mesmo PID e três TIDs diferentes (leitora,
compactadora e gravadora), mostrando as threads trabalhando em pipeline.
 
## Limitações
 
- O programa trata apenas **texto ASCII**. Acentos em UTF-8 usam bytes a partir de 0x80,
  que se confundiriam com os códigos do dicionário. Os textos de teste não têm acentos.
- O dicionário foi montado a partir dos textos de teste. Palavras de outros textos que não
  estejam nele continuam sendo gravadas como estão, sem erro.
- Palavras de uma letra pouco frequentes (como `c`) ficam com código de 2 bytes e
  aumentam de tamanho. É o custo de cobrir todas as palavras dos textos.