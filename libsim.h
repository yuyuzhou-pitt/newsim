/* This the epb's configure */
#ifndef LIBSIM_H_
#define LIBSIM_H_


#include <stdint.h>
#include "galloc.h"
#include "locks.h"

#define ARCHITECTURE EPB
#define COMMAND "./test/a.out"
#define FASTFORWARD true
#define FF_INS 1

#define PHASE_LENGTH 10000

#define PIN  "/home/yuyuzhou/epb/pin/pin-2.10-45467-gcc.3.4.6-ia32_intel64-linux/pin"
#define ARGS "-t obj-intel64/libsim.so -- ./test/a.out"
#define numProcs 1

#define L1D_SIZE 512   // make then extra small to help debuggin
//#define L1D_SIZE 32768   //32KB
#define L1D_LATENCY 4    // 4 cycle
#define L1D_WAYS 4


#define L1I_SIZE 512   // make then extra small to help debuggin
//#define L1I_SIZE 32768
#define L1I_LATENCY 4
#define L1I_WAYS 4


#define L2_SIZE 1024   // make then extra small to help debuggin
//#define L2_SIZE 226214   //256KB
#define L2_LATENCY 7
#define L2_WAYS 8

//   NVC
#define NVC_SIZE 2048   // make then extra small to help debuggin
//#define NVC_SIZE 4194304   //4MB
#define NVC_READ_LATENCY 7
#define NVC_WRITE_LATENCY 57
#define NVC_WAYS 16

// on package dram

#define DRAM_SIZE 2048   // make then extra small to help debuggin
//#define DRAM_SIZE 67108864   //64MB
#define DRAM_LATENCY 114  //50ns
#define DRAM_WAYS 16
#define DRAM_BANKS 8

// NVM
#define NVM_ROW_HIT_LATENCY 114   //50ns
#define NVM_READ_LATENCY 340 //300ns
#define NVM_WRITE_LATENCY 2270 //1000ns



typedef int64_t EPOCH_ID;
typedef int64_t EPOCH_SID;
typedef uint64_t Address;

typedef enum{
    NATIVE,  // no persistent suppport
    NVMLOG,  // undo log into nvm
    KILN,    // kiln
    EPB,     // epb
    EPB_BF   // epb with back flush. 
} Arch; 

typedef enum {
    GETS, // get line, exclusive permission not needed (triggered by a processor load)
    GETX, // get line, exclusive permission needed (triggered by a processor store o atomic access)
    PUTS, // clean writeback (lower cache is evicting this line, line was not modified)
    PUTX  // dirty writeback (lower cache is evicting this line, line was modified)
} AccessType;

typedef enum {
    INV,  // fully invalidate this line
    INVX, // invalidate exclusive access to this line (lower level can still keep a non-exclusive copy)
    FWD,  // don't invalidate, just send up the data (used by directories). Only valid on S lines.
} InvType;

typedef enum {
    I, // invalid
    S, // shared (and clean)
    E, // exclusive and clean
    M  // exclusive and dirty
} MESIState;

inline bool IsGet(AccessType t) { return t == GETS || t == GETX; }
inline bool IsPut(AccessType t) { return t == PUTS || t == PUTX; }

struct MemReq {
    Address lineAddr;
    AccessType type;
    uint64_t cycle; //cycle where request arrives at component
    bool persistent; // whethre it is a persistent request
    EPOCH_ID epoch_id;
    EPOCH_SID epoch_sid;
};


struct SimpleCore{
        uint64_t lastUpdateCycles;
        uint64_t lastUpdateInstrs;
}; 

void debug();

struct L1Cache{
        uint32_t accLat; //latency of a normal access, split in get/put
        Address array[L1D_SIZE/64];
        MESIState state[L1D_SIZE/64];
        uint64_t ts[L1D_SIZE/64]; //timestamp for LRU
        uint32_t numSets;

   /*     L1Cache(uint32_t _accLat, uint32_t numSets);
        int32_t lookup(const Address lineAddr, const MemReq* req, bool updateReplacement);
        Address reverse_lookup(const int32_t lineID);
        uint32_t preinsert(const Address lineAddr, const MemReq* req, Address* wbLineAddr);
        void postinsert(const Address lineAddr, const MemReq* req, uint32_t lineId);
        uint64_t access(MemReq& req); */
};


