# Thrashing de cache: simulação e medição em C

Programa em C que demonstra **thrashing por conflito** na memória cache. Ele tem dois experimentos:

1. **Simulação de cache:** simula uma cache de 32 KB (linha de 64 B, LRU) e conta hits e misses quando dois arrays são acessados de forma alternada.
2. **Medição de tempo real:** soma N arrays em dois layouts de memória, um que provoca conflito na L1 e outro com *padding*, e compara o tempo.

O trabalho executado é o mesmo nos dois layouts. A única diferença é o endereço-base de cada array, então a diferença de tempo vem do mapeamento na cache.

## Como funciona

Numa cache associativa por conjuntos, um bloco da memória só pode ocupar as `vias` linhas do conjunto indicado por `(endereço / 64) % sets`. Se mais de `vias` blocos usados em sequência caem no mesmo conjunto, eles se expulsam mutuamente (*conflict misses*), mesmo que o resto da cache esteja vazio.

- **Layout de conflito:** os arrays ficam a 256 KB uns dos outros. Essa distância é múltiplo de `sets × 64`, então a linha `i` de todos os arrays cai no mesmo conjunto. Com N maior que o número de vias, as linhas são expulsas antes de serem reutilizadas.
- **Layout com padding:** cada array é deslocado de 64 B (uma linha), o que espalha os arrays por conjuntos diferentes.

Antes de medir, o programa calcula o conjunto da L1 em que cai cada array e imprime se há conflito. Para L1 com 64 conjuntos e linha de 64 B, o índice usa os bits 6 a 11 do endereço, que ficam dentro do offset da página de 4 KB. Por isso o cálculo vale tanto para o endereço virtual quanto para o físico.

## Requisitos

- Linux (usa `clock_gettime`, `sysconf` e `aligned_alloc`)
- `gcc` ou outro compilador C99 ou mais novo

## Compilar

```bash
gcc -O2 -fno-tree-vectorize -o thrashing thrashing.c
```

`-fno-tree-vectorize` evita que o compilador vetorize o laço de soma e altere o padrão de acesso.

## Executar

```bash
taskset -c 2 ./thrashing [N_arrays] [vias_L1] [conjuntos_L1]
```

| Argumento | Padrão | Descrição |
| --- | --- | --- |
| `N_arrays` | 32 | Quantidade de arrays somados. O efeito só aparece quando N é maior que as vias |
| `vias_L1` | lido do sistema (8 se indisponível) | Associatividade da L1 de dados |
| `conjuntos_L1` | 64 | Número de conjuntos da L1 de dados |

`taskset -c 2` fixa o programa em um núcleo, o que reduz o ruído nas medições. Feche outros programas durante o teste.

### Descobrindo os parâmetros da sua L1

```bash
lscpu -C
```

Na linha da L1d, use `ONE-SIZE` (tamanho por núcleo) e `WAYS` (vias). Se o número de conjuntos não aparecer, calcule `conjuntos = tamanho por núcleo ÷ (vias × 64)`. Atenção: o `lscpu` sem `-C` mostra o tamanho somado de todos os núcleos, e o programa precisa do valor por núcleo.

Exemplo para uma L1 de 32 KB com 8 vias:

```bash
taskset -c 2 ./thrashing 32 8 64
```

## Exemplo de saída

Exemplo de uma execução em uma máquina virtual (seus números variam):

