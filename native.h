#include <sys/types.h>
#include <sys/prctl.h>
#include <unistd.h>
#include <stdio.h>
#include <chrono>
#include <numeric>
#include <sstream>
#include "galloc.h"
#include "libsim.h"
#include "log.h"


/***********************l1 ************************************/

int32_t native_l1_lookup(uint32_t procId, Address lineAddr){
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

Address native_l1_reverse_lookup(uint32_t procId, const int32_t lineID){
     return zinfo->l1cache[procId].array[lineID];
}

uint64_t native_l1_access(uint32_t procId, MemReq req){
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
         cycles += zinfo->l1cache[procId].accLat;
     } else { //hit 
         if (req.type == GETS) zinfo->pc.l1_hGETS[procId]++;
         else if (req.type == GETX) zinfo->pc.l1_hGETX[procId]++;
         l1_postinsert(procId, req, target_lineId);
         if (req.type!=PUTS) cycles += zinfo->l1cache[procId].accLat; 
     }
     printf("l1 access cycle: %lu\n", cycles );
     return cycles; 
}

uint32_t native_l1_preinsert(uint32_t procId, MemReq req){
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


void native_l1_postinsert(uint32_t procId, MemReq req, int32_t lineID){
    switch (req.type) {
         case GETS:
             zinfo->l1cache[procId].array[lineID]=req.lineAddr;
             if (zinfo->l1cache[procId].state[lineID] != M) zinfo->l1cache[procId].state[lineID]=E; 
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

uint64_t native_l1_evict(uint32_t procId, MemReq req, const int32_t lineID){
    uint64_t cycles;
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
    cycles = l2_access(procId, newreq);
    zinfo->l1cache[procId].state[lineID] = I;
    return cycles;  
}

uint64_t native_l1_fetch(uint32_t procId, MemReq req){
    return l2_access(procId, req); 
}


/****************l2 *******************************/
int32_t native_l2_lookup(uint32_t procId, Address lineAddr){
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

Address native_l2_reverse_lookup(uint32_t procId, const int32_t lineID){
     return zinfo->l2cache[procId].array[lineID];
}

uint64_t native_l2_access(uint32_t procId, MemReq req){
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
         cycles += zinfo->l2cache[procId].accLat;
     } else { //hit
         if (req.type == GETS) zinfo->pc.l2_hGETS[procId]++;
         else if (req.type == GETX) zinfo->pc.l2_hGETX[procId]++;
         l2_postinsert(procId, req, target_lineId);
         if (req.type != PUTS) cycles += zinfo->l2cache[procId].accLat;  //clean write back hit does not need to write
     }
     printf("l2 access cycle: %lu\n", cycles );
     return cycles; 
}

uint32_t native_l2_preinsert(uint32_t procId, MemReq req){
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

void native_l2_postinsert(uint32_t procId, MemReq req, int32_t lineID){
    switch (req.type) {
         case GETS:
             zinfo->l2cache[procId].array[lineID]=req.lineAddr;
             if (zinfo->l2cache[procId].state[lineID] != M) zinfo->l2cache[procId].state[lineID]=E; 
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

uint64_t native_l2_evict(uint32_t procId, MemReq req, const int32_t lineID){
    uint64_t cycles;
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
    cycles = nvc_access(procId, newreq);
    zinfo->l2cache[procId].state[lineID] = I;  
    return cycles;
}


uint64_t native_l2_fetch(uint32_t procId, MemReq req){
    return nvc_access(procId, req); 
}



/******************** nvc *************************************/

int32_t native_nvc_lookup(uint32_t procId, Address lineAddr){
    //range
    Address start = (lineAddr % zinfo->nvc.numSets)*NVC_WAYS; 
    Address end = start + NVC_WAYS; 
    for (uint32_t i = start; i<end; i++) {
         if ((zinfo->nvc.array[i] == lineAddr) && (zinfo->nvc.procId[i]==procId))
             return i;  
    }
    return -1; 
}

Address native_nvc_reverse_lookup(uint32_t procId, const int32_t lineID){
     return zinfo->nvc.array[lineID];
}

uint64_t native_nvc_access(uint32_t procId, MemReq req){
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
                 cycles += zinfo->nvc.write_accLat;
                 break;
             case GETX:
                 cycles += zinfo->nvc.write_accLat;
                 break;
             case PUTS:
                 cycles += zinfo->nvc.write_accLat;
                 break;
             case PUTX:
                 cycles += zinfo->nvc.write_accLat;
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
                 cycles += zinfo->nvc.read_accLat;
                 break;
             case GETX:
                 cycles += zinfo->nvc.read_accLat;
                 break;
             case PUTS:
                 //cycles += zinfo->nvc.write_accLat;
                 break;
             case PUTX:
                 cycles += zinfo->nvc.write_accLat;
                 break;
             default:
                 break;
         }
     }
     futex_unlock(&zinfo->nvc_lock);
     printf("nvc access cycle: %lu\n", cycles );
     return cycles; 
}

uint32_t native_nvc_preinsert(uint32_t procId, MemReq req){
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


void native_nvc_postinsert(uint32_t procId, MemReq req, int32_t lineID){
    switch (req.type) {
         case GETS:
             zinfo->nvc.array[lineID]=req.lineAddr;
             if (zinfo->nvc.state[lineID] != M) zinfo->nvc.state[lineID]=E; 
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

uint64_t native_nvc_evict(uint32_t procId, MemReq req, const int32_t lineID){
    uint64_t cycles;
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
    cycles = dram_access(procId, newreq); 
    zinfo->nvc.state[lineID] = I;
    return cycles; 
}


uint64_t native_nvc_fetch(uint32_t procId, MemReq req){
    return dram_access(procId, req); 
}


/************************* dram    ***************************/

int32_t native_dram_lookup(uint32_t procId, Address lineAddr){
    //range
    Address start = (lineAddr % zinfo->dram.numSets)*DRAM_WAYS; 
    Address end = start + DRAM_WAYS; 
    for (uint32_t i = start; i<end; i++) {
         if ((zinfo->dram.array[i] == lineAddr) && (zinfo->dram.procId[i]==procId))
             return i;  
    }
    return -1; 
}

Address native_dram_reverse_lookup(uint32_t procId, const int32_t lineID){
     return zinfo->dram.array[lineID];
}

uint64_t native_dram_access(uint32_t procId, MemReq req){
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
         cycles += zinfo->dram.accLat;
     } else { //hit
         if (req.type == GETS) zinfo->pc.dram_hGETS++;
         else if (req.type == GETX) zinfo->pc.dram_hGETX++;
         dram_postinsert(procId, req, target_lineId);
         if (req.type!=PUTS) cycles += zinfo->dram.accLat; 
     }
     futex_unlock(&zinfo->dram_lock);
     printf("dram access cycle: %lu\n", cycles );
     return cycles; 
}

uint32_t native_dram_preinsert(uint32_t procId, MemReq req){
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


void native_dram_postinsert(uint32_t procId, MemReq req, int32_t lineID){
    switch (req.type) {
         case GETS:
             zinfo->dram.array[lineID]=req.lineAddr;
             if (zinfo->dram.state[lineID]!=M) zinfo->dram.state[lineID]=E; 
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

uint64_t native_dram_evict(uint32_t procId, MemReq req, const int32_t lineID){
    uint64_t cycles;
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
    cycles = nvm_access(procId, newreq); 
    zinfo->dram.state[lineID] = I; 
    return cycles; 
}

uint64_t native_dram_fetch(uint32_t procId, MemReq req){
    return nvm_access(procId, req); 
}

/************************* nvm ****************************/

uint64_t native_nvm_access(uint32_t procId, MemReq req){
     futex_lock(&zinfo->nvm_lock);
     uint64_t cycles = req.cycle;
     switch (req.type) {
         case GETS:
             cycles += zinfo->nvm.read_accLat;
             zinfo->pc.nvm_GETS++;
             break;
         case GETX:
             cycles += zinfo->nvm.read_accLat; 
             zinfo->pc.nvm_GETX++;
             break;
         case PUTS: 
             //cycles += zinfo->nvm.write_accLat;
             zinfo->pc.nvm_PUTS++;
             break; 
         case PUTX:
             zinfo->pc.nvm_PUTX++;
             cycles += zinfo->nvm.write_accLat; 
             break; 
         default:
             break;
     } 
     futex_unlock(&zinfo->nvm_lock);
     printf("nvm access cycle: %lu\n", cycles );
     return cycles; 
}

