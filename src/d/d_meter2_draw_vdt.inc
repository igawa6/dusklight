#include "dusk/game_clock.h"
#include "dusk/interp/frame_interpolation.h"
#include "dusk/interp/user_interface.h"

#include "d/d_com_inf_game.h"
#include "d/d_meter2_info.h"
#include "d/d_meter_HIO.h"
#include "d/d_meter_map.h"
#include "d/d_pane_class.h"

namespace {

struct FadeRow {
    bool used;
    bool show;
    bool follow;
    bool got_free;
    bool got_follow;
    f32 timer;
    f32 duration;
    f32 full;
};

struct FollowA {
    bool live;
    bool dim;
    bool dim_txt;
};

struct FollowB {
    bool live;
    bool dim;
    bool dim_txt;
    bool light_on;
    bool item_visible;
};

struct FollowXY {
    bool live;
    bool dim;
    bool fishing;
    bool item_visible;
};

struct MidonaChase {
    bool used;
    bool live;
    bool hold;
    f32 z_target;
    f32 xy_target;
};

struct GaugeFill {
    bool used;
    s32 prev;
    s32 now;
    s32 max;
};

enum FadeRowId {
    kFadeRowLife,
    kFadeRowKantera,
    kFadeRowOxygen,
    kFadeRowLightDrop,
    kFadeRowRupee,
    kFadeRowKey,
    kFadeRowButtonParent,
    kFadeRowButtonA,
    kFadeRowButtonB,
    kFadeRowCross,
    kFadeRowCrossItem,
    kFadeRowCrossMap,
    kFadeRowMap,
    kFadeRowCount
};

FadeRow s_fade_rows[kFadeRowCount];
FollowA s_follow_a;
FollowB s_follow_b;
FollowXY s_follow_xy[2];
MidonaChase s_midona;
GaugeFill s_kantera_fill;
GaugeFill s_oxygen_fill;
bool s_light_drop_get;

f32 fade_row_factor(FadeRow const& r) {
    if (r.duration <= 0.0f) {
        return r.show ? 1.0f : 0.0f;
    }
    return dusk::vdt::clamped_fraction(r.timer, r.duration);
}

void publish_fade_row(FadeRow& c, bool show, f32 duration, f32 full) {
    c.used = true;
    c.duration = duration > 0.0f ? duration : 1.0f;
    if (c.timer > c.duration) {
        c.timer = c.duration;
    }
    c.full = full;
    c.show = show;
    c.got_free = true;
}

void publish_follow_row(FadeRow& c) {
    c.used = true;
    c.got_follow = true;
}

f32 button_parent_rate(dMeter2Draw_c* d) {
    if (s_fade_rows[kFadeRowButtonParent].used) {
        return s_fade_rows[kFadeRowButtonParent].full * fade_row_factor(s_fade_rows[kFadeRowButtonParent]);
    }
    if (d->mpButtonParent != NULL) {
        return d->mpButtonParent->getAlphaRate();
    }
    return 0.0f;
}

void reset_follow_intent(FadeRow& r) {
    r.got_free = false;
    r.got_follow = false;
}

void resolve_follow_row(FadeRow& r, FadeRow const& parent) {
    if (r.got_follow) {
        r.follow = true;
    } else if (r.follow && r.got_free) {
        r.follow = false;
        r.timer = fade_row_factor(parent) * r.duration;
    }
}

void step_fade_row(FadeRow& c) {
    if (c.used && !c.follow && (c.show ? c.timer < c.duration : c.timer > 0.0f)) {
        dusk::vdt::advance_toward_frame(c.timer, c.show ? c.duration : 0.0f, 1.0f);
    }
}

f32 const kFadeDuration = 5.0f;

f32 fade_full(dMeter2Draw_c* d, FadeRowId id) {
    f32 const parent = g_drawHIO.mParentAlpha;
    f32 const buttons = parent * g_drawHIO.mMainHUDButtonsAlpha;
    f32 const rupee_key = parent * g_drawHIO.mRupeeKeyAlpha;
    switch (id) {
    case kFadeRowLife:
    case kFadeRowKantera:
    case kFadeRowOxygen:
    case kFadeRowCross:
        return parent;
    case kFadeRowLightDrop:
        return parent * d->field_0x6fc;
    case kFadeRowRupee:
        return g_drawHIO.mRupeeAlpha * rupee_key;
    case kFadeRowKey:
        return g_drawHIO.mKeyAlpha * rupee_key;
    case kFadeRowButtonParent:
        return buttons;
    case kFadeRowButtonA:
        return g_drawHIO.mButtonAAlpha * buttons;
    case kFadeRowButtonB:
        return g_drawHIO.mButtonBAlpha * buttons;
    case kFadeRowCrossItem:
        return parent * g_drawHIO.mButtonCrossITEMAlpha;
    case kFadeRowCrossMap:
        return parent * g_drawHIO.mButtonCrossMAPAlpha;
    case kFadeRowMap:
        return 1.0f;
    default:
        return 0.0f;
    }
}

void apply_parent_rate(CPaneMgrAlpha* pane, FadeRow const& row) {
    if (!row.used || pane == NULL || pane->getPanePtr() == NULL) {
        return;
    }
    const f32 rate = row.full * fade_row_factor(row);
    if (pane->getAlphaRate() != rate) {
        pane->setAlphaRate(rate);
    }
}

void apply_meter_rate(dMeter2Draw_c* d, int index, FadeRow const& row) {
    if (!row.used) {
        return;
    }
    d->mMeterAlphaRate[index] = row.full * fade_row_factor(row);
}

void apply_cross_text(CPaneMgr* text, CPaneMgr** juji, FadeRow const& row) {
    if (!row.used) {
        return;
    }
    const f32 factor = fade_row_factor(row);
    if (text != NULL) {
        text->setAlphaRate(row.full * factor);
    }
    const f32 juji_rate = g_drawHIO.mParentAlpha * factor;
    for (int i = 0; i < 5; i++) {
        if (juji[i] != NULL) {
            juji[i]->setAlphaRate(juji_rate);
        }
    }
}

void apply_light_drop(dMeter2Draw_c* d) {
    if (!s_fade_rows[kFadeRowLightDrop].used || d->mpLightDropParent == NULL) {
        return;
    }

    const f32 parent_rate = s_fade_rows[kFadeRowLightDrop].full *
                            fade_row_factor(s_fade_rows[kFadeRowLightDrop]);
    if (d->mpLightDropParent->getAlphaRate() != parent_rate) {
        d->mpLightDropParent->setAlphaRate(parent_rate);
    }

    f32 si_parent_rate[2];
    for (int i = 0; i < 2; i++) {
        si_parent_rate[i] = g_drawHIO.mLightDrop.mVesselAlpha[i + 2] * parent_rate;
        if (d->mpSIParent[i] != NULL) {
            d->mpSIParent[i]->setAlphaRate(si_parent_rate[i]);
        }
    }

    if (s_light_drop_get && s_fade_rows[kFadeRowLightDrop].show) {
        if (g_drawHIO.mLightDrop.mDropGetScaleAnimFrameNum > 1) {
            const f32 duration = g_drawHIO.mLightDrop.mDropGetScaleAnimFrameNum;
            const f32 remainder = duration - s_fade_rows[kFadeRowLightDrop].timer;
            const f32 scale = g_drawHIO.mLightDrop.mDropGetScale - 1.0f;
            d->field_0x6f8 = 1.0f + (scale * remainder) / (duration - 1.0f);
        } else {
            d->field_0x6f8 = 1.0f;
        }
    }

    const int drop_num = dComIfGs_getLightDropNum(dComIfGp_getStartStageDarkArea());
    const f32 si1 = si_parent_rate[1];
    for (int i = 0; i < 16; i++) {
        const f32 rate = i < drop_num ? si1 : si1 * 0.3f;
        if (d->mpSIParts[i][1] != NULL) {
            d->mpSIParts[i][1]->setAlphaRate(rate);
        }
        if (d->mpSIParts[i][2] != NULL) {
            d->mpSIParts[i][2]->setAlphaRate(rate);
        }
    }
}

void apply_rupee(dMeter2Draw_c* d) {
    if (!s_fade_rows[kFadeRowRupee].used) {
        return;
    }

    const f32 factor = fade_row_factor(s_fade_rows[kFadeRowRupee]);
    const f32 parent_rate[3] = {
        s_fade_rows[kFadeRowRupee].full * factor,
        g_drawHIO.mRupeeFrameAlpha * (g_drawHIO.mParentAlpha * g_drawHIO.mRupeeKeyAlpha) * factor,
        g_drawHIO.mRupeeFrameAlpha * (g_drawHIO.mParentAlpha * g_drawHIO.mRupeeKeyAlpha) * factor,
    };

    for (int i = 0; i < 3; i++) {
        if (d->mpRupeeParent[i] != NULL && d->mpRupeeParent[i]->getAlphaRate() != parent_rate[i]) {
            d->mpRupeeParent[i]->setAlphaRate(parent_rate[i]);
        }
    }
}

void apply_button_parent(dMeter2Draw_c* d) {
    if (!s_fade_rows[kFadeRowButtonParent].used || d->mpButtonParent == NULL) {
        return;
    }

    apply_parent_rate(d->mpButtonParent, s_fade_rows[kFadeRowButtonParent]);
    const f32 parent_rate = s_fade_rows[kFadeRowButtonParent].full *
                            fade_row_factor(s_fade_rows[kFadeRowButtonParent]);
    if (d->mpUzu != NULL) {
        d->mpUzu->setAlphaRate(d->mButtonBaseAlpha * parent_rate);
    }
}

void apply_button_a(dMeter2Draw_c* d) {
    if (!s_fade_rows[kFadeRowButtonA].used || d->mpButtonA == NULL) {
        return;
    }

    if (s_fade_rows[kFadeRowButtonA].follow) {
        d->setButtonIconAAlpha(0, 0, false);
        return;
    }

    const f32 factor = fade_row_factor(s_fade_rows[kFadeRowButtonA]);
    d->mpButtonA->setAlphaRate(s_fade_rows[kFadeRowButtonA].full * factor);
    const f32 text_rate = (g_drawHIO.mParentAlpha * g_drawHIO.mMainHUDButtonsAlpha) * factor;
    for (int i = 0; i < 5; i++) {
        if (d->mpAText[i] != NULL) {
            d->mpAText[i]->setAlphaRate(text_rate);
        }
    }
}

void apply_button_b(dMeter2Draw_c* d) {
    if (!s_fade_rows[kFadeRowButtonB].used || d->mpButtonB == NULL) {
        return;
    }

    if (s_fade_rows[kFadeRowButtonB].follow) {
        d->setButtonIconBAlpha(0, 0, false);
        return;
    }

    const f32 factor = fade_row_factor(s_fade_rows[kFadeRowButtonB]);
    const f32 button_rate = s_fade_rows[kFadeRowButtonB].full * factor;
    const f32 text_rate = (g_drawHIO.mParentAlpha * g_drawHIO.mMainHUDButtonsAlpha) * factor;
    d->mpButtonB->setAlphaRate(button_rate);
    if (d->mpLightB != NULL) {
        d->mpLightB->setAlphaRate(button_rate);
    }
    if (d->mpItemB != NULL) {
        d->mpItemB->setAlphaRate(text_rate);
    }
    for (int i = 0; i < 5; i++) {
        if (d->mpBText[i] != NULL) {
            d->mpBText[i]->setAlphaRate(text_rate);
        }
    }
}

void advance_fade_rows() {
    for (int i = 0; i < kFadeRowCount; i++) {
        step_fade_row(s_fade_rows[i]);
    }
}

void write_fade_row(dMeter2Draw_c* d, FadeRowId id, bool show) {
    if (id >= kFadeRowCount) {
        return;
    }
    publish_fade_row(s_fade_rows[id], show, kFadeDuration, fade_full(d, id));
    if (id == kFadeRowButtonParent && show) {
        if (fade_row_factor(s_fade_rows[kFadeRowButtonParent]) >= 1.0f) {
            if (dMeter2Info_getLightDropGetFlag(dComIfGp_getStartStageDarkArea()) == 1) {
                dMeter2Info_setLightDropGetFlag(dComIfGp_getStartStageDarkArea(), 2);
            }
        }
    }
}

void follow_button_a(bool live, bool dim, bool dim_txt) {
    s_follow_a = {live, dim, dim_txt};
    if (live) {
        publish_follow_row(s_fade_rows[kFadeRowButtonA]);
    }
}

void follow_button_b(bool live, bool dim, bool dim_txt, bool light_on, bool item_visible) {
    s_follow_b = {live, dim, dim_txt, light_on, item_visible};
    if (live) {
        publish_follow_row(s_fade_rows[kFadeRowButtonB]);
    }
}

void follow_button_xy(int i_no, bool live, bool dim, bool fishing, bool item_visible) {
    if (i_no < 0 || i_no > 1) {
        return;
    }
    s_follow_xy[i_no] = {live, dim, fishing, item_visible};
}

void chase_midona(bool live, bool hold, f32 z_target, f32 xy_target) {
    s_midona = {true, live, hold, z_target, xy_target};
}

void seed_gauge(GaugeFill& g, s32 now) {
    g.prev = now;
}

void publish_gauge(GaugeFill& g, s32 now, s32 max) {
    g.used = true;
    g.now = now;
    g.max = max;
}

f32 gauge_draw(GaugeFill const& g) {
    const f32 step = dusk::interp::get_interpolation_step();
    return (f32)g.prev + ((f32)g.now - (f32)g.prev) * step;
}
}  // namespace

