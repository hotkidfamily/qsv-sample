// qsv-demo.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include "windows.h"
#include <vector>
#include <fstream>
#include <string>
#include <sstream>

#include <mfxvideo++.h>

#pragma comment(lib, "libmfx_vs2015.lib")

mfxSession _session;
std::vector<mfxExtBuffer *> videoExCfgs;
mfxExtCodingOption codingOpt;
mfxExtCodingOption2 codingOpt2;
mfxExtCodingOption3 codingOpt3;
mfxExtVideoSignalInfo vui;

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

bool getVideoCaps(MFXVideoENCODE* encoder, mfxVideoParam &ip)
{
	encoder->Query(&ip, &ip);

	//printf("session %p", &_session);

	return true;
}
#define MSDK_ALIGN16(value) (((value + 15) >> 4) << 4) // round up to a multiple of 16
#define MSDK_ALIGN32(value) (((value + 31) >> 5) << 5) // round up to a multiple of 32

bool setupVideoParams(mfxVideoParam *vp, int fps, int kbps, int width, int height, int syncDepth)
{
	{
		auto &frame = vp->mfx.FrameInfo;
		frame.Width = MSDK_ALIGN16(width);
		frame.Height = MSDK_ALIGN16(height);

		frame.CropX = 0;
		frame.CropY = 0;
		frame.CropW = width;
		frame.CropH = height;

		frame.FourCC = MFX_FOURCC_YV12;
		frame.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
		frame.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
		frame.FrameRateExtN = fps;
		frame.FrameRateExtD = 1;
	}

	vp->AllocId = 0;
	vp->AsyncDepth = syncDepth;
	vp->IOPattern = MFX_IOPATTERN_IN_SYSTEM_MEMORY;
	
	vp->mfx.CodecId = MFX_CODEC_AVC; 
	vp->mfx.CodecProfile = MFX_PROFILE_AVC_HIGH;
	vp->mfx.TargetUsage = MFX_TARGETUSAGE_BALANCED;

	{
		vp->mfx.GopPicSize = fps * 2;
		vp->mfx.GopRefDist = 2 + 1; // bframes
		vp->mfx.GopOptFlag = MFX_GOP_CLOSED;
		vp->mfx.IdrInterval = fps * 2;

		vp->mfx.RateControlMethod = MFX_RATECONTROL_CBR;

		//vp->mfx.MaxKbps = kbps;
		//vp->mfx.InitialDelayInKB = kbps /8;
		vp->mfx.BufferSizeInKB = kbps/8;

		if (vp->mfx.RateControlMethod == MFX_RATECONTROL_CQP)
		{
			vp->mfx.QPI = vp->mfx.QPP = vp->mfx.QPB = 23;
		}
		else if (vp->mfx.RateControlMethod == MFX_RATECONTROL_ICQ ||
			vp->mfx.RateControlMethod == MFX_RATECONTROL_LA_ICQ)
		{
			//vp->mfx.ICQQuality = pInParams->ICQQuality;
		}
		else if (vp->mfx.RateControlMethod == MFX_RATECONTROL_AVBR)
		{
			//vp->mfx.Accuracy = pInParams->Accuracy;
			vp->mfx.TargetKbps = kbps;
			//vp->mfx.Convergence = pInParams->Convergence;
		}
		else
		{
			vp->mfx.TargetKbps = kbps; // in Kbps
		}

		if (false) // for battery power device using
		{
			vp->mfx.LowPower = MFX_CODINGOPTION_ON;
		}
	}
	vp->mfx.NumSlice = 1;
	vp->mfx.NumRefFrame = 3;
	//vp->mfx.EncodedOrder = 0;

	videoExCfgs.clear();
	
	ZeroMemory(&codingOpt, sizeof(mfxExtCodingOption));
	codingOpt.Header.BufferId = MFX_EXTBUFF_CODING_OPTION;
	codingOpt.Header.BufferSz = sizeof(mfxExtCodingOption);
	codingOpt.CAVLC = MFX_CODINGOPTION_OFF;
	codingOpt.NalHrdConformance = MFX_CODINGOPTION_OFF;
	codingOpt.SingleSeiNalUnit = MFX_CODINGOPTION_ON;
	codingOpt.MaxDecFrameBuffering = 3;
	codingOpt.FramePicture = MFX_CODINGOPTION_OFF; // progressive frame
	codingOpt.PicTimingSEI = MFX_CODINGOPTION_OFF;
	codingOpt.AUDelimiter = MFX_CODINGOPTION_OFF;

	ZeroMemory(&codingOpt2, sizeof(mfxExtCodingOption2));
	codingOpt2.Header.BufferId = MFX_EXTBUFF_CODING_OPTION2;
	codingOpt2.Header.BufferSz = sizeof(mfxExtCodingOption2);
	codingOpt2.RepeatPPS = MFX_CODINGOPTION_OFF;

	codingOpt2.AdaptiveI = MFX_CODINGOPTION_OFF;
	codingOpt2.AdaptiveB = MFX_CODINGOPTION_ON;
	codingOpt2.LookAheadDepth = 10;
	codingOpt2.BRefType = MFX_B_REF_OFF;
	codingOpt2.DisableDeblockingIdc = MFX_CODINGOPTION_OFF; // should check codec caps
	codingOpt2.FixedFrameRate = MFX_CODINGOPTION_OFF;

	ZeroMemory(&codingOpt3, sizeof(mfxExtCodingOption3));
	codingOpt3.Header.BufferId = MFX_EXTBUFF_CODING_OPTION3;
	codingOpt3.Header.BufferSz = sizeof(mfxExtCodingOption3);
	codingOpt3.NumSliceI = codingOpt3.NumSliceP = codingOpt3.NumSliceB = 1;
	codingOpt3.WeightedBiPred = 1;
	codingOpt3.WeightedPred = 1;
	codingOpt3.ScenarioInfo = MFX_SCENARIO_LIVE_STREAMING;
	codingOpt3.ContentInfo = MFX_CONTENT_FULL_SCREEN_VIDEO;
	
	ZeroMemory(&vui, sizeof(mfxExtVideoSignalInfo));
	vui.Header.BufferId = MFX_EXTBUFF_VIDEO_SIGNAL_INFO;
	vui.Header.BufferSz = sizeof(mfxExtVideoSignalInfo);
	vui.ColourPrimaries = 1;
	vui.TransferCharacteristics = 1;
	vui.MatrixCoefficients = 1;
	vui.VideoFormat = 0;
	vui.VideoFullRange = 0;
	vui.ColourDescriptionPresent = 1;

	videoExCfgs.push_back(&codingOpt.Header);
	videoExCfgs.push_back(&codingOpt2.Header);
	videoExCfgs.push_back(&codingOpt3.Header);
	videoExCfgs.push_back(&vui.Header);

	vp->ExtParam = videoExCfgs.data();
	vp->NumExtParam = videoExCfgs.size();

	return true;
}

