// PresentBlit: pixel-shader blit of the working frame onto a flip-model
// swapchain back buffer. Flip back buffers may only transition between
// PRESENT and RENDER_TARGET (not UAV), so the present path is a fullscreen
// triangle sample instead of a compute write.

#include "HdrColor.hlsli"
#include "PresentationRotation.hlsli"
Texture2D<float4> sourceTex : register(t0);

cbuffer PresentBlitConstants : register(b0)
{
    // 64 = 输出前加抖动（12 位源量化到 10 位交换链时抑制色带）。
    // 64 = dither before output (suppresses banding when a 12-bit source is quantised
    // onto a 10-bit swapchain).
    float4 srcDims; // z flags: 1=sRGB encode, 2=fine, 4=PQ encode, 8=SDR white, 16=HDR curve, 32=PQ, 64/128=rotation, 256=dither decode
    float4 dstDims; // x/y=client extent z/w=view center
    // Custom: static HDR brightness curve, applied on absolute nits before the PQ
    // encode. See veyra/engine/HdrOutputTuning.h for the contract; every field is
    // a no-op at its default (lift 0, mid 203, start 0, cap 0).
    float4 curve0;  // x=shadow lift EV, y=shadow range nits, z=mid-gray nits, w=highlight start nits
    float4 curve1;  // x=roll-off scale nits, y=peak cap nits (0 = off)
                    // Custom (完整 B): z=DV master peak nits, w=DV expansion ceiling nits
                    // (both 0 while the per-pixel expansion is off).
};

// Custom (完整 B): Dolby Vision per-pixel highlight expansion.
//
// 257 entries of "nits of lift", packed four to a float4 (entry k -> k>>2, lane
// k&3) and sent as a constant-buffer root descriptor, not a texture: the host
// rebuilds the whole table every frame, and a dynamic index into a cbuffer is a
// constant load rather than a sampled fetch. The table is derived on the CPU
// (veyra/pipeline/DoviEnhancedCurve.h) from the same L1 levels the follow curve
// uses, so nothing here has to know about Dolby Vision itself.
cbuffer DoviExpansionTable : register(b1)
{
    float4 doviExtra[65];   // 260 floats; entries 0..256 are used
    // Custom (完整 C): CB1's colour restraint. The 65-point circular hue table
    // follows the expansion entries, then the two global coefficients.
    float4 doviHue[17];     // 65 floats used
    float4 doviColor;       // x=chroma, y=saturation, z=1 when the table is live
};

float DoviHueAt(int k)
{
    const float4 v = doviHue[k >> 2];
    const int lane = k & 3;
    return lane == 0 ? v.x : (lane == 1 ? v.y : (lane == 2 ? v.z : v.w));
}

// ST2084 encode, matching cm4NitsToPq on the CPU side.
float DoviPq(float nits)
{
    const float y = pow(saturate(nits / 10000.0), 2610.0 / 16384.0);
    return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), 2523.0 / 32.0);
}

// CB1's chroma gate. The lift is one luminance gain across all three channels, so
// a saturated highlight would carry its colour past what the display can show.
// Returns the fraction of the lift that survives, bounded to [0.65, 1]; neutral
// greys bypass the transform, exactly like doviColorRestraint() on the CPU.
float DoviColorRestraint(float3 nits, float masterNits)
{
    if (doviColor.z < 0.5) return 1.0;
    if (all(nits == nits.rrr)) return 1.0;
    static const float3x3 rgb2lms = {
        0.4122214708, 0.5363325363, 0.0514459929,
        0.2119034982, 0.6806995451, 0.1073969566,
        0.0883024619, 0.2817188376, 0.6299787005
    };
    const float3 lms = mul(rgb2lms, max(nits, 0.0));
    const float3 v = float3(DoviPq(lms.x), DoviPq(lms.y), DoviPq(lms.z));
    const float intensity = dot(v, float3(0.4, 0.4, 0.2));
    if (!(intensity > 1e-6)) return 1.0;
    const float ct = dot(v, float3(4.455, -4.851, 0.396));
    const float cp = dot(v, float3(0.8056, 0.3572, -1.1628));
    const float gate = smoothstep(0.0, 0.15, length(float2(ct, cp)) / intensity);
    const float z = saturate(intensity / DoviPq(masterNits));
    const float pos = (atan2(cp, ct) + 3.14159265) / 6.2831853 * 64.0;
    const int idx = min(int(pos), 63);
    const float frac = saturate(pos - float(idx));
    const float hue = lerp(DoviHueAt(idx), DoviHueAt(idx + 1), frac);
    const float severity = doviColor.x * abs(2.0 * z - 1.0) + doviColor.y + hue;
    return max(0.65, 1.0 / (1.0 + 0.5 * gate * severity));
}

