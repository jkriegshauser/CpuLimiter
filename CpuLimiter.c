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
// #define LOGGING 1
#if !defined LOGGING
#    ifdef NDEBUG
#        define LOGGING 0
#    else
#        define LOGGING 1
#    endif
#endif

#define OVERRIDE_BINK 1

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

// Needed BINK types follow
typedef int32_t S32;
typedef uint32_t U32;
typedef uint8_t U8;
#define RADLINK __stdcall
#define PTR4

typedef struct BINKPLANE
{
    S32 Allocate;
    void* Buffer;
    U32 BufferPitch;
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
    S32 TotalFrames;
    U32 YABufferWidth;
    U32 YABufferHeight;
    U32 cRcBBufferWidth;
    U32 cRcBBufferHeight;

    U32 FrameNum;
    BINKFRAMEPLANESET Frames[BINKMAXFRAMEBUFFERS];
} BINKFRAMEBUFFERS;

typedef struct BINK
{
    U32 Width;
    U32 Height;
    // Unnecessary fields follow
} BINK, *HBINK;

typedef U32 BINK_OPEN_FLAGS;

typedef HBINK (*BinkOpen_t)(const char* file_name, BINK_OPEN_FLAGS flags);
typedef void (*BinkClose_t)(HBINK bink);
typedef void (*BinkRegisterFrameBuffers_t)(HBINK bink, BINKFRAMEBUFFERS* set);

HMODULE g_bink;

static BinkOpen_t OrigBinkOpen;
static BinkClose_t OrigBinkClose;
static BinkRegisterFrameBuffers_t OrigBinkRegisterFrameBuffers;

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

static HWND g_window;
static LONG g_width, g_height;
static float g_aspect;

typedef struct MyBink
{
    HBINK bink;
    U32 origWidth;
    U32 byteOffset;
    BINKFRAMEBUFFERS buffers;
} MyBink;

#define MAXBINK 8
static MyBink g_binks[MAXBINK];

static MyBink* newMyBink()
{
    // Simple linear search
    for (int i = 0; i != MAXBINK; ++i)
        if (!g_binks[i].bink)
            return &g_binks[i];
    Log("Too many open binks!");
    return NULL;
}

static MyBink* findMyBink(HBINK bink)
{
    // Simple linear search
    for (int i = 0; i != MAXBINK; ++i)
        if (g_binks[i].bink == bink)
            return &g_binks[i];
    Log("Bink %p not found", bink);
    return NULL;
}

static void freeMyBink(HBINK bink)
{
    MyBink* b = findMyBink(bink);
    if (b)
    {
        memset(b, 0, sizeof(MyBink));
    }
}

static BOOL CALLBACK EnumFunc(HWND hwnd, LPARAM lp)
{
    DWORD pid;
    LONG width, height;
    RECT wnd = { 0 }, rect = { 0 };

    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId())
        return TRUE;

    GetWindowRect(hwnd, &wnd);
    GetClientRect(hwnd, &rect);
    width = rect.right - rect.left;
    height = rect.bottom - rect.top;
    // Take the largest window
    if (width > g_width && height > g_height)
    {
        g_window = hwnd;
        g_width = width;
        g_height = height;
        g_aspect = (float)g_width / (float)g_height;
    }
    Log("HWND: %p rect(%d,%d - %d,%d) client(%d,%d - %d,%d)%s", hwnd, wnd.left, wnd.top, wnd.right, wnd.bottom,
        rect.left, rect.top, rect.right, rect.bottom, hwnd == g_window ? " **SELECTED**" : "");

    return TRUE;
}

