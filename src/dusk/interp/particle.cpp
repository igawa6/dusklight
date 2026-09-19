#include "particle.h"

#include "frame_interpolation.h"
#include "lerp.h"

#include "dusk/game_clock.h"

#include "JSystem/JParticle/JPAChildShape.h"

#include <cmath>
#include <optional>
#include <unordered_map>

namespace dusk::interp::particle {
namespace {
struct ParticleState {
    JGeometry::TVec3<f32> position, localPosition, offsetPosition, velocity, baseAxis;
    float scaleX, scaleY, time, age;
    u16 angle;
    u8 alpha;
    GXColor prm, env;

    explicit ParticleState(const JPABaseParticle* p) : ParticleState(p, p->mPosition) {}

    ParticleState(const JPABaseParticle* p, const JGeometry::TVec3<f32>& pos)
        : position(pos), localPosition(p->mLocalPosition),
          offsetPosition(p->mOffsetPosition), velocity(p->mVelocity), baseAxis(p->mBaseAxis),
          scaleX(p->mParticleScaleX), scaleY(p->mParticleScaleY), time(p->mTime), age(p->mAge),
          angle(p->mRotateAngle), alpha(p->mPrmColorAlphaAnm), prm(p->mPrmClr),
          env{p->mEnvClr.r, p->mEnvClr.g, p->mEnvClr.b, 0} {}
};

struct EmitterState {
    EmitterVisualState visual;
    JGeometry::TVec3<f32> position, direction;
    float age;

