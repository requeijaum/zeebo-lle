#include "zeebo_audio_sink.h"
#include <cassert>
#include <iostream>

int main() {
    UnifiedAudioSink sink(44100);

    // Voice 1: Sine wave 440Hz stereo
    uint32_t v1 = sink.allocate_voice(44100, 2, 0.8f);
    assert(v1 != 0);

    std::vector<int16_t> samples1(88200); // 1 sec stereo = 44100 frames
    for (size_t i = 0; i < 44100; i++) {
        int16_t s = (int16_t)(10000.0 * std::sin(2.0 * M_PI * 440.0 * i / 44100.0));
        samples1[i * 2] = s;
        samples1[i * 2 + 1] = s;
    }
    sink.submit_pcm(v1, samples1.data(), samples1.size());

    // Mix 512 frames
    std::vector<int16_t> out_buffer(1024, 0);
    sink.mix_samples(out_buffer.data(), 512);

    // Verify non-zero output generated
    bool has_sound = false;
    for (auto val : out_buffer) {
        if (val != 0) {
            has_sound = true;
            break;
        }
    }
    assert(has_sound);

    sink.release_voice(v1);
    std::cout << "[Test] UnifiedAudioSink mixed " << out_buffer.size() << " samples successfully!" << std::endl;
    return 0;
}
