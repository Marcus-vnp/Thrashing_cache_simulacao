/*
 * thrashing.c - Demonstracao de thrashing (conflict misses) em cache.
 *
 * Experimento 1: medicao real de tempo.
 *   Soma N arrays elemento a elemento. No layout "conflito", as bases dos
 *   arrays distam um multiplo de (sets * linha), entao a linha i de TODOS os
 *   arrays cai no mesmo conjunto da L1. Com N > vias, as linhas se expulsam
 *   antes de serem reutilizadas. No layout "padding", cada array e deslocado
 *   de uma linha (64 B) e os conjuntos ficam diferentes.
 *
 * Experimento 2: simulacao de cache (hits/misses), 2 arrays distantes 32 KB.
 *
 * Compilar:  gcc -O2 -fno-tree-vectorize -o thrashing thrashing.c
 * Executar:  taskset -c 2 ./thrashing [N_arrays=32] [vias_L1] [sets_L1=64]
 * Varredura: taskset -c 2 ./thrashing --csv [vias_L1] [sets_L1] [vias_L2] [sets_L2] > dados.csv
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>

#define LINE 64
#define ARRAY_BYTES (256 * 1024)           /* tamanho de cada array (multiplo de 4096) */
#define ARRAY_INTS (ARRAY_BYTES / sizeof(int))
#define REPS 7                             /* repeticoes; usamos o minimo */
#define PASSES 40                          /* passadas sobre os arrays por repeticao */

static int g_silencioso = 0;               /* 1 = nao imprime o mapeamento (modo CSV) */

static double now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e9 + t.tv_nsec;
}

/* ---------------- Experimento 1: tempo real ---------------- */

__attribute__((noinline))
static long soma(int **p, int n) {
    long s = 0;
    for (int pass = 0; pass < PASSES; pass++)
        for (size_t i = 0; i < ARRAY_INTS; i++)
            for (int k = 0; k < n; k++)
                s += p[k][i];
    return s;
}

/*
 * Prova, a partir dos enderecos REAIS, em qual conjunto da L1 cai a primeira
 * linha de cada array:  conjunto = (endereco / 64) % sets.
 * A L1 e indexada pelos bits [6..11] do endereco (para 64 conjuntos), que ficam
 * dentro do offset da pagina de 4 KB e portanto sao iguais no endereco virtual
 * e no fisico. Assim o calculo vale para o hardware real.
 * Como todos os arrays sao percorridos em passo, o conjunto de A_k[i] e
 * (conjunto_inicial + i/16) % sets: se as bases coincidem, coincidem sempre.
 */
static void verifica_mapeamento(const char *nome, int **p, int n, int sets, int vias) {
    int *contagem = calloc(sets, sizeof(int));
    for (int k = 0; k < n; k++) {
        uintptr_t a = (uintptr_t)p[k];
        contagem[(a / LINE) % sets]++;
    }
    int distintos = 0, maximo = 0;
    for (int s = 0; s < sets; s++) {
        if (contagem[s]) distintos++;
        if (contagem[s] > maximo) maximo = contagem[s];
    }
    printf("[mapeamento %-8s] %d arrays em %d conjunto(s) distinto(s); "
           "maximo %d arrays por conjunto (vias=%d) -> %s\n",
           nome, n, distintos, maximo, vias,
           maximo > vias ? "CONFLITO: excede a associatividade" : "sem conflito");
    printf("   conjuntos dos 4 primeiros arrays:");
    for (int k = 0; k < n && k < 4; k++)
        printf(" %zu", ((uintptr_t)p[k] / LINE) % sets);
    printf("\n");
    free(contagem);
}

/* stride_bytes: distancia entre as bases de arrays consecutivos */
static double mede(int n, size_t stride_bytes, long *resultado,
                   const char *nome, int sets, int vias) {
    size_t total = (size_t)n * stride_bytes + 4096;
    /* Alinha a 2 MB e pede paginas grandes (THP): a memoria fica contigua na RAM
     * fisica, e o conjunto das caches L2/L3 (indexadas por endereco fisico)
     * passa a seguir o layout escolhido, em vez de depender de paginas
     * de 4 KB espalhadas pelo sistema. */
    size_t tam = (total + (2UL << 20) - 1) & ~((2UL << 20) - 1);
    char *bloco = aligned_alloc(2UL << 20, tam);
    if (!bloco) { perror("aligned_alloc"); exit(1); }
#ifdef MADV_HUGEPAGE
    madvise(bloco, tam, MADV_HUGEPAGE);
#endif
    memset(bloco, 1, total);

    int **p = malloc(n * sizeof(int *));
    for (int k = 0; k < n; k++)
        p[k] = (int *)(bloco + (size_t)k * stride_bytes);

    if ((uintptr_t)bloco % 4096 != 0) {
        fprintf(stderr, "erro: bloco nao alinhado a 4096 B\n");
        exit(1);
    }
    if (!g_silencioso) verifica_mapeamento(nome, p, n, sets, vias);

    double melhor = 1e30;
    long s = 0;
    for (int r = 0; r < REPS; r++) {
        double t0 = now_ns();
        s += soma(p, n);
        double t1 = now_ns();
        if (t1 - t0 < melhor) melhor = t1 - t0;
    }
    *resultado = s;           /* impede o compilador de eliminar o laco */
    free(p);
    free(bloco);
    return melhor;
}

