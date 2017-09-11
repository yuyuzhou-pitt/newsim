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
#include "native.h"
#include "kiln.h"
#include "nvmlog.h"
#include "epb.h"
#include "epb_bf.h"
#include "log.h"

KNOB<INT32> KnobProcIdx(KNOB_MODE_WRITEONCE, "pintool",
        "procIdx", "0", "zsim process idx (internal)");

KNOB<INT32> KnobShmid(KNOB_MODE_WRITEONCE, "pintool",
        "shmid", "0", "SysV IPC shared memory id used when running in multi-process mode");


/* Global Variables */


/* Per-process variables */
uint32_t procIdx;


FILE * trace;

std::chrono::high_resolution_clock::time_point start;


void atomic_add_timestamp(){
    futex_lock(&zinfo->lock);
    zinfo->timestamp++;
    futex_unlock(&zinfo->lock);
}

void atomic_add_persist_w(){
    futex_lock(&zinfo->lock);
    zinfo->persist_w++;
    futex_unlock(&zinfo->lock);
}

void atomic_add_persist_w_evict_nvc(){
    futex_lock(&zinfo->lock);
    zinfo->persist_w_evict_nvc++;
    futex_unlock(&zinfo->lock);
}

void SimInit(uint32_t shmid){
    zinfo = gm_calloc<GlobSimInfo>();
    futex_lock(&zinfo->lock); 
    zinfo->timestamp = 0; 
    zinfo->arch = ARCHITECTURE;

    for (uint32_t i = 0; i < 8; i++) {
        zinfo->cycles[i]=0;
        zinfo->FastForwardIns[i]=0;
        zinfo->phase[i]=0;
        zinfo->core[i].lastUpdateCycles = 0;
        zinfo->core[i].lastUpdateInstrs = 0;
        zinfo->persistent[i]=false;
        zinfo->tx_id[i] = 0;
        zinfo->nextPersistTrax[i]=0;
        zinfo->nextAvailablePBLine[i]=0;

// initial persistent buffer
        for (uint32_t j =0; j < PB_SIZE; j++) { 
            zinfo->pb[i][j].tx_id = -1;
            zinfo->pb[i][j].level = NONE;
            zinfo->pb[i][j].lineId = -1;      
            zinfo->pb[i][j].lineAddr = -1;
        }

// initialize L1 cache
        zinfo->l1cache[i].accLat = L1D_LATENCY;
        zinfo->l1cache[i].numSets = L1D_SIZE/ (64*L1D_WAYS); 
        for (uint32_t j =0; j<L1D_SIZE/64; j++) {
            zinfo->l1cache[i].array[j]=0;
            zinfo->l1cache[i].state[j]=I;
            zinfo->l1cache[i].ts[j]=0;
            zinfo->l1cache[i].pb_line[j]=-1;
            zinfo->l1cache[i].tx_id[j]=-1;
        }

// initialize L2 cache
        zinfo->l2cache[i].accLat = L2_LATENCY;
        zinfo->l2cache[i].numSets = L2_SIZE/ (64*L2_WAYS); 
        for (uint32_t j =0; j<L2_SIZE/64; j++) {
            zinfo->l2cache[i].array[j]=0;
            zinfo->l2cache[i].state[j]=I;
            zinfo->l2cache[i].ts[j]=0;
            zinfo->l2cache[i].pb_line[j]=-1;
            zinfo->l2cache[i].tx_id[j]=-1;
        }
    }

// initialize NVC cache
    zinfo->nvc.read_accLat = NVC_READ_LATENCY;
    zinfo->nvc.write_accLat = NVC_WRITE_LATENCY;
    zinfo->nvc.numSets = NVC_SIZE/(64*NVC_WAYS); 
    for (uint32_t j=0;j<NVC_SIZE/64;j++) {
        zinfo->nvc.procId[j]=-1;
        zinfo->nvc.array[j]=0;
        zinfo->nvc.state[j]=I;
        zinfo->nvc.ts[j]=0;
        zinfo->nvc.pb_line[j]=-1;
        zinfo->nvc.tx_id[j]=-1;
    }

// initialize DRAM cache
    zinfo->dram.accLat = DRAM_LATENCY;
    zinfo->dram.numSets = DRAM_SIZE/(64*DRAM_WAYS);
    for (uint32_t j=0;j<DRAM_SIZE/64;j++) {
        zinfo->dram.procId[j]=-1;
        zinfo->dram.array[j]=0;
        zinfo->dram.state[j]=I;
        zinfo->dram.ts[j]=0;
        zinfo->dram.pb_line[j]=-1;
        zinfo->dram.tx_id[j]=-1;
    }


// initialize NVM
    zinfo->nvm.read_accLat = NVM_READ_LATENCY;
    zinfo->nvm.write_accLat = NVM_WRITE_LATENCY;

 
    zinfo->global_phase = 0;    
    zinfo->phaseLength = PHASE_LENGTH;
    zinfo->FastForward = FASTFORWARD;


// initialize perfomrance counters. 
    for (uint32_t i = 0; i < 8; i++) { 
       zinfo->pc.l1_hGETS[i]=0;
       zinfo->pc.l1_hGETX[i]=0;
       zinfo->pc.l1_mGETS[i]=0;
       zinfo->pc.l1_mGETX[i]=0;
       zinfo->pc.l1_PUTS[i]=0;
       zinfo->pc.l1_PUTX[i]=0;

       zinfo->pc.l2_hGETS[i]=0;
       zinfo->pc.l2_hGETX[i]=0;
       zinfo->pc.l2_mGETS[i]=0;
       zinfo->pc.l2_mGETX[i]=0;
       zinfo->pc.l2_PUTS[i]=0;
       zinfo->pc.l2_PUTX[i]=0;
    }
    zinfo->pc.nvc_hGETS=0;
    zinfo->pc.nvc_hGETX=0;
    zinfo->pc.nvc_mGETS=0;
    zinfo->pc.nvc_mGETX=0;
    zinfo->pc.nvc_PUTS=0;
    zinfo->pc.nvc_PUTX=0;

    zinfo->pc.dram_hGETS=0;
    zinfo->pc.dram_hGETX=0;
    zinfo->pc.dram_mGETS=0;
    zinfo->pc.dram_mGETX=0;
    zinfo->pc.dram_PUTS=0;
    zinfo->pc.dram_PUTX=0;

    zinfo->pc.nvm_GETS=0;
    zinfo->pc.nvm_GETX=0;
    zinfo->pc.nvm_PUTS=0;
    zinfo->pc.nvm_PUTX=0;
    
    zinfo->nvc_to_dram_write = 0;
    zinfo->dram_to_nvc_write = 0;
    zinfo->nvc_to_nvm_write = 0;

    zinfo->persist_w_evict_nvc = 0; 
    zinfo->persist_w = 0;
    zinfo->mem_access = 0;

    zinfo->last_nvm_access = 0; 

    futex_unlock(&zinfo->lock); 
    gm_set_glob_ptr(zinfo);

}

