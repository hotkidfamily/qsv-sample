// qsv-demo.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include "windows.h"
#include <vector>
#include <fstream>
#include <string>
#include <sstream>
#include <deque>

#include <mfxvideo++.h>

#pragma comment(lib, "libmfx_vs2015.lib")

#define MSDK_ALIGN16(value) (((value + 15) >> 4) << 4) // round up to a multiple of 16
#define MSDK_ALIGN32(value) (((value + 31) >> 5) << 5) // round up to a multiple of 32

typedef struct {
    mfxBitstream mfxBS;
    mfxSyncPoint syncp;
} encOpera;

typedef struct _tagContext
{
    mfxExtBuffer* exBufs[4];
    mfxExtCodingOption co;
    mfxExtCodingOption2 co2;
    mfxExtCodingOption3 co3;
    mfxExtVideoSignalInfo vui;

    mfxFrameSurface1* surfaces = nullptr;
    int32_t surfacesCnt = 0;

    encOpera* encChain = nullptr;

    MFXVideoSession* session = nullptr;
    MFXVideoENCODE* encoder = nullptr;
    mfxVideoParam encParams;

    int32_t asyncDepth = 4;

    std::deque<int64_t> ptsQueue;

    uint64_t firstPts = UINT64_MAX;
} APPContext;

static int _log(const char *fmt, ...)
{
    va_list vl;
    va_start(vl, fmt);
    char buf[1024] = { 0 };

    vsnprintf(buf, 256, fmt, vl);

    fprintf(stderr, "%s\r\n", buf);

    va_end(vl);
    return 0;
}