```
=== Experimento 2: simulacao (cache 32 KB, linha 64 B, LRU) ===
Acesso alternado A[i], B[i]
direto,  A e B a 32 KB (conflito)            hits=      0 misses=  81920  miss rate=100.00%
direto,  A e B a 32 KB + 64 B (padding)      hits=  76818 misses=   5102  miss rate=  6.23%
2 vias,  A e B a 32 KB                       hits=  81408 misses=    512  miss rate=  0.62%
8 vias,  A e B a 32 KB                       hits=  81408 misses=    512  miss rate=  0.62%

[mapeamento conflito] 32 arrays em 1 conjunto(s) distinto(s); maximo 32 arrays por conjunto (vias=8) -> CONFLITO: excede a associatividade
   conjuntos dos 4 primeiros arrays: 0 0 0 0
[mapeamento padding ] 32 arrays em 32 conjunto(s) distinto(s); maximo 1 arrays por conjunto (vias=8) -> sem conflito
   conjuntos dos 4 primeiros arrays: 0 1 2 3
=== Experimento 1: tempo real ===
L1 assumida: 8 vias, 64 conjuntos, linha de 64 B
Arrays: 32 x 256 KB  (N=32 > vias=8)
Layout conflito (stride 262144 B):    62.98 ms  (0.751 ns/acesso)
Layout padding  (stride 262208 B):    40.25 ms  (0.480 ns/acesso)
Slowdown conflito/padding: 1.56x   (checksum 9890258002903040 9890258002903040)
```

### Como ler a saída

- **Simulação:** com mapeamento direto e os arrays a 32 KB, todo acesso dá miss (100%). Com padding de 64 B, a taxa cai para cerca de 1 miss a cada 16 acessos (uma linha de 64 B guarda 16 `int`). Com 2 ou mais vias, os dois arrays convivem no mesmo conjunto sem conflito.
- **Mapeamento:** confirma que o layout de conflito põe os N arrays no mesmo conjunto e que o de padding os espalha.
- **Tempo real:** `Slowdown` é o tempo do layout de conflito dividido pelo do layout com padding. Os checksums iguais mostram que os dois fizeram a mesma conta.

## Observações sobre os resultados

- **N deve ser maior que as vias.** Com N menor ou igual às vias, a lentidão fica perto de 1,0x. Aumentar N reforça o efeito, porque os arrays também passam a exceder as vias da L2.
- **O efeito real costuma ser menor que o teórico.** Prefetchers e a execução fora de ordem escondem parte do custo dos misses de L1, e a L2 atende boa parte deles.
- **Ambientes virtualizados** podem informar parâmetros de cache diferentes dos reais. Confira com `lscpu -C`.

### Confirmando com contadores de hardware (opcional)

```bash
perf stat -e L1-dcache-loads,L1-dcache-load-misses ./thrashing 32 8 64
```

O programa executa os dois layouts em sequência, então os contadores somam ambos. Em máquinas virtuais, os contadores muitas vezes não estão disponíveis.

## Estrutura do código

| Parte | Descrição |
| --- | --- |
| `soma` | Laço medido: para cada índice, lê o elemento de cada um dos N arrays |
| `verifica_mapeamento` | Calcula e imprime em qual conjunto da L1 cai cada array |
| `mede` | Aloca os arrays alinhados a 4096 B, cronometra `soma` e devolve o menor de 7 tempos |
| `experimento_tempo` | Compara os layouts de conflito e de padding |
| `struct Cache`, `cache_acesso`, `simula` | Cache simulada com mapeamento por conjuntos e LRU |
| `experimento_simulacao` | Roda a simulação em quatro configurações (direto, direto com padding, 2 e 8 vias) |
| `main` | Lê os argumentos e executa os dois experimentos |

## Referências

- OUSTERHOUT, J. *Thrashing and Working Sets*. CS 140: Operating Systems, Stanford University, 2014. https://web.stanford.edu/~ouster/cgi-bin/cs140-spring14/lecture.php?topic=thrashing
- SONG, S. W. *MAC0344 2021 Aula 13: Memória cache*. e-Aulas USP, 2021. https://eaulas.usp.br/portal/Coffee/Coffee-Set/video?idItem=31552
- ALEKSAMCODE. *Cache Simulator*. https://github.com/AleksaMCode/cache-simulator
- TANENBAUM, A. S.; AUSTIN, T. *Organização estruturada de computadores*. 6. ed. São Paulo: Pearson, 2013.