f32 dMeter2Draw_c::getMeterGaugeAlphaRate(u8 i_no) {
    if (i_no == 1) {
        return fade_row_factor(s_fade_rows[kFadeRowKantera]) * g_drawHIO.mParentAlpha;
    }
    if (i_no == 2) {
        return fade_row_factor(s_fade_rows[kFadeRowOxygen]) * g_drawHIO.mParentAlpha;
    }
    return mMeterAlphaRate[i_no];
}

void dMeter2Draw_c::beginExecution() {
    mExecuting = true;
    reset_follow_intent(s_fade_rows[kFadeRowButtonA]);
    reset_follow_intent(s_fade_rows[kFadeRowButtonB]);
    seed_gauge(s_kantera_fill, dComIfGp_getItemNowOil());
    seed_gauge(s_oxygen_fill, dComIfGp_getNowOxygen());
}

void dMeter2Draw_c::endExecution() {
    resolve_follow_row(s_fade_rows[kFadeRowButtonA], s_fade_rows[kFadeRowButtonParent]);
    resolve_follow_row(s_fade_rows[kFadeRowButtonB], s_fade_rows[kFadeRowButtonParent]);
    mExecuting = false;
}

void dMeter2Draw_c::publishKantera(s32 now, s32 max) {
    publish_gauge(s_kantera_fill, now, max);
}

