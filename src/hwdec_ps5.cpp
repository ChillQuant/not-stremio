// Hardware video decoding, see hwdec_ps5.h. Ported from Nuvio PS5's
// evo_vdec_native.c (GPL-3.0-or-later, Copyright (C) 2026 Husam Osman); the
// libSceVideodec2 structures and call sequence come from there, which took
// them from blackbearreloaded/ProsperoLight and SvenGDK/SharpProspero.
#include "hwdec_ps5.h"

#include "util.h"

#ifndef PLATFORM_PS5_NATIVE

void HwDecoder::load_module() {}
HwDecoder* HwDecoder::open(const AVCodecParameters*, std::string* why) {
	if (why) *why = "no hardware decoder on this platform";
	return nullptr;
}
HwDecoder::~HwDecoder() {}
int HwDecoder::send(const uint8_t*, int, int64_t) { return -1; }
int HwDecoder::receive(Picture*) { return -1; }
void HwDecoder::flush() {}
const char* HwDecoder::name() const { return ""; }

#else

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
}

#include <algorithm>
#include <cstring>
#include <mutex>

// ---------------------------------------------------------------------------
// libSceVideodec2 (imports through native/stubs/videodec2.c)

extern "C" {
struct SceVideodec2DecoderConfigInfo {
	uint64_t size;
	uint32_t resource_type;
	uint32_t codec_type;
	uint32_t profile;
	uint32_t max_level;
	int32_t max_width;
	int32_t max_height;
	int32_t max_dpb_frames;
	uint32_t pipeline_depth;
	uint64_t compute_queue;
	uint64_t cpu_affinity;
	int32_t cpu_priority;
	uint32_t optimize_progressive;
	uint32_t check_memory_type;
	uint32_t reserved;
};
struct SceVideodec2DecoderMemoryInfo {
	uint64_t size;
	uint64_t cpu_size;
	void* cpu;
	uint64_t gpu_size;
	void* gpu;
	uint64_t cpu_gpu_size;
	void* cpu_gpu;
	uint64_t max_frame_size;
	uint32_t frame_alignment;
	uint32_t reserved;
};
struct SceVideodec2ComputeConfigInfo {
	uint64_t size;
	uint16_t pipe_id;
	uint16_t queue_id;
	uint8_t check_memory_type;
	uint8_t reserved0;
	uint16_t reserved1;
};
struct SceVideodec2ComputeMemoryInfo {
	uint64_t size;
	uint64_t cpu_gpu_size;
	void* cpu_gpu;
};
struct SceVideodec2InputData {
	uint64_t size;
	void* au;
	uint64_t au_size;
	uint64_t pts;
	uint64_t dts;
	uint64_t attached;
};
struct SceVideodec2FrameBuffer {
	uint64_t size;
	void* buffer;
	uint64_t buffer_size;
	uint32_t accepted;
	uint32_t reserved;
};
struct SceVideodec2OutputInfo {
	uint64_t size;
	uint8_t valid;
	uint8_t error;
	uint8_t picture_count;
	uint8_t padding;
	uint32_t codec;
	uint32_t width;
	uint32_t pitch;
	uint32_t height;
	uint32_t reserved;
	void* buffer;
	uint64_t buffer_size;
	uint32_t frame_format;
	uint32_t pitch_bytes;
};

int32_t sceVideodec2QueryComputeMemoryInfo(SceVideodec2ComputeMemoryInfo* memory);
int32_t sceVideodec2AllocateComputeQueue(const SceVideodec2ComputeConfigInfo* config,
                                         const SceVideodec2ComputeMemoryInfo* memory, void** compute_queue);
int32_t sceVideodec2ReleaseComputeQueue(void* compute_queue);
int32_t sceVideodec2QueryDecoderMemoryInfo(const SceVideodec2DecoderConfigInfo* config,
                                           SceVideodec2DecoderMemoryInfo* memory);
int32_t sceVideodec2CreateDecoder(const SceVideodec2DecoderConfigInfo* config,
                                  const SceVideodec2DecoderMemoryInfo* memory, void** decoder);
int32_t sceVideodec2DeleteDecoder(void* decoder);
int32_t sceVideodec2Reset(void* decoder);
int32_t sceVideodec2Decode(void* decoder, SceVideodec2InputData* input, SceVideodec2FrameBuffer* frame,
                           SceVideodec2OutputInfo* output);
int32_t sceVideodec2Flush(void* decoder, SceVideodec2FrameBuffer* frame, SceVideodec2OutputInfo* output);

int sceSysmoduleLoadModule(uint16_t id);
int64_t sceKernelGetDirectMemorySize(void);
int sceKernelAllocateDirectMemory(int64_t, int64_t, size_t, size_t, int, int64_t*);
int sceKernelMapDirectMemory(void**, size_t, int, int, int64_t, size_t);
int sceKernelReleaseDirectMemory(int64_t, size_t);
int sceKernelAvailableDirectMemorySize(int64_t, int64_t, size_t, int64_t*, size_t*);
int sceKernelMunmap(void*, size_t);
int sceKernelMapNamedFlexibleMemory(void**, size_t, int, int, const char*);
int sceKernelReleaseFlexibleMemory(void*, size_t);
int sceKernelAvailableFlexibleMemorySize(size_t*);
}

namespace {

const uint16_t kSysmoduleVideodec2 = 207;
const uint32_t kCodecAvc = 1, kCodecHevc = 974921, kCodecVp9 = 2382845;
const uint32_t kResourceCompute = 1;
const size_t kInputSlotBytes = 8u << 20;  // ~50x the largest 4K keyframe seen
const unsigned kInputSlots = 4;
const unsigned kFrameSlots = 24;
const int kReorderDepth = 8;
const int kReorderSlots = 14;  // MUST be >= kReorderDepth + 4 so send() never starves receive()
const int kPtsPool = kReorderSlots + int(kInputSlots) + 16;
const int kPostFlushErrors = 32;  // refusals tolerated before the first picture after a seek
const int kMaxW = 3840, kMaxH = 2176;

bool g_module_loaded = false;

enum Kind { H264, HEVC, VP9, HEVC10, KIND_COUNT };

struct CodecDesc {
	Kind kind;
	uint32_t codec_type, profile;
	int level_1080, level_4k;
	const char* bsf;
	bool vp9;
	const char* tag;
	unsigned depth;
};

// max_level: AVC level x10, HEVC general_level_idc (x30), VP9 x10.
const CodecDesc kCodecs[KIND_COUNT] = {
    {H264, kCodecAvc, 100, 51, 52, "h264_mp4toannexb", false, "H.264", 4},
    {HEVC, kCodecHevc, 1, 153, 156, "hevc_mp4toannexb", false, "HEVC", 4},
    {VP9, kCodecVp9, 0, 41, 51, "vp9_superframe_split", true, "VP9", 4},
    {HEVC10, kCodecHevc, 2, 153, 156, "hevc_mp4toannexb", false, "HEVC 10-bit", 4},
};

size_t align16k(size_t v) { return (v + 0x3fffu) & ~size_t(0x3fff); }

int alloc_direct(size_t size, int prot, int64_t* start, void** addr) {
	int rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size, 0x4000, 12, start);
	if (rc == 0) rc = sceKernelMapDirectMemory(addr, size, prot, 0, *start, 0x4000);
	return rc;
}
void free_direct(void* addr, int64_t start, size_t size) {
	if (addr) sceKernelMunmap(addr, size);
	if (start >= 0) sceKernelReleaseDirectMemory(start, size);
}

