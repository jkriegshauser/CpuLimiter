/**
 * @file CpuLimiter.c
 * @author Joshua Kriegshauser (https://github.com/jkriegshauser)
 * @brief A DLL that can be injected into applications to limit the number of CPUs available
 * @version 1.0
 * @date 2024-11-16
 *
 * @copyright Copyright (c) 2024 Joshua Kriegshauser
 *
 */

#include <windows.h>
#include <winerror.h>
#include <detours.h>

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <malloc.h>
#include <math.h>

//! This is the number of CPUs that we'll tell the current process that we have.
#define NUM_CPUS 24u

//! Logging to OutputDebugString (i.e. readable with SysInternals DebugView) is enabled by setting LOGGING 1
#define LOGGING 1
#if !defined LOGGING
#    ifdef NDEBUG
#        define LOGGING 0
#    else
#        define LOGGING 1
#    endif
#endif

#define PROCINFO_LOGGING (LOGGING && 0)

// Typedefs for functions that we'll be hooking
typedef void(WINAPI* GetSystemInfo_t)(LPSYSTEM_INFO);
typedef void(WINAPI* GetNativeSystemInfo_t)(LPSYSTEM_INFO);
typedef BOOL(WINAPI* GetProcessAffinityMask_t)(HANDLE, PDWORD_PTR, PDWORD_PTR);
typedef BOOL(WINAPI* SetProcessAffinityMask_t)(HANDLE hProcess, DWORD_PTR dwProcessAffinityMask);
typedef DWORD_PTR(WINAPI* SetThreadAffinityMask_t)(HANDLE hThread, DWORD_PTR dwThreadAffinityMask);
typedef BOOL(WINAPI* GetProcessGroupAffinity_t)(HANDLE hProcess, PUSHORT GroupCount, PUSHORT GroupArray);
typedef BOOL(WINAPI* GetThreadGroupAffinity_t)(HANDLE hThread, PGROUP_AFFINITY GroupAffinity);
typedef BOOL(WINAPI* SetThreadGroupAffinity_t)(HANDLE hThread,
                                               const GROUP_AFFINITY* GroupAffinity,
                                               PGROUP_AFFINITY PreviousGroupAffinity);
typedef DWORD(WINAPI* SetThreadIdealProcessor_t)(HANDLE hThread, DWORD dwIdealProcessor);
typedef BOOL(WINAPI* SetThreadIdealProcessorEx_t)(HANDLE hThread,
                                                  PPROCESSOR_NUMBER lpIdealProcessor,
                                                  PPROCESSOR_NUMBER lpPreviousIdealProcessor);
typedef BOOL(WINAPI* GetLogicalProcessorInformation_t)(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION, PDWORD);
typedef BOOL(WINAPI* GetLogicalProcessorInformationEx_t)(LOGICAL_PROCESSOR_RELATIONSHIP,
                                                         PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX,
                                                         PDWORD);
typedef LPTOP_LEVEL_EXCEPTION_FILTER(WINAPI* SetUnhandledExceptionFilter_t)(LPTOP_LEVEL_EXCEPTION_FILTER filter);

// TODO: CPU Set support?
// TODO: Hybrid CPU detection? Offloading efficiency cores?

static bool installed;

const unsigned kNumCpus = NUM_CPUS;
const unsigned long long kCpuMask = (1ull << NUM_CPUS) - 1;

static GetSystemInfo_t OrigGetSystemInfo;
static GetSystemInfo_t OrigGetNativeSystemInfo;
static GetProcessAffinityMask_t OrigGetProcessAffinityMask;
static SetProcessAffinityMask_t OrigSetProcessAffinityMask;
static SetThreadAffinityMask_t OrigSetThreadAffinityMask;
static GetProcessGroupAffinity_t OrigGetProcessGroupAffinity;
static GetThreadGroupAffinity_t OrigGetThreadGroupAffinity;
static SetThreadGroupAffinity_t OrigSetThreadGroupAffinity;
static SetThreadIdealProcessor_t OrigSetThreadIdealProcessor;
static SetThreadIdealProcessorEx_t OrigSetThreadIdealProcessorEx;
static GetLogicalProcessorInformation_t OrigGetLogicalProcessorInformation;
static GetLogicalProcessorInformationEx_t OrigGetLogicalProcessorInformationEx;
static SetUnhandledExceptionFilter_t OrigSetUnhandledExceptionFilter;

struct BINK;
typedef struct BINK* HBINK;
#define PTR4
typedef struct BINKPLANE
{
    int32_t Allocate;
    void* Buffer;
    uint32_t BufferPitch;
} BINKPLANE;

typedef struct BINKFRAMEPLANESET
{
    BINKPLANE YPlane;
    BINKPLANE cRPlane;
    BINKPLANE cBPlane;
    BINKPLANE APlane;
} BINKFRAMEPLANESET;

#define BINKMAXFRAMEBUFFERS 2

typedef struct BINKFRAMEBUFFERS
{
    int32_t TotalFrames;
    uint32_t YABufferWidth;
    uint32_t YABufferHeight;
    uint32_t cRcBBufferWidth;
    uint32_t cRcBBufferHeight;

    uint32_t FrameNum;
    BINKFRAMEPLANESET Frames[BINKMAXFRAMEBUFFERS];
} BINKFRAMEBUFFERS;

typedef int32_t S32;
typedef uint32_t U32;
typedef struct BINKSUMMARY
{
    U32 Width; // Width of frames
    U32 Height; // Height of frames
    U32 TotalTime; // total time (ms)
    U32 FileFrameRate; // frame rate
    U32 FileFrameRateDiv; // frame rate divisor
    U32 FrameRate; // frame rate
    U32 FrameRateDiv; // frame rate divisor
    U32 TotalOpenTime; // Time to open and prepare for decompression
    U32 TotalFrames; // Total Frames
    U32 TotalPlayedFrames; // Total Frames played
    U32 SkippedFrames; // Total number of skipped frames
    U32 SkippedBlits; // Total number of skipped blits
    U32 SoundSkips; // Total number of sound skips
    U32 TotalBlitTime; // Total time spent blitting
    U32 TotalReadTime; // Total time spent reading
    U32 TotalVideoDecompTime; // Total time spent decompressing video
    U32 TotalAudioDecompTime; // Total time spent decompressing audio
    U32 TotalIdleReadTime; // Total time spent reading while idle
    U32 TotalBackReadTime; // Total time spent reading in background
    U32 TotalReadSpeed; // Total io speed (bytes/second)
    U32 SlowestFrameTime; // Slowest single frame time (ms)
    U32 Slowest2FrameTime; // Second slowest single frame time (ms)
    U32 SlowestFrameNum; // Slowest single frame number
    U32 Slowest2FrameNum; // Second slowest single frame number
    U32 AverageDataRate; // Average data rate of the movie
    U32 AverageFrameSize; // Average size of the frame
    U32 HighestMemAmount; // Highest amount of memory allocated
    U32 TotalIOMemory; // Total extra memory allocated
    U32 HighestIOUsed; // Highest extra memory actually used
    U32 Highest1SecRate; // Highest 1 second rate
    U32 Highest1SecFrame; // Highest 1 second start frame
} BINKSUMMARY;

typedef struct BINKRECT
{
    S32 Left, Top, Width, Height;
} BINKRECT;
#define BINKMAXDIRTYRECTS 8

typedef uint8_t U8;
#define RADLINK __stdcall

struct BINKIO;
typedef S32(RADLINK PTR4* BINKIOOPEN)(struct BINKIO PTR4* Bnkio, const char PTR4* name, U32 flags);
typedef U32(RADLINK PTR4* BINKIOREADHEADER)(struct BINKIO PTR4* Bnkio, S32 Offset, void PTR4* Dest, U32 Size);
typedef U32(RADLINK PTR4* BINKIOREADFRAME)(struct BINKIO PTR4* Bnkio, U32 Framenum, S32 origofs, void PTR4* dest, U32 size);
typedef U32(RADLINK PTR4* BINKIOGETBUFFERSIZE)(struct BINKIO PTR4* Bnkio, U32 Size);
typedef void(RADLINK PTR4* BINKIOSETINFO)(struct BINKIO PTR4* Bnkio, void PTR4* Buf, U32 Size, U32 FileSize, U32 simulate);
typedef U32(RADLINK PTR4* BINKIOIDLE)(struct BINKIO PTR4* Bnkio);
typedef void(RADLINK PTR4* BINKIOCLOSE)(struct BINKIO PTR4* Bnkio);
typedef S32(RADLINK PTR4* BINKIOBGCONTROL)(struct BINKIO PTR4* Bnkio, U32 Control);