static HBINK MyBinkOpen(const char* name, BINK_OPEN_FLAGS flags)
{
    HBINK retval;

    retval = OrigBinkOpen(name, flags);
    Log("BinkOpen(\"%s\", 0x%x) -> %p", name, flags, retval);

    if (retval)
    {
        Log("  Width x Height=(%u x %u)", retval->Width, retval->Height);

        if (!g_window)
        {
            // Find the window
            EnumWindows(EnumFunc, 0);
        }

        if (g_window)
        {
            float aspect = (float)retval->Width / (float)retval->Height;
            if ((g_aspect - aspect) > 0.1f) // 16:9 -> 16:10 (lower) is okay, but 16:9 -> 32:9 (higher) is not :P
            {
#if OVERRIDE_BINK
                MyBink* b = newMyBink();
                if (b)
                {
                    b->bink = retval;
                    b->origWidth = retval->Width;
                    // Lie about the width so we calculate larger frame buffers
                    retval->Width = (U32)((float)retval->Height * g_aspect);
                    Log("  Assigned to g_binks[%zu]: origWidth=%u aspect=%f g_aspect=%f; new width=%u", b - g_binks,
                        b->origWidth, aspect, g_aspect, retval->Width);
                }
#else
                Log("  BINK overriding disabled: origWidth=%u aspect=%f g_aspect=%f", retval->Width, aspect, g_aspect);
#endif
            }
        }
        else
        {
            Log("Suitable window not found!");
        }
    }

    return retval;
}

static void MyBinkClose(HBINK bink)
{
    OrigBinkClose(bink);
    freeMyBink(bink);
    Log("BinkClose(%p)", bink);
}

static void DumpFramebuffers(const BINKFRAMEBUFFERS* set)
{
#if LOGGING
    Log("  TotalFrames=%d YABufferWidth=%u YABufferHeight=%u cRcBBufferWidth=%u cRcBBufferHeight=%u FrameNum=%u",
        set->TotalFrames, set->YABufferWidth, set->YABufferHeight, set->cRcBBufferWidth, set->cRcBBufferHeight,
        set->FrameNum);
    if (set->TotalFrames <= BINKMAXFRAMEBUFFERS)
    {
        for (int32_t i = 0; i != set->TotalFrames; ++i)
        {
            const BINKFRAMEPLANESET* ps = set->Frames + i;
#    define PLANE(a) a.Allocate, a.Buffer, a.BufferPitch
            Log("    %d: YPlane=(%d, %p, %u size=%u) cRPlane=(%d, %p, %u size=%u) cBPlane=(%d, %p, %u size=%u) APlane=(%d, %p, %u size=%u)",
                i, PLANE(ps->YPlane), ps->YPlane.BufferPitch * set->YABufferHeight, PLANE(ps->cRPlane),
                ps->cRPlane.BufferPitch * set->cRcBBufferHeight, PLANE(ps->cBPlane),
                ps->cBPlane.BufferPitch * set->cRcBBufferHeight, PLANE(ps->APlane),
                ps->APlane.BufferPitch * set->YABufferHeight);
#    undef PLANE
        }
    }
    else
        Log("Invalid number of framenum");
#endif
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
    MyBink* my = findMyBink(bink);
    if (my)
    {
        // We have our own copy of the buffers. The pointer passed to Register must be valid as long as the HBINK exists
        memcpy(&my->buffers, set, sizeof(BINKFRAMEBUFFERS));
        set = &my->buffers;

        Log("Before modification");
        DumpFramebuffers(set);

        // Clear the buffers to black. YcRcB for black is 16/128/128
        // (https://tvone.com/tech-support/faqs/120-ycrcb-values-for-various-colors)
        // We also advance the framebuffer pointer via centerPlane() so that the video always appears centered
        for (int i = 0; i != set->TotalFrames; ++i)
        {
            if (set->Frames[i].YPlane.Buffer)
            {
                memset(set->Frames[i].YPlane.Buffer, 0, set->Frames[i].YPlane.BufferPitch * set->YABufferHeight);
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

    // Handle bink detours
    g_bink = LoadLibraryW(L"bink2w64.dll");
    if (!g_bink)
        Log("Failed to load bink, GLE=%u", GetLastError());
    else
        Log("Loaded Bink library at %p", g_bink);

    HOOK(BinkOpen, g_bink);
    HOOK(BinkClose, g_bink);
    HOOK(BinkRegisterFrameBuffers, g_bink);

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

    UNHOOK(BinkOpen);
    UNHOOK(BinkClose);
    UNHOOK(BinkRegisterFrameBuffers);

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