mfxFrameSurface1 **_pmfxSurfaces = nullptr;
int32_t _NumFrameSurface = 0;

int allocFrame(MFXVideoENCODE* encoder, mfxVideoParam *vp)
{
	if (encoder) {
		mfxFrameAllocRequest req;
		ZeroMemory(&req, sizeof(mfxFrameAllocRequest));
		encoder->QueryIOSurf(vp, &req);

		auto width = req.Info.Width;
		auto height = req.Info.Height;
		auto size = width * height * 3 / 2;
		_NumFrameSurface = req.NumFrameSuggested;

		_pmfxSurfaces = new mfxFrameSurface1 *[req.NumFrameSuggested];

		for (auto i = 0; i < req.NumFrameSuggested; i++) {
			auto &surf = _pmfxSurfaces[i];
			surf = new mfxFrameSurface1;
			ZeroMemory(surf, sizeof(mfxFrameSurface1));
			memcpy(&surf->Info, &vp->mfx.FrameInfo, sizeof(mfxFrameInfo));
			auto &data = surf->Data;
			
 			mfxU8 *pSurface = (mfxU8 *)_aligned_malloc(size, 16);
			data.Y = pSurface;
			data.U = pSurface + width * height;
			data.V = pSurface + width * height + width * height / 4;
		}
	}
	return true;
}