int32_t (*l1_lookup) (uint32_t procId, Address lineAddr);
Address (*l1_reverse_lookup) (uint32_t procId, const int32_t lineID);
uint64_t (*l1_access) (uint32_t procId, MemReq req);
uint64_t (*l1_evict) (uint32_t procId, MemReq req, const int32_t lineID);
uint32_t (*l1_preinsert) (uint32_t procId, MemReq req);
void (*l1_postinsert) (uint32_t procId, MemReq req, int32_t lineID);
uint64_t (*l1_fetch) (uint32_t procId, MemReq req); 



struct L2Cache{
        uint32_t accLat; //latency of a normal access, split in get/put
        Address array[L2_SIZE/64];
        MESIState state[L2_SIZE/64];
        uint64_t ts[L2_SIZE/64]; //timestamp for LRU
        uint32_t numSets;
};

int32_t (*l2_lookup) (uint32_t procId, Address lineAddr);
Address (*l2_reverse_lookup) (uint32_t procId, const int32_t lineID);
uint64_t (*l2_access) (uint32_t procId, MemReq req);
uint64_t (*l2_evict) (uint32_t procId, MemReq req, const int32_t lineID);
uint32_t (*l2_preinsert) (uint32_t procId, MemReq req);
void (*l2_postinsert)(uint32_t procId, MemReq req, int32_t lineID);
uint64_t (*l2_fetch) (uint32_t procId, MemReq req); 


struct NVCCache{
        uint32_t read_accLat; //latency of a normal access, split in get/put
        uint32_t write_accLat;
        Address array[NVC_SIZE/64];
        MESIState state[NVC_SIZE/64];
        uint64_t ts[NVC_SIZE/64]; //timestamp for LRU
        uint32_t numSets;
};

int32_t (*nvc_lookup) (uint32_t procId, Address lineAddr);
Address (*nvc_reverse_lookup) (uint32_t procId, const int32_t lineID);
uint64_t (*nvc_access) (uint32_t procId, MemReq req);
uint64_t (*nvc_evict) (uint32_t procId, MemReq req, const int32_t lineID);
uint32_t (*nvc_preinsert) (uint32_t procId, MemReq req);
void (*nvc_postinsert) (uint32_t procId, MemReq req, int32_t lineID);
uint64_t (*nvc_fetch) ( uint32_t procId, MemReq req); 

struct DRAM {
        uint32_t accLat; //latency of a normal access, split in get/put
        Address array[DRAM_SIZE/64];
        MESIState state[DRAM_SIZE/64];
        uint64_t ts[DRAM_SIZE/64]; //timestamp for LRU
        uint32_t numSets;
};

int32_t (*dram_lookup) (uint32_t procId, Address lineAddr);
Address (*dram_reverse_lookup) (uint32_t procId, const int32_t lineID);
uint64_t (*dram_access) (uint32_t procId, MemReq req);
uint64_t (*dram_evict) (uint32_t procId, MemReq req, const int32_t lineID);
uint32_t (*dram_preinsert) (uint32_t procId, MemReq req);
void (*dram_postinsert) (uint32_t procId, MemReq req, int32_t lineID);
uint64_t (*dram_fetch) (uint32_t procId, MemReq req); 


struct NVRAM{
        uint32_t read_accLat; //latency of a normal access, split in get/put
        uint32_t write_accLat;
};

uint64_t (*nvm_access) (uint32_t procId, MemReq req);

struct PerformanceCounters{
      uint64_t l1_hGETS[8]; // L1 GETS hits
      uint64_t l1_hGETX[8]; // L1 GETX hits
      uint64_t l1_mGETS[8]; // L1 GETS miss
      uint64_t l1_mGETX[8]; // L1 GETX miss
      uint64_t l1_PUTS[8]; // L1 PUTS
      uint64_t l1_PUTX[8]; // L1 PUTX
 
      uint64_t l2_hGETS[8]; // L2 GETS hits
      uint64_t l2_hGETX[8]; // L2 GETX hits
      uint64_t l2_mGETS[8]; // L2 GETS miss
      uint64_t l2_mGETX[8]; // L2 GETX miss
      uint64_t l2_PUTS[8]; // L2 PUTS
      uint64_t l2_PUTX[8]; // L2 PUTX

      uint64_t nvc_hGETS; // NVC GETS hits
      uint64_t nvc_hGETX; // NVC GETX hits
      uint64_t nvc_mGETS; // NVC GETS miss
      uint64_t nvc_mGETX; // NVC GETX miss
      uint64_t nvc_PUTS; // NVC PUTS
      uint64_t nvc_PUTX; // NVC PUTX