typedef void(RADLINK PTR4* BINKCBSUSPEND)(struct BINKIO PTR4* Bnkio);
typedef S32(RADLINK PTR4* BINKCBTRYSUSPEND)(struct BINKIO PTR4* Bnkio);
typedef void(RADLINK PTR4* BINKCBRESUME)(struct BINKIO PTR4* Bnkio);
typedef void(RADLINK PTR4* BINKCBIDLE)(struct BINKIO PTR4* Bnkio);

typedef struct BINKIO
{
    BINKIOREADHEADER ReadHeader;
    BINKIOREADFRAME ReadFrame;
    BINKIOGETBUFFERSIZE GetBufferSize;
    BINKIOSETINFO SetInfo;
    BINKIOIDLE Idle;
    BINKIOCLOSE Close;
    BINKIOBGCONTROL BGControl;
    HBINK bink;
    volatile U32 ReadError;
    volatile U32 DoingARead;
    volatile U32 BytesRead;
    volatile U32 Working;
    volatile U32 TotalTime;
    volatile U32 ForegroundTime;
    volatile U32 IdleTime;
    volatile U32 ThreadTime;
    volatile U32 BufSize;
    volatile U32 BufHighUsed;
    volatile U32 CurBufSize;
    volatile U32 CurBufUsed;
    volatile U32 Suspended;
    volatile U8 iodata[128 + 32];

    // filled in by the caller
    BINKCBSUSPEND suspend_callback;
    BINKCBTRYSUSPEND try_suspend_callback;
    BINKCBRESUME resume_callback;
    BINKCBIDLE idle_on_callback;
    volatile U32 callback_control[16]; // buffer for background IO callback
} BINKIO;

struct BINKSND;
typedef S32(RADLINK PTR4* BINKSNDOPEN)(struct BINKSND PTR4* BnkSnd, U32 freq, S32 bits, S32 chans, U32 flags, HBINK bink);
typedef S32(RADLINK PTR4* BINKSNDREADY)(struct BINKSND PTR4* BnkSnd);
typedef S32(RADLINK PTR4* BINKSNDLOCK)(struct BINKSND PTR4* BnkSnd, U8 PTR4* PTR4* addr, U32 PTR4* len);
typedef S32(RADLINK PTR4* BINKSNDUNLOCK)(struct BINKSND PTR4* BnkSnd, U32 filled);
typedef void(RADLINK PTR4* BINKSNDVOLUME)(struct BINKSND PTR4* BnkSnd, S32 volume);
typedef void(RADLINK PTR4* BINKSNDPAN)(struct BINKSND PTR4* BnkSnd, S32 pan);
typedef void(RADLINK PTR4* BINKSNDMIXBINS)(struct BINKSND PTR4* BnkSnd, U32 PTR4* mix_bins, U32 total);
typedef void(RADLINK PTR4* BINKSNDMIXBINVOLS)(struct BINKSND PTR4* BnkSnd,
                                              U32 PTR4* vol_mix_bins,
                                              S32 PTR4* volumes,
                                              U32 total);
typedef S32(RADLINK PTR4* BINKSNDONOFF)(struct BINKSND PTR4* BnkSnd, S32 status);
typedef S32(RADLINK PTR4* BINKSNDPAUSE)(struct BINKSND PTR4* BnkSnd, S32 status);
typedef void(RADLINK PTR4* BINKSNDCLOSE)(struct BINKSND PTR4* BnkSnd);

typedef BINKSNDOPEN(RADLINK PTR4* BINKSNDSYSOPEN)(U32 param);

typedef struct BINKSND
{
    BINKSNDREADY Ready;
    BINKSNDLOCK Lock;
    BINKSNDUNLOCK Unlock;
    BINKSNDVOLUME Volume;
    BINKSNDPAN Pan;
    BINKSNDPAUSE Pause;
    BINKSNDONOFF SetOnOff;
    BINKSNDCLOSE Close;
    BINKSNDMIXBINS MixBins;
    BINKSNDMIXBINVOLS MixBinVols;

    U32 sndbufsize; // sound buffer size
    U8 PTR4* sndbuf; // sound buffer
    U8 PTR4* sndend; // end of the sound buffer
    U8 PTR4* sndwritepos; // current write position
    U8 PTR4* sndreadpos; // current read position
    U32 sndcomp; // sound compression handle
    U32 sndamt; // amount of sound currently in the buffer
    U32 sndconvert8; // convert back to 8-bit sound at runtime
    U32 sndendframe; // frame number that the sound ends on
    U32 sndprime; // amount of data to prime the playahead
    U32 sndpad; // padded this much audio

    U32 BestSizeIn16;
    U32 BestSizeMask;
    U32 SoundDroppedOut;
    S32 NoThreadService;
    S32 OnOff;
    U32 Latency;
    U32 VideoScale;
    U32 freq;
    S32 bits, chans;
    U8 snddata[256];
} BINKSND;

typedef struct BUNDLEPOINTERS
{
    void* typeptr;
    void* type16ptr;
    void* colorptr;
    void* bits2ptr;
    void* motionXptr;
    void* motionYptr;
    void* dctptr;
    void* mdctptr;
    void* patptr;
} BUNDLEPOINTERS;

