/* ========================================================================

   (C) Copyright 2024 by Molly Rocket, Inc., All Rights Reserved.

   This software is provided 'as-is', without any express or implied
   warranty. In no event will the authors be held liable for any damages
   arising from the use of this software.

   Please see https://computerenhance.com for more information

   ======================================================================== */

#if !defined(PMC_DEBUG_LOG)
#define PMC_DEBUG_LOG 0
#endif

#if PMC_DEBUG_LOG
#define DEBUG_PRINT(Format, ...) \
{ \
    u64 Max = Tracer->LogEnd - Tracer->LogAt; \
    u64 Temp = snprintf(Tracer->LogAt, Max, Format, __VA_ARGS__); \
    if((Temp > 0) && (Temp < Max)) {Tracer->LogAt += Temp;} \
}
#else
#define DEBUG_PRINT(...)
#endif

enum trace_marker_type : u32
{
    TraceMarker_None,

    TraceMarker_Open,
    TraceMarker_Close,

    TraceMarker_Count,
};

struct win32_trace_description
{
    EVENT_TRACE_PROPERTIES_V2 Properties;
    WCHAR Name[1024];
};

struct pmc_tracer_etw_marker_userdata
{
    u64 TraceKey;
    pmc_traced_region *Dest;
};
struct pmc_tracer_etw_marker
{
    EVENT_TRACE_HEADER Header;
    pmc_tracer_etw_marker_userdata UserData;
};

struct etw_thread_switch_userdata
{
    DWORD NewThreadId;
    DWORD OldThreadId;
};

struct pmc_tracer_cpu
{
    pmc_traced_region *FirstRunningRegion;
    pmc_traced_region *WaitingForSysExitToStart;

    u64 LastSysEnterCounters[MAX_TRACE_PMC_COUNT];
    u64 LastSysEnterTSC;
    b32 LastSysEnterValid;
};

#define PMC_TRACE_RESULT_MASK 0xff
struct pmc_tracer
{
    win32_trace_description Win32TraceDesc;
    TRACEHANDLE MarkerRegistrationHandle;
    TRACEHANDLE TraceHandle;
    TRACEHANDLE TraceSession;
    HANDLE ProcessingThread;

    pmc_source_mapping Mapping;
    pmc_tracer_cpu *CPUs; // NOTE(casey): [CPUCount]
    pmc_traced_region *FirstSuspendedRegion;

    u32 CPUCount;

    b32 Error;
    char const *ErrorMessage;

    u64 TraceKey;

#if PMC_DEBUG_LOG
    char *Log;
    char *LogAt;
    char *LogEnd;
#endif
};

#define WIN32_TRACE_OPCODE_SWITCH_THREAD 36
#define WIN32_TRACE_OPCODE_SYSTEMCALL_ENTER 51
#define WIN32_TRACE_OPCODE_SYSTEMCALL_EXIT 52

static GUID Win32ThreadEventGuid = {0x3d6fa8d1, 0xfe05, 0x11d0, {0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c}};
static GUID Win32DPCEventGuid = {0xce1dbfb4, 0x137e, 0x4da6, {0x87, 0xb0, 0x3f, 0x59, 0xaa, 0x10, 0x2c, 0xbc}};

static GUID TraceMarkerProviderGuid = {0xb877a9af, 0x4155, 0x40f2, {0xa9, 0xba, 0x34, 0xbe, 0xdf, 0xaf, 0xd1, 0x22}};
static GUID TraceMarkerCategoryGuid = {0x5c96d7f7, 0xb1ea, 0x4fbe, {0x86, 0x55, 0xe0, 0x43, 0x1e, 0x23, 0x2e, 0x53}};