void pb_info(){
    // print out persistent buffer
    info("Persistent Buffer");
    int cl; 
    for (uint32_t i = 0; i <  PB_SIZE; i++) {
       switch (zinfo->pb[0][i].level) {
           case CL1:
               cl = 1;
               break;  
           case CL2:
               cl = 2;
               break; 
           case CL3:
               cl = 3;
               break; 
           case CL4:
               cl = 4;
               break; 
            default: 
                cl = -1;
       }
       info("[%d] tx_id: %lu, level: %d, lineId %lu, lineAddr %lu",i, zinfo->pb[0][i].tx_id, cl, zinfo->pb[0][i].lineId, zinfo->pb[0][i].lineAddr); 
    }

}


void debug_info()
{
    //print content of l1, l2, nvc, dram

    char ch; 
    info("L1 ");
        for (uint32_t j =0; j<L1D_SIZE/64; j++) {
            switch (zinfo->l1cache[0].state[j]) { 
                case I:
                    ch = 'I';
                    break;
                case S: 
                    ch = 'S';
                    break;
                case E: 
                    ch = 'E';
                    break;
                case M:
                    ch = 'M';
                    break; 
                default: 
                    ch = 'I';
            }
            info("Addr:%lx, State: %c, ts:%lu" ,zinfo->l1cache[0].array[j], ch, zinfo->l1cache[0].ts[j]);
        }

    info("L2 ");
        for (uint32_t j =0; j<L2_SIZE/64; j++) {
            switch (zinfo->l2cache[0].state[j]) {
                case I:
                    ch = 'I';
                    break;
                case S:
                    ch = 'S';
                    break;
                case E:
                    ch = 'E';
                    break;
                case M:
                    ch = 'M';
                    break;
                default:
                    ch = 'I';
            }
            info("Addr:%lx, State: %c, ts:%lu" ,zinfo->l2cache[0].array[j], ch, zinfo->l2cache[0].ts[j]);
        }

// print NVC cache
    info("NVC ");
        for (uint32_t j =0; j<NVC_SIZE/64; j++) {
            switch (zinfo->nvc.state[j]) {
                case I:
                    ch = 'I';
                    break;
                case S:
                    ch = 'S';
                    break;
                case E:
                    ch = 'E';
                    break;
                case M:
                    ch = 'M';
                    break;
                default:
                    ch = 'I';
            }
            info("Addr:%lx, State: %c, ts:%lu" ,zinfo->nvc.array[j], ch, zinfo->nvc.ts[j]);
        }

//print DRAM cache
    info("DRAM ");
        for (uint32_t j =0; j<DRAM_SIZE/64; j++) {
            switch (zinfo->dram.state[j]) {
                case I:
                    ch = 'I';
                    break;
                case S:
                    ch = 'S';
                    break;
                case E:
                    ch = 'E';
                    break;
                case M:
                    ch = 'M';
                    break;
                default:
                    ch = 'I';
            }
            info("Addr:%lx, State: %c, ts:%lu" ,zinfo->dram.array[j], ch, zinfo->dram.ts[j]);
        }
}

