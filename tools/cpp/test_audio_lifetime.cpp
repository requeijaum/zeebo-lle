// test_audio_lifetime.cpp — DD-QW5/DD-QW6: prova que o mixer suporta produtor e
// consumidor concorrentes e que o reset do sink nao corrompe quem esta mixando.
// Sem SDL: exercita UnifiedAudioSink diretamente, como o par
// (hook do Core1) x (thread de audio do host).
//
// Build: g++ -std=c++23 -O2 -fsanitize=thread,undefined -o test_audio_lifetime test_audio_lifetime.cpp
#include "zeebo_audio_sink.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>

int main() {
    // --- 1. Produtor e consumidor concorrentes sobre o mesmo sink ---------
    {
        UnifiedAudioSink sink(44100);
        std::atomic<bool> stop{false};
        std::atomic<uint64_t> mixes{0};

        std::thread consumer([&] {
            std::vector<int16_t> out(1024 * 2);
            while (!stop.load(std::memory_order_relaxed)) {
                sink.mix_samples(out.data(), 1024);
                mixes.fetch_add(1, std::memory_order_relaxed);
            }
        });

        // O produtor aloca vozes novas o tempo todo: e o push_back que
        // realocava o vetor sob os pes do consumidor.
        std::vector<int16_t> pcm(2048, 1234);
        for (int i = 0; i < 400; ++i) {
            uint32_t v = sink.allocate_voice(22050, 2, 0.5f);
            assert(v != 0);
            sink.submit_pcm(v, pcm.data(), pcm.size());
            if (i % 3 == 0) sink.release_voice(v);
        }

        stop.store(true, std::memory_order_relaxed);
        consumer.join();
        assert(mixes.load() > 0);
        printf("[ok] produtor/consumidor concorrentes: %llu mixagens\n",
               (unsigned long long)mixes.load());
    }

    // --- 2. Reset do sink durante mixagem (audpp faz sink_ = UnifiedAudioSink) ---
    {
        UnifiedAudioSink sink(44100);
        std::vector<int16_t> pcm(4096, 500);
        uint32_t v = sink.allocate_voice(44100, 2, 1.0f);
        sink.submit_pcm(v, pcm.data(), pcm.size());

        std::atomic<bool> stop{false};
        std::thread consumer([&] {
            std::vector<int16_t> out(512 * 2);
            while (!stop.load(std::memory_order_relaxed)) sink.mix_samples(out.data(), 512);
        });
        for (int i = 0; i < 200; ++i) sink = UnifiedAudioSink(44100);
        stop.store(true, std::memory_order_relaxed);
        consumer.join();
        printf("[ok] reset concorrente do sink nao corrompe a mixagem\n");
    }

    // --- 3. Frequencia de saida negociada e respeitada --------------------
    // 22.05 kHz -> 48 kHz: a fonte deve durar mais que o mesmo numero de frames
    // a 22.05 kHz, senao o audio toca acelerado.
    {
        UnifiedAudioSink sink(48000);
        std::vector<int16_t> pcm(2205 * 2, 3000);  // 0,1 s a 22050 Hz, estereo
        uint32_t v = sink.allocate_voice(22050, 2, 1.0f);
        sink.submit_pcm(v, pcm.data(), pcm.size());

        size_t nonzero_frames = 0;
        std::vector<int16_t> out(256 * 2);
        for (int block = 0; block < 64; ++block) {
            sink.mix_samples(out.data(), 256);
            for (size_t i = 0; i < 256; ++i)
                if (out[i * 2] != 0) ++nonzero_frames;
        }
        // 2205 frames de fonte a 48 kHz -> ~4800 frames de saida.
        printf("[info] frames de saida com sinal: %zu (esperado ~4800)\n", nonzero_frames);
        assert(nonzero_frames > 4000 && nonzero_frames < 5200);
        printf("[ok] reamostragem 22050 -> 48000 respeita a taxa de saida\n");
    }

    printf("[Test] audio lifetime/concorrencia/taxa: OK\n");
    return 0;
}
