#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <cassert>
#include <iostream>
#include <sys/personality.h>

#include "epb.h"

using namespace std;

#define PIN  "/home/yuyuzhou/epb/pin/pin-2.10-45467-gcc.3.4.6-ia32_intel64-linux/pin"
#define ARGS "-t obj-intel64/libsim.so -- ls"
#define numProcs 8

typedef enum {
    PS_INVALID,
    PS_RUNNING,
    PS_DONE,
} ProcStatus;

struct ProcInfo {
    int pid;
    volatile ProcStatus status;
};

ProcInfo childInfo[numProcs];
bool aslr = false;


void LaunchProcess(uint32_t procIdx) {
    const char *aptrs[10];
    aptrs[0]=PIN;
    aptrs[1]="-t";
    aptrs[2]="obj-intel64/libsim.so";
    aptrs[3]="--";
    aptrs[4]="ls";
    aptrs[5]=nullptr;

    int cpid = fork();
    if (cpid) { //parent
        assert(cpid > 0);
        childInfo[procIdx].pid = cpid;
        childInfo[procIdx].status = PS_RUNNING;
    } else if (cpid < 0)            // failed to fork
    {
        perror("Failed to fork");
        return;
    } else { //child
        /* In a modern kernel, we must disable address space randomization. Otherwise,
         * different zsim processes will load zsim.so on different addresses,
         * which would be fine except that the vtable pointers will be different
         * per process, and virtual functions will not work.
         *
         * WARNING: The harness itself is run with randomization on, which should
         * be fine because it doesn't load zsim.so anyway. If this changes at some
         * point, we'll need to have the harness be executed via a wrapper that just
         * changes the personalily and forks, or run the harness with setarch -R
         */
        if (!aslr) {
            //Get old personality flags & update
            int pers = personality(((unsigned int)-1) /*returns current pers flags; arg is a long, hence the cast, see man*/);
            if (pers == -1 || personality(pers | ADDR_NO_RANDOMIZE) == -1) {
                perror("personality() call failed");
                perror("Could not change personality to disable address space randomization!");
            }
            int newPers = personality(((unsigned int)-1));
            if ((newPers & ADDR_NO_RANDOMIZE) == 0) perror("personality() call was not honored!");
        }
      
        //cout << "child" << aptrs[0] <<endl;
        if (execvp(aptrs[0], (char* const*)aptrs) == -1) {
            perror("Could not exec, killing child");
        } 
    }
}


int main(int argc, char *argv[])
{
    for (uint32_t procIdx = 0; procIdx < numProcs; procIdx++) {
        LaunchProcess(procIdx);
    }
 
   return 0;
}
