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

    // Regression: preserve the fractional cursor between callbacks while
    // upsampling 22.05 kHz to 44.1 kHz. The old size_t cursor restarted the
    // half-frame on every callback and repeated sample 0 forever for 1-frame mixes.
    uint32_t v2 = sink.allocate_voice(22050, 1, 1.0f);
    assert(v2 != 0);
    const int16_t low_rate[] = {100, 200, 300};
    sink.submit_pcm(v2, low_rate, 3);
    int16_t one_frame[2]{};
    sink.mix_samples(one_frame, 1);
    assert(one_frame[0] == 100 && one_frame[1] == 100);
    sink.mix_samples(one_frame, 1);
    assert(one_frame[0] == 100 && one_frame[1] == 100);
    sink.mix_samples(one_frame, 1);
    assert(one_frame[0] == 200 && one_frame[1] == 200);

    assert(sink.allocate_voice(0, 2) == 0);
    assert(sink.allocate_voice(44100, 0) == 0);
    assert(sink.allocate_voice(44100, 3) == 0);

    std::cout << "[Test] UnifiedAudioSink mixed and resampled successfully!" << std::endl;
    return 0;
}
