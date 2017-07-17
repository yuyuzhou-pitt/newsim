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




uint64_t flush(uint32_t procId, uint64_t PersistTrax){
    //info("PB flush %lu", PersistTrax);
    //pb_info();
    int i;
    uint64_t cycles = 0; 
    for (i = 0; i < PB_SIZE; i++) {
        if (zinfo->pb[procId][i].tx_id <= PersistTrax) {
             if (zinfo->pb[procId][i].level == CL3) { // in NVC alredy 
                 cycles = cycles + 1; 
                 zinfo->pb[procId][i].tx_id = -1;
                 zinfo->pb[procId][i].level = NONE;
                 zinfo->pb[procId][i].lineId = -1;
                 zinfo->pb[procId][i].lineAddr = -1;
             }
             else { // flush NVC
                 cycles = cycles + 1 + NVC_WRITE_LATENCY; 
                 zinfo->pb[procId][i].tx_id = -1;
                 zinfo->pb[procId][i].level = NONE;
                 zinfo->pb[procId][i].lineId = -1;
                 zinfo->pb[procId][i].lineAddr = -1;
             }
        } 
    }

    return cycles;
}

uint64_t overflow_flush(uint32_t procId, uint64_t PersistTrax){
    int i;
    uint64_t cycles = 0;
    for (i = 0; i < PB_SIZE; i++) {
        if (zinfo->pb[procId][i].tx_id <= PersistTrax) {
              // flush NVM
                 cycles = cycles + 1 + 2*NVM_WRITE_LATENCY;
                 zinfo->pb[procId][i].tx_id = -1;
                 zinfo->pb[procId][i].level = NONE;
                 zinfo->pb[procId][i].lineId = -1;
                 zinfo->pb[procId][i].lineAddr = -1;
             
        }
    }
    return cycles;
}


uint64_t insert_PB(uint32_t procId, uint64_t buffer_lineID, uint64_t epoch_id, CacheLevel cl, uint64_t lineID,Address lineAddr){
    //info("PB insert"); 
    //pb_info();

   if (epoch_id  < zinfo->nextPersistTrax[procId]) // persisted data, no need to insert. 
        return 0; 

    for (int i=0; i < PB_SIZE; i ++ ){
        if (zinfo->pb[procId][i].lineAddr == lineAddr) buffer_lineID =i;
    }

    if ((zinfo->pb[procId][buffer_lineID].tx_id != epoch_id) && (zinfo->pb[procId][buffer_lineID].tx_id != (uint64_t)(-1) )) { //buffer is full
        info("Error buffeer is full, buffer_id %lu, epoch_id %lu, nexPer %lu", buffer_lineID, epoch_id, zinfo->nextPersistTrax[procId]);
        overflow_flush(procId, epoch_id);
    }
       if (zinfo->pb[procId][buffer_lineID].tx_id != epoch_id) // a new entry
           zinfo->nextAvailablePBLine[procId] = (zinfo->nextAvailablePBLine[procId] + 1) % PB_SIZE; 
       zinfo->pb[procId][buffer_lineID].tx_id = epoch_id;
       zinfo->pb[procId][buffer_lineID].level = cl;
       zinfo->pb[procId][buffer_lineID].lineId = lineID;        
       zinfo->pb[procId][buffer_lineID].lineAddr = lineAddr;
    return 1; 
}



/***********************l1 ************************************/

