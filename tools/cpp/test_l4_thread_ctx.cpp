// test_l4_thread_ctx.cpp — Bug 2: preservação completa de contexto de CPU + SID.
// on_exchange_registers só guardava sp/ip/flags. Uma troca de thread cooperativa
// precisa salvar/restaurar o contexto COMPLETO (r0..r12, sp, lr, pc, cpsr) e o
// space_id (SID) de cada thread, senão a thread retomada perde registradores e
// o modo (ARM/Thumb via CPSR-T).
#include "zeebo_l4_thread.h"
#include <cassert>
#include <cstdio>
#include <cstring>

using namespace zeebo_l4;
static int g_fail=0;
#define CHECK(c) do{ if(!(c)){ printf("  FAIL: %s (line %d)\n",#c,__LINE__); g_fail++; } }while(0)

int main(){
    printf("== L4 thread full-context + SID ==\n");
    ThreadTable tt;

    // Cria thread 0x14 com SID 3.
    uint32_t dest=0x14, ctrl=EXREGS_CTRL_SP|EXREGS_CTRL_IP|EXREGS_CTRL_DELIVER;
    tt.on_exchange_registers(dest, ctrl, 0xb01ffff0, 0xb0100000, 0);
    tt.set_thread_space(dest, 3);
    const ThreadInfo* t = tt.get_thread(dest);
    CHECK(t && t->space_id==3);

    // Salva contexto COMPLETO da thread corrente.
    CpuContext ctx{};
    for(int i=0;i<13;i++) ctx.r[i]=0x1000+i;
    ctx.sp=0xb01fff00; ctx.lr=0xb0100abc; ctx.pc=0xb0100200;
    ctx.cpsr=0x60000030; // modo Thumb (bit 5 set) + condition flags
    tt.save_context(dest, ctx);

    const ThreadInfo* t2 = tt.get_thread(dest);
    CHECK(t2->ctx.pc==0xb0100200);
    CHECK(t2->ctx.cpsr==0x60000030);
    CHECK(t2->ctx.r[5]==0x1005);
    CHECK(t2->ctx.lr==0xb0100abc);
    CHECK(t2->has_context==true);

    // Restaura: retorna byte-idêntico.
    CpuContext out{};
    CHECK(tt.load_context(dest, &out));
    CHECK(memcmp(&out,&ctx,sizeof(CpuContext))==0);

    // SID preservado independente de novo exregs sem alteração de SID.
    tt.on_exchange_registers(dest, 0, 0,0,0);
    CHECK(tt.get_thread(dest)->space_id==3);

    // Thread sem contexto salvo: load_context falha (negativo).
    uint32_t t3=0x30;
    tt.on_exchange_registers(t3, ctrl, 0xc0000000, 0xc0001000, 0);
    CpuContext dummy{};
    CHECK(tt.load_context(t3,&dummy)==false);
    CHECK(tt.get_thread(t3)->has_context==false);

    // Modo preservado: bit T do CPSR sobrevive ao round-trip.
    CHECK((out.cpsr>>5)&1);

    if(g_fail==0){ printf("ALL TESTS PASSED\n"); return 0; }
    printf("%d CHECK(s) FAILED\n", g_fail); return 1;
}