// One decoder per codec, kept for the next video of the same kind (creating
// one maps a few hundred MB; doing that per video would fragment memory).
struct Slot {
	bool ready = false, owned = false;
	void* compute_queue = nullptr;
	void* compute_mem = nullptr;
	int64_t compute_start = -1;
	size_t compute_size = 0;
	void* decoder = nullptr;
	void* cpu_mem = nullptr;
	size_t cpu_map = 0;
	bool cpu_direct = false;  // workspace in direct memory (flexible memory was short)
	int64_t cpu_start = -1;
	void* gpu_mem = nullptr;
	int64_t gpu_start = -1;
	size_t gpu_size = 0;
	void* cpu_gpu_mem = nullptr;
	int64_t cpu_gpu_start = -1;
	size_t cpu_gpu_size = 0;
	void* input_mem = nullptr;
	int64_t input_start = -1;
	size_t input_pool = 0;
	void* frame_mem = nullptr;
	int64_t frame_start = -1;
	size_t frame_pool = 0, frame_size = 0;
	int max_w = 0, max_h = 0;
	SceVideodec2DecoderConfigInfo cfg{};
	SceVideodec2DecoderMemoryInfo mem{};
};

std::mutex g_slots_m;
Slot g_slots[KIND_COUNT];
// Parked decoder instances for back-to-back reuse (episode transitions,
// seek re-opens): one per codec kind, filled by ~HwDecoder on clean close
// and consumed by open() when the new stream's parameters match
// byte-for-byte. Guarded by g_slots_m. (The helpers live after the Impl
// definition, just before HwDecoder::open.)
HwDecoder::Impl* g_parked[KIND_COUNT] = {};

void slot_teardown(Slot& s) {
	if (s.decoder) sceVideodec2DeleteDecoder(s.decoder);
	free_direct(s.frame_mem, s.frame_start, s.frame_pool);
	free_direct(s.input_mem, s.input_start, s.input_pool);
	free_direct(s.cpu_gpu_mem, s.cpu_gpu_start, s.cpu_gpu_size);
	free_direct(s.gpu_mem, s.gpu_start, s.gpu_size);
	if (s.cpu_mem) {
		if (s.cpu_direct) {
			free_direct(s.cpu_mem, s.cpu_start, s.cpu_map);
		} else {
			sceKernelReleaseFlexibleMemory(s.cpu_mem, s.cpu_map);
			sceKernelMunmap(s.cpu_mem, s.cpu_map);
		}
	}
	if (s.compute_queue) sceVideodec2ReleaseComputeQueue(s.compute_queue);
	free_direct(s.compute_mem, s.compute_start, s.compute_size);
	s = Slot();
}

// Sony calls can fault instead of failing: each step is logged before it
// runs, so the last line in the log names the one that did.
#define STAGE(name)                                                    \
	do {                                                               \
		stage = (name);                                                \
		dlog("hwdec: %s %dx%d [%s]", d.tag, w, h, stage);              \
	} while (0)

int slot_bringup(Slot& s, const CodecDesc& d, int w, int h) {
	const char* stage = "?";
	int rc;
	SceVideodec2ComputeMemoryInfo cm{};
	SceVideodec2ComputeConfigInfo cc{};
	cm.size = sizeof cm;
	STAGE("QueryComputeMemoryInfo");
	if ((rc = sceVideodec2QueryComputeMemoryInfo(&cm)) != 0) goto fail;
	s.compute_size = align16k(size_t(cm.cpu_gpu_size));
	STAGE("alloc(compute)");
	if ((rc = alloc_direct(s.compute_size, 0x33, &s.compute_start, &s.compute_mem)) != 0) goto fail;
	cm.cpu_gpu = s.compute_mem;
	cm.cpu_gpu_size = s.compute_size;
	cc.size = sizeof cc;
	STAGE("AllocateComputeQueue");
	if ((rc = sceVideodec2AllocateComputeQueue(&cc, &cm, &s.compute_queue)) != 0) goto fail;
	if (!s.compute_queue) {
		rc = -1;
		goto fail;
	}
	{
		SceVideodec2DecoderConfigInfo config{};
		SceVideodec2DecoderMemoryInfo mem{};
		config.size = sizeof config;
		config.resource_type = kResourceCompute;
		config.codec_type = d.codec_type;
		config.profile = d.profile;
		config.max_level = uint32_t((w > 1920 || h > 1088) ? d.level_4k : d.level_1080);
		config.max_width = w;
		config.max_height = h;
		config.max_dpb_frames = -1;  // the decoder sizes it
		config.pipeline_depth = d.depth;
		config.compute_queue = uint64_t(s.compute_queue);
		config.cpu_affinity = 0x3f;
		config.cpu_priority = 700;
		config.optimize_progressive = 1;
		mem.size = sizeof mem;
		STAGE("QueryDecoderMemoryInfo");
		if ((rc = sceVideodec2QueryDecoderMemoryInfo(&config, &mem)) != 0) goto fail;
		{
			int64_t st = 0;
			size_t avail = 0;
			sceKernelAvailableDirectMemorySize(0, sceKernelGetDirectMemorySize(), 0x4000, &st, &avail);
			dlog("hwdec: needs workspace %llu KB, gpu %llu KB, cpu_gpu %llu KB, frames %u x %llu KB, input %zu KB; "
			     "%zu MB direct memory free",
			     (unsigned long long)(mem.cpu_size >> 10), (unsigned long long)(mem.gpu_size >> 10),
			     (unsigned long long)(mem.cpu_gpu_size >> 10), kFrameSlots,
			     (unsigned long long)(mem.max_frame_size >> 10), (kInputSlotBytes * kInputSlots) >> 10, avail >> 20);
		}

		// The decoder's CPU workspace: flexible memory, as the decoder
		// expects. This app has little of it, so if that fails, try
		// direct memory mapped for the CPU.
		s.cpu_map = align16k(size_t(mem.cpu_size));
		size_t flex = 0;
		sceKernelAvailableFlexibleMemorySize(&flex);
		STAGE("MapNamedFlexibleMemory");
		rc = sceKernelMapNamedFlexibleMemory(&mem.cpu, s.cpu_map, 0x03, 0, "StremioVdec");
		if (rc != 0 || !mem.cpu) {
			dlog("hwdec: workspace of %zu KB not in flexible memory (rc 0x%08x, %zu KB free); using direct memory",
			     s.cpu_map >> 10, unsigned(rc), flex >> 10);
			mem.cpu = nullptr;
			STAGE("alloc(workspace)");
			if ((rc = alloc_direct(s.cpu_map, 0x33, &s.cpu_start, &mem.cpu)) != 0) goto fail;
			s.cpu_direct = true;
		}
		s.cpu_mem = mem.cpu;

		s.gpu_size = align16k(size_t(mem.gpu_size));
		s.cpu_gpu_size = align16k(size_t(mem.cpu_gpu_size));
		s.frame_size = align16k(size_t(mem.max_frame_size));
		s.input_pool = kInputSlotBytes * kInputSlots;
		s.frame_pool = s.frame_size * kFrameSlots;
		if (!s.frame_size) {
			rc = -1;
			goto fail;
		}
		mem.gpu_size = s.gpu_size;
		if (s.cpu_gpu_size) mem.cpu_gpu_size = s.cpu_gpu_size;
		STAGE("alloc(gpu)");
		if ((rc = alloc_direct(s.gpu_size, 0x33, &s.gpu_start, &s.gpu_mem)) != 0) goto fail;
		mem.gpu = s.gpu_mem;
		if (s.cpu_gpu_size) {
			STAGE("alloc(cpu_gpu)");
			if ((rc = alloc_direct(s.cpu_gpu_size, 0x33, &s.cpu_gpu_start, &s.cpu_gpu_mem)) != 0) goto fail;
			mem.cpu_gpu = s.cpu_gpu_mem;
		}
		STAGE("alloc(input)");
		if ((rc = alloc_direct(s.input_pool, 0x33, &s.input_start, &s.input_mem)) != 0) goto fail;
		STAGE("alloc(frames)");
		if ((rc = alloc_direct(s.frame_pool, 0x33, &s.frame_start, &s.frame_mem)) != 0) goto fail;
		STAGE("CreateDecoder");
		if ((rc = sceVideodec2CreateDecoder(&config, &mem, &s.decoder)) != 0 || !s.decoder) {
			if (!rc) rc = -1;
			goto fail;
		}
		s.cfg = config;
		s.mem = mem;
		STAGE("Reset");
		if ((rc = sceVideodec2Reset(s.decoder)) != 0) goto fail;
		s.max_w = w;
		s.max_h = h;
		s.ready = true;
		dlog("hwdec: %s decoder ready for %dx%d (%zu MB)", d.tag, w, h,
		     (s.compute_size + s.gpu_size + s.cpu_gpu_size + s.input_pool + s.frame_pool + s.cpu_map) >> 20);
		return 0;
	}
fail:
	dlog("hwdec: %s %dx%d failed at [%s]: rc 0x%08x", d.tag, w, h, stage, unsigned(rc));
	slot_teardown(s);
	return rc ? rc : -1;
}
#undef STAGE

