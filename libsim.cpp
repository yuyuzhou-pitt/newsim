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


void atomic_add_timestamp(){
    futex_lock(&zinfo->lock);
    zinfo->timestamp++;
    futex_unlock(&zinfo->lock);
}


/***********************l1 ************************************/

int32_t l1_lookup(uint32_t procId, Address lineAddr){
    //range
    Address start = (lineAddr % zinfo->l1cache[procId].numSets)*L1D_WAYS; 
    Address end = start + L1D_WAYS; 
    //fprintf(trace, "L1 lookup %lu - %lu\n", start, end);
    for (uint32_t i = start; i<end; i++) {
         if (zinfo->l1cache[procId].array[i] == lineAddr)
             return i;  
    }
    return -1; 
}

Address l1_reverse_lookup(uint32_t procId, const int32_t lineID){
     return zinfo->l1cache[procId].array[lineID];
}

uint64_t l1_access(uint32_t procId, MemReq req){
     uint64_t cycles = req.cycle;
     int32_t target_lineId = l1_lookup(procId, req.lineAddr); 
     if (req.type == PUTS) zinfo->pc.l1_PUTS[procId]++;
     else if (req.type == PUTX) zinfo->pc.l1_PUTX[procId]++;
     if (target_lineId == -1) { //miss               
         if (req.type == GETS) zinfo->pc.l1_mGETS[procId]++;
         else if (req.type == GETX) zinfo->pc.l1_mGETX[procId]++;
         target_lineId=l1_preinsert(procId, req);
         cycles = l1_evict(procId, req, target_lineId);
         req.cycle=cycles;
         cycles = l1_fetch(procId, req); 
         l1_postinsert(procId, req, target_lineId);    
         cycles += zinfo->l1cache[procId].accLat - 1;
     } else { //hit 
         if (req.type == GETS) zinfo->pc.l1_hGETS[procId]++;
         else if (req.type == GETX) zinfo->pc.l1_hGETX[procId]++;
         l1_postinsert(procId, req, target_lineId);
         if (req.type!=PUTS) cycles += zinfo->l1cache[procId].accLat - 1; 
     }
     return cycles; 
}

uint32_t l1_preinsert(uint32_t procId, MemReq req){
    Address start = (req.lineAddr%zinfo->l1cache[procId].numSets)*L1D_WAYS;
    Address end = start + L1D_WAYS;
    //fprintf(trace, "L1 preinsert %lu - %lu\n", start, end);
    uint32_t bestCand = start;
    uint64_t bestTS = zinfo->l1cache[procId].ts[start];
    for (uint32_t i = start; i<end; i++) {
        if (zinfo->l1cache[procId].state[i] == I) {  //find an unused cacheline: 
            return i;
        }
        else if (zinfo->l1cache[procId].ts[i] < bestTS) {  // finder the eariler one
            bestCand = i; 
            bestTS = zinfo->l1cache[procId].ts[i];
        }
    }
    return bestCand;  
}


void l1_postinsert(uint32_t procId, MemReq req, int32_t lineID){
    switch (req.type) {
         case GETS:
             zinfo->l1cache[procId].array[lineID]=req.lineAddr;
             zinfo->l1cache[procId].state[lineID]=E; 
             zinfo->l1cache[procId].ts[lineID]=zinfo->timestamp;
             atomic_add_timestamp();
             break;
         case GETX: 
             zinfo->l1cache[procId].array[lineID]=req.lineAddr;
             zinfo->l1cache[procId].state[lineID]=M; 
             zinfo->l1cache[procId].ts[lineID]=zinfo->timestamp;
             atomic_add_timestamp();
             break;
         case PUTS:
             zinfo->l1cache[procId].array[lineID]=req.lineAddr;
             zinfo->l1cache[procId].state[lineID]=E; 
             zinfo->l1cache[procId].ts[lineID]=zinfo->timestamp;
             atomic_add_timestamp();
             break;
         case PUTX:
             zinfo->l1cache[procId].array[lineID]=req.lineAddr;
             zinfo->l1cache[procId].state[lineID]=M; 
             zinfo->l1cache[procId].ts[lineID]=zinfo->timestamp;
             atomic_add_timestamp();
             break;
         default: 
             return;
    }
    return;
}

