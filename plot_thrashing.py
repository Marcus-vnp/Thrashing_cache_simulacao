#!/usr/bin/env python3
"""Gera o grafico de misses e tempo (com comparacao 2 vias x 8 vias).

Uso: python3 plot_thrashing.py dados_thrashing.csv [vias_L1_hw] [vias_L2] [saida.png]

Misses de 2 e 8 vias: vem da simulacao (colunas *_2v_* e *_8v_* do CSV).
Tempo de 2 e 8 vias: ESTIMADO. O hardware real so tem uma associatividade, entao
ajusta-se, com os tempos medidos nele, o modelo
    tempo(ns) = a + b*miss_L1 + c*miss_L2 + d*N*miss_L2      (miss em fracao, 0..1)
e aplica-se esse modelo aos misses simulados de 2 e 8 vias.
"""
import csv
import sys
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

arq = sys.argv[1] if len(sys.argv) > 1 else "dados_thrashing.csv"
vias_hw = int(sys.argv[2]) if len(sys.argv) > 2 else 12
vias2 = int(sys.argv[3]) if len(sys.argv) > 3 else 16
saida = sys.argv[4] if len(sys.argv) > 4 else "grafico_thrashing.png"

NECESSARIAS = {"n", "layout", "l1_miss_pct", "l2_miss_global_pct", "ns_por_acesso",
               "l1_miss_2v_pct", "l2_miss_2v_pct", "l1_miss_8v_pct", "l2_miss_8v_pct"}
dados = {"conflito": {}, "padding": {}}
try:
    with open(arq) as f:
        leitor = csv.DictReader(f)
        faltam = NECESSARIAS - set(leitor.fieldnames or [])
        if faltam:
            sys.exit(f"erro: '{arq}' nao parece um CSV valido (faltam colunas: "
                     f"{', '.join(sorted(faltam))}).\n"
                     "Gere-o com a versao atual do thrashing.c:\n"
                     "  ./thrashing --csv <vias_L1> <sets_L1> <vias_L2> <sets_L2> > dados_thrashing.csv")
        for r in leitor:
            dados[r["layout"]][int(r["n"])] = r
except FileNotFoundError:
    sys.exit(f"erro: arquivo '{arq}' nao encontrado.")
if not dados["conflito"] or not dados["padding"]:
    sys.exit(f"erro: '{arq}' nao tem linhas de dados (arquivo vazio?).\n"
             "Rode o programa de novo, SUBSTITUINDO os marcadores pelos numeros reais:\n"
             "  ./thrashing --csv 8 64 16 2048 > dados_thrashing.csv\n"
             "e confira com:  wc -l dados_thrashing.csv   (deve ter 25 linhas)")
ns = sorted(dados["conflito"])
col = lambda lay, c: np.array([float(dados[lay][n][c]) for n in ns])

# ---- modelo de tempo calibrado com as medicoes reais ----
Nn = np.array(ns, float)
def features(m1, m2, n):
    m1, m2 = m1 / 100.0, m2 / 100.0
    return np.column_stack([np.ones_like(m1), m1, m2, n * m2])

X = np.vstack([features(col(l, "l1_miss_pct"), col(l, "l2_miss_global_pct"), Nn)
               for l in ("conflito", "padding")])
y = np.concatenate([col(l, "ns_por_acesso") for l in ("conflito", "padding")])
coef, *_ = np.linalg.lstsq(X, y, rcond=None)
r2 = 1 - np.sum((y - X @ coef) ** 2) / np.sum((y - y.mean()) ** 2)
print("coeficientes a,b,c,d =", np.round(coef, 4), " R2 =", round(r2, 3))

def estima(lay, v):
    return features(col(lay, f"l1_miss_{v}v_pct"), col(lay, f"l2_miss_{v}v_pct"), Nn) @ coef

AZUL, LARANJA, VERDE = "#1f6f99", "#e0702f", "#4a9a5b"
fig, (ax1, ax2, ax3) = plt.subplots(3, 1, figsize=(10, 12), sharex=True)

