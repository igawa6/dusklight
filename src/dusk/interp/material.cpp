#include "material.h"

#include "frame_interpolation.h"

#include "dusk/game_clock.h"

#include "d/d_com_inf_game.h"
#include "m_Do/m_Do_graphic.h"

#include "JSystem/J3DGraphAnimator/J3DAnimation.h"
#include "JSystem/J3DGraphAnimator/J3DModel.h"
#include "JSystem/J3DGraphAnimator/J3DModelData.h"
#include "JSystem/J3DGraphBase/J3DMaterial.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace dusk::interp::material {

namespace {
struct ViewProjection {
    f32 scaleS, scaleT, transS, transT;

    void apply(J3DTexMtxInfo* info) const {
        Mtx projection;
        const auto* view = dComIfGd_getView();
        C_MTXLightPerspective(projection, view->fovy, view->aspect, scaleS, scaleT, transS, transT);
#if WIDESCREEN_SUPPORT
        mDoGph_gInf_c::setWideZoomLightProjection(projection);
#endif
        info->setEffectMtx(projection);
    }
};

struct LightView {
    Mtx inverse;

    void apply(J3DLightInfo* light, bool rotateDirection) const {
        Mtx changeOfView;
        MTXConcat(j3dSys.getViewMtx(), inverse, changeOfView);
        MTXMultVec(changeOfView, &light->mLightPosition, &light->mLightPosition);
        if (rotateDirection) {
            MTXMultVecSR(changeOfView, &light->mLightDirection, &light->mLightDirection);
        }
    }
};

struct Tables {
    std::unordered_map<const J3DFrameCtrl*, Frames> controllers;
    std::unordered_map<const J3DAnmBase*, Frames> resources;
    std::unordered_map<J3DTexMtxInfo*, ViewProjection> projections;
    std::unordered_map<J3DMaterial*, LightView> lightViews;
};

Tables& tables() {
    static Tables stored;
    return stored;
}

template <typename Map, typename Key>
Frames lookup(const Map& map, Key key) {
    auto it = map.find(key);
    return it == map.end() ? Frames{} : it->second;
}

f32 sample_texture_translation(const J3DAnmKeyTableBase& table, const BE(f32)* values, f32 frame, f32 evaluated) {
    if (table.mMaxFrame < 2 || table.mType != 1) {
        return evaluated;
    }

    const auto* keys = values + table.mOffset;
    for (u16 i = 1; i < table.mMaxFrame; ++i) {
        const auto* left = keys + (i - 1) * 4;
        const auto* right = left + 4;
        if ((f32)left[0] >= frame) {
            break;
        }
        if ((f32)right[0] <= frame || (f32)right[0] - (f32)left[0] != 1.0f) {
            continue;
        }
        const f32 slope = left[2];
        const f32 jump = (f32)right[1] - (f32)left[1] - slope;
        const f32 tiles = std::round(jump);
        if (std::abs((f32)right[3] - slope) < 0.0001f && std::abs(tiles) >= 1.0f &&
            std::abs(jump - tiles) < 0.0001f)
        {
            return (f32)left[1] + slope * (frame - (f32)left[0]);
        }
    }

    return evaluated;
}
}  // namespace

Frames get_frames(const J3DFrameCtrl* controller) {
    return lookup(tables().controllers, controller);
}

Frames get_frames(const J3DAnmBase* resource) {
    return lookup(tables().resources, resource);
}

void reset(const J3DFrameCtrl* controller) {
    tables().controllers.erase(controller);
}

void reset(const J3DAnmBase* resource) {
    tables().resources.erase(resource);
}

void bind(J3DAnmBase* resource, const J3DFrameCtrl* controller) {
    Frames frames = get_frames(controller);
    if (frames.valid && frames.tick == sim_tick_seq() &&
        frames.epoch == game_clock::g_frameTiming.presentationEpoch)
    {
        tables().resources[resource] = frames;
    } else {
        reset(resource);
    }
}