uint64_t l1_evict(uint32_t procId, MemReq req, const int32_t lineID){
    MemReq newreq; 
    newreq.lineAddr = zinfo->l1cache[procId].array[lineID]; 
    newreq.cycle = req.cycle; 
    switch (zinfo->l1cache[procId].state[lineID]) {
        case I: 
            return req.cycle;
            break; 
        case S:
            newreq.type=PUTS;
            break; 
        case E: 
            newreq.type=PUTS;
            break; 
        case M: 
            newreq.type=PUTX;
            break; 
        default: 
            return newreq.cycle; 
    }    
    return l2_access(procId, newreq); 
}

uint64_t l1_fetch(uint32_t procId, MemReq req){
    return l2_access(procId, req); 
}


/****************l2 *******************************/
int32_t l2_lookup(uint32_t procId, Address lineAddr){
    //range
    Address start = (lineAddr % zinfo->l2cache[procId].numSets)*L2_WAYS; 
    Address end = start + L2_WAYS; 
    //fprintf(trace, "L2 lookup %lu - %lu, addr lineAddr %lx\n", start, end, lineAddr);
    //debug();
    for (uint32_t i = start; i<end; i++) {
         if (zinfo->l2cache[procId].array[i] == lineAddr){
             //fprintf(trace, "hit %u\n", i);
             return i;  
         }
    }
    //fprintf(trace, "miss\n");
    return -1; 
}

Address l2_reverse_lookup(uint32_t procId, const int32_t lineID){
     return zinfo->l2cache[procId].array[lineID];
}

uint64_t l2_access(uint32_t procId, MemReq req){
     uint64_t cycles = req.cycle;
     int32_t target_lineId = l2_lookup(procId, req.lineAddr); 

     if (req.type == PUTS) zinfo->pc.l2_PUTS[procId]++;
     else if (req.type == PUTX) zinfo->pc.l2_PUTX[procId]++;
     if (target_lineId == -1) { //miss               
         if (req.type == GETS) zinfo->pc.l2_mGETS[procId]++;
         else if (req.type == GETX) zinfo->pc.l2_mGETX[procId]++;
         target_lineId=l2_preinsert(procId, req);
         cycles = l2_evict(procId, req, target_lineId);
         req.cycle=cycles;
         cycles = l2_fetch(procId, req); 
         l2_postinsert(procId, req, target_lineId);    
         cycles += zinfo->l2cache[procId].accLat - 1;
     } else { //hit
         if (req.type == GETS) zinfo->pc.l2_hGETS[procId]++;
         else if (req.type == GETX) zinfo->pc.l2_hGETX[procId]++;
         l2_postinsert(procId, req, target_lineId);
         if (req.type != PUTS) cycles += zinfo->l2cache[procId].accLat - 1;  //clean write back hit does not need to wrtie
     }
     return cycles; 
}

uint32_t l2_preinsert(uint32_t procId, MemReq req){
    Address start = (req.lineAddr%zinfo->l2cache[procId].numSets)*L2_WAYS;
    Address end = start + L2_WAYS;
    //fprintf(trace, "L2 preinsert %lu - %lu\n", start, end);
    uint32_t bestCand = start;
    uint64_t bestTS = zinfo->l2cache[procId].ts[start];
    for (uint32_t i = start; i<end; i++) {
        if (zinfo->l2cache[procId].state[i] == I) {  //find an unused cacheline: 
            return i;
        }
        else if (zinfo->l2cache[procId].ts[i] < bestTS) {  // finder the eariler one
            bestCand = i; 
            bestTS = zinfo->l2cache[procId].ts[i];
        }
    }
    return bestCand;  
}