bool setupVideoParams(APPContext *ctx, int fps, int kbps, int width, int height, int bframes)
{
    auto& vp = ctx->encParams;
    auto& exBufs = ctx->exBufs;
    auto& co = ctx->co;
    auto& co2 = ctx->co2;
    auto& co3 = ctx->co3;
    auto& vui = ctx->vui;
    auto& asyncDepth = ctx->asyncDepth;
    auto& mfx = ctx->encParams.mfx;

    {
        auto &frame = mfx.FrameInfo;
        frame.Width = MSDK_ALIGN16(width);
        frame.Height = MSDK_ALIGN16(height);

        frame.CropX = 0;
        frame.CropY = 0;
        frame.CropW = width;
        frame.CropH = height;

        frame.FourCC = MFX_FOURCC_NV12;
        frame.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
        frame.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
        frame.FrameRateExtN = fps;
        frame.FrameRateExtD = 1;
    }

    vp.AsyncDepth = asyncDepth;
    vp.IOPattern = MFX_IOPATTERN_IN_SYSTEM_MEMORY;

    mfx.CodecId = MFX_CODEC_AVC;
    mfx.CodecProfile = MFX_PROFILE_AVC_HIGH;
    mfx.TargetUsage = MFX_TARGETUSAGE_BALANCED;

    {
        mfx.GopPicSize = fps * 2;
        mfx.GopRefDist = bframes + 1; // bframes
        mfx.GopOptFlag = MFX_GOP_CLOSED;
        mfx.IdrInterval = 0; // every I frame is IDR 

        mfx.RateControlMethod = MFX_RATECONTROL_QVBR;

        if (mfx.RateControlMethod == MFX_RATECONTROL_CBR) {
            // HRD  mode
            mfx.MaxKbps = kbps;
            mfx.BufferSizeInKB = kbps / 8;
            mfx.TargetKbps = kbps;
        }
        else if (mfx.RateControlMethod == MFX_RATECONTROL_CQP)
        {
            mfx.QPI = mfx.QPP = mfx.QPB = 23;
        }
        else if (mfx.RateControlMethod == MFX_RATECONTROL_ICQ ||
            mfx.RateControlMethod == MFX_RATECONTROL_LA_ICQ)
        {
            mfx.ICQQuality = 23;
        }
        else if (mfx.RateControlMethod == MFX_RATECONTROL_AVBR)
        {
            mfx.Accuracy = 1;
            mfx.TargetKbps = kbps;
            mfx.Convergence = kbps * 100 / 60 ;
        }
        else if (mfx.RateControlMethod == MFX_RATECONTROL_QVBR)
        {
            mfx.TargetKbps = kbps;
            mfx.InitialDelayInKB = kbps / 8;
            mfx.BufferSizeInKB = kbps / 8;
        }
        else if (mfx.RateControlMethod == MFX_RATECONTROL_VBR)
        {
            mfx.TargetKbps = kbps;
            mfx.InitialDelayInKB = kbps / 8;
            mfx.BufferSizeInKB = kbps / 8;
        }
        else
        {
            mfx.TargetKbps = kbps;
        }

        if (false) // for battery power device using
        {
            mfx.LowPower = MFX_CODINGOPTION_ON;
        }
    }
    mfx.NumSlice = 1;
    mfx.NumRefFrame = 3;
    //mfx.EncodedOrder = 0;

    ZeroMemory(&co, sizeof(mfxExtCodingOption));
    co.Header.BufferId = MFX_EXTBUFF_CODING_OPTION;
    co.Header.BufferSz = sizeof(mfxExtCodingOption);
    co.CAVLC = MFX_CODINGOPTION_OFF;
    co.NalHrdConformance = MFX_CODINGOPTION_OFF;
    co.SingleSeiNalUnit = MFX_CODINGOPTION_ON;
    co.MaxDecFrameBuffering = 3;
    co.FramePicture = MFX_CODINGOPTION_OFF; // progressive frame
    co.PicTimingSEI = MFX_CODINGOPTION_OFF;
    co.AUDelimiter = MFX_CODINGOPTION_OFF;

    ZeroMemory(&co2, sizeof(mfxExtCodingOption2));
    co2.Header.BufferId = MFX_EXTBUFF_CODING_OPTION2;
    co2.Header.BufferSz = sizeof(mfxExtCodingOption2);
    co2.RepeatPPS = MFX_CODINGOPTION_OFF;

    co2.AdaptiveI = MFX_CODINGOPTION_OFF;
    co2.AdaptiveB = MFX_CODINGOPTION_ON;
    co2.LookAheadDepth = 10;
    co2.BRefType = MFX_B_REF_OFF;
    co2.DisableDeblockingIdc = MFX_CODINGOPTION_OFF; // should check codec caps
    co2.FixedFrameRate = MFX_CODINGOPTION_OFF;

    ZeroMemory(&co3, sizeof(mfxExtCodingOption3));
    co3.Header.BufferId = MFX_EXTBUFF_CODING_OPTION3;
    co3.Header.BufferSz = sizeof(mfxExtCodingOption3);
    co3.NumSliceI = co3.NumSliceP = co3.NumSliceB = 1;
    co3.WeightedBiPred = 1;
    co3.WeightedPred = 1;
    co3.ScenarioInfo = MFX_SCENARIO_LIVE_STREAMING;
    co3.ContentInfo = MFX_CONTENT_FULL_SCREEN_VIDEO;

    if (mfx.RateControlMethod == MFX_RATECONTROL_QVBR){
        co3.QVBRQuality = 27;
    }

    ZeroMemory(&vui, sizeof(mfxExtVideoSignalInfo));
    vui.Header.BufferId = MFX_EXTBUFF_VIDEO_SIGNAL_INFO;
    vui.Header.BufferSz = sizeof(mfxExtVideoSignalInfo);
    vui.ColourPrimaries = 1;
    vui.TransferCharacteristics = 1;
    vui.MatrixCoefficients = 1;
    vui.VideoFormat = 0;
    vui.VideoFullRange = 0;
    vui.ColourDescriptionPresent = 1;

    ZeroMemory(exBufs, sizeof(exBufs[0]) * ARRAYSIZE(exBufs));

    exBufs[0] = &co.Header;
    exBufs[1] = &co2.Header;
    exBufs[2] = &co3.Header;
    exBufs[3] = &vui.Header;

    vp.ExtParam = exBufs;
    vp.NumExtParam = (mfxU16)4;

    ctx->encoder->Query(&vp, &vp);

    mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;

    return true;
}

#pragma region buffer_alloc

int allocFrame(APPContext* ctx)
{
    auto& encoder = ctx->encoder;
    auto& vp = ctx->encParams;

    if (encoder) {
        mfxFrameAllocRequest req;
        ZeroMemory(&req, sizeof(mfxFrameAllocRequest));
        encoder->QueryIOSurf(&vp, &req);

        auto width = req.Info.Width;
        auto height = req.Info.Height;
        auto size = width * height * 3 / 2;
        auto& surfaces = ctx->surfaces;
        auto& surfacesCnt = ctx->surfacesCnt;

        surfacesCnt = req.NumFrameSuggested;

        surfaces = new mfxFrameSurface1[req.NumFrameSuggested];
        ZeroMemory(surfaces, sizeof(mfxFrameSurface1)*req.NumFrameSuggested);

        for (auto i = 0; i < req.NumFrameSuggested; i++) {
            auto &surf = surfaces[i];
            memcpy(&surf.Info, &vp.mfx.FrameInfo, sizeof(mfxFrameInfo));
            auto &data = surf.Data;

            mfxU8 *pSurface = (mfxU8 *)_aligned_malloc(size, 16);
            ZeroMemory(pSurface, size);
            data.Y = pSurface;
            data.UV = pSurface + width * height;
            data.Pitch = width; // must not empty, or EncodeFrameAsync return MFX_ERR_UNDEFINED_BEHAVIOR
        }
    }
    return true;
}


