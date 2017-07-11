/*BEGIN_LEGAL 
Intel Open Source License 

Copyright (c) 2002-2011 Intel Corporation. All rights reserved.
 
Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.  Redistributions
in binary form must reproduce the above copyright notice, this list of
conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.  Neither the name of
the Intel Corporation nor the names of its contributors may be used to
endorse or promote products derived from this software without
specific prior written permission.
 
THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE INTEL OR
ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
END_LEGAL */
/*
 *  This file contains an ISA-portable PIN tool for tracing memory accesses.
 */

#include <sys/types.h>
#include <sys/prctl.h>
#include <unistd.h>
#include <stdio.h>
#include "pin.H"
#include <chrono>
#include <numeric>
#include <sstream>
#include "galloc.h"
#include "libsim.h"
#include "log.h"

KNOB<INT32> KnobProcIdx(KNOB_MODE_WRITEONCE, "pintool",
        "procIdx", "0", "zsim process idx (internal)");

KNOB<INT32> KnobShmid(KNOB_MODE_WRITEONCE, "pintool",
        "shmid", "0", "SysV IPC shared memory id used when running in multi-process mode");


/* Global Variables */

GlobSimInfo* zinfo;

/* Per-process variables */
uint32_t procIdx;






FILE * trace;
std::chrono::high_resolution_clock::time_point start;

void SimInit(uint32_t shmid){
    zinfo = gm_calloc<GlobSimInfo>();
    for (uint32_t i = 0; i < 8; i++) {
        zinfo->cycles[i]=0;
        zinfo->FastForwardIns[i]=0;
        zinfo->phase[i]=0;
    } 
    zinfo->global_phase = 0;    
    zinfo->phaseLength = PHASE_LENGTH;
    zinfo->FastForward = FASTFORWARD;
    gm_set_glob_ptr(zinfo);
}


// Print a memory read record
VOID RecordMemRead(VOID * ip, VOID * addr, uint32_t id)
{

    if ( (zinfo->FastForward == false) || (zinfo->FastForwardIns[procIdx] >= FF_INS) ) {  // no longer in fast forwarding
        auto ttp = std::chrono::system_clock::now();
        std::chrono::duration<double> diff = ttp-start;
        fprintf(trace,"%lf %p: R %p\n", diff.count(), ip, addr);
        info("AF %d", zinfo->FastForwardIns[id]);
        fprintf(trace,"%lf : R %p\n", diff.count(), addr);
    }
}

// Print a memory write record
VOID RecordMemWrite(VOID * ip, VOID * addr, uint32_t id)
{
    if ( (zinfo->FastForward == false) || (zinfo->FastForwardIns[procIdx] >= FF_INS) ) {  // no longer in fast forwarding
        auto ttp = std::chrono::system_clock::now();
        std::chrono::duration<double> diff = ttp-start;
        info("AF %d", zinfo->FastForwardIns[id]);
        fprintf(trace,"%lf : W %p\n", diff.count(), addr);
    }
}

VOID UpdateGlobalPhase(){
    int i;
    for (i=1;i<numProcs;i++) {
       if (zinfo->phase[i]!=zinfo->phase[0]) return; 
    }
    zinfo->global_phase = zinfo->phase[0];
}


VOID weave(uint32_t id){
    while (zinfo->phase[id] > zinfo->global_phase) {
         UpdateGlobalPhase();
         usleep(100);
    }
    //weave work
    //
    //
    printf("[core %d]Enter Bound %d\n", id, zinfo->global_phase); 
}


VOID fastForward(uint32_t id){
    zinfo->FastForwardIns[id]++;
    if ( (zinfo->FastForward == true) && (zinfo->FastForwardIns[id] < FF_INS) ) {
    //    info("id %d: FF %d", id, zinfo->FastForwardIns[id]);
        PIN_RemoveInstrumentation();
    } 
    else {
        zinfo->cycles[id]++;
        if (zinfo->cycles[id]/PHASE_LENGTH > zinfo->phase[id]) {
            zinfo->phase[id]++; //Previous phase end; 
            weave(id); 
        }
    }
    //if ( (zinfo->FastForward == true) && (zinfo->FastForwardIns[id] >= FF_INS) ) {
    //    zinfo->FastForwardIns[id]++;
    //    info("id %d: AF %d", id, zinfo->FastForwardIns[id]);
    //} 
}