void l2_postinsert(uint32_t procId, MemReq req, int32_t lineID){
    switch (req.type) {
         case GETS:
             zinfo->l2cache[procId].array[lineID]=req.lineAddr;
             zinfo->l2cache[procId].state[lineID]=E; 
             zinfo->l2cache[procId].ts[lineID]=zinfo->timestamp;
             atomic_add_timestamp();
             break;
         case GETX: 
             zinfo->l2cache[procId].array[lineID]=req.lineAddr;
             zinfo->l2cache[procId].state[lineID]=M; 
             zinfo->l2cache[procId].ts[lineID]=zinfo->timestamp;
             atomic_add_timestamp();
             break;
         case PUTS:
             zinfo->l2cache[procId].array[lineID]=req.lineAddr;
             zinfo->l2cache[procId].state[lineID]=E; 
             zinfo->l2cache[procId].ts[lineID]=zinfo->timestamp;
             atomic_add_timestamp();
             break;
         case PUTX:
             zinfo->l2cache[procId].array[lineID]=req.lineAddr;
             zinfo->l2cache[procId].state[lineID]=M; 
             zinfo->l2cache[procId].ts[lineID]=zinfo->timestamp;
             atomic_add_timestamp();
             break;
         default: 
             return;
    }
    return;
}

uint64_t l2_evict(uint32_t procId, MemReq req, const int32_t lineID){
    MemReq newreq; 
    newreq.lineAddr = zinfo->l2cache[procId].array[lineID]; 
    newreq.cycle = req.cycle; 
    switch (zinfo->l2cache[procId].state[lineID]) {
        case I: 
            return req.cycle;
            break; 
        case S:
            newreq.type=PUTS;
            break; 
        case E: 
            newreq.type=PUTS;
            break; 
        case M: 
            newreq.type=PUTX;
            break; 
        default: 
            return newreq.cycle; 
    }    
    return nvc_access(procId, newreq); 
}


uint64_t l2_fetch(uint32_t procId, MemReq req){
    return nvc_access(procId, req); 
}



/******************** nvc *************************************/

int32_t nvc_lookup(uint32_t procId, Address lineAddr){
    //range
    Address start = (lineAddr % zinfo->nvc.numSets)*NVC_WAYS; 
    Address end = start + NVC_WAYS; 
    for (uint32_t i = start; i<end; i++) {
         if (zinfo->nvc.array[i] == lineAddr)
             return i;  
    }
    return -1; 
}

Address nvc_reverse_lookup(uint32_t procId, const int32_t lineID){
     return zinfo->nvc.array[lineID];
}

uint64_t nvc_access(uint32_t procId, MemReq req){
     futex_lock(&zinfo->nvc_lock);
     uint64_t cycles = req.cycle;
     int32_t target_lineId = nvc_lookup(procId, req.lineAddr); 
     if (req.type == PUTS) zinfo->pc.nvc_PUTS++;
     else if (req.type == PUTX) zinfo->pc.nvc_PUTX++;
     if (target_lineId == -1) { //miss               
         if (req.type == GETS) zinfo->pc.nvc_mGETS++;
         else if (req.type == GETX) zinfo->pc.nvc_mGETX++;
         target_lineId=nvc_preinsert(procId, req);
         cycles = nvc_evict(procId, req, target_lineId);
         req.cycle=cycles;
         cycles = nvc_fetch(procId, req); 
         nvc_postinsert(procId, req, target_lineId);    
         switch (req.type) {
             case GETS:
                 cycles += zinfo->nvc.write_accLat - 1;
                 break;
             case GETX:
                 cycles += zinfo->nvc.write_accLat - 1;
                 break;
             case PUTS:
                 cycles += zinfo->nvc.write_accLat - 1;
                 break;
             case PUTX:
                 cycles += zinfo->nvc.write_accLat - 1;
                 break;
             default:
                 break;
         }
     } else { //hit
         if (req.type == GETS) zinfo->pc.nvc_hGETS++;
         else if (req.type == GETX) zinfo->pc.nvc_hGETX++;
         nvc_postinsert(procId, req, target_lineId);
         switch (req.type) {
             case GETS:
                 cycles += zinfo->nvc.read_accLat - 1;
                 break;
             case GETX:
                 cycles += zinfo->nvc.read_accLat - 1;
                 break;
             case PUTS:
                 //cycles += zinfo->nvc.write_accLat - 1;
                 break;
             case PUTX:
                 cycles += zinfo->nvc.write_accLat - 1;
                 break;
             default:
                 break;
         }
     }
     futex_unlock(&zinfo->nvc_lock);
     return cycles; 
}

