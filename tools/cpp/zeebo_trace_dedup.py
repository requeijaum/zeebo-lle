#!/usr/bin/env python3
"""
QW2 — Deduplicação de trace no cliente Python (Zeebo LLE).

Torna loops apertados, CTZ e NOP-slides legíveis num trace de PCs sem qualquer
instrumentação C++ permanente no core. É uma camada puramente de cliente:

  - máscara de PCs visitados (`visited`): quais PCs já foram emitidos;
  - janela de repetição recente (`recent_depth`): supressão só vale enquanto o
    PC continua dentro dos N passos mais recentes — um PC que "saiu" da janela
    volta a ser emitido, preservando marcos de progresso;
  - contador de omitidos (`omitted`): quantas linhas de trace foram suprimidas;
  - invalidação por escrita de código (SMC): `invalidate(addr, size)` remove da
    máscara qualquer PC que caia no range escrito por um `poke`, tornando-o
    elegível a reemissão (self-modifying code passa a reaparecer no trace).

Determinístico e sem dependência de socket/emulador — testável isoladamente.
"""

from collections import deque


class TraceDeduplicator:
    def __init__(self, recent_depth=64):
        if recent_depth < 1:
            raise ValueError("recent_depth deve ser >= 1")
        self.recent_depth = recent_depth
        self.visited = set()            # máscara de PCs já emitidos
        self.recent = deque(maxlen=recent_depth)  # janela de PCs recentes
        self.omitted = 0
        self.emitted = 0

    def observe(self, pc):
        """Registra um PC do trace. Retorna True se deve ser emitido, False se
        omitido por deduplicação. Suprime apenas repetições que ainda estejam
        na janela recente e cujo PC já tenha sido emitido."""
        suppress = pc in self.visited and pc in self.recent
        self.recent.append(pc)
        if suppress:
            self.omitted += 1
            return False
        self.visited.add(pc)
        self.emitted += 1
        return True

    def invalidate(self, addr, size=4):
        """Invalidação após `poke` que altera código executável (SMC).

        Remove da máscara e da janela qualquer PC no range [addr, addr+size),
        tornando-o elegível a reemissão. Retorna o conjunto de PCs afetados."""
        if size < 1:
            size = 1
        lo, hi = addr, addr + size
        removed = {pc for pc in self.visited if lo <= pc < hi}
        if removed:
            self.visited -= removed
            # Reconstrói a janela sem os PCs invalidados, preservando ordem.
            kept = [pc for pc in self.recent if pc not in removed]
            self.recent = deque(kept, maxlen=self.recent_depth)
        return removed

    def reset(self):
        self.visited.clear()
        self.recent.clear()
        self.omitted = 0
        self.emitted = 0

    def stats(self):
        return {
            "emitted": self.emitted,
            "omitted": self.omitted,
            "unique_pcs": len(self.visited),
            "recent_depth": self.recent_depth,
        }