HMODULE g_bink;
struct BINKBUFFER;
typedef struct BINK
{
    U32 Width; // Width (1 based, 640 for example)
    U32 Height; // Height (1 based, 480 for example)
    U32 Frames; // Number of frames (1 based, 100 = 100 frames)
    U32 FrameNum; // Frame to *be* displayed (1 based)
    U32 LastFrameNum; // Last frame decompressed or skipped (1 based)

    U32 FrameRate; // Frame Rate Numerator
    U32 FrameRateDiv; // Frame Rate Divisor (frame rate=numerator/divisor)

    U32 ReadError; // Non-zero if a read error has ocurred
    U32 OpenFlags; // flags used on open
    U32 BinkType; // Bink flags

    U32 Size; // size of file
    U32 FrameSize; // The current frame's size in bytes
    U32 SndSize; // The current frame sound tracks' size in bytes

    BINKRECT FrameRects[BINKMAXDIRTYRECTS]; // Dirty rects from BinkGetRects
    S32 NumRects;

    BINKFRAMEBUFFERS* FrameBuffers; // Bink frame buffers that we decompress to

    void PTR4* MaskPlane; // pointer to the mask plane (Ywidth/16*Yheight/16)
    U32 MaskPitch; // Mask Pitch
    U32 MaskLength; // total length of the mask plane

    U32 LargestFrameSize; // Largest frame size
    U32 InternalFrames; // how many frames were potentially compressed

    S32 NumTracks; // how many tracks

    U32 Highest1SecRate; // Highest 1 sec data rate
    U32 Highest1SecFrame; // Highest 1 sec data rate starting frame

    S32 Paused; // is the bink movie paused?

    U32 BackgroundThread; // handle to background thread

    // everything below is for internal Bink use

    void PTR4* compframe; // compressed frame data
    void PTR4* preloadptr; // preloaded compressed frame data
    U32* frameoffsets; // offsets of each of the frames

    BINKIO bio; // IO structure
    U8 PTR4* ioptr; // io buffer ptr
    U32 iosize; // io buffer size
    U32 decompwidth; // width not include scaling
    U32 decompheight; // height not include scaling

    S32 PTR4* trackindexes; // track indexes
    U32 PTR4* tracksizes; // largest single frame of track
    U32 PTR4* tracktypes; // type of each sound track
    S32 PTR4* trackIDs; // external track numbers

    U32 numrects; // number of rects from BinkGetRects

    U32 playedframes; // how many frames have we played
    U32 firstframetime; // very first frame start
    U32 startframetime; // start frame start
    U32 startblittime; // start of blit period
    U32 startsynctime; // start of synched time
    U32 startsyncframe; // frame of startsynctime
    U32 twoframestime; // two frames worth of time
    U32 entireframetime; // entire frame time

    U32 slowestframetime; // slowest frame in ms
    U32 slowestframe; // slowest frame number
    U32 slowest2frametime; // second slowest frame in ms
    U32 slowest2frame; // second slowest frame

    U32 soundon; // sound turned on?
    U32 videoon; // video turned on?

    U32 totalmem; // total memory used
    U32 timevdecomp; // total time decompressing video
    U32 timeadecomp; // total time decompressing audio
    U32 timeblit; // total time blitting
    U32 timeopen; // total open time

    U32 fileframerate; // frame rate originally in the file
    U32 fileframeratediv;

    U32 runtimeframes; // max frames for runtime analysis
    S32 rtindex; // index of where we are in the runtime frames
    U32 PTR4* rtframetimes; // start times for runtime frames
    U32 PTR4* rtadecomptimes; // decompress times for runtime frames
    U32 PTR4* rtvdecomptimes; // decompress times for runtime frames
    U32 PTR4* rtblittimes; // blit times for runtime frames
    U32 PTR4* rtreadtimes; // read times for runtime frames
    U32 PTR4* rtidlereadtimes; // idle read times for runtime frames
    U32 PTR4* rtthreadreadtimes; // thread read times for runtime frames

    U32 lastblitflags; // flags used on last blit
    U32 lastdecompframe; // last frame number decompressed

    U32 lastresynctime; // last loop point that we did a resync on
    U32 doresync; // should we do a resync in the next doframe?

    U32 skipcount; // how many skipped blocks on last frame
    U32 toofewskipstomask; // fewer than this many skips shuold be full blitted

    U32 playingtracks; // how many tracks are playing
    U32 soundskips; // number of sound stops
    BINKSND PTR4* bsnd; // SND structures
    U32 skippedlastblit; // skipped last frame?
    U32 skipped_this_frame; // skipped the current frame?
    U32 skippedblits; // how many blits were skipped

    BUNDLEPOINTERS bunp; // pointers to internal temporary memory
    U32 skipped_in_a_row; // how many frames have we skipped in a row
    U32 big_sound_skip_adj; // adjustment for large skips
    U32 big_sound_skip_reduce; // amount to reduce large skips by each frame
    U32 paused_sync_diff; // sync delta at the time of a pause
    U32 last_time_almost_empty; // time of last almost empty IO buffer
    U32 last_read_count; // counter to keep track of the last bink IO
    U32 last_sound_count; // counter to keep track of the last bink sound
    U32 snd_callback_buffer[16]; // buffer for background sound callback
    S32 allkeys; // are all frames keyframes?
    BINKFRAMEBUFFERS* allocatedframebuffers; // pointer to internally allocated buffers
} BINK;
typedef struct BINKGPUBUFFERS
{
    U32 unk1;
    U32 unk2;
    void* buf;
    U32 unk3;
    U32 unk4;
    U32 unk5;
    U32 unk6;
} BINKGPUBUFFERS;
// typedef struct BINKFRAMEBUFFERS BINKFRAMEBUFFERS;
typedef struct BINKBUFFER* HBINKBUFFER;
typedef struct BINK* HBINK;
typedef uint32_t BINK_OPEN_FLAGS;
typedef uint64_t BINK_COPY_FLAGS;
typedef uint64_t BINKBUFFER_OPEN_FLAGS;
typedef HBINKBUFFER (*BinkBufferOpen_t)(HWND wnd, uint32_t width, uint32_t height, BINKBUFFER_OPEN_FLAGS flags);
typedef int32_t (*BinkBufferSetOffset_t)(HBINKBUFFER hbuf, int32_t dest_x, int32_t dest_y);
typedef int32_t (*BinkBufferSetScale_t)(HBINKBUFFER hbuf, uint32_t width, uint32_t height);
typedef HBINK (*BinkOpen_t)(const char* file_name, BINK_OPEN_FLAGS flags);
typedef void (*BinkClose_t)(HBINK bink);
typedef int32_t (*BinkCopyToBuffer_t)(
    HBINK bink, void* dest, int32_t pitch, uint32_t height, uint32_t x, uint32_t y, BINK_COPY_FLAGS flags);
typedef int32_t (*BinkCopyToBufferRect_t)(HBINK bink,
                                          void* dest,
                                          int32_t pitch,
                                          uint32_t height,
                                          uint32_t x,
                                          uint32_t y,
                                          uint32_t src_x,
                                          uint32_t src_y,
                                          uint32_t src_w,
                                          uint32_t src_h,
                                          BINK_COPY_FLAGS flags);
typedef void (*BinkGetFrameBuffersInfo_t)(HBINK bink, BINKFRAMEBUFFERS* set);
typedef void (*BinkRegisterFrameBuffers_t)(HBINK bink, BINKFRAMEBUFFERS* set);
typedef void (*BinkGetSummary_t)(HBINK bink, BINKSUMMARY* summary);
typedef S32 (*BinkDoFrame_t)(HBINK bink);
typedef void (*BinkNextFrame_t)(HBINK bink);
typedef S32 (*BinkGetGPUDataBuffersInfo_t)(HBINK bink, BINKGPUBUFFERS* out);
typedef S32 (*BinkGetPlatformInfo_t)(U32 param, U32* out);
typedef void (*BinkBufferSetResolution_t)(U32 width, U32 height, U32 color_depth);
typedef U32 (*BinkControlPlatformFeatures_t)(U32 feature, U32 setting);

static BinkBufferOpen_t OrigBinkBufferOpen;
static BinkBufferSetOffset_t OrigBinkBufferSetOffset;
static BinkBufferSetScale_t OrigBinkBufferSetScale;
static BinkOpen_t OrigBinkOpen;
static BinkClose_t OrigBinkClose;
static BinkCopyToBuffer_t OrigBinkCopyToBuffer;
static BinkCopyToBufferRect_t OrigBinkCopyToBufferRect;
static BinkGetFrameBuffersInfo_t OrigBinkGetFrameBuffersInfo;
static BinkRegisterFrameBuffers_t OrigBinkRegisterFrameBuffers;
static BinkGetSummary_t OrigBinkGetSummary;
static BinkDoFrame_t OrigBinkDoFrame;
static BinkNextFrame_t OrigBinkNextFrame;
static BinkGetGPUDataBuffersInfo_t OrigBinkGetGPUDataBuffersInfo;
static BinkGetPlatformInfo_t OrigBinkGetPlatformInfo;
static BinkBufferSetResolution_t OrigBinkBufferSetResolution;
static BinkControlPlatformFeatures_t OrigBinkControlPlatformFeatures;

#define boolstr(s) (s ? "true" : "false")

#define _STRINGIFY(a) #a
#define STRINGIFY(a) _STRINGIFY(a)

#if LOGGING
#    define Log(...) Log_("(" STRINGIFY(__LINE__) ") " __VA_ARGS__)
static void Log_(const char* str, ...)
{
    char buffer[1024] = "CpuLimiter: ";
    va_list ap;
    int ret;

    va_start(ap, str);
    ret = vsnprintf(buffer + 12, sizeof(buffer) - 12, str, ap);
    va_end(ap);

    if (ret < 0)
    {
        OutputDebugString(L"CpuLimiter: vsnprintf failed, format string follows: ");
        OutputDebugStringA(str);
        OutputDebugString(L"\n");
    }
    else
    {
        ret += 12;
        if (ret >= (sizeof(buffer) - 1))
            buffer[sizeof(buffer) - 2] = '\n';
        else
        {
            buffer[ret++] = '\n';
            buffer[ret] = '\0';
        }

        OutputDebugStringA(buffer);
    }
}
#else
#    define Log(...) ((void)0)
#endif

static void WINAPI MyGetSystemInfo(LPSYSTEM_INFO pinfo)
{
    static bool called;

    OrigGetSystemInfo(pinfo);
    if (!called)
    {
        called = true;
        Log("GetSystemInfo called at least once; orig processors: %u", pinfo->dwNumberOfProcessors);
    }
    pinfo->dwNumberOfProcessors = min(pinfo->dwNumberOfProcessors, kNumCpus);
}

static void WINAPI MyGetNativeSystemInfo(LPSYSTEM_INFO pinfo)
{
    static bool called;

    OrigGetNativeSystemInfo(pinfo);
    if (!called)
    {
        called = true;
        Log("GetNativeSystemInfo called at least once; orig processors: % u", pinfo->dwNumberOfProcessors);
    }
    pinfo->dwNumberOfProcessors = min(pinfo->dwNumberOfProcessors, kNumCpus);
}