float DoviExtraAt(int k)
{
    const float4 v = doviExtra[k >> 2];
    const int lane = k & 3;
    return lane == 0 ? v.x : (lane == 1 ? v.y : (lane == 2 ? v.z : v.w));
}

// CB1's cb1_dv_enhanced_pixel: look the lift up by the BT.2020 luminance, turn it
// into a ratio of that luminance, restrain it by the creative anchors' colour
// table (DoviColorRestraint), then cap the ratio with a soft knee so expansion
// never overshoots the display ceiling in a single step. `dvp.x` is the master peak the table is indexed against, `dvp.y`
// the ceiling the picture may reach.
float3 DoviExpand(float3 nits, float4 dvp)
{
    const float y = dot(nits, float3(.2627, .6780, .0593));
    if (y <= 10.0 || dvp.x <= 10.0) return nits;
    const float u = saturate((y - 10.0) / max(dvp.x - 10.0, 1e-3)) * 256.0;
    const int index = min(int(u), 255);
    float extra = lerp(DoviExtraAt(index), DoviExtraAt(index + 1), u - float(index));
    if (!(extra > 0.0)) return nits;
    // Custom (完整 C): CB1 applies the colour gate before the soft knee.
    extra *= DoviColorRestraint(nits, dvp.x);
    // The knee is measured on the brightest channel, so a saturated highlight is
    // protected exactly like a neutral one.
    const float peak = max(max(nits.r, nits.g), nits.b);
    const float headroom = max(0.0, dvp.y / max(peak, 1e-3) - 1.0);
    const float knee = 0.9 * headroom, width = 0.1 * headroom;
    float delta = extra / y;   // the lift as a multiple of this pixel's luminance
    if (delta > knee) {
        const float tail = delta - knee;
        delta = knee + (width + tail > 0.0 ? width * tail / (width + tail) : 0.0);
    }
    return nits * (1.0 + delta);
}

// Lift the shadows without touching the highlights, move the mid-gray reference
// with the peak held fixed, then roll off and cap the top end.
float HdrCurveLuminance(float nits, float4 c0, float4 c1)
{
    if (c0.x != 0.0 && c0.y > 0.0) {
        const float w = saturate(1.0 - nits / c0.y);
        nits *= 1.0 + w * w * (exp2(c0.x) - 1.0);
    }
    if (c0.z > 0.0 && abs(c0.z - 203.0) > 0.5) {
        const float peak = c1.y > 0.0 ? c1.y : 1000.0;
        if (peak > c0.z + 1.0) {
            const float g = log(c0.z / peak) / log(203.0 / peak);
            nits = peak * pow(max(nits / peak, 0.0), g);
        }
    }
    if (c0.w > 0.0 && c1.x > 0.0 && nits > c0.w) nits = c0.w + (nits - c0.w) / (1.0 + (nits - c0.w) / c1.x);
    if (c1.y > 0.0) nits = min(nits, c1.y);
    return nits;
}

float3 HdrCurve(float3 nits, float4 c0, float4 c1)
{
    // A per-channel gamma/knee changes hue and clipping negative scRGB removes
    // BT.2020 colors outside BT.709. Apply one luminance gain to all components.
    const float y = dot(nits, float3(.212639, .715169, .072192));
    if (y <= 1e-6) return nits;
    float3 mapped = nits * (HdrCurveLuminance(y, c0, c1) / y);
    if (c1.y > 0.0) {
        const float3 rgb2020 = HdrTo2020(mapped);
        const float peak = max(max(rgb2020.r, rgb2020.g), rgb2020.b);
        if (peak > c1.y) mapped *= c1.y / peak;
    }
    return mapped;
}

SamplerState linearClamp : register(s0);

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut vsMain(uint vertexId : SV_VertexID)
{
    VSOut o;
    const float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
}

float4 CubicWeights(float t)
{
    float t2=t*t,t3=t2*t;
    return float4(-0.5*t+t2-0.5*t3,1.0-2.5*t2+1.5*t3,
                  0.5*t+2.0*t2-1.5*t3,-0.5*t2+0.5*t3);
}