void debug()
{
    //print content of l1, l2, nvc, dram

    char ch; 
    fprintf(trace, "L1 \n");
        for (uint32_t j =0; j<L1D_SIZE/64; j++) {
            switch (zinfo->l1cache[0].state[j]) { 
                case I:
                    ch = 'I';
                    break;
                case S: 
                    ch = 'S';
                    break;
                case E: 
                    ch = 'E';
                    break;
                case M:
                    ch = 'M';
                    break; 
                default: 
                    ch = 'I';
            }
            fprintf(trace,"Addr:%lx, State: %c, ts:%lu\n" ,zinfo->l1cache[0].array[j], ch, zinfo->l1cache[0].ts[j]);
        }

    fprintf(trace, "L2 \n");
        for (uint32_t j =0; j<L2_SIZE/64; j++) {
            switch (zinfo->l2cache[0].state[j]) {
                case I:
                    ch = 'I';
                    break;
                case S:
                    ch = 'S';
                    break;
                case E:
                    ch = 'E';
                    break;
                case M:
                    ch = 'M';
                    break;
                default:
                    ch = 'I';
            }
            fprintf(trace,"Addr:%lx, State: %c, ts:%lu\n" ,zinfo->l2cache[0].array[j], ch, zinfo->l2cache[0].ts[j]);
        }

// print NVC cache
    fprintf(trace, "NVC \n");
        for (uint32_t j =0; j<NVC_SIZE/64; j++) {
            switch (zinfo->nvc.state[j]) {
                case I:
                    ch = 'I';
                    break;
                case S:
                    ch = 'S';
                    break;
                case E:
                    ch = 'E';
                    break;
                case M:
                    ch = 'M';
                    break;
                default:
                    ch = 'I';
            }
            fprintf(trace,"Addr:%lx, State: %c, ts:%lu\n" ,zinfo->nvc.array[j], ch, zinfo->nvc.ts[j]);
        }

//print DRAM cache
    fprintf(trace, "DRAM \n");
        for (uint32_t j =0; j<DRAM_SIZE/64; j++) {
            switch (zinfo->dram.state[j]) {
                case I:
                    ch = 'I';
                    break;
                case S:
                    ch = 'S';
                    break;
                case E:
                    ch = 'E';
                    break;
                case M:
                    ch = 'M';
                    break;
                default:
                    ch = 'I';
            }
            fprintf(trace,"Addr:%lx, State: %c, ts:%lu\n" ,zinfo->dram.array[j], ch, zinfo->dram.ts[j]);
        }
    fprintf(trace, "\n");
}


// Print a memory read record
VOID RecordMemRead(VOID * ip, VOID * addr, uint32_t id)
{
    atomic_add_timestamp();
    zinfo->cycles[id]--;
    zinfo->core[id].lastUpdateCycles--;

    zinfo->mem_access++;
    if ((zinfo->FastForward == false) || (zinfo->FastForwardIns[procIdx] >= FF_INS)) {  // no longer in fast forwarding
//        info("AF %d", zinfo->FastForwardIns[id]);
//        fprintf(trace,"%lf : R %p\n", diff.count(), addr);
        //auto ttp = std::chrono::system_clock::now();
        //std::chrono::duration<double> diff = ttp-start;
        //fprintf(trace,"%lf : R %p\n", diff.count(),  addr);
        //info("%lf : R %p", diff.count(),  addr);
        //debug_info();
          MemReq req; 
          req.lineAddr = (Address) addr; 
          req.type = GETS;
          req.persistent = false;
          req.epoch_id = -1;
          req.pb_id = -1;
          req.cycle = zinfo->core[id].lastUpdateCycles; 
          zinfo->core[id].lastUpdateCycles = l1_access(id, req);
        //fprintf(trace, "current cycle : %lu\n", zinfo->core[id].lastUpdateCycles);
        //debug();

    }
}

