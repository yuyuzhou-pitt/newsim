/* This the epb's configure */

#include<stdint.h>
#include"galloc.h"

#define COMMAND "ls"
#define FASTFORWARD true
#define FF_INS 10000

#define PHASE_LENGTH 10000;

#define PIN  "/home/yuyuzhou/epb/pin/pin-2.10-45467-gcc.3.4.6-ia32_intel64-linux/pin"
#define ARGS "-t obj-intel64/libsim.so -- ls"
#define numProcs 4

#define L1D_SIZE 32768   //32KB
#define L1D_LATENCY 4    // 4 cycle
#define L1D_WAYS 4


#define L1I_SIZE 32768
#define L1I_LATENCY 4
#define L1I_WAYS 4

#define L2_SIZE 226214   //256KB
#define L2_LATENCY 7
#define L2_WAYS 8

//   NVC
#define NVC_SIZE 4194304   //4MB
#define NVC_READ_LATENCY 7
#define NVC_WRITE_LATENCY 57
#define NVC_WAYS 16

// on package dram
#define DRAM_SIZE 67108864   //64MB
#define DRAM_LATENCY 114  //50ns
#define DRAM_WAYS 16
#define DRAM_BANKS 8

// NVM
#define NVM_ROW_HIT_LATENCY 114   //50ns
#define NVM_READ_LATENCY 340 //300ns
#define NVM_WRITE_LATENCY 2270 //1000ns

class SimpleCore : public GlobAlloc{
    private: 
        uint64_t lastUpdateCycles;
        uint64_t lastUpdateInstrs;

    public:
        SimpleCore() {lastUpdateCycles=0; lastUpdateInstrs=0;}
        uint64_t getInstrs() const {return lastUpdateInstrs;}
        uint64_t getCycles() const {return lastUpdateCycles;}
}; 


struct GlobSimInfo {
    uint32_t cycles[8];
    uint32_t FastForwardIns[8]; 
    uint32_t phaseLength;
    bool FastForward;
    SimpleCore core[8];
};

extern GlobSimInfo* zinfo;