void dMeter2Draw_c::publishOxygen(s32 now, s32 max) {
    publish_gauge(s_oxygen_fill, now, max);
}

void dMeter2Draw_c::present() {
    apply_parent_rate(this->mpLifeParent, s_fade_rows[kFadeRowLife]);
    if (s_fade_rows[kFadeRowLife].used) {
        this->setAlphaLifeChange(false);
    }

    apply_meter_rate(this, 1, s_fade_rows[kFadeRowKantera]);
    if (s_fade_rows[kFadeRowKantera].used) {
        this->setAlphaKanteraChange(true);
    }
    if (s_kantera_fill.used) {
        drawKantera(s_kantera_fill.max, gauge_draw(s_kantera_fill),
                    g_drawHIO.mLanternMeterPosX, g_drawHIO.mLanternMeterPosY);
    }

    apply_meter_rate(this, 2, s_fade_rows[kFadeRowOxygen]);
    if (s_fade_rows[kFadeRowOxygen].used) {
        this->setAlphaOxygenChange(false);
    }
    if (s_oxygen_fill.used) {
        drawOxygen(s_oxygen_fill.max, gauge_draw(s_oxygen_fill),
                   g_drawHIO.mOxygenMeterPosX, g_drawHIO.mOxygenMeterPosY);
    }

    apply_light_drop(this);
    if (s_fade_rows[kFadeRowLightDrop].used) {
        this->setAlphaLightDropChange(false);
    }

    apply_rupee(this);
    if (s_fade_rows[kFadeRowRupee].used) {
        this->setAlphaRupeeChange(false);
    }

    apply_parent_rate(this->mpKeyParent, s_fade_rows[kFadeRowKey]);
    if (s_fade_rows[kFadeRowKey].used) {
        this->setAlphaKeyChange(false);
    }

    apply_button_parent(this);
    apply_button_a(this);
    apply_button_b(this);
    setButtonIconAlpha(0, 0, 0, false);
    setButtonIconAlpha(1, 0, 0, false);
    if (s_fade_rows[kFadeRowButtonParent].used) {
        this->setAlphaButtonChange(false);
    }

    apply_parent_rate(this->mpButtonCrossParent, s_fade_rows[kFadeRowCross]);
    apply_cross_text(this->mpTextI, this->mpJujiI, s_fade_rows[kFadeRowCrossItem]);
    apply_cross_text(this->mpTextM, this->mpJujiM, s_fade_rows[kFadeRowCrossMap]);

    if (!s_midona.used || this->mpButtonMidona == NULL) {
        return;
    }

    const f32 parent_rate = button_parent_rate(this);
    if (s_midona.live) {
        if (s_midona.hold) {
            this->mButtonZAlpha = s_midona.z_target;
        } else {
            dusk::vdt::present_addCalc2(&this->mButtonZAlpha, s_midona.z_target, 0.4f, 0.5f, 0.1f);
        }
        const f32 full = g_drawHIO.mMidnaIconAlpha * (g_drawHIO.mParentAlpha * g_drawHIO.mMainHUDButtonsAlpha);
        this->mpButtonMidona->setAlpha((u8)(full * (255.0f * this->mButtonZAlpha * parent_rate)));
    } else {
        this->mButtonZAlpha = 0.0f;
    }
    dusk::vdt::present_addCalc2(&this->field_0x724, s_midona.xy_target, 0.4f, 0.5f, 0.1f);
    this->mpButtonXY[2]->setAlpha((u8)(255.0f * this->field_0x724 * parent_rate));
}