static void experimento_tempo(int n, int vias, int sets) {
    size_t periodo = (size_t)sets * LINE;   /* bytes ate o mapa de conjuntos repetir */
    /* ARRAY_BYTES e multiplo de 'periodo' quando sets e potencia de 2 <= 4096 */
    size_t stride_conflito = ARRAY_BYTES;                 /* bases no mesmo conjunto */
    size_t stride_padding = ARRAY_BYTES + LINE;           /* +1 linha: conjuntos diferentes */

    if (ARRAY_BYTES % periodo != 0)
        fprintf(stderr, "aviso: ARRAY_BYTES nao e multiplo de sets*linha; "
                        "o layout 'conflito' pode nao conflitar.\n");

    long r1, r2;
    double t_conf = mede(n, stride_conflito, &r1, "conflito", sets, vias);
    double t_pad = mede(n, stride_padding, &r2, "padding", sets, vias);
    double acessos = (double)n * ARRAY_INTS * PASSES;

    printf("=== Experimento 1: tempo real ===\n");
    printf("L1 assumida: %d vias, %d conjuntos, linha de %d B\n", vias, sets, LINE);
    printf("Arrays: %d x %d KB  (N=%d %s vias=%d)\n", n, ARRAY_BYTES / 1024, n,
           n > vias ? ">" : "<=", vias);
    printf("Layout conflito (stride %zu B): %8.2f ms  (%.3f ns/acesso)\n",
           stride_conflito, t_conf / 1e6, t_conf / acessos);
    printf("Layout padding  (stride %zu B): %8.2f ms  (%.3f ns/acesso)\n",
           stride_padding, t_pad / 1e6, t_pad / acessos);
    printf("Slowdown conflito/padding: %.2fx   (checksum %ld %ld)\n\n",
           t_conf / t_pad, r1, r2);
}

/* ---------------- Experimento 2: simulacao de cache ---------------- */

typedef struct {
    int sets, vias;
    uint64_t *tag;     /* sets*vias */
    uint64_t *idade;   /* para LRU */
    uint8_t *valida;
    uint64_t relogio, hits, misses;
} Cache;

static void cache_init(Cache *c, int tam_bytes, int vias) {
    c->vias = vias;
    c->sets = tam_bytes / (LINE * vias);
    size_t n = (size_t)c->sets * vias;
    c->tag = calloc(n, sizeof(uint64_t));
    c->idade = calloc(n, sizeof(uint64_t));
    c->valida = calloc(n, 1);
    c->relogio = c->hits = c->misses = 0;
}

static void cache_free(Cache *c) { free(c->tag); free(c->idade); free(c->valida); }

static int cache_acesso(Cache *c, uint64_t addr) {
    uint64_t bloco = addr / LINE;
    int set = bloco % c->sets;
    uint64_t tag = bloco / c->sets;
    uint64_t *T = &c->tag[(size_t)set * c->vias];
    uint64_t *A = &c->idade[(size_t)set * c->vias];
    uint8_t *V = &c->valida[(size_t)set * c->vias];
    c->relogio++;

    int vitima = 0;
    for (int w = 0; w < c->vias; w++) {
        if (V[w] && T[w] == tag) { A[w] = c->relogio; c->hits++; return 1; }
    }
    c->misses++;
    for (int w = 0; w < c->vias; w++) {     /* linha invalida ou a mais antiga (LRU) */
        if (!V[w]) { vitima = w; break; }
        if (A[w] < A[vitima]) vitima = w;
    }
    V[vitima] = 1; T[vitima] = tag; A[vitima] = c->relogio;
    return 0;
}

static void simula(const char *nome, int cache_bytes, int vias, uint64_t dist) {
    Cache c;
    cache_init(&c, cache_bytes, vias);
    const uint64_t base_a = 0, base_b = dist;
    const int n = 4096;                      /* elementos int por array */
    for (int rep = 0; rep < 10; rep++)
        for (int i = 0; i < n; i++) {
            cache_acesso(&c, base_a + (uint64_t)i * 4);
            cache_acesso(&c, base_b + (uint64_t)i * 4);
        }
    uint64_t tot = c.hits + c.misses;
    printf("%-44s hits=%7llu misses=%7llu  miss rate=%6.2f%%\n", nome,
           (unsigned long long)c.hits, (unsigned long long)c.misses,
           100.0 * c.misses / tot);
    cache_free(&c);
}