int32_t kiln_l1_lookup(uint32_t procId, Address lineAddr){
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

Address kiln_l1_reverse_lookup(uint32_t procId, const int32_t lineID){
     return zinfo->l1cache[procId].array[lineID];
}

uint64_t kiln_l1_access(uint32_t procId, MemReq req){
     //info("l1 access"); 
     uint64_t cycles = req.cycle;
     if (req.persistent == true) {  // it's a persistent write
         if (req.epoch_id > zinfo->nextPersistTrax[procId]) { // new trax, flush previous trax
             cycles+=flush(procId, zinfo->nextPersistTrax[procId]);
             zinfo->nextPersistTrax[procId]++;              
         } else if (req.epoch_id < zinfo->nextPersistTrax[procId]) { // old data treated as non volatile
             req.epoch_id=-1; 
             req.persistent = false; 
             req.pb_id=-1;
         }
     }

// votile write, or incomplete persistent write
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

         if (req.persistent == true) {  // it's a persistent write
             zinfo->l1cache[procId].tx_id[target_lineId]=req.epoch_id;
             if ( req.pb_id == (uint64_t)(-1))
                 zinfo->l1cache[procId].pb_line[target_lineId] = zinfo->nextAvailablePBLine[procId];
             else zinfo->l1cache[procId].pb_line[target_lineId] = req.pb_id;
             cycles+=insert_PB(procId, zinfo->l1cache[procId].pb_line[target_lineId],req.epoch_id, CL1, target_lineId, req.lineAddr);
         } else {
             zinfo->l1cache[procId].pb_line[target_lineId]=-1;
             zinfo->l1cache[procId].tx_id[target_lineId]=-1; 
         }

         l1_postinsert(procId, req, target_lineId);    
         cycles += zinfo->l1cache[procId].accLat - 1;
     } else { //hit 
         if (req.type == GETS) zinfo->pc.l1_hGETS[procId]++;
         else if (req.type == GETX) zinfo->pc.l1_hGETX[procId]++;

         if (req.persistent == true) {  // it's a persistent write
             zinfo->l1cache[procId].tx_id[target_lineId]=req.epoch_id;
             if ( req.pb_id == (uint64_t)(-1))
                 zinfo->l1cache[procId].pb_line[target_lineId] = zinfo->nextAvailablePBLine[procId];
             else zinfo->l1cache[procId].pb_line[target_lineId] = req.pb_id;
             cycles+=insert_PB(procId, zinfo->l1cache[procId].pb_line[target_lineId],req.epoch_id, CL1, target_lineId, req.lineAddr);
         } else {
             zinfo->l1cache[procId].pb_line[target_lineId]=-1;
             zinfo->l1cache[procId].tx_id[target_lineId]=-1; 
         }

         l1_postinsert(procId, req, target_lineId);
         if (req.type!=PUTS) cycles += zinfo->l1cache[procId].accLat - 1; 
     }
     return cycles; 

}