bool releaseFrame(APPContext *ctx)
{
    auto& surfaces = ctx->surfaces;
    auto& surfacesCnt = ctx->surfacesCnt;

    if (surfaces)
    {
        for (auto i = 0; i < surfacesCnt; i++) {
            auto &surf = surfaces[i];
            auto &pSurface = surf.Data.Y;
            _aligned_free(pSurface);
        }
        delete[] surfaces;
    }
    return true;
}


mfxStatus allocBitstream(APPContext * ctx, int32_t kbps)
{
    auto& chain = ctx->encChain;
    auto& asyncDepth = ctx->asyncDepth;

    chain = new encOpera[asyncDepth];
    ZeroMemory(chain, sizeof(encOpera) * asyncDepth);

    for (int i = 0; i < asyncDepth; i++) {
        auto &opera = chain[i];
        opera.mfxBS.MaxLength = kbps * 1000 / 8;
        opera.mfxBS.Data = (mfxU8*)_aligned_malloc(opera.mfxBS.MaxLength, 32);// new mfxU8[opera.mfxBS.MaxLength];
        ZeroMemory(opera.mfxBS.Data, opera.mfxBS.MaxLength);
        opera.mfxBS.DataOffset = 0;
        opera.mfxBS.DataLength = 0;
    }

    return MFX_ERR_NONE;
}

void releaseBitstream(APPContext *ctx)
{
    auto& chain = ctx->encChain;
    auto& asyncDepth = ctx->asyncDepth;

    if (chain) {
        for (int i = 0; i < asyncDepth; i++) {
            auto &opera = chain[i];
            _aligned_free(opera.mfxBS.Data);
            opera.mfxBS.Data = nullptr;
        }

        delete[] chain;
    }
}

#pragma endregion buffer_alloc

mfxStatus encode(APPContext* ctx,
    int64_t index,
    mfxFrameSurface1* surf,
    mfxBitstream& bs,
    mfxSyncPoint& syncPt,
    std::ofstream& h264
)
{
    mfxStatus sts = MFX_ERR_NONE;

    if (surf)
        ctx->ptsQueue.push_back(surf->Data.TimeStamp);

    if (ctx->firstPts == UINT64_MAX) {
        ctx->firstPts = surf->Data.TimeStamp;
    }

    sts = ctx->encoder->EncodeFrameAsync(nullptr, surf, &bs, &syncPt);

    if (MFX_ERR_MORE_DATA == sts) {
        if (surf) {
            sts = MFX_ERR_NONE;
        }
    }
    else if (MFX_ERR_NONE < sts && !syncPt) {
        // Repeat the call if warning and no output
        if (MFX_WRN_DEVICE_BUSY == sts)
            Sleep(1); // Wait if device is busy, then repeat the same call
        sts = MFX_ERR_NONE;
    }
    else if (MFX_ERR_NONE < sts && syncPt) {
        sts = MFX_ERR_NONE; // Ignore warnings if output is available
    }
    else if (MFX_ERR_NONE > sts) {
        _log("enc error code : %s", std::to_string(sts).c_str());
    }
    else {
        ctx->session->SyncOperation(syncPt, INFINITE);
        h264.write(reinterpret_cast<const char*>(bs.Data) + bs.DataOffset, bs.DataLength);
        std::string result;
        const char* type = "Err";
        switch (bs.FrameType & 0xf) {
        case MFX_FRAMETYPE_I:
            type = "I";
            if ((bs.FrameType & MFX_FRAMETYPE_IDR) == MFX_FRAMETYPE_IDR) {
                type = "IDR";
            }
            break;
        case MFX_FRAMETYPE_P:
            type = "P";
            break;
        case MFX_FRAMETYPE_B:
            type = "B";
            break;
        default:
        {
            std::stringstream stream;
            stream << std::hex << bs.FrameType;
            result = stream.str();
            type = result.c_str();
        }
        break;
        }
        auto & dts = ctx->ptsQueue.front();
        ctx->ptsQueue.pop_front();

        mfxEncodeStat stat = { 0 };
        ctx->encoder->GetEncodeStat(&stat);

        auto ms = (dts - ctx->firstPts) / 90;
        int64_t kbps = 0;
        if (ms != 0) {
            kbps = stat.NumBit / ms;
        }

        _log("%6lld,%8d,%4s,%8lld,%8lld,%8lld", index, bs.DataLength, type, bs.TimeStamp / 90, dts / 90, kbps);
        bs.DataLength = 0;
        sts = MFX_ERR_NONE;
    }
    return sts;
}

