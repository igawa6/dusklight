#pragma once

#include "JSystem/JParticle/JPAEmitter.h"
#include "JSystem/JParticle/JPAParticle.h"

namespace dusk::interp::particle {

void clear();
void prune();
void reset(const JPABaseParticle*);
void reset(const JPABaseEmitter*);
void reset_child(const JPABaseParticle*, const JPABaseParticle* parent, const JPAEmitterWorkData*);
void capture_birth(const JPABaseParticle*, const JPAEmitterWorkData*);
void capture(const JPABaseParticle*);
void capture(JPABaseEmitter*);

JPABaseParticle* present(JPABaseParticle*, JPABaseParticle& scratch, float& age);
JGeometry::TVec3<f32> position(const JPABaseParticle*);
JGeometry::TVec3<f32> emitter_position(const JPABaseEmitter*, const JGeometry::TVec3<f32>&);
JGeometry::TVec3<f32> emitter_direction(const JPABaseEmitter*, const JGeometry::TVec3<f32>&);
float emitter_age(const JPABaseEmitter*);

struct EmitterVisualState {
    JGeometry::TVec2<f32> scale;
    GXColor prm, env, globalPrm, globalEnv;
    explicit EmitterVisualState(const JPABaseEmitter*);
    void apply(JPABaseEmitter*) const;
};

class EmitterDraw {
public:
    explicit EmitterDraw(JPABaseEmitter*);
    ~EmitterDraw();
    EmitterDraw(const EmitterDraw&) = delete;
    EmitterDraw& operator=(const EmitterDraw&) = delete;

private:
    JPABaseEmitter* m_emitter;
    EmitterVisualState m_saved;
};

}  // namespace dusk::interp::particle