static BOOL MyGetProcessAffinityMask(HANDLE hProcess, PDWORD_PTR lpProcessAffinityMask, PDWORD_PTR lpSystemAffinityMask)
{
    static bool called;

    BOOL retval = OrigGetProcessAffinityMask(hProcess, lpProcessAffinityMask, lpSystemAffinityMask);
    if (!called)
    {
        called = true;
        Log("GetProcessAffinityMask called at least once, first: (%p, %p, %p) returned %s (process = %zx, system = %zx) (GLE=%u)",
            hProcess, lpProcessAffinityMask, lpSystemAffinityMask, boolstr(retval),
            lpProcessAffinityMask ? *lpProcessAffinityMask : 0, lpSystemAffinityMask ? *lpSystemAffinityMask : 0,
            GetLastError());
    }
    if (retval)
    {
        if (lpProcessAffinityMask)
        {
            *lpProcessAffinityMask &= kCpuMask;
        }
        if (lpSystemAffinityMask)
        {
            *lpSystemAffinityMask &= kCpuMask;
        }
    }
    return retval;
}

static BOOL MySetProcessAffinityMask(HANDLE hProcess, DWORD_PTR dwProcessAffinityMask)
{
    static bool called;
    DWORD_PTR myAffinityMask = dwProcessAffinityMask & kCpuMask;

    BOOL retval = OrigSetProcessAffinityMask(hProcess, myAffinityMask);
    if (!called)
    {
        called = true;
        Log("SetProcessAffinityMask called at least once, first: (%p, %zx) returned %s (GLE=%u)", hProcess,
            dwProcessAffinityMask, boolstr(retval), GetLastError());
    }
    return retval;
}

static DWORD_PTR MySetThreadAffinityMask(HANDLE hThread, DWORD_PTR dwThreadAffinityMask)
{
    static bool called;
    DWORD_PTR myAffinityMask = dwThreadAffinityMask & kCpuMask;

    DWORD_PTR retval = OrigSetThreadAffinityMask(hThread, myAffinityMask);
    if (!called)
    {
        called = true;
        Log("SetThreadAffinityMask called at least once, first: (%p, %zx) returned %zx (GLE=%u)", hThread,
            dwThreadAffinityMask, retval, GetLastError());
    }

    retval &= kCpuMask;

    return retval;
}

static BOOL MyGetProcessGroupAffinity(HANDLE hProcess, PUSHORT GroupCount, PUSHORT GroupArray)
{
    // Just logging for now
    BOOL retval = OrigGetProcessGroupAffinity(hProcess, GroupCount, GroupArray);
    Log("GetProcessGroupAffinity(%p, %p, %p) returned %s (GroupCount = %u) (GLE=%u)", hProcess, GroupCount, GroupArray,
        boolstr(retval), GroupCount ? *GroupCount : 0, GetLastError());
    return retval;
}

static BOOL MyGetThreadGroupAffinity(HANDLE hThread, PGROUP_AFFINITY GroupAffinity)
{
    // Just logging for now
    BOOL retval = OrigGetThreadGroupAffinity(hThread, GroupAffinity);
    Log("GetThreadGroupAffinity(%p, %p) returned %s (GLE=%u)", hThread, GroupAffinity, boolstr(retval), GetLastError());
    return retval;
}

static BOOL MySetThreadGroupAffinity(HANDLE hThread,
                                     const GROUP_AFFINITY* GroupAffinity,
                                     PGROUP_AFFINITY PreviousGroupAffinity)
{
    // Just logging for now
    BOOL retval = OrigSetThreadGroupAffinity(hThread, GroupAffinity, PreviousGroupAffinity);
    Log("SetThreadGroupAffinity(%p, %p, %p) returned %s (GLE=%u)", hThread, GroupAffinity, PreviousGroupAffinity,
        boolstr(retval), GetLastError());
    return retval;
}

static DWORD MySetThreadIdealProcessor(HANDLE hThread, DWORD dwIdealProcessor)
{
    static bool called;

    if (dwIdealProcessor >= kNumCpus && dwIdealProcessor != MAXIMUM_PROCESSORS)
        return (DWORD)-1;

    DWORD retval = OrigSetThreadIdealProcessor(hThread, dwIdealProcessor);
    if (!called)
    {
        called = true;
        Log("SetThreadIdealProcessor called at least once, first: (%p, %u) returned %u (GLE=%u)", hThread,
            dwIdealProcessor, retval, GetLastError());
    }
    if (retval == (DWORD)-1)
        return retval;

    return retval % kNumCpus;
}

static BOOL MySetThreadIdealProcessorEx(HANDLE hThread,
                                        PPROCESSOR_NUMBER lpIdealProcessor,
                                        PPROCESSOR_NUMBER lpPreviousIdealProcessor)
{
    BOOL retval = OrigSetThreadIdealProcessorEx(hThread, lpIdealProcessor, lpPreviousIdealProcessor);
    Log("SetThreadIdealProcessorEx(%p, %p, %p) returned %s (GLE=%u)", hThread, lpIdealProcessor,
        lpPreviousIdealProcessor);
    return retval;
}

static const char* GetName(LOGICAL_PROCESSOR_RELATIONSHIP r)
{
    static const char* RelationshipNames[] = {
        "RelationProcessorCore", "RelationNumaNode",     "RelationCache",      "RelationProcessorPackage",
        "RelationGroup",         "RelationProcessorDie", "RelationNumaNodeEx", "RelationProcessorModule",
    };
    if (r < (sizeof(RelationshipNames) / sizeof(RelationshipNames[1])))
        return RelationshipNames[r];
    if (r == RelationAll)
        return "RelationAll";
    return "Unknown";
}

static LogLogicalProcessorInformation(const char* name, PSYSTEM_LOGICAL_PROCESSOR_INFORMATION Buffer, DWORD count)
{
#if PROCINFO_LOGGING
    Log("LogicalProcessorInformation (%s) - %u entries", name, count);
    const PSYSTEM_LOGICAL_PROCESSOR_INFORMATION end = Buffer + count;
    for (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION iter = Buffer; iter < end; ++iter)
    {
        switch (iter->Relationship)
        {
            case RelationCache:
                Log("%zu: ProcessorMask=%zx Relationship=%d(%s) %u %u %u %u %u", iter - Buffer, iter->ProcessorMask,
                    iter->Relationship, GetName(iter->Relationship), iter->Cache.Level, iter->Cache.Associativity,
                    iter->Cache.LineSize, iter->Cache.Size, iter->Cache.Type);
                break;
            case RelationNumaNode:
                Log("%zu: ProcessorMask=%zx Relationship=%d(%s) %u", iter - Buffer, iter->ProcessorMask,
                    iter->Relationship, GetName(iter->Relationship), iter->NumaNode.NodeNumber);
                break;
            case RelationProcessorCore:
                Log("%zu: ProcessorMask=%zx Relationship=%d(%s) %u", iter - Buffer, iter->ProcessorMask,
                    iter->Relationship, GetName(iter->Relationship), iter->ProcessorCore.Flags);
                break;
            case RelationProcessorPackage:
                Log("%zu: ProcessorMask=%zx Relationship=%d(%s)", iter - Buffer, iter->ProcessorMask,
                    iter->Relationship, GetName(iter->Relationship));
                break;
            default:
                Log("%zu: ProcessorMask=%zx Relationship=%d(%s) Reserved=[%llx][%llx]", iter - Buffer,
                    iter->ProcessorMask, iter->Relationship, GetName(iter->Relationship), iter->Reserved[0],
                    iter->Reserved[1]);
                break;
        }
    }
#endif
}

