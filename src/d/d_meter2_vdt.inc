#include "dusk/interp/user_interface.h"

#include "d/d_meter_map.h"

void dMeter2_c::presentAnims() {
    if (mpMeterDraw != NULL) {
        mpMeterDraw->present();

        if (!mPresentationTargets.ready) {
            return;
        }

        if (mpMeterDraw->lightDropRowUsed()) {
            dusk::vdt::present_addCalc2(&mVesselPosX, mPresentationTargets.vesselX, 1.0f, 10.0f, 0.1f);
            dusk::vdt::present_addCalc2(&mVesselPosY, mPresentationTargets.vesselY, 1.0f, 10.0f, 0.1f);
            dusk::vdt::present_addCalc2(&mVesselScale, mPresentationTargets.vesselScale, 0.2f, 1.0f, 0.1f);
            dusk::vdt::present_addCalc2(&mVesselAlpha, mPresentationTargets.vesselAlpha, 0.2f, 1.0f, 0.1f);
            mpMeterDraw->field_0x6fc = mVesselAlpha;
            mpMeterDraw->presentVessel(mVesselPosX, mVesselPosY, mVesselScale);
        }

        for (int i = 0; i < 2; i++) {
            dusk::vdt::present_addCalc2(&mButtonATalkPosX[i], mPresentationTargets.buttonAX[i], 1.0f, 10.0f, 0.1f);
            dusk::vdt::present_addCalc2(&mButtonATalkPosY[i], mPresentationTargets.buttonAY[i], 1.0f, 10.0f, 0.1f);
            dusk::vdt::present_addCalc2(&field_0x148[i], mPresentationTargets.buttonBX[i], 1.0f, 10.0f, 0.1f);
            dusk::vdt::present_addCalc2(&field_0x150[i], mPresentationTargets.buttonBY[i], 1.0f, 10.0f, 0.1f);
        }
        dusk::vdt::present_addCalc2(&field_0x15c, mPresentationTargets.crossY, 0.5f, 50.0f, 0.5f);
        mpMeterDraw->presentButtonA(mButtonATalkPosX[0], mButtonATalkPosY[0], mButtonATalkPosX[1], mButtonATalkPosY[1], 1.0f);
        mpMeterDraw->presentButtonB(field_0x148[0], field_0x150[0], field_0x148[1], field_0x150[1], 1.0f);
        mpMeterDraw->drawButtonCross(mPresentationTargets.crossX, field_0x15c);
    }
}

void dMeter2_c::presentMap() {
    if (mpMeterDraw == NULL || mpMap == NULL) {
        return;
    }
    mpMeterDraw->presentMapAlpha(mpMap);
}