// Is called for every instruction and instruments reads and writes
VOID Instruction(INS ins, VOID *v)
{
    // Instruments memory accesses using a predicated call, i.e.
    // the instrumentation is called iff the instruction will actually be executed.
    //
    // The IA-64 architecture has explicitly predicated instructions. 
    // On the IA-32 and Intel(R) 64 architectures conditional moves and REP 
    // prefixed instructions appear as predicated instructions in Pin.
    // 

    INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)fastForward,IARG_UINT32,  procIdx, IARG_END);


    UINT32 memOperands = INS_MemoryOperandCount(ins);

    // Iterate over each memory operand of the instruction.
    for (UINT32 memOp = 0; memOp < memOperands; memOp++)
    {
        if (INS_MemoryOperandIsRead(ins, memOp))
        {
            INS_InsertPredicatedCall(
                ins, IPOINT_BEFORE, (AFUNPTR)RecordMemRead,
                IARG_INST_PTR,
                IARG_MEMORYOP_EA, memOp,
                IARG_UINT32, procIdx,
                IARG_END);
        }
        // Note that in some architectures a single memory operand can be 
        // both read and written (for instance incl (%eax) on IA-32)
        // In that case we instrument it once for read and once for write.
        if (INS_MemoryOperandIsWritten(ins, memOp))
        {
            INS_InsertPredicatedCall(
                ins, IPOINT_BEFORE, (AFUNPTR)RecordMemWrite,
                IARG_INST_PTR,
                IARG_MEMORYOP_EA, memOp,
                IARG_UINT32,  procIdx,
                IARG_END);
        }
    }
}

VOID Fini(INT32 code, VOID *v)
{
    fprintf(trace, "#eof\n");
    fclose(trace);
}

/* ===================================================================== */
/* Print Help Message                                                    */
/* ===================================================================== */
   
INT32 Usage()
{
    PIN_ERROR( "This Pintool prints a trace of memory addresses\n" 
              + KNOB_BASE::StringKnobSummary() + "\n");
    return -1;
}

/* ===================================================================== */
/* Main                                                                  */
/* ===================================================================== */

int main(int argc, char *argv[])
{
    start = std::chrono::high_resolution_clock::now();
    PIN_InitSymbols();
    if (PIN_Init(argc, argv)) return Usage();

    procIdx = KnobProcIdx.Value();
    char header[64];
    snprintf(header, sizeof(header), "[S %d] ", procIdx);
    std::stringstream logfile_ss;
    logfile_ss << "./libsim.log." << procIdx;
    InitLog(header, logfile_ss.str().c_str());

    if (prctl(PR_SET_PDEATHSIG, 9 /*SIGKILL*/) != 0) {
        panic("prctl() failed");
    }

    info("Started instance %d", KnobProcIdx.Value());

    gm_attach(KnobShmid.Value());

    //gm_attach();
    //bool masterProcess = false;
    if (procIdx == 0 && !gm_isready()) {  // process 0 can exec() without fork()ing first, so we must check gm_isready() to ensure we don't initialize twice
        //masterProcess = true;
        SimInit(KnobShmid.Value());
    } else {
        while (!gm_isready()) usleep(1000);  // wait till proc idx 0 initializes everything
        zinfo = static_cast<GlobSimInfo*>(gm_get_glob_ptr());
        info("PHASE_LENGTH %d", zinfo->phaseLength);
        info("Core %d, inst %lu", procIdx, zinfo->core[procIdx].getInstrs());
    } 


    pid_t cur = getpid();
    char pid_s[15];
    sprintf(pid_s, "libsim_%d.out", cur);

    trace = fopen(pid_s, "w");

    INS_AddInstrumentFunction(Instruction, 0);
    PIN_AddFiniFunction(Fini, 0);

    // Never returns
    PIN_StartProgram();
    
    return 0;
}