static void *Win32AllocateSize(u64 Size)
{
    void *Result = VirtualAlloc(0, Size, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
    return Result;
}

static void Win32Deallocate(void *Memory)
{
    if(Memory)
    {
        VirtualFree(Memory, 0, MEM_RELEASE);
    }
}

static b32 GUIDsAreEqual(GUID A, GUID B)
{
    __m128i Compare = _mm_cmpeq_epi8(_mm_loadu_si128((__m128i *)&A), _mm_loadu_si128((__m128i *)&B));
    int Mask = _mm_movemask_epi8(Compare);
    b32 Result = (Mask == 0xffff);
    return Result;
}

static b32 NoErrors(pmc_tracer *Tracer)
{
    b32 Result = !Tracer->Error;
    return Result;
}

static char const *GetErrorMessage(pmc_tracer *Tracer)
{
    char const *Result = Tracer->ErrorMessage;
    return Result;
}

static char const *GetDebugLog(pmc_tracer *Tracer)
{
    char const *Result = "(debug log not enabled - define PMC_DEBUG_LOG to 1 to enable)";
#if PMC_DEBUG_LOG
    Result = Tracer->Log;
#endif
    return Result;
}

static void TraceError(pmc_tracer *Tracer, char const *Message)
{
    DEBUG_PRINT("%s\n", Message);
    if(!Tracer->Error)
    {
        Tracer->Error = true;
        Tracer->ErrorMessage = Message;
    }
}

static void Win32FindPMCData(pmc_tracer *Tracer, EVENT_RECORD *Event, u32 PMCCount, u64 *PMCData)
{
    EVENT_EXTENDED_ITEM_PMC_COUNTERS *PMC = 0;
    u64 PMCDataSize = 0;
    u32 PMCPresent = 0;
    for(u32 EDIndex = 0; EDIndex < Event->ExtendedDataCount; ++EDIndex)
    {
        EVENT_HEADER_EXTENDED_DATA_ITEM *Item = Event->ExtendedData + EDIndex;
        if(Item->ExtType == EVENT_HEADER_EXT_TYPE_PMC_COUNTERS)
        {
            PMC = (EVENT_EXTENDED_ITEM_PMC_COUNTERS *)Item->DataPtr;
            PMCDataSize = Item->DataSize;
            ++PMCPresent;
        }
    }

    if(PMCDataSize != (sizeof(u64)*PMCCount))
    {
        TraceError(Tracer, "Unexpected PMC data size");
    }

    if(PMCPresent != 1)
    {
        TraceError(Tracer, "Unexpected PMC data count");
    }

    if(NoErrors(Tracer))
    {
        for(u32 PMCIndex = 0; PMCIndex < PMCCount; ++PMCIndex)
        {
            PMCData[PMCIndex] = PMC->Counter[PMCIndex];
        }
    }
}

static void ApplyPMCsAsOpen(pmc_traced_region *Region, u32 PMCCount, u64 *PMCData, u64 TSC)
{
    pmc_trace_result *Results = &Region->Results;

    for(u32 PMCIndex = 0; PMCIndex < PMCCount; ++PMCIndex)
    {
        Results->Counters[PMCIndex] -= PMCData[PMCIndex];
    }

    Results->TSCElapsed -= TSC;
}

static void ApplyPMCsAsClose(pmc_traced_region *Region, u32 PMCCount, u64 *PMCData, u64 TSC)
{
    pmc_trace_result *Results = &Region->Results;

    for(u32 PMCIndex = 0; PMCIndex < PMCCount; ++PMCIndex)
    {
        Results->Counters[PMCIndex] += PMCData[PMCIndex];
    }

    Results->TSCElapsed += TSC;
}

static void CALLBACK Win32ProcessETWEvent(EVENT_RECORD *Event)
{
    pmc_tracer *Tracer = (pmc_tracer *)Event->UserContext;

    GUID EventGUID = Event->EventHeader.ProviderId;
	UCHAR Opcode = Event->EventHeader.EventDescriptor.Opcode;
    u32 CPUID = GetEventProcessorIndex(Event);
    u32 PMCCount = Tracer->Mapping.PMCCount;
    u64 TSC = Event->EventHeader.TimeStamp.QuadPart;
    u64 PMCData[MAX_TRACE_PMC_COUNT] = {};

    if(CPUID < Tracer->CPUCount)
    {
        pmc_tracer_cpu *CPU = &Tracer->CPUs[CPUID];

        if(GUIDsAreEqual(EventGUID, TraceMarkerCategoryGuid))
        {
            pmc_tracer_etw_marker_userdata *Marker = (pmc_tracer_etw_marker_userdata *)Event->UserData;
            u64 MarkerKey = Marker->TraceKey;
            pmc_traced_region *Region = Marker->Dest;

            // NOTE(casey): Only process marker events if the keys match. If they don't match, they
            // are events that were inserted by another instance of the tracer, so we don't want
            // to accidentally start counting them as if they came from our own trace.
            if(Tracer->TraceKey == MarkerKey)
            {
                if(Opcode == TraceMarker_Open)
                {
                    DEBUG_PRINT("OPEN\n");

                    // NOTE(casey): Add this region to the list of regions running on this CPU core
                    Region->Next = CPU->FirstRunningRegion;
                    CPU->FirstRunningRegion = Region;

                    // NOTE(casey): Mark that this region will get its starting counter values from the next SysExit event
                    if(CPU->WaitingForSysExitToStart)
                    {
                        TraceError(Tracer, "Additional region opened on the same thread before SysExit event started the prior region");
                    }
                    CPU->WaitingForSysExitToStart = Region;
                }
                else if(Opcode == TraceMarker_Close)
                {
                    DEBUG_PRINT("CLOSE\n");

                    pmc_trace_result *Results = &Region->Results;

                    if(CPU->LastSysEnterValid)
                    {
                        // NOTE(casey): Apply the counters and TSC we saved from the preceeding SysEnter event
                        ApplyPMCsAsClose(Region, PMCCount, CPU->LastSysEnterCounters, CPU->LastSysEnterTSC);

                        CPU->LastSysEnterValid = false;
                    }
                    else
                    {
                        TraceError(Tracer, "No ENTER for CLOSE event");
                    }

                    // NOTE(casey): Remove this trace from the list of traces running on this CPU core
                    pmc_traced_region **FindRegion = &CPU->FirstRunningRegion;
                    while(*FindRegion)
                    {
                        if(*FindRegion == Region)
                        {
                            *FindRegion = Region->Next;
                            break;
                        }

                        FindRegion = &(*FindRegion)->Next;
                    }

                    // NOTE(casey): Make sure everything is written back before signaling completion
                    _mm_mfence(); // NOTE(casey): This is a stronger memory barrier than necessary, but should not be harmful

                    // NOTE(casey): Signal completion to anyone waiting for these results
                    Results->Completed = true;
                }
                else
                {
                    TraceError(Tracer, "Unrecognized ETW marker type");
                }
            }
        }
        else if(GUIDsAreEqual(EventGUID, Win32ThreadEventGuid))
        {
            if(Opcode == WIN32_TRACE_OPCODE_SWITCH_THREAD)
            {
                if(Event->UserDataLength == 24)
                {
                    etw_thread_switch_userdata *Switch = (etw_thread_switch_userdata *)Event->UserData;

                    // NOTE(casey): Get the PMC data once, since we may need it multiple times as we
                    // process suspending and resuming regions
                    if(CPU->FirstRunningRegion || Tracer->FirstSuspendedRegion)
                    {
                        Win32FindPMCData(Tracer, Event, PMCCount, PMCData);
                    }

                    // NOTE(casey): Suspend any existing regions running on this CPU core
                    while(CPU->FirstRunningRegion)
                    {
                        DEBUG_PRINT("SWITCH FROM\n");

                        pmc_traced_region *Region = CPU->FirstRunningRegion;

                        if(Switch->OldThreadId != Region->OnThreadID)
                        {
                            TraceError(Tracer, "Switched thread ID mismatch");
                        }

                        // NOTE(casey): Apply the current PMCs as "ending" counters
                        ApplyPMCsAsClose(Region, PMCCount, PMCData, TSC);

                        // NOTE(casey): Record that this region has incurred a context switch
                        ++Region->Results.ContextSwitchCount;

                        // NOTE(casey): Remove this region from the running set
                        CPU->FirstRunningRegion = Region->Next;

                        // NOTE(casey): Put this region on the suspended list
                        Region->Next = Tracer->FirstSuspendedRegion;
                        Tracer->FirstSuspendedRegion = Region;
                    }

                    // NOTE(casey): Look for matching region IDs that will be resumed
                    pmc_traced_region **FindRegion = &Tracer->FirstSuspendedRegion;
                    while(*FindRegion)
                    {
                        if((*FindRegion)->OnThreadID == Switch->NewThreadId)
                        {
                            DEBUG_PRINT("SWITCH TO\n");

                            pmc_traced_region *Region = *FindRegion;

                            // NOTE(casey): Apply the current PMCs as "begin" counters
                            ApplyPMCsAsOpen(Region, PMCCount, PMCData, TSC);

                            // NOTE(casey): Remove this region from the suspended list
                            *FindRegion = (*FindRegion)->Next;

                            // NOTE(casey): Put this region on the running list
                            Region->Next = CPU->FirstRunningRegion;
                            CPU->FirstRunningRegion = Region;
                        }
                        else
                        {
                            FindRegion = &(*FindRegion)->Next;
                        }
                    }
                }
                else
                {
                    TraceError(Tracer, "Unexpected CSwitch data size");
                }
            }
        }
        else if(GUIDsAreEqual(EventGUID, Win32DPCEventGuid))
        {
            if(Opcode == WIN32_TRACE_OPCODE_SYSTEMCALL_ENTER)
            {
                DEBUG_PRINT("ENTER\n");

                // NOTE(casey): Remember the state at this SysEnter so it can be applied to a
                // region later if there is a following Close event.
                if(CPU->FirstRunningRegion)
                {
                    CPU->LastSysEnterValid = true;
                    CPU->LastSysEnterTSC = TSC;
                    Win32FindPMCData(Tracer, Event, PMCCount, CPU->LastSysEnterCounters);
                }
            }
            else if(Opcode == WIN32_TRACE_OPCODE_SYSTEMCALL_EXIT)
            {
                DEBUG_PRINT("EXIT\n");

                // NOTE(casey): If there was a region waiting to open on the next syscall exit,
                // apply the PMCs to that region
                if(CPU->WaitingForSysExitToStart)
                {
                    pmc_traced_region *Region = CPU->WaitingForSysExitToStart;
                    CPU->WaitingForSysExitToStart = 0;

                    Win32FindPMCData(Tracer, Event, PMCCount, PMCData);
                    ApplyPMCsAsOpen(Region, PMCCount, PMCData, TSC);
                }
            }
        }
    }
    else
    {
        TraceError(Tracer, "Out-of-bounds CPUID in ETW event");
    }
}

static DWORD CALLBACK Win32ProcessEventThread(void *Arg)
{
    TRACEHANDLE Session = (TRACEHANDLE)Arg;
    ProcessTrace(&Session, 1, 0, 0);
    return 0;
}

static ULONG WINAPI ControlCallback(WMIDPREQUESTCODE, void *, ULONG *, void *)
{
    return ERROR_SUCCESS;
}

static b32 IsValid(pmc_source_mapping *Mapping)
{
    b32 Result = Mapping->Valid;
    return Result;
}

static pmc_source_mapping MapPMCNames(pmc_name_array *SourceNames)
{
    pmc_source_mapping Result = {};

    ULONG BufferSize;
    TraceQueryInformation(0, TraceProfileSourceListInfo, 0, 0, &BufferSize);
    BYTE *Buffer = (BYTE *)Win32AllocateSize(BufferSize);
    if(Buffer)
    {
        if(TraceQueryInformation(0, TraceProfileSourceListInfo, Buffer, BufferSize, &BufferSize) == ERROR_SUCCESS)
        {
            u32 FoundCount = 0;

            for(PROFILE_SOURCE_INFO *Info = (PROFILE_SOURCE_INFO *)Buffer;
                ;
                Info = (PROFILE_SOURCE_INFO *)((u8 *)Info + Info->NextEntryOffset))
            {
                for(u32 SourceNameIndex = 0; SourceNameIndex < ArrayCount(SourceNames->Strings); ++SourceNameIndex)
                {
                    wchar_t const *SourceString = SourceNames->Strings[SourceNameIndex];
                    if(SourceString)
                    {
                        u32 CheckMax = SourceNameIndex + 1;
                        if(Result.PMCCount < CheckMax)
                        {
                            Result.PMCCount = CheckMax;
                        }
                        if(lstrcmpW(Info->Description, SourceString) == 0)
                        {
                            Result.SourceIndex[SourceNameIndex] = Info->Source;
                            ++FoundCount;
                            break;
                        }
                    }
                }

                if(Info->NextEntryOffset == 0)
                {
                    break;
                }
            }

            Result.Valid = (Result.PMCCount == FoundCount);
        }
    }

    Win32Deallocate(Buffer);

    return Result;
}

static void SetTracePMCSources(pmc_tracer *Tracer, pmc_source_mapping *Mapping)
{
    ULONG Status = TraceSetInformation(Tracer->TraceHandle, TracePmcCounterListInfo,
                                       Mapping->SourceIndex, Mapping->PMCCount * sizeof(Mapping->SourceIndex[0]));
    if(Status != ERROR_SUCCESS)
    {
        TraceError(Tracer, "Unable to select PMCs");
    }

    CLASSIC_EVENT_ID EventIDs[] =
    {
        {Win32ThreadEventGuid, WIN32_TRACE_OPCODE_SWITCH_THREAD},
        {Win32DPCEventGuid, WIN32_TRACE_OPCODE_SYSTEMCALL_ENTER},
        {Win32DPCEventGuid, WIN32_TRACE_OPCODE_SYSTEMCALL_EXIT},
    };

    ULONG EventListStatus = TraceSetInformation(Tracer->TraceHandle, TracePmcEventListInfo, EventIDs, sizeof(EventIDs));
    if(EventListStatus != ERROR_SUCCESS)
    {
        TraceError(Tracer, "Unable to select events");
    }
}

static void Win32RegisterTraceMarker(pmc_tracer *Tracer)
{
    TRACE_GUID_REGISTRATION MarkerEventClassGuids[] = {(LPGUID)&TraceMarkerCategoryGuid, 0};
    ULONG Status = RegisterTraceGuids((WMIDPREQUEST)ControlCallback, 0, (LPGUID)&TraceMarkerProviderGuid,
                                      sizeof(MarkerEventClassGuids)/sizeof(TRACE_GUID_REGISTRATION),
                                      MarkerEventClassGuids,
                                      0, 0, &Tracer->MarkerRegistrationHandle);
    if(Status != ERROR_SUCCESS)
    {
        TraceError(Tracer, "ETW marker registration failed");
    }
}

static void Win32CreateTrace(pmc_tracer *Tracer, pmc_source_mapping *SourceMapping)
{
    const WCHAR TraceName[] = L"Win32PMCTrace";

    EVENT_TRACE_PROPERTIES_V2 *Props = &Tracer->Win32TraceDesc.Properties;
    Props->Wnode.BufferSize = sizeof(Tracer->Win32TraceDesc);
    Props->LoggerNameOffset = offsetof(win32_trace_description, Name);

    // NOTE(casey): Attempt to stop any existing orphaned trace from a previous run
    ControlTraceW(0, TraceName, (EVENT_TRACE_PROPERTIES *)Props, EVENT_TRACE_CONTROL_STOP);

    /* NOTE(casey): Attempt to start the trace. Note that the fields we care about MUST
       be filled in after the EVENT_TRACE_CONTROL_STOP ControlTraceW call, because
       that call will overwrite the properties! */
    Props->Wnode.ClientContext = 3;
    Props->Wnode.Flags = WNODE_FLAG_TRACED_GUID | WNODE_FLAG_VERSIONED_PROPERTIES;
    Props->LogFileMode = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_SYSTEM_LOGGER_MODE;
    Props->VersionNumber = 2;
    Props->EnableFlags = EVENT_TRACE_FLAG_CSWITCH | EVENT_TRACE_FLAG_NO_SYSCONFIG | EVENT_TRACE_FLAG_SYSTEMCALL;
    ULONG StartStatus = StartTraceW(&Tracer->TraceHandle, TraceName, (EVENT_TRACE_PROPERTIES*)Props);

    if(StartStatus != ERROR_SUCCESS)
    {
        TraceError(Tracer, "Unable to start trace - may occur if not run as admin");
    }

    Tracer->Mapping = *SourceMapping;
    if(IsValid(&Tracer->Mapping))
    {
        SetTracePMCSources(Tracer, &Tracer->Mapping);
    }
    else
    {
        TraceError(Tracer, "PMC source mapping failed");
    }

    EVENT_TRACE_LOGFILEW Log = {};
    Log.LoggerName = Tracer->Win32TraceDesc.Name;
    Log.EventRecordCallback = Win32ProcessETWEvent;
    Log.ProcessTraceMode = PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP | PROCESS_TRACE_MODE_REAL_TIME;
    Log.Context = Tracer;

    Tracer->TraceSession = OpenTraceW(&Log);
    if(Tracer->TraceSession == INVALID_PROCESSTRACE_HANDLE)
    {
        TraceError(Tracer, "Unable to open trace");
    }

    Tracer->ProcessingThread = CreateThread(0, 0, Win32ProcessEventThread, (void *)Tracer->TraceSession, 0, 0);
    if(Tracer->ProcessingThread == 0)
    {
        TraceError(Tracer, "Unable to create processing thread");
    }
}

static void StartTracing(pmc_tracer *Tracer, pmc_source_mapping *SourceMapping)
{
    *Tracer = {};

    Tracer->TraceKey = __rdtsc();

#if PMC_DEBUG_LOG
    u64 RequestedLogSize = 1024*1024*1024;
    Tracer->Log = Tracer->LogAt = (char *)Win32AllocateSize(RequestedLogSize);
    if(Tracer->Log)
    {
        Tracer->LogEnd = Tracer->Log + RequestedLogSize;
    }
#endif

    SYSTEM_INFO SysInfo = {};
    GetSystemInfo(&SysInfo);
    Tracer->CPUCount = SysInfo.dwNumberOfProcessors;

    Tracer->CPUs = (pmc_tracer_cpu *)Win32AllocateSize(Tracer->CPUCount * sizeof(pmc_tracer_cpu));
    if(Tracer->CPUs)
    {
        Win32RegisterTraceMarker(Tracer);
        Win32CreateTrace(Tracer, SourceMapping);
    }
    else
    {
        TraceError(Tracer, "Unable to allocate memory for CPU core tracking");
    }
}

static void StopTracing(pmc_tracer *Tracer)
{
    // TODO(casey): Try to verify that 0 is never a valid trace handle - it's unclear from the documentation
    if(Tracer->TraceHandle)
    {
        ControlTraceW(Tracer->TraceHandle, 0, (EVENT_TRACE_PROPERTIES *)&Tracer->Win32TraceDesc.Properties, EVENT_TRACE_CONTROL_STOP);
    }

    if(Tracer->TraceSession != INVALID_PROCESSTRACE_HANDLE)
    {
        CloseTrace(Tracer->TraceSession);
    }

    if(Tracer->ProcessingThread)
    {
        WaitForSingleObject(Tracer->ProcessingThread, INFINITE);
        CloseHandle(Tracer->ProcessingThread);
    }

    if(Tracer->MarkerRegistrationHandle)
    {
        UnregisterTraceGuids(Tracer->MarkerRegistrationHandle);
    }

#if PMC_DEBUG_LOG
    Win32Deallocate(Tracer->Log);
#endif
    Win32Deallocate(Tracer->CPUs);
}

static void StartCountingPMCs(pmc_tracer *Tracer, pmc_traced_region *ResultDest)
{
    pmc_tracer_etw_marker TraceMarker = {};
    TraceMarker.Header.Size = sizeof(TraceMarker);
    TraceMarker.Header.Flags = WNODE_FLAG_TRACED_GUID;
    TraceMarker.Header.Guid = TraceMarkerCategoryGuid;
    TraceMarker.Header.Class.Type = TraceMarker_Open;

    TraceMarker.UserData.TraceKey = Tracer->TraceKey;
    TraceMarker.UserData.Dest = ResultDest;

    /* TODO(casey): Is this necessary, or is it safe to pick up the thread index from the OPEN marker?
       If we never see an error where the open marker differs from the thread ID recorded here, then
       presumably this is not necessary, */
    ResultDest->OnThreadID = GetCurrentThreadId();
    ResultDest->Results = {};
    ResultDest->Results.PMCCount = Tracer->Mapping.PMCCount;

    if(TraceEvent(Tracer->TraceHandle, &TraceMarker.Header) != ERROR_SUCCESS)
    {
        TraceError(Tracer, "Unable to insert ETW open marker");
    }
}

static void StopCountingPMCs(pmc_tracer *Tracer, pmc_traced_region *ResultDest)
{
    pmc_tracer_etw_marker TraceMarker = {};
    TraceMarker.Header.Size = sizeof(TraceMarker);
    TraceMarker.Header.Flags = WNODE_FLAG_TRACED_GUID;
    TraceMarker.Header.Guid = TraceMarkerCategoryGuid;
    TraceMarker.Header.Class.Type = TraceMarker_Close;

    TraceMarker.UserData.TraceKey = Tracer->TraceKey;
    TraceMarker.UserData.Dest = ResultDest;

    /* TODO(casey): In some circumstances, I believe this can fail due to ETW's internal buffers being
       full. In that case, I _think_ it should be possible to mark the particular trace results as
       invalid, but keep trying to issue the TraceEvent, succeed, and then continune without having
       to error out of the entire run. However, I have not found a reliable repro case for this
       yet, so I haven't yet tried to implement such a recovery case. */
    if(TraceEvent(Tracer->TraceHandle, &TraceMarker.Header) != ERROR_SUCCESS)
    {
        TraceError(Tracer, "Unable to insert ETW close marker");
    }
}

static b32 IsComplete(pmc_traced_region *Region)
{
    b32 Result = Region->Results.Completed;
    return Result;
}

static pmc_trace_result GetOrWaitForResult(pmc_tracer *Tracer, pmc_traced_region *Region)
{
    while(NoErrors(Tracer) && !IsComplete(Region))
    {
        /* NOTE(casey): This is a spin-lock loop on purpose, because if there was a Sleep() in here
           or some other yield, it might cause Windows to demote this region, which we don't want.
           Ideally, we rarely spin here, because there are enough traces in flight to ensure that,
           whenever we check for results, there are some waiting, except perhaps at the very end of a
           batch. */

        _mm_pause();
    }

    _mm_mfence(); // NOTE(casey): This is a stronger memory barrier than necessary, but should not be harmful

    pmc_trace_result Result = Region->Results;
    return Result;
}