/* This the epb's configure */


#define COMMAND1 "ls"

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