// A new decoder object on the slot's memory: what a seek needs. Reset alone
// left some 4K streams unable to restart (Nuvio PS5, hardware 2026-10-01).
int slot_renew(Slot& s) {
	if (s.decoder) sceVideodec2DeleteDecoder(s.decoder);
	s.decoder = nullptr;
	SceVideodec2DecoderMemoryInfo mem = s.mem;
	int rc = sceVideodec2CreateDecoder(&s.cfg, &mem, &s.decoder);
	if (rc != 0 || !s.decoder) {
		s.decoder = nullptr;
		return rc ? rc : -1;
	}
	return sceVideodec2Reset(s.decoder);
}

// ---------------------------------------------------------------------------
// HEVC access-unit handling

const int kNalTrailHi = 5, kNalRaslN = 8, kNalRaslR = 9, kNalIrapHi = 23;

// First VCL NAL type in an Annex-B access unit, or -1.
int hevc_au_nal_type(const uint8_t* au, int size) {
	for (int i = 0; i + 4 < size; i++) {
		if (au[i] != 0 || au[i + 1] != 0) continue;
		int payload;
		if (au[i + 2] == 1) payload = i + 3;
		else if (au[i + 2] == 0 && au[i + 3] == 1) payload = i + 4;
		else continue;
		if (payload >= size) break;
		int type = (au[payload] >> 1) & 0x3f;
		if (type <= kNalIrapHi) return type;
		i = payload;
	}
	return -1;
}

struct Bits {
	const uint8_t* p;
	int n, byte, bit, zeros;
	int get() {
		if (byte >= n) return 0;
		if (bit == 0 && zeros >= 2 && p[byte] == 3) {  // emulation prevention
			byte++;
			zeros = 0;
			if (byte >= n) return 0;
		}
		int v = (p[byte] >> (7 - bit)) & 1;
		if (++bit == 8) {
			zeros = p[byte] == 0 ? zeros + 1 : 0;
			bit = 0;
			byte++;
		}
		return v;
	}
	unsigned u(int k) {
		unsigned v = 0;
		while (k-- > 0) v = (v << 1) | unsigned(get());
		return v;
	}
	unsigned ue() {
		int lz = 0;
		while (lz < 31 && !get()) lz++;
		return ((1u << lz) - 1) + u(lz);
	}
};

// The ID a VPS (32), SPS (33) or PPS (34) defines.
unsigned hevc_ps_id(int type, const uint8_t* payload, int len) {
	Bits b{payload, len, 0, 0, 0};
	if (type == 32) return b.u(4);
	if (type == 34) return b.ue();
	b.u(4);
	unsigned subs = b.u(3);
	b.u(1);
	b.u(32), b.u(32), b.u(32);
	unsigned prof[8] = {0}, lvl[8] = {0};
	for (unsigned i = 0; i < subs && i < 8; i++) prof[i] = b.u(1), lvl[i] = b.u(1);
	if (subs > 0)
		for (unsigned i = subs; i < 8; i++) b.u(2);
	for (unsigned i = 0; i < subs && i < 8; i++) {
		if (prof[i]) b.u(32), b.u(32), b.u(24);
		if (lvl[i]) b.u(8);
	}
	return b.ue();
}

// Copies an HEVC access unit without what the decoder can't take: NAL types
// 48-63 (Dolby Vision RPU / enhancement layer; the base layer plays) and a
// parameter set that a later one in the same AU replaces (UHD Blu-ray remuxes
// repeat theirs in-band; two different PPS 0 in one AU make the decoder
// refuse the keyframe).
int hevc_copy_base_layer(uint8_t* dst, const uint8_t* au, int size) {
	const int kMaxNals = 512;
	static thread_local int sc_at[kMaxNals], nal_at[kMaxNals], end_at[kMaxNals];
	static thread_local uint32_t key[kMaxNals];
	int count = 0, sc = -1, nal = -1;
	for (int i = 0; i + 3 <= size; i++)
		if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1) {
			sc = (i > 0 && au[i - 1] == 0) ? i - 1 : i;
			nal = i + 3;
			break;
		}
	if (nal < 0) {
		memcpy(dst, au, size_t(size));
		return size;
	}
	while (nal < size && count < kMaxNals) {
		int next_sc = size, next_nal = -1;
		for (int j = nal; j + 3 <= size; j++)
			if (au[j] == 0 && au[j + 1] == 0 && au[j + 2] == 1) {
				next_sc = (j > nal && au[j - 1] == 0) ? j - 1 : j;
				next_nal = j + 3;
				break;
			}
		int type = (au[nal] >> 1) & 0x3f;
		sc_at[count] = sc;
		nal_at[count] = nal;
		end_at[count] = next_sc;
		key[count] = 0;
		if (type >= 32 && type <= 34 && nal + 2 < next_sc) {
			unsigned layer = ((au[nal] & 1u) << 5) | (au[nal + 1] >> 3);
			unsigned id = hevc_ps_id(type, au + nal + 2, next_sc - nal - 2);
			key[count] = 0x80000000u | (uint32_t(type) << 24) | (layer << 16) | (id & 0xffffu);
		}
		count++;
		if (next_nal < 0) break;
		sc = next_sc;
		nal = next_nal;
	}
	if (count == kMaxNals && nal < size) {
		memcpy(dst, au, size_t(size));
		return size;
	}
	int o = 0;
	for (int k = 0; k < count; k++) {
		int type = (au[nal_at[k]] >> 1) & 0x3f;
		if (type >= 48) continue;
		if (key[k]) {
			bool replaced = false;
			for (int m = k + 1; m < count && !replaced; m++) replaced = key[m] == key[k];
			if (replaced) continue;
		}
		memcpy(dst + o, au + sc_at[k], size_t(end_at[k] - sc_at[k]));
		o += end_at[k] - sc_at[k];
	}
	return o;
}

// tiles_enabled_flag of an HEVC PPS: the hardware decoder doesn't do tiles.
bool hevc_pps_tiles(const uint8_t* payload, int len) {
	Bits b{payload, len, 0, 0, 0};
	b.ue(), b.ue();
	b.u(1), b.u(1);
	b.u(3);
	b.u(1), b.u(1);
	b.ue(), b.ue();
	b.ue();
	b.u(1), b.u(1);
	if (b.u(1)) b.ue();
	b.ue(), b.ue();
	b.u(1), b.u(1), b.u(1);
	b.u(1);
	return b.u(1) != 0;
}