bool releaseFrame()
{
	if (_pmfxSurfaces)
	{
		for (auto i = 0; i < _NumFrameSurface; i++) {
			auto &surf = _pmfxSurfaces[i];
			surf = new mfxFrameSurface1;
			ZeroMemory(surf, sizeof(mfxFrameSurface1));
			auto &pSurface = surf->Data.Y;
			_aligned_free(pSurface);
			delete surf;
		}
		delete[] _pmfxSurfaces;
	}
	return true;
}

typedef struct {
	mfxBitstream mfxBS;
	mfxSyncPoint syncp;
} encOpera;

encOpera * _Operas = nullptr;

mfxStatus allocBitstream(int32_t asyncDepth, int32_t kbps)
{
	_Operas = new encOpera[asyncDepth];
	ZeroMemory(_Operas, sizeof(encOpera) * asyncDepth);

	for (int i = 0; i < asyncDepth; i++) {
		auto &opera = _Operas[i];
		opera.mfxBS.MaxLength = kbps * 1000 / 8;
		opera.mfxBS.Data = (mfxU8*)_aligned_malloc(opera.mfxBS.MaxLength, 32);// new mfxU8[opera.mfxBS.MaxLength];
		ZeroMemory(opera.mfxBS.Data, opera.mfxBS.MaxLength);
		opera.mfxBS.DataOffset = 0;
		opera.mfxBS.DataLength = 0;
	}

	return MFX_ERR_NONE;
}

void releaseBitstream(int32_t asyncDepth)
{
	if (_Operas) {
		for (int i = 0; i < asyncDepth; i++) {
			auto &opera = _Operas[i];
			_aligned_free(opera.mfxBS.Data);
			opera.mfxBS.Data = nullptr;
		}

		delete[] _Operas;
	}
}