// Print a memory write record
VOID RecordMemWrite(VOID * ip, VOID * addr, uint32_t id)
{

    atomic_add_timestamp();
    zinfo->cycles[id]--;
    zinfo->core[id].lastUpdateCycles--;
    if ( (zinfo->FastForward == false) || (zinfo->FastForwardIns[procIdx] >= FF_INS) ) {  // no longer in fast forwarding
        //auto ttp = std::chrono::system_clock::now();
        //std::chrono::duration<double> diff = ttp-start;
        zinfo->mem_access++;
        //fprintf(trace,"%lf : W %p\n", diff.count(), addr );
        //info("%lf : W %p", diff.count(), addr );
        //debug_info();
          MemReq req;
          req.lineAddr = (Address) addr; 
          req.type = GETX;
          if (req.lineAddr >= 140737488200000) req.persistent = false;
          else 
              req.persistent = zinfo->persistent[id];
          req.epoch_id = zinfo->tx_id[id];
          req.pb_id = -1; // new req
          req.cycle = zinfo->core[id].lastUpdateCycles;

          if (req.persistent == true) atomic_add_persist_w();
          zinfo->core[id].lastUpdateCycles = l1_access(id, req);

        //fprintf(trace, "current cycle : %lu\n", zinfo->core[id].lastUpdateCycles);
        //debug();


    }
}

VOID UpdateGlobalPhase(){
    int i;
    for (i=1;i<numProcs;i++) {
       if (zinfo->phase[i]!=zinfo->phase[0]) return; 
    }
    zinfo->global_phase = zinfo->phase[0];
    zinfo->weave = true;
}


VOID weave(uint32_t id){
    while (zinfo->phase[id] > zinfo->global_phase) {
         UpdateGlobalPhase();
         usleep(100);
    }
    //weave work
    //  calculate bandwidth nvc dram nvm 
    while (zinfo->weave == true) {
        if (id == 0) { 
            uint64_t nvc_dram_max = PHASE_LENGTH * DRAM_BANKS;
            uint64_t nvm_max = PHASE_LENGTH * NVM_BANKS;
        
            uint64_t nvc_to_dram_write = zinfo->nvc_to_dram_write;  //number of access nvc-> drm
            uint64_t dram_to_nvc_write = zinfo->dram_to_nvc_write; // number of write dram -> nvm
        
            uint64_t nvc_to_nvm_write = zinfo ->nvc_to_nvm_write; //number of write nvc->nvm
         
            uint64_t persist_w_evict_nvc = zinfo->persist_w_evict_nvc;
            uint64_t persist_w = zinfo->persist_w; 
            uint64_t mem_access = zinfo->mem_access; 

            double nvc_and_dram_bandwidth_percentage = (double)((nvc_to_dram_write * DRAM_LATENCY) + (dram_to_nvc_write * NVC_WRITE_LATENCY)) / (double)(nvc_dram_max);
            double nvc_to_dram_bandwidth_percentage = (double)(nvc_to_dram_write * DRAM_LATENCY) / (double)(nvc_dram_max);
            double dram_to_nvc_bandwidth_percentage = (double)(dram_to_nvc_write * NVC_WRITE_LATENCY) / (double)(nvc_dram_max);
            double nvc_to_nvm_bandwidth_percentage = (double)((nvc_to_nvm_write * NVM_WRITE_LATENCY)) /(double) (nvm_max);

                    
            printf("mem access %lu\n", mem_access);        
            printf("persist_w %lu\n", persist_w);        
            printf("persist_w_evict_nvc %lu\n", persist_w_evict_nvc);        

            printf("nvc_and_dram_bandwidth_percentage %f\n", nvc_and_dram_bandwidth_percentage);
            printf("nvc_to_dram_bandwidth_percentage %f\n",nvc_to_dram_bandwidth_percentage);
            printf("dram_to_nvc_bandwidth_percentage %f\n",dram_to_nvc_bandwidth_percentage);
            printf("nvc_to_nvm_bandwidth_percentage %f\n",nvc_to_nvm_bandwidth_percentage);
             
            // clear data
            zinfo->nvc_to_dram_write =0;
            zinfo->dram_to_nvc_write = 0;
            zinfo->nvc_to_nvm_write = 0;
        
            zinfo->persist_w_evict_nvc = 0;
            zinfo->persist_w = 0;
            zinfo->mem_access = 0;
            zinfo->weave =false; 
        }
        else usleep(100);
    }

    printf("[core %d]Finish Bound %d\n", id, zinfo->global_phase-1);
//
}


