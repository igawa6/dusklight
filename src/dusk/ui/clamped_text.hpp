#pragma once

#include "component.hpp"

namespace dusk::ui {

class ClampedText final : public Component {
public:
    ClampedText(Rml::Element* root, Rml::String text, int maxLines);
    void update() override;

private:
    Rml::String mSource;
    Rml::ElementText* mText;
    int mMaxLines;
    float mWidth = 0;
    Rml::FontFaceHandle mFont = 0;
};

}  // namespace dusk::ui