int main(int argc, char *argv[])
{
    int fps = 60;
    int kbps = 8000;
    int width = 1920;
    int height = 1080;
    int bframe = 3;

    APPContext *ctx = new APPContext;

    int pitchW = MSDK_ALIGN16(width);
    int pitchH = MSDK_ALIGN16(height);

    mfxInitParam initP = { 0 };
    initP.Implementation = MFX_IMPL_HARDWARE_ANY;
    initP.GPUCopy = MFX_GPUCOPY_OFF;
    mfxVersion ver;
    ver.Minor = 0;
    ver.Major = 1;
    initP.Version = ver;

    mfxStatus sts;
    mfxIMPL impl;

    ctx->session = new MFXVideoSession();
    sts = ctx->session->InitEx(initP);
    if (sts != MFX_ERR_NONE) {
        _log("can not init session.");
    }
    ctx->session->QueryIMPL(&impl);
    ctx->session->QueryVersion(&ver);

    _log("with impl %x, in %d.%d", impl, ver.Major, ver.Minor);

    {
        switch (MFX_IMPL_BASETYPE(impl)) {
        case MFX_IMPL_HARDWARE:
        case MFX_IMPL_HARDWARE2:
        case MFX_IMPL_HARDWARE3:
        case MFX_IMPL_HARDWARE4:
            _log("using hardware session");
            break;
        default:
            _log("using not hardware session");
            break;
        }
    }

    ctx->encoder = new MFXVideoENCODE(*ctx->session);
    ZeroMemory(&ctx->encParams, sizeof(mfxVideoParam));
    setupVideoParams(ctx, fps, kbps, width, height, bframe);
    sts = ctx->encoder->Init(&ctx->encParams);
    if (sts != MFX_ERR_NONE) {
        _log("can not init encoder.");
    }

    allocFrame(ctx);
    allocBitstream(ctx, kbps);

    std::string ifn("C:\\temps\\pubg-sand-cloud-1920x1080p-60fps.yuv");
    std::string ofn;
    _log("Usage: %s -i input_file_name, \r\n \t the output file will be = input_file_name + \".h264\"", argv[0]);
    for (auto i = 1; i < argc - 1; i++) {
        if (argv[i] == "-i") {
            ifn = argv[i + 1];
        }
    }

    ofn = ifn + ".h264";

    std::ifstream yuv;
    yuv.open(ifn, std::ios::binary | std::ios::in);
    if (!yuv.is_open()) {
        _log("can not open file %s.", ifn.c_str());
    }

    std::ofstream h264;
    h264.open(ofn, std::ios::binary | std::ios::out);
    if (!h264.is_open()) {
        _log("can not open output file. %s", ofn.c_str());
    }

    auto size = width * height * 3 / 2;
    uint8_t *buf = (uint8_t*)_aligned_malloc(size, 32);
    if (!buf) {
        _log("can not alloc reading buffer.");
    }

    int64_t index = 0;

    while (!yuv.eof()) {
        yuv.read((char*)buf, size);
        if (yuv.gcount() != size) {
            break;
        }

        mfxU64 timeStamp = index * 90000 / fps; // ms to 90kHz

        {
            auto &surface = ctx->surfaces[index % ctx->surfacesCnt];
            auto &bs = ctx->encChain[index % ctx->asyncDepth].mfxBS;
            auto &syncPt = ctx->encChain[index % ctx->asyncDepth].syncp;

            // trans to nv12
            {
                auto len = pitchW*pitchH;
                auto srclen = width * height;

                auto ptr = surface.Data.Y;
                auto src = buf;
                for (auto i = 0; i < height; i++) {
                    memcpy(ptr, src, width);
                    ptr += pitchW;
                    src += width;
                }

                ptr = surface.Data.U;
                src = buf + srclen;
                auto src2 = buf + srclen + srclen / 4;
                for (auto i = 0; i < height / 2; i++) {
                    for (auto j = 0; j < width / 2; j++) {
                        *(ptr + 2 * j) = *(src + j);
                        *(ptr + 2 * j + 1) = *(src2 + j);
                    }
                    src += width / 2;
                    src2 += width / 2;
                    ptr += pitchW;
                }

                surface.Data.TimeStamp = timeStamp;
                surface.Data.MemType = MFX_MEMTYPE_SYSTEM_MEMORY;
            }

            sts = encode(ctx, index, &surface, bs, syncPt, h264);
            if (sts != MFX_ERR_NONE) {
                break;
            }
            index++;
        }
    }

    // flush encoder
    {
        while (sts == MFX_ERR_NONE) {
            auto& bs = ctx->encChain[index % ctx->asyncDepth].mfxBS;
            auto& syncPt = ctx->encChain[index % ctx->asyncDepth].syncp;
            sts = encode(ctx, index, nullptr, bs, syncPt, h264);
            index++;
        }
    }
    yuv.close();
    h264.close();

    sts = ctx->encoder->Close();
    delete ctx->encoder;

    releaseFrame(ctx);
    releaseBitstream(ctx);

    ctx->session->Close();
    delete ctx->session;
    delete ctx;

    _aligned_free(buf);
    buf = nullptr;

    return 0;
}