      uint64_t dram_hGETS; // DRAM GETS hits
      uint64_t dram_hGETX; // DRAM GETX hits
      uint64_t dram_mGETS; // DRAM GETS miss
      uint64_t dram_mGETX; // DRAM GETX miss
      uint64_t dram_PUTS; // DRAM PUTS
      uint64_t dram_PUTX; // DRAM PUTX

      uint64_t nvm_GETS; // NVM GETS
      uint64_t nvm_GETX; // NVM GETX
      uint64_t nvm_PUTS; // NVM PUTS
      uint64_t nvm_PUTX; // NVM PUTX

};

void atomic_add_timestamp();


// NATIVE
int32_t native_l1_lookup(uint32_t procId, Address lineAddr);
Address native_l1_reverse_lookup(uint32_t procId, const int32_t lineID);
uint64_t native_l1_access(uint32_t procId, MemReq req);
uint64_t native_l1_evict(uint32_t procId, MemReq req, const int32_t lineID);
uint32_t native_l1_preinsert(uint32_t procId, MemReq req);
void native_l1_postinsert(uint32_t procId, MemReq req, int32_t lineID);
uint64_t native_l1_fetch(uint32_t procId, MemReq req);
int32_t native_l2_lookup(uint32_t procId, Address lineAddr);
Address native_l2_reverse_lookup(uint32_t procId, const int32_t lineID);
uint64_t native_l2_access(uint32_t procId, MemReq req);
uint64_t native_l2_evict(uint32_t procId, MemReq req, const int32_t lineID);
uint32_t native_l2_preinsert(uint32_t procId, MemReq req);
void native_l2_postinsert(uint32_t procId, MemReq req, int32_t lineID);
uint64_t native_l2_fetch(uint32_t procId, MemReq req);
int32_t native_nvc_lookup(uint32_t procId, Address lineAddr);
Address native_nvc_reverse_lookup(uint32_t procId, const int32_t lineID);
uint64_t native_nvc_access(uint32_t procId, MemReq req);
uint64_t native_nvc_evict(uint32_t procId, MemReq req, const int32_t lineID);
uint32_t native_nvc_preinsert(uint32_t procId, MemReq req);
void native_nvc_postinsert(uint32_t procId, MemReq req, int32_t lineID);
uint64_t native_nvc_fetch( uint32_t procId, MemReq req);
int32_t native_dram_lookup(uint32_t procId, Address lineAddr);
Address native_dram_reverse_lookup(uint32_t procId, const int32_t lineID);
uint64_t native_dram_access(uint32_t procId, MemReq req);
uint64_t native_dram_evict(uint32_t procId, MemReq req, const int32_t lineID);
uint32_t native_dram_preinsert(uint32_t procId, MemReq req);
void native_dram_postinsert(uint32_t procId, MemReq req, int32_t lineID);
uint64_t native_dram_fetch(uint32_t procId, MemReq req);
uint64_t native_nvm_access(uint32_t procId, MemReq req);

// NVMLOG
int32_t nvmlog_l1_lookup(uint32_t procId, Address lineAddr);
Address nvmlog_l1_reverse_lookup(uint32_t procId, const int32_t lineID);
uint64_t nvmlog_l1_access(uint32_t procId, MemReq req);
uint64_t nvmlog_l1_evict(uint32_t procId, MemReq req, const int32_t lineID);
uint32_t nvmlog_l1_preinsert(uint32_t procId, MemReq req);
void nvmlog_l1_postinsert(uint32_t procId, MemReq req, int32_t lineID);
uint64_t nvmlog_l1_fetch(uint32_t procId, MemReq req);
int32_t nvmlog_l2_lookup(uint32_t procId, Address lineAddr);
Address nvmlog_l2_reverse_lookup(uint32_t procId, const int32_t lineID);
uint64_t nvmlog_l2_access(uint32_t procId, MemReq req);
uint64_t nvmlog_l2_evict(uint32_t procId, MemReq req, const int32_t lineID);
uint32_t nvmlog_l2_preinsert(uint32_t procId, MemReq req);
void nvmlog_l2_postinsert(uint32_t procId, MemReq req, int32_t lineID);
uint64_t nvmlog_l2_fetch(uint32_t procId, MemReq req);
int32_t nvmlog_nvc_lookup(uint32_t procId, Address lineAddr);
Address nvmlog_nvc_reverse_lookup(uint32_t procId, const int32_t lineID);
uint64_t nvmlog_nvc_access(uint32_t procId, MemReq req);
uint64_t nvmlog_nvc_evict(uint32_t procId, MemReq req, const int32_t lineID);
uint32_t nvmlog_nvc_preinsert(uint32_t procId, MemReq req);
void nvmlog_nvc_postinsert(uint32_t procId, MemReq req, int32_t lineID);
uint64_t nvmlog_nvc_fetch( uint32_t procId, MemReq req);
int32_t nvmlog_dram_lookup(uint32_t procId, Address lineAddr);
Address nvmlog_dram_reverse_lookup(uint32_t procId, const int32_t lineID);
uint64_t nvmlog_dram_access(uint32_t procId, MemReq req);
uint64_t nvmlog_dram_evict(uint32_t procId, MemReq req, const int32_t lineID)
;
uint32_t nvmlog_dram_preinsert(uint32_t procId, MemReq req);
void nvmlog_dram_postinsert(uint32_t procId, MemReq req, int32_t lineID);
uint64_t nvmlog_dram_fetch(uint32_t procId, MemReq req);
uint64_t nvmlog_nvm_access(uint32_t procId, MemReq req);

