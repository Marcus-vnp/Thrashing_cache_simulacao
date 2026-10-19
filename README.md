# Thrashing de cache: simulação e medição em C

Programa em C que demonstra **thrashing por conflito** na memória cache. Ele combina uma simulação de cache (taxa de miss) com a medição do tempo real de execução, e acompanha um script em Python que gera o gráfico comparativo.

O trabalho executado é o mesmo nos dois layouts de memória testados. A única diferença é o endereço-base de cada array, então qualquer diferença de tempo vem do mapeamento na cache.

## Conteúdo do repositório

| Arquivo | Descrição |
| --- | --- |
| `thrashing.c` | Programa principal: simulação de cache, medição de tempo e modo de varredura (`--csv`) |
| `plot_thrashing.py` | Lê o CSV da varredura e gera o gráfico de misses e tempo |
| `dados_thrashing.csv` | Exemplo de saída do modo `--csv` (opcional) |
| `grafico_thrashing.png` | Exemplo de gráfico gerado (opcional) |

## Como funciona

Numa cache associativa por conjuntos, um bloco da memória só pode ocupar as `vias` linhas do conjunto indicado por `(endereço / 64) % sets`. Se mais de `vias` blocos usados em sequência caem no mesmo conjunto, eles se expulsam mutuamente (*conflict misses*), mesmo que o resto da cache esteja vazio.

O programa soma N arrays de 256 KB elemento a elemento, em dois layouts:

- **Layout de conflito:** os arrays ficam a 256 KB uns dos outros. Essa distância é múltiplo de `sets × 64`, então a linha `i` de todos os arrays cai no mesmo conjunto. Com N maior que o número de vias, as linhas são expulsas antes de serem reutilizadas.
- **Layout com padding:** cada array é deslocado de 64 B (uma linha), o que espalha os arrays por conjuntos diferentes.

Há duas formas de observar o resultado:

1. **Simulação:** uma cache virtual (struct `Cache`, mapeamento por conjuntos, substituição LRU) processa os mesmos endereços e conta hits e misses. Um valor de 6,25% significa um miss a cada 16 acessos, que é o mínimo esperado (uma linha de 64 B guarda 16 `int`). Um valor de 100% significa que todo acesso errou.
2. **Medição real:** o programa cronometra a soma com `clock_gettime`, repete 7 vezes e usa o menor tempo.

Antes de medir, o programa calcula em qual conjunto da L1 cai cada array e imprime se há conflito. Para uma L1 com 64 conjuntos e linhas de 64 B, o índice usa os bits 6 a 11 do endereço, que ficam dentro do offset da página de 4 KB, então o cálculo vale para o endereço virtual e para o físico.

A memória dos arrays é alinhada a 2 MB e o programa pede páginas grandes (`madvise` com `MADV_HUGEPAGE`). Isso deixa a memória contígua na RAM física, de modo que as caches L2 e L3, indexadas por endereço físico, sigam o layout escolhido.

## Requisitos

- Linux (usa `clock_gettime`, `sysconf`, `aligned_alloc` e `madvise`)
- `gcc` ou outro compilador C99 ou mais novo
- Para o gráfico: Python 3 com `matplotlib` e `numpy` (`pip install matplotlib numpy`)

## Descobrindo os parâmetros das suas caches

```bash
lscpu -C
```

Nas linhas **L1d** e **L2**, use as colunas `WAYS` (vias) e `SETS` (conjuntos). Se `SETS` não aparecer, calcule `conjuntos = tamanho por núcleo ÷ (vias × 64)`, usando `ONE-SIZE`, que é o tamanho por núcleo. O `lscpu` sem `-C` mostra o tamanho somado de todos os núcleos, e o programa precisa do valor por núcleo.

## Compilar

```bash
gcc -O2 -fno-tree-vectorize -o thrashing thrashing.c
```

`-fno-tree-vectorize` evita que o compilador vetorize o laço de soma e altere o padrão de acesso.

## Modo 1: um experimento

```bash
taskset -c 2 ./thrashing [N_arrays] [vias_L1] [conjuntos_L1]
```

| Argumento | Padrão | Descrição |
| --- | --- | --- |
| `N_arrays` | 32 | Quantidade de arrays somados. O efeito só aparece quando N é maior que as vias |
| `vias_L1` | lido do sistema (8 se indisponível) | Associatividade da L1 de dados |
| `conjuntos_L1` | 64 | Número de conjuntos da L1 de dados |

Exemplo para uma L1 de 32 KB com 8 vias:

```bash
taskset -c 2 ./thrashing 32 8 64
```

Esse modo imprime a simulação com 2 arrays (mapeamento direto, padding, 2 e 8 vias), o mapeamento dos arrays e a comparação de tempo entre os layouts. `taskset -c 2` fixa o programa em um núcleo, o que reduz o ruído. Feche outros programas durante o teste.

## Modo 2: varredura em N (`--csv`)

Repete o experimento para N = 2, 4, 6, 8, 10, 12, 14, 16, 20, 24, 28 e 32, nos dois layouts, e imprime um CSV:

```bash
taskset -c 2 ./thrashing --csv <vias_L1> <conjuntos_L1> <vias_L2> <conjuntos_L2> > dados_thrashing.csv
```

| Posição | Significado |
| --- | --- |
| 1º | Vias da L1 de dados |
| 2º | Conjuntos da L1 de dados |
| 3º | Vias da L2 |
| 4º | Conjuntos da L2 |

Exemplo para uma máquina com L1 de 8 vias e 64 conjuntos, e L2 de 8 vias e 512 conjuntos:

```bash
taskset -c 2 ./thrashing --csv 8 64 8 512 > dados_thrashing.csv
```