bool hevc_extradata_tiles(const uint8_t* x, int n) {
	if (!x || n < 4) return false;
	if (x[0] == 0 && x[1] == 0 && (x[2] == 1 || (x[2] == 0 && x[3] == 1))) {
		for (int i = 0; i + 4 < n; i++) {
			if (x[i] || x[i + 1] || x[i + 2] != 1) continue;
			int nal = i + 3;
			if (((x[nal] >> 1) & 0x3f) == 34 && hevc_pps_tiles(x + nal + 2, n - nal - 2)) return true;
		}
		return false;
	}
	if (n < 23) return false;
	int pos = 23, arrays = x[22];
	for (int a = 0; a < arrays && pos + 3 <= n; a++) {
		int type = x[pos] & 0x3f;
		int count = (x[pos + 1] << 8) | x[pos + 2];
		pos += 3;
		for (int k = 0; k < count && pos + 2 <= n; k++) {
			int len = (x[pos] << 8) | x[pos + 1];
			pos += 2;
			if (pos + len > n) return false;
			if (type == 34 && len > 2 && hevc_pps_tiles(x + pos + 2, len - 2)) return true;
			pos += len;
		}
	}
	return false;
}

// H.264 SPS (RBSP, NAL header already skipped): num_reorder_frames from the
// VUI bitstream-restriction info. -1 when absent or unparseable.
int h264_sps_num_reorder_frames(const uint8_t* rbsp, int len) {
	Bits b{rbsp, len, 0, 0, 0};
	unsigned profile = b.u(8);
	b.u(8);  // constraint flags
	b.u(8);  // level_idc
	b.ue();  // seq_parameter_set_id
	if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 ||
	    profile == 83 || profile == 86 || profile == 118 || profile == 128 || profile == 138 ||
	    profile == 139 || profile == 134 || profile == 135) {
		if (b.ue() == 3) b.u(1);  // chroma_format_idc, separate_colour_plane_flag
		b.ue();                   // bit_depth_luma_minus8
		b.ue();                   // bit_depth_chroma_minus8
		b.u(1);                   // qpprime_y_zero_transform_bypass_flag
		if (b.u(1)) return -1;    // scaling lists: not worth the skip logic
	}
	b.ue();  // log2_max_frame_num_minus4
	unsigned poc = b.ue();
	if (poc == 0) b.ue();
	else if (poc == 1) {
		b.u(1);
		b.ue();
		b.ue();  // se() values skipped as ue(): identical bit length
		unsigned n = b.ue();
		if (n > 32) return -1;
		for (unsigned i = 0; i < n; i++) b.ue();
	}
	b.ue();  // max_num_ref_frames
	b.u(1);  // gaps_in_frame_num_value_allowed_flag
	b.ue();
	b.ue();  // pic_width/height_in_mbs_minus1
	if (!b.u(1)) b.u(1);  // frame_mbs_only_flag / mb_adaptive_frame_field_flag
	b.u(1);              // direct_8x8_inference_flag
	if (b.u(1)) {
		b.ue();
		b.ue();
		b.ue();
		b.ue();  // frame crop offsets
	}
	if (!b.u(1)) return -1;  // no VUI: num_reorder_frames lives there
	if (b.u(1)) {            // aspect_ratio_info_present_flag
		if (b.u(8) == 255) {
			b.u(16);
			b.u(16);
		}
	}
	if (b.u(1)) b.u(1);  // overscan_info_present_flag
	if (b.u(1)) {        // video_signal_type_present_flag
		b.u(3);
		b.u(1);
		b.u(1);
		if (b.u(1)) {
			b.u(8);
			b.u(8);
			b.u(8);  // colour description
		}
	}
	if (b.u(1)) {
		b.ue();
		b.ue();  // chroma_sample_loc_type_top/bottom_field
	}
	if (b.u(1)) {
		b.u(32);
		b.u(32);
		b.u(1);  // timing info
	}
	bool nal_hrd = b.u(1), vcl_hrd = b.u(1);
	if (nal_hrd || vcl_hrd) {
		// Skip hrd_parameters() (once or twice): only the bit positions matter.
		for (int h = 0, hn = (nal_hrd && vcl_hrd) ? 2 : 1; h < hn; h++) {
			unsigned cpb = b.ue();
			if (cpb > 31) return -1;
			b.u(4);
			b.u(4);
			for (unsigned i = 0; i <= cpb; i++) {
				b.ue();
				b.ue();
				b.u(1);
			}
			b.u(5);
			b.u(5);
			b.u(5);
			b.u(5);
		}
		b.u(1);  // low_delay_hrd_flag
	}
	b.u(1);  // pic_struct_present_flag
	if (!b.u(1)) return -1;  // bitstream_restriction_flag
	b.u(1);                 // motion_vectors_over_pic_boundaries_flag
	b.ue();
	b.ue();
	b.ue();
	b.ue();
	unsigned reorder = b.ue();  // num_reorder_frames
	if (b.byte > b.n) return -1;
	return int(reorder);
}

// HEVC SPS (RBSP, 2-byte NAL header already skipped): the largest
// sps_max_num_reorder_pics over the sub-layers. -1 when absent/unparseable.
int hevc_sps_max_num_reorder_pics(const uint8_t* rbsp, int len) {
	Bits b{rbsp, len, 0, 0, 0};
	b.u(4);  // sps_video_parameter_set_id
	unsigned max_sub = b.u(3);
	if (max_sub > 6) return -1;
	b.u(1);  // sps_temporal_id_nesting_flag
	// profile_tier_level(profilePresentFlag=1, maxNumSubLayersMinus1):
	b.u(2);
	b.u(1);
	b.u(5);  // general_profile_space/tier/idc
	b.u(32);  // general_profile_compatibility_flag[32]
	b.u(1);
	b.u(1);
	b.u(1);
	b.u(1);  // progressive/interlaced/non_packed/frame_only
	b.u(44);  // general_reserved_zero_44bits
	b.u(8);   // general_level_idc
	bool sub_prof[8] = {false}, sub_lvl[8] = {false};
	for (unsigned i = 0; i < max_sub; i++) {
		sub_prof[i] = b.u(1) != 0;
		sub_lvl[i] = b.u(1) != 0;
	}
	if (max_sub > 0)
		for (unsigned i = max_sub; i < 8; i++) b.u(2);  // reserved_zero_2bits
	for (unsigned i = 0; i < max_sub; i++) {
		if (sub_prof[i]) {
			b.u(2);
			b.u(1);
			b.u(5);
			b.u(32);
			b.u(1);
			b.u(1);
			b.u(1);
			b.u(1);
			b.u(44);  // sub-layer profile fields (no level_idc here)
		}
		if (sub_lvl[i]) b.u(8);  // sub_layer_level_idc[i]
	}
	b.ue();  // sps_seq_parameter_set_id
	if (b.ue() == 3) b.u(1);  // chroma_format_idc == 3: separate_colour_plane_flag
	b.ue();
	b.ue();  // pic_width/height_in_luma_samples
	if (b.u(1)) {
		b.ue();
		b.ue();
		b.ue();
		b.ue();  // conformance window offsets
	}
	b.ue();
	b.ue();  // bit_depth_luma/chroma_minus8
	b.ue();  // log2_max_pic_order_cnt_lsb_minus4
	bool ordering_info = b.u(1) != 0;  // sps_sub_layer_ordering_info_present_flag
	unsigned start = ordering_info ? 0 : max_sub;
	int reorder = -1;
	for (unsigned i = start; i <= max_sub; i++) {
		b.ue();  // sps_max_dec_pic_buffering_minus1[i]
		int r = int(b.ue());  // sps_max_num_reorder_pics[i]
		if (r > reorder) reorder = r;
		b.ue();  // sps_max_latency_increase_plus1[i]
	}
	if (b.byte > b.n) return -1;
	return reorder;
}

// First SPS NAL out of an avcC (ISO BMFF) H.264 extradata block.
bool avcc_first_sps(const uint8_t* x, int n, const uint8_t*& rbsp, int& len) {
	if (!x || n < 7 || x[0] != 1) return false;
	int num = x[5] & 0x1f, pos = 6;
	for (int i = 0; i < num && pos + 2 <= n; i++) {
		int l = (x[pos] << 8) | x[pos + 1];
		pos += 2;
		if (l < 2 || pos + l > n) return false;
		if ((x[pos] & 0x1f) == 7) {  // SPS NAL type
			rbsp = x + pos + 1;  // skip the 1-byte NAL header
			len = l - 1;
			return true;
		}
		pos += l;
	}
	return false;
}

// First SPS NAL out of an hvcC (ISO BMFF) HEVC extradata block.
bool hvcc_first_sps(const uint8_t* x, int n, const uint8_t*& rbsp, int& len) {
	if (!x || n < 23 || x[0] != 1) return false;
	int pos = 23, arrays = x[22];
	for (int a = 0; a < arrays && pos + 3 <= n; a++) {
		int type = x[pos] & 0x3f, count = (x[pos + 1] << 8) | x[pos + 2];
		pos += 3;
		for (int k = 0; k < count && pos + 2 <= n; k++) {
			int l = (x[pos] << 8) | x[pos + 1];
			pos += 2;
			if (l < 3 || pos + l > n) return false;
			if (type == 33) {  // SPS NAL type
				rbsp = x + pos + 2;  // skip the 2-byte HEVC NAL header
				len = l - 2;
				return true;
			}
			pos += l;
		}
	}
	return false;
}

// Best-effort reorder depth from the stream's own SPS. -1 when the value
// can't be determined (no/odd extradata, VP9, parse miss); the caller keeps
// the default. Never returns more than kReorderDepth: the reorder slots and
// the PTS pool are sized for that compile-time maximum.
int sps_reorder_depth(const AVCodecParameters* par) {
	if (!par || !par->extradata || par->extradata_size <= 0) return -1;
	const uint8_t* x = par->extradata;
	int n = par->extradata_size, rd = -1;
	const uint8_t* rbsp = nullptr;
	int len = 0;
	if (par->codec_id == AV_CODEC_ID_H264 && avcc_first_sps(x, n, rbsp, len))
		rd = h264_sps_num_reorder_frames(rbsp, len);
	else if (par->codec_id == AV_CODEC_ID_HEVC && hvcc_first_sps(x, n, rbsp, len))
		rd = hevc_sps_max_num_reorder_pics(rbsp, len);
	if (rd < 0) return -1;
	return rd < kReorderDepth ? rd : kReorderDepth;
}

// VP9: is this coded frame shown? Hidden alt-ref frames are decoded for
// reference but never presented.
bool vp9_frame_is_shown(const uint8_t* b, int size) {
	int p = 0;
	auto bit = [&]() { int i = p++; return (i >> 3) < size ? (b[i >> 3] >> (7 - (i & 7))) & 1 : 0; };
	if (bit() != 1 || bit() != 0) return true;
	int prof = bit();
	prof |= bit() << 1;
	if (prof == 3) bit();
	if (bit()) return true;  // show_existing_frame
	bit();                   // frame_type
	return bit() != 0;       // show_frame
}

}  // namespace

// ---------------------------------------------------------------------------

struct HwDecoder::Impl {
	const CodecDesc* desc = nullptr;
	Slot* slot = nullptr;
	int disp_w = 0, disp_h = 0;
	unsigned au_ring = 0, calls = 0, fails = 0, frames = 0;
	bool flushing = false, fatal = false;
	bool drop_leading = true, need_irap = false;
	int post_flush_errs = 0, since_flush_out = 0;
	bool logged_err = false, logged_strip = false;

	AVBSFContext* bsf = nullptr;
	AVCodecParameters* bsf_par = nullptr;
	AVCodecParameters* par_copy = nullptr;  // the stream's parameters, for parked-reuse matching
	std::string extradata;  // Annex-B parameter sets, sent first when there's no bsf
	AVPacket* in_pkt = nullptr;
	AVPacket* out_pkt = nullptr;

	int64_t pts_pool[kPtsPool];
	int pts_n = 0;

	struct Out {
		bool used = false;
		uint64_t seq = 0;
		Picture pic;
	} ro[kReorderSlots];
	int ro_count = 0;
	uint64_t next_seq = 0;
	// Reorder depth actually used for picture hold-back and send backpressure.
	// Read from the stream's SPS when available, otherwise kReorderDepth.
	// Never larger than kReorderDepth: ro[] and pts_pool are sized for that.
	int reorder_depth = kReorderDepth;

	void pts_push(int64_t pts) {
		if (pts_n >= kPtsPool) {
			memmove(pts_pool, pts_pool + 1, (kPtsPool - 1) * sizeof(int64_t));
			pts_n = kPtsPool - 1;
		}
		pts_pool[pts_n++] = pts;
	}
	// Pictures come out in display order without a timestamp: pair each
	// with the smallest pending one.
	//
	// Assumption: every access unit is pushed (pts_push, decode order) before
	// any picture depending on it can emerge from the decoder, and pictures
	// emerge in display order. Then the smallest pending PTS is always the
	// right one for the next emitted picture, as long as the pool never
	// overflows (kPtsPool covers reorder_depth + input slots in flight).
	// Streams whose timestamps aren't monotonic in decode order can still
	// mis-pair; this decoder gives us nothing better to pair with.
	int64_t pts_take() {
		if (pts_n <= 0) return INT64_MIN;
		int mi = 0;
		for (int i = 1; i < pts_n; i++) {
			if (pts_pool[i] == INT64_MIN) continue;
			if (pts_pool[mi] == INT64_MIN || pts_pool[i] < pts_pool[mi]) mi = i;
		}
		int64_t v = pts_pool[mi];
		pts_pool[mi] = pts_pool[--pts_n];
		return v;
	}

	// Where the chroma plane starts, in luma rows: right after the height the
	// decoder reports for the picture, as Nuvio PS5 does (ro_harvest: planes[1]
	// = data + pitch x out->height, used as is by pp_playback.c).
	uint32_t chroma_rows = 0;
	uint32_t find_chroma(const SceVideodec2OutputInfo& out, uint32_t pitch, bool ten) {
		(void)ten;
		dlog("hwdec: picture %ux%u pitch %u, buffer %llu bytes; chroma at row %u", out.width, out.height, pitch,
		     (unsigned long long)out.buffer_size, out.height);
		return out.height;
	}
	void harvest(const SceVideodec2OutputInfo& out) {
		Out* s = nullptr;
		for (auto& r : ro)
			if (!r.used) {
				s = &r;
				break;
			}
		if (!s) {
			dlog("hwdec: BUG: ro[] overflow (ro_count=%d)!", ro_count);
			return;
		}
		if (!out.buffer) return;
		bool ten = out.pitch_bytes ? out.pitch_bytes >= out.pitch * 2u : desc->kind == HEVC10;
		uint32_t pitch = out.pitch_bytes ? out.pitch_bytes : out.pitch;
		if (ten && pitch < out.pitch * 2u) pitch = out.pitch * 2u;
		// The frame stays in the decoder's pool until it reuses the slot,
		// several pictures later: hand out pointers into it, no copy.
		s->pic.y = static_cast<const uint8_t*>(out.buffer);
		if (chroma_rows == 0) chroma_rows = find_chroma(out, pitch, ten);
		s->pic.uv = s->pic.y + size_t(pitch) * chroma_rows;  // after the padded luma rows
		s->pic.pitch = int(pitch);
		s->pic.width = int(disp_w && uint32_t(disp_w) < out.width ? uint32_t(disp_w) : out.width);
		s->pic.height = int(disp_h && uint32_t(disp_h) < out.height ? uint32_t(disp_h) : out.height);
		s->pic.ten_bit = ten;
		s->pic.pts_us = INT64_MIN;  // paired in receive() after reorder_depth packets
		s->seq = ++next_seq;
		s->used = true;
		ro_count++;
	}

	// Frame slot the decoder may DMA into without clobbering a picture still
	// queued in ro[]: scan from the round-robin cursor for the first slot no
	// queued picture points at. The driver decodes into the fb.buffer we hand
	// it (frame_mem + fslot*frame_size) and reports it back as out.buffer,
	// which harvest() publishes as pic.y — so a queued picture whose pic.y
	// equals a slot's address pins that slot. The scan always finds one:
	// ro[] holds at most kReorderSlots (14) distinct pinned slots, fewer than
	// the kFrameSlots (24) in the pool.
	unsigned alloc_fslot() {
		for (unsigned k = 0; k < kFrameSlots; ++k) {
			unsigned cand = (au_ring + k) % kFrameSlots;
			const uint8_t* ptr =
			    static_cast<const uint8_t*>(slot->frame_mem) + size_t(cand) * slot->frame_size;
			bool busy = false;
			for (const auto& r : ro) {
				if (r.used && r.pic.y == ptr) {
					busy = true;
					break;
				}
			}
			if (!busy) return cand;
		}
		return au_ring % kFrameSlots;
	}

	// One access unit to the decoder. present=false: decode for reference only.
	int decode_one(const uint8_t* au, int size, int64_t pts, bool present) {
		if (size <= 0 || size_t(size) > kInputSlotBytes) return -1;
		// Leading pictures after a random access point (open GOP) reference
		// what was never decoded: drop them before the decoder sees them.
		if (desc->codec_type == kCodecHevc && drop_leading) {
			int type = hevc_au_nal_type(au, size);
			if (type == kNalRaslN || type == kNalRaslR) return 0;
			if (type >= 0 && type <= kNalTrailHi) drop_leading = false;
		}
		unsigned islot = au_ring % kInputSlots, fslot = alloc_fslot();
		au_ring++;
		uint8_t* in_mem = static_cast<uint8_t*>(slot->input_mem) + size_t(islot) * kInputSlotBytes;
		bool irap = false;
		int vcl = -1;
		if (desc->codec_type == kCodecHevc) {
			int n = hevc_copy_base_layer(in_mem, au, size);
			if (n != size && !logged_strip) {
				logged_strip = true;
				dlog("hwdec: left out Dolby Vision layers / repeated parameter sets");
			}
			size = n;
			if (size <= 0) return 0;
			vcl = hevc_au_nal_type(in_mem, size);
			irap = vcl >= 16 && vcl <= kNalIrapHi;
			if (need_irap && vcl >= 0 && !irap) return 0;  // depends on a refused keyframe
		} else {
			memcpy(in_mem, au, size_t(size));
			vcl = 1;
		}

		SceVideodec2InputData in{};
		SceVideodec2FrameBuffer fb{};
		SceVideodec2OutputInfo out{};
		in.size = sizeof in;
		in.au = in_mem;
		in.au_size = uint64_t(size);
		in.pts = uint64_t(pts);
		in.dts = UINT64_MAX;
		fb.size = sizeof fb;
		fb.buffer = static_cast<uint8_t*>(slot->frame_mem) + size_t(fslot) * slot->frame_size;
		fb.buffer_size = slot->frame_size;
		out.size = sizeof out;
		int rc = sceVideodec2Decode(slot->decoder, &in, &fb, &out);
		// Step 3 (sub-0.5s seek): if the very first keyframe after a flush is
		// refused, the decoder may still hold pre-seek state — renew the
		// slot, re-feed the parameter sets, and retry once before the normal
		// error path below runs.
		if (rc != 0 && since_flush_out == 0 && irap) {
			std::lock_guard<std::mutex> lock(g_slots_m);
			if (slot_renew(*slot) == 0) {
				if (!extradata.empty() && extradata.size() <= kInputSlotBytes) {
					SceVideodec2InputData xin{};
					SceVideodec2FrameBuffer xfb{};
					SceVideodec2OutputInfo xout{};
					uint8_t* xmem = static_cast<uint8_t*>(slot->input_mem) +
					                size_t((au_ring++) % kInputSlots) * kInputSlotBytes;
					memcpy(xmem, extradata.data(), extradata.size());
					xin.size = sizeof xin;
					xin.au = xmem;
					xin.au_size = extradata.size();
					xin.pts = UINT64_MAX;
					xin.dts = UINT64_MAX;
					xfb.size = sizeof xfb;
					xfb.buffer = fb.buffer;
					xfb.buffer_size = fb.buffer_size;
					xout.size = sizeof xout;
					sceVideodec2Decode(slot->decoder, &xin, &xfb, &xout);
				}
				rc = sceVideodec2Decode(slot->decoder, &in, &fb, &out);
			}
		}
		calls++;
		if (calls <= 2 || (out.valid && frames == 0))
			dlog("hwdec: decode #%u rc 0x%08x valid %u %ux%u pitch %u", calls, unsigned(rc), out.valid, out.width,
			     out.height, out.pitch_bytes ? out.pitch_bytes : out.pitch);
		if (irap) need_irap = rc != 0;
		if (rc != 0) {
			fails++;
			if (!logged_err) {
				logged_err = true;
				dlog("hwdec: decode #%u failed rc 0x%08x (%d bytes)", calls, unsigned(rc), size);
			}
			// Before the first picture after a seek, errors mean we started
			// mid-stream, not that the stream can't be decoded.
			if (since_flush_out == 0 && ++post_flush_errs <= kPostFlushErrors) return 0;
			return -1;
		}
		if (present && vcl >= 0) pts_push(pts);
		if (out.valid && out.picture_count) {
			since_flush_out++;
			if (present) {
				frames++;
				harvest(out);
			}
		}
		return 0;
	}

	void drain() {
		for (int guard = 0; guard < 64; guard++) {
			unsigned fslot = alloc_fslot();
			au_ring++;
			SceVideodec2FrameBuffer fb{};
			SceVideodec2OutputInfo out{};
			fb.size = sizeof fb;
			fb.buffer = static_cast<uint8_t*>(slot->frame_mem) + size_t(fslot) * slot->frame_size;
			fb.buffer_size = slot->frame_size;
			out.size = sizeof out;
			if (sceVideodec2Flush(slot->decoder, &fb, &out) != 0) break;
			if (!(out.valid && out.picture_count)) break;
			harvest(out);
			if (ro_count >= kReorderSlots - 1) break;
		}
	}

	// (Re)made at open and on every seek: a fresh h264_mp4toannexb puts the
	// parameter sets back in front of the first keyframe; a flushed one doesn't.
	bool bsf_build() {
		const AVBitStreamFilter* bf = av_bsf_get_by_name(desc->bsf);
		if (!bf) return false;
		if (bsf) av_bsf_free(&bsf);
		if (av_bsf_alloc(bf, &bsf) < 0) return bsf = nullptr, false;
		if (avcodec_parameters_copy(bsf->par_in, bsf_par) < 0) return av_bsf_free(&bsf), false;
		bsf->time_base_in = AVRational{1, 1000000};
		if (av_bsf_init(bsf) < 0) return av_bsf_free(&bsf), false;
		return true;
	}

	bool present_of(const AVPacket* p) const { return !desc->vp9 || vp9_frame_is_shown(p->data, p->size); }

	int feed_filtered() {
		while (av_bsf_receive_packet(bsf, out_pkt) == 0) {
			int64_t fp = out_pkt->pts == AV_NOPTS_VALUE ? INT64_MIN : out_pkt->pts;
			int r = decode_one(out_pkt->data, out_pkt->size, fp, present_of(out_pkt));
			av_packet_unref(out_pkt);
			if (r < 0) return -1;
		}
		return 0;
	}
};

