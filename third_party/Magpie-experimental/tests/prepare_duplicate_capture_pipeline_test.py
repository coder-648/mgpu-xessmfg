"""Extract production capture update/filter/resize; double only platform work.

The HDR processor records calls and metadata, not a copy of its color formula.
The duplicate GPU result is controlled by the fixture; pixels are tested in WARP.
"""
from pathlib import Path
import re
import sys

repo = Path(__file__).resolve().parents[1]
source = (repo / "src/Magpie.Core/FrameSourceBase.cpp").read_text(encoding="utf-8-sig")
header = (repo / "src/Magpie.Core/FrameSourceBase.h").read_text(encoding="utf-8-sig")
def body(signature, following):
    start = source.index(signature)
    text = source[source.index("{", start)+1:source.index(following, start)]
    return text[:text.rindex("}")]

fixture = r'''
#include <atomic>
#include <optional>
#include <string>
#include <string_view>
#include <cstdint>
#include <cstdlib>
#include <iostream>
ENUMS
enum class DuplicateFrameDetectionMode { Always, Dynamic, Never };
struct ScalingOptions {
    DuplicateFrameDetectionMode duplicateFrameDetectionMode=DuplicateFrameDetectionMode::Always;
    bool Is3DGameMode() const { return false; }
    bool IsStatisticsForDynamicDetectionEnabled() const { return false; }
};
struct ScalingWindow {
    static ScalingWindow& Get() { static ScalingWindow result; return result; }
    const ScalingOptions& Options() const { static ScalingOptions result; return result; }
};
namespace wil {
template<class F> struct scope_exit { F callback; explicit scope_exit(F f):callback(f){} ~scope_exit(){callback();} };
}
namespace FrameTrace {
enum class Event { CaptureClassification, HdrCapture };
inline void Mark(Event,int64_t,uint64_t) {}
struct Scope { explicit Scope(Event){} };
}
namespace fmt { template<class... T> std::string format(std::string_view text,T&&...) { return std::string(text); } }
struct Logger {
    static Logger& Get() { static Logger logger; return logger; }
    void Error(const char*) {} void Info(const std::string&) {}
};
struct D3D11_TEXTURE2D_DESC { uint32_t Width=17,Height=19,Format=10; };
struct Resource {
    bool valid=false; int pixel=5; D3D11_TEXTURE2D_DESC desc;
    Resource* get(){return this;} Resource* operator->(){return this;}
    explicit operator bool() const{return valid;}
    void operator=(std::nullptr_t){valid=false;}
    void GetDesc(D3D11_TEXTURE2D_DESC* out){*out=desc;}
};
struct ID3D11DeviceContext4 {
    void CopyResource(Resource* to,Resource* from) { to->pixel=from->pixel; ++copies; }
    int copies=0;
};
struct Device { ID3D11DeviceContext4 dc; ID3D11DeviceContext4* GetD3DDC(){return &dc;} };
struct ColorDescription { int revision=0; bool operator==(const ColorDescription&) const=default; };
enum class HdrFrameStage { RawCapture };
struct HdrFrameMetadata {
    uint64_t frameId=0,captureSequence=0,resourceGeneration=0;
    int64_t timestamp100ns=0; uint32_t width=0,height=0,sourceFormat=0;
    ColorDescription color; HdrFrameStage stage{}; bool valid=false;
};
struct Processor {
    int calls=0,prepares=0,canonical=0; bool failure=false; HdrFrameMetadata metadata;
    bool Process(Resource* source,const HdrFrameMetadata& frame) {
        ++calls; if(failure)return false; canonical=source->pixel; metadata=frame; return true;
    }
    bool Prepare(Resource*,uint32_t,ColorDescription){++prepares;canonical=0;metadata={};return true;}
    const HdrFrameMetadata& GetFrameMetadata() const{return metadata;}
    std::string_view LastAssumption() const{return {};}
};
enum class HdrAdapterProfile { DirectFP16 };
struct HdrDiagnostics {
    bool hdrOptionEnabled=false; const char* captureMethod=nullptr; uint32_t sourceFormat=0;
    ColorDescription sourceColorDescription; HdrAdapterProfile selectedAdapterProfile{};
    const char* conversionPath=nullptr;
};
void AppendHdrAssumption(HdrDiagnostics&,std::string_view){} void LogHdrDiagnostics(HdrDiagnostics&,bool){}
constexpr uint16_t INITIAL_CHECK_COUNT=16,INITIAL_SKIP_COUNT=1,MAX_SKIP_COUNT=16;
struct Capture {
    FrameSourceState next=FrameSourceState::NewFrame;
    CaptureFrameReason _lastUpdateReason=CaptureFrameReason::NoFrame;
    uint64_t _captureSequence=1,_duplicateCaptureSequence=0,_resourceGeneration=1,_hdrFrameSequence=0;
    int64_t _captureTimestamp100ns=1000;
    bool _captureInterrupted=false,_hdrEnabled=true,_hdrFrameReady=false,_hdrDiagnosticsLogged=false;
    bool _duplicateSourceColorValid=false,_duplicateComparisonFailed=false;
    bool _isCheckingForDuplicateFrame=true,duplicate=true,initFailure=false,mapFailure=false;
    uint32_t _framesLeft=16,_nextSkipCount=1;
    ColorDescription color,_duplicateSourceColor;
    std::optional<bool> _duplicateFrameDetectionOverride;
    std::atomic<std::pair<uint32_t,uint32_t>> _statistics{{0,0}};
    Resource _output,_prevFrame,_prevFrameSrv; Resource* _duplicateSourceTexture=nullptr;
    Device device; Device* _deviceResources=&device; Processor _hdrProcessor;
    Capture(){_output.valid=true;}
    FrameSourceState _Update(){return next;}
    ColorDescription _GetSourceColorDescription(){return color;}
    const char* Name(){return "fixture";}
    bool _InitCheckingForDuplicateFrame(){_prevFrame.valid=true;_prevFrame.desc=_output.desc;_duplicateSourceTexture=_output.get();return !initFailure;}
    void _ResetDuplicateDetection(){_prevFrame=nullptr;_prevFrameSrv=nullptr;_framesLeft=16;_nextSkipCount=1;_isCheckingForDuplicateFrame=true;}
    bool _IsDuplicateFrame(){_duplicateComparisonFailed=mapFailure;return !mapFailure&&duplicate;}
    FrameSourceState Update() noexcept { UPDATE }
    FrameSourceState _FilterDuplicateFrame(FrameSourceState state,bool forceAccept) noexcept { FILTER }
    bool PrepareHdrOutputForResize() noexcept { RESIZE }
};
void Check(bool value,const char* message){if(!value){std::cerr<<message<<'\n';std::exit(1);}}
int main(){
    using State=FrameSourceState;
    Capture capture;
    Check(capture.Update()==State::NewFrame&&capture._hdrProcessor.calls==1,"first HDR input was not converted");
    Check(capture.Update()==State::Waiting&&capture._hdrProcessor.calls==1,"duplicate still performed HDR conversion");
    ++capture.color.revision;
    Check(capture.Update()==State::NewFrame&&capture._hdrProcessor.calls==2&&capture._lastUpdateReason==CaptureFrameReason::MetadataChanged,"new color semantics were hidden by RGB duplicate");
    Check(capture.Update()==State::Waiting,"color semantics update failed to establish baseline");
    ++capture._captureSequence;
    Check(capture.Update()==State::NewFrame&&capture._hdrProcessor.calls==3,"recovered sequence first frame was filtered");
    Check(capture.Update()==State::Waiting,"recovered sequence did not resume filtering");
    capture.next=State::Waiting; capture._captureInterrupted=true;
    Check(capture.Update()==State::Waiting&&capture._lastUpdateReason==CaptureFrameReason::Interrupted&&capture._hdrProcessor.calls==3,"interruption advanced conversion/history");
    capture._captureInterrupted=false;
    Check(capture.Update()==State::Waiting&&capture._lastUpdateReason==CaptureFrameReason::NoFrame,"no-frame cause was confused with pixel duplicate");
    const auto timestamp=capture._hdrProcessor.metadata.timestamp100ns;
    Check(capture.PrepareHdrOutputForResize()&&capture._hdrFrameReady&&capture._hdrProcessor.canonical==5&&capture._hdrProcessor.calls==4,"same-frame resize used cleared canonical pixels");
    Check(capture._hdrProcessor.metadata.timestamp100ns==timestamp&&capture._hdrProcessor.metadata.resourceGeneration==capture._resourceGeneration,"resize fabricated capture time or lost resource generation");
    capture.next=State::NewFrame; capture.mapFailure=true;
    Check(capture.Update()==State::NewFrame&&capture._lastUpdateReason==CaptureFrameReason::ReadbackFailed,"readback failure did not fail open");
    capture.mapFailure=false;
    Check(capture.Update()==State::Waiting,"readback recovery reused an obsolete flag");
    capture.duplicate=false; capture._hdrProcessor.failure=true;
    Check(capture.Update()==State::Error&&!capture._hdrFrameReady,"failed conversion left canonical output valid");
    capture._hdrProcessor.failure=false; capture.duplicate=true;
    Check(capture.Update()==State::NewFrame&&capture._hdrFrameReady,"conversion retry filtered an invalid canonical output");
    capture._output.desc.Width=33;
    Check(capture.Update()==State::NewFrame&&capture._prevFrame.desc.Width==33,"changed capture resources used an old comparison texture");
    Capture empty;
    Check(empty.PrepareHdrOutputForResize()&&!empty._hdrFrameReady&&empty._hdrProcessor.calls==0,"resize invented a first capture");
    std::cout<<"Production capture pipeline: HDR reuse, color, sequence, interruption, resize and failure cases passed.\n";
}
'''
enums = "\n".join(re.search(r"enum class " + name + r".*?\n};", header, re.S)[0]
                  for name in ("FrameSourceState", "CaptureFrameReason"))
fixture = fixture.replace("ENUMS", enums)
fixture = fixture.replace("UPDATE", body("FrameSourceState FrameSourceBase::Update()", "FrameSourceState FrameSourceBase::_FilterDuplicateFrame("))
fixture = fixture.replace("FILTER", body("FrameSourceState FrameSourceBase::_FilterDuplicateFrame(", "ColorDescription FrameSourceBase::_GetSourceColorDescription("))
fixture = fixture.replace("RESIZE", body("bool FrameSourceBase::PrepareHdrOutputForResize()", "std::pair<uint32_t, uint32_t> FrameSourceBase::GetStatisticsForDynamicDetection()"))
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
(out / "duplicate_capture_pipeline.cpp").write_text(fixture, encoding="utf-8")
print(out / "duplicate_capture_pipeline.cpp")
