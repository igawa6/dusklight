#include "clamped_text.hpp"

#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/ElementUtilities.h>
#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <utility>

namespace dusk::ui {

ClampedText::ClampedText(Rml::Element* root, Rml::String text, int maxLines)
    : Component{root}, mSource{std::move(text)},
      mText{static_cast<Rml::ElementText*>(append_text(root, mSource))},
      mMaxLines{std::max(maxLines, 1)} {
    mRoot->SetProperty("white-space", "pre-line");
    mRoot->SetProperty("word-break", "break-word");
}

void ClampedText::update() {
    const float width = mRoot->GetBox().GetSize(Rml::BoxArea::Content).x;
    const auto font = mText->GetFontFaceHandle();
    if (width > 0 && font && (width != mWidth || font != mFont)) {
        mWidth = width;
        mFont = font;
        mText->SetText(mSource);
        Rml::String result;
        int offset = 0;
        for (int index = 0; index < mMaxLines && offset < mSource.size(); ++index) {
            Rml::String line;
            int consumed = 0;
            float lineWidth = 0;
            mText->GenerateLine(line, consumed, lineWidth, offset, width, 0, true, false, false);
            offset += consumed;
            line = Rml::StringUtilities::StripWhitespace(line);
            if (index == mMaxLines - 1 && offset < mSource.size()) {
                while (!line.empty() && static_cast<float>(Rml::ElementUtilities::GetStringWidth(
                                            mText, line + "…")) > width)
                {
                    const auto* end =
                        Rml::StringUtilities::SeekBackwardUTF8(&line.back(), line.data());
                    line.resize(end - line.data());
                }
                line = Rml::StringUtilities::StripWhitespace(line) + "…";
            }
            if (index > 0) {
                result += '\n';
            }
            result += line;
        }
        mText->SetText(result);
    }
    Component::update();
}

}  // namespace dusk::ui