void swap_frames(J3DAnmBase* resource, Frames& frames) {
    Frames saved = get_frames(resource);
    if (frames.valid) {
        tables().resources[resource] = frames;
    } else {
        reset(resource);
    }
    frames = saved;
}

void clear() {
    tables().controllers.clear();
    tables().resources.clear();
    tables().projections.clear();
    tables().lightViews.clear();
}

void prune() {
    tables().projections.clear();
    tables().lightViews.clear();
    const uint64_t tick = sim_tick_seq();
    const uint64_t epoch = game_clock::g_frameTiming.presentationEpoch;
    auto stale = [=](const auto& entry) {
        return entry.second.epoch != epoch || entry.second.tick < tick - (tick != 0);
    };
    std::erase_if(tables().controllers, stale);
    std::erase_if(tables().resources, stale);
}

f32 Frames::read(f32 requested) const {
    if (!valid || !smooth || !is_enabled() || !is_presentation_active() || tick != sim_tick_seq() ||
        epoch != game_clock::g_frameTiming.presentationEpoch || requested != current)
    {
        return requested;
    }

    const f32 step = get_interpolation_step();
    if (step <= 0.0f) {
        return previous;
    }
    if (step >= 1.0f) {
        return current;
    }

    f32 frame = previous + (loop ? travel : current - previous) * step;
    if (loop && (frame < loopStart || frame >= loopEnd)) {
        if ((travel > 0.0f && frame >= loopEnd) || (travel < 0.0f && frame < loopStart)) {
            const f32 length = loopEnd - loopStart;
            frame = loopStart + std::fmod(frame - loopStart, length);
            if (frame < loopStart) {
                frame += length;
            }
        }
    }

    return frame;
}

Update::Update(J3DFrameCtrl& controller) : m_controller(controller), m_before(controller.getFrame()), m_continuous(true) {
    m_travel = controller.getRate();
    const f32 next = m_before + m_travel;
    const f32 start = controller.getStart();
    const f32 end = controller.getEnd();
    m_continuous = std::isfinite(m_before) && std::isfinite(next) && end > start;
    switch (controller.getAttribute()) {
    case J3DFrameCtrl::EMode_NONE:
        break;
    case J3DFrameCtrl::EMode_RESET:
        m_continuous &= next < end;
        break;
    case J3DFrameCtrl::EMode_LOOP:
        m_continuous &= end - start > 1.0f;
        m_loopStart = m_travel < 0.0f ? start : controller.getLoop();
        m_loopEnd = m_travel < 0.0f ? controller.getLoop() : end;
        if (m_loopEnd > m_loopStart && controller.getLoop() >= start && controller.getLoop() <= end) {
            m_continuous &= m_before >= start && m_before < end;
        } else {
            m_loopStart = m_loopEnd = 0.0f;
            m_continuous &= next >= start && next < end;
        }
        break;
    case J3DFrameCtrl::EMode_REVERSE:
        m_continuous &= next >= start && next < end;
        break;
    case J3DFrameCtrl::EMode_LOOP_REVERSE:
        m_continuous &= next >= start && next < end - 1.0f;
        break;
    default:
        m_continuous = false;
        break;
    }
}

Update::~Update() {
    auto& stored = tables().controllers;
    Frames frames = get_frames(&m_controller);
    if (!should_capture() || is_presentation_active()) {
        reset(&m_controller);
        return;
    }
    const uint64_t tick = sim_tick_seq();
    const uint64_t epoch = game_clock::g_frameTiming.presentationEpoch;
    const bool same_tick = frames.tick == tick && frames.epoch == epoch && frames.valid;
    const f32 current = m_controller.getFrame();
    const bool loop = m_loopEnd > m_loopStart;
    const bool compatible = !same_tick ||
        (frames.loop == loop && (!loop || (frames.loopStart == m_loopStart &&
         frames.loopEnd == m_loopEnd && frames.travel * m_travel >= 0.0f)));
    const bool continuous = compatible && m_continuous && std::isfinite(current) &&
                            (!same_tick || (frames.smooth && frames.current == m_before));
    const f32 previous = same_tick ? frames.previous : m_before;
    Frames captured{continuous ? previous : current, current, tick, epoch, true, continuous};
    captured.loop = continuous && loop;
    captured.travel = same_tick ? frames.travel + m_travel : m_travel;
    captured.loopStart = m_loopStart;
    captured.loopEnd = m_loopEnd;
    stored[&m_controller] = captured;
}