int main()
{
	int fps = 60;
	int kbps = 8000;
	int width = 1920;
	int height = 1080;

	int pitchW = MSDK_ALIGN16(width);
	int pitchH = MSDK_ALIGN16(height);

	mfxInitParam initP = {0};
	initP.Implementation = MFX_IMPL_HARDWARE_ANY;
	initP.GPUCopy = MFX_GPUCOPY_OFF;
	mfxVersion ver;
	ver.Minor = 0;
	ver.Major = 1;
	initP.Version = ver;

	mfxStatus sts;
	mfxIMPL impl;
	MFXVideoSession *session = new MFXVideoSession();
	sts = session->InitEx(initP);
	session->QueryIMPL(&impl);
	session->QueryVersion(&ver);

	_log("with impl %x, in %d.%d", impl, ver.Major, ver.Minor);

	{
		switch(MFX_IMPL_BASETYPE(impl)) {
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

	MFXVideoENCODE *_encoder = new MFXVideoENCODE(*session);

	mfxVideoParam vp;
	ZeroMemory(&vp, sizeof(mfxVideoParam));

	int32_t syncDepth = 4;
	setupVideoParams(&vp, fps, kbps, width, height, syncDepth);
	getVideoCaps(_encoder, vp);

	sts = _encoder->Init(&vp);
	if (sts != MFX_ERR_NONE) {
		_log("can not init encoder.");
	}

	allocFrame(_encoder, &vp);
	allocBitstream(syncDepth, kbps);

	std::ifstream yuv;
	std::string ifile("C:\\temps\\lol-single-person-nvenc-1920x1080-60fps.yuv");
	yuv.open(ifile, std::ios::binary | std::ios::in);
	if (!yuv.is_open()) {
		_log("can not open file %s.", ifile.c_str());
	}

	std::ofstream h264;
	std::string ofile("C:\\temps\\lol-single-person-nvenc-1920x1080-60fps-qsv.h264");
	h264.open(ofile, std::ios::binary | std::ios::out);
	if (!h264.is_open()) {
		_log("can not open output file. %s", ofile.c_str());
	}

	auto size = width * height * 3 / 2;
	uint8_t *buf = (uint8_t*)_aligned_malloc(size, 32);
	if (!buf) {
		_log("can not alloc reading buffer.");
	}

	int64_t index = 0;

	while (!yuv.eof()) {
		yuv.read((char*)buf, size);
		mfxU64 timeStamp = index * (1000/fps) * 90000 / 1000; // ms to 90kHz

		while (1) {
			auto &surface = _pmfxSurfaces[index % _NumFrameSurface];
			auto &bs = _Operas[index % syncDepth].mfxBS;
			auto &syncPt = _Operas[index % syncDepth].syncp;

			{
				auto len = pitchW*pitchH;
				auto srclen = width * height;

				auto ptr = surface->Data.Y;
				auto src = buf;
				for (auto i = 0; i < height; i++) {
					memcpy(ptr, src, width);
					ptr += pitchW;
					src += width;
				}

				ptr = surface->Data.U;
				src = buf + srclen;
				for (auto i = 0; i < height / 2; i++) {
					memcpy(ptr, src, width / 2);
					ptr += pitchW / 2;
					src += width / 2;
				}

				ptr = surface->Data.V;
				src = buf + srclen + srclen / 4;
				for (auto i = 0; i < height / 2; i++) {
					memcpy(ptr, src, width / 2);
					ptr += pitchW / 2;
					src += width / 2;
				}

				surface->Data.TimeStamp = timeStamp;
				surface->Data.MemType = MFX_MEMTYPE_SYSTEM_MEMORY;
			}

			sts = _encoder->EncodeFrameAsync(nullptr, surface, &bs, &syncPt);

			if (sts == MFX_ERR_NONE) {
				bs.DataLength = 0;
			}
			if (MFX_ERR_NONE < sts && !syncPt) {
				// Repeat the call if warning and no output
				if (MFX_WRN_DEVICE_BUSY == sts)
					Sleep(1); // Wait if device is busy, then repeat the same call
			}
			else if (MFX_ERR_NONE < sts && syncPt) {
				sts = MFX_ERR_NONE; // Ignore warnings if output is available
				break;
			}
			else if (MFX_ERR_NOT_ENOUGH_BUFFER == sts) {
				// Allocate more bitstream buffer memory here if needed...
				break;
			}
			else if (MFX_ERR_MORE_DATA == sts) {
				break;
			}
			else if (MFX_ERR_NONE != sts) {
				_log("%s", std::to_string(sts).c_str());
				break;
			}
			else {
				session->SyncOperation(syncPt, INFINITE);
				h264.write(reinterpret_cast<const char*>(bs.Data) + bs.DataOffset, bs.DataLength);
				std::string result;
				const char * type = "unknow";
				switch (bs.FrameType & 0xf) {
				case MFX_FRAMETYPE_I:
					type = "I";
					if (bs.FrameType & MFX_FRAMETYPE_IDR == MFX_FRAMETYPE_IDR) {
						type = "IDR";
					}
					break;
				case MFX_FRAMETYPE_P :
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
				_log("%6d - %8d - %4s, pts = %8lld , dts = %8lld", index, bs.DataLength, type, bs.TimeStamp * 1000 / 90000, bs.DecodeTimeStamp * 1000/ 90000);
				bs.DataLength = 0;
				break;
			}
		}
		index++;
	}
	yuv.close();
	h264.close();

	sts = _encoder->Close();
	delete _encoder;

	releaseFrame();
	releaseBitstream(syncDepth);

	session->Close();
	delete session;

	_aligned_free(buf);
	buf = nullptr;

	return 0;
}

