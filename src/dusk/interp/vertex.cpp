#include "vertex.h"

#include "frame_interpolation.h"
#include "dusk/game_clock.h"
#include "JSystem/J3DGraphBase/J3DVertex.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace dusk::interp::vertex {
namespace {

struct Array {
    std::vector<Vec> previous;
    std::vector<Vec> current;
    std::vector<Vec> presentation;
    const void* source = nullptr;
    float cached_step = -1.0f;

    void capture(const void* data, u32 count, bool continuous, bool same_tick) {
        source = data;
        cached_step = -1.0f;
        if (data == nullptr || count == 0) {
            *this = {};
            return;
        }
        continuous &= current.size() == count;
        if (continuous && !same_tick) previous.swap(current);
        const auto* values = static_cast<const Vec*>(data);
        current.assign(values, values + count);
        if (!continuous) previous = current;
    }

    void* read(void* requested, float step) {
        if (requested != source || current.empty() || step >= 1.0f) return requested;
        if (step <= 0.0f) return previous.data();
        if (step != cached_step) {
            presentation.resize(current.size());
            for (size_t i = 0; i < current.size(); ++i) {
                const auto& a = previous[i];
                const auto& b = current[i];
                presentation[i] = {a.x + (b.x - a.x) * step,
                                   a.y + (b.y - a.y) * step,
                                   a.z + (b.z - a.z) * step};
            }
            cached_step = step;
        }
        return presentation.data();
    }
};

struct Record {
    const J3DDeformData* deformation = nullptr;
    const J3DVertexData* layout = nullptr;
    uint64_t tick = 0;
    uint64_t epoch = 0;
    Array positions;
    Array normals;
};

auto& records() {
    static std::unordered_map<const J3DVertexBuffer*, Record> stored;
    return stored;
}

void* read(const J3DVertexBuffer* buffer, void* current, Array Record::* array) {
    if (!is_enabled() || !is_presentation_active()) return current;
    auto it = records().find(buffer);
    if (it == records().end() || it->second.tick != sim_tick_seq() ||
        it->second.epoch != game_clock::g_frameTiming.presentationEpoch)
    {
        return current;
    }
    return (it->second.*array).read(current, get_interpolation_step());
}

}  // namespace

void capture(J3DVertexBuffer* buffer, const J3DDeformData* deformation) {
    if (!should_capture() || is_presentation_active()) return;
    if (deformation == nullptr) {
        reset(buffer);
        return;
    }
    auto* layout = buffer->getVertexData();
    void* positions = buffer->getCurrentVtxPos();
    void* normals = buffer->getCurrentVtxNrm();
    if (layout->getVtxNum() == 0 || layout->getVtxPosType() != GX_F32 ||
        positions == layout->getVtxPosArray()) positions = nullptr;
    if (layout->getNrmNum() == 0 || layout->getVtxNrmType() != GX_F32 ||
        normals == layout->getVtxNrmArray()) normals = nullptr;
    if (positions == nullptr && normals == nullptr) {
        reset(buffer);
        return;
    }

    auto& record = records()[buffer];
    const uint64_t tick = sim_tick_seq();
    const uint64_t epoch = game_clock::g_frameTiming.presentationEpoch;
    const bool same_tick = record.tick == tick;
    const bool continuous = record.deformation == deformation && record.layout == layout &&
        record.epoch == epoch && (same_tick || record.tick + 1 == tick);

    record.positions.capture(positions, layout->getVtxNum(), continuous, same_tick);
    record.normals.capture(normals, layout->getNrmNum(), continuous, same_tick);
    record.deformation = deformation;
    record.layout = layout;
    record.tick = tick;
    record.epoch = epoch;
}

void* positions(const J3DVertexBuffer* buffer, void* current) { return read(buffer, current, &Record::positions); }
void* normals(const J3DVertexBuffer* buffer, void* current) { return read(buffer, current, &Record::normals); }
void reset(const J3DVertexBuffer* buffer) { records().erase(buffer); }

void invalidate(const J3DDeformData* deformation) {
    std::erase_if(records(), [=](const auto& entry) { return entry.second.deformation == deformation; });
}

void prune() {
    const uint64_t tick = sim_tick_seq();
    const uint64_t epoch = game_clock::g_frameTiming.presentationEpoch;
    std::erase_if(records(), [=](const auto& entry) {
        return entry.second.epoch != epoch || entry.second.tick < tick - (tick != 0);
    });
}

void clear() { records().clear(); }

}  // namespace dusk::interp::vertex