//KILN
int32_t kiln_l1_lookup(uint32_t procId, Address lineAddr);
Address kiln_l1_reverse_lookup(uint32_t procId, const int32_t lineID);
uint64_t kiln_l1_access(uint32_t procId, MemReq req);
uint64_t kiln_l1_evict(uint32_t procId, MemReq req, const int32_t lineID);
uint32_t kiln_l1_preinsert(uint32_t procId, MemReq req);
void kiln_l1_postinsert(uint32_t procId, MemReq req, int32_t lineID);
uint64_t kiln_l1_fetch(uint32_t procId, MemReq req);
int32_t kiln_l2_lookup(uint32_t procId, Address lineAddr);
Address kiln_l2_reverse_lookup(uint32_t procId, const int32_t lineID);
uint64_t kiln_l2_access(uint32_t procId, MemReq req);
uint64_t kiln_l2_evict(uint32_t procId, MemReq req, const int32_t lineID);
uint32_t kiln_l2_preinsert(uint32_t procId, MemReq req);
void kiln_l2_postinsert(uint32_t procId, MemReq req, int32_t lineID);
uint64_t kiln_l2_fetch(uint32_t procId, MemReq req);
int32_t kiln_nvc_lookup(uint32_t procId, Address lineAddr);
Address kiln_nvc_reverse_lookup(uint32_t procId, const int32_t lineID);
uint64_t kiln_nvc_access(uint32_t procId, MemReq req);
uint64_t kiln_nvc_evict(uint32_t procId, MemReq req, const int32_t lineID);
uint32_t kiln_nvc_preinsert(uint32_t procId, MemReq req);
void kiln_nvc_postinsert(uint32_t procId, MemReq req, int32_t lineID);
uint64_t kiln_nvc_fetch( uint32_t procId, MemReq req);
int32_t kiln_dram_lookup(uint32_t procId, Address lineAddr);
Address kiln_dram_reverse_lookup(uint32_t procId, const int32_t lineID);
uint64_t kiln_dram_access(uint32_t procId, MemReq req);
uint64_t kiln_dram_evict(uint32_t procId, MemReq req, const int32_t lineID)
;
uint32_t kiln_dram_preinsert(uint32_t procId, MemReq req);
void kiln_dram_postinsert(uint32_t procId, MemReq req, int32_t lineID);
uint64_t kiln_dram_fetch(uint32_t procId, MemReq req);
uint64_t kiln_nvm_access(uint32_t procId, MemReq req);


//EPB
int32_t epb_l1_lookup(uint32_t procId, Address lineAddr);
Address epb_l1_reverse_lookup(uint32_t procId, const int32_t lineID);
uint64_t epb_l1_access(uint32_t procId, MemReq req);
uint64_t epb_l1_evict(uint32_t procId, MemReq req, const int32_t lineID);
uint32_t epb_l1_preinsert(uint32_t procId, MemReq req);
void epb_l1_postinsert(uint32_t procId, MemReq req, int32_t lineID);
uint64_t epb_l1_fetch(uint32_t procId, MemReq req);
int32_t epb_l2_lookup(uint32_t procId, Address lineAddr);
Address epb_l2_reverse_lookup(uint32_t procId, const int32_t lineID);
uint64_t epb_l2_access(uint32_t procId, MemReq req);
uint64_t epb_l2_evict(uint32_t procId, MemReq req, const int32_t lineID);
uint32_t epb_l2_preinsert(uint32_t procId, MemReq req);
void epb_l2_postinsert(uint32_t procId, MemReq req, int32_t lineID);
uint64_t epb_l2_fetch(uint32_t procId, MemReq req);
int32_t epb_nvc_lookup(uint32_t procId, Address lineAddr);
Address epb_nvc_reverse_lookup(uint32_t procId, const int32_t lineID);
uint64_t epb_nvc_access(uint32_t procId, MemReq req);
uint64_t epb_nvc_evict(uint32_t procId, MemReq req, const int32_t lineID);
uint32_t epb_nvc_preinsert(uint32_t procId, MemReq req);
void epb_nvc_postinsert(uint32_t procId, MemReq req, int32_t lineID);
uint64_t epb_nvc_fetch( uint32_t procId, MemReq req);
int32_t epb_dram_lookup(uint32_t procId, Address lineAddr);
Address epb_dram_reverse_lookup(uint32_t procId, const int32_t lineID);
uint64_t epb_dram_access(uint32_t procId, MemReq req);
uint64_t epb_dram_evict(uint32_t procId, MemReq req, const int32_t lineID)
;
uint32_t epb_dram_preinsert(uint32_t procId, MemReq req);
void epb_dram_postinsert(uint32_t procId, MemReq req, int32_t lineID);
uint64_t epb_dram_fetch(uint32_t procId, MemReq req);
uint64_t epb_nvm_access(uint32_t procId, MemReq req);