static LogLogicalProcessorInformationEx(const char* name,
                                        LOGICAL_PROCESSOR_RELATIONSHIP Relationship,
                                        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX Buffer,
                                        DWORD bytes)
{
#if PROCINFO_LOGGING
    Log("LogicalProcessorInformationEx (%s) - Relationship=%d - %u bytes", name, Relationship, bytes);
    size_t i = 0;
    BYTE* const end = (BYTE*)Buffer + bytes;
    for (BYTE* p = (BYTE*)Buffer; p < end; ++i)
    {
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX iter = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)p;
        p += iter->Size;
        switch (iter->Relationship)
        {
            case RelationProcessorCore:
            case RelationProcessorDie:
            case RelationProcessorModule:
            case RelationProcessorPackage:
                Log("%zu: Relationship=%d(%s) Size=%u FLAGS=%x EfficiencyClass=%u GroupCount=%d", i, iter->Relationship,
                    GetName(iter->Relationship), iter->Size, iter->Processor.Flags, iter->Processor.EfficiencyClass,
                    iter->Processor.GroupCount);
                for (int j = 0; j < iter->Processor.GroupCount; ++j)
                    Log("    Group %d: Mask=%zx Group=%u", j, iter->Processor.GroupMask[j].Mask,
                        iter->Processor.GroupMask[j].Group);
                break;

            case RelationNumaNode:
            case RelationNumaNodeEx:
                Log("%zu: Relationship=%d(%s) Size=%u NodeNumber=%u GroupCount=%u", i, iter->Relationship,
                    GetName(iter->Relationship), iter->Size, iter->NumaNode.NodeNumber, iter->NumaNode.GroupCount);
                Log("    Group (0?): Mask=%zx Group=%u", iter->NumaNode.GroupMask.Mask, iter->NumaNode.GroupMask.Group);
                for (int j = 0; j < iter->NumaNode.GroupCount; ++j)
                    Log("    Group %d: Mask=%zx Group=%u", j, iter->NumaNode.GroupMasks[j].Mask,
                        iter->NumaNode.GroupMasks[j].Group);
                break;

            case RelationCache:
                Log("%zu: Relationship=%d(%s) Size=%u Level=%u Associativity=%u LineSize=%u CacheSize=%u Type=%d GroupCount=%d",
                    i, iter->Relationship, GetName(iter->Relationship), iter->Size, iter->Cache.Level,
                    iter->Cache.Associativity, iter->Cache.LineSize, iter->Cache.CacheSize, iter->Cache.Type,
                    iter->Cache.GroupCount);
                Log("    Group (0?): Mask=%zx Group=%u", iter->Cache.GroupMask.Mask, iter->Cache.GroupMask.Group);
                for (int j = 0; j < iter->Cache.GroupCount; ++j)
                    Log("    Group %d: Mask=%zx Group=%u", j, iter->Cache.GroupMasks[j].Mask,
                        iter->Cache.GroupMasks[j].Group);
                break;

            case RelationGroup:
                Log("%zu: Relationship=%d(%s) Size=%u MaximumGroupCount=%d ActiveGroupCount=%d", i, iter->Relationship,
                    GetName(iter->Relationship), iter->Size, iter->Group.MaximumGroupCount, iter->Group.ActiveGroupCount);
                for (int j = 0; j < iter->Group.ActiveGroupCount; ++j)
                    Log("    Group %d: MaximumProcessorCount = %u ActiveProcessorCount = %u ActiveProcessorMask=%zx", j,
                        iter->Group.GroupInfo[j].MaximumProcessorCount, iter->Group.GroupInfo[j].ActiveProcessorCount,
                        iter->Group.GroupInfo[j].ActiveProcessorMask);
                break;
            default:
                Log("%zu: Relationship=%d(%s) Size=%u (unknown)", i, iter->Relationship, GetName(iter->Relationship),
                    iter->Size);
                break;
        }
    }
#endif
}


static SRWLOCK CPUInfoLock = SRWLOCK_INIT;

// Information from GetLogicalProcessorInformation is cached once and always the same (CPUInfoLock is hold only for
// creation/destruction).
static PSYSTEM_LOGICAL_PROCESSOR_INFORMATION CachedCPUInfo;
static DWORD CachedCPUInfoCount;

// Information from GetLogicalProcessorInformationEx can change depending on the requested relationship, and therefore
// will be cached based on the requested relationship (CPUInfoLock must be held for use).
static LOGICAL_PROCESSOR_RELATIONSHIP CachedRelationship = (LOGICAL_PROCESSOR_RELATIONSHIP)-1;
static PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX CachedCPUInfoEx;
static DWORD CachedCPUInfoExBytes;

// Request info from GetLogicalProcessorInformation and cache/filter it for our fake number of CPUs
static BOOL CacheCPUInfo()
{
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION buf, write, read, end;
    DWORD length = 0;

    if (OrigGetLogicalProcessorInformation(NULL, &length) || GetLastError() != ERROR_INSUFFICIENT_BUFFER)
    {
        Log("CacheCPUInfo: GetLogicalProcessorInformation failed GLE=%u", GetLastError());
        return FALSE;
    }

    buf = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION)_alloca(length);
    if (!OrigGetLogicalProcessorInformation(buf, &length))
    {
        Log("CacheCPUInfo: GetLogicalProcessorInformation failed GLE=%u", GetLastError());
        return FALSE;
    }

    LogLogicalProcessorInformation("Before processing", buf, length / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));

    // Only allow CPUs that are within our mask
    end = buf + (length / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
    write = read = buf;

    // Walk through all of the returned data and filter it. Any entries that are about CPUs not in our limited set are
    // culled. Entries that do reference our limited set are trimmed down to ensure that it's *only* about our set.
    for (; read < end; ++read)
    {
        if (read->ProcessorMask & kCpuMask)
        {
            read->ProcessorMask &= kCpuMask;
            if (read != write)
            {
                memcpy(write, read, sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
            }
            ++write;
        }
    }

    LogLogicalProcessorInformation("After processing", buf, (DWORD)(write - buf));

    AcquireSRWLockExclusive(&CPUInfoLock);
    if (!CachedCPUInfo)
    {
        CachedCPUInfo =
            (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)write - (SIZE_T)buf);
        if (!CachedCPUInfo)
        {
            ReleaseSRWLockExclusive(&CPUInfoLock);
            return FALSE;
        }

        CachedCPUInfoCount = (DWORD)(write - buf);
        memcpy(CachedCPUInfo, buf, CachedCPUInfoCount * sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
    }

    ReleaseSRWLockExclusive(&CPUInfoLock);
    return TRUE;
}

// Request info from GetLogicalProcessorInformationEx based on the given Relationship and cache/filter it for our fake
// number of CPUs
static BOOL CacheCPUInfoExLocked(LOGICAL_PROCESSOR_RELATIONSHIP Relationship)
{
    DWORD length = 0, size;
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX buf, read, write, next, end;

    if (CachedCPUInfoEx)
    {
        HeapFree(GetProcessHeap(), 0, CachedCPUInfoEx);
        CachedRelationship = (LOGICAL_PROCESSOR_RELATIONSHIP)-1;
        CachedCPUInfoEx = NULL;
        CachedCPUInfoExBytes = 0;
    }

    if (OrigGetLogicalProcessorInformationEx(Relationship, NULL, &length) || GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        return FALSE;

    buf = write = read = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)_alloca(length);
    if (!OrigGetLogicalProcessorInformationEx(Relationship, read, &length))
        return FALSE;

    LogLogicalProcessorInformationEx("Before processing", Relationship, read, length);

    // Walk through all of the returned data and filter it. We don't allow any processor groups above the first, and
    // that is trimmed down to our fake number of CPUs. Any entries that are about CPUs not in our limited set are
    // culled. Entries that do reference our limited set are trimmed down to ensure that it's *only* about our set.
    end = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)((PBYTE)read + length);
    for (; read < end; read = next)
    {
        next = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)((PBYTE)read + read->Size);

        switch (read->Relationship)
        {
            case RelationProcessorCore:
            case RelationProcessorDie:
            case RelationProcessorModule:
            case RelationProcessorPackage:
                if (read->Processor.GroupCount == 0)
                    continue;
                if (read->Processor.GroupCount > 1)
                    read->Processor.GroupCount = 1;
                if (!(read->Processor.GroupMask[0].Mask & kCpuMask))
                    continue;
                read->Processor.GroupMask[0].Mask &= kCpuMask;
                size = (DWORD)((PBYTE)&read->Processor.GroupMask[1] - (PBYTE)read);
                break;

            case RelationNumaNode:
            case RelationNumaNodeEx:
                if (read->NumaNode.GroupCount > 1)
                    read->NumaNode.GroupCount = 1;
                if (!(read->NumaNode.GroupMask.Mask & kCpuMask))
                    continue;
                read->NumaNode.GroupMask.Mask &= kCpuMask;
                size = (DWORD)((PBYTE)&read->NumaNode.GroupMasks[1] - (PBYTE)read);
                break;

            case RelationCache:
                if (read->Cache.GroupCount > 1)
                    read->Cache.GroupCount = 1;
                if (!(read->Cache.GroupMask.Mask & kCpuMask))
                    continue;
                read->Cache.GroupMask.Mask &= kCpuMask;
                size = (DWORD)((PBYTE)&read->Cache.GroupMasks[1] - (PBYTE)read);
                break;

            case RelationGroup:
                if (!read->Group.ActiveGroupCount)
                    continue;
                if (read->Group.MaximumGroupCount > 1)
                    read->Group.MaximumGroupCount = 1;
                if (read->Group.ActiveGroupCount > 1)
                    read->Group.ActiveGroupCount = 1;
                if (read->Group.GroupInfo[0].ActiveProcessorCount > kNumCpus)
                    read->Group.GroupInfo[0].ActiveProcessorCount = kNumCpus;
                if (read->Group.GroupInfo[0].MaximumProcessorCount > kNumCpus)
                    read->Group.GroupInfo[0].MaximumProcessorCount = kNumCpus;
                read->Group.GroupInfo[0].ActiveProcessorMask &= kCpuMask;
                size = (DWORD)((PBYTE)&read->Group.GroupInfo[1] - (PBYTE)read);
                break;

            default:
                // Skip unknown relationship types; these won't end up in the output data since we don't know how to
                // interpret them.
                continue;
        }

        // If we get here, we want to keep the entry, possibly truncating it to `size`
        if (write != read)
        {
            memmove(write, read, size);
        }
        write->Size = size;
        write = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)((PBYTE)write + size);
    }

    CachedCPUInfoExBytes = (DWORD)((PBYTE)write - (PBYTE)buf);
    CachedCPUInfoEx = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)HeapAlloc(GetProcessHeap(), 0, CachedCPUInfoExBytes);
    if (!CachedCPUInfoEx)
    {
        CachedCPUInfoExBytes = 0;
        return FALSE;
    }
    memcpy(CachedCPUInfoEx, buf, CachedCPUInfoExBytes);
    CachedRelationship = Relationship;

    LogLogicalProcessorInformationEx("After processing", Relationship, CachedCPUInfoEx, CachedCPUInfoExBytes);

    return TRUE;
}