static void experimento_simulacao(void) {
    printf("=== Experimento 2: simulacao (cache 32 KB, linha 64 B, LRU) ===\n");
    printf("Acesso alternado A[i], B[i]\n");
    simula("direto,  A e B a 32 KB (conflito)", 32768, 1, 32768);
    simula("direto,  A e B a 32 KB + 64 B (padding)", 32768, 1, 32768 + 64);
    simula("2 vias,  A e B a 32 KB", 32768, 2, 32768);
    simula("8 vias,  A e B a 32 KB", 32768, 8, 32768);
    printf("\n");
}


/* ---------------- Experimento 3: varredura em N (modo CSV) ---------------- */

/*
 * Simula UMA passada do laco de soma de 'soma()': para cada i, le o elemento i
 * dos N arrays (bases separadas por 'stride' bytes), em uma hierarquia L1 + L2
 * (LRU). Devolve a taxa de miss da L1 e a taxa GLOBAL de miss da L2
 * (misses da L2 / total de acessos), ambas em %.
 */
static void simula_n(int n, size_t stride, int vias1, int sets1, int vias2, int sets2,
                     double *miss_l1, double *miss_l2) {
    Cache c1, c2;
    cache_init(&c1, sets1 * vias1 * LINE, vias1);
    cache_init(&c2, sets2 * vias2 * LINE, vias2);
    for (size_t i = 0; i < ARRAY_INTS; i++)
        for (int k = 0; k < n; k++) {
            uint64_t addr = (uint64_t)k * stride + i * sizeof(int);
            if (!cache_acesso(&c1, addr))
                cache_acesso(&c2, addr);
        }
    double total = (double)n * ARRAY_INTS;
    *miss_l1 = 100.0 * c1.misses / total;
    *miss_l2 = 100.0 * c2.misses / total;
    cache_free(&c1);
    cache_free(&c2);
}

static void varredura(int vias1, int sets1, int vias2, int sets2) {
    static const int Ns[] = {2, 4, 6, 8, 10, 12, 14, 16, 20, 24, 28, 32};
    g_silencioso = 1;
    printf("n,layout,l1_miss_pct,l2_miss_global_pct,tempo_ms,ns_por_acesso,"
           "l1_miss_2v_pct,l2_miss_2v_pct,l1_miss_8v_pct,l2_miss_8v_pct\n");
    for (size_t j = 0; j < sizeof(Ns) / sizeof(Ns[0]); j++) {
        int n = Ns[j];
        for (int lay = 0; lay < 2; lay++) {
            size_t stride = lay == 0 ? ARRAY_BYTES : ARRAY_BYTES + LINE;
            double m1, m2, a1, a2, b1, b2;
            long chk;
            simula_n(n, stride, vias1, sets1, vias2, sets2, &m1, &m2);
            /* L1 hipotetica de 32 KB com 2 vias (256 conjuntos) e com 8 vias (64 conjuntos) */
            simula_n(n, stride, 2, 32768 / (2 * LINE), vias2, sets2, &a1, &a2);
            simula_n(n, stride, 8, 32768 / (8 * LINE), vias2, sets2, &b1, &b2);
            double t = mede(n, stride, &chk, "", sets1, vias1);
            double acessos = (double)n * ARRAY_INTS * PASSES;
            printf("%d,%s,%.3f,%.3f,%.3f,%.4f,%.3f,%.3f,%.3f,%.3f\n", n,
                   lay == 0 ? "conflito" : "padding", m1, m2, t / 1e6, t / acessos,
                   a1, a2, b1, b2);
            fflush(stdout);
        }
    }
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--csv") == 0) {
        /* ./thrashing --csv [vias_L1=8] [sets_L1=64] [vias_L2=16] [sets_L2=2048] */
        int v1 = argc > 2 ? atoi(argv[2]) : 8, s1 = argc > 3 ? atoi(argv[3]) : 64;
        int v2 = argc > 4 ? atoi(argv[4]) : 16, s2 = argc > 5 ? atoi(argv[5]) : 2048;
        varredura(v1, s1, v2, s2);
        return 0;
    }
    int vias = 0, sets = 64;
#ifdef _SC_LEVEL1_DCACHE_ASSOC
    vias = (int)sysconf(_SC_LEVEL1_DCACHE_ASSOC);
#endif
    if (vias <= 0) vias = 8;
    int n = (argc > 1) ? atoi(argv[1]) : 32;   /* N > vias da L1 e da L2 */
    if (argc > 2) vias = atoi(argv[2]);
    if (argc > 3) sets = atoi(argv[3]);
    if (n < 2) n = 2;

    experimento_simulacao();
    experimento_tempo(n, vias, sets);
    return 0;
}
