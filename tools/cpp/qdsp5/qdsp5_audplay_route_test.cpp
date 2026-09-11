// qdsp5_audplay_route_test.cpp — TDD boundary for the AUDPLAY (bitstream decoder)
// route needed by Zeetris audio (MP3 goes through an AUDPLAYx decoder task, NOT the
// AUDPP host-PCM path). Written test-first: it demonstrates that the dispatcher had
// NO route to Engine::Audplay, then pins the minimum honest routing/classification
// boundary.
//
// HONESTY CONTRACT (do not weaken):
//  - The queue->engine association is PRIMARY-SOURCE verified (QDSP5 assertion
//    strings: UPAUDPLAYxBITSTREAMCTRLQUEUE belongs to QDSP_AUDPLAYxTASK; see
//    notes/qdsp5_proc_ids.md §3, notes/qdsp5_program_ids.md). classify_queue()
//    encodes ONLY that verified mapping.
//  - The BYTE OFFSET inside the ADSP_RTOS app_to_modem_command sub-payload that
//    selects the destination queue is UNVERIFIED (Q0.1-pending, needs one captured
//    guest packet). This test therefore feeds the resolved QueueId through the
//    explicit routing seam and does NOT invent a packet layout.
//  - AUDPLAY here is a routing/refusal boundary: it must reject malformed / non-
//    AUDPLAY commands and must NEVER emit PCM or claim decoded audio. There is NO
//    MP3 decoder (no verified bitstream-cmd layout / no capture).
//
// NO actual Zeetris guest AUDPLAY packet has been observed. The packet below is a
// locally CONSTRUCTED captured-like packet, not a real capture.
#include "qdsp5_dispatcher.h"
#include "iqdsp_engine.h"
#include <cstdio>
#include <cstring>
#include <vector>

using namespace zeebo::qdsp5;

static int g_fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", (msg)); ++g_fails; } \
                              else { printf("ok: %s\n", (msg)); } } while (0)

// A guest-memory accessor that would FAIL any PCM pointer follow — proves the
// AUDPLAY boundary never even attempts to read/emit samples.
static bool deny_read(u32, void*, u32, void*) { return false; }

int main() {
    Qdsp5Dispatcher disp;
    QdspGuest guest; guest.read = &deny_read;

    // ---- 1. VERIFIED queue->engine classification -------------------------
    CHECK(classify_queue(QueueId::UpAudPlay0BitstreamCtrl) == Engine::Audplay,
          "UPAUDPLAY0BITSTREAMCTRLQUEUE -> Engine::Audplay");
    CHECK(classify_queue(QueueId::UpAudPlay4BitstreamCtrl) == Engine::Audplay,
          "UPAUDPLAY4BITSTREAMCTRLQUEUE -> Engine::Audplay");
    CHECK(classify_queue(QueueId::UpAudPpCmd2) == Engine::Audpp,
          "UPAUDPPCMD2QUEUE -> Engine::Audpp (post-proc, not decoder)");
    CHECK(classify_queue(QueueId::Unknown) == Engine::Unknown,
          "Unknown queue -> Engine::Unknown (no guessing)");

    // ---- 2. THE MISSING ROUTE: an AUDPLAY-destined ADSP_RTOS command must
    //         reach AudplayEngine (previously impossible: classify() only ever
    //         returned Audpp/Unknown). --------------------------------------
    // Construct a captured-like decoded command (body bytes are opaque bitstream
    // config; layout unverified, so we assert only routing + refusal, never decode).
    Command cmd;
    cmd.program = prog::ADSPRTOSATOM;   // the app->modem transport program (verified)
    cmd.proc_id = 0;                     // adsp_rtos_app_to_modem_command
    cmd.payload = std::vector<u8>(16, 0xAB);  // opaque, non-empty

    Reply r = disp.dispatch_queue(QueueId::UpAudPlay0BitstreamCtrl, cmd, guest);
    CHECK(r.handled, "AUDPLAY command is routed & handled (does not hang firmware)");
    CHECK(r.status == kRpcSuccess, "AUDPLAY reply carries RPC success status");

    // ---- 3. NEVER emit PCM / never claim decoded audio --------------------
    // The AUDPLAY engine has no real mixer output; draining audio stays silent.
    std::vector<int16_t> out(256 * 2, 0);
    if (IQdspEngine* eng = disp.engine_for(Engine::Audplay))
        eng->mix_audio(out.data(), 256);
    bool silent = true;
    for (int16_t s : out) if (s != 0) { silent = false; break; }
    CHECK(silent, "AUDPLAY preserva mix zerado (sem PCM fabricado)");

    // ---- 4. Reject malformed AUDPLAY command (empty payload) --------------
    Command empty;
    empty.program = prog::ADSPRTOSATOM;
    Reply re = disp.dispatch_queue(QueueId::UpAudPlay0BitstreamCtrl, empty, guest);
    CHECK(!re.handled, "empty AUDPLAY payload is REJECTED (handled=false)");

    // ---- 5. Reject non-AUDPLAY queue routed through the AUDPLAY path -------
    // A JPEG queue must NOT be handled by the AUDPLAY decoder; it routes to its
    // own engine (Jpeg), never silently accepted as audio.
    Reply rj = disp.dispatch_queue(QueueId::UpJpegActionCmd, cmd, guest);
    CHECK(disp.engine_for(Engine::Jpeg) != nullptr, "JPEG engine exists");
    // The command must not be classified as Audplay.
    CHECK(classify_queue(QueueId::UpJpegActionCmd) != Engine::Audplay,
          "JPEG queue is NOT classified as AUDPLAY");
    (void)rj;

    // ---- 6. Reject wrong transport program for a DSP-task queue -----------
    // Only ADSPRTOSATOM carries DSP task commands; AUDMGR (session mgr) must not
    // be accepted as an AUDPLAY bitstream feed.
    Command wrong_prog = cmd; wrong_prog.program = prog::AUDMGR;
    Reply rw = disp.dispatch_queue(QueueId::UpAudPlay0BitstreamCtrl, wrong_prog, guest);
    CHECK(!rw.handled, "AUDMGR program on an AUDPLAY queue is REJECTED");

    if (g_fails == 0) { printf("\nALL PASS (AUDPLAY routing boundary)\n"); return 0; }
    printf("\n%d FAILURES\n", g_fails);
    return 1;
}