uint32_t nvc_preinsert(uint32_t procId, MemReq req){
    Address start = (req.lineAddr%zinfo->nvc.numSets)*NVC_WAYS;
    Address end = start + NVC_WAYS;
    uint32_t bestCand = start;
    uint64_t bestTS = zinfo->nvc.ts[start];
    for (uint32_t i = start; i<end; i++) {
        if (zinfo->nvc.state[i] == I) {  //find an unused cacheline: 
            return i;
        }
        else if (zinfo->nvc.ts[i] < bestTS) {  // finder the eariler one
            bestCand = i; 
            bestTS = zinfo->nvc.ts[i];
        }
    }
    return bestCand;  
}


void nvc_postinsert(uint32_t procId, MemReq req, int32_t lineID){
    switch (req.type) {
         case GETS:
             zinfo->nvc.array[lineID]=req.lineAddr;
             zinfo->nvc.state[lineID]=E; 
             zinfo->nvc.ts[lineID]=zinfo->timestamp;
             atomic_add_timestamp();
             break;
         case GETX: 
             zinfo->nvc.array[lineID]=req.lineAddr;
             zinfo->nvc.state[lineID]=M; 
             zinfo->nvc.ts[lineID]=zinfo->timestamp;
             atomic_add_timestamp();
             break;
         case PUTS:
             zinfo->nvc.array[lineID]=req.lineAddr;
             zinfo->nvc.state[lineID]=E; 
             zinfo->nvc.ts[lineID]=zinfo->timestamp;
             atomic_add_timestamp();
             break;
         case PUTX:
             zinfo->nvc.array[lineID]=req.lineAddr;
             zinfo->nvc.state[lineID]=M; 
             zinfo->nvc.ts[lineID]=zinfo->timestamp;
             atomic_add_timestamp();
             break;
         default: 
             return;
    }
    return;
}

uint64_t nvc_evict(uint32_t procId, MemReq req, const int32_t lineID){
    MemReq newreq; 
    newreq.lineAddr = zinfo->nvc.array[lineID]; 
    newreq.cycle = req.cycle; 
    switch (zinfo->nvc.state[lineID]) {
        case I: 
            return req.cycle;
            break; 
        case S:
            newreq.type=PUTS;
            break; 
        case E: 
            newreq.type=PUTS;
            break; 
        case M: 
            newreq.type=PUTX;
            break; 
        default: 
            return newreq.cycle; 
    }    
    return dram_access(procId, newreq); 
}


uint64_t nvc_fetch(uint32_t procId, MemReq req){
    return dram_access(procId, req); 
}


/************************* dram    ***************************/

int32_t dram_lookup(uint32_t procId, Address lineAddr){
    //range
    Address start = (lineAddr % zinfo->dram.numSets)*DRAM_WAYS; 
    Address end = start + DRAM_WAYS; 
    for (uint32_t i = start; i<end; i++) {
         if (zinfo->dram.array[i] == lineAddr)
             return i;  
    }
    return -1; 
}

Address dram_reverse_lookup(uint32_t procId, const int32_t lineID){
     return zinfo->dram.array[lineID];
}

uint64_t dram_access(uint32_t procId, MemReq req){
     futex_lock(&zinfo->dram_lock);
     uint64_t cycles = req.cycle;
     int32_t target_lineId = dram_lookup(procId, req.lineAddr); 
     if (req.type == PUTS) zinfo->pc.dram_PUTS++;
     else if (req.type == PUTX) zinfo->pc.dram_PUTX++;
     if (target_lineId == -1) { //miss               
         if (req.type == GETS) zinfo->pc.dram_mGETS++;
         else if (req.type == GETX) zinfo->pc.dram_mGETX++;
         target_lineId=dram_preinsert(procId, req);
         cycles = dram_evict(procId, req, target_lineId);
         req.cycle=cycles;
         cycles = dram_fetch(procId, req); 
         dram_postinsert(procId, req, target_lineId);    
         cycles += zinfo->dram.accLat - 1;
     } else { //hit
         if (req.type == GETS) zinfo->pc.dram_hGETS++;
         else if (req.type == GETX) zinfo->pc.dram_hGETX++;
         dram_postinsert(procId, req, target_lineId);
         if (req.type!=PUTS) cycles += zinfo->dram.accLat - 1; 
     }
     futex_unlock(&zinfo->dram_lock);
     return cycles; 
}