    explicit EmitterState(JPABaseEmitter* e) : visual(e), age(e->mTick) {
        e->calcEmitterGlobalPosition(&position);
        JPAEmitterWorkData drawWork;
        drawWork.mpEmtr = e;
        e->pRes->calcWorkData_d(&drawWork);
        direction = drawWork.mGlobalEmtrDir;
    }
};

template <typename T>
struct History {
    T previous, current;
    uint64_t tick, epoch;
};

std::unordered_map<const JPABaseParticle*, History<ParticleState>> s_particles;
std::unordered_map<const JPABaseEmitter*, History<EmitterState>> s_emitters;

// Infuriatingly, some particles are recreated every frame rather than having a real
// lifespan, so it's necessary to track their relationship to be able to do interpolation
struct ReplacementTrack {
    uint64_t generation = 0, tick = 0, epoch = 0;
    std::optional<History<ParticleState>> frames;
};

void capture_replacement(ReplacementTrack& track, History<ParticleState>& h) {
    if (track.frames && track.frames->epoch == h.epoch) {
        const auto& last = *track.frames;
        if (last.tick == h.tick) {
            h.previous = last.previous;
        } else if (last.tick + 1 == h.tick) {
            h.previous = last.current;
        }
    }
    track.frames = h;
    track.tick = h.tick;
    track.epoch = h.epoch;
}

struct ChildBirth {
    const JPABaseParticle* parent;
    uint64_t generation, tick, epoch;
};

std::unordered_map<const JPABaseParticle*, ReplacementTrack> s_childTracks;
std::unordered_map<const JPABaseParticle*, ChildBirth> s_childBirths;
std::unordered_map<const JPABaseEmitter*, ReplacementTrack> s_singleFrameParticles;
uint64_t s_childGeneration = 0;

template <typename Map, typename Key, typename State>
void capture_state(Map& map, Key key, const State& current) {
    const uint64_t tick = sim_tick_seq();
    const uint64_t epoch = game_clock::g_frameTiming.presentationEpoch;
    auto [it, added] = map.try_emplace(key, History<State>{current, current, tick, epoch});
    auto& h = it->second;
    if (!added) {
        if (h.epoch != epoch || (h.tick != tick && h.tick + 1 != tick) || current.age < h.current.age) {
            h.previous = current;
        } else if (h.tick != tick) {
            h.previous = h.current;
        }
        h.current = current;
        h.tick = tick;
        h.epoch = epoch;
    }
}

template <typename Map, typename Key>
const Map::mapped_type* history(const Map& map, Key key) {
    if (!is_enabled() || !is_presentation_active()) {
        return nullptr;
    }
    auto it = map.find(key);
    if (it == map.end() || it->second.tick != sim_tick_seq() ||
        it->second.epoch != game_clock::g_frameTiming.presentationEpoch)
    {
        return nullptr;
    }
    return &it->second;
}

u16 blend_angle(u16 a, u16 b, float t) {
    int delta = (static_cast<int>(b) - a + 32768) & 65535;
    delta -= 32768;
    return static_cast<u16>(static_cast<int>(a) + std::lround(delta * t));
}

template <typename Map>
void prune_map(Map& map) {
    for (auto it = map.begin(); it != map.end();) {
        if (it->second.epoch != game_clock::g_frameTiming.presentationEpoch ||
            it->second.tick + 1 < sim_tick_seq())
        {
            it = map.erase(it);
        } else {
            ++it;
        }
    }
}
}  // namespace

void clear() {
    s_particles.clear();
    s_emitters.clear();
    s_childTracks.clear();
    s_childBirths.clear();
    s_singleFrameParticles.clear();
}

void prune() {
    prune_map(s_particles);
    prune_map(s_emitters);
    prune_map(s_childTracks);
    prune_map(s_childBirths);
    prune_map(s_singleFrameParticles);
}

void reset(const JPABaseParticle* p) {
    s_particles.erase(p);
    s_childTracks.erase(p);
    s_childBirths.erase(p);
}

void reset_child(const JPABaseParticle* p, const JPABaseParticle* parent,
    const JPAEmitterWorkData* work) {
    reset(p);
    const auto* shape = work->mpRes->getCsp();
    if (!should_capture() || shape->getLife() != 1 || shape->getRate() != 1 || shape->getStep() != 0) {
        return;
    }
    const uint64_t tick = sim_tick_seq();
    const uint64_t epoch = game_clock::g_frameTiming.presentationEpoch;
    auto [it, added] = s_childTracks.try_emplace(parent, ReplacementTrack{0, tick, epoch, std::nullopt});
    if (added) {
        it->second.generation = ++s_childGeneration;
    }
    s_childBirths.insert_or_assign(p, ChildBirth{parent, it->second.generation, tick, epoch});
}

void reset(const JPABaseEmitter* e) {
    s_emitters.erase(e);
    s_singleFrameParticles.erase(e);
}

void capture_birth(const JPABaseParticle* p, const JPAEmitterWorkData* work) {
    if (!should_capture() || p->mAge != 0 || s_particles.contains(p)) {
        return;
    }
    const JGeometry::TVec3 position(p->mOffsetPosition.x + p->mLocalPosition.x * work->mPublicScale.x,
                                    p->mOffsetPosition.y + p->mLocalPosition.y * work->mPublicScale.y,
                                    p->mOffsetPosition.z + p->mLocalPosition.z * work->mPublicScale.z);
    capture_state(s_particles, p, ParticleState(p, position));
}

void capture(const JPABaseParticle* p) {
    if (should_capture() && p->mAge >= 0) {
        capture_state(s_particles, p, ParticleState(p));
        if (auto birth = s_childBirths.find(p); birth != s_childBirths.end()) {
            auto track = s_childTracks.find(birth->second.parent);
            auto& h = s_particles.at(p);
            if (p->mAge == 0 && birth->second.tick == h.tick && birth->second.epoch == h.epoch &&
                track != s_childTracks.end() && track->second.generation == birth->second.generation)
            {
                capture_replacement(track->second, h);
            }
            s_childBirths.erase(birth);
        }
    }
}

void capture(JPABaseEmitter* e) {
    if (should_capture()) {
        capture_state(s_emitters, e, EmitterState(e));

        const auto* dynamics = e->pRes->getDyn();
        if (e->mLifeTime == 1 && e->mRate == 1.0f && e->mRateStep == 0 &&
            dynamics->getRateRndm() == 0.0f && dynamics->getLifetimeRndm() == 0.0f &&
            e->mAlivePtclBase.getNum() == 1)
        {
            const auto* particle = e->mAlivePtclBase.getFirst()->getObject();
            if (particle->mAge == 0 && particle->mLifeTime == 1) {
                auto& track = s_singleFrameParticles[e];
                capture_state(s_particles, particle, ParticleState(particle));
                auto& h = s_particles.at(particle);
                capture_replacement(track, h);
                return;
            }
        }
        s_singleFrameParticles.erase(e);
    }
}

JPABaseParticle* present(JPABaseParticle* p, JPABaseParticle& scratch, float& age) {
    age = p->mAge;
    if (!game_clock::is_presentation_frame()) {
        return p;
    }
    scratch = *p;
    const auto* h = history(s_particles, p);
    const float t = get_interpolation_step();
    if (h == nullptr || t >= 1.0f) {
        return &scratch;
    }
    const auto& a = h->previous;
    const auto& b = h->current;
    scratch.mPosition = lerp(a.position, b.position, t);
    scratch.mLocalPosition = lerp(a.localPosition, b.localPosition, t);
    scratch.mOffsetPosition = lerp(a.offsetPosition, b.offsetPosition, t);
    scratch.mVelocity = lerp(a.velocity, b.velocity, t);
    scratch.mBaseAxis = lerp(a.baseAxis, b.baseAxis, t);
    if (scratch.mBaseAxis.isZero()) {
        scratch.mBaseAxis = b.baseAxis;
    }
    scratch.mParticleScaleX = lerp(a.scaleX, b.scaleX, t);
    scratch.mParticleScaleY = lerp(a.scaleY, b.scaleY, t);
    scratch.mRotateAngle = blend_angle(a.angle, b.angle, t);
    scratch.mPrmColorAlphaAnm = lerp(a.alpha, b.alpha, t);
    scratch.mPrmClr = lerp(a.prm, b.prm, t);
    const GXColor env = lerp(a.env, b.env, t);
    scratch.mEnvClr.r = env.r;
    scratch.mEnvClr.g = env.g;
    scratch.mEnvClr.b = env.b;
    scratch.mTime = lerp(a.time, b.time, t);
    age = lerp(a.age, b.age, t);
    return &scratch;
}

JGeometry::TVec3<f32> position(const JPABaseParticle* p) {
    const auto* h = history(s_particles, p);
    const float t = get_interpolation_step();
    if (h != nullptr && t < 1.0f) {
        return lerp(h->previous.position, h->current.position, t);
    }
    return p->mPosition;
}

JGeometry::TVec3<f32> emitter_position(const JPABaseEmitter* e, const JGeometry::TVec3<f32>& current) {
    const auto* h = history(s_emitters, e);
    const float t = get_interpolation_step();
    if (h != nullptr && t < 1.0f) {
        return lerp(h->previous.position, h->current.position, t);
    }
    return current;
}

JGeometry::TVec3<f32> emitter_direction(const JPABaseEmitter* e, const JGeometry::TVec3<f32>& current) {
    const auto* h = history(s_emitters, e);
    const float t = get_interpolation_step();
    if (h != nullptr && t < 1.0f) {
        return lerp(h->previous.direction, h->current.direction, t);
    }
    return current;
}

float emitter_age(const JPABaseEmitter* e) {
    const auto* h = history(s_emitters, e);
    const float t = get_interpolation_step();
    if (h != nullptr && t < 1.0f) {
        return lerp(h->previous.age, h->current.age, t);
    }
    return e->mTick;
}

EmitterVisualState::EmitterVisualState(const JPABaseEmitter* e)
    : scale(e->mGlobalPScl), prm(e->mPrmClr), env(e->mEnvClr), globalPrm(e->mGlobalPrmClr),
      globalEnv(e->mGlobalEnvClr) {}

void EmitterVisualState::apply(JPABaseEmitter* e) const {
    e->mGlobalPScl = scale;
    e->mPrmClr = prm;
    e->mEnvClr = env;
    e->mGlobalPrmClr = globalPrm;
    e->mGlobalEnvClr = globalEnv;
}

EmitterDraw::EmitterDraw(JPABaseEmitter* e) : m_emitter(nullptr), m_saved(e) {
    if (!game_clock::is_presentation_frame()) {
        return;
    }
    m_emitter = e;
    const auto* h = history(s_emitters, e);
    const float t = get_interpolation_step();
    if (h == nullptr || t >= 1.0f) {
        return;
    }
    const auto& a = h->previous.visual;
    const auto& b = h->current.visual;
    EmitterVisualState visual(e);
    visual.scale.set(lerp(a.scale.x, b.scale.x, t), lerp(a.scale.y, b.scale.y, t));
    visual.prm = lerp(a.prm, b.prm, t);
    visual.env = lerp(a.env, b.env, t);
    visual.globalPrm = lerp(a.globalPrm, b.globalPrm, t);
    visual.globalEnv = lerp(a.globalEnv, b.globalEnv, t);
    visual.apply(e);
}

EmitterDraw::~EmitterDraw() {
    if (m_emitter != nullptr) {
        m_saved.apply(m_emitter);
    }
}

}  // namespace dusk::interp::particle