Sample::Sample(J3DAnmBase* animation) : m_animation(animation), m_frame(animation->getFrame()) {
    animation->mFrame = get_frames(animation).read(m_frame);
}

Sample::~Sample() {
    m_animation->mFrame = m_frame;
}

void sample_texture(const J3DAnmTextureSRTKey* animation, u16 track, J3DTextureSRTInfo* result) {
    const Frames frames = get_frames(animation);
    const f32 current = animation->getFrame();
    const f32 sampled = frames.read(current);
    animation->calcTransform(sampled, track, result);

    if (!is_enabled() || !is_presentation_active() || sampled == current) {
        return;
    }

    const auto* table = animation->mAnmTable + track * 3;
    result->mTranslationX = sample_texture_translation(table[0].mTranslateInfo, animation->mTransData,
                                                       sampled, result->mTranslationX);
    result->mTranslationY = sample_texture_translation(table[1].mTranslateInfo, animation->mTransData,
                                                       sampled, result->mTranslationY);
}

namespace {
template <typename T>
struct Values {
    struct Entry {
        T* target;
        T captured;
        T saved;
    };
    std::vector<Entry> entries;

    bool capture(T* target) {
        if (target == nullptr || std::any_of(entries.begin(), entries.end(),
            [=](const Entry& entry) {
                return entry.target == target;
            }))
        {
            return false;
        }
        entries.push_back({target, *target, *target});
        return true;
    }

    void apply() {
        for (auto& entry : entries) {
            entry.saved = *entry.target;
            *entry.target = entry.captured;
        }
    }

    void restore() {
        for (auto& entry : entries) {
            *entry.target = entry.saved;
        }
    }
};

template <typename Color>
bool same_color(const Color& a, const Color& b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}
}  // namespace

struct ModelBindings::State {
    std::tuple<Values<J3DMaterial>,
        Values<J3DColorBlockLightOff>, Values<J3DColorBlockAmbientOn>, Values<J3DColorBlockLightOn>,
        Values<J3DTexGenBlockPatched>, Values<J3DTexGenBlock4>, Values<J3DTexGenBlockBasic>,
        Values<J3DTevBlockPatched>, Values<J3DTevBlock1>, Values<J3DTevBlock2>,
        Values<J3DTevBlock4>, Values<J3DTevBlock16>, Values<J3DIndBlockFull>,
        Values<J3DPEBlockFogOff>, Values<J3DPEBlockFull>, Values<J3DTexMtx>,
        Values<J3DLightInfo>, Values<GXLightObj>, Values<u16>> values;

    struct Animation {
        J3DMaterial* material;
        J3DMaterialAnm tracks;
    };

    struct Resource {
        J3DAnmBase* target;
        f32 frame;
        Frames frames;
        f32 savedFrame;
        Frames savedFrames;
    };

    struct Projection {
        J3DTexMtxInfo* target;
        ViewProjection recipe;
    };

    struct Light {
        J3DLightInfo* target;
        LightView view;
        bool rotateDirection;
    };

    std::vector<Animation> animations;
    std::vector<Resource> resources;
    std::vector<Projection> projections;
    std::vector<Light> lights;

    template <typename T>
    bool capture(T* target) {
        return std::get<Values<T>>(values).capture(target);
    }
};