static BOOL WINAPI MyGetLogicalProcessorInformation(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION Buffer, PDWORD ReturnedLength)
{
    static bool called;

    if (!called)
    {
        called = true;
        Log("GetLogicalProcessorInformation called at least once, first: (%p, %p)", Buffer, ReturnedLength);
    }

    if (!CachedCPUInfo && !CacheCPUInfo())
        return FALSE;

    if (!ReturnedLength)
    {
        // Do whatever the parent function does with bad input
        return OrigGetLogicalProcessorInformation(NULL, NULL);
    }

    if (!Buffer || *ReturnedLength < CachedCPUInfoCount * sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION))
    {
        *ReturnedLength = CachedCPUInfoCount * sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }

    *ReturnedLength = CachedCPUInfoCount * sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
    memcpy(Buffer, CachedCPUInfo, *ReturnedLength);
    return TRUE;
}

static BOOL WINAPI MyGetLogicalProcessorInformationEx(LOGICAL_PROCESSOR_RELATIONSHIP RelationshipType,
                                                      PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX Buffer,
                                                      PDWORD ReturnedLength)
{
    static bool called;

    if (!called)
    {
        called = true;
        Log("GetLogicalProcessorInformationEx called at least once, first: (%d, %p, %p)", RelationshipType, Buffer,
            ReturnedLength);
    }

    if (!ReturnedLength)
    {
        // Do whatever the parent function does with bad input
        return OrigGetLogicalProcessorInformationEx(RelationshipType, Buffer, ReturnedLength);
    }

    AcquireSRWLockExclusive(&CPUInfoLock);
    if (CachedRelationship != RelationshipType || !CachedCPUInfoEx)
    {
        if (!CacheCPUInfoExLocked(RelationshipType))
        {
            ReleaseSRWLockExclusive(&CPUInfoLock);
            return FALSE;
        }
    }

    if (!Buffer || *ReturnedLength < CachedCPUInfoExBytes)
    {
        *ReturnedLength = CachedCPUInfoExBytes;
        ReleaseSRWLockExclusive(&CPUInfoLock);
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    memcpy(Buffer, CachedCPUInfoEx, CachedCPUInfoExBytes);
    *ReturnedLength = CachedCPUInfoExBytes;
    ReleaseSRWLockExclusive(&CPUInfoLock);
    return TRUE;
}

bool g_filterInstalled;
static LPTOP_LEVEL_EXCEPTION_FILTER g_exceptionFilter;

static LONG MyExceptionFilter(struct _EXCEPTION_POINTERS* ExceptionInfo)
{
    if (ExceptionInfo->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
        Sleep(INFINITE);

    return g_exceptionFilter(ExceptionInfo);
}

static LPTOP_LEVEL_EXCEPTION_FILTER MySetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER filter)
{
    LPTOP_LEVEL_EXCEPTION_FILTER ret;

    if (!g_filterInstalled)
    {
        g_filterInstalled = true;
        g_exceptionFilter = OrigSetUnhandledExceptionFilter(MyExceptionFilter);
    }

    Log("SetUnhandledExceptionFilter(%p) -> %p", filter, g_exceptionFilter);
    ret = g_exceptionFilter;
    g_exceptionFilter = filter;

    return ret;
}

static HWND g_window;
U32 g_width, g_height;
float g_aspect;

typedef struct MyBink
{
    HBINK bink;
    U32 origWidth;
    U32 byteOffset;
    BINKFRAMEBUFFERS buffers;
} MyBink;

#define MAXBINK 8
static MyBink g_binks[MAXBINK];

static MyBink* getEmptyBink()
{
    for (int i = 0; i != MAXBINK; ++i)
        if (!g_binks[i].bink)
            return &g_binks[i];
    Log("Too many open binks!");
    return NULL;
}

static MyBink* findBink(HBINK bink)
{
    for (int i = 0; i != MAXBINK; ++i)
        if (g_binks[i].bink == bink)
            return &g_binks[i];
    Log("Bink %p not found", bink);
    return NULL;
}

static void freeBink(HBINK bink)
{
    MyBink* b = findBink(bink);
    if (b)
    {
        memset(b, 0, sizeof(MyBink));
    }
}

static HBINKBUFFER MyBinkBufferOpen(HWND wnd, uint32_t width, uint32_t height, BINKBUFFER_OPEN_FLAGS flags)
{
    HBINKBUFFER retval = OrigBinkBufferOpen(wnd, width, height, flags);

    {
        Log("BinKBufferOpen(%p, %u, %u, %llx) -> %p", wnd, width, height, flags, retval);
    }

    return retval;
}

static int32_t MyBinkBufferSetOffset(HBINKBUFFER hbuf, int32_t off_x, int32_t off_y)
{
    static bool called;
    int32_t retval;

    retval = OrigBinkBufferSetOffset(hbuf, off_x, off_y);

    // if (!called)
    {
        called = true;
        Log("BinkBufferSetOffset(%p, %d, %d) -> %d", hbuf, off_x, off_y);
    }

    return retval;
}

static int32_t MyBinkBufferSetScale(HBINKBUFFER hbuf, uint32_t width, uint32_t height)
{
    static bool called;
    int32_t retval;

    retval = OrigBinkBufferSetScale(hbuf, width, height);

    // if (!called)
    {
        called = true;
        Log("BinkBufferSetScale(%p, %u, %u) -> %d", hbuf, width, height);
    }

    return retval;
}

static BOOL CALLBACK EnumFunc(HWND hwnd, LPARAM lp)
{
    DWORD pid;
    RECT wnd = { 0 }, rect = { 0 };

    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId())
        return TRUE;

    GetWindowRect(hwnd, &wnd);
    GetClientRect(hwnd, &rect);
    if ((rect.right - rect.left) > 0 && (rect.bottom - rect.top) > 0)
    {
        g_window = hwnd;
        g_width = rect.right - rect.left;
        g_height = rect.bottom - rect.top;
        g_aspect = (float)g_width / (float)g_height;
    }
    Log("HWND: %p rect(%d,%d - %d,%d) client(%d,%d - %d,%d)%s", hwnd, wnd.left, wnd.top, wnd.right, wnd.bottom,
        rect.left, rect.top, rect.right, rect.bottom, hwnd == g_window ? " **SELECTED**" : "");

    return TRUE;
}

