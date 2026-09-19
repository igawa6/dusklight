#pragma once

#include "samples.h"

namespace dusk::interp {

struct MenuPose {
    f32 x;
    f32 y;
    f32 alpha;
};

inline void lerp(MenuPose& out, const MenuPose& lhs, const MenuPose& rhs, float step) {
    out = {lerp(lhs.x, rhs.x, step), lerp(lhs.y, rhs.y, step), lerp(lhs.alpha, rhs.alpha, step)};
}

struct MenuTransition {
    Samples<MenuPose> poses;
};

inline void capture_menu_pose(const void* owner, MenuPose pose) {
    if (should_capture()) {
        get<MenuTransition>(owner).poses.capture(&pose, 1);
    }
}

inline MenuPose read_menu_pose(const void* owner, MenuPose current) {
    return get<MenuTransition>(owner).poses.read(0, current);
}

}  // namespace dusk::interp