ModelBindings::ModelBindings() : m_state(std::make_unique<State>()) {}
ModelBindings::~ModelBindings() = default;

void ModelBindings::capture(J3DMaterial* material) {
    auto& state = *m_state;
    if (!state.capture(material)) {
        return;
    }

    auto* color = material->getColorBlock();
    switch (color->getType()) {
    case 'CLOF': state.capture(static_cast<J3DColorBlockLightOff*>(color)); break;
    case 'CLAB': state.capture(static_cast<J3DColorBlockAmbientOn*>(color)); break;
    case 'CLON': state.capture(static_cast<J3DColorBlockLightOn*>(color)); break;
    }

    auto* texgen = material->getTexGenBlock();
    switch (texgen->getType()) {
    case 'TGPT': state.capture(static_cast<J3DTexGenBlockPatched*>(texgen)); break;
    case 'TGB4': state.capture(static_cast<J3DTexGenBlock4*>(texgen)); break;
    case 'TGBC': state.capture(static_cast<J3DTexGenBlockBasic*>(texgen)); break;
    }

    auto* tev = material->getTevBlock();
    switch (tev->getType()) {
    case 'TVPT': state.capture(static_cast<J3DTevBlockPatched*>(tev)); break;
    case 'TVB1': state.capture(static_cast<J3DTevBlock1*>(tev)); break;
    case 'TVB2': state.capture(static_cast<J3DTevBlock2*>(tev)); break;
    case 'TVB4': state.capture(static_cast<J3DTevBlock4*>(tev)); break;
    case 'TV16': state.capture(static_cast<J3DTevBlock16*>(tev)); break;
    }

    auto* indirect = material->getIndBlock();
    if (indirect->getType() == 'IBLF') {
        state.capture(static_cast<J3DIndBlockFull*>(indirect));
    }

    auto* pe = material->getPEBlock();
    switch (pe->getType()) {
    case 'PEFG': state.capture(static_cast<J3DPEBlockFogOff*>(pe)); break;
    case 'PEFL': state.capture(static_cast<J3DPEBlockFull*>(pe)); break;
    }

    for (int i = 0; i < 8; ++i) {
        if (auto* light = color->getLight(i)) {
            if (state.capture(light->getLightInfo())) {
                if (auto it = tables().lightViews.find(material); i != 1 && it != tables().lightViews.end()) {
                    state.lights.push_back({light->getLightInfo(), it->second, i >= 2});
                }
            }
            state.capture(&light->mLightObj);
        }
        if (auto* coord = texgen->getTexCoord(i)) {
            state.capture(&coord->mTexMtxReg);
        }
        if (auto* matrix = texgen->getTexMtx(i); state.capture(matrix)) {
            auto* info = &matrix->getTexMtxInfo();
            if (auto it = tables().projections.find(info); it != tables().projections.end()) {
                state.projections.push_back({info, it->second});
            }
        }
    }

    auto* animation = material->getMaterialAnm();
    if (animation == nullptr) {
        return;
    }

    auto& tracks = state.animations.emplace_back(State::Animation{material, *animation}).tracks;
    for (int i = 0; i < 2; ++i) {
        const auto& track = tracks.getMatColorAnm(i);
        if (!track.getAnmFlag()) {
            continue;
        }
        capture(track.getAnimation());
        GXColor evaluated;
        track.calc(&evaluated);
        if (!same_color<GXColor>(*material->getMatColor(i), evaluated)) {
            tracks.setMatColorAnm(i, nullptr);
        }
    }
    for (int i = 0; i < 8; ++i) {
        const auto& track = tracks.getTexNoAnm(i);
        if (!track.getAnmFlag()) {
            continue;
        }
        capture(track.getAnimation());
        u16 evaluated;
        track.calc(&evaluated);
        if (material->getTexNo(i) != evaluated) {
            tracks.setTexNoAnm(i, nullptr);
        }
    }
    for (int i = 0; i < 8; ++i) {
        const auto& track = tracks.getTexMtxAnm(i);
        if (!track.getAnmFlag()) {
            continue;
        }
        capture(track.getAnimation());
        J3DTextureSRTInfo evaluated;
        track.calc(&evaluated);
        if (!material->getTexMtx(i)->getTexMtxInfo().mSRT.operator==(evaluated)) {
            tracks.setTexMtxAnm(i, nullptr);
        }
    }
    for (int i = 0; i < 3; ++i) {
        const auto& track = tracks.getTevColorAnm(i);
        if (!track.getAnmFlag()) {
            continue;
        }
        capture(track.getAnimation());
        GXColorS10 evaluated;
        track.calc(&evaluated);
        if (!same_color<GXColorS10>(*material->getTevColor(i), evaluated)) {
            tracks.setTevColorAnm(i, nullptr);
        }
    }
    for (int i = 0; i < 4; ++i) {
        const auto& track = tracks.getTevKColorAnm(i);
        if (!track.getAnmFlag()) {
            continue;
        }
        capture(track.getAnimation());
        GXColor evaluated;
        track.calc(&evaluated);
        if (!same_color<GXColor>(*material->getTevKColor(i), evaluated)) {
            tracks.setTevKColorAnm(i, nullptr);
        }
    }
}