float4 FineSample(float2 uv)
{
    uint width,height;sourceTex.GetDimensions(width,height);
    float2 p=uv*float2(width,height)-0.5;
    // A cubic magnifier is not an antialiased minifier. Preserve the legacy
    // minification path instead of applying an undersized kernel to downscale.
    float footprint=max(length(ddx(p)),length(ddy(p)));
    if(footprint>1.001)return sourceTex.SampleLevel(linearClamp,uv,0);
    int2 last=int2(width,height)-1;
    if(all(abs(p-round(p))<0.0005))return sourceTex.Load(int3(clamp(int2(round(p)),int2(0,0),last),0));
    int2 base=int2(floor(p));float2 t=frac(p);
    float4 wx=CubicWeights(t.x),wy=CubicWeights(t.y);
    float4 result=0;
    [unroll]for(int y=0;y<4;++y){
        [unroll]for(int x=0;x<4;++x){
            int2 q=clamp(base+int2(x-1,y-1),int2(0,0),last);
            result+=sourceTex.Load(int3(q,0))*wx[x]*wy[y];
        }
    }
    // Bound cubic lobes to the local 2x2 neighborhood to avoid bright/dark
    // halos. Keep negative/HDR values when present in the actual neighborhood.
    float4 a=sourceTex.Load(int3(clamp(base,int2(0,0),last),0));
    float4 b=sourceTex.Load(int3(clamp(base+int2(1,0),int2(0,0),last),0));
    float4 c=sourceTex.Load(int3(clamp(base+int2(0,1),int2(0,0),last),0));
    float4 d=sourceTex.Load(int3(clamp(base+int2(1,1),int2(0,0),last),0));
    return clamp(result,min(min(a,b),min(c,d)),max(max(a,b),max(c,d)));
}

// Ordered dither for the 12 -> 10 bit quantisation.
//
// The reconstruction produces 12 bits and the swapchain takes 10 (R10G10B10A2_UNORM).
// A 16-float texture is written by the shader and the hardware truncates it to 10 bits
// when the swapchain is resolved, so a smooth gradient that crosses one 10-bit step
// shows as a visible band. Adding a sub-LSB offset before that truncation turns the
// step into noise the eye averages away.
//
// The pattern is an 8x8 interleaved gradient noise (Jimenez 2014): cheap, no texture,
// and its visual character is stable under motion - a random offset would crawl.
// The amplitude is exactly one 10-bit step (1/1023) scaled to the shader's output
// domain, so the dither never moves a value by more than half a step either way.
float DitherOffset(uint2 pixel)
{
    float noise = frac(52.9829189 * frac(dot(float2(pixel), float2(0.06711056, 0.00583715))));
    return (noise - 0.5) / 1023.0;
}

float4 psMain(VSOut input) : SV_Target
{
    // zoom==0 retains the original contract for diagnostic harness callers.
    float2 uv=input.uv;
    if(srcDims.w>0){
        float fit=min(dstDims.x/srcDims.x,dstDims.y/srcDims.y)*srcDims.w;
        uv=(input.uv-.5)*dstDims.xy/(srcDims.xy*fit)+dstDims.zw;
        if(any(uv<0)||any(uv>1))return float4(0,0,0,(uint(srcDims.z+0.5)&256)?0:1);
    }
    uint flags=uint(srcDims.z+0.5);
    uv=PresentationSourceUv(uv,(flags>>6)&3);
    float4 color=(flags&2)?FineSample(uv):sourceTex.SampleLevel(linearClamp,uv,0);
    if((flags&256)&&color.a>1e-6)color.rgb/=color.a;
    if(flags&32)color.rgb=HdrDecodePq(color.rgb);
    if(flags&8)color.rgb*=203.0/80.0; // SDR comparison white in an HDR output.
    // Custom: static HDR curve, on absolute nits (the working space is linear
    // scRGB, 1.0 = 80 nits), before the transfer function is encoded. The
    // per-pixel Dolby Vision expansion runs first, in the source's own nits, so
    // the curve then rolls off the expanded highlight instead of the original one.
    if(flags&16){
        float3 nits=color.rgb*80.0;
        if(flags&1024)nits=DoviExpand(nits,float4(curve1.z,curve1.w,0,0));
        color.rgb=HdrCurve(nits,curve0,curve1)/80.0;
    }
    if(flags&1){float3 c=max(color.rgb,0);color.rgb=lerp(1.055*pow(c,1.0/2.4)-0.055,c*12.92,step(c,0.0031308));}
    if(flags&4)color.rgb=HdrEncodePq(color.rgb);
    if(flags&256)color.rgb*=color.a;
    // Custom dither (moved off 256, which now carries the alpha round trip): the
    // offset belongs after the transfer encode, where one 10-bit step is exact.
    if(flags&512){
        const int2 p=int2(input.pos.xy);
        color.rgb=saturate(color.rgb+DitherOffset(uint2(p)));
    }
    return color;
}
