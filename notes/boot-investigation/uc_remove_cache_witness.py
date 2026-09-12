"""Unicorn semantics witness: uc_ctl_remove_cache(uc, address, END) — NOT (address, size).

Contexto: tools/cpp/zeebo_lle_main.cpp chama uc_ctl_remove_cache(uc, ADDR, TAMANHO)
em 27 dos 28 call-sites (ex.: 0xb000c720 com 0x100). O header instalado declara
    #define uc_ctl_remove_cache(uc, address, end) ...
ou seja, o 2o argumento e' o ENDERECO FINAL. Com end < address o Unicorn devolve
UC_ERR_ARG e NAO invalida nada -- e o C++ ignora o retorno.

Este witness mede o comportamento, com controle positivo (forma correta) e
controle negativo (forma usada hoje). Escopo: semantica da API, nao o boot.
"""
import json
from pathlib import Path

import unicorn
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UcError

ADDR = 0xb0000720
cases = []
uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
uc.mem_map(0xb0000000, 0x1000)

for tag, a, b in (("como_o_projeto_chama__addr_size", ADDR, 0x100),
                  ("forma_correta__addr_end",        ADDR, ADDR + 0x100)):
    try:
        uc.ctl_remove_cache(a, b)
        cases.append({"caso": tag, "address": hex(a), "arg2": hex(b), "erro": None})
    except UcError as e:
        cases.append({"caso": tag, "address": hex(a), "arg2": hex(b), "erro": str(e)})

assert cases[0]["erro"] is not None, "controle negativo falhou: (addr,size) deveria dar UC_ERR_ARG"
assert cases[1]["erro"] is None, "controle positivo falhou: (addr,end) deveria passar"

report = {"unicorn": unicorn.__version__,
          "escopo": "semantics witness; nao e' regressao do boot",
          "conclusao": "o 2o argumento e' END; passar TAMANHO => UC_ERR_ARG => invalidacao nunca ocorreu",
          "cases": cases}
Path(__file__).with_suffix(".json").write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps(report, indent=2))
