#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#endif
namespace vkr {
// BT.709 limited-range NV12 -> BGRA, identical integer rounding on all paths.
inline void nv12ToBgra(const uint8_t* yPlane,size_t yStride,const uint8_t* uvPlane,
                      size_t uvStride,uint8_t* dst,size_t dstStride,unsigned w,unsigned h) {
    for(unsigned y=0;y<h;++y) {
        const auto* row=yPlane+y*yStride;const auto* uv=uvPlane+(y/2)*uvStride;
        auto* out=dst+y*dstStride;unsigned x=0;
#if defined(__SSE2__) || defined(_M_X64)
        const auto zero=_mm_setzero_si128();
        const auto cy=_mm_set1_epi32(298),cr=_mm_set1_epi32(459),cb=_mm_set1_epi32(541);
        const auto cu=_mm_set1_epi32(55),cv=_mm_set1_epi32(136);
        const auto bias=_mm_set1_epi32(128);
        for(;x+4<=w;x+=4) {
            uint32_t yy;std::memcpy(&yy,row+x,4);
            auto y16=_mm_sub_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128(yy),zero),_mm_set1_epi16(16));
            auto luma=_mm_add_epi32(_mm_madd_epi16(_mm_unpacklo_epi16(y16,zero),cy),bias);
            const int u0=int(uv[x])-128,v0=int(uv[x+1])-128,u1=int(uv[x+2])-128,v1=int(uv[x+3])-128;
            auto u=_mm_set_epi16(0,u1,0,u1,0,u0,0,u0);
            auto v=_mm_set_epi16(0,v1,0,v1,0,v0,0,v0);
            auto r=_mm_srai_epi32(_mm_add_epi32(luma,_mm_madd_epi16(v,cr)),8);
            auto b=_mm_srai_epi32(_mm_add_epi32(luma,_mm_madd_epi16(u,cb)),8);
            auto g=_mm_srai_epi32(_mm_sub_epi32(_mm_sub_epi32(luma,_mm_madd_epi16(u,cu)),_mm_madd_epi16(v,cv)),8);
            auto rb=_mm_packus_epi16(_mm_packs_epi32(r,zero),zero);
            auto gb=_mm_packus_epi16(_mm_packs_epi32(g,zero),zero);
            auto bb=_mm_packus_epi16(_mm_packs_epi32(b,zero),zero);
            auto bg=_mm_unpacklo_epi8(bb,gb);
            auto ra=_mm_unpacklo_epi8(rb,_mm_set1_epi8(-1));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(out+x*4),_mm_unpacklo_epi16(bg,ra));
        }
#endif
        for(;x<w;++x) {
            int yy=int(row[x])-16,u=int(uv[x&~1u])-128,v=int(uv[(x&~1u)+1])-128;
            out[x*4+2]=uint8_t(std::clamp((298*yy+459*v+128)>>8,0,255));
            out[x*4+1]=uint8_t(std::clamp((298*yy-55*u-136*v+128)>>8,0,255));
            out[x*4]=uint8_t(std::clamp((298*yy+541*u+128)>>8,0,255));out[x*4+3]=255;
        }
    }
}
}