void dMeter2Draw_c::presentVessel(f32 i_posX, f32 i_posY, f32 i_scale) {
    mLightDropVesselScale = i_scale;
    const f32 lightDropScale = mLightDropVesselScale * field_0x6f8 * dGetUserHudScale();
    mpLightDropParent->scale(lightDropScale, lightDropScale);
    dAnchorHudScale(mpLightDropParent, HudCorner::TopRight, &i_posX, &i_posY);
    mpLightDropParent->paneTrans(i_posX, i_posY);
}

void dMeter2Draw_c::presentButtonA(f32 i_posX, f32 i_posY, f32 i_textPosX, f32 i_textPosY, f32 i_scale) {
    mpButtonA->scale(s_button_a_scale * i_scale, s_button_a_scale * i_scale);
    mpButtonA->paneTrans(i_posX, i_posY);
    mpTextA->scale(s_button_a_text_scale * i_scale, s_button_a_text_scale * i_scale);
    mpTextA->paneTrans(g_drawHIO.mButtonATextPosX + i_textPosX,
                       g_drawHIO.mButtonATextPosY + i_textPosY + (dusk::version::isLessThanWiiJpn() ? 6.0f : 0.0f));
}

void dMeter2Draw_c::presentButtonB(f32 i_posX, f32 i_posY, f32 i_textPosX, f32 i_textPosY, f32 i_scale) {
    const int kind = s_button_b_kind;
    field_0x730 = s_button_b_scale * i_scale;
    mpButtonB->scale(field_0x730 * field_0x734, field_0x730 * field_0x734);
    mpButtonB->paneTrans(g_drawHIO.mButtonBPosX + i_posX, g_drawHIO.mButtonBPosY + i_posY);

    field_0x728 = g_drawHIO.mButtonBItemScale[kind] * i_scale;
    mpItemB->scale(field_0x728 * field_0x734, field_0x728 * field_0x734);
    mpItemB->paneTrans(field_0x6dc + (g_drawHIO.mButtonBItemPosX[kind] + i_posX), field_0x6e0 + (g_drawHIO.mButtonBItemPosY[kind] + i_posY));

    field_0x72c = g_drawHIO.mItemBBaseScale[0] * i_scale;
    mpLightB->scale(field_0x72c * field_0x734, field_0x72c * field_0x734);
    mpLightB->paneTrans(g_drawHIO.mItemBBasePosX[0] + i_posX, g_drawHIO.mItemBBasePosY[0] + i_posY);

    mpTextB->scale(s_button_b_text_scale * i_scale, s_button_b_text_scale * i_scale);
    mpTextB->paneTrans(g_drawHIO.mButtonBFontPosX + i_textPosX,
                       g_drawHIO.mButtonBFontPosY + i_textPosY + (dusk::version::isLessThanWiiJpn() ? 4.0f : 0.0f));
}