//EPB_BF
int32_t epb_bf_l1_lookup(uint32_t procId, Address lineAddr);
Address epb_bf_l1_reverse_lookup(uint32_t procId, const int32_t lineID);
uint64_t epb_bf_l1_access(uint32_t procId, MemReq req);
uint64_t epb_bf_l1_evict(uint32_t procId, MemReq req, const int32_t lineID);
uint32_t epb_bf_l1_preinsert(uint32_t procId, MemReq req);
void epb_bf_l1_postinsert(uint32_t procId, MemReq req, int32_t lineID);
uint64_t epb_bf_l1_fetch(uint32_t procId, MemReq req);
int32_t epb_bf_l2_lookup(uint32_t procId, Address lineAddr);
Address epb_bf_l2_reverse_lookup(uint32_t procId, const int32_t lineID);
uint64_t epb_bf_l2_access(uint32_t procId, MemReq req);
uint64_t epb_bf_l2_evict(uint32_t procId, MemReq req, const int32_t lineID);
uint32_t epb_bf_l2_preinsert(uint32_t procId, MemReq req);
void epb_bf_l2_postinsert(uint32_t procId, MemReq req, int32_t lineID);
uint64_t epb_bf_l2_fetch(uint32_t procId, MemReq req);
int32_t epb_bf_nvc_lookup(uint32_t procId, Address lineAddr);
Address epb_bf_nvc_reverse_lookup(uint32_t procId, const int32_t lineID);
uint64_t epb_bf_nvc_access(uint32_t procId, MemReq req);
uint64_t epb_bf_nvc_evict(uint32_t procId, MemReq req, const int32_t lineID);
uint32_t epb_bf_nvc_preinsert(uint32_t procId, MemReq req);
void epb_bf_nvc_postinsert(uint32_t procId, MemReq req, int32_t lineID);
uint64_t epb_bf_nvc_fetch( uint32_t procId, MemReq req);
int32_t epb_bf_dram_lookup(uint32_t procId, Address lineAddr);
Address epb_bf_dram_reverse_lookup(uint32_t procId, const int32_t lineID);
uint64_t epb_bf_dram_access(uint32_t procId, MemReq req);
uint64_t epb_bf_dram_evict(uint32_t procId, MemReq req, const int32_t lineID)
;
uint32_t epb_bf_dram_preinsert(uint32_t procId, MemReq req);
void epb_bf_dram_postinsert(uint32_t procId, MemReq req, int32_t lineID);
uint64_t epb_bf_dram_fetch(uint32_t procId, MemReq req);
uint64_t epb_bf_nvm_access(uint32_t procId, MemReq req);


struct GlobSimInfo {
    Arch arch; 
    lock_t lock; 
    lock_t nvc_lock;
    lock_t dram_lock;
    lock_t nvm_lock;
    uint64_t timestamp;
    uint32_t cycles[8];
    uint32_t FastForwardIns[8]; 
    uint32_t phaseLength;
    bool FastForward;
    uint32_t phase[8];
    uint32_t global_phase;
    SimpleCore core[8];
    L1Cache l1cache[8];
    L2Cache l2cache[8];
    NVCCache nvc;
    DRAM dram;
    NVRAM nvm;
    PerformanceCounters pc; 
    bool persistent[8];
    uint64_t tx_id[8];
};

//extern GlobSimInfo* zinfo;

GlobSimInfo* zinfo;
#endif // LIBSIM_H_
