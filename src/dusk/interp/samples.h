#pragma once

#include "dusk/game_clock.h"
#include "dusk/interp/frame_interpolation.h"
#include "dusk/interp/lerp.h"

#include <vector>

namespace dusk::interp {

template <typename T>
class Samples {
public:
    void reset() { m_samples.clear(); }

    template <typename Sample>
    void capture(int count, Sample sample) {
        if (!should_capture()) {
            return;
        }
        if (count <= 0) {
            reset();
            return;
        }

        const uint64_t tick = sim_tick_seq();
        const uint64_t epoch = game_clock::g_frameTiming.presentationEpoch;
        const bool continuous = epoch == m_epoch && (tick == m_tick || tick == m_tick + 1);
        if (!continuous) {
            reset();
        }
        if (count < m_samples.size()) {
            m_samples.erase(m_samples.begin() + count, m_samples.end());
        }
        m_samples.reserve(count);
        for (int i = 0; i < count; ++i) {
            T current = sample(i);
            if (i == m_samples.size()) {
                m_samples.push_back({current, current});
                continue;
            }
            auto& entry = m_samples[i];
            if (tick != m_tick) {
                entry.previous = entry.current;
            }
            entry.current = current;
        }
        m_tick = tick;
        m_epoch = epoch;
    }

    void capture(const T* source, int count) {
        capture(source != nullptr ? count : 0, [&](int i) { return source[i]; });
    }

    T read(int index, const T& current) const {
        if (!valid(index)) {
            return current;
        }
        T result = current;
        const auto& entry = m_samples[index];
        lerp(result, entry.previous, entry.current, get_interpolation_step());
        return result;
    }

    T read(int index, const T& current, f32 snap_distance) const {
        if (!valid(index) ||
            m_samples[index].previous.abs(m_samples[index].current) > snap_distance)
        {
            return current;
        }
        return read(index, current);
    }

private:
    bool valid(int index) const {
        return is_enabled() && is_presentation_active() && m_tick == sim_tick_seq() &&
               m_epoch == game_clock::g_frameTiming.presentationEpoch &&
               index >= 0 && static_cast<size_t>(index) < m_samples.size();
    }

    struct Entry {
        T previous;
        T current;
    };
    std::vector<Entry> m_samples;
    uint64_t m_tick = 0;
    uint64_t m_epoch = 0;
};

namespace detail {
void* acquire(const void* key, const void* type, void* (*make)(), void (*destroy)(void*));
}

template <typename Record>
Record& get(const void* key) {
    static const char token{};
    return *static_cast<Record*>(detail::acquire(
        key, &token, []() -> void* { return new Record; },
        [](void* p) { delete static_cast<Record*>(p); }));
}

void erase_owned_samples(const void* key);
void clear_owned_samples();

}  // namespace dusk::interp
