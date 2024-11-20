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

int main(void)
{
    pmc_name_array AMDNameArray =
    {
        L"TotalIssues",
        L"BranchMispredictions",
        L"DcacheMisses",
        L"IcacheMisses",
    };

    pmc_name_array IntelNameArray =
    {
        L"TotalIssues",
        L"UnhaltedCoreCycles",
        L"BranchInstructions",
        L"BranchMispredictions",
    };

    printf("Looking for AMD PMCs...\n");
    pmc_name_array *UsedNames = &AMDNameArray;
    pmc_source_mapping PMCMapping = MapPMCNames(&AMDNameArray);
    if(!IsValid(&PMCMapping))
    {
        printf("Looking for Intel PMCs...\n");
        UsedNames = &IntelNameArray;
        PMCMapping = MapPMCNames(&IntelNameArray);
    }

    //
    // NOTE(casey): Collect PMCs
    //

    if(IsValid(&PMCMapping))
    {
        pmc_tracer Tracer;

        printf("Starting trace...\n");
        StartTracing(&Tracer, &PMCMapping);

        pmc_traced_region Region[2];

        StartCountingPMCs(&Tracer, &Region[0]);
        printf("... This printf is measured only by Region[0].\n");
        StartCountingPMCs(&Tracer, &Region[1]);
        printf("... This printf is measured by both.\n");
        StopCountingPMCs(&Tracer, &Region[0]);
        StopCountingPMCs(&Tracer, &Region[1]);

        printf("Getting results...\n");
        for(u32 ResultIndex = 0; ResultIndex < ArrayCount(Region); ++ResultIndex)
        {
            pmc_trace_result Result = GetOrWaitForResult(&Tracer, &Region[ResultIndex]);
            if(NoErrors(&Tracer))
            {
                printf("\n%llu TSC elapsed [%llu context switch%s]\n",
                       Result.TSCElapsed, Result.ContextSwitchCount,
                       (Result.ContextSwitchCount != 1) ? "es" : "");
                for(u32 CI = 0; CI < Result.PMCCount; ++CI)
                {
                    printf("  %llu %S\n", Result.Counters[CI], UsedNames->Strings[CI]);
                }
            }
            else
            {
                printf("ERROR: %s\n", GetErrorMessage(&Tracer));
                printf("LOG:\n%s\n", GetDebugLog(&Tracer));
                break;
            }
        }

        printf("Stopping trace...\n");
        StopTracing(&Tracer);
    }
    else
    {
        printf("ERROR: Unable to find suitable ETW PMCs\n");
    }

    return 0;
}