VOID fastForward(uint32_t id, OPCODE op){
    zinfo->FastForwardIns[id]++;
    if ( (zinfo->FastForward == true) && (zinfo->FastForwardIns[id] < FF_INS) ) {
        //info("id %d: FF %d", id, zinfo->FastForwardIns[id]);
        PIN_RemoveInstrumentation();
    } 
    else {
        zinfo->cycles[id]++;
        zinfo->core[id].lastUpdateInstrs++;
        zinfo->core[id].lastUpdateCycles++;

        if (op == 271) {
            fprintf(trace, "start tx %lu", zinfo->tx_id[id]);
            //printf("start tx %lu\n", zinfo->tx_id[id]);
            zinfo->persistent[id]=true;
        }
        else if (op == 578) {
            fprintf(trace, "end tx %lu", zinfo->tx_id[id]);
            //printf("end tx %lu\n", zinfo->tx_id[id]);
            zinfo->persistent[id]=false; 
            zinfo->tx_id[id]++;
        }

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
 
    INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)fastForward,IARG_UINT32,  procIdx, IARG_UINT32, INS_Opcode(ins),  IARG_END);

    //info("OPCODE %u : %s", INS_Opcode(ins), INS_Mnemonic(ins).c_str());



    UINT32 memOperands = INS_MemoryOperandCount(ins);

    // Iterate over each memory operand of the instruction.
    for (UINT32 memOp = 0; memOp < memOperands; memOp++)
    {
        //info("memory Access"); 
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
    if (procIdx == 0) {
        switch (zinfo->arch) {
            case NATIVE: 
                info("NATIVE");
                break;
            case NVMLOG: 
                info("NVMLOG");
                break;
            case KILN: 
                info("KILN");
                break;
            case EPB: 
                info("EPB");
                break;
            case EPB_BF: 
                info("EPB_BF");
                break;
            default: 
                info("UNKNOWN");

        }
        for (uint32_t i = 0; i < 8; i++) {
           info("zinfo->core[%u].lastUpdateCycles: %lu", i, zinfo->core[i].lastUpdateCycles);
        }

        for (uint32_t i = 0; i < 8; i++) {
           info("zinfo->pc.l1_hGETS[%u]: %lu", i, zinfo->pc.l1_hGETS[i]);
           info("zinfo->pc.l1_hGETX[%u]: %lu", i, zinfo->pc.l1_hGETX[i]);
           info("zinfo->pc.l1_mGETS[%u]: %lu", i, zinfo->pc.l1_mGETS[i]);
           info("zinfo->pc.l1_mGETX[%u]: %lu", i, zinfo->pc.l1_mGETX[i]);
           info("zinfo->pc.l1_PUTS[%u]: %lu", i, zinfo->pc.l1_PUTS[i]);
           info("zinfo->pc.l1_PUTS[%u]: %lu", i, zinfo->pc.l1_PUTX[i]);

           info("zinfo->pc.l2_hGETS[%u]: %lu", i, zinfo->pc.l2_hGETS[i]);
           info("zinfo->pc.l2_hGETX[%u]: %lu", i, zinfo->pc.l2_hGETX[i]);
           info("zinfo->pc.l2_mGETS[%u]: %lu", i, zinfo->pc.l2_mGETS[i]);
           info("zinfo->pc.l2_mGETX[%u]: %lu", i, zinfo->pc.l2_mGETX[i]);
           info("zinfo->pc.l2_PUTS[%u]: %lu", i, zinfo->pc.l2_PUTS[i]);
           info("zinfo->pc.l2_PUTS[%u]: %lu", i, zinfo->pc.l2_PUTX[i]);
        }
        info("zinfo->pc.nvc_hGETS: %lu", zinfo->pc.nvc_hGETS);
        info("zinfo->pc.nvc_hGETX: %lu", zinfo->pc.nvc_hGETX);
        info("zinfo->pc.nvc_mGETS: %lu", zinfo->pc.nvc_mGETS);
        info("zinfo->pc.nvc_nGETX: %lu", zinfo->pc.nvc_mGETX);
        info("zinfo->pc.nvc_PUTS: %lu", zinfo->pc.nvc_PUTS);
        info("zinfo->pc.nvc_PUTX: %lu", zinfo->pc.nvc_PUTX);

        info("zinfo->pc.dram_hGETS: %lu", zinfo->pc.dram_hGETS);
        info("zinfo->pc.dram_hGETX: %lu", zinfo->pc.dram_hGETX);
        info("zinfo->pc.dram_mGETS: %lu", zinfo->pc.dram_mGETS);
        info("zinfo->pc.dram_mGETX: %lu", zinfo->pc.dram_mGETX);
        info("zinfo->pc.dram_PUTS: %lu", zinfo->pc.dram_PUTS);
        info("zinfo->pc.dram_PUTS: %lu", zinfo->pc.dram_PUTX);

        info("zinfo->pc.nvm_GETS: %lu", zinfo->pc.nvm_GETS);
        info("zinfo->pc.nvm_GETX: %lu", zinfo->pc.nvm_GETX);
        info("zinfo->pc.nvm_PUTS: %lu", zinfo->pc.nvm_PUTS);
        info("zinfo->pc.nvm_PUTS: %lu", zinfo->pc.nvm_PUTX);
    }
    fprintf(trace, "#eof\n");
    fclose(trace);

            uint64_t nvc_dram_max = PHASE_LENGTH * DRAM_BANKS;
            uint64_t nvm_max = PHASE_LENGTH * NVM_BANKS;


            uint64_t nvc_to_dram_write = zinfo->nvc_to_dram_write;  //number of access nvc-> drm
            uint64_t dram_to_nvc_write = zinfo->dram_to_nvc_write; // number of write dram -> nvm
        
            uint64_t nvc_to_nvm_write = zinfo ->nvc_to_nvm_write; //number of write nvc->nvm
         
            uint64_t persist_w_evict_nvc = zinfo->persist_w_evict_nvc;
            uint64_t persist_w = zinfo->persist_w; 
            uint64_t mem_access = zinfo->mem_access; 
        
            double nvc_and_dram_bandwidth_percentage = (double)((nvc_to_dram_write * DRAM_LATENCY) + (dram_to_nvc_write * NVC_WRITE_LATENCY)) / (double)(nvc_dram_max); 
            double nvc_to_dram_bandwidth_percentage = (double)(nvc_to_dram_write * DRAM_LATENCY) / (double)(nvc_dram_max); 
            double dram_to_nvc_bandwidth_percentage = (double)(dram_to_nvc_write * NVC_WRITE_LATENCY) / (double)(nvc_dram_max); 
            double nvc_to_nvm_bandwidth_percentage = (double)((nvc_to_nvm_write * NVM_WRITE_LATENCY)) /(double) (nvm_max);

            printf("mem access %lu\n", mem_access);        
            printf("persist_w %lu\n", persist_w);
            printf("persist_w_evict_nvc %lu\n", persist_w_evict_nvc);

            printf("nvc_and_dram_bandwidth_percentage %f\n", nvc_and_dram_bandwidth_percentage);
            printf("nvc_to_dram_bandwidth_percentage %f\n",nvc_to_dram_bandwidth_percentage);
            printf("dram_to_nvc_bandwidth_percentage %f\n",dram_to_nvc_bandwidth_percentage);
            printf("nvc_to_nvm_bandwidth_percentage %f\n",nvc_to_nvm_bandwidth_percentage);
    printf("Finish Bound %d\n", zinfo->global_phase-1);
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
/* main                                                                  */
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
       // info("PHASE_LENGTH %d", zinfo->phaseLength);
       // info("Core %d, inst %lu", procIdx, zinfo->core[procIdx].lastUpdateInstrs);
    } 


    switch (ARCHITECTURE) {
        case NATIVE:
            info("NATIVE");
            l1_lookup=&native_l1_lookup;
            l1_reverse_lookup=&native_l1_reverse_lookup;
            l1_access=&native_l1_access;
            l1_evict=&native_l1_evict;
            l1_preinsert=&native_l1_preinsert;
            l1_postinsert=&native_l1_postinsert;
            l1_fetch=&native_l1_fetch;
            l2_lookup=&native_l2_lookup;
            l2_reverse_lookup=&native_l2_reverse_lookup;
            l2_access=&native_l2_access;
            l2_evict=&native_l2_evict;
            l2_preinsert=&native_l2_preinsert;
            l2_postinsert=&native_l2_postinsert;
            l2_fetch=&native_l2_fetch;
            nvc_lookup=&native_nvc_lookup;
            nvc_reverse_lookup=&native_nvc_reverse_lookup;
            nvc_access=&native_nvc_access;
            nvc_evict=&native_nvc_evict;
            nvc_preinsert=&native_nvc_preinsert;
            nvc_postinsert=&native_nvc_postinsert;
            nvc_fetch=&native_nvc_fetch;
            dram_lookup=&native_dram_lookup;
            dram_reverse_lookup=&native_dram_reverse_lookup;
            dram_access=&native_dram_access;
            dram_evict=&native_dram_evict;
            dram_preinsert=&native_dram_preinsert;
            dram_postinsert=&native_dram_postinsert;
            dram_fetch=&native_dram_fetch;
            nvm_access=&native_nvm_access;
            break;
        case NVMLOG:
            info("NVMLOG");
            l1_lookup=&nvmlog_l1_lookup;
            l1_reverse_lookup=&nvmlog_l1_reverse_lookup;
            l1_access=&nvmlog_l1_access;
            l1_evict=&nvmlog_l1_evict;
            l1_preinsert=&nvmlog_l1_preinsert;
            l1_postinsert=&nvmlog_l1_postinsert;
            l1_fetch=&nvmlog_l1_fetch;
            l2_lookup=&nvmlog_l2_lookup;
            l2_reverse_lookup=&nvmlog_l2_reverse_lookup;
            l2_access=&nvmlog_l2_access;
            l2_evict=&nvmlog_l2_evict;
            l2_preinsert=&nvmlog_l2_preinsert;
            l2_postinsert=&nvmlog_l2_postinsert;
            l2_fetch=&nvmlog_l2_fetch;
            nvc_lookup=&nvmlog_nvc_lookup;
            nvc_reverse_lookup=&nvmlog_nvc_reverse_lookup;
            nvc_access=&nvmlog_nvc_access;
            nvc_evict=&nvmlog_nvc_evict;
            nvc_preinsert=&nvmlog_nvc_preinsert;
            nvc_postinsert=&nvmlog_nvc_postinsert;
            nvc_fetch=&nvmlog_nvc_fetch;
            dram_lookup=&nvmlog_dram_lookup;
            dram_reverse_lookup=&nvmlog_dram_reverse_lookup;
            dram_access=&nvmlog_dram_access;
            dram_evict=&nvmlog_dram_evict;
            dram_preinsert=&nvmlog_dram_preinsert;
            dram_postinsert=&nvmlog_dram_postinsert;
            dram_fetch=&nvmlog_dram_fetch;
            nvm_access=&nvmlog_nvm_access;
            break;
        case KILN:     
            info("KILN");
            l1_lookup=&kiln_l1_lookup;
            l1_reverse_lookup=&kiln_l1_reverse_lookup;
            l1_access=&kiln_l1_access;
            l1_evict=&kiln_l1_evict;
            l1_preinsert=&kiln_l1_preinsert;
            l1_postinsert=&kiln_l1_postinsert;
            l1_fetch=&kiln_l1_fetch;
            l2_lookup=&kiln_l2_lookup;
            l2_reverse_lookup=&kiln_l2_reverse_lookup;
            l2_access=&kiln_l2_access;
            l2_evict=&kiln_l2_evict;
            l2_preinsert=&kiln_l2_preinsert;
            l2_postinsert=&kiln_l2_postinsert;
            l2_fetch=&kiln_l2_fetch;
            nvc_lookup=&kiln_nvc_lookup;
            nvc_reverse_lookup=&kiln_nvc_reverse_lookup;
            nvc_access=&kiln_nvc_access;
            nvc_evict=&kiln_nvc_evict;
            nvc_preinsert=&kiln_nvc_preinsert;
            nvc_postinsert=&kiln_nvc_postinsert;
            nvc_fetch=&kiln_nvc_fetch;
            dram_lookup=&kiln_dram_lookup;
            dram_reverse_lookup=&kiln_dram_reverse_lookup;
            dram_access=&kiln_dram_access;
            dram_evict=&kiln_dram_evict;
            dram_preinsert=&kiln_dram_preinsert;
            dram_postinsert=&kiln_dram_postinsert;
            dram_fetch=&kiln_dram_fetch;
            nvm_access=&kiln_nvm_access;
            break;
        case EPB:   
            info("EPB");
            l1_lookup=&epb_l1_lookup;
            l1_reverse_lookup=&epb_l1_reverse_lookup;
            l1_access=&epb_l1_access;
            l1_evict=&epb_l1_evict;
            l1_preinsert=&epb_l1_preinsert;
            l1_postinsert=&epb_l1_postinsert;
            l1_fetch=&epb_l1_fetch;
            l2_lookup=&epb_l2_lookup;
            l2_reverse_lookup=&epb_l2_reverse_lookup;
            l2_access=&epb_l2_access;
            l2_evict=&epb_l2_evict;
            l2_preinsert=&epb_l2_preinsert;
            l2_postinsert=&epb_l2_postinsert;
            l2_fetch=&epb_l2_fetch;
            nvc_lookup=&epb_nvc_lookup;
            nvc_reverse_lookup=&epb_nvc_reverse_lookup;
            nvc_access=&epb_nvc_access;
            nvc_evict=&epb_nvc_evict;
            nvc_preinsert=&epb_nvc_preinsert;
            nvc_postinsert=&epb_nvc_postinsert;
            nvc_fetch=&epb_nvc_fetch;
            dram_lookup=&epb_dram_lookup;
            dram_reverse_lookup=&epb_dram_reverse_lookup;
            dram_access=&epb_dram_access;
            dram_evict=&epb_dram_evict;
            dram_preinsert=&epb_dram_preinsert;
            dram_postinsert=&epb_dram_postinsert;
            dram_fetch=&epb_dram_fetch;
            nvm_access=&epb_nvm_access;
            break;
        case EPB_BF:
            info("EPB_BF");
            l1_lookup=&epb_bf_l1_lookup;
            l1_reverse_lookup=&epb_bf_l1_reverse_lookup;
            l1_access=&epb_bf_l1_access;
            l1_evict=&epb_bf_l1_evict;
            l1_preinsert=&epb_bf_l1_preinsert;
            l1_postinsert=&epb_bf_l1_postinsert;
            l1_fetch=&epb_bf_l1_fetch;
            l2_lookup=&epb_bf_l2_lookup;
            l2_reverse_lookup=&epb_bf_l2_reverse_lookup;
            l2_access=&epb_bf_l2_access;
            l2_evict=&epb_bf_l2_evict;
            l2_preinsert=&epb_bf_l2_preinsert;
            l2_postinsert=&epb_bf_l2_postinsert;
            l2_fetch=&epb_bf_l2_fetch;
            nvc_lookup=&epb_bf_nvc_lookup;
            nvc_reverse_lookup=&epb_bf_nvc_reverse_lookup;
            nvc_access=&epb_bf_nvc_access;
            nvc_evict=&epb_bf_nvc_evict;
            nvc_preinsert=&epb_bf_nvc_preinsert;
            nvc_postinsert=&epb_bf_nvc_postinsert;
            nvc_fetch=&epb_bf_nvc_fetch;
            dram_lookup=&epb_bf_dram_lookup;
            dram_reverse_lookup=&epb_bf_dram_reverse_lookup;
            dram_access=&epb_bf_dram_access;
            dram_evict=&epb_bf_dram_evict;
            dram_preinsert=&epb_bf_dram_preinsert;
            dram_postinsert=&epb_bf_dram_postinsert;
            dram_fetch=&epb_bf_dram_fetch;
            nvm_access=&epb_bf_nvm_access;
            break;
        default:
            info("UNKNOWN");
            l1_lookup=&native_l1_lookup;
            l1_reverse_lookup=&native_l1_reverse_lookup;
            l1_access=&native_l1_access;
            l1_evict=&native_l1_evict;
            l1_preinsert=&native_l1_preinsert;
            l1_postinsert=&native_l1_postinsert;
            l1_fetch=&native_l1_fetch;
            l2_lookup=&native_l2_lookup;
            l2_reverse_lookup=&native_l2_reverse_lookup;
            l2_access=&native_l2_access;
            l2_evict=&native_l2_evict;
            l2_preinsert=&native_l2_preinsert;
            l2_postinsert=&native_l2_postinsert;
            l2_fetch=&native_l2_fetch;
            nvc_lookup=&native_nvc_lookup;
            nvc_reverse_lookup=&native_nvc_reverse_lookup;
            nvc_access=&native_nvc_access;
            nvc_evict=&native_nvc_evict;
            nvc_preinsert=&native_nvc_preinsert;
            nvc_postinsert=&native_nvc_postinsert;
            nvc_fetch=&native_nvc_fetch;
            dram_lookup=&native_dram_lookup;
            dram_reverse_lookup=&native_dram_reverse_lookup;
            dram_access=&native_dram_access;
            dram_evict=&native_dram_evict;
            dram_preinsert=&native_dram_preinsert;
            dram_postinsert=&native_dram_postinsert;
            dram_fetch=&native_dram_fetch;
            nvm_access=&native_nvm_access;
            break;
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