# Painel 1: miss L1 (simulacao)
ax1.plot(ns, col("conflito", "l1_miss_2v_pct"), "o-", color=LARANJA, lw=2.2,
         label="Conflito, L1 de 2 vias")
ax1.plot(ns, col("conflito", "l1_miss_8v_pct"), "s-", color=VERDE, lw=2.2,
         label="Conflito, L1 de 8 vias")
ax1.plot(ns, col("padding", "l1_miss_2v_pct"), "^-", color=AZUL, lw=2.2,
         label="Padding (2 e 8 vias)")
ax1.set_ylabel("Miss na L1 (%)")
ax1.set_title("Simulação: taxa de miss na L1 (cache de 32 KB)")
ax1.set_ylim(-5, 108)

# Painel 2: miss L2 global (simulacao)
ax2.plot(ns, col("conflito", "l2_miss_2v_pct"), "o-", color=LARANJA, lw=2.2,
         label="Conflito, L1 de 2 vias")
ax2.plot(ns, col("conflito", "l2_miss_8v_pct"), "s--", color=VERDE, lw=2.2,
         label="Conflito, L1 de 8 vias")
ax2.plot(ns, col("padding", "l2_miss_2v_pct"), "^-", color=AZUL, lw=2.2,
         label="Padding (2 e 8 vias)")
ax2.set_ylabel("Miss global na L2 (%)")
ax2.set_title(f"Simulação: taxa de miss global na L2 ({vias2} vias)")
ax2.set_ylim(-5, 108)

# Painel 3: tempo
# O modelo so e confiavel se os dados medidos trazem um caso com miss alto na L1
# e miss baixo na L2 ao mesmo tempo; senao os efeitos de L1 e L2 nao se separam.
identificavel = bool(np.any((col("conflito", "l1_miss_pct") > 50) &
                            (col("conflito", "l2_miss_global_pct") < 50)))
ax3.plot(ns, col("conflito", "ns_por_acesso"), "o-", color="black", lw=2,
         label=f"Medido (hardware, L1 de {vias_hw} vias): conflito")
ax3.plot(ns, col("padding", "ns_por_acesso"), "o-", color="gray", lw=2,
         label=f"Medido (hardware, L1 de {vias_hw} vias): padding")
if identificavel:
    for v, mk, cor in (("2", "s--", LARANJA), ("8", "^--", VERDE)):
        if int(v) != vias_hw:
            ax3.plot(ns, estima("conflito", v), mk, color=cor, lw=2.2,
                     label=f"Estimado: conflito, {v} vias")
    vp = "2" if vias_hw != 2 else "8"
    ax3.plot(ns, estima("padding", vp), "v--", color=AZUL, lw=2.2,
             label="Estimado: padding")
    ax3.set_title(f"Tempo por acesso (medido e estimado por modelo, R² = {r2:.2f})")
else:
    ax3.set_title(f"Tempo por acesso (medido na máquina, L1 de {vias_hw} vias)")
    print("aviso: estimativa de tempo omitida (miss de L1 e L2 sobem juntos nos dados).")
ax3.set_ylabel("Tempo por acesso (ns)")
ax3.set_xlabel("Número de arrays somados (N)")
ax3.set_ylim(0, max(col("conflito", "ns_por_acesso")) * 1.2)

marcas = {}
for v, c, t in ((2, LARANJA, "2 vias"), (8, VERDE, "8 vias"), (vias2, "gray", f"{vias2} vias (L2)")):
    marcas.setdefault(v, [c, []])[1].append(t)
for ax in (ax1, ax2, ax3):
    for v, (c, _) in marcas.items():
        ax.axvline(v, color=c, ls=":", lw=1.4)
    ax.grid(alpha=0.25)
    ax.legend(loc="center left", fontsize=8.5, frameon=False)
for v, (c, ts) in marcas.items():
    ax1.text(v - 0.25, 100, "N = " + " / ".join(ts), rotation=90, ha="right",
             va="top", fontsize=8, color=c)
ax3.set_xticks(ns)

fig.tight_layout()
fig.savefig(saida, dpi=160)
print("salvo em", saida)