uint32_t dram_preinsert(uint32_t procId, MemReq req){
    Address start = (req.lineAddr%zinfo->dram.numSets)*DRAM_WAYS;
    Address end = start + DRAM_WAYS;
    uint32_t bestCand = start;
    uint64_t bestTS = zinfo->dram.ts[start];
    for (uint32_t i = start; i<end; i++) {
        if (zinfo->dram.state[i] == I) {  //find an unused cacheline: 
            return i;
        }
        else if (zinfo->dram.ts[i] < bestTS) {  // finder the eariler one
            bestCand = i; 
            bestTS = zinfo->dram.ts[i];
        }
    }
    return bestCand;  
}


void dram_postinsert(uint32_t procId, MemReq req, int32_t lineID){
    switch (req.type) {
         case GETS:
             zinfo->dram.array[lineID]=req.lineAddr;
             zinfo->dram.state[lineID]=E; 
             zinfo->dram.ts[lineID]=zinfo->timestamp;
             atomic_add_timestamp();
             break;
         case GETX: 
             zinfo->dram.array[lineID]=req.lineAddr;
             zinfo->dram.state[lineID]=M; 
             zinfo->dram.ts[lineID]=zinfo->timestamp;
             atomic_add_timestamp();
             break;
         case PUTS:
             zinfo->dram.array[lineID]=req.lineAddr;
             zinfo->dram.state[lineID]=E; 
             zinfo->dram.ts[lineID]=zinfo->timestamp;
             atomic_add_timestamp();
             break;
         case PUTX:
             zinfo->dram.array[lineID]=req.lineAddr;
             zinfo->dram.state[lineID]=M; 
             zinfo->dram.ts[lineID]=zinfo->timestamp;
             atomic_add_timestamp();
             break;
         default: 
             return;
    }
    return;
}

uint64_t dram_evict(uint32_t procId, MemReq req, const int32_t lineID){
    MemReq newreq; 
    newreq.lineAddr = zinfo->dram.array[lineID]; 
    newreq.cycle = req.cycle; 
    switch (zinfo->dram.state[lineID]) {
        case I: 
            return req.cycle;
            break; 
        case S:
            newreq.type=PUTS;
            break; 
        case E: 
            newreq.type=PUTS;
            break; 
        case M: 
            newreq.type=PUTX;
            break; 
        default: 
            return newreq.cycle; 
    }    
    return nvm_access(procId, newreq); 
}

uint64_t dram_fetch(uint32_t procId, MemReq req){
    return nvm_access(procId, req); 
}

/************************* nvm ****************************/

uint64_t nvm_access(uint32_t procId, MemReq req){
     futex_lock(&zinfo->nvm_lock);
     uint64_t cycles = req.cycle;
     switch (req.type) {
         case GETS:
             cycles += zinfo->nvm.read_accLat - 1;
             zinfo->pc.nvm_GETS++;
             break;
         case GETX:
             cycles += zinfo->nvm.write_accLat - 1; 
             zinfo->pc.nvm_GETX++;
             break;
         case PUTS: 
             //cycles += zinfo->nvm.write_accLat - 1;
             zinfo->pc.nvm_PUTS++;
             break; 
         case PUTX:
             zinfo->pc.nvm_PUTX++;
             cycles += zinfo->nvm.write_accLat - 1; 
             break; 
         default:
             break;
     } 
     futex_unlock(&zinfo->nvm_lock);
     return cycles; 
}