static HBINK MyBinkOpen(const char* name, BINK_OPEN_FLAGS flags)
{
    static bool called;
    HBINK retval;

    retval = OrigBinkOpen(name, flags);

    // if (!called)
    {
        called = true;
        Log("BinkOpen(\"%s\", 0x%x) -> %p", name, flags, retval);
    }

    if (retval)
    {
        EnumWindows(EnumFunc, 0);

        Log("  Width x Height=(%u x %u)", retval->Width, retval->Height);

        if (g_window)
        {
            float aspect = (float)retval->Width / (float)retval->Height;
            if ((g_aspect - aspect) > 0.1f) // 16:9 -> 16:10 (lower) is okay, but 16:9 -> 32:9 (higher) is not :P
            {
                MyBink* b = getEmptyBink();
                if (b)
                {
                    b->bink = retval;
                    b->origWidth = retval->Width;
                    // Lie about the width so we calculate larger frame buffers
                    retval->Width = (U32)((float)retval->Height * g_aspect);
                    Log("  Assigned to g_binks[%zu]: origWidth=%u aspect=%f g_aspect=%f; new width=%u", b - g_binks,
                        b->origWidth, aspect, g_aspect, retval->Width);
                }
            }
        }
    }

    return retval;
}

static void MyBinkClose(HBINK bink)
{
    OrigBinkClose(bink);
    freeBink(bink);
    Log("BinkClose(%p)", bink);
}

static int32_t MyBinkCopyToBuffer(
    HBINK bink, void* dest, int32_t pitch, uint32_t height, uint32_t x, uint32_t y, BINK_COPY_FLAGS flags)
{
    static bool called;
    int32_t retval;

    retval = OrigBinkCopyToBuffer(bink, dest, pitch, height, x, y, flags);

    // if (!called)
    {
        called = true;
        Log("BinkCopyToBuffer(%p, %p, %d, %u, %u, %u, %llx) -> %d", bink, dest, pitch, height, x, y, flags, retval);
    }

    return retval;
}

static int32_t MyBinkCopyToBufferRect(HBINK bink,
                                      void* dest,
                                      int32_t pitch,
                                      uint32_t height,
                                      uint32_t x,
                                      uint32_t y,
                                      uint32_t src_x,
                                      uint32_t src_y,
                                      uint32_t src_w,
                                      uint32_t src_h,
                                      BINK_COPY_FLAGS flags)
{
    static bool called;
    int32_t retval;

    retval = OrigBinkCopyToBufferRect(bink, dest, pitch, height, x, y, src_x, src_y, src_w, src_h, flags);

    // if (!called)
    {
        called = true;
        Log("BinkCopyToBufferRect(%p, %p, %d, %u, %u, %u, %u, %u, %u, %u, %llx) -> %d", bink, dest, pitch, height, x, y,
            src_x, src_y, src_w, src_h, flags, retval);
    }

    return retval;
}

static void DumpFramebuffers(const BINKFRAMEBUFFERS* set)
{
    Log("  TotalFrames=%d YABufferWidth=%u YABufferHeight=%u cRcBBufferWidth=%u cRcBBufferHeight=%u FrameNum=%u",
        set->TotalFrames, set->YABufferWidth, set->YABufferHeight, set->cRcBBufferWidth, set->cRcBBufferHeight,
        set->FrameNum);
    if (set->TotalFrames <= BINKMAXFRAMEBUFFERS)
    {
        for (int32_t i = 0; i != set->TotalFrames; ++i)
        {
            const BINKFRAMEPLANESET* ps = set->Frames + i;
#define PLANE(a) a.Allocate, a.Buffer, a.BufferPitch
            Log("    %d: YPlane=(%d, %p, %u size=%u) cRPlane=(%d, %p, %u size=%u) cBPlane=(%d, %p, %u size=%u) APlane=(%d, %p, %u size=%u)",
                i, PLANE(ps->YPlane), ps->YPlane.BufferPitch * set->YABufferHeight, PLANE(ps->cRPlane),
                ps->cRPlane.BufferPitch * set->cRcBBufferHeight, PLANE(ps->cBPlane),
                ps->cBPlane.BufferPitch * set->cRcBBufferHeight, PLANE(ps->APlane),
                ps->APlane.BufferPitch * set->YABufferHeight);
#undef PLANE
        }
    }
    else
        Log("Invalid number of framenum");
}

BINKFRAMEBUFFERS g_buffers = { 0 };

static void ProtectBuffers(DWORD prot)
{
#if 0
    S32 const frames = min(g_buffers.TotalFrames, BINKMAXFRAMEBUFFERS);
    for (int i = 0; i != frames; ++i)
    {
        BINKFRAMEPLANESET* set = &g_buffers.Frames[i];
        SIZE_T size = set->YPlane.BufferPitch * g_buffers.YABufferHeight;
        U8* bytes = (U8*)set->YPlane.Buffer;
        DWORD prev;

        VirtualProtect((LPVOID)(((SIZE_T)bytes + 4095) & -4096ll), size & -4096ll, prot, &prev);
    }
#endif
}

static void MyBinkGetFrameBuffersInfo(HBINK bink, BINKFRAMEBUFFERS* set)
{
    MyBink* my = findBink(bink);
    OrigBinkGetFrameBuffersInfo(bink, set);
    if (my)
        memcpy(&my->buffers, set, sizeof(BINKFRAMEBUFFERS));
    Log("BinkGetFrameBuffersInfo(%p, %p)", bink, set);
    DumpFramebuffers(set);
    ProtectBuffers(PAGE_NOACCESS);
}

static void centerPlane(MyBink* my, BINKPLANE* plane, U32 width, U32 height)
{
    U32 scaledWidth;
    S32 offset;

    if (!plane->Buffer)
        return;

    scaledWidth = (U32)((float)height * g_aspect);
    offset = (S32)(scaledWidth - width) / 2;
    if (offset <= 0)
        return;

    plane->Buffer = (U8*)(plane->Buffer) + (offset & ~15); // must be 16-byte aligned
}

static void MyBinkRegisterFrameBuffers(HBINK bink, BINKFRAMEBUFFERS* set)
{
    MyBink* my = findBink(bink);
    if (my)
    {
        memcpy(&my->buffers, set, sizeof(BINKFRAMEBUFFERS));
        set = &my->buffers;
        Log("Before modification");
        DumpFramebuffers(set);

        // Clear the buffers to black
        for (int i = 0; i != set->TotalFrames; ++i)
        {
            // Black color from https://tvone.com/tech-support/faqs/120-ycrcb-values-for-various-colors is 16/128/128
            if (set->Frames[i].YPlane.Buffer)
            {
                memset(set->Frames[i].YPlane.Buffer, 16, set->Frames[i].YPlane.BufferPitch * set->YABufferHeight);
                centerPlane(my, &set->Frames[i].YPlane, set->YABufferWidth, set->YABufferHeight);
            }
            if (set->Frames[i].cRPlane.Buffer)
            {
                memset(set->Frames[i].cRPlane.Buffer, 128, set->Frames[i].cRPlane.BufferPitch * set->cRcBBufferHeight);
                centerPlane(my, &set->Frames[i].cRPlane, set->cRcBBufferWidth, set->cRcBBufferHeight);
            }
            if (set->Frames[i].cBPlane.Buffer)
            {
                memset(set->Frames[i].cBPlane.Buffer, 128, set->Frames[i].cBPlane.BufferPitch * set->cRcBBufferHeight);
                centerPlane(my, &set->Frames[i].cBPlane, set->cRcBBufferWidth, set->cRcBBufferHeight);
            }
            if (set->Frames[i].APlane.Buffer)
            {
                memset(set->Frames[i].APlane.Buffer, 0, set->Frames[i].APlane.BufferPitch * set->YABufferHeight);
                centerPlane(my, &set->Frames[i].APlane, set->YABufferWidth, set->YABufferHeight);
            }
        }
    }
    OrigBinkRegisterFrameBuffers(bink, set);
    Log("BinkRegisterFrameBuffers(%p, %p)", bink, set);
    DumpFramebuffers(set);
}

