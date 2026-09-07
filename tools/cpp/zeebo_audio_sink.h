#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <memory>
#include <algorithm>
#include <cmath>
#include <iostream>

// Multi-stream audio mixer & PCM sink inspired by HLE patterns (Infuse / Zeebulator / Zeemu)
// Adapts HLE PCM/MIDI voice concepts to LLE QDSP5 / Audio DMA streams
class UnifiedAudioSink {
public:
    struct StreamVoice {
        uint32_t id{0};
        bool active{false};
        uint32_t sample_rate{44100};
        uint32_t channels{2};
        float volume{1.0f};
        std::vector<int16_t> pcm_data;
        size_t playback_pos{0};
    };

    UnifiedAudioSink(uint32_t output_sample_rate = 44100)
        : out_rate_(output_sample_rate) {}

    // Channel allocation and PCM streaming
    uint32_t allocate_voice(uint32_t sample_rate = 44100, uint32_t channels = 2, float volume = 1.0f) {
        for (size_t i = 0; i < voices_.size(); i++) {
            if (!voices_[i].active) {
                voices_[i].id = (uint32_t)(i + 1);
                voices_[i].active = true;
                voices_[i].sample_rate = sample_rate;
                voices_[i].channels = channels;
                voices_[i].volume = volume;
                voices_[i].pcm_data.clear();
                voices_[i].playback_pos = 0;
                return voices_[i].id;
            }
        }
        VoiceSlot new_slot{};
        new_slot.id = (uint32_t)(voices_.size() + 1);
        new_slot.active = true;
        new_slot.sample_rate = sample_rate;
        new_slot.channels = channels;
        new_slot.volume = volume;
        voices_.push_back(new_slot);
        return new_slot.id;
    }

    void submit_pcm(uint32_t voice_id, const int16_t* samples, size_t count) {
        VoiceSlot* v = find_voice(voice_id);
        if (!v || !v->active) return;
        v->pcm_data.insert(v->pcm_data.end(), samples, samples + count);
    }

    void release_voice(uint32_t voice_id) {
        VoiceSlot* v = find_voice(voice_id);
        if (v) {
            v->active = false;
            v->pcm_data.clear();
            v->playback_pos = 0;
        }
    }

    // Mix active voices into interleaved stereo 16-bit PCM buffer
    void mix_samples(int16_t* out_buffer, size_t num_frames) {
        if (!out_buffer || num_frames == 0) return;

        std::vector<float> mix_l(num_frames, 0.0f);
        std::vector<float> mix_r(num_frames, 0.0f);

        for (auto& v : voices_) {
            if (!v.active || v.pcm_data.empty()) continue;

            float step = (float)v.sample_rate / (float)out_rate_;
            size_t available_frames = (v.channels == 2) ? (v.pcm_data.size() / 2) : v.pcm_data.size();

            for (size_t i = 0; i < num_frames; i++) {
                size_t src_frame = (size_t)(v.playback_pos + i * step);
                if (src_frame >= available_frames) {
                    v.active = false; // finished stream
                    break;
                }

                float sample_l = 0.0f;
                float sample_r = 0.0f;

                if (v.channels == 2) {
                    sample_l = v.pcm_data[src_frame * 2] * v.volume;
                    sample_r = v.pcm_data[src_frame * 2 + 1] * v.volume;
                } else {
                    float s = v.pcm_data[src_frame] * v.volume;
                    sample_l = s;
                    sample_r = s;
                }

                mix_l[i] += sample_l;
                mix_r[i] += sample_r;
            }

            v.playback_pos += (size_t)(num_frames * step);
        }

        // Clamp to int16 range
        for (size_t i = 0; i < num_frames; i++) {
            float l = std::clamp(mix_l[i], -32768.0f, 32767.0f);
            float r = std::clamp(mix_r[i], -32768.0f, 32767.0f);
            out_buffer[i * 2] = (int16_t)l;
            out_buffer[i * 2 + 1] = (int16_t)r;
        }
    }

private:
    using VoiceSlot = StreamVoice;
    std::vector<VoiceSlot> voices_;
    uint32_t out_rate_{44100};

    VoiceSlot* find_voice(uint32_t id) {
        for (auto& v : voices_) {
            if (v.id == id) return &v;
        }
        return nullptr;
    }
};