void SimInit(uint32_t shmid){
    zinfo = gm_calloc<GlobSimInfo>();
    futex_lock(&zinfo->lock); 
    zinfo->timestamp = 0; 
    zifo->arch = ARCHITECURE;
    for (uint32_t i = 0; i < 8; i++) {
        zinfo->cycles[i]=0;
        zinfo->FastForwardIns[i]=0;
        zinfo->phase[i]=0;
        zinfo->core[i].lastUpdateCycles = 0;
        zinfo->core[i].lastUpdateInstrs = 0;

// initialize L1 cache
        zinfo->l1cache[i].accLat = L1D_LATENCY;
        zinfo->l1cache[i].numSets = L1D_SIZE/ (64*L1D_WAYS); 
        for (uint32_t j =0; j<L1D_SIZE/64; j++) {
            zinfo->l1cache[i].array[j]=0;
            zinfo->l1cache[i].state[j]=I;
            zinfo->l1cache[i].ts[j]=0;
        }

// initialize L2 cache
        zinfo->l2cache[i].accLat = L2_LATENCY;
        zinfo->l2cache[i].numSets = L2_SIZE/ (64*L2_WAYS); 
        for (uint32_t j =0; j<L2_SIZE/64; j++) {
            zinfo->l2cache[i].array[j]=0;
            zinfo->l2cache[i].state[j]=I;
            zinfo->l2cache[i].ts[j]=0;
        }
    }

// initialize NVC cache
    zinfo->nvc.read_accLat = NVC_READ_LATENCY;
    zinfo->nvc.write_accLat = NVC_WRITE_LATENCY;
    zinfo->nvc.numSets = NVC_SIZE/(64*NVC_WAYS); 
    for (uint32_t j=0;j<NVC_SIZE/64;j++) {
        zinfo->nvc.array[j]=0;
        zinfo->nvc.state[j]=I;
        zinfo->nvc.ts[j]=0;
    }

// initialize DRAM cache
    zinfo->dram.accLat = DRAM_LATENCY;
    zinfo->dram.numSets = DRAM_SIZE/(64*DRAM_WAYS);
    for (uint32_t j=0;j<DRAM_SIZE/64;j++) {
        zinfo->dram.array[j]=0;
        zinfo->dram.state[j]=I;
        zinfo->dram.ts[j]=0;
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

    futex_unlock(&zinfo->lock); 
    gm_set_glob_ptr(zinfo);

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
    if ( (zinfo->FastForward == false) || (zinfo->FastForwardIns[procIdx] >= FF_INS) ) {  // no longer in fast forwarding
//        info("AF %d", zinfo->FastForwardIns[id]);
//        fprintf(trace,"%lf : R %p\n", diff.count(), addr);
        auto ttp = std::chrono::system_clock::now();
        std::chrono::duration<double> diff = ttp-start;
        fprintf(trace,"%lf : R %p\n", diff.count(),  addr);

          MemReq req; 
          req.lineAddr = (Address) addr; 
          req.type = GETS;
          req.cycle = zinfo->core[id].lastUpdateCycles; 
          zinfo->core[id].lastUpdateCycles = l1_access(id, req)-1;
        fprintf(trace, "current cycle : %lu\n", zinfo->core[id].lastUpdateCycles);
        //debug();

    }
}

// Print a memory write record
VOID RecordMemWrite(VOID * ip, VOID * addr, uint32_t id)
{
    atomic_add_timestamp();
    if ( (zinfo->FastForward == false) || (zinfo->FastForwardIns[procIdx] >= FF_INS) ) {  // no longer in fast forwarding
        auto ttp = std::chrono::system_clock::now();
        std::chrono::duration<double> diff = ttp-start;
        fprintf(trace,"%lf : W %p\n", diff.count(), addr );
          MemReq req;
          req.lineAddr = (Address) addr; 
          req.type = GETX;
          req.cycle = zinfo->core[id].lastUpdateCycles;
          zinfo->core[id].lastUpdateCycles = l1_access(id, req)-1;

        fprintf(trace, "current cycle : %lu\n", zinfo->core[id].lastUpdateCycles);
        //debug();


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
        zinfo->core[id].lastUpdateInstrs++;
        zinfo->core[id].lastUpdateCycles++;
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
    if (procIdx == 0) {
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
/* ain                                                                  */
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


