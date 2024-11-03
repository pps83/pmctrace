/* ========================================================================

   (C) Copyright 2024 by Molly Rocket, Inc., All Rights Reserved.
   
   This software is provided 'as-is', without any express or implied
   warranty. In no event will the authors be held liable for any damages
   arising from the use of this software.
   
   Please see https://computerenhance.com for more information
   
   ======================================================================== */

#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <intrin.h>
#include <windows.h>
#include <psapi.h>
#include <evntrace.h>
#include <evntcons.h>

#pragma comment (lib, "advapi32.lib")

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int32_t b32;

typedef float f32;
typedef double f64;

#define ArrayCount(Array) (sizeof(Array)/sizeof((Array)[0]))

#include "pmctrace.h"
#include "pmctrace.cpp"

extern "C" void CountNonZeroesWithBranch(u64 Count, u8 *Data);
#pragma comment (lib, "pmctrace_test_asm")

struct thread_context
{
    HANDLE ThreadHandle;
    
    pmc_tracer *Tracer;
    
    u64 BufferCount;
    u64 NonZeroCount;
    
    pmc_trace_result BestResult;

    // NOTE(casey): The scratch space is in the thread_context rather than on the thread's stack
    // because if there is an error, the thread may exit before the tracer is finished writing back
    // results, which would lead to a crash - so the scratch targets must remain valid until after
    // the tracer's receiver thread exits.
    pmc_traced_region ScratchResults[32];
};

static DWORD CALLBACK TestThread(void *Arg)
{
    thread_context *Context = (thread_context *)Arg;
    pmc_tracer *Tracer = Context->Tracer;
    
    u64 BufferCount = Context->BufferCount;
    u64 NonZeroCount = Context->NonZeroCount;
    
    u8 *BufferData = (u8 *)VirtualAlloc(0, BufferCount, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
    if(BufferData)
    {
        for(u64 Index = 0; Index < NonZeroCount; ++Index)
        {
            u64 Random;
            while(_rdrand64_step(&Random) == 0) {}
            BufferData[Random % BufferCount] = 1;
        }
        
        Context->BestResult.TSCElapsed = (u64)-1ll;
        for(u32 Iteration = 0; NoErrors(Tracer) && (Iteration < 10); ++Iteration)
        {
            u32 BatchSize = ArrayCount(Context->ScratchResults);
            for(u32 BatchIndex = 0; NoErrors(Tracer) && (BatchIndex < BatchSize); ++BatchIndex)
            {
                pmc_traced_region *TracedThread = &Context->ScratchResults[BatchIndex];
                StartCountingPMCs(Tracer, TracedThread);
                CountNonZeroesWithBranch(BufferCount, BufferData);
                StopCountingPMCs(Tracer, TracedThread);
            }
            
            for(u32 BatchIndex = 0; BatchIndex < BatchSize; ++BatchIndex)
            {
                pmc_trace_result Result = GetOrWaitForResult(Tracer, &Context->ScratchResults[BatchIndex]);
                if(NoErrors(Tracer) && (Context->BestResult.TSCElapsed > Result.TSCElapsed))
                {
                    Context->BestResult = Result;
                }
            }
        }
    }
    else
    {
        printf("ERROR: Unable to allocate test memory\n");
    }
    
    return 0;
}

int main(void)
{
    printf("Looking for PMC names...\n");
    pmc_name_array SharedNameArray =
    {
        L"TotalIssues",
        L"BranchInstructions",
        L"BranchMispredictions",
    };
    
    pmc_name_array *UsedNames = &SharedNameArray;
    pmc_source_mapping PMCMapping = MapPMCNames(&SharedNameArray);
    if(IsValid(&PMCMapping))
    {
        pmc_tracer Tracer;
        
        printf("Starting trace...\n");
        StartTracing(&Tracer, &PMCMapping);
        
        thread_context Threads[16] = {};
        HANDLE ThreadHandles[ArrayCount(Threads)] = {};
        
        printf("Launching threads...\n");
        for(u32 ThreadIndex = 0; ThreadIndex < ArrayCount(ThreadHandles); ++ThreadIndex)
        {
            thread_context *Thread = Threads + ThreadIndex;
            Thread->Tracer = &Tracer;
            Thread->BufferCount = 64*1024*1024;
            Thread->NonZeroCount = ThreadIndex*8192;
            
            ThreadHandles[ThreadIndex] = CreateThread(0, 0, TestThread, Thread, 0, 0);
        }

        printf("Waiting for threads to complete...\n");
        WaitForMultipleObjects(ArrayCount(Threads), ThreadHandles, TRUE, INFINITE);
        
        if(NoErrors(&Tracer))
        {
            for(u32 ThreadIndex = 0; ThreadIndex < ArrayCount(ThreadHandles); ++ThreadIndex)
            {
                thread_context *Thread = Threads + ThreadIndex;
                pmc_trace_result BestResult = Thread->BestResult;
                
                printf("\nTHREAD %u - %llu non-zeroes:\n", ThreadIndex, Thread->NonZeroCount);
                printf("  %llu TSC elapsed / %llu iterations [%llu switch%s]\n",
                       BestResult.TSCElapsed, Thread->BufferCount, BestResult.ContextSwitchCount,
                       (BestResult.ContextSwitchCount != 1) ? "es" : "");
                for(u32 CI = 0; CI < BestResult.PMCCount; ++CI)
                {
                    printf("  %llu %S\n", BestResult.Counters[CI], UsedNames->Strings[CI]);
                }
            }
        }
        else
        {
            printf("ERROR: %s\n", GetErrorMessage(&Tracer));
            printf("LOG:\n%s\n", GetDebugLog(&Tracer));
        }
        
        StopTracing(&Tracer);
    }
    else
    {
        printf("ERROR: Unable to find suitable ETW PMCs\n");
    }
    
    return 0;
}