void HwDecoder::load_module() {
	int r = sceSysmoduleLoadModule(kSysmoduleVideodec2);
	g_module_loaded = r == 0;
	dlog("hwdec: video decoder module %s (0x%x)", g_module_loaded ? "loaded" : "NOT loaded", unsigned(r));
}

// Strict parameter match for parked reuse: codec, profile, dimensions,
// pixel format/bit depth and the codec extradata must be identical, so the
// parked bitstream filter and fed parameter sets stay valid. Anything else
// falls back to a full init.
bool hwdec_par_equal(const AVCodecParameters* a, const AVCodecParameters* b) {
	if (!a || !b) return false;
	if (a->codec_id != b->codec_id || a->profile != b->profile || a->width != b->width ||
	    a->height != b->height || a->format != b->format || a->bits_per_raw_sample != b->bits_per_raw_sample ||
	    a->extradata_size != b->extradata_size)
		return false;
	if (a->extradata_size > 0 && std::memcmp(a->extradata, b->extradata, size_t(a->extradata_size)) != 0)
		return false;
	return true;
}

void hwdec_impl_free(HwDecoder::Impl* impl) {
	if (!impl) return;
	if (impl->bsf) av_bsf_free(&impl->bsf);
	if (impl->bsf_par) avcodec_parameters_free(&impl->bsf_par);
	if (impl->par_copy) avcodec_parameters_free(&impl->par_copy);
	av_packet_free(&impl->in_pkt);
	av_packet_free(&impl->out_pkt);
	delete impl;
}

HwDecoder* HwDecoder::open(const AVCodecParameters* par, std::string* why) {
	auto refuse = [&](const std::string& w) {
		if (why) *why = w;
		return nullptr;
	};
	if (!g_module_loaded) return refuse("no decoder module");
	int depth = par->bits_per_raw_sample > 8 ? par->bits_per_raw_sample : 8;
	if (par->format == AV_PIX_FMT_YUV420P10LE || par->format == AV_PIX_FMT_YUV420P10BE ||
	    (par->codec_id == AV_CODEC_ID_HEVC && par->profile == AV_PROFILE_HEVC_MAIN_10) ||
	    (par->codec_id == AV_CODEC_ID_VP9 && par->profile == AV_PROFILE_VP9_2))
		depth = 10;
	if (par->format >= 0 && par->format != AV_PIX_FMT_YUV420P && par->format != AV_PIX_FMT_YUVJ420P &&
	    par->format != AV_PIX_FMT_YUV420P10LE && par->format != AV_PIX_FMT_YUV420P10BE &&
	    par->format != AV_PIX_FMT_NV12)
		return refuse("pixel format isn't 4:2:0");

	const CodecDesc* d = nullptr;
	switch (par->codec_id) {
	case AV_CODEC_ID_H264:
		if (depth > 8) return refuse("10-bit H.264");
		// Baseline, Main, High (and constrained); not High 10/4:2:2/4:4:4.
		if (par->profile != AV_PROFILE_UNKNOWN && par->profile > 100 && par->profile != 578)
			return refuse("H.264 profile " + std::to_string(par->profile));
		d = &kCodecs[H264];
		break;
	case AV_CODEC_ID_HEVC:
		if (depth > 10) return refuse("12-bit HEVC");
		if (par->profile != AV_PROFILE_UNKNOWN && par->profile != AV_PROFILE_HEVC_MAIN &&
		    par->profile != AV_PROFILE_HEVC_MAIN_10)
			return refuse("HEVC profile " + std::to_string(par->profile));
		if (hevc_extradata_tiles(par->extradata, par->extradata_size)) return refuse("HEVC coded in tiles");
		d = &kCodecs[depth == 10 ? HEVC10 : HEVC];
		break;
	case AV_CODEC_ID_VP9:
		if (depth > 8) return refuse("10-bit VP9");
		d = &kCodecs[VP9];
		break;
	default:
		return refuse(std::string(avcodec_get_name(par->codec_id)) + " has no hardware decoder");
	}
	int w = par->width, h = par->height;
	if (w < 16 || h < 16) return refuse("unknown picture size");
	// Size for the coded picture: encoders pad to their block size (up to 64).
	w = std::min((w + 63) & ~63, kMaxW);
	h = std::min((h + 63) & ~63, kMaxH);
	if (par->width > kMaxW || par->height > kMaxH) return refuse("bigger than 4K");
	w = std::max(w, 1920);
	h = std::max(h, 1088);

	std::lock_guard<std::mutex> lock(g_slots_m);
	Slot& slot = g_slots[d->kind];
	if (slot.owned) return refuse("decoder busy");
	if (slot.ready && (slot.max_w < w || slot.max_h < h)) {
		// The parked instance points into this slot: free it before teardown
		// or g_parked becomes a dangling pointer.
		if (g_parked[d->kind]) {
			hwdec_impl_free(g_parked[d->kind]);
			g_parked[d->kind] = nullptr;
		}
		slot_teardown(slot);
	}
	if (!slot.ready && slot_bringup(slot, *d, w, h) != 0) return refuse("decoder set-up failed");

	// Back-to-back reuse (episode transitions, seek re-opens): when the
	// parked instance's parameters match byte-for-byte (see
	// hwdec_par_equal), hand it back instead of building a fresh one.
	// Anything else falls through to the full init below.
	if (Impl* parked = g_parked[d->kind]) {
		if (hwdec_par_equal(parked->par_copy, par)) {
			g_parked[d->kind] = nullptr;
			parked->desc = d;
			parked->slot = &slot;
			parked->disp_w = par->width;
			parked->disp_h = par->height;
			parked->au_ring = 0;
			parked->flushing = false;
			parked->fatal = false;
			parked->drop_leading = true;
			parked->need_irap = false;
			parked->post_flush_errs = 0;
			parked->since_flush_out = 0;
			parked->logged_err = false;
			parked->logged_strip = false;
			parked->pts_n = 0;
			parked->ro_count = 0;
			parked->next_seq = 0;
			for (auto& r : parked->ro) r.used = false;
			parked->chroma_rows = 0;
			// reorder_depth and extradata are still valid: the parameters
			// are identical. The bitstream filter is rebuilt exactly like
			// flush() does after a seek: it may hold the previous stream's
			// tail, and only a fresh filter puts the parameter sets back in
			// front of the first keyframe.
			if (parked->bsf_par && !parked->bsf_build()) {
				dlog("hwdec: parked decoder's bitstream filter rebuild failed");
				hwdec_impl_free(parked);
			} else if (slot_renew(slot) != 0) {
				// Reset alone leaves some 4K streams unable to restart after
				// seek/episode transitions: renew the decoder object.
				dlog("hwdec: parked decoder slot_renew failed; rebuilding slot");
				hwdec_impl_free(parked);
				slot_teardown(slot);
				if (slot_bringup(slot, *d, w, h) != 0) return refuse("decoder set-up failed");
			} else {
				slot.owned = true;
				if (!parked->extradata.empty())
					parked->decode_one(reinterpret_cast<const uint8_t*>(parked->extradata.data()),
					                   int(parked->extradata.size()), INT64_MIN, false);
				auto* hd = new HwDecoder();
				hd->d_ = parked;
				dlog("hwdec: reusing parked decoder for %dx%d %s", par->width, par->height, d->tag);
				return hd;
			}
		} else {
			// Parameters differ: drop the parked instance, full init below.
			hwdec_impl_free(parked);
		}
		g_parked[d->kind] = nullptr;
	}

	auto* impl = new Impl();
	impl->desc = d;
	impl->slot = &slot;
	impl->disp_w = par->width;
	impl->disp_h = par->height;
	// Snapshot the parameters for parked-reuse matching (best effort: a
	// failed snapshot just disables reuse for this instance).
	impl->par_copy = avcodec_parameters_alloc();
	if (impl->par_copy && avcodec_parameters_copy(impl->par_copy, par) < 0) {
		avcodec_parameters_free(&impl->par_copy);
		impl->par_copy = nullptr;
	}
	// Reorder depth from the stream's own SPS when the container carries one;
	// otherwise the historical default of 4. A smaller real depth only lets
	// pictures out earlier (less latency); pairing still takes the smallest
	// pending PTS, so this degrades gracefully to the old behavior.
	{
		int rd = sps_reorder_depth(par);
		if (rd >= 0) {
			// SPS explicitly reports depth: use it (capped at kReorderDepth).
			if (rd != impl->reorder_depth) {
				impl->reorder_depth = rd < kReorderDepth ? rd : kReorderDepth;
				dlog("hwdec: reorder depth %d from SPS", impl->reorder_depth);
			}
		} else {
			// SPS unparseable (e.g. custom scaling lists): default to 4, not 8.
			// Holding 8 frames back on a 2-frame stream wastes latency.
			impl->reorder_depth = 4;
		}
	}
	bool need_bsf = d->vp9 || (par->extradata && par->extradata_size >= 4 && par->extradata[0] == 1);
	if (need_bsf) {
		impl->bsf_par = avcodec_parameters_alloc();
		if (!impl->bsf_par || avcodec_parameters_copy(impl->bsf_par, par) < 0 || !impl->bsf_build()) {
			hwdec_impl_free(impl);
			return refuse(std::string("bitstream filter ") + d->bsf + " failed");
		}
	} else if (par->extradata && par->extradata_size > 0) {
		impl->extradata.assign(reinterpret_cast<const char*>(par->extradata), size_t(par->extradata_size));
	}
	impl->in_pkt = av_packet_alloc();
	impl->out_pkt = av_packet_alloc();
	slot.owned = true;
	// Renew, don't just reset: reset alone leaves some 4K streams unable to
	// restart after seek/episode transitions.
	if (slot_renew(slot) != 0) {
		dlog("hwdec: slot_renew failed on fresh open; rebuilding");
		slot_teardown(slot);
		if (slot_bringup(slot, *d, w, h) != 0) return refuse("decoder set-up failed");
		slot.owned = true;
	}
	if (!impl->extradata.empty())
		impl->decode_one(reinterpret_cast<const uint8_t*>(impl->extradata.data()), int(impl->extradata.size()),
		                 INT64_MIN, false);
	impl->drop_leading = true;

	auto* hd = new HwDecoder();
	hd->d_ = impl;
	dlog("hwdec: playing %dx%d %s on the hardware decoder", par->width, par->height, d->tag);
	return hd;
}