static S32 MyBinkDoFrame(HBINK bink)
{
    S32 retval;
    MyBink* my = findBink(bink);
    U32 width = bink->Width;
    if (my)
        bink->Width = my->origWidth;
    ProtectBuffers(PAGE_READWRITE);

    retval = OrigBinkDoFrame(bink);
    ProtectBuffers(PAGE_NOACCESS);

    if (my)
    {
        bink->Width = width;
    }
    return retval;
}

static void MyBinkNextFrame(HBINK bink)
{
    ProtectBuffers(PAGE_READWRITE);
    OrigBinkNextFrame(bink);
    ProtectBuffers(PAGE_NOACCESS);
}

static S32 MyBinkGetGPUDataBuffersInfo(HBINK bink, BINKGPUBUFFERS* out)
{
    S32 retval = OrigBinkGetGPUDataBuffersInfo(bink, out);
    Log("BinkGetGPUDataBuffersInfo(%p, %p) -> %d (%u %u %p %u %u %u %u)", bink, out, retval, out->unk1, out->unk2,
        out->buf, out->unk3, out->unk4, out->unk5, out->unk6);
    return retval;
}

static S32 MyBinkGetPlatformInfo(U32 param, U32* out)
{
    S32 retval = OrigBinkGetPlatformInfo(param, out);
    Log("BinkGetPlatformInfo(%u, %p = [%u]) -> %d", param, out, out && retval ? *out : 0, retval);
    return retval;
}

static void MyBinkBufferSetResolution(U32 width, U32 height, U32 color_depth)
{
    OrigBinkBufferSetResolution(width, height, color_depth);
    Log("BinkBufferSetResolution(%u, %u, %u)", width, height, color_depth);
}

static U32 MyBinkControlPlatformFeatures(U32 feature, U32 setting)
{
    U32 retval = OrigBinkControlPlatformFeatures(feature, setting);
    Log("BinkControlPlatformFeatures(%u, %u) -> %u", feature, setting, retval);
    return retval;
}

static void InstallDetours()
{
    LONG err;
    HINSTANCE hKernel32, hLocal;
    WCHAR buf[_MAX_PATH + 1];

    Log("InstallDetours");
    if ((err = DetourTransactionBegin()) != NO_ERROR)
        Log("DetourTransactionBegin failed: %d", err);
    hKernel32 = GetModuleHandleW(L"Kernel32.dll");
    if (!hKernel32)
    {
        Log("Failed to find Kernel32.dll");
        DetourTransactionAbort();
        return;
    }

    /*
    if (GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
    (LPCWSTR)&InstallDetours, &hLocal))
    {
        GetModuleFileNameW(hLocal, buf, _MAX_PATH + 1);
        SetDllDirectoryW()
    }

    SetDllDirectoryW()
    */
    g_bink = LoadLibraryW(L"bink2w64.dll");
    if (!g_bink)
        Log("Failed to load bink, GLE=%u", GetLastError());
    else
        Log("Loaded Bink library at %p", g_bink);

#define HOOK(fn, mod)                                                                                                  \
    if (!(Orig##fn = (fn##_t)GetProcAddress(mod, #fn)))                                                                \
        Log("Failed to find " #fn);                                                                                    \
    if (Orig##fn && (err = DetourAttach((PVOID*)&Orig##fn, (void*)My##fn)) != NO_ERROR)                                \
    Log("DetourAttach(" #fn ") failed: %d", err)

    HOOK(GetSystemInfo, hKernel32);
    HOOK(GetNativeSystemInfo, hKernel32);
    HOOK(GetProcessAffinityMask, hKernel32);
    HOOK(SetProcessAffinityMask, hKernel32);
    HOOK(SetThreadAffinityMask, hKernel32);
    HOOK(GetProcessGroupAffinity, hKernel32);
    HOOK(GetThreadGroupAffinity, hKernel32);
    HOOK(SetThreadGroupAffinity, hKernel32);
    HOOK(SetThreadIdealProcessor, hKernel32);
    HOOK(SetThreadIdealProcessorEx, hKernel32);
    HOOK(GetLogicalProcessorInformation, hKernel32);
    HOOK(GetLogicalProcessorInformationEx, hKernel32);
    HOOK(SetUnhandledExceptionFilter, hKernel32);

    HOOK(BinkBufferOpen, g_bink);
    HOOK(BinkBufferSetOffset, g_bink);
    HOOK(BinkBufferSetScale, g_bink);
    HOOK(BinkOpen, g_bink);
    HOOK(BinkClose, g_bink);
    HOOK(BinkCopyToBuffer, g_bink);
    HOOK(BinkCopyToBufferRect, g_bink);
    HOOK(BinkGetFrameBuffersInfo, g_bink);
    HOOK(BinkRegisterFrameBuffers, g_bink);
    HOOK(BinkDoFrame, g_bink);
    HOOK(BinkNextFrame, g_bink);
    OrigBinkGetSummary = (BinkGetSummary_t)GetProcAddress(g_bink, "BinkGetSummary");
    HOOK(BinkGetGPUDataBuffersInfo, g_bink);
    HOOK(BinkGetPlatformInfo, g_bink);
    HOOK(BinkBufferSetResolution, g_bink);
    HOOK(BinkControlPlatformFeatures, g_bink);

    if ((err = DetourTransactionCommit()) != NO_ERROR)
        Log("DetourTransactionCommit failed: %d", err);
    installed = true;
}

static void RestoreDetours()
{
    Log("RestoreDetours installed=%s", installed ? "true" : "false");
    if (!installed)
        return;

#define UNHOOK(fn)                                                                                                     \
    if (Orig##fn)                                                                                                      \
    DetourDetach((PVOID*)&Orig##fn, (void*)My##fn)

    DetourTransactionBegin();

    UNHOOK(GetSystemInfo);
    UNHOOK(GetNativeSystemInfo);
    UNHOOK(GetProcessAffinityMask);
    UNHOOK(SetProcessAffinityMask);
    UNHOOK(SetThreadAffinityMask);
    UNHOOK(GetProcessGroupAffinity);
    UNHOOK(GetThreadGroupAffinity);
    UNHOOK(SetThreadGroupAffinity);
    UNHOOK(SetThreadIdealProcessor);
    UNHOOK(SetThreadIdealProcessorEx);
    UNHOOK(GetLogicalProcessorInformation);
    UNHOOK(GetLogicalProcessorInformationEx);
    UNHOOK(SetUnhandledExceptionFilter);

    UNHOOK(BinkBufferOpen);
    UNHOOK(BinkBufferSetOffset);
    UNHOOK(BinkBufferSetScale);
    UNHOOK(BinkOpen);
    UNHOOK(BinkClose);
    UNHOOK(BinkCopyToBuffer);
    UNHOOK(BinkCopyToBufferRect);
    UNHOOK(BinkGetFrameBuffersInfo);
    UNHOOK(BinkRegisterFrameBuffers);
    UNHOOK(BinkDoFrame);
    UNHOOK(BinkNextFrame);
    OrigBinkGetSummary = NULL;
    UNHOOK(BinkGetGPUDataBuffersInfo);
    UNHOOK(BinkGetPlatformInfo);
    UNHOOK(BinkBufferSetResolution);
    UNHOOK(BinkControlPlatformFeatures);

    DetourTransactionCommit();

    // Clean up cached logical processor info
    AcquireSRWLockExclusive(&CPUInfoLock);
    if (CachedCPUInfo)
    {
        HeapFree(GetProcessHeap(), 0, CachedCPUInfo);
        CachedCPUInfo = NULL;
        CachedCPUInfoCount = 0;
    }
    if (CachedCPUInfoEx)
    {
        HeapFree(GetProcessHeap(), 0, CachedCPUInfoEx);
        CachedRelationship = (LOGICAL_PROCESSOR_RELATIONSHIP)-1;
        CachedCPUInfoEx = NULL;
        CachedCPUInfoExBytes = 0;
    }
    ReleaseSRWLockExclusive(&CPUInfoLock);

    installed = false;
}

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD dwReason, LPVOID reserved)
{
    if (DetourIsHelperProcess())
        return TRUE;

    if (dwReason == DLL_PROCESS_ATTACH)
    {
        HMODULE out;

        DetourRestoreAfterWith();

        // Pin our module so that we stay loaded for the length of the process.
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCWSTR)&DllMain, &out);

        InstallDetours();
    }
    else if (dwReason == DLL_PROCESS_DETACH)
    {
        RestoreDetours();
    }
    return TRUE;
}
