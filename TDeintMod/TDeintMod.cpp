/*
**   VapourSynth port by HolyWu
**
**                TDeinterlace v1.1 for Avisynth 2.5.x
**
**   TDeinterlace is a bi-directionally motion adaptive deinterlacer.
**   It also uses a couple modified forms of ela interpolation which
**   help to reduce "jaggy" edges in places where interpolation must
**   be used. TDeinterlace currently supports YV12 and YUY2 colorspaces.
**
**   Copyright (C) 2004-2007 Kevin Stone
**
**                    TMM v1.0 for Avisynth 2.5.x
**
**   TMM builds a motion-mask for TDeint, which TDeint uses via
**   its 'emask' parameter.  TMM can use fixed or per-pixel adaptive
**   motion thresholds, as well as any length static period.  It
**   checks backwards, across, and forwards when looking for motion.
**
**   Copyright (C) 2007 Kevin Stone
**
**   This program is free software; you can redistribute it and/or modify
**   it under the terms of the GNU General Public License as published by
**   the Free Software Foundation; either version 2 of the License, or
**   (at your option) any later version.
**
**   This program is distributed in the hope that it will be useful,
**   but WITHOUT ANY WARRANTY; without even the implied warranty of
**   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**   GNU General Public License for more details.
**
**   You should have received a copy of the GNU General Public License
**   along with this program; if not, write to the Free Software
**   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>

#include "TDeintMod.hpp"

//////////////////////////////////////////
// TDeintMod

#ifdef VS_TARGET_CPU_X86
template<typename T1, typename T2, int step> extern void threshMask_sse2(const VSFrameRef *, VSFrameRef *, const int, const TDeintModData *, const VSAPI *) noexcept;
template<typename T1, typename T2, int step> extern void threshMask_avx2(const VSFrameRef *, VSFrameRef *, const int, const TDeintModData *, const VSAPI *) noexcept;

template<typename T1, typename T2, int step> extern void motionMask_sse2(const VSFrameRef *, const VSFrameRef *, const VSFrameRef *, const VSFrameRef *, VSFrameRef *, const int, const TDeintModData *, const VSAPI *) noexcept;
template<typename T1, typename T2, int step> extern void motionMask_avx2(const VSFrameRef *, const VSFrameRef *, const VSFrameRef *, const VSFrameRef *, VSFrameRef *, const int, const TDeintModData *, const VSAPI *) noexcept;

template<typename T1, typename T2, int step> extern void andMasks_sse2(const VSFrameRef *, const VSFrameRef *, VSFrameRef *, const int, const TDeintModData *, const VSAPI *) noexcept;
template<typename T1, typename T2, int step> extern void andMasks_avx2(const VSFrameRef *, const VSFrameRef *, VSFrameRef *, const int, const TDeintModData *, const VSAPI *) noexcept;

template<typename T1, typename T2, int step> extern void combineMasks_sse2(const VSFrameRef *, VSFrameRef *, const int, const TDeintModData *, const VSAPI *) noexcept;
template<typename T1, typename T2, int step> extern void combineMasks_avx2(const VSFrameRef *, VSFrameRef *, const int, const TDeintModData *, const VSAPI *) noexcept;
#endif

template<typename T>
static void copyPad(const VSFrameRef * src, VSFrameRef * dst, const int plane, const int widthPad, const VSAPI * vsapi) noexcept {
    const int width = vsapi->getFrameWidth(src, plane);
    const int height = vsapi->getFrameHeight(src, plane);
    const int stride = vsapi->getStride(dst, 0) / sizeof(T);
    const T * srcp = reinterpret_cast<const T *>(vsapi->getReadPtr(src, plane));
    T * VS_RESTRICT dstp = reinterpret_cast<T *>(vsapi->getWritePtr(dst, 0)) + widthPad;

    vs_bitblt(dstp, vsapi->getStride(dst, 0), srcp, vsapi->getStride(src, plane), width * sizeof(T), height);

    for (int y = 0; y < height; y++) {
        dstp[-1] = dstp[1];
        dstp[width] = dstp[width - 2];

        dstp += stride;
    }
}

template<typename T>
static void threshMask_c(const VSFrameRef * src, VSFrameRef * dst, const int plane, const TDeintModData * d, const VSAPI * vsapi) noexcept {
    constexpr T peak = std::numeric_limits<T>::max();

    const int width = d->vi.width >> (plane ? d->vi.format->subSamplingW : 0);
    const int height = d->vi.height >> (plane ? d->vi.format->subSamplingH : 0);
    const int stride = vsapi->getStride(src, 0) / sizeof(T);
    const T * srcp = reinterpret_cast<const T *>(vsapi->getReadPtr(src, 0)) + d->widthPad;
    T * VS_RESTRICT dstp0 = reinterpret_cast<T *>(vsapi->getWritePtr(dst, 0)) + d->widthPad;
    T * VS_RESTRICT dstp1 = dstp0 + stride * height;

    if (plane == 0 && d->mtqL > -1 && d->mthL > -1) {
        std::fill_n(dstp0 - d->widthPad, stride * height, static_cast<T>(d->mtqL));
        std::fill_n(dstp1 - d->widthPad, stride * height, static_cast<T>(d->mthL));
        return;
    } else if (plane > 0 && d->mtqC > -1 && d->mthC > -1) {
        std::fill_n(dstp0 - d->widthPad, stride * height, static_cast<T>(d->mtqC));
        std::fill_n(dstp1 - d->widthPad, stride * height, static_cast<T>(d->mthC));
        return;
    }

    const T * srcpp = srcp + stride;
    const T * srcpn = srcpp;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int min0 = peak, max0 = 0;
            int min1 = peak, max1 = 0;

            if (d->ttype == 0) { // 4 neighbors - compensated
                if (srcpp[x] < min0)
                    min0 = srcpp[x];
                if (srcpp[x] > max0)
                    max0 = srcpp[x];
                if (srcp[x - 1] < min1)
                    min1 = srcp[x - 1];
                if (srcp[x - 1] > max1)
                    max1 = srcp[x - 1];
                if (srcp[x + 1] < min1)
                    min1 = srcp[x + 1];
                if (srcp[x + 1] > max1)
                    max1 = srcp[x + 1];
                if (srcpn[x] < min0)
                    min0 = srcpn[x];
                if (srcpn[x] > max0)
                    max0 = srcpn[x];

                const int atv = std::max((std::abs(srcp[x] - min0) + d->vHalf[plane]) >> d->vShift[plane], (std::abs(srcp[x] - max0) + d->vHalf[plane]) >> d->vShift[plane]);
                const int ath = std::max((std::abs(srcp[x] - min1) + d->hHalf[plane]) >> d->hShift[plane], (std::abs(srcp[x] - max1) + d->hHalf[plane]) >> d->hShift[plane]);
                const int atmax = std::max(atv, ath);
                dstp0[x] = (atmax + 2) >> 2;
                dstp1[x] = (atmax + 1) >> 1;
            } else if (d->ttype == 1) { // 8 neighbors - compensated
                if (srcpp[x - 1] < min0)
                    min0 = srcpp[x - 1];
                if (srcpp[x - 1] > max0)
                    max0 = srcpp[x - 1];
                if (srcpp[x] < min0)
                    min0 = srcpp[x];
                if (srcpp[x] > max0)
                    max0 = srcpp[x];
                if (srcpp[x + 1] < min0)
                    min0 = srcpp[x + 1];
                if (srcpp[x + 1] > max0)
                    max0 = srcpp[x + 1];
                if (srcp[x - 1] < min1)
                    min1 = srcp[x - 1];
                if (srcp[x - 1] > max1)
                    max1 = srcp[x - 1];
                if (srcp[x + 1] < min1)
                    min1 = srcp[x + 1];
                if (srcp[x + 1] > max1)
                    max1 = srcp[x + 1];
                if (srcpn[x - 1] < min0)
                    min0 = srcpn[x - 1];
                if (srcpn[x - 1] > max0)
                    max0 = srcpn[x - 1];
                if (srcpn[x] < min0)
                    min0 = srcpn[x];
                if (srcpn[x] > max0)
                    max0 = srcpn[x];
                if (srcpn[x + 1] < min0)
                    min0 = srcpn[x + 1];
                if (srcpn[x + 1] > max0)
                    max0 = srcpn[x + 1];

                const int atv = std::max((std::abs(srcp[x] - min0) + d->vHalf[plane]) >> d->vShift[plane], (std::abs(srcp[x] - max0) + d->vHalf[plane]) >> d->vShift[plane]);
                const int ath = std::max((std::abs(srcp[x] - min1) + d->hHalf[plane]) >> d->hShift[plane], (std::abs(srcp[x] - max1) + d->hHalf[plane]) >> d->hShift[plane]);
                const int atmax = std::max(atv, ath);
                dstp0[x] = (atmax + 2) >> 2;
                dstp1[x] = (atmax + 1) >> 1;
            } else if (d->ttype == 2) { // 4 neighbors - not compensated
                if (srcpp[x] < min0)
                    min0 = srcpp[x];
                if (srcpp[x] > max0)
                    max0 = srcpp[x];
                if (srcp[x - 1] < min0)
                    min0 = srcp[x - 1];
                if (srcp[x - 1] > max0)
                    max0 = srcp[x - 1];
                if (srcp[x + 1] < min0)
                    min0 = srcp[x + 1];
                if (srcp[x + 1] > max0)
                    max0 = srcp[x + 1];
                if (srcpn[x] < min0)
                    min0 = srcpn[x];
                if (srcpn[x] > max0)
                    max0 = srcpn[x];

                const int at = std::max(std::abs(srcp[x] - min0), std::abs(srcp[x] - max0));
                dstp0[x] = (at + 2) >> 2;
                dstp1[x] = (at + 1) >> 1;
            } else if (d->ttype == 3) { // 8 neighbors - not compensated
                if (srcpp[x - 1] < min0)
                    min0 = srcpp[x - 1];
                if (srcpp[x - 1] > max0)
                    max0 = srcpp[x - 1];
                if (srcpp[x] < min0)
                    min0 = srcpp[x];
                if (srcpp[x] > max0)
                    max0 = srcpp[x];
                if (srcpp[x + 1] < min0)
                    min0 = srcpp[x + 1];
                if (srcpp[x + 1] > max0)
                    max0 = srcpp[x + 1];
                if (srcp[x - 1] < min0)
                    min0 = srcp[x - 1];
                if (srcp[x - 1] > max0)
                    max0 = srcp[x - 1];
                if (srcp[x + 1] < min0)
                    min0 = srcp[x + 1];
                if (srcp[x + 1] > max0)
                    max0 = srcp[x + 1];
                if (srcpn[x - 1] < min0)
                    min0 = srcpn[x - 1];
                if (srcpn[x - 1] > max0)
                    max0 = srcpn[x - 1];
                if (srcpn[x] < min0)
                    min0 = srcpn[x];
                if (srcpn[x] > max0)
                    max0 = srcpn[x];
                if (srcpn[x + 1] < min0)
                    min0 = srcpn[x + 1];
                if (srcpn[x + 1] > max0)
                    max0 = srcpn[x + 1];

                const int at = std::max(std::abs(srcp[x] - min0), std::abs(srcp[x] - max0));
                dstp0[x] = (at + 2) >> 2;
                dstp1[x] = (at + 1) >> 1;
            } else if (d->ttype == 4) { // 4 neighbors - not compensated (range)
                if (srcpp[x] < min0)
                    min0 = srcpp[x];
                if (srcpp[x] > max0)
                    max0 = srcpp[x];
                if (srcp[x - 1] < min0)
                    min0 = srcp[x - 1];
                if (srcp[x - 1] > max0)
                    max0 = srcp[x - 1];
                if (srcp[x] < min0)
                    min0 = srcp[x];
                if (srcp[x] > max0)
                    max0 = srcp[x];
                if (srcp[x + 1] < min0)
                    min0 = srcp[x + 1];
                if (srcp[x + 1] > max0)
                    max0 = srcp[x + 1];
                if (srcpn[x] < min0)
                    min0 = srcpn[x];
                if (srcpn[x] > max0)
                    max0 = srcpn[x];

                const int at = max0 - min0;
                dstp0[x] = (at + 2) >> 2;
                dstp1[x] = (at + 1) >> 1;
            } else { // 8 neighbors - not compensated (range)
                if (srcpp[x - 1] < min0)
                    min0 = srcpp[x - 1];
                if (srcpp[x - 1] > max0)
                    max0 = srcpp[x - 1];
                if (srcpp[x] < min0)
                    min0 = srcpp[x];
                if (srcpp[x] > max0)
                    max0 = srcpp[x];
                if (srcpp[x + 1] < min0)
                    min0 = srcpp[x + 1];
                if (srcpp[x + 1] > max0)
                    max0 = srcpp[x + 1];
                if (srcp[x - 1] < min0)
                    min0 = srcp[x - 1];
                if (srcp[x - 1] > max0)
                    max0 = srcp[x - 1];
                if (srcp[x] < min0)
                    min0 = srcp[x];
                if (srcp[x] > max0)
                    max0 = srcp[x];
                if (srcp[x + 1] < min0)
                    min0 = srcp[x + 1];
                if (srcp[x + 1] > max0)
                    max0 = srcp[x + 1];
                if (srcpn[x - 1] < min0)
                    min0 = srcpn[x - 1];
                if (srcpn[x - 1] > max0)
                    max0 = srcpn[x - 1];
                if (srcpn[x] < min0)
                    min0 = srcpn[x];
                if (srcpn[x] > max0)
                    max0 = srcpn[x];
                if (srcpn[x + 1] < min0)
                    min0 = srcpn[x + 1];
                if (srcpn[x + 1] > max0)
                    max0 = srcpn[x + 1];

                const int at = max0 - min0;
                dstp0[x] = (at + 2) >> 2;
                dstp1[x] = (at + 1) >> 1;
            }
        }

        srcpp = srcp;
        srcp = srcpn;
        srcpn += (y < height - 2) ? stride : -stride;
        dstp0 += stride;
        dstp1 += stride;
    }

    T * dstp = reinterpret_cast<T *>(vsapi->getWritePtr(dst, 0));
    if (plane == 0 && d->mtqL > -1)
        std::fill_n(dstp, stride * height, static_cast<T>(d->mtqL));
    else if (plane == 0 && d->mthL > -1)
        std::fill_n(dstp + stride * height, stride * height, static_cast<T>(d->mthL));
    else if (plane > 0 && d->mtqC > -1)
        std::fill_n(dstp, stride * height, static_cast<T>(d->mtqC));
    else if (plane > 0 && d->mthC > -1)
        std::fill_n(dstp + stride * height, stride * height, static_cast<T>(d->mthC));
}

template<typename T>
static void motionMask_c(const VSFrameRef * src1, const VSFrameRef * msk1, const VSFrameRef * src2, const VSFrameRef * msk2, VSFrameRef * dst,
                         const int plane, const TDeintModData * d, const VSAPI * vsapi) noexcept {
    constexpr T peak = std::numeric_limits<T>::max();

    const int width = d->vi.width >> (plane ? d->vi.format->subSamplingW : 0);
    const int height = d->vi.height >> (plane ? d->vi.format->subSamplingH : 0);
    const int stride = vsapi->getStride(src1, 0) / sizeof(T);
    const T * srcp1 = reinterpret_cast<const T *>(vsapi->getReadPtr(src1, 0)) + d->widthPad;
    const T * srcp2 = reinterpret_cast<const T *>(vsapi->getReadPtr(src2, 0)) + d->widthPad;
    const T * mskp1q = reinterpret_cast<const T *>(vsapi->getReadPtr(msk1, 0)) + d->widthPad;
    const T * mskp2q = reinterpret_cast<const T *>(vsapi->getReadPtr(msk2, 0)) + d->widthPad;
    T * VS_RESTRICT dstpq = reinterpret_cast<T *>(vsapi->getWritePtr(dst, 0)) + d->widthPad;

    const T * mskp1h = mskp1q + stride * height;
    const T * mskp2h = mskp2q + stride * height;
    T * VS_RESTRICT dstph = dstpq + stride * height;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const int diff = std::abs(srcp1[x] - srcp2[x]);
            dstpq[x] = (diff <= std::min(std::max(std::min(mskp1q[x], mskp2q[x]) + d->nt, d->minthresh), d->maxthresh)) ? peak : 0;
            dstph[x] = (diff <= std::min(std::max(std::min(mskp1h[x], mskp2h[x]) + d->nt, d->minthresh), d->maxthresh)) ? peak : 0;
        }

        srcp1 += stride;
        srcp2 += stride;
        mskp1q += stride;
        mskp1h += stride;
        mskp2q += stride;
        mskp2h += stride;
        dstpq += stride;
        dstph += stride;
    }
}

template<typename T>
static void andMasks_c(const VSFrameRef * src1, const VSFrameRef * src2, VSFrameRef * dst, const int plane, const TDeintModData * d, const VSAPI * vsapi) noexcept {
    const int width = d->vi.width >> (plane ? d->vi.format->subSamplingW : 0);
    const int height = (d->vi.height * 2) >> (plane ? d->vi.format->subSamplingH : 0);
    const int stride = vsapi->getStride(src1, 0) / sizeof(T);
    const T * srcp1 = reinterpret_cast<const T *>(vsapi->getReadPtr(src1, 0)) + d->widthPad;
    const T * srcp2 = reinterpret_cast<const T *>(vsapi->getReadPtr(src2, 0)) + d->widthPad;
    T * VS_RESTRICT dstp = reinterpret_cast<T *>(vsapi->getWritePtr(dst, 0)) + d->widthPad;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++)
            dstp[x] &= srcp1[x] & srcp2[x];

        dstp[-1] = dstp[1];
        dstp[width] = dstp[width - 2];

        srcp1 += stride;
        srcp2 += stride;
        dstp += stride;
    }
}

template<typename T>
static void combineMasks_c(const VSFrameRef * src, VSFrameRef * dst, const int plane, const TDeintModData * d, const VSAPI * vsapi) noexcept {
    constexpr T peak = std::numeric_limits<T>::max();

    const int width = vsapi->getFrameWidth(dst, plane);
    const int height = vsapi->getFrameHeight(dst, plane);
    const int srcStride = vsapi->getStride(src, 0) / sizeof(T);
    const int dstStride = vsapi->getStride(dst, plane) / sizeof(T);
    const T * srcp0 = reinterpret_cast<const T *>(vsapi->getReadPtr(src, 0)) + d->widthPad;
    T * VS_RESTRICT dstp = reinterpret_cast<T *>(vsapi->getWritePtr(dst, plane));

    const T * srcpp0 = srcp0 + srcStride;
    const T * srcpn0 = srcpp0;
    const T * srcp1 = srcp0 + srcStride * height;

    vs_bitblt(dstp, vsapi->getStride(dst, plane), srcp0, vsapi->getStride(src, 0), width * sizeof(T), height);

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            if (srcp0[x] || !srcp1[x])
                continue;

            int count = 0;

            if (srcpp0[x - 1])
                count++;
            if (srcpp0[x])
                count++;
            if (srcpp0[x + 1])
                count++;
            if (srcp0[x - 1])
                count++;
            if (srcp0[x + 1])
                count++;
            if (srcpn0[x - 1])
                count++;
            if (srcpn0[x])
                count++;
            if (srcpn0[x + 1])
                count++;

            if (count >= d->cstr)
                dstp[x] = peak;
        }

        srcpp0 = srcp0;
        srcp0 = srcpn0;
        srcpn0 += (y < height - 2) ? srcStride : -srcStride;
        srcp1 += srcStride;
        dstp += dstStride;
    }
}

template<typename T>
static void buildMask(VSFrameRef ** cSrc, VSFrameRef ** oSrc, VSFrameRef * dst, const int cCount, const int oCount, const int order, const int field,
                      const TDeintModData * d, const VSAPI * vsapi) noexcept {
    const uint8_t * tmmlut = d->tmmlut16.data() + order * 8 + field * 4;
    uint8_t tmmlutf[64];
    for (int i = 0; i < 64; i++)
        tmmlutf[i] = tmmlut[d->vlut[i]];

    T * VS_RESTRICT plut[2];
    for (int i = 0; i < 2; i++)
        plut[i] = new T[2 * d->length - 1];

    T * VS_RESTRICT * VS_RESTRICT ptlut[3];
    for (int i = 0; i < 3; i++)
        ptlut[i] = new T *[i & 1 ? cCount : oCount];

    const int offo = (d->length & 1) ? 0 : 1;
    const int offc = (d->length & 1) ? 1 : 0;
    const int ct = cCount / 2;

    for (int plane = 0; plane < d->vi.format->numPlanes; plane++) {
        if (d->process[plane]) {
            const int width = vsapi->getFrameWidth(dst, plane);
            const int height = vsapi->getFrameHeight(dst, plane);
            const int stride = vsapi->getStride(dst, plane) / sizeof(T);
            for (int i = 0; i < cCount; i++)
                ptlut[1][i] = reinterpret_cast<T *>(vsapi->getWritePtr(cSrc[i], plane));
            for (int i = 0; i < oCount; i++) {
                if (field == 1) {
                    ptlut[0][i] = reinterpret_cast<T *>(vsapi->getWritePtr(oSrc[i], plane));
                    ptlut[2][i] = ptlut[0][i] + stride;
                } else {
                    ptlut[0][i] = ptlut[2][i] = reinterpret_cast<T *>(vsapi->getWritePtr(oSrc[i], plane));
                }
            }
            T * VS_RESTRICT dstp = reinterpret_cast<T *>(vsapi->getWritePtr(dst, plane));

            if (field == 1) {
                for (int j = 0; j < height; j += 2)
                    std::fill_n(dstp + stride * j, width, static_cast<T>(10));
                dstp += stride;
            } else {
                for (int j = 1; j < height; j += 2)
                    std::fill_n(dstp + stride * j, width, static_cast<T>(10));
            }

            for (int y = field; y < height; y += 2) {
                for (int x = 0; x < width; x++) {
                    if (!ptlut[1][ct - 2][x] && !ptlut[1][ct][x] && !ptlut[1][ct + 1][x]) {
                        dstp[x] = 60;
                        continue;
                    }

                    for (int j = 0; j < cCount; j++)
                        plut[0][j * 2 + offc] = plut[1][j * 2 + offc] = ptlut[1][j][x];
                    for (int j = 0; j < oCount; j++) {
                        plut[0][j * 2 + offo] = ptlut[0][j][x];
                        plut[1][j * 2 + offo] = ptlut[2][j][x];
                    }

                    int val = 0;
                    for (int i = 0; i < d->length; i++) {
                        for (int j = 0; j < d->length - 4; j++) {
                            if (!plut[0][i + j])
                                goto j1;
                        }
                        val |= d->gvlut[i] * 8;
                    j1:
                        for (int j = 0; j < d->length - 4; j++) {
                            if (!plut[1][i + j])
                                goto j2;
                        }
                        val |= d->gvlut[i];
                    j2:
                        if (d->vlut[val] == 2)
                            break;
                    }
                    dstp[x] = tmmlutf[val];
                }

                for (int i = 0; i < cCount; i++)
                    ptlut[1][i] += stride;
                for (int i = 0; i < oCount; i++) {
                    if (y != 0)
                        ptlut[0][i] += stride;
                    if (y != height - 3)
                        ptlut[2][i] += stride;
                }
                dstp += stride * 2;
            }
        }
    }

    for (int i = 0; i < 2; i++)
        delete[] plut[i];
    for (int i = 0; i < 3; i++)
        delete[] ptlut[i];
}

template<typename T>
static void setMaskForUpsize(VSFrameRef * mask, const int field, const TDeintModData * d, const VSAPI * vsapi) noexcept {
    for (int plane = 0; plane < d->vi.format->numPlanes; plane++) {
        if (d->process[plane]) {
            const int width = vsapi->getFrameWidth(mask, plane);
            const int height = vsapi->getFrameHeight(mask, plane) / 2;
            const int stride = vsapi->getStride(mask, plane) / sizeof(T) * 2;
            T * VS_RESTRICT maskwc = reinterpret_cast<T *>(vsapi->getWritePtr(mask, plane));
            T * VS_RESTRICT maskwn = maskwc + stride / 2;

            if (field == 1) {
                for (int y = 0; y < height - 1; y++) {
                    std::fill_n(maskwc, width, static_cast<T>(10));
                    std::fill_n(maskwn, width, static_cast<T>(60));
                    maskwc += stride;
                    maskwn += stride;
                }
                std::fill_n(maskwc, width, static_cast<T>(10));
                std::fill_n(maskwn, width, static_cast<T>(10));
            } else {
                std::fill_n(maskwc, width, static_cast<T>(10));
                std::fill_n(maskwn, width, static_cast<T>(10));
                for (int y = 0; y < height - 1; y++) {
                    maskwc += stride;
                    maskwn += stride;
                    std::fill_n(maskwc, width, static_cast<T>(60));
                    std::fill_n(maskwn, width, static_cast<T>(10));
                }
            }
        }
    }
}

template<typename T>
static void checkSpatial(const VSFrameRef * src, VSFrameRef * dst, const TDeintModData * d, const VSAPI * vsapi) noexcept {
    for (int plane = 0; plane < d->vi.format->numPlanes; plane++) {
        if (d->process[plane]) {
            const int width = vsapi->getFrameWidth(src, plane);
            const int height = vsapi->getFrameHeight(src, plane);
            const int stride = vsapi->getStride(src, plane) / sizeof(T);
            const T * srcp = reinterpret_cast<const T *>(vsapi->getReadPtr(src, plane));
            T * VS_RESTRICT dstp = reinterpret_cast<T *>(vsapi->getWritePtr(dst, plane));

            const T * srcppp = srcp - stride * 2;
            const T * srcpp = srcp - stride;
            const T * srcpn = srcp + stride;
            const T * srcpnn = srcp + stride * 2;

            if (d->metric == 0) {
                for (int x = 0; x < width; x++) {
                    const int sFirst = srcp[x] - srcpn[x];
                    if (dstp[x] == 60 && !((sFirst > d->athresh || sFirst < -d->athresh) &&
                                           std::abs(srcpnn[x] + srcp[x] * 4 + srcpnn[x] - 3 * (srcpn[x] + srcpn[x])) > d->athresh6))
                        dstp[x] = 10;
                }
                srcppp += stride;
                srcpp += stride;
                srcp += stride;
                srcpn += stride;
                srcpnn += stride;
                dstp += stride;

                for (int x = 0; x < width; x++) {
                    const int sFirst = srcp[x] - srcpp[x];
                    const int sSecond = srcp[x] - srcpn[x];
                    if (dstp[x] == 60 && !(((sFirst > d->athresh && sSecond > d->athresh) || (sFirst < -d->athresh && sSecond < -d->athresh)) &&
                                           std::abs(srcpnn[x] + srcp[x] * 4 + srcpnn[x] - 3 * (srcpp[x] + srcpn[x])) > d->athresh6))
                        dstp[x] = 10;
                }
                srcppp += stride;
                srcpp += stride;
                srcp += stride;
                srcpn += stride;
                srcpnn += stride;
                dstp += stride;

                for (int y = 2; y < height - 2; y++) {
                    for (int x = 0; x < width; x++) {
                        const int sFirst = srcp[x] - srcpp[x];
                        const int sSecond = srcp[x] - srcpn[x];
                        if (dstp[x] == 60 && !(((sFirst > d->athresh && sSecond > d->athresh) || (sFirst < -d->athresh && sSecond < -d->athresh)) &&
                                               std::abs(srcppp[x] + srcp[x] * 4 + srcpnn[x] - 3 * (srcpp[x] + srcpn[x])) > d->athresh6))
                            dstp[x] = 10;
                    }
                    srcppp += stride;
                    srcpp += stride;
                    srcp += stride;
                    srcpn += stride;
                    srcpnn += stride;
                    dstp += stride;
                }

                for (int x = 0; x < width; x++) {
                    const int sFirst = srcp[x] - srcpp[x];
                    const int sSecond = srcp[x] - srcpn[x];
                    if (dstp[x] == 60 && !(((sFirst > d->athresh && sSecond > d->athresh) || (sFirst < -d->athresh && sSecond < -d->athresh)) &&
                                           std::abs(srcppp[x] + srcp[x] * 4 + srcppp[x] - 3 * (srcpp[x] + srcpn[x])) > d->athresh6))
                        dstp[x] = 10;
                }
                srcppp += stride;
                srcpp += stride;
                srcp += stride;
                srcpn += stride;
                srcpnn += stride;
                dstp += stride;

                for (int x = 0; x < width; x++) {
                    const int sFirst = srcp[x] - srcpp[x];
                    if (dstp[x] == 60 && !((sFirst > d->athresh || sFirst < -d->athresh) &&
                                           std::abs(srcppp[x] + srcp[x] * 4 + srcppp[x] - 3 * (srcpp[x] + srcpp[x])) > d->athresh6))
                        dstp[x] = 10;
                }
            } else {
                for (int x = 0; x < width; x++) {
                    if (dstp[x] == 60 && !((srcp[x] - srcpn[x]) * (srcp[x] - srcpn[x]) > d->athreshsq))
                        dstp[x] = 10;
                }
                srcpp += stride;
                srcp += stride;
                srcpn += stride;
                dstp += stride;

                for (int y = 1; y < height - 1; y++) {
                    for (int x = 0; x < width; x++) {
                        if (dstp[x] == 60 && !((srcp[x] - srcpp[x]) * (srcp[x] - srcpn[x]) > d->athreshsq))
                            dstp[x] = 10;
                    }
                    srcpp += stride;
                    srcp += stride;
                    srcpn += stride;
                    dstp += stride;
                }

                for (int x = 0; x < width; x++) {
                    if (dstp[x] == 60 && !((srcp[x] - srcpp[x]) * (srcp[x] - srcpp[x]) > d->athreshsq))
                        dstp[x] = 10;
                }
            }
        }
    }
}

template<typename T>
static void expandMask(VSFrameRef * mask, const int field, const TDeintModData * d, const VSAPI * vsapi) noexcept {
    for (int plane = 0; plane < d->vi.format->numPlanes; plane++) {
        if (d->process[plane]) {
            const int width = vsapi->getFrameWidth(mask, plane);
            const int height = vsapi->getFrameHeight(mask, plane);
            const int stride = vsapi->getStride(mask, plane) / sizeof(T) * 2;
            T * VS_RESTRICT maskp = reinterpret_cast<T *>(vsapi->getWritePtr(mask, plane)) + stride / 2 * field;

            const int dis = d->expand >> (plane ? d->vi.format->subSamplingW : 0);

            for (int y = field; y < height; y += 2) {
                for (int x = 0; x < width; x++) {
                    if (maskp[x] == 60) {
                        int xt = x - 1;
                        while (xt >= 0 && xt >= x - dis)
                            maskp[xt--] = 60;
                        xt = x + 1;

                        int nc = x + dis + 1;
                        while (xt < width && xt <= x + dis) {
                            if (maskp[xt] == 60) {
                                nc = xt;
                                break;
                            } else {
                                maskp[xt++] = 60;
                            }
                        }
                        x = nc - 1;
                    }
                }

                maskp += stride;
            }
        }
    }
}

template<typename T>
static void linkMask(VSFrameRef * mask, const int field, const TDeintModData * d, const VSAPI * vsapi) noexcept {
    const int width = vsapi->getFrameWidth(mask, 2);
    const int height = vsapi->getFrameHeight(mask, 2);
    const int strideY = vsapi->getStride(mask, 0) / sizeof(T);
    const int strideUV = vsapi->getStride(mask, 2) / sizeof(T);
    const T * maskpY = reinterpret_cast<const T *>(vsapi->getReadPtr(mask, 0)) + strideY * field;
    T * VS_RESTRICT maskpU = reinterpret_cast<T *>(vsapi->getWritePtr(mask, 1)) + strideUV * field;
    T * VS_RESTRICT maskpV = reinterpret_cast<T *>(vsapi->getWritePtr(mask, 2)) + strideUV * field;

    const T * maskpnY = maskpY + strideY * 2;

    const int strideY2 = strideY * (2 << d->vi.format->subSamplingH);
    const int strideUV2 = strideUV * 2;

    for (int y = field; y < height; y += 2) {
        for (int x = 0; x < width; x++) {
            if (d->vi.format->subSamplingW == 0) {
                if (d->vi.format->subSamplingH == 0) {
                    if (maskpY[x] == 0x3C)
                        maskpU[x] = maskpV[x] = 0x3C;
                } else {
                    if (maskpY[x] == 0x3C && maskpnY[x] == 0x3C)
                        maskpU[x] = maskpV[x] = 0x3C;
                }
            } else {
                if (std::is_same<T, uint8_t>::value) {
                    if (d->vi.format->subSamplingH == 0) {
                        if (reinterpret_cast<const uint16_t *>(maskpY)[x] == 0x3C3C)
                            maskpU[x] = maskpV[x] = 0x3C;
                    } else {
                        if (reinterpret_cast<const uint16_t *>(maskpY)[x] == 0x3C3C && reinterpret_cast<const uint16_t *>(maskpnY)[x] == 0x3C3C)
                            maskpU[x] = maskpV[x] = 0x3C;
                    }
                } else {
                    if (d->vi.format->subSamplingH == 0) {
                        if (reinterpret_cast<const uint32_t *>(maskpY)[x] == 0x3C003C)
                            maskpU[x] = maskpV[x] = 0x3C;
                    } else {
                        if (reinterpret_cast<const uint32_t *>(maskpY)[x] == 0x3C003C && reinterpret_cast<const uint32_t *>(maskpnY)[x] == 0x3C003C)
                            maskpU[x] = maskpV[x] = 0x3C;
                    }
                }
            }
        }

        maskpY += strideY2;
        maskpnY += strideY2;
        maskpU += strideUV2;
        maskpV += strideUV2;
    }
}

template<typename T>
static void eDeint(VSFrameRef * dst, const VSFrameRef * mask, const VSFrameRef * prv, const VSFrameRef * src, const VSFrameRef * nxt, const VSFrameRef * edeint,
                   const TDeintModData * d, const VSAPI * vsapi) noexcept {
    for (int plane = 0; plane < d->vi.format->numPlanes; plane++) {
        if (d->process[plane]) {
            const int width = vsapi->getFrameWidth(src, plane);
            const int height = vsapi->getFrameHeight(src, plane);
            const int stride = vsapi->getStride(src, plane) / sizeof(T);
            const T * prvp = reinterpret_cast<const T *>(vsapi->getReadPtr(prv, plane));
            const T * srcp = reinterpret_cast<const T *>(vsapi->getReadPtr(src, plane));
            const T * nxtp = reinterpret_cast<const T *>(vsapi->getReadPtr(nxt, plane));
            const T * maskp = reinterpret_cast<const T *>(vsapi->getReadPtr(mask, plane));
            const T * edeintp = reinterpret_cast<const T *>(vsapi->getReadPtr(edeint, plane));
            T * VS_RESTRICT dstp = reinterpret_cast<T *>(vsapi->getWritePtr(dst, plane));

            for (int y = 0; y < height; y++) {
                for (int x = 0; x < width; x++) {
                    if (maskp[x] == 10)
                        dstp[x] = srcp[x];
                    else if (maskp[x] == 20)
                        dstp[x] = prvp[x];
                    else if (maskp[x] == 30)
                        dstp[x] = nxtp[x];
                    else if (maskp[x] == 40)
                        dstp[x] = (srcp[x] + nxtp[x] + 1) >> 1;
                    else if (maskp[x] == 50)
                        dstp[x] = (srcp[x] + prvp[x] + 1) >> 1;
                    else if (maskp[x] == 70)
                        dstp[x] = (prvp[x] + srcp[x] * 2 + nxtp[x] + 2) >> 2;
                    else if (maskp[x] == 60)
                        dstp[x] = edeintp[x];
                }

                prvp += stride;
                srcp += stride;
                nxtp += stride;
                maskp += stride;
                edeintp += stride;
                dstp += stride;
            }
        }
    }
}

template<typename T>
static void cubicDeint(VSFrameRef * dst, const VSFrameRef * mask, const VSFrameRef * prv, const VSFrameRef * src, const VSFrameRef * nxt,
                       const TDeintModData * d, const VSAPI * vsapi) noexcept {
    for (int plane = 0; plane < d->vi.format->numPlanes; plane++) {
        if (d->process[plane]) {
            const int width = vsapi->getFrameWidth(src, plane);
            const int height = vsapi->getFrameHeight(src, plane);
            const int stride = vsapi->getStride(src, plane) / sizeof(T);
            const T * prvp = reinterpret_cast<const T *>(vsapi->getReadPtr(prv, plane));
            const T * srcp = reinterpret_cast<const T *>(vsapi->getReadPtr(src, plane));
            const T * nxtp = reinterpret_cast<const T *>(vsapi->getReadPtr(nxt, plane));
            const T * maskp = reinterpret_cast<const T *>(vsapi->getReadPtr(mask, plane));
            T * VS_RESTRICT dstp = reinterpret_cast<T *>(vsapi->getWritePtr(dst, plane));

            const T * srcpp = srcp - stride;
            const T * srcppp = srcpp - stride * 2;
            const T * srcpn = srcp + stride;
            const T * srcpnn = srcpn + stride * 2;

            for (int y = 0; y < height; y++) {
                for (int x = 0; x < width; x++) {
                    if (maskp[x] == 10)
                        dstp[x] = srcp[x];
                    else if (maskp[x] == 20)
                        dstp[x] = prvp[x];
                    else if (maskp[x] == 30)
                        dstp[x] = nxtp[x];
                    else if (maskp[x] == 40)
                        dstp[x] = (srcp[x] + nxtp[x] + 1) >> 1;
                    else if (maskp[x] == 50)
                        dstp[x] = (srcp[x] + prvp[x] + 1) >> 1;
                    else if (maskp[x] == 70)
                        dstp[x] = (prvp[x] + srcp[x] * 2 + nxtp[x] + 2) >> 2;
                    else if (maskp[x] == 60) {
                        if (y == 0) {
                            dstp[x] = srcpn[x];
                        } else if (y == height - 1) {
                            dstp[x] = srcpp[x];
                        } else if (y < 3 || y > height - 4) {
                            dstp[x] = (srcpn[x] + srcpp[x] + 1) >> 1;
                        } else {
                            const int temp = (19 * (srcpp[x] + srcpn[x]) - 3 * (srcppp[x] + srcpnn[x]) + 16) >> 5;
                            dstp[x] = std::min(std::max(temp, 0), d->peak);
                        }
                    }
                }

                prvp += stride;
                srcppp += stride;
                srcpp += stride;
                srcp += stride;
                srcpn += stride;
                srcpnn += stride;
                nxtp += stride;
                maskp += stride;
                dstp += stride;
            }
        }
    }
}

template<typename T>
static void binaryMask(const VSFrameRef * src, VSFrameRef * dst, const TDeintModData * d, const VSAPI * vsapi) noexcept {
    for (int plane = 0; plane < d->vi.format->numPlanes; plane++) {
        if (d->process[plane]) {
            const int width = vsapi->getFrameWidth(src, plane);
            const int height = vsapi->getFrameHeight(src, plane);
            const int stride = vsapi->getStride(src, plane) / sizeof(T);
            const T * srcp = reinterpret_cast<const T *>(vsapi->getReadPtr(src, plane));
            T * VS_RESTRICT dstp = reinterpret_cast<T *>(vsapi->getWritePtr(dst, plane));

            for (int y = 0; y < height; y++) {
                for (int x = 0; x < width; x++)
                    dstp[x] = (srcp[x] == 60) ? d->peak : 0;

                srcp += stride;
                dstp += stride;
            }
        }
    }
}

static void selectFunctions(const unsigned opt, TDeintModData * d) noexcept {
#ifdef VS_TARGET_CPU_X86
    const int iset = instrset_detect();
#endif

    if (d->vi.format->bytesPerSample == 1) {
        d->copyPad = copyPad<uint8_t>;
        d->threshMask = threshMask_c<uint8_t>;
        d->motionMask = motionMask_c<uint8_t>;
        d->andMasks = andMasks_c<uint8_t>;
        d->combineMasks = combineMasks_c<uint8_t>;
        d->buildMask = buildMask<uint8_t>;
        d->setMaskForUpsize = setMaskForUpsize<uint8_t>;
        d->checkSpatial = checkSpatial<uint8_t>;
        d->expandMask = expandMask<uint8_t>;
        d->linkMask = linkMask<uint8_t>;
        d->eDeint = eDeint<uint8_t>;
        d->cubicDeint = cubicDeint<uint8_t>;
        d->binaryMask = binaryMask<uint8_t>;

#ifdef VS_TARGET_CPU_X86
        if ((opt == 0 && iset >= 8) || opt == 3) {
            d->threshMask = threshMask_avx2<uint8_t, Vec32uc, 32>;
            d->motionMask = motionMask_avx2<uint8_t, Vec32uc, 32>;
            d->andMasks = andMasks_avx2<uint8_t, Vec32uc, 32>;
            d->combineMasks = combineMasks_avx2<uint8_t, Vec32uc, 32>;
        } else if ((opt == 0 && iset >= 2) || opt == 2) {
            d->threshMask = threshMask_sse2<uint8_t, Vec16uc, 16>;
            d->motionMask = motionMask_sse2<uint8_t, Vec16uc, 16>;
            d->andMasks = andMasks_sse2<uint8_t, Vec16uc, 16>;
            d->combineMasks = combineMasks_sse2<uint8_t, Vec16uc, 16>;
        }
#endif
    } else {
        d->copyPad = copyPad<uint16_t>;
        d->threshMask = threshMask_c<uint16_t>;
        d->motionMask = motionMask_c<uint16_t>;
        d->andMasks = andMasks_c<uint16_t>;
        d->combineMasks = combineMasks_c<uint16_t>;
        d->buildMask = buildMask<uint16_t>;
        d->setMaskForUpsize = setMaskForUpsize<uint16_t>;
        d->checkSpatial = checkSpatial<uint16_t>;
        d->expandMask = expandMask<uint16_t>;
        d->linkMask = linkMask<uint16_t>;
        d->eDeint = eDeint<uint16_t>;
        d->cubicDeint = cubicDeint<uint16_t>;
        d->binaryMask = binaryMask<uint16_t>;

#ifdef VS_TARGET_CPU_X86
        if ((opt == 0 && iset >= 8) || opt == 3) {
            d->threshMask = threshMask_avx2<uint16_t, Vec16us, 16>;
            d->motionMask = motionMask_avx2<uint16_t, Vec16us, 16>;
            d->andMasks = andMasks_avx2<uint16_t, Vec16us, 16>;
            d->combineMasks = combineMasks_avx2<uint16_t, Vec16us, 16>;
        } else if ((opt == 0 && iset >= 2) || opt == 2) {
            d->threshMask = threshMask_sse2<uint16_t, Vec8us, 8>;
            d->motionMask = motionMask_sse2<uint16_t, Vec8us, 8>;
            d->andMasks = andMasks_sse2<uint16_t, Vec8us, 8>;
            d->combineMasks = combineMasks_sse2<uint16_t, Vec8us, 8>;
        }
#endif
    }
}

static void VS_CC tdeintmodInit(VSMap *in, VSMap *out, void **instanceData, VSNode *node, VSCore *core, const VSAPI *vsapi) {
    TDeintModData * d = static_cast<TDeintModData *>(*instanceData);
    vsapi->setVideoInfo(&d->vi, 1, node);
}

static const VSFrameRef *VS_CC tdeintmodCreateMMGetFrame(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    const TDeintModData * d = static_cast<const TDeintModData *>(*instanceData);

    if (activationReason == arInitial) {
        for (int i = n; i <= std::min(n + 2, d->vi.numFrames - 1); i++)
            vsapi->requestFrameFilter(i, d->node, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        const VSFrameRef * src[3];
        VSFrameRef * pad[3], * msk[3][2];
        for (int i = 0; i < 3; i++) {
            src[i] = vsapi->getFrameFilter(std::min(n + i, d->vi.numFrames - 1), d->node, frameCtx);
            pad[i] = vsapi->newVideoFrame(d->format, d->vi.width + d->widthPad * 2, d->vi.height, nullptr, core);
            msk[i][0] = vsapi->newVideoFrame(d->format, d->vi.width + d->widthPad * 2, d->vi.height * 2, nullptr, core);
            msk[i][1] = vsapi->newVideoFrame(d->format, d->vi.width + d->widthPad * 2, d->vi.height * 2, nullptr, core);
        }
        VSFrameRef * dst[] = { vsapi->newVideoFrame(d->format, d->vi.width + d->widthPad * 2, d->vi.height * 2, nullptr, core),
                               vsapi->newVideoFrame(d->vi.format, d->vi.width, d->vi.height, nullptr, core) };

        for (int plane = 0; plane < d->vi.format->numPlanes; plane++) {
            if (d->process[plane]) {
                for (int i = 0; i < 3; i++) {
                    d->copyPad(src[i], pad[i], plane, d->widthPad, vsapi);
                    d->threshMask(pad[i], msk[i][0], plane, d, vsapi);
                }
                for (int i = 0; i < 2; i++)
                    d->motionMask(pad[i], msk[i][0], pad[i + 1], msk[i + 1][0], msk[i][1], plane, d, vsapi);
                d->motionMask(pad[0], msk[0][0], pad[2], msk[2][0], dst[0], plane, d, vsapi);
                d->andMasks(msk[0][1], msk[1][1], dst[0], plane, d, vsapi);
                d->combineMasks(dst[0], dst[1], plane, d, vsapi);
            }
        }

        for (int i = 0; i < 3; i++) {
            vsapi->freeFrame(src[i]);
            vsapi->freeFrame(pad[i]);
            vsapi->freeFrame(msk[i][0]);
            vsapi->freeFrame(msk[i][1]);
        }
        vsapi->freeFrame(dst[0]);
        return dst[1];
    }

    return nullptr;
}

static const VSFrameRef *VS_CC tdeintmodBuildMMGetFrame(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    const TDeintModData * d = static_cast<const TDeintModData *>(*instanceData);

    if (activationReason == arInitial) {
        if (d->mode == 1)
            n /= 2;

        const int start = std::max(n - 1 - (d->length - 2) / 2, 0);
        const int stop = std::min(n + 1 + (d->length - 2) / 2 - 2, d->viSaved->numFrames - 3);
        for (int i = start; i <= stop; i++) {
            vsapi->requestFrameFilter(i, d->node, frameCtx);
            vsapi->requestFrameFilter(i, d->node2, frameCtx);
        }

        vsapi->requestFrameFilter(n, d->propNode, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        const int nSaved = n;
        if (d->mode == 1)
            n /= 2;

        int err;
        const VSFrameRef * propSrc = vsapi->getFrameFilter(n, d->propNode, frameCtx);
        const int fieldBased = int64ToIntS(vsapi->propGetInt(vsapi->getFramePropsRO(propSrc), "_FieldBased", 0, &err));
        vsapi->freeFrame(propSrc);

        int order = d->order;
        if (fieldBased == 1)
            order = 0;
        else if (fieldBased == 2)
            order = 1;

        int field;
        if (d->mode == 1)
            field = (nSaved & 1) ? 1 - order : order;
        else
            field = (d->field == -1) ? order : d->field;

        VSFrameRef ** srct = new VSFrameRef *[d->length - 2];
        VSFrameRef ** srcb = new VSFrameRef *[d->length - 2];
        VSFrameRef * dst = vsapi->newVideoFrame(d->vi.format, d->vi.width, d->vi.height, nullptr, core);

        int tStart, tStop, bStart, bStop, cCount, oCount;
        VSFrameRef ** cSrc, ** oSrc;
        if (field == 1) {
            tStart = n - (d->length - 1) / 2;
            tStop = n + (d->length - 1) / 2 - 2;
            const int bn = (order == 1) ? n - 1 : n;
            bStart = bn - (d->length - 2) / 2;
            bStop = bn + 1 + (d->length - 2) / 2 - 2;
            oCount = tStop - tStart + 1;
            cCount = bStop - bStart + 1;
            oSrc = srct;
            cSrc = srcb;
        } else {
            const int tn = (order == 0) ? n - 1 : n;
            tStart = tn - (d->length - 2) / 2;
            tStop = tn + 1 + (d->length - 2) / 2 - 2;
            bStart = n - (d->length - 1) / 2;
            bStop = n + (d->length - 1) / 2 - 2;
            cCount = tStop - tStart + 1;
            oCount = bStop - bStart + 1;
            cSrc = srct;
            oSrc = srcb;
        }

        for (int i = tStart; i <= tStop; i++) {
            if (i < 0 || i >= d->viSaved->numFrames - 2) {
                srct[i - tStart] = vsapi->newVideoFrame(d->viSaved->format, d->viSaved->width, d->viSaved->height, nullptr, core);
                for (int plane = 0; plane < d->viSaved->format->numPlanes; plane++)
                    memset(vsapi->getWritePtr(srct[i - tStart], plane), 0, vsapi->getStride(srct[i - tStart], plane) * vsapi->getFrameHeight(srct[i - tStart], plane));
            } else {
                const VSFrameRef * src = vsapi->getFrameFilter(i, d->node, frameCtx);
                srct[i - tStart] = vsapi->copyFrame(src, core);
                vsapi->freeFrame(src);
            }
        }
        for (int i = bStart; i <= bStop; i++) {
            if (i < 0 || i >= d->viSaved->numFrames - 2) {
                srcb[i - bStart] = vsapi->newVideoFrame(d->viSaved->format, d->viSaved->width, d->viSaved->height, nullptr, core);
                for (int plane = 0; plane < d->viSaved->format->numPlanes; plane++)
                    memset(vsapi->getWritePtr(srcb[i - bStart], plane), 0, vsapi->getStride(srcb[i - bStart], plane) * vsapi->getFrameHeight(srcb[i - bStart], plane));
            } else {
                const VSFrameRef * src = vsapi->getFrameFilter(i, d->node2, frameCtx);
                srcb[i - bStart] = vsapi->copyFrame(src, core);
                vsapi->freeFrame(src);
            }
        }

        d->buildMask(cSrc, oSrc, dst, cCount, oCount, order, field, d, vsapi);

        for (int i = tStart; i <= tStop; i++)
            vsapi->freeFrame(srct[i - tStart]);
        for (int i = bStart; i <= bStop; i++)
            vsapi->freeFrame(srcb[i - bStart]);
        delete[] srct;
        delete[] srcb;
        return dst;
    }

    return nullptr;
}

static const VSFrameRef *VS_CC tdeintmodGetFrame(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    const TDeintModData * d = static_cast<const TDeintModData *>(*instanceData);

    if (activationReason == arInitial) {
        const int nSaved = n;
        if (d->mode == 1)
            n /= 2;

        if (n > 0)
            vsapi->requestFrameFilter(n - 1, d->node, frameCtx);
        vsapi->requestFrameFilter(n, d->node, frameCtx);
        if (n < d->viSaved->numFrames - 1)
            vsapi->requestFrameFilter(n + 1, d->node, frameCtx);

        if (d->mask)
            vsapi->requestFrameFilter(nSaved, d->mask, frameCtx);

        if (!d->show && d->edeint)
            vsapi->requestFrameFilter(nSaved, d->edeint, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        const int nSaved = n;
        if (d->mode == 1)
            n /= 2;

        const VSFrameRef * prv = vsapi->getFrameFilter(std::max(n - 1, 0), d->node, frameCtx);
        const VSFrameRef * src = vsapi->getFrameFilter(n, d->node, frameCtx);
        const VSFrameRef * nxt = vsapi->getFrameFilter(std::min(n + 1, d->viSaved->numFrames - 1), d->node, frameCtx);
        const VSFrameRef * fr[] = { d->process[0] ? nullptr : src, d->process[1] ? nullptr : src, d->process[2] ? nullptr : src };
        const int pl[] = { 0, 1, 2 };
        VSFrameRef * mask, * dst;

        int err;
        const int fieldBased = int64ToIntS(vsapi->propGetInt(vsapi->getFramePropsRO(src), "_FieldBased", 0, &err));

        int order = d->order;
        if (fieldBased == 1)
            order = 0;
        else if (fieldBased == 2)
            order = 1;

        int field;
        if (d->mode == 1)
            field = (nSaved & 1) ? 1 - order : order;
        else
            field = (d->field == -1) ? order : d->field;

        if (d->mask) {
            mask = const_cast<VSFrameRef *>(vsapi->getFrameFilter(nSaved, d->mask, frameCtx));
        } else {
            mask = vsapi->newVideoFrame(d->vi.format, d->vi.width, d->vi.height, nullptr, core);
            d->setMaskForUpsize(mask, field, d, vsapi);
        }

        if (d->athresh > -1)
            d->checkSpatial(src, mask, d, vsapi);

        if (d->expand)
            d->expandMask(mask, field, d, vsapi);

        if (d->link)
            d->linkMask(mask, field, d, vsapi);

        if (!d->show) {
            dst = vsapi->newVideoFrame2(d->vi.format, d->vi.width, d->vi.height, fr, pl, src, core);

            if (d->edeint) {
                const VSFrameRef * edeint = vsapi->getFrameFilter(nSaved, d->edeint, frameCtx);
                d->eDeint(dst, mask, prv, src, nxt, edeint, d, vsapi);
                vsapi->freeFrame(edeint);
            } else {
                d->cubicDeint(dst, mask, prv, src, nxt, d, vsapi);
            }
        } else {
            dst = vsapi->newVideoFrame(d->vi.format, d->vi.width, d->vi.height, src, core);

            d->binaryMask(mask, dst, d, vsapi);
        }

        VSMap * props = vsapi->getFramePropsRW(dst);
        vsapi->propSetInt(props, "_FieldBased", 0, paReplace);

        if (d->mode == 1) {
            int errNum, errDen;
            int64_t durationNum = vsapi->propGetInt(props, "_DurationNum", 0, &errNum);
            int64_t durationDen = vsapi->propGetInt(props, "_DurationDen", 0, &errDen);
            if (!errNum && !errDen) {
                muldivRational(&durationNum, &durationDen, 1, 2);
                vsapi->propSetInt(props, "_DurationNum", durationNum, paReplace);
                vsapi->propSetInt(props, "_DurationDen", durationDen, paReplace);
            }
        }

        vsapi->freeFrame(prv);
        vsapi->freeFrame(src);
        vsapi->freeFrame(nxt);
        vsapi->freeFrame(mask);
        return dst;
    }

    return nullptr;
}

static void VS_CC tdeintmodCreateMMFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    TDeintModData * d = static_cast<TDeintModData *>(instanceData);
    vsapi->freeNode(d->node);
    delete d;
}

static void VS_CC tdeintmodBuildMMFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    TDeintModData * d = static_cast<TDeintModData *>(instanceData);
    vsapi->freeNode(d->node);
    vsapi->freeNode(d->node2);
    vsapi->freeNode(d->propNode);
    delete[] d->gvlut;
    delete d;
}

static void VS_CC tdeintmodFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    TDeintModData * d = static_cast<TDeintModData *>(instanceData);
    vsapi->freeNode(d->node);
    vsapi->freeNode(d->mask);
    vsapi->freeNode(d->edeint);
    delete d;
}

static void VS_CC tdeintmodCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    TDeintModData d{};
    int err;

    d.order = int64ToIntS(vsapi->propGetInt(in, "order", 0, nullptr));

    d.field = int64ToIntS(vsapi->propGetInt(in, "field", 0, &err));
    if (err)
        d.field = -1;

    d.mode = int64ToIntS(vsapi->propGetInt(in, "mode", 0, &err));

    d.length = int64ToIntS(vsapi->propGetInt(in, "length", 0, &err));
    if (err)
        d.length = 10;

    d.mtype = int64ToIntS(vsapi->propGetInt(in, "mtype", 0, &err));
    if (err)
        d.mtype = 1;

    d.ttype = int64ToIntS(vsapi->propGetInt(in, "ttype", 0, &err));
    if (err)
        d.ttype = 1;

    d.mtqL = int64ToIntS(vsapi->propGetInt(in, "mtql", 0, &err));
    if (err)
        d.mtqL = -1;

    d.mthL = int64ToIntS(vsapi->propGetInt(in, "mthl", 0, &err));
    if (err)
        d.mthL = -1;

    d.mtqC = int64ToIntS(vsapi->propGetInt(in, "mtqc", 0, &err));
    if (err)
        d.mtqC = -1;

    d.mthC = int64ToIntS(vsapi->propGetInt(in, "mthc", 0, &err));
    if (err)
        d.mthC = -1;

    d.nt = int64ToIntS(vsapi->propGetInt(in, "nt", 0, &err));
    if (err)
        d.nt = 2;

    d.minthresh = int64ToIntS(vsapi->propGetInt(in, "minthresh", 0, &err));
    if (err)
        d.minthresh = 4;

    d.maxthresh = int64ToIntS(vsapi->propGetInt(in, "maxthresh", 0, &err));
    if (err)
        d.maxthresh = 75;

    d.cstr = int64ToIntS(vsapi->propGetInt(in, "cstr", 0, &err));
    if (err)
        d.cstr = 4;

    d.athresh = int64ToIntS(vsapi->propGetInt(in, "athresh", 0, &err));
    if (err)
        d.athresh = -1;

    d.metric = int64ToIntS(vsapi->propGetInt(in, "metric", 0, &err));

    d.expand = int64ToIntS(vsapi->propGetInt(in, "expand", 0, &err));

    d.link = !!vsapi->propGetInt(in, "link", 0, &err);
    if (err)
        d.link = true;

    d.show = !!vsapi->propGetInt(in, "show", 0, &err);

    const int opt = int64ToIntS(vsapi->propGetInt(in, "opt", 0, &err));

    if (d.order < 0 || d.order > 1) {
        vsapi->setError(out, "TDeintMod: order must be 0 or 1");
        return;
    }

    if (d.field < -1 || d.field > 1) {
        vsapi->setError(out, "TDeintMod: field must be -1, 0 or 1");
        return;
    }

    if (d.mode < 0 || d.mode > 1) {
        vsapi->setError(out, "TDeintMod: mode must be 0 or 1");
        return;
    }

    if (d.length < 6) {
        vsapi->setError(out, "TDeintMod: length must be greater than or equal to 6");
        return;
    }

    if (d.mtype < 0 || d.mtype > 2) {
        vsapi->setError(out, "TDeintMod: mtype must be 0, 1 or 2");
        return;
    }

    if (d.ttype < 0 || d.ttype > 5) {
        vsapi->setError(out, "TDeintMod: ttype must be 0, 1, 2, 3, 4 or 5");
        return;
    }

    if (d.mtqL < -2 || d.mtqL > 255) {
        vsapi->setError(out, "TDeintMod: mtql must be between -2 and 255 (inclusive)");
        return;
    }

    if (d.mthL < -2 || d.mthL > 255) {
        vsapi->setError(out, "TDeintMod: mthl must be between -2 and 255 (inclusive)");
        return;
    }

    if (d.mtqC < -2 || d.mtqC > 255) {
        vsapi->setError(out, "TDeintMod: mtqc must be between -2 and 255 (inclusive)");
        return;
    }

    if (d.mthC < -2 || d.mthC > 255) {
        vsapi->setError(out, "TDeintMod: mthc must be between -2 and 255 (inclusive)");
        return;
    }

    if (d.nt < 0 || d.nt > 255) {
        vsapi->setError(out, "TDeintMod: nt must be between 0 and 255 (inclusive)");
        return;
    }

    if (d.minthresh < 0 || d.minthresh > 255) {
        vsapi->setError(out, "TDeintMod: minthresh must be between 0 and 255 (inclusive)");
        return;
    }

    if (d.maxthresh < 0 || d.maxthresh > 255) {
        vsapi->setError(out, "TDeintMod: maxthresh must be between 0 and 255 (inclusive)");
        return;
    }

    if (d.athresh < -1 || d.athresh > 255) {
        vsapi->setError(out, "TDeintMod: athresh must be between -1 and 255 (inclusive)");
        return;
    }

    if (d.metric < 0 || d.metric > 1) {
        vsapi->setError(out, "TDeintMod: metric must be 0 or 1");
        return;
    }

    if (d.expand < 0) {
        vsapi->setError(out, "TDeintMod: expand must be greater than or equal to 0");
        return;
    }

    if (opt < 0 || opt > 3) {
        vsapi->setError(out, "TDeintMod: opt must be 0, 1, 2 or 3");
        return;
    }

    d.node = vsapi->propGetNode(in, "clip", 0, nullptr);
    d.vi = *vsapi->getVideoInfo(d.node);

    if (!isConstantFormat(&d.vi) || d.vi.format->sampleType != stInteger || d.vi.format->bitsPerSample > 16) {
        vsapi->setError(out, "TDeintMod: only constant format 8-16 bit integer input supported");
        vsapi->freeNode(d.node);
        return;
    }

    if (d.vi.height < 4) {
        vsapi->setError(out, "TDeintMod: height must be greater than or equal to 4");
        vsapi->freeNode(d.node);
        return;
    }

    if (d.vi.width & 1 || d.vi.height & 1) {
        vsapi->setError(out, "TDeintMod: width and height must be multiples of 2");
        vsapi->freeNode(d.node);
        return;
    }

    if (d.vi.format->subSamplingW > 1) {
        vsapi->setError(out, "TDeintMod: only horizontal chroma subsampling 1x-2x supported");
        vsapi->freeNode(d.node);
        return;
    }

    if (d.vi.format->subSamplingH > 1) {
        vsapi->setError(out, "TDeintMod: only vertical chroma subsampling 1x-2x supported");
        vsapi->freeNode(d.node);
        return;
    }

    if (d.link && d.vi.format->colorFamily == cmGray) {
        vsapi->setError(out, "TDeintMod: link can not be true for Gray color family");
        vsapi->freeNode(d.node);
        return;
    }

    const int m = vsapi->propNumElements(in, "planes");

    for (int i = 0; i < 3; i++)
        d.process[i] = (m <= 0);

    for (int i = 0; i < m; i++) {
        const int n = int64ToIntS(vsapi->propGetInt(in, "planes", i, nullptr));

        if (n < 0 || n >= d.vi.format->numPlanes) {
            vsapi->setError(out, "TDeintMod: plane index out of range");
            vsapi->freeNode(d.node);
            return;
        }

        if (d.process[n]) {
            vsapi->setError(out, "TDeintMod: plane specified twice");
            vsapi->freeNode(d.node);
            return;
        }

        d.process[n] = true;
    }

    selectFunctions(opt, &d);

    d.format = vsapi->registerFormat(cmGray, stInteger, d.vi.format->bitsPerSample, 0, 0, core);
    d.widthPad = 32 / d.vi.format->bytesPerSample;
    d.peak = (1 << d.vi.format->bitsPerSample) - 1;

    if (d.mtqL > -2 || d.mthL > -2 || d.mtqC > -2 || d.mthC > -2) {
        if (d.mtqL > -1)
            d.mtqL = d.mtqL * d.peak / 255;
        if (d.mthL > -1)
            d.mthL = d.mthL * d.peak / 255;
        if (d.mtqC > -1)
            d.mtqC = d.mtqC * d.peak / 255;
        if (d.mthC > -1)
            d.mthC = d.mthC * d.peak / 255;
        d.nt = d.nt * d.peak / 255;
        d.minthresh = d.minthresh * d.peak / 255;
        d.maxthresh = d.maxthresh * d.peak / 255;

        for (int plane = 0; plane < d.vi.format->numPlanes; plane++) {
            d.hShift[plane] = plane ? d.vi.format->subSamplingW : 0;
            d.vShift[plane] = plane ? 1 << d.vi.format->subSamplingH : 1;
            d.hHalf[plane] = d.hShift[plane] ? 1 << (d.hShift[plane] - 1) : d.hShift[plane];
            d.vHalf[plane] = 1 << (d.vShift[plane] - 1);
        }

        VSMap * args = vsapi->createMap();
        VSPlugin * stdPlugin = vsapi->getPluginById("com.vapoursynth.std", core);

        vsapi->propSetNode(args, "clip", d.node, paReplace);
        vsapi->freeNode(d.node);
        vsapi->propSetData(args, "prop", "_FieldBased", -1, paReplace);
        vsapi->propSetInt(args, "intval", 2, paReplace);
        VSMap * ret = vsapi->invoke(stdPlugin, "SetFrameProp", args);
        d.node = vsapi->propGetNode(ret, "clip", 0, nullptr);
        vsapi->clearMap(args);
        vsapi->freeMap(ret);

        vsapi->propSetNode(args, "clip", d.node, paReplace);
        vsapi->freeNode(d.node);
        vsapi->propSetInt(args, "tff", 1, paReplace);
        ret = vsapi->invoke(stdPlugin, "SeparateFields", args);
        VSNodeRef * separated = vsapi->propGetNode(ret, "clip", 0, nullptr);
        vsapi->clearMap(args);
        vsapi->freeMap(ret);

        vsapi->propSetNode(args, "clip", separated, paReplace);
        vsapi->propSetInt(args, "cycle", 2, paReplace);
        vsapi->propSetInt(args, "offsets", 0, paReplace);
        ret = vsapi->invoke(stdPlugin, "SelectEvery", args);
        d.node = vsapi->propGetNode(ret, "clip", 0, nullptr);
        d.vi = *vsapi->getVideoInfo(d.node);
        vsapi->clearMap(args);
        vsapi->freeMap(ret);

        TDeintModData * data = new TDeintModData{ d };

        vsapi->createFilter(in, out, "TDeintMod", tdeintmodInit, tdeintmodCreateMMGetFrame, tdeintmodCreateMMFree, fmParallel, 0, data, core);
        VSNodeRef * temp = vsapi->propGetNode(out, "clip", 0, nullptr);
        vsapi->propSetNode(args, "clip", temp, paReplace);
        vsapi->freeNode(temp);
        ret = vsapi->invoke(stdPlugin, "Cache", args);
        temp = vsapi->propGetNode(ret, "clip", 0, nullptr);
        vsapi->clearMap(out);
        vsapi->clearMap(args);
        vsapi->freeMap(ret);

        vsapi->propSetNode(args, "clip", separated, paReplace);
        vsapi->freeNode(separated);
        vsapi->propSetInt(args, "cycle", 2, paReplace);
        vsapi->propSetInt(args, "offsets", 1, paReplace);
        ret = vsapi->invoke(stdPlugin, "SelectEvery", args);
        d.node = vsapi->propGetNode(ret, "clip", 0, nullptr);
        d.vi = *vsapi->getVideoInfo(d.node);
        vsapi->clearMap(args);
        vsapi->freeMap(ret);

        data = new TDeintModData{ d };

        vsapi->createFilter(in, out, "TDeintMod", tdeintmodInit, tdeintmodCreateMMGetFrame, tdeintmodCreateMMFree, fmParallel, 0, data, core);
        d.node2 = vsapi->propGetNode(out, "clip", 0, nullptr);
        vsapi->propSetNode(args, "clip", d.node2, paReplace);
        vsapi->freeNode(d.node2);
        ret = vsapi->invoke(stdPlugin, "Cache", args);
        d.node2 = vsapi->propGetNode(ret, "clip", 0, nullptr);
        vsapi->clearMap(out);
        vsapi->clearMap(args);
        vsapi->freeMap(ret);

        d.node = temp;
        d.propNode = vsapi->propGetNode(in, "clip", 0, nullptr);
        d.vi = *vsapi->getVideoInfo(d.node);
        d.viSaved = vsapi->getVideoInfo(d.node);

        d.vi.height *= 2;
        if (d.mode == 1)
            d.vi.numFrames *= 2;

        d.gvlut = new uint8_t[d.length];
        for (int i = 0; i < d.length; i++)
            d.gvlut[i] = (i == 0) ? 1 : (i == d.length - 1 ? 4 : 2);

        if (d.mtype == 0) {
            d.vlut = {
                0, 1, 2, 2, 3, 0, 2, 2,
                1, 1, 2, 2, 0, 1, 2, 2,
                2, 2, 2, 2, 2, 2, 2, 2,
                2, 2, 2, 2, 2, 2, 2, 2,
                3, 0, 2, 2, 3, 3, 2, 2,
                0, 1, 2, 2, 3, 1, 2, 2,
                2, 2, 2, 2, 2, 2, 2, 2,
                2, 2, 2, 2, 2, 2, 2, 2
            };
        } else if (d.mtype == 1) {
            d.vlut = {
                0, 0, 2, 2, 0, 0, 2, 2,
                0, 1, 2, 2, 0, 1, 2, 2,
                2, 2, 2, 2, 2, 2, 2, 2,
                2, 2, 2, 2, 2, 2, 2, 2,
                0, 0, 2, 2, 3, 3, 2, 2,
                0, 1, 2, 2, 3, 1, 2, 2,
                2, 2, 2, 2, 2, 2, 2, 2,
                2, 2, 2, 2, 2, 2, 2, 2
            };
        } else {
            d.vlut = {
                0, 0, 0, 0, 0, 0, 0, 0,
                0, 1, 0, 1, 0, 1, 0, 1,
                0, 0, 2, 2, 0, 0, 2, 2,
                0, 1, 2, 2, 0, 1, 2, 2,
                0, 0, 0, 0, 3, 3, 3, 3,
                0, 1, 0, 1, 3, 1, 3, 1,
                0, 0, 2, 2, 3, 3, 2, 2,
                0, 1, 2, 2, 3, 1, 2, 2
            };
        }

        d.tmmlut16 = {
            60, 20, 50, 10, 60, 10, 40, 30,
            60, 10, 40, 30, 60, 20, 50, 10
        };

        data = new TDeintModData{ d };

        vsapi->createFilter(in, out, "TDeintMod", tdeintmodInit, tdeintmodBuildMMGetFrame, tdeintmodBuildMMFree, fmParallel, 0, data, core);
        d.mask = vsapi->propGetNode(out, "clip", 0, nullptr);
        vsapi->propSetNode(args, "clip", d.mask, paReplace);
        vsapi->freeNode(d.mask);
        ret = vsapi->invoke(stdPlugin, "Cache", args);
        d.mask = vsapi->propGetNode(ret, "clip", 0, nullptr);
        vsapi->clearMap(out);
        vsapi->freeMap(args);
        vsapi->freeMap(ret);
    }

    if (d.athresh > -1) {
        d.athresh = d.athresh * d.peak / 255;
        d.athresh6 = d.athresh * 6;
        d.athreshsq = d.athresh * d.athresh;
    }

    if (d.mask)
        d.node = vsapi->propGetNode(in, "clip", 0, nullptr);
    d.edeint = vsapi->propGetNode(in, "edeint", 0, &err);
    d.vi = *vsapi->getVideoInfo(d.node);
    d.viSaved = vsapi->getVideoInfo(d.node);

    if (d.mode == 1) {
        if (d.vi.numFrames > INT_MAX / 2) {
            vsapi->setError(out, "TDeintMod: resulting clip is too long");
            vsapi->freeNode(d.node);
            vsapi->freeNode(d.mask);
            vsapi->freeNode(d.edeint);
            return;
        }
        d.vi.numFrames *= 2;

        if (d.vi.fpsNum && d.vi.fpsDen)
            muldivRational(&d.vi.fpsNum, &d.vi.fpsDen, 2, 1);
    }

    if (d.edeint) {
        if (!isSameFormat(vsapi->getVideoInfo(d.edeint), &d.vi)) {
            vsapi->setError(out, "TDeintMod: edeint clip must have the same dimensions as main clip and be the same format");
            vsapi->freeNode(d.node);
            vsapi->freeNode(d.mask);
            vsapi->freeNode(d.edeint);
            return;
        }

        if (vsapi->getVideoInfo(d.edeint)->numFrames != d.vi.numFrames) {
            vsapi->setError(out, "TDeintMod: edeint clip's number of frames doesn't match");
            vsapi->freeNode(d.node);
            vsapi->freeNode(d.mask);
            vsapi->freeNode(d.edeint);
            return;
        }
    }

    TDeintModData * data = new TDeintModData{ d };

    vsapi->createFilter(in, out, "TDeintMod", tdeintmodInit, tdeintmodGetFrame, tdeintmodFree, fmParallel, 0, data, core);
}

//////////////////////////////////////////
// IsCombed

struct IsCombedData {
    VSNodeRef * node;
    const VSVideoInfo * vi;
    int cthresh, blockx, blocky, MI, metric;
    bool chroma;
    int cthresh6, cthreshsq, xHalf, yHalf, xShift, yShift, arraySize, xBlocks4, widtha, heighta;
    std::unordered_map<std::thread::id, int *> cArray;
    std::mutex unordered_map_mutex;
};

static bool isPowerOf2(const int i) noexcept {
    return i && !(i & (i - 1));
}

template<typename T>
static int64_t checkCombed(const VSFrameRef * src, VSFrameRef * cmask, IsCombedData * d, const VSAPI * vsapi) noexcept {
    constexpr T peak = std::numeric_limits<T>::max();
    d->unordered_map_mutex.lock();
    int * VS_RESTRICT cArray = d->cArray.at(std::this_thread::get_id());
    d->unordered_map_mutex.unlock();

    for (int plane = 0; plane < (d->chroma ? 3 : 1); plane++) {
        const int width = vsapi->getFrameWidth(src, plane);
        const int height = vsapi->getFrameHeight(src, plane);
        const int stride = vsapi->getStride(src, plane) / sizeof(T);
        const T * srcp = reinterpret_cast<const T *>(vsapi->getReadPtr(src, plane));
        T * VS_RESTRICT cmkp = reinterpret_cast<T *>(vsapi->getWritePtr(cmask, plane));

        const T * srcppp = srcp - stride * 2;
        const T * srcpp = srcp - stride;
        const T * srcpn = srcp + stride;
        const T * srcpnn = srcp + stride * 2;

        memset(cmkp, 0, vsapi->getStride(cmask, plane) * height);

        if (d->metric == 0) {
            for (int x = 0; x < width; x++) {
                const int sFirst = srcp[x] - srcpn[x];
                if ((sFirst > d->cthresh || sFirst < -d->cthresh) && std::abs(srcpnn[x] + srcp[x] * 4 + srcpnn[x] - 3 * (srcpn[x] + srcpn[x])) > d->cthresh6)
                    cmkp[x] = peak;
            }
            srcppp += stride;
            srcpp += stride;
            srcp += stride;
            srcpn += stride;
            srcpnn += stride;
            cmkp += stride;

            for (int x = 0; x < width; x++) {
                const int sFirst = srcp[x] - srcpp[x];
                const int sSecond = srcp[x] - srcpn[x];
                if (((sFirst > d->cthresh && sSecond > d->cthresh) || (sFirst < -d->cthresh && sSecond < -d->cthresh)) &&
                    std::abs(srcpnn[x] + srcp[x] * 4 + srcpnn[x] - 3 * (srcpp[x] + srcpn[x])) > d->cthresh6)
                    cmkp[x] = peak;
            }
            srcppp += stride;
            srcpp += stride;
            srcp += stride;
            srcpn += stride;
            srcpnn += stride;
            cmkp += stride;

            for (int y = 2; y < height - 2; y++) {
                for (int x = 0; x < width; x++) {
                    const int sFirst = srcp[x] - srcpp[x];
                    const int sSecond = srcp[x] - srcpn[x];
                    if (((sFirst > d->cthresh && sSecond > d->cthresh) || (sFirst < -d->cthresh && sSecond < -d->cthresh)) &&
                        std::abs(srcppp[x] + srcp[x] * 4 + srcpnn[x] - 3 * (srcpp[x] + srcpn[x])) > d->cthresh6)
                        cmkp[x] = peak;
                }
                srcppp += stride;
                srcpp += stride;
                srcp += stride;
                srcpn += stride;
                srcpnn += stride;
                cmkp += stride;
            }

            for (int x = 0; x < width; x++) {
                const int sFirst = srcp[x] - srcpp[x];
                const int sSecond = srcp[x] - srcpn[x];
                if (((sFirst > d->cthresh && sSecond > d->cthresh) || (sFirst < -d->cthresh && sSecond < -d->cthresh)) &&
                    std::abs(srcppp[x] + srcp[x] * 4 + srcppp[x] - 3 * (srcpp[x] + srcpn[x])) > d->cthresh6)
                    cmkp[x] = peak;
            }
            srcppp += stride;
            srcpp += stride;
            srcp += stride;
            srcpn += stride;
            srcpnn += stride;
            cmkp += stride;

            for (int x = 0; x < width; x++) {
                const int sFirst = srcp[x] - srcpp[x];
                if ((sFirst > d->cthresh || sFirst < -d->cthresh) && std::abs(srcppp[x] + srcp[x] * 4 + srcppp[x] - 3 * (srcpp[x] + srcpp[x])) > d->cthresh6)
                    cmkp[x] = peak;
            }
        } else {
            for (int x = 0; x < width; x++) {
                if ((srcp[x] - srcpn[x]) * (srcp[x] - srcpn[x]) > d->cthreshsq)
                    cmkp[x] = peak;
            }
            srcpp += stride;
            srcp += stride;
            srcpn += stride;
            cmkp += stride;

            for (int y = 1; y < height - 1; y++) {
                for (int x = 0; x < width; x++) {
                    if ((srcp[x] - srcpp[x]) * (srcp[x] - srcpn[x]) > d->cthreshsq)
                        cmkp[x] = peak;
                }
                srcpp += stride;
                srcp += stride;
                srcpn += stride;
                cmkp += stride;
            }

            for (int x = 0; x < width; x++) {
                if ((srcp[x] - srcpp[x]) * (srcp[x] - srcpp[x]) > d->cthreshsq)
                    cmkp[x] = peak;
            }
        }
    }

    if (d->chroma) {
        const int width = vsapi->getFrameWidth(cmask, 2);
        const int height = vsapi->getFrameHeight(cmask, 2);
        const int stride = vsapi->getStride(cmask, 0) / sizeof(T);
        const int strideY = stride << d->vi->format->subSamplingH;
        const int strideUV = vsapi->getStride(cmask, 2) / sizeof(T);
        T * VS_RESTRICT cmkp = reinterpret_cast<T *>(vsapi->getWritePtr(cmask, 0));
        const T * cmkpU = reinterpret_cast<const T *>(vsapi->getReadPtr(cmask, 1));
        const T * cmkpV = reinterpret_cast<const T *>(vsapi->getReadPtr(cmask, 2));

        T * VS_RESTRICT cmkpp3 = cmkp - stride * 3;
        T * VS_RESTRICT cmkpp2 = cmkp - stride * 2;
        T * VS_RESTRICT cmkpp = cmkp - stride;
        T * VS_RESTRICT cmkpn = cmkp + stride;
        T * VS_RESTRICT cmkpn2 = cmkp + stride * 2;
        const T * cmkppU = cmkpU - strideUV;
        const T * cmkpnU = cmkpU + strideUV;
        const T * cmkppV = cmkpV - strideUV;
        const T * cmkpnV = cmkpV + strideUV;

        for (int y = 1; y < height - 1; y++) {
            cmkpp3 += strideY;
            cmkpp2 += strideY;
            cmkpp += strideY;
            cmkp += strideY;
            cmkpn += strideY;
            cmkpn2 += strideY;
            cmkppU += strideUV;
            cmkpU += strideUV;
            cmkpnU += strideUV;
            cmkppV += strideUV;
            cmkpV += strideUV;
            cmkpnV += strideUV;

            for (int x = 1; x < width - 1; x++) {
                if ((cmkpU[x] && (cmkpU[x - 1] || cmkpU[x + 1] || cmkppU[x - 1] || cmkppU[x] || cmkppU[x + 1] || cmkpnU[x - 1] || cmkpnU[x] || cmkpnU[x + 1])) ||
                    (cmkpV[x] && (cmkpV[x - 1] || cmkpV[x + 1] || cmkppV[x - 1] || cmkppV[x] || cmkppV[x + 1] || cmkpnV[x - 1] || cmkpnV[x] || cmkpnV[x + 1]))) {
                    if (d->vi->format->subSamplingW == 0) {
                        cmkp[x] = peak;

                        if (d->vi->format->subSamplingH > 0) {
                            cmkpn[x] = peak;
                            (y & 1 ? cmkpp : cmkpn2)[x] = peak;

                            if (d->vi->format->subSamplingH == 2) {
                                cmkpp2[x] = peak;
                                (y & 1 ? cmkpp3 : cmkpp)[x] = peak;
                            }
                        }
                    } else if (d->vi->format->subSamplingW == 1) {
                        if (std::is_same<T, uint8_t>::value) {
                            constexpr uint16_t peak2 = std::numeric_limits<uint16_t>::max();
                            reinterpret_cast<uint16_t *>(cmkp)[x] = peak2;

                            if (d->vi->format->subSamplingH > 0) {
                                reinterpret_cast<uint16_t *>(cmkpn)[x] = peak2;
                                reinterpret_cast<uint16_t *>(y & 1 ? cmkpp : cmkpn2)[x] = peak2;

                                if (d->vi->format->subSamplingH == 2) {
                                    reinterpret_cast<uint16_t *>(cmkpp2)[x] = peak2;
                                    reinterpret_cast<uint16_t *>(y & 1 ? cmkpp3 : cmkpp)[x] = peak2;
                                }
                            }
                        } else {
                            constexpr uint32_t peak2 = std::numeric_limits<uint32_t>::max();
                            reinterpret_cast<uint32_t *>(cmkp)[x] = peak2;

                            if (d->vi->format->subSamplingH > 0) {
                                reinterpret_cast<uint32_t *>(cmkpn)[x] = peak2;
                                reinterpret_cast<uint32_t *>(y & 1 ? cmkpp : cmkpn2)[x] = peak2;

                                if (d->vi->format->subSamplingH == 2) {
                                    reinterpret_cast<uint32_t *>(cmkpp2)[x] = peak2;
                                    reinterpret_cast<uint32_t *>(y & 1 ? cmkpp3 : cmkpp)[x] = peak2;
                                }
                            }
                        }
                    } else {
                        if (std::is_same<T, uint8_t>::value) {
                            constexpr uint32_t peak2 = std::numeric_limits<uint32_t>::max();
                            reinterpret_cast<uint32_t *>(cmkp)[x] = peak2;

                            if (d->vi->format->subSamplingH > 0) {
                                reinterpret_cast<uint32_t *>(cmkpn)[x] = peak2;
                                reinterpret_cast<uint32_t *>(y & 1 ? cmkpp : cmkpn2)[x] = peak2;

                                if (d->vi->format->subSamplingH == 2) {
                                    reinterpret_cast<uint32_t *>(cmkpp2)[x] = peak2;
                                    reinterpret_cast<uint32_t *>(y & 1 ? cmkpp3 : cmkpp)[x] = peak2;
                                }
                            }
                        } else {
                            constexpr uint64_t peak2 = std::numeric_limits<uint64_t>::max();
                            reinterpret_cast<uint64_t *>(cmkp)[x] = peak2;

                            if (d->vi->format->subSamplingH > 0) {
                                reinterpret_cast<uint64_t *>(cmkpn)[x] = peak2;
                                reinterpret_cast<uint64_t *>(y & 1 ? cmkpp : cmkpn2)[x] = peak2;

                                if (d->vi->format->subSamplingH == 2) {
                                    reinterpret_cast<uint64_t *>(cmkpp2)[x] = peak2;
                                    reinterpret_cast<uint64_t *>(y & 1 ? cmkpp3 : cmkpp)[x] = peak2;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    const int width = vsapi->getFrameWidth(cmask, 0);
    const int height = vsapi->getFrameHeight(cmask, 0);
    const int stride = vsapi->getStride(cmask, 0) / sizeof(T);
    const T * cmkp = reinterpret_cast<const T *>(vsapi->getReadPtr(cmask, 0)) + stride;

    const T * cmkpp = cmkp - stride;
    const T * cmkpn = cmkp + stride;

    memset(cArray, 0, d->arraySize * sizeof(int));

    for (int y = 1; y < d->yHalf; y++) {
        const int temp1 = (y >> d->yShift) * d->xBlocks4;
        const int temp2 = ((y + d->yHalf) >> d->yShift) * d->xBlocks4;

        for (int x = 0; x < width; x++) {
            if (cmkpp[x] && cmkp[x] && cmkpn[x]) {
                const int box1 = (x >> d->xShift) * 4;
                const int box2 = ((x + d->xHalf) >> d->xShift) * 4;
                ++cArray[temp1 + box1];
                ++cArray[temp1 + box2 + 1];
                ++cArray[temp2 + box1 + 2];
                ++cArray[temp2 + box2 + 3];
            }
        }

        cmkpp += stride;
        cmkp += stride;
        cmkpn += stride;
    }

    for (int y = d->yHalf; y < d->heighta; y += d->yHalf) {
        const int temp1 = (y >> d->yShift) * d->xBlocks4;
        const int temp2 = ((y + d->yHalf) >> d->yShift) * d->xBlocks4;

        for (int x = 0; x < d->widtha; x += d->xHalf) {
            const T * cmkppT = cmkpp;
            const T * cmkpT = cmkp;
            const T * cmkpnT = cmkpn;
            int sum = 0;

            for (int u = 0; u < d->yHalf; u++) {
                for (int v = 0; v < d->xHalf; v++) {
                    if (cmkppT[x + v] && cmkpT[x + v] && cmkpnT[x + v])
                        sum++;
                }
                cmkppT += stride;
                cmkpT += stride;
                cmkpnT += stride;
            }

            if (sum) {
                const int box1 = (x >> d->xShift) * 4;
                const int box2 = ((x + d->xHalf) >> d->xShift) * 4;
                cArray[temp1 + box1] += sum;
                cArray[temp1 + box2 + 1] += sum;
                cArray[temp2 + box1 + 2] += sum;
                cArray[temp2 + box2 + 3] += sum;
            }
        }

        for (int x = d->widtha; x < width; x++) {
            const T * cmkppT = cmkpp;
            const T * cmkpT = cmkp;
            const T * cmkpnT = cmkpn;
            int sum = 0;

            for (int u = 0; u < d->yHalf; u++) {
                if (cmkppT[x] && cmkpT[x] && cmkpnT[x])
                    sum++;
                cmkppT += stride;
                cmkpT += stride;
                cmkpnT += stride;
            }

            if (sum) {
                const int box1 = (x >> d->xShift) * 4;
                const int box2 = ((x + d->xHalf) >> d->xShift) * 4;
                cArray[temp1 + box1] += sum;
                cArray[temp1 + box2 + 1] += sum;
                cArray[temp2 + box1 + 2] += sum;
                cArray[temp2 + box2 + 3] += sum;
            }
        }

        cmkpp += stride * d->yHalf;
        cmkp += stride * d->yHalf;
        cmkpn += stride * d->yHalf;
    }

    for (int y = d->heighta; y < height - 1; y++) {
        const int temp1 = (y >> d->yShift) * d->xBlocks4;
        const int temp2 = ((y + d->yHalf) >> d->yShift) * d->xBlocks4;

        for (int x = 0; x < width; x++) {
            if (cmkpp[x] && cmkp[x] && cmkpn[x]) {
                const int box1 = (x >> d->xShift) * 4;
                const int box2 = ((x + d->xHalf) >> d->xShift) * 4;
                ++cArray[temp1 + box1];
                ++cArray[temp1 + box2 + 1];
                ++cArray[temp2 + box1 + 2];
                ++cArray[temp2 + box2 + 3];
            }
        }

        cmkpp += stride;
        cmkp += stride;
        cmkpn += stride;
    }

    int MIC = 0;
    for (int x = 0; x < d->arraySize; x++) {
        if (cArray[x] > MIC)
            MIC = cArray[x];
    }
    return MIC > d->MI;
}

static void VS_CC iscombedInit(VSMap *in, VSMap *out, void **instanceData, VSNode *node, VSCore *core, const VSAPI *vsapi) {
    IsCombedData * d = static_cast<IsCombedData *>(*instanceData);
    vsapi->setVideoInfo(d->vi, 1, node);
}

static const VSFrameRef *VS_CC iscombedGetFrame(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    IsCombedData * d = static_cast<IsCombedData *>(*instanceData);

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, d->node, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        auto threadId = std::this_thread::get_id();
        d->unordered_map_mutex.lock();
        if (!d->cArray.count(threadId)) {
            int * cArray = new (std::nothrow) int[d->arraySize];
            if (!cArray) {
                vsapi->setFilterError("IsCombed: malloc failure (cArray)", frameCtx);
                return nullptr;
            }
            d->cArray.emplace(threadId, cArray);
        }
        d->unordered_map_mutex.unlock();
        const VSFrameRef * src = vsapi->getFrameFilter(n, d->node, frameCtx);
        VSFrameRef * cmask = vsapi->newVideoFrame(d->vi->format, d->vi->width, d->vi->height, nullptr, core);
        VSFrameRef * dst = vsapi->copyFrame(src, core);

        if (d->vi->format->bytesPerSample == 1)
            vsapi->propSetInt(vsapi->getFramePropsRW(dst), "_Combed", checkCombed<uint8_t>(src, cmask, d, vsapi), paReplace);
        else
            vsapi->propSetInt(vsapi->getFramePropsRW(dst), "_Combed", checkCombed<uint16_t>(src, cmask, d, vsapi), paReplace);

        vsapi->freeFrame(src);
        vsapi->freeFrame(cmask);
        return dst;
    }

    return nullptr;
}

static void VS_CC iscombedFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    IsCombedData * d = static_cast<IsCombedData *>(instanceData);

    vsapi->freeNode(d->node);

    for (auto & iter : d->cArray)
        delete[] iter.second;

    delete d;
}

static void VS_CC iscombedCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    std::unique_ptr<IsCombedData> d{ new IsCombedData{} };
    int err;

    d->node = vsapi->propGetNode(in, "clip", 0, nullptr);
    d->vi = vsapi->getVideoInfo(d->node);

    try {
        if (!isConstantFormat(d->vi) || d->vi->format->sampleType != stInteger || d->vi->format->bitsPerSample > 16)
            throw std::string{ "only constant format 8-16 bit integer input supported" };

        if (d->vi->height < 5)
            throw std::string{ "height must be greater than or equal to 5" };

        if (d->vi->format->subSamplingW > 2)
            throw std::string{ "only horizontal chroma subsampling 1x-4x supported" };

        if (d->vi->format->subSamplingH > 2)
            throw std::string{ "only vertical chroma subsampling 1x-4x supported" };

        d->cthresh = int64ToIntS(vsapi->propGetInt(in, "cthresh", 0, &err));
        if (err)
            d->cthresh = 6;

        d->blockx = int64ToIntS(vsapi->propGetInt(in, "blockx", 0, &err));
        if (err)
            d->blockx = 16;

        d->blocky = int64ToIntS(vsapi->propGetInt(in, "blocky", 0, &err));
        if (err)
            d->blocky = 16;

        d->chroma = !!vsapi->propGetInt(in, "chroma", 0, &err);

        d->MI = int64ToIntS(vsapi->propGetInt(in, "mi", 0, &err));
        if (err)
            d->MI = 64;

        d->metric = int64ToIntS(vsapi->propGetInt(in, "metric", 0, &err));

        if (d->cthresh < 0 || d->cthresh > 255)
            throw std::string{ "cthresh must be between 0 and 255 (inclusive)" };

        if (!isPowerOf2(d->blockx) || d->blockx < 4 || d->blockx > 2048)
            throw std::string{ "illegal blockx size" };

        if (!isPowerOf2(d->blocky) || d->blocky < 4 || d->blocky > 2048)
            throw std::string{ "illegal blocky size" };

        if (d->chroma && d->vi->format->colorFamily == cmGray)
            throw std::string{ "chroma can not be true for Gray color family" };

        if (d->MI < 0)
            throw std::string{ "mi must be greater than or equal to 0" };

        if (d->metric < 0 || d->metric > 1)
            throw std::string{ "metric must be 0 or 1" };

        d->cArray.reserve(vsapi->getCoreInfo(core)->numThreads);

        d->cthresh = d->cthresh * ((1 << d->vi->format->bitsPerSample) - 1) / 255;
        d->cthresh6 = d->cthresh * 6;
        d->cthreshsq = d->cthresh * d->cthresh;

        d->xHalf = d->blockx / 2;
        d->yHalf = d->blocky / 2;
        d->xShift = static_cast<int>(std::log2(d->blockx));
        d->yShift = static_cast<int>(std::log2(d->blocky));

        const int xBlocks = ((d->vi->width + d->xHalf) >> d->xShift) + 1;
        const int yBlocks = ((d->vi->height + d->yHalf) >> d->yShift) + 1;
        d->arraySize = xBlocks * yBlocks * 4;
        d->xBlocks4 = xBlocks * 4;

        d->widtha = (d->vi->width >> (d->xShift - 1)) << (d->xShift - 1);
        d->heighta = (d->vi->height >> (d->yShift - 1)) << (d->yShift - 1);
        if (d->heighta == d->vi->height)
            d->heighta = d->vi->height - d->yHalf;
    } catch (const std::string & error) {
        vsapi->setError(out, ("IsCombed: " + error).c_str());
        vsapi->freeNode(d->node);
        return;
    }

    vsapi->createFilter(in, out, "IsCombed", iscombedInit, iscombedGetFrame, iscombedFree, fmParallel, 0, d.release(), core);
}

//////////////////////////////////////////
// Init

VS_EXTERNAL_API(void) VapourSynthPluginInit(VSConfigPlugin configFunc, VSRegisterFunction registerFunc, VSPlugin *plugin) {
    configFunc("com.holywu.tdeintmod", "tdm", "A bi-directionally motion adaptive deinterlacer", VAPOURSYNTH_API_VERSION, 1, plugin);
    registerFunc("TDeintMod",
                 "clip:clip;"
                 "order:int;"
                 "field:int:opt;"
                 "mode:int:opt;"
                 "length:int:opt;"
                 "mtype:int:opt;"
                 "ttype:int:opt;"
                 "mtql:int:opt;"
                 "mthl:int:opt;"
                 "mtqc:int:opt;"
                 "mthc:int:opt;"
                 "nt:int:opt;"
                 "minthresh:int:opt;"
                 "maxthresh:int:opt;"
                 "cstr:int:opt;"
                 "athresh:int:opt;"
                 "metric:int:opt;"
                 "expand:int:opt;"
                 "link:int:opt;"
                 "show:int:opt;"
                 "edeint:clip:opt;"
                 "opt:int:opt;"
                 "planes:int[]:opt;",
                 tdeintmodCreate, nullptr, plugin);
    registerFunc("IsCombed",
                 "clip:clip;"
                 "cthresh:int:opt;"
                 "blockx:int:opt;"
                 "blocky:int:opt;"
                 "chroma:int:opt;"
                 "mi:int:opt;"
                 "metric:int:opt;",
                 iscombedCreate, nullptr, plugin);
}