Sem o `>`, o CSV só é impresso na tela e nenhum arquivo é criado. Se você omitir os valores, o programa assume 8, 64, 16 e 2048. Esses números configuram apenas a **simulação**: o tempo é sempre medido no seu processador. A varredura leva alguns segundos e o arquivo final tem 25 linhas (cabeçalho mais 24).

### Colunas do CSV

| Coluna | Conteúdo |
| --- | --- |
| `n` | Número de arrays somados |
| `layout` | `conflito` ou `padding` |
| `l1_miss_pct` | % de misses na L1 simulada, com a geometria informada |
| `l2_miss_global_pct` | % de acessos que erraram na L1 e na L2 (misses da L2 divididos pelo total de acessos) |
| `tempo_ms` | Tempo total medido, em ms |
| `ns_por_acesso` | Tempo médio por acesso, em ns |
| `l1_miss_2v_pct`, `l2_miss_2v_pct` | Misses de uma L1 hipotética de 32 KB com **2 vias** |
| `l1_miss_8v_pct`, `l2_miss_8v_pct` | Misses de uma L1 hipotética de 32 KB com **8 vias** |

## Gerar o gráfico

```bash
python3 plot_thrashing.py dados_thrashing.csv <vias_L1> <vias_L2> [saida.png]
```

Exemplo: `python3 plot_thrashing.py dados_thrashing.csv 8 8 grafico_thrashing.png`.

O gráfico tem três painéis:

1. **Miss na L1 (simulação):** compara L1 de 2 e de 8 vias, no layout de conflito e no de padding.
2. **Miss global na L2 (simulação).**
3. **Tempo por acesso (medido):** os dois layouts na sua máquina. As linhas verticais pontilhadas marcam onde N é igual ao número de vias.

Quando os dados permitem, o script também **estima** o tempo para as outras associatividades com um modelo calibrado nas suas medições. Se os misses de L1 e de L2 sobem juntos nos dados (como acontece quando a L2 tem poucas vias), o modelo não consegue separar os efeitos e a estimativa é omitida, com um aviso. O tempo medido corresponde sempre à L1 real da máquina.

## Exemplo de resultado

Máquina de teste: L1d de 8 vias e 64 conjuntos, L2 de 8 vias e 512 conjuntos. Tempo por acesso medido:

| N | Conflito | Padding | Lentidão |
| --- | --- | --- | --- |
| 8 | 0,495 ns | 0,455 ns | 1,1x |
| 10 | 0,712 ns | 0,465 ns | 1,5x |
| 16 | 1,005 ns | 0,455 ns | 2,2x |
| 32 | 1,835 ns | 0,438 ns | 4,2x |

Até N = 8 (as vias da L1) os dois layouts são parecidos. A partir de N = 10 o layout de conflito fica cada vez mais lento, enquanto o de padding permanece estável. Na simulação, a miss da L1 com 2 vias vai a 100% em N = 4, e com 8 vias em N = 10, enquanto o padding fica em 6,25%.

## Observações sobre os resultados

- **N deve ser maior que as vias.** Com N menor ou igual às vias, a lentidão fica perto de 1,0x.
- **O efeito real costuma ser menor que o teórico.** Prefetchers e a execução fora de ordem escondem parte do custo dos misses de L1, e a L2 atende boa parte deles. O tempo só dispara de forma acentuada quando os misses também chegam à L2.
- **Páginas grandes.** Se o efeito medido for muito menor que o esperado, verifique se o kernel permite páginas grandes: `cat /sys/kernel/mm/transparent_hugepage/enabled` deve mostrar `madvise` ou `always`.
- **A simulação usa LRU exato**, enquanto caches reais costumam usar aproximações. Os valores simulados servem como referência do mecanismo, não como previsão exata do hardware.
- **Ambientes virtualizados** podem informar parâmetros de cache diferentes dos reais. Confira com `lscpu -C`.

### Confirmando com contadores de hardware (opcional)

```bash
perf stat -e L1-dcache-loads,L1-dcache-load-misses ./thrashing 32 8 64
```

O programa executa os dois layouts em sequência, então os contadores somam ambos. Em máquinas virtuais, os contadores muitas vezes não estão disponíveis.

## Estrutura do código (`thrashing.c`)

| Parte | Descrição |
| --- | --- |
| `soma` | Laço medido: para cada índice, lê o elemento de cada um dos N arrays |
| `verifica_mapeamento` | Calcula e imprime em qual conjunto da L1 cai cada array |
| `mede` | Aloca os arrays (alinhados a 2 MB, com páginas grandes), cronometra `soma` e devolve o menor de 7 tempos |
| `experimento_tempo` | Compara os layouts de conflito e de padding para um N |
| `struct Cache`, `cache_acesso` | Cache simulada com mapeamento por conjuntos e LRU; `cache_acesso` devolve se foi hit ou miss |
| `simula`, `experimento_simulacao` | Simulação com 2 arrays em quatro configurações (direto, direto com padding, 2 e 8 vias) |
| `simula_n`, `varredura` | Simulação em L1 + L2 e medição para vários N, com saída em CSV |
| `main` | Lê os argumentos e executa o modo escolhido |

## Referências

- OUSTERHOUT, J. *Thrashing and Working Sets*. CS 140: Operating Systems, Stanford University, 2014. https://web.stanford.edu/~ouster/cgi-bin/cs140-spring14/lecture.php?topic=thrashing
- SONG, S. W. *MAC0344 2021 Aula 13: Memória cache*. e-Aulas USP, 2021. https://eaulas.usp.br/portal/Coffee/Coffee-Set/video?idItem=31552
- ALEKSAMCODE. *Cache Simulator*. https://github.com/AleksaMCode/cache-simulator
- TANENBAUM, A. S.; AUSTIN, T. *Organização estruturada de computadores*. 6. ed. São Paulo: Pearson, 2013.
