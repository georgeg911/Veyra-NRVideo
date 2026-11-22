#include "SubtitleOverlay.h"

#include "Theme.h"
#include "veyra/Log.h"

#ifdef VEYRA_HAS_LIBASS
#include "AssSubtitleRenderer.h"
#include <array>
#endif

#include <algorithm>
#include <cmath>
#include <format>
#include <memory>
#include <string>
#include <vector>

namespace veyra::ui {
namespace {
LRESULT CALLBACK proc(HWND h,UINT msg,WPARAM wp,LPARAM lp){
    if(msg==WM_NCHITTEST)return HTTRANSPARENT;
    if(msg==WM_PAINT){PAINTSTRUCT ps{};BeginPaint(h,&ps);EndPaint(h,&ps);return 0;}
    return DefWindowProcW(h,msg,wp,lp);
}

int toFontStyle(bool bold,bool italic){
    int style=Gdiplus::FontStyleRegular;
    if(bold)style|=Gdiplus::FontStyleBold;
    if(italic)style|=Gdiplus::FontStyleItalic;
    return style;
}
// numpad layout: column 1/2/3 = left/centre/right, row 1/2/3 = bottom/middle/top
Gdiplus::StringAlignment horizontalOf(int alignment){const int column=(alignment-1)%3;return column==0?Gdiplus::StringAlignmentNear:column==2?Gdiplus::StringAlignmentFar:Gdiplus::StringAlignmentCenter;}
Gdiplus::StringAlignment verticalOf(int alignment){const int row=(alignment-1)/3;return row==0?Gdiplus::StringAlignmentFar:row==2?Gdiplus::StringAlignmentNear:Gdiplus::StringAlignmentCenter;}

std::wstring signatureOf(const std::vector<SubtitleLine>& lines,const SubtitleView& view,const RECT& rect){
    std::wstring signature;
    signature.reserve(lines.size()*64);
    for(const auto& line:lines){
        signature+=std::format(L"{}:{}|{:08X}|{:08X}|{}:{}|{}|{}|{}|{}|",line.text.size(),line.text,line.style.primary,line.style.outline,line.style.font.size(),line.style.font,
            line.style.size,line.style.alignment,line.alignOverride,line.secondary?1:0);
        signature+=std::format(L"{}:{}:{}:{}|",reinterpret_cast<uintptr_t>(line.bitmap.get()),reinterpret_cast<uintptr_t>(line.ass.get()),line.posX,line.posY);
        const auto& s=line.style;
        signature+=std::format(L"{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|",s.back,s.outlineWidth,s.shadow,s.marginL,s.marginR,s.marginV,s.bold,s.italic,s.background,s.font.size());
    }
    signature+=std::format(L"#{}|{}:{}|{}|{}|{}|{}",view.scale,view.fontOverride.size(),view.fontOverride,view.outline?1:0,view.background?1:0,view.bottomMargin,view.blockGap);
    signature+=std::format(L"/{}x{}/{}/{}",rect.right,rect.bottom,view.targetLines,view.fitToLines);
    signature+=std::format(L"/{}/{}/{}/{}/{}",view.preview.zoom,view.preview.centerX,view.preview.centerY,view.videoWidth,view.videoHeight);
    return signature;
}

struct Line {
    std::unique_ptr<Gdiplus::GraphicsPath> path;
    Gdiplus::RectF bounds{};
    engine::SubtitleStyle style;
    bool positioned=false;
    float positionX=0,positionY=0;
};
} // namespace

HWND createSubtitleOverlay(HWND parent){
    WNDCLASSW wc{};wc.lpfnWndProc=proc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"VeyraSubtitleOverlay";RegisterClassW(&wc);
    return CreateWindowExW(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE,wc.lpszClassName,L"",WS_CHILD,0,0,1,1,parent,nullptr,wc.hInstance,nullptr);
}

void updateSubtitleOverlay(HWND h,const std::vector<SubtitleLine>& lines,const SubtitleView& view){
    RECT rect{};GetClientRect(h,&rect);
    static thread_local std::wstring lastSignature;
    static thread_local HWND lastWindow=nullptr;
    static thread_local std::vector<std::shared_ptr<const engine::SubtitleBitmapFrame>> lastBitmaps;
    if(h!=lastWindow){lastSignature.clear();lastBitmaps.clear();lastWindow=h;}
    if(lines.empty()||rect.right<1||rect.bottom<1){lastSignature.clear();lastBitmaps.clear();ShowWindow(h,SW_HIDE);SetWindowTextW(h,L"");return;}
    const int width=rect.right,height=rect.bottom;
    auto signature=signatureOf(lines,view,rect)+std::format(L"/dpi{}",GetDpiForWindow(h));
#ifdef VEYRA_HAS_LIBASS
    // libass decides whether its frame changed (animation, karaoke, new
    // events); the rest of the signature covers geometry and other lines.
    static thread_local std::array<std::unique_ptr<AssSubtitleRenderer>,2> assRenderers;
    static thread_local uint64_t assGeneration=0;
    struct AssPlacement {AssSubtitleRenderer* renderer=nullptr;int x=0,y=0;};
    std::vector<AssPlacement> assPlaced;
    for(const auto& line:lines){
        if(!line.ass)continue;
        auto& renderer=assRenderers[line.secondary?1:0];
        if(!renderer)renderer=std::make_unique<AssSubtitleRenderer>();
        if(!renderer->prepare(line.ass))continue;
        const double vw=view.videoWidth>0?view.videoWidth:width,vh=view.videoHeight>0?view.videoHeight:height;
        const double fit=std::min(width/vw,height/vh)*view.preview.zoom;
        const int frameW=int(std::lround(vw*fit)),frameH=int(std::lround(vh*fit));
        const int ox=int(std::lround(width*.5-view.preview.centerX*vw*fit)),oy=int(std::lround(height*.5-view.preview.centerY*vh*fit));
        const int change=renderer->render(int64_t(std::llround(line.assTimeMs)),frameW,frameH,int(vw),int(vh),view.scale);
        if(change>0)++assGeneration;
        signature+=std::format(L"/ass{}:{}:{}x{}@{},{}#{}",line.secondary?1:0,change>=0,frameW,frameH,ox,oy,assGeneration);
        if(change>=0)assPlaced.push_back({renderer.get(),ox,oy});
    }
#endif
    if(lastSignature==signature){ShowWindow(h,SW_SHOWNOACTIVATE);return;}
    // The DIB is reused while the overlay size is unchanged; zoom/drag only
    // re-renders into it (a 4K DIB is ~33 MB, re-allocating it per tick stalled
    // middle-button drags).
    static thread_local HBITMAP cachedBitmap=nullptr;static thread_local void* cachedBits=nullptr;static thread_local int cachedW=0,cachedH=0;
    HDC screen=GetDC(nullptr),memory=CreateCompatibleDC(screen);
    if(!cachedBitmap||cachedW!=width||cachedH!=height){
        if(cachedBitmap){DeleteObject(cachedBitmap);cachedBitmap=nullptr;cachedBits=nullptr;}
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;
        info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
        cachedBitmap=CreateDIBSection(screen,&info,DIB_RGB_COLORS,&cachedBits,nullptr,0);
        if(!cachedBitmap||!cachedBits){if(cachedBitmap)DeleteObject(cachedBitmap);cachedBitmap=nullptr;cachedBits=nullptr;DeleteDC(memory);ReleaseDC(nullptr,screen);return;}
        cachedW=width;cachedH=height;
    }
    auto bitmap=cachedBitmap;void* bits=cachedBits;
    auto previousObject=SelectObject(memory,bitmap);
    memset(bits,0,size_t(width)*height*4);
    {
        using namespace Gdiplus;
        Bitmap canvas(width,height,width*4,PixelFormat32bppPARGB,static_cast<BYTE*>(bits));
        Graphics graphics(&canvas);
        graphics.SetSmoothingMode(SmoothingModeAntiAlias);
        graphics.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        const float padding=float(dip(h,6));
        const float layoutWidth=std::max(1.0f,float(width)-2*padding);
        FontFamily fallbackFamily(L"Microsoft YaHei UI");   // per call: never survives GdiplusShutdown
        std::vector<Line> laid;
        laid.reserve(lines.size());
        for(const auto& line:lines){
            if(line.ass)continue;   // drawn by libass below (or skipped in builds without it)
            if(line.bitmap){
                const auto& frame=*line.bitmap;
                const double vw=view.videoWidth>0?view.videoWidth:frame.width,vh=view.videoHeight>0?view.videoHeight:frame.height;
                if(frame.width<=0||frame.height<=0||vw<=0||vh<=0)continue;
                const double fit=std::min(width/vw,height/vh)*view.preview.zoom;
                const double sx=vw*fit/frame.width,sy=vh*fit/frame.height;
                const double ox=width*.5-view.preview.centerX*vw*fit,oy=height*.5-view.preview.centerY*vh*fit;
                graphics.SetInterpolationMode(InterpolationModeHighQualityBilinear);
                for(const auto& image:frame.images){
                    auto pixels=image.pixels();if(pixels.empty())continue;
                    Bitmap source(image.width,image.height,image.width*4,PixelFormat32bppPARGB,reinterpret_cast<BYTE*>(pixels.data()));
                    graphics.DrawImage(&source,RectF(float(ox+image.x*sx),float(oy+image.y*sy),float(image.width*sx),float(image.height*sy)),0,0,float(image.width),float(image.height),UnitPixel);
                }
                continue;
            }
            if(line.text.empty())continue;
            // 双语字幕（一个 cue 里 '\n' 分隔的两行）按语言给出不同字号：
            // 第一行是中文（同时也是主语言），放大一点；第二行是英文译文，缩小一点。
            // 两行同字号时英文看起来比中文「重」，因为它更宽、占的横向面积更大，
            // 视觉上会抢掉中文那一行。
            // Bilingual cues (two lines joined by '\n' in one cue) get different sizes per
            // language: the first line is Chinese (also the primary language) and is enlarged,
            // the second is the English translation and is shrunk. At equal sizes the English
            // line looks heavier simply because it is wider, and it visually steals attention
            // from the Chinese one.
            const size_t newlinePos=line.text.find(L'\n');
            const bool bilingualCue=newlinePos!=std::wstring::npos&&newlinePos+1<line.text.size();
            Line entry;
            entry.path=std::make_unique<Gdiplus::GraphicsPath>();
            entry.style=line.style;
            if(line.alignOverride>=1&&line.alignOverride<=9)entry.style.alignment=line.alignOverride;
            if(entry.style.alignment<1||entry.style.alignment>9)entry.style.alignment=2;
            const double logicalSize=std::max(8.0,(line.style.size>0?line.style.size:24.0)*view.scale);
            const std::wstring family=!view.fontOverride.empty()?view.fontOverride:(line.style.font.empty()?L"Microsoft YaHei UI":line.style.font);
            FontFamily requested(family.c_str());
            FontFamily* use=requested.IsAvailable()?&requested:&fallbackFamily;
            if(!requested.IsAvailable())entry.style.font=L"Microsoft YaHei UI";
            StringFormat format;
            // 多行 cue（双语字幕就是一个 cue 里用 '\n' 拼中文和英文两行）的每一行
            // 都按这个 format 的水平对齐绘制。写死 Near 会让两行都靠左，即使整体
            // 位置是居中的——看上去就是「中文一行偏左、英文一行也偏左」，两行左边缘
            // 对齐但整块不在屏幕中间。改用该行实际的对齐方式，两行就各自居中。
            // A multi-line cue (bilingual subtitles are Chinese and English joined with
            // '\n' in one cue) draws every line using this format's horizontal alignment.
            // Hard-coding Near left-aligned both lines even when the block itself was
            // centred: the two lines lined up on their left edges but the block sat off
            // centre. Use the line's actual alignment so each line is centred on its own.
            format.SetAlignment(horizontalOf(entry.style.alignment));
            format.SetLineAlignment(StringAlignmentNear);
            format.SetFormatFlags(StringFormatFlagsNoClip|StringFormatFlagsMeasureTrailingSpaces);
            // 双语 cue 拆成两条独立的 entry 分别排版：中文那行按原字号（稍放大），
            // 英文那行缩小一些。这样两行各有自己的 GraphicsPath，堆叠逻辑会把它们
            // 当作两个块上下排列，而同一个 path 里的两行只能共用一个字号。
            // A bilingual cue is laid out as two separate entries: the Chinese line at the
            // user's size (slightly enlarged), the English one smaller. Each gets its own
            // GraphicsPath so the stacking logic places them as two blocks; lines inside one
            // path can only share a single font size.
            const bool positioned=line.posX>=0&&line.posY>=0;
            std::wstring primaryText=line.text;
            std::wstring secondaryText;
            if(bilingualCue){
                primaryText=line.text.substr(0,newlinePos);
                secondaryText=line.text.substr(newlinePos+1);
            }
            auto layOne=[&](std::wstring text,double sizeScale,Line& target,const std::wstring& textOverride){
                target.path=std::make_unique<Gdiplus::GraphicsPath>();
                target.style=entry.style;
                const auto& renderText=textOverride;
                const float availableWidth=positioned?layoutWidth:std::max(1.0f,layoutWidth-float(dip(h,std::max(0.0,target.style.marginL)+std::max(0.0,target.style.marginR))));
                float fontPixels=float(dip(h,logicalSize*sizeScale));
                auto measureOne=[&](float size){Font font(use,size,toFontStyle(target.style.bold,target.style.italic),UnitPixel);RectF bounds;
                    graphics.MeasureString(renderText.c_str(),int(renderText.size()),&font,RectF(0,0,availableWidth,1e7f),&format,&bounds);return bounds;};
                // 宽度约束：长行不能横向冲出画面（英文往往比中文宽得多）。
                // Width constraint: a long line must not run off the side (English is usually
                // much wider than Chinese).
                const float minimum=float(dip(h,10));
                for(int attempt=0;attempt<16&&fontPixels>minimum;++attempt){
                    if(measureOne(fontPixels).Width<=availableWidth+1)break;
                    fontPixels=std::max(minimum,fontPixels*.92f);
                }
                const auto measured=measureOne(fontPixels);
                target.path->AddString(renderText.c_str(),int(renderText.size()),use,toFontStyle(target.style.bold,target.style.italic),fontPixels,RectF(0,0,availableWidth,std::max(measured.Height+fontPixels*2,1.0f)),&format);
                target.path->GetBounds(&target.bounds);
                if(positioned){
                    target.positioned=true;
                    target.positionX=float(line.posX)*float(width);
                    target.positionY=float(line.posY)*float(height);
                }
            };
            if(bilingualCue){
                // 中文行放大 8%，英文行缩到 78%：英文更宽，同字号时视觉上会压过中文。
                // Chinese up 8%, English down to 78%: English is wider and at the same size it
                // visually overpowers the Chinese line.
                Line zh;layOne(primaryText,1.08,zh,primaryText);
                Line en;layOne(secondaryText,0.78,en,secondaryText);
                laid.push_back(std::move(zh));
                laid.push_back(std::move(en));
            }else{
                Line only;layOne(primaryText,1.0,only,primaryText);
                laid.push_back(std::move(only));
            }
        }
        // The caller passes secondary lines first, primary last: the loop below
        // lays them out bottom-up, so the primary cue ends up on the bottom.
        float cursor=std::max(padding+1,float(height)-padding-float(dip(h,view.bottomMargin)));
        float totalHeight=0;size_t stacked=0;
        for(const auto& entry:laid)if(!entry.positioned){totalHeight+=entry.bounds.Height;++stacked;}
        const float gap=stacked>1?std::min(float(dip(h,view.blockGap)),std::max(0.0f,(cursor-padding)/float(stacked*2))):0;
        const float space=std::max(1.0f,cursor-padding-gap*float(stacked?stacked-1:0));
        const float scale=totalHeight>space?space/totalHeight:1;
        if(scale<1)for(auto& entry:laid)if(!entry.positioned){Matrix shrink;shrink.Scale(scale,scale);entry.path->Transform(&shrink);entry.path->GetBounds(&entry.bounds);}
        float middleHeight=0;size_t middleCount=0;
        for(const auto& entry:laid)if(!entry.positioned&&verticalOf(entry.style.alignment)==StringAlignmentCenter){middleHeight+=entry.bounds.Height;++middleCount;}
        if(middleCount>1)middleHeight+=gap*float(middleCount-1);
        float topCursor=padding,middleCursor=std::max(padding,(float(height)-middleHeight)/2);
        for(size_t index=laid.size();index-->0;){
            auto& entry=laid[index];
            const int alignment=(entry.style.alignment>=1&&entry.style.alignment<=9)?entry.style.alignment:2;
            const float blockWidth=entry.bounds.Width,blockHeight=entry.bounds.Height;
            float x=padding,y=cursor-blockHeight;
            if(entry.positioned){
                const auto horizontal=horizontalOf(alignment),vertical=verticalOf(alignment);
                x=entry.positionX-(horizontal==StringAlignmentNear?0.0f:horizontal==StringAlignmentFar?blockWidth:blockWidth/2);
                y=entry.positionY-(vertical==StringAlignmentNear?0.0f:vertical==StringAlignmentFar?blockHeight:blockHeight/2);
                y=std::clamp(y,0.0f,std::max(0.0f,float(height)-blockHeight));
                x=std::clamp(x,0.0f,std::max(0.0f,float(width)-blockWidth));
            }else{
                const auto vertical=verticalOf(alignment);
                const float marginV=float(dip(h,std::max(0.0,entry.style.marginV)));
                if(vertical==StringAlignmentNear){y=std::max(topCursor,padding+marginV);topCursor=y+blockHeight+gap;}
                else if(vertical==StringAlignmentCenter){y=middleCursor;middleCursor=y+blockHeight+gap;}
                else{y=std::min(cursor,float(height)-padding-float(dip(h,view.bottomMargin))-marginV)-blockHeight;cursor=y-gap;}
                const float marginL=float(dip(h,entry.style.marginL));
                const float marginR=float(dip(h,entry.style.marginR));
                const float left=padding+marginL,right=float(width)-padding-marginR;
                const auto horizontal=horizontalOf(alignment);
                x=horizontal==StringAlignmentNear?left:horizontal==StringAlignmentFar?right-blockWidth:left+(right-left-blockWidth)/2;
            }
            Matrix translation;
            translation.Translate(x-entry.bounds.X,y-entry.bounds.Y);
            entry.path->Transform(&translation);
            entry.bounds.X=x;entry.bounds.Y=y;
        }
        for(const auto& entry:laid){
            const auto& style=entry.style;
            const Color primary(Color::MakeARGB(byte((style.primary>>24)&0xFF),byte((style.primary>>16)&0xFF),byte((style.primary>>8)&0xFF),byte(style.primary&0xFF)));
            if(view.background||style.background){
                const float inflate=float(dip(h,2));
                RectF box(entry.bounds.X-inflate,entry.bounds.Y-inflate,entry.bounds.Width+2*inflate,entry.bounds.Height+2*inflate);
                SolidBrush back(Color::MakeARGB(byte((style.back>>24)&0xFF),byte((style.back>>16)&0xFF),byte((style.back>>8)&0xFF),byte(style.back&0xFF)));
                graphics.FillRectangle(&back,box);
            }
            const float shadow=float(dip(h,style.shadow>0?style.shadow:(view.outline?1.8:0.0)));
            if(shadow>0.5f){
                GraphicsPath shadowPath;
                shadowPath.AddPath(entry.path.get(),FALSE);
                Matrix offset;
                offset.Translate(shadow,shadow);
                shadowPath.Transform(&offset);
                SolidBrush shadowBrush(Color(150,0,0,0));
                graphics.FillPath(&shadowBrush,&shadowPath);
            }
            if(view.outline){
                Pen outline(Color::MakeARGB(byte((style.outline>>24)&0xFF),byte((style.outline>>16)&0xFF),byte((style.outline>>8)&0xFF),byte(style.outline&0xFF)),
                            float(dip(h,style.outlineWidth>0?style.outlineWidth:2.4)));
                outline.SetLineJoin(LineJoinRound);
                graphics.DrawPath(&outline,entry.path.get());
            }
            SolidBrush fill(primary);
            graphics.FillPath(&fill,entry.path.get());
        }
        graphics.Flush(FlushIntentionSync);
        lastSignature=signature;
        // Retain the identities used by the cache until its signature changes.
        lastBitmaps.clear();
        for(const auto& line:lines)if(line.bitmap)lastBitmaps.push_back(line.bitmap);
    }
#ifdef VEYRA_HAS_LIBASS
    // After GDI+ released the DIB: other subtitle lines are composed already.
    for(const auto& placed:assPlaced)placed.renderer->composite(static_cast<uint32_t*>(bits),width,height,placed.x,placed.y);
#endif
    SIZE dimensions{width,height};POINT origin{};BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
    if(UpdateLayeredWindow(h,screen,nullptr,&dimensions,memory,&origin,0,&blend,ULW_ALPHA)){
        SetWindowTextW(h,L"subtitle");
        ShowWindow(h,SW_SHOWNOACTIVATE);
        if(!GetPropW(h,L"subtitle.logged")){
            log::info("subtitle",std::format("layered update succeeded extent={}x{} lines={}",width,height,lines.size()));
            SetPropW(h,L"subtitle.logged",HANDLE(1));
        }
    }else{
        lastSignature.clear();
        if(!GetPropW(h,L"subtitle.error")){
            log::error("subtitle",std::format("UpdateLayeredWindow failed error={}",GetLastError()));
            SetPropW(h,L"subtitle.error",HANDLE(1));
        }
    }
    SelectObject(memory,previousObject);DeleteDC(memory);ReleaseDC(nullptr,screen);
}
} // namespace veyra::ui
