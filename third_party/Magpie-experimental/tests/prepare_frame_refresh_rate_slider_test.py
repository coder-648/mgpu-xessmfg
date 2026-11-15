"""Exercise the production rate widget with real ImGui mouse input."""
from pathlib import Path
import sys

repo = Path(__file__).resolve().parents[1]
source = (repo / 'src/Magpie.Core/OverlayDrawer.cpp').read_text(encoding='utf-8-sig')
start = source.index('auto rate = [&](const char* id, const wchar_t* label, float& value')
end = source.index('{', start) + 1
depth = 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
widget = source[start:end] + ';'
assert 'FrameRefreshSettings::MinimumEditedRate' in widget and 'FrameRefreshSettings::MaximumEditedRate' in widget
prefix = r'''
#include <imgui.h>
#include "FrameRefreshSettings.h"
#include <algorithm>
#include <cmath>
#include <cassert>
#include <iostream>
#include <string>
using namespace Magpie;
bool Draw(float& value, bool allowInput) {
    bool parameterEdited=false, needRedraw=false;
    bool _parameterFocusSwitchingEnabled=allowInput;
    auto _GetResourceString=[](const wchar_t*) { return std::string("Target rate"); };
'''
suffix = r'''
    const bool changed=rate("##target", L"rate", value);
    assert(changed==parameterEdited && changed==needRedraw);
    return changed;
}
int main() {
    ImGui::CreateContext();
    auto& io=ImGui::GetIO(); io.IniFilename=nullptr; io.LogFilename=nullptr;
    io.DisplaySize=ImVec2(1100,200); io.DeltaTime=1.0f/60;
    unsigned char* pixels; int width,height; io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
    ImGui::GetStyle().WindowPadding=ImVec2(8,8);
    int edits=0, unitSteps=0;
    for (bool allowInput : {false,true}) for (int control=0;control<3;++control) {
        const ImVec2 bounds(15,360);
        float value=60; ImVec2 left{},right{};
        auto frame=[&](float mouseX, bool down) {
            io.MousePos=ImVec2(mouseX,(left.y+right.y)/2); io.MouseDown[0]=down;
            ImGui::NewFrame(); ImGui::SetNextWindowPos(ImVec2(0,0)); ImGui::SetNextWindowSize(ImVec2(1060,130));
            ImGui::Begin("rates",nullptr,ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoMove);
            bool changed=Draw(value,allowInput);
            left=ImGui::GetItemRectMin(); right=ImGui::GetItemRectMax();
            ImGui::End(); ImGui::Render(); return changed;
        };
        frame(-100,false); frame(-100,false);
        // Showing an existing fractional value must not save an implicit edit.
        value=59.94f; assert(!frame(-100,false) && value==59.94f);
        float previous=-1;
        for(float x=left.x; x<=right.x; x+=1) {
            if(frame(x,true)) {
                ++edits; assert(value==std::round(value));
                assert(value>=bounds.x && value<=bounds.y);
                if(previous>=0 && value-previous==1) ++unitSteps;
                previous=value;
            }
        }
        assert(value==bounds.y); frame(right.x,false);
        frame(left.x,true); assert(value==bounds.x); frame(left.x,false);
    }
    assert(edits>100 && unitSteps>100);
    ImGui::DestroyContext();
    std::cout<<"PASS production rate widget: all three controls, integer mouse edits, 1-FPS values, 15..360 boundaries, both input modes, no implicit fractional-value save ("<<edits<<" edits).\n";
}
'''
output = Path(sys.argv[1])
output.mkdir(parents=True, exist_ok=True)
(output/'FrameRefreshRateSliderTests.cpp').write_text(prefix+widget+suffix,encoding='utf-8')
