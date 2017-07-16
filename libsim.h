/* This the epb's configure */

#include<stdint.h>
#include"galloc.h"

#define COMMAND "./test/a.out"
#define FASTFORWARD true
#define FF_INS 10000

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


int32_t l1_lookup(uint32_t procId, Address lineAddr);
Address l1_reverse_lookup(uint32_t procId, const int32_t lineID);
uint64_t l1_access(uint32_t procId, MemReq req);
uint64_t l1_evict(uint32_t procId, MemReq req, const int32_t lineID);
uint32_t l1_preinsert(uint32_t procId, MemReq req);
void l1_postinsert(uint32_t procId, MemReq req, int32_t lineID);
uint64_t l1_fetch(uint32_t procId, MemReq req); 



struct L2Cache{
        uint32_t accLat; //latency of a normal access, split in get/put
        Address array[L2_SIZE/64];
        MESIState state[L2_SIZE/64];
        uint64_t ts[L2_SIZE/64]; //timestamp for LRU
        uint32_t numSets;
};

int32_t l2_lookup(uint32_t procId, Address lineAddr);
Address l2_reverse_lookup(uint32_t procId, const int32_t lineID);
uint64_t l2_access(uint32_t procId, MemReq req);
uint64_t l2_evict(uint32_t procId, MemReq req, const int32_t lineID);
uint32_t l2_preinsert(uint32_t procId, MemReq req);
void l2_postinsert(uint32_t procId, MemReq req, int32_t lineID);
uint64_t l2_fetch(uint32_t procId, MemReq req); 


struct NVCCache{
        uint32_t read_accLat; //latency of a normal access, split in get/put
        uint32_t write_accLat;
        Address array[NVC_SIZE/64];
        MESIState state[NVC_SIZE/64];
        uint64_t ts[NVC_SIZE/64]; //timestamp for LRU
        uint32_t numSets;
};

int32_t nvc_lookup(Address lineAddr);
Address nvc_reverse_lookup(const int32_t lineID);
uint64_t nvc_access(MemReq req);
uint64_t nvc_evict(MemReq req, const int32_t lineID);
uint32_t nvc_preinsert(MemReq req);
void nvc_postinsert(MemReq req, int32_t lineID);
uint64_t nvc_fetch( MemReq req); 

struct DRAM {
        uint32_t accLat; //latency of a normal access, split in get/put
        Address array[DRAM_SIZE/64];
        MESIState state[DRAM_SIZE/64];
        uint64_t ts[DRAM_SIZE/64]; //timestamp for LRU
        uint32_t numSets;
};

int32_t dram_lookup(Address lineAddr);
Address dram_reverse_lookup(const int32_t lineID);
uint64_t dram_access(MemReq req);
uint64_t dram_evict(MemReq req, const int32_t lineID);
uint32_t dram_preinsert(MemReq req);
void dram_postinsert(MemReq req, int32_t lineID);
uint64_t dram_fetch( MemReq req); 


struct NVRAM{
        uint32_t read_accLat; //latency of a normal access, split in get/put
        uint32_t write_accLat;
};

uint64_t nvm_access(MemReq req);


struct GlobSimInfo {
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
};

extern GlobSimInfo* zinfo;