void dMeter2Draw_c::presentMapAlpha(dMeterMap_c* map) {
    if (map == NULL) {
        return;
    }
    if (!s_fade_rows[kFadeRowMap].used) {
        map->setMapAlpha(0);
        return;
    }
    map->setMapAlpha((u8)(fade_row_factor(s_fade_rows[kFadeRowMap]) * 255.0f));
}

void dMeter2Draw_c::showLightDrop(bool show, bool get_path) {
    if (show && get_path) {
        if (s_fade_rows[kFadeRowLightDrop].timer <= 0.0f) {
            Z2GetAudioMgr()->seStart(Z2SE_SY_LIGHT_POT_EQUIP, NULL, 0, 0, 1.0f, 1.0f, -1.0f, -1.0f, 0);
        }
        if (fade_row_factor(s_fade_rows[kFadeRowLightDrop]) >= 1.0f) {
            dMeter2Info_setLightDropGetFlag(dComIfGp_getStartStageDarkArea(), 0xFF);
        }
    }
    const f32 duration = get_path ? (f32)g_drawHIO.mLightDrop.mDropGetScaleAnimFrameNum : kFadeDuration;
    publish_fade_row(s_fade_rows[kFadeRowLightDrop], show, duration, fade_full(this, kFadeRowLightDrop));
    s_light_drop_get = get_path;
}

void dMeter2Draw_c::showCross(bool show) {
    write_fade_row(this, kFadeRowCross, show);
    write_fade_row(this, kFadeRowCrossItem, show && dMeter2Info_isItemOpenCheck());
    write_fade_row(this, kFadeRowCrossMap, show && dMeter2Info_isMapOpenCheck());
}

void dMeter2Draw_c::showMap(bool show) {
    write_fade_row(this, kFadeRowMap, show);
}

bool dMeter2Draw_c::lightDropRowUsed() const {
    return s_fade_rows[kFadeRowLightDrop].used;
}

void dMeter2Draw_c::resetRows() {
    mExecuting = false;
    for (int i = 0; i < kFadeRowCount; i++) {
        s_fade_rows[i] = FadeRow{};
    }
    s_follow_a = FollowA{};
    s_follow_b = FollowB{};
    s_follow_xy[0] = FollowXY{};
    s_follow_xy[1] = FollowXY{};
    s_midona = MidonaChase{};
    s_kantera_fill = GaugeFill{};
    s_oxygen_fill = GaugeFill{};
    s_light_drop_get = false;
}