void ModelBindings::capture(J3DAnmBase* resource) {
    if (resource == nullptr) {
        return;
    }

    for (const auto& entry : m_state->resources) {
        if (entry.target == resource) {
            return;
        }
    }

    m_state->resources.push_back({resource, resource->getFrame(), get_frames(resource)});
}

void ModelBindings::apply() {
    std::apply([](auto&... values) { (values.apply(), ...); }, m_state->values);

    for (auto& entry : m_state->animations) {
        entry.material->setMaterialAnm(&entry.tracks);
    }

    for (auto& entry : m_state->resources) {
        entry.savedFrame = entry.target->mFrame;
        entry.target->mFrame = entry.frame;
        entry.savedFrames = entry.frames;
        swap_frames(entry.target, entry.savedFrames);
    }

    for (const auto& entry : m_state->projections) {
        entry.recipe.apply(entry.target);
    }

    for (const auto& entry : m_state->lights) {
        entry.view.apply(entry.target, entry.rotateDirection);
    }
}

void ModelBindings::restore() {
    for (auto& entry : m_state->resources) {
        entry.target->mFrame = entry.savedFrame;
        swap_frames(entry.target, entry.savedFrames);
    }

    std::apply([](auto&... values) { (values.restore(), ...); }, m_state->values);
}

void record_model(J3DModel* model) {
    if (!should_capture() || is_presentation_active()) {
        return;
    }

    struct Recording {
        J3DModel* model;
        ModelBindings bindings;
    };

    auto recording = std::make_shared<Recording>();
    recording->model = model;
    J3DModelData* data = model->getModelData();
    for (u16 i = 0; i < data->getMaterialNum(); ++i) {
        recording->bindings.capture(data->getMaterialNodePointer(i));
    }

    add_interpolation_callback([](void* work) {
        auto& recording = *static_cast<Recording*>(work);
        ModelScope scope(recording.bindings);
        recording.model->calcMaterial();
        recording.model->diff();
    }, recording.get(), recording);
}

void set_view_projection(J3DTexMtxInfo* info, f32 scaleS, f32 scaleT, f32 transS, f32 transT) {
    ViewProjection recipe{scaleS, scaleT, transS, transT};
    recipe.apply(info);
    if (should_capture() && !is_presentation_active()) {
        tables().projections.insert_or_assign(info, recipe);
    }
}

void record_light_view(J3DMaterial* material) {
    if (!should_capture() || is_presentation_active()) {
        return;
    }

    LightView view;
    if (MTXInverse(j3dSys.getViewMtx(), view.inverse)) {
        tables().lightViews.insert_or_assign(material, view);
    }
}

}  // namespace dusk::interp::material