HwDecoder::~HwDecoder() {
	if (!d_) return;
	dlog("hwdec: closed after %u decodes, %u pictures, %u errors", d_->calls, d_->frames, d_->fails);
	// Park the instance for back-to-back reuse when it closed cleanly: the
	// next open() with byte-identical parameters hands it back instead of
	// building a fresh one. A decoder that failed mid-stream is never
	// parked, and without a parameter snapshot there is nothing to match.
	const bool park = !d_->fatal && d_->par_copy;
	{
		std::lock_guard<std::mutex> lock(g_slots_m);
		if (d_->slot->decoder) sceVideodec2Reset(d_->slot->decoder);
		d_->slot->owned = false;
		if (park) {
			if (g_parked[d_->desc->kind]) hwdec_impl_free(g_parked[d_->desc->kind]);
			g_parked[d_->desc->kind] = d_;
		}
	}
	if (park) return;  // the parked instance keeps its packets/bsf/par for reuse
	hwdec_impl_free(d_);
}

int HwDecoder::send(const uint8_t* data, int size, int64_t pts_us) {
	Impl* n = d_;
	if (n->fatal || !n->slot->decoder) return -1;
	if (!data || size <= 0) {  // end of stream: everything out
		if (n->bsf) {
			av_bsf_send_packet(n->bsf, nullptr);
			n->feed_filtered();
		}
		n->drain();
		n->flushing = true;
		return 0;
	}
	// Backpressure: stop feeding input only when the output buffer is nearly
	// full (kReorderSlots - 2), not at reorder_depth. The old threshold
	// (ro_count > reorder_depth) starved the decoder on streams with deep
	// B-frame reorder (anime): the 4-frame pipeline filled instantly, send()
	// blocked, and the decoder idled while the player starved.
	if (n->ro_count >= kReorderSlots - 2) return 1;
	int r;
	if (n->bsf) {
		av_packet_unref(n->in_pkt);
		if (av_new_packet(n->in_pkt, size) < 0) return -1;
		memcpy(n->in_pkt->data, data, size_t(size));
		n->in_pkt->pts = pts_us == INT64_MIN ? AV_NOPTS_VALUE : pts_us;
		n->in_pkt->dts = AV_NOPTS_VALUE;
		if (av_bsf_send_packet(n->bsf, n->in_pkt) < 0) return -1;
		r = n->feed_filtered();
	} else {
		r = n->decode_one(data, size, pts_us, true);
	}
	if (r < 0) n->fatal = true;
	return r < 0 ? -1 : 0;
}

int HwDecoder::receive(Picture* out) {
	Impl* n = d_;
	if (n->fatal) return -1;
	if (n->ro_count <= (n->flushing ? 0 : n->reorder_depth)) return 0;
	// Pick the oldest picture (lowest seq = FIFO display order from VDEC).
	// Pair its timestamp NOW via pts_take(), after reorder_depth more packets
	// have been fed, so all B-frame timestamps are in pts_pool.
	Impl::Out* best = nullptr;
	for (auto& r : n->ro) {
		if (!r.used) continue;
		if (!best || r.seq < best->seq)
			best = &r;
	}
	if (!best) return 0;
	best->pic.pts_us = n->pts_take();
	*out = best->pic;
	best->used = false;
	n->ro_count--;
	return 1;
}

void HwDecoder::flush() {
	Impl* n = d_;
	for (auto& r : n->ro) r.used = false;
	n->ro_count = 0;
	n->next_seq = 0;
	n->pts_n = 0;
	n->au_ring = 0;
	n->flushing = false;
	n->fatal = false;
	n->drop_leading = true;
	n->need_irap = false;
	n->post_flush_errs = 0;
	n->since_flush_out = 0;
	n->logged_err = false;
	if (n->bsf_par && !n->bsf_build()) {
		dlog("hwdec: bitstream filter rebuild failed after a seek");
		n->fatal = true;
		return;
	}
	{
		std::lock_guard<std::mutex> lock(g_slots_m);
		// Unconditional renew (no reset-first attempt): sceVideodec2Reset
		// returns SCE_OK while silently emitting zero pictures on some 4K
		// streams (Nuvio PS5, hardware 2026-10-01), so an rc != 0 fallback
		// can never catch that wedge — the decoder looks fine while
		// producing nothing forever. Renewing reuses the already-mapped
		// Direct Memory pools, so it stays fast (~3-5ms).
		if (slot_renew(*n->slot) != 0) {
			dlog("hwdec: renew after a seek failed");
			n->fatal = true;
			n->slot->ready = false;  // the next open builds it again
			return;
		}
	}
	if (!n->extradata.empty())
		n->decode_one(reinterpret_cast<const uint8_t*>(n->extradata.data()), int(n->extradata.size()), INT64_MIN,
		              false);
}

const char* HwDecoder::name() const { return d_->desc->tag; }

#endif  // PLATFORM_PS5_NATIVE
