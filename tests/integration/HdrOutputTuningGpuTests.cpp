// PR19 integration: real graph -> VideoPresenter -> native DXGI readback.
// Readback/waits are diagnostic-only. This measures pixels, not panel light.
#include "veyra/pipeline/EnhanceGraph.h"
#include "veyra/engine/VideoPresenter.h"
#include "veyra/engine/EnhancementSettings.h"
#include "veyra/pipeline/PqTransfer.h"
#include "veyra/gfx/D3D12DeviceContext.h"
#include "veyra/gfx/CommandSlotRing.h"
#include "veyra/RuntimePaths.h"
#include <DirectXPackedVector.h>
#include <iostream>
#include <array>
#include <thread>
#include <d3d12sdklayers.h>
#include <cmath>
#include <string_view>
extern "C" {
#include <libavutil/frame.h>
}
#include "CaptureFormatGpuCases.h"
#include "HdrNativeRoundTripCases.h"

using namespace veyra;
// Optional runtime-backed regression: an SDR source converted by TrueHDR must
// keep the display curve, even though graph.hdrInput() is false. The regular
// native HDR suite below remains independent of proprietary runtimes.
static int videoHdrPreview(gfx::D3D12DeviceContext& ctx,gfx::CommandSlotRing& ring){
    pipeline::EnhanceGraph graph(ctx,ring);pipeline::EnhanceGraphDesc gd;
    gd.sourceWidth=gd.workWidth=gd.nrWidth=1280;gd.sourceHeight=gd.workHeight=gd.nrHeight=720;
    gd.rgbInput=true;gd.enableNr=gd.enableSr=gd.enableFg=gd.enableNvofStandalone=false;
    gd.videoHdr.enabled=true;gd.hdrOutput=true;gd.hdrOutputMode=engine::HdrOutputMode::ScRgb;
    gd.runtimeAbsPath=runtime::localRuntimeDirectory().wstring();
    AVFrame* frame=av_frame_alloc();if(!frame)return 2;
    frame->format=AV_PIX_FMT_BGRA;frame->width=1280;frame->height=720;
    frame->color_range=AVCOL_RANGE_JPEG;frame->color_primaries=AVCOL_PRI_BT709;frame->color_trc=AVCOL_TRC_IEC61966_2_1;
    bool ok=av_frame_get_buffer(frame,32)>=0;
    if(ok)for(int y=0;y<720;++y)for(int x=0;x<1280;++x){auto* p=frame->data[0]+y*frame->linesize[0]+x*4;p[0]=p[1]=p[2]=uint8_t(x*255/1279);p[3]=255;}
    pipeline::EnhanceGraph::FrameOutputs out;std::vector<float> raw,shown;
    ok=ok&&graph.initialize(gd)&&graph.createViews()&&graph.videoHdrActive()&&!graph.hdrInput()&&
        graph.process(frame,0,true,out,1)&&hdrRoundTrip::read(ctx,ring,graph.videoFrameResource(out.videoSlot),raw);
    HWND window=CreateWindowExW(0,L"STATIC",L"SDR to HDR display curve regression",WS_POPUP,0,0,1280,720,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    engine::VideoPresenter presenter;
    const auto present=[&](){
        bool pass=presenter.present(ctx,ring,graph,out.videoSlot,false,true,0,false,.5f,{1,1,1});
        auto buffer=presenter.presentedResourceForTest();return pass&&hdrRoundTrip::read(ctx,ring,buffer.Get(),shown);
    };
    ok=ok&&window&&presenter.open(ctx,window,graph)&&present()&&shown==raw;
    engine::EnhancementSettings settings;settings.nr=settings.sr=false;settings.multiplier=1;
    settings.videoHdr=gd.videoHdr;settings.hdrOutputMode=gd.hdrOutputMode;settings.hdrCurve.enabled=true;
    settings.hdrCurve.highlightStartNits=200;settings.hdrCurve.peakCapNits=400;settings.hdrCurve.displayPeakNits=1000;
    ok=ok&&graph.applySettings(settings)&&present()&&shown.size()==raw.size();
    double maxError=0,meanChange=0;
    if(ok)for(size_t i=0;i<raw.size();++i){
        const double before=raw[i]*80,after=shown[i]*80;
        const double expected=before<=200?before:200+200*(before-200)/(200+before-200);
        if(expected>=1)maxError=std::max(maxError,std::abs(after-expected)/expected);
        meanChange+=std::abs(after-before);
    }
    if(ok){meanChange/=raw.size();ok=maxError<.015&&meanChange>1;}
    presenter.observeHdrCoverage(true);ok=ok&&present()&&present()&&presenter.liveCurveCoverage().valid;
    settings.hdrCurve.strengthPercent=0;
    ok=ok&&graph.applySettings(settings)&&present()&&shown==raw;
    std::cout<<"VIDEO_HDR_DISPLAY_CURVE pass="<<ok<<" maxRelative="<<maxError<<" meanChangeNits="<<meanChange
        <<" sourceHdr="<<graph.hdrInput()<<" videoHdrActive="<<graph.videoHdrActive()<<std::endl;
    ring.drainQueue();presenter.close();out={};graph.shutdown();av_frame_free(&frame);if(window)DestroyWindow(window);
    return ok?0:1;
}
// Custom (完整 B): the per-pixel Dolby Vision expansion has to reach real pixels.
//
// The same 16-step PQ ramp is presented twice - expansion off, then on - and the
// two readbacks are compared step by step. This is the only check that proves the
// b1 table upload, the extra root-signature parameter and the shader flag all line
// up; the unit tests pin the arithmetic, this pins the wiring.
static int doviExpansionPreview(gfx::D3D12DeviceContext& ctx,gfx::CommandSlotRing& ring){
    constexpr std::array<double,16> patches={0,.005,.05,.5,1,2,5,10,20,50,100,203,400,1000,4000,10000};
    AVFrame* frame=av_frame_alloc();if(!frame)return 2;
    frame->width=64;frame->height=16;frame->format=AV_PIX_FMT_YUV420P10LE;
    frame->color_range=AVCOL_RANGE_MPEG;frame->colorspace=AVCOL_SPC_BT2020_NCL;
    frame->color_primaries=AVCOL_PRI_BT2020;frame->color_trc=AVCOL_TRC_SMPTE2084;
    if(av_frame_get_buffer(frame,32)<0){av_frame_free(&frame);return 2;}
    for(unsigned y=0;y<16;++y)for(unsigned x=0;x<64;++x){
        // Rows 0..7 are the neutral ramp used by every other check. Rows 8..15 carry
        // HALF the luminance plus a moderate chroma offset: the offset raises R, and a
        // full-luminance saturated row would land above the master peak, where the
        // expansion refuses to run at all (so it could never exercise the gate).
        const double patch=patches[x/4]*(y<8?1.0:0.5);
        reinterpret_cast<uint16_t*>(frame->data[0]+y*frame->linesize[0])[x]=uint16_t(std::lround(64+876*hdrRoundTrip::pq(patch)));
        if(!(y%2)&&!(x%2))for(unsigned c=1;c<3;++c)
            reinterpret_cast<uint16_t*>(frame->data[c]+y/2*frame->linesize[c])[x/2]= (y<8||c==1)?512:560;
    }
    pipeline::EnhanceGraph graph(ctx,ring);pipeline::EnhanceGraphDesc gd;
    gd.sourceWidth=gd.workWidth=64;gd.sourceHeight=gd.workHeight=16;
    gd.hdrInput=gd.hdrOutput=true;gd.noFeatures=gd.noNgx=true;gd.enableNr=gd.enableSr=gd.enableFg=false;
    gd.hdrOutputMode=engine::HdrOutputMode::ScRgb;
    gd.hdrCurve.displayPeakNits=1000;
    pipeline::EnhanceGraph::FrameOutputs output;std::vector<float> raw;
    bool ready=graph.initialize(gd)&&graph.createViews()&&graph.process(frame,0,true,output,1)&&
        hdrRoundTrip::read(ctx,ring,graph.videoFrameResource(output.videoSlot),raw);
    HWND window=CreateWindowExW(0,L"STATIC",L"DV expansion diagnostic",WS_POPUP,0,0,64,16,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    engine::VideoPresenter presenter;
    ready=ready&&window&&presenter.open(ctx,window,graph);
    std::vector<float> shown;
    const auto present=[&](uint64_t epoch){
        bool ok=presenter.present(ctx,ring,graph,output.videoSlot,false,true,0,false,.5f,{epoch,1,1});
        auto buffer=presenter.presentedResourceForTest();
        ok=ok&&hdrRoundTrip::read(ctx,ring,buffer.Get(),shown);buffer.Reset();return ok;
    };
    // A 443-nit DV master (a typical streaming P5/P8 grade) under a 1000-nit
    // ceiling: exactly the case CB1's expansion exists for.
    pipeline::DoviFrameInfo dovi;dovi.valid=true;dovi.profile=5;
    dovi.maxPq=float(pipeline::nitsToPq(442.8));dovi.avgPq=float(pipeline::nitsToPq(40.0));dovi.minPq=0.0f;
    presenter.setFrameDovi(dovi);
    engine::EnhancementSettings settings;settings.hdrOutputMode=engine::HdrOutputMode::ScRgb;
    settings.hdrCurve.enabled=true;settings.hdrCurve.displayPeakNits=1000;
    // No knee and no cap: this case isolates the expansion, not the curve.
    settings.hdrCurve.highlightStartNits=0;settings.hdrCurve.rollOffPercent=100;settings.hdrCurve.peakCapNits=0;
    settings.doviFollow=true;settings.doviEnhanced=false;
    std::vector<float> plain;
    ready=ready&&graph.applySettings(settings)&&present(2);
    if(ready)plain=shown;
    settings.doviEnhanced=true;
    const bool expanded=ready&&graph.applySettings(settings)&&present(3);
    // Keep the expanded readback: the regression below presents again and would
    // otherwise overwrite `shown`.
    std::vector<float> expandedShown;
    if(expanded)expandedShown=shown;
    // Regression: with no ceiling stated, the expansion must stay off. The first
    // implementation fell back to the EDID report (10000 nits on almost every
    // panel with a near-zero black), which turned a 1162-nit master into an
    // 8837-nit lift - a white picture. `plain` is the identity readback, so the
    // two must match byte for byte.
    std::vector<float> noCeiling;
    bool noCeilingOk=false;
    {
        engine::EnhancementSettings bare=settings;
        bare.hdrCurve.displayPeakNits=0;
        bare.hdrCurve.peakCapNits=0;
        noCeilingOk=ready&&graph.applySettings(bare)&&present(4);
        if(noCeilingOk)noCeiling=shown;
    }
    int failures=0;
    const auto check=[&](bool ok,const char* label){std::cout<<(ok?"PASS ":"FAIL ")<<label<<std::endl;failures+=!ok;};
    check(ready&&expanded,"graph, presenter and both presentations succeed");
    check(noCeilingOk&&!noCeiling.empty()&&noCeiling==plain,
          "no stated ceiling: the EDID 10000-nit report is never used, expansion stays off");
    const auto nitsAt=[&](const std::vector<float>& v,unsigned patch,unsigned row=0){
        return double(v.at((size_t(row)*64+size_t(patch*4+2))*3))*80.0;
    };
    double lift400=0,lift203=0,lift1000=0,liftDark=0;
    if(expanded&&plain.size()==expandedShown.size()){
        lift400=nitsAt(expandedShown,12)-nitsAt(plain,12);   // 400 nits, below the master
        lift203=nitsAt(expandedShown,11)-nitsAt(plain,11);   // 203 nits, SDR reference white
        lift1000=nitsAt(expandedShown,13)-nitsAt(plain,13);  // 1000 nits, above the master
        liftDark=nitsAt(expandedShown,4)-nitsAt(plain,4);    // 1 nit, black end
        // The table must be inside the ceiling and monotonic per step.
        bool insideCeiling=true,monotonic=true;double previous=0;
        for(unsigned i=5;i<patches.size();++i){
            const double value=nitsAt(expandedShown,i);
            insideCeiling=insideCeiling&&value<=1002.0;
            monotonic=monotonic&&value+0.5>=previous;previous=value;
        }
        check(lift400>150.0,"400-nit step is lifted by more than 150 nits");
        check(lift203>30.0&&lift203<lift400,"SDR white is lifted less than the 400-nit step");
        check(std::abs(liftDark)<0.5,"the black end is untouched");
        check(insideCeiling,"no step exceeds the 1000-nit ceiling");
        check(monotonic,"the expanded ramp stays monotonic");
        // Deliberately not asserted here: a step above the master peak still moves,
        // because the presenter widens the roll-off window to the expanded peak so
        // the lift is not compressed straight back out. The "refuse to expand above
        // the master" rule itself is pinned by the CPU tests, where the table can
        // be read without the curve in the way.
    }else{
        check(false,"readbacks are comparable");
    }
    // ---- colour gate: one pixel, two anchors, only the trim differs ----
    //
    // Comparing a neutral row against a saturated one would compare two different
    // luminances (the chroma offset raises R), so instead the same pixel is
    // presented under a neutral anchor and under a trimmed one. Any difference is
    // the gate and nothing else.
    {
        const auto makeAnchor = [&](pipeline::DoviFrameInfo& info, bool trimmed) {
            info = dovi;
            info.cm4.clear();
            info.cm4.count = 1;
            auto& a = info.cm4.anchors[0];
            a.targetMaxPq = uint16_t(std::lround(pipeline::cm4NitsToPq(1000.0) * 4095.0));
            a.trimChromaWeight = trimmed ? 3200 : 2048;
            a.trimSaturationGain = trimmed ? 3200 : 2048;
            for (int j = 0; j < 6; ++j) {
                a.saturationVector[std::size_t(j)] = trimmed ? 220 : 128;
                a.hueVector[std::size_t(j)] = trimmed ? 240 : 128;
            }
            a.fromLevel8 = true;
        };
        pipeline::DoviFrameInfo anchorNeutral, anchorTrimmed;
        makeAnchor(anchorNeutral, false);
        makeAnchor(anchorTrimmed, true);
        std::vector<float> neutralShown, trimmedShown;
        presenter.setFrameDovi(anchorNeutral);
        bool ok = ready && graph.applySettings(settings) && present(5);
        if (ok) neutralShown = shown;
        presenter.setFrameDovi(anchorTrimmed);
        // CB1's bounded transition now charges the colour table to the same budget
        // as the lift, so a real player eases the gate in over a few seconds. This
        // test has no frame interval, so the easing would never progress: clear the
        // previous plan with one expansion-off frame, after which the transition
        // adopts the new table directly.
        engine::EnhancementSettings off = settings;
        off.doviEnhanced = false;
        ok = ok && graph.applySettings(off) && present(6);
        ok = ok && graph.applySettings(settings) && present(7);
        if (ok) trimmedShown = shown;
        check(ok, "gated presentations succeed");
        if (ok && neutralShown.size() == shown.size()) {
            // Row 8 carries the chroma offset, so the gate has something to see.
            const double neutralLift = nitsAt(neutralShown, 12, 8) - nitsAt(plain, 12, 8);
            const double trimmedLift = nitsAt(trimmedShown, 12, 8) - nitsAt(plain, 12, 8);
            const double neutralOnlyLift = nitsAt(neutralShown, 12, 0) - nitsAt(plain, 12, 0);
            const double trimmedOnlyLift = nitsAt(trimmedShown, 12, 0) - nitsAt(plain, 12, 0);
            check(neutralOnlyLift > 150.0, "neutral anchor keeps the neutral row expanding");
            check(std::abs(trimmedOnlyLift - neutralOnlyLift) < 1.0,
                  "neutral grey is unaffected by a trimmed anchor");
            check(neutralLift > 150.0, "neutral anchor still expands the saturated row");
            check(trimmedLift < neutralLift - 1.0,
                  "a trimmed anchor expands the saturated row less");
            const auto rgbAt=[&](const std::vector<float>& v,unsigned row){
                const size_t base=(size_t(row)*64+size_t(12*4+2))*3;
                return std::array<double,3>{double(v.at(base))*80.0,double(v.at(base+1))*80.0,double(v.at(base+2))*80.0};
            };
            const auto r8=rgbAt(raw,8), r0=rgbAt(raw,0);
            std::cout<<"DV_GATE_RGB row0="<<r0[0]<<","<<r0[1]<<","<<r0[2]
                     <<" row8="<<r8[0]<<","<<r8[1]<<","<<r8[2]<<std::endl;
            std::cout << "DV_GATE_EVIDENCE neutralRow=" << neutralLift
                      << " trimmedRow=" << trimmedLift
                      << " neutralGrey=" << neutralOnlyLift
                      << " trimmedGrey=" << trimmedOnlyLift << std::endl;
        } else {
            check(false, "gated readbacks are comparable");
        }
    }

    // ---- a ceiling above the 1000-nit fallback must raise the roll-off too ----
    //
    // The expansion is sized against the stated ceiling; the roll-off closing point
    // used to be hard-wired to 1000 when no peakCapNits was set, so a 2000-nit
    // ceiling lifted the highlight and then compressed it straight back. Both frames
    // here use the same curve, so the only difference is the expansion itself.
    {
        engine::EnhancementSettings wide = settings;
        wide.hdrCurve.displayPeakNits = 2000;
        wide.hdrCurve.peakCapNits = 0;
        wide.hdrCurve.highlightStartNits = 0;
        wide.doviEnhanced = false;
        bool ok = ready && graph.applySettings(wide) && present(8);
        std::vector<float> widePlain;
        if (ok) widePlain = shown;
        wide.doviEnhanced = true;
        ok = ok && graph.applySettings(wide) && present(9);
        std::vector<float> wideShown;
        if (ok) wideShown = shown;
        check(ok, "wide-ceiling presentations succeed");
        if (ok && widePlain.size() == wideShown.size()) {
            const double wideLift = nitsAt(wideShown, 12, 0) - nitsAt(widePlain, 12, 0);
            std::cout << "DV_WIDE_CEILING lift400=" << lift400
                      << " wideLift=" << wideLift << std::endl;
            check(wideLift > lift400 * 1.2,
                  "a 2000-nit ceiling lifts the 400-nit step further than the 1000 fallback");
        } else {
            check(false, "wide-ceiling readbacks are comparable");
        }
        graph.applySettings(settings);
    }

    std::cout<<"DV_EXPAND_EVIDENCE lift400="<<lift400<<" lift203="<<lift203
        <<" lift1000="<<lift1000<<" liftDark="<<liftDark<<std::endl;
    ring.drainQueue();presenter.close();output={};graph.shutdown();av_frame_free(&frame);
    if(window)DestroyWindow(window);
    return failures?1:0;
}
int wmain(int argc,wchar_t** argv){
    CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    gfx::D3D12DeviceContext ctx;gfx::CommandSlotRing ring;Status status;
    gfx::DeviceContextDesc dd;dd.enableDebugLayer=true;
    if(!ctx.initialize(dd,status)||!ring.initialize(ctx.device(),ctx.directQueue(),ctx.fence(),ctx.fenceEvent(),4,status))return 2;
    if(argc==2&&std::wstring_view(argv[1])==L"--video-hdr")return videoHdrPreview(ctx,ring);
    if(argc==2&&std::wstring_view(argv[1])==L"--dovi-expand")return doviExpansionPreview(ctx,ring);
    int failures=0;
    const auto check=[&](bool ok,const char* label){std::cout<<(ok?"PASS ":"FAIL ")<<label<<std::endl;failures+=!ok;};
    constexpr std::array<double,16> patches={0,.005,.05,.5,1,2,5,10,20,50,100,203,400,1000,4000,10000};
    AVFrame* frame=av_frame_alloc();frame->width=64;frame->height=16;frame->format=AV_PIX_FMT_YUV420P10LE;
    frame->color_range=AVCOL_RANGE_MPEG;frame->colorspace=AVCOL_SPC_BT2020_NCL;
    frame->color_primaries=AVCOL_PRI_BT2020;frame->color_trc=AVCOL_TRC_SMPTE2084;
    if(av_frame_get_buffer(frame,32)<0)return 2;
    for(unsigned y=0;y<16;++y)for(unsigned x=0;x<64;++x){
        reinterpret_cast<uint16_t*>(frame->data[0]+y*frame->linesize[0])[x]=uint16_t(std::lround(64+876*hdrRoundTrip::pq(patches[x/4])));
        if(!(y%2)&&!(x%2))for(unsigned c=1;c<3;++c)reinterpret_cast<uint16_t*>(frame->data[c]+y/2*frame->linesize[c])[x/2]=512;
    }
    for(bool packed:{false,true}){
        pipeline::EnhanceGraph graph(ctx,ring);pipeline::EnhanceGraphDesc gd;
        gd.sourceWidth=gd.workWidth=64;gd.sourceHeight=gd.workHeight=16;
        gd.hdrInput=gd.hdrOutput=true;gd.noFeatures=gd.noNgx=true;gd.enableNr=gd.enableSr=gd.enableFg=false;
        gd.hdrOutputMode=packed?engine::HdrOutputMode::Hdr10:engine::HdrOutputMode::ScRgb;
        gd.hdrCurve.displayPeakNits=1000;
        pipeline::EnhanceGraph::FrameOutputs output;std::vector<float> raw,shown;
        bool ready=graph.initialize(gd)&&graph.createViews()&&graph.process(frame,0,true,output,1)&&hdrRoundTrip::read(ctx,ring,graph.videoFrameResource(output.videoSlot),raw);
        HWND window=CreateWindowExW(0,L"STATIC",L"PR19 HDR output diagnostic",WS_POPUP,0,0,64,16,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        engine::VideoPresenter presenter;
        ready=ready&&window&&presenter.open(ctx,window,graph);
        uint64_t epoch=1;
        const auto present=[&](int comparison=0,bool retain=false){
            bool ok=presenter.present(ctx,ring,graph,output.videoSlot,false,true,comparison,false,.5f,{epoch,1,1},{},-1,retain);
            auto buffer=presenter.presentedResourceForTest();
            ok=ok&&hdrRoundTrip::read(ctx,ring,buffer.Get(),shown);buffer.Reset();return ok;
        };
        check(ready&&present(0,true)&&shown==raw,packed?"HDR10 disabled curve: original PQ pixels exact":"scRGB disabled curve: original signed pixels exact");
        const auto idleCount=presenter.submittedCount();
        check(present(0,true)&&presenter.submittedCount()==idleCount,"unchanged paused view reuses its buffer");
        engine::EnhancementSettings settings;settings.hdrOutputMode=gd.hdrOutputMode;
        settings.hdrCurve=gd.hdrCurve;settings.hdrCurve.enabled=true;
        settings.hdrCurve.highlightStartNits=200;settings.hdrCurve.peakCapNits=400;
        ready=ready&&graph.applySettings(settings)&&present(0,true);
        check(ready&&presenter.submittedCount()>idleCount,"HDR uniform edits invalidate the paused display cache");
        check(!presenter.liveCurveCoverage().valid,"HDR coverage is silent before a panel requests it");
        presenter.observeHdrCoverage(true);
        check(present(),"visible HDR panel records a statistics probe");
        check(present(),"statistics collect without a CPU fence wait");
        const auto coverage=presenter.liveCurveCoverage();
        check(coverage.valid&&coverage.samples==2304&&coverage.inRollOff>0,"HDR10/scRGB statistics report a completed sparse sample");
        check(coverage.aboveCeiling==0,"soft highlight knee is not misreported as hard clipping");
        // A true black frame must be a valid zero reading, not a copy error.
        auto savedOutput=std::move(output);AVFrame* black=av_frame_clone(frame);
        if(black&&av_frame_make_writable(black)>=0){
            for(int y=0;y<black->height;++y)for(int x=0;x<black->width;++x)reinterpret_cast<uint16_t*>(black->data[0]+y*black->linesize[0])[x]=64;
            check(graph.process(black,10,false,output,2),"black HDR source enters the real graph");
            std::this_thread::sleep_for(std::chrono::milliseconds(260));
            check(present()&&present(),"black HDR probe completes");
            const auto zero=presenter.liveCurveCoverage();
            check(zero.valid&&zero.samples==2304&&zero.aboveCeiling==0&&zero.inRollOff==0&&zero.inLift==0,"black HDR image reports valid zero statistics");
            ++epoch;check(present()&&!presenter.liveCurveCoverage().valid,"source epoch invalidates old HDR statistics");
        }else check(false,"allocate black diagnostic frame");
        av_frame_free(&black);output=std::move(savedOutput);epoch=1;
        presenter.observeHdrCoverage(false);check(!presenter.liveCurveCoverage().valid,"hiding HDR panel clears its statistics");
        check(present(),"restore grayscale presentation after black-frame fixture");
        double maxRelative=0;bool monotonic=true;double previous=0;
        if(ready)for(unsigned i=0;i<patches.size();++i){
            const auto at=size_t(i*4+2)*3;
            const double before=packed?hdrRoundTrip::nits(raw[at]):raw[at]*80;
            const double after=packed?hdrRoundTrip::nits(shown[at]):shown[at]*80;
            // Independent expected knee, expressed in absolute nits from the
            // actual graph output (including its input/output quantization).
            const double expected=before<=200?before:200+200*(before-200)/(200+(before-200));
            if(expected>=1)maxRelative=std::max(maxRelative,std::abs(after-expected)/expected);
            monotonic=monotonic&&after+.001>=previous&&std::isfinite(after)&&after<=402;previous=after;
        }
        std::cout<<"CURVE_EVIDENCE packed="<<packed<<" maxRelative="<<maxRelative<<" monotonic="<<monotonic<<std::endl;
        check(ready&&maxRelative<.018&&monotonic,packed?"HDR10 curve: PQ decoded to nits, knee and re-encode":"scRGB curve: linear nits knee and peak bound");
        std::vector<float> untouched;
        check(hdrRoundTrip::read(ctx,ring,graph.videoFrameResource(output.videoSlot),untouched)&&untouched==raw,"preview tuning leaves graph/export surface byte-equivalent");
        settings.hdrCurve.strengthPercent=0;
        check(graph.applySettings(settings)&&present()&&shown==raw,"live zero strength restores exact identity without rebuilding");
        settings.hdrCurve.strengthPercent=100;
        check(graph.applySettings(settings)&&present(1),"linear comparison reference presents through the same curve (no PQ double decode)");
        // Reopening the same presenter instance must reset metadata and monitor
        // caches; a new graph/source never inherits the previous clip's state.
        ring.drainQueue();presenter.close();
        check(presenter.open(ctx,window,graph)&&present(),"same presenter reopen retains working HDR output");
        if(packed){
            // Exercise the live DXGI hint through ResizeBuffers, then disable
            // before another enabled render. The runner checks accepted hints
            // are followed by a clear call; unsupported hints remain optional.
            std::cout<<"METADATA_RESIZE_BEGIN"<<std::endl;
            settings.hdrMetadata.enabled=true;
            check(graph.applySettings(settings)&&present(),"optional HDR metadata does not block presentation");
            SetWindowPos(window,nullptr,0,0,96,16,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
            Sleep(120); // Native resize coalescing; diagnostic-only.
            check(present(),"HDR window resizes with metadata active");
            {const auto buffer=presenter.presentedResourceForTest();check(buffer&&buffer->GetDesc().Width==96,"resize exercised real DXGI buffers");}
            settings.hdrMetadata.enabled=false;
            check(graph.applySettings(settings)&&present(),"disable metadata immediately after resize");
            std::cout<<"METADATA_RESIZE_END"<<std::endl;
            SetWindowPos(window,nullptr,0,0,64,16,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
            Sleep(120);
            check(present(),"restore diagnostic window extent");
        }
        // BT.2020 primaries decoded into scRGB contain negative components.
        // A curve must scale signed RGB uniformly instead of changing hue.
        constexpr std::array<std::array<double,3>,4> colors={{{1000,0,0},{0,1000,0},{0,0,1000},{400,100,20}}};
        for(unsigned y=0;y<16;++y)for(unsigned x=0;x<64;++x){
            const auto& c=colors[x/16];
            const double r=hdrRoundTrip::pq(c[0]),g=hdrRoundTrip::pq(c[1]),b=hdrRoundTrip::pq(c[2]);
            const double yy=.2627*r+.678*g+.0593*b;
            reinterpret_cast<uint16_t*>(frame->data[0]+y*frame->linesize[0])[x]=uint16_t(std::lround(64+876*yy));
            if(!(y%2)&&!(x%2)){
                reinterpret_cast<uint16_t*>(frame->data[1]+y/2*frame->linesize[1])[x/2]=uint16_t(std::lround(512+896*(b-yy)/1.8814));
                reinterpret_cast<uint16_t*>(frame->data[2]+y/2*frame->linesize[2])[x/2]=uint16_t(std::lround(512+896*(r-yy)/1.4746));
            }
        }
        output={};ring.drainQueue();
        ready=graph.process(frame,16,true,output,2)&&hdrRoundTrip::read(ctx,ring,graph.videoFrameResource(output.videoSlot),raw);
        settings.hdrCurve=engine::findHdrTuningPreset("darkLift")->curve;settings.hdrCurve.strengthPercent=200;settings.hdrCurve.displayPeakNits=1000;
        ready=ready&&graph.applySettings(settings)&&present();
        const auto linear=[packed](const std::vector<float>& pixels,size_t at){
            std::array<double,3> c{pixels[at],pixels[at+1],pixels[at+2]};
            if(!packed)return c;
            for(auto& v:c)v=hdrRoundTrip::nits(v)/80;
            return std::array<double,3>{1.660491*c[0]-.587641*c[1]-.072850*c[2],-.124550*c[0]+1.132900*c[1]-.008349*c[2],-.018151*c[0]-.100579*c[1]+1.118730*c[2]};
        };
        double maxChromaError=0;bool signs=true;
        if(ready)for(unsigned i=0;i<4;++i){
            const auto at=size_t(i*16+8)*3;const auto a=linear(raw,at),b=linear(shown,at);
            const double aNorm=std::sqrt(a[0]*a[0]+a[1]*a[1]+a[2]*a[2]);
            const double bNorm=std::sqrt(b[0]*b[0]+b[1]*b[1]+b[2]*b[2]);
            if(aNorm<=0||bNorm<=0){signs=false;continue;}
            for(unsigned c=0;c<3;++c){maxChromaError=std::max(maxChromaError,std::abs(a[c]/aNorm-b[c]/bNorm));if(a[c]<-.05)signs=signs&&b[c]<0;}
        }
        std::cout<<"COLOR_EVIDENCE packed="<<packed<<" normalizedRgbError="<<maxChromaError<<" negativeSigns="<<signs<<std::endl;
        check(ready&&signs&&maxChromaError<(packed?.015:.002),"enabled HDR curve preserves wide-gamut RGB direction and negative scRGB");
        ring.drainQueue();presenter.close();DestroyWindow(window);output={};graph.shutdown();
        // Restore grayscale input for the second output contract.
        for(unsigned y=0;y<16;++y)for(unsigned x=0;x<64;++x){
            reinterpret_cast<uint16_t*>(frame->data[0]+y*frame->linesize[0])[x]=uint16_t(std::lround(64+876*hdrRoundTrip::pq(patches[x/4])));
            if(!(y%2)&&!(x%2))for(unsigned c=1;c<3;++c)reinterpret_cast<uint16_t*>(frame->data[c]+y/2*frame->linesize[c])[x/2]=512;
        }
    }
    av_frame_free(&frame);ring.drainQueue();
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> messages;
    if(SUCCEEDED(ctx.device()->QueryInterface(IID_PPV_ARGS(&messages)))){
        for(UINT64 i=0;i<messages->GetNumStoredMessages();++i){SIZE_T bytes=0;messages->GetMessage(i,nullptr,&bytes);std::vector<uint8_t> storage(bytes);auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());if(SUCCEEDED(messages->GetMessage(i,message,&bytes))&&message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++failures;std::cout<<"GPU_VALIDATION "<<message->pDescription<<std::endl;}}
    }
    std::cout<<"HDR output integration failures="<<failures<<" (GPU/DXGI pixels only; physical monitor unmeasured)"<<std::endl;
    return failures?1:0;
}