uint32_t kiln_l1_preinsert(uint32_t procId, MemReq req){
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


void kiln_l1_postinsert(uint32_t procId, MemReq req, int32_t lineID){
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

uint64_t kiln_l1_evict(uint32_t procId, MemReq req, const int32_t lineID){
    MemReq newreq; 
    newreq.lineAddr = zinfo->l1cache[procId].array[lineID]; 
    newreq.cycle = req.cycle; 
    //info("kiln evict %d", lineID);
    if (zinfo->l1cache[procId].pb_line[lineID] == (uint64_t)(-1)) {
        //info("not persist");
        newreq.persistent = false;
        newreq.epoch_id = -1;
        newreq.pb_id = -1; 
    }
    else { // persist
        //info("persist %lu", zinfo->l1cache[procId].pb_line[lineID] );
        newreq.persistent = true;
        newreq.epoch_id = zinfo->l1cache[procId].tx_id[lineID]; 
        newreq.pb_id = zinfo->l1cache[procId].pb_line[lineID];
    }
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

uint64_t kiln_l1_fetch(uint32_t procId, MemReq req){
    return l2_access(procId, req); 
}


/****************l2 *******************************/
int32_t kiln_l2_lookup(uint32_t procId, Address lineAddr){
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

Address kiln_l2_reverse_lookup(uint32_t procId, const int32_t lineID){
     return zinfo->l2cache[procId].array[lineID];
}

uint64_t kiln_l2_access(uint32_t procId, MemReq req){
     //info("l2 access"); 
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

         if (req.persistent == true) {  // it's a persistent write
             zinfo->l2cache[procId].tx_id[target_lineId]=req.epoch_id;
             if ( req.pb_id == (uint64_t)(-1))
                 zinfo->l2cache[procId].pb_line[target_lineId] = zinfo->nextAvailablePBLine[procId];
             else zinfo->l2cache[procId].pb_line[target_lineId] = req.pb_id;
             cycles+=insert_PB(procId, zinfo->l2cache[procId].pb_line[target_lineId],req.epoch_id, CL2, target_lineId, req.lineAddr);
         } else {
             zinfo->l2cache[procId].pb_line[target_lineId]=-1;
             zinfo->l2cache[procId].tx_id[target_lineId]=-1; 
         }

         l2_postinsert(procId, req, target_lineId);    
         cycles += zinfo->l2cache[procId].accLat - 1;
     } else { //hit
         if (req.type == GETS) zinfo->pc.l2_hGETS[procId]++;
         else if (req.type == GETX) zinfo->pc.l2_hGETX[procId]++;

         if (req.persistent == true) {  // it's a persistent write
             zinfo->l2cache[procId].tx_id[target_lineId]=req.epoch_id;
             if ( req.pb_id == (uint64_t)(-1))
                 zinfo->l2cache[procId].pb_line[target_lineId] = zinfo->nextAvailablePBLine[procId];
             else zinfo->l2cache[procId].pb_line[target_lineId] = req.pb_id;
             cycles+=insert_PB(procId, zinfo->l2cache[procId].pb_line[target_lineId],req.epoch_id, CL2, target_lineId, req.lineAddr);
         } else {
             zinfo->l2cache[procId].pb_line[target_lineId]=-1;
             zinfo->l2cache[procId].tx_id[target_lineId]=-1; 
         }

         l2_postinsert(procId, req, target_lineId);
         if (req.type != PUTS) cycles += zinfo->l2cache[procId].accLat - 1;  //clean write back hit does not need to wrtie
     }
     return cycles; 
}

uint32_t kiln_l2_preinsert(uint32_t procId, MemReq req){
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

void kiln_l2_postinsert(uint32_t procId, MemReq req, int32_t lineID){
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

uint64_t kiln_l2_evict(uint32_t procId, MemReq req, const int32_t lineID){
    MemReq newreq; 
    newreq.lineAddr = zinfo->l2cache[procId].array[lineID]; 
    newreq.cycle = req.cycle;

    if (zinfo->l2cache[procId].pb_line[lineID] == (uint64_t)(-1)) {
        newreq.persistent = false;
        newreq.epoch_id = -1;
        newreq.pb_id = -1;
    }
    else { // persist
        newreq.persistent = true;
        newreq.epoch_id = zinfo->l2cache[procId].tx_id[lineID];
        newreq.pb_id = zinfo->l2cache[procId].pb_line[lineID];
    }


 
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


uint64_t kiln_l2_fetch(uint32_t procId, MemReq req){
    return nvc_access(procId, req); 
}



/******************** nvc *************************************/

int32_t kiln_nvc_lookup(uint32_t procId, Address lineAddr){
    //range
    Address start = (lineAddr % zinfo->nvc.numSets)*NVC_WAYS; 
    Address end = start + NVC_WAYS; 
    for (uint32_t i = start; i<end; i++) {
         if (zinfo->nvc.array[i] == lineAddr)
             return i;  
    }
    return -1; 
}

Address kiln_nvc_reverse_lookup(uint32_t procId, const int32_t lineID){
     return zinfo->nvc.array[lineID];
}

uint64_t kiln_nvc_access(uint32_t procId, MemReq req){
     //info("nvc access"); 
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

         if (req.persistent == true) {  // it's a persistent write
             zinfo->nvc.tx_id[target_lineId]=req.epoch_id;
             if ( req.pb_id == (uint64_t)(-1))
                 zinfo->nvc.pb_line[target_lineId] = zinfo->nextAvailablePBLine[procId];
             else zinfo->nvc.pb_line[target_lineId] = req.pb_id;
             cycles+=insert_PB(procId, zinfo->nvc.pb_line[target_lineId],req.epoch_id, CL3, target_lineId, req.lineAddr);
         } else {
             zinfo->nvc.pb_line[target_lineId]=-1;
             zinfo->nvc.tx_id[target_lineId]=-1; 
         }

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

         if (req.persistent == true) {  // it's a persistent write
             zinfo->nvc.tx_id[target_lineId]=req.epoch_id;
             if ( req.pb_id == (uint64_t)(-1))
                 zinfo->nvc.pb_line[target_lineId] = zinfo->nextAvailablePBLine[procId];
             else zinfo->nvc.pb_line[target_lineId] = req.pb_id;
             cycles+=insert_PB(procId, zinfo->nvc.pb_line[target_lineId],req.epoch_id, CL3, target_lineId, req.lineAddr);
         } else {
             zinfo->nvc.pb_line[target_lineId]=-1;
             zinfo->nvc.tx_id[target_lineId]=-1; 
         }

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

uint32_t kiln_nvc_preinsert(uint32_t procId, MemReq req){
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


void kiln_nvc_postinsert(uint32_t procId, MemReq req, int32_t lineID){
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

uint64_t kiln_nvc_evict(uint32_t procId, MemReq req, const int32_t lineID){
    MemReq newreq; 
    newreq.lineAddr = zinfo->nvc.array[lineID]; 
    newreq.cycle = req.cycle; 

    if (zinfo->nvc.pb_line[lineID] == (uint64_t)(-1)) {
        newreq.persistent = false;
        newreq.epoch_id = -1;
        newreq.pb_id = -1;
    }
    else { // persist
        newreq.persistent = true;
        newreq.epoch_id = zinfo->nvc.tx_id[lineID];
        newreq.pb_id = zinfo->nvc.pb_line[lineID];
    }

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

    if (zinfo->nvc.pb_line[lineID] == (uint64_t)(-1)) 
        return dram_access(procId, newreq); 
    else return nvm_access(procId, newreq); 
}


uint64_t kiln_nvc_fetch(uint32_t procId, MemReq req){
    return dram_access(procId, req); 
}


/************************* dram    ***************************/

int32_t kiln_dram_lookup(uint32_t procId, Address lineAddr){
    //range
    Address start = (lineAddr % zinfo->dram.numSets)*DRAM_WAYS; 
    Address end = start + DRAM_WAYS; 
    for (uint32_t i = start; i<end; i++) {
         if (zinfo->dram.array[i] == lineAddr)
             return i;  
    }
    return -1; 
}

Address kiln_dram_reverse_lookup(uint32_t procId, const int32_t lineID){
     return zinfo->dram.array[lineID];
}

uint64_t kiln_dram_access(uint32_t procId, MemReq req){
     //info("dram access"); 
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

         if (req.persistent == true) {  // it's a persistent write
             zinfo->dram.tx_id[target_lineId]=req.epoch_id;
             if ( req.pb_id == (uint64_t)(-1))
                 zinfo->dram.pb_line[target_lineId] = zinfo->nextAvailablePBLine[procId];
             else zinfo->dram.pb_line[target_lineId] = req.pb_id;
             cycles+=insert_PB(procId, zinfo->dram.pb_line[target_lineId],req.epoch_id, CL4, target_lineId, req.lineAddr);
         } else {
             zinfo->dram.pb_line[target_lineId]=-1;
             zinfo->dram.tx_id[target_lineId]=-1; 
         }

         dram_postinsert(procId, req, target_lineId);    
         cycles += zinfo->dram.accLat - 1;
     } else { //hit
         if (req.type == GETS) zinfo->pc.dram_hGETS++;
         else if (req.type == GETX) zinfo->pc.dram_hGETX++;

         if (req.persistent == true) {  // it's a persistent write
             zinfo->dram.tx_id[target_lineId]=req.epoch_id;
             if ( req.pb_id == (uint64_t)(-1))
                 zinfo->dram.pb_line[target_lineId] = zinfo->nextAvailablePBLine[procId];
             else zinfo->dram.pb_line[target_lineId] = req.pb_id;
             cycles+=insert_PB(procId, zinfo->dram.pb_line[target_lineId],req.epoch_id, CL4, target_lineId, req.lineAddr);
         } else {
             zinfo->dram.pb_line[target_lineId]=-1;
             zinfo->dram.tx_id[target_lineId]=-1; 
         }

         dram_postinsert(procId, req, target_lineId);
         if (req.type!=PUTS) cycles += zinfo->dram.accLat - 1; 
     }
     futex_unlock(&zinfo->dram_lock);
     return cycles; 
}

uint32_t kiln_dram_preinsert(uint32_t procId, MemReq req){
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


void kiln_dram_postinsert(uint32_t procId, MemReq req, int32_t lineID){
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

uint64_t kiln_dram_evict(uint32_t procId, MemReq req, const int32_t lineID){
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

uint64_t kiln_dram_fetch(uint32_t procId, MemReq req){
    return nvm_access(procId, req); 
}

/************************* nvm ****************************/

uint64_t kiln_nvm_access(uint32_t procId, MemReq req){
     //info("nvc access"); 
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

