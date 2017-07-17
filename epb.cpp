#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <iostream>
#include <sys/personality.h>
#include <sys/wait.h>
#include "libsim.h"
#include "galloc.h"
#include "log.h"

using namespace std;

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

int getNumChildren() {
    int num = 0;
    for (int i = 0; i < 8; i++) {
        if (childInfo[i].status == PS_RUNNING) num++;
    }
    return num;
}

void LaunchProcess(uint32_t procIdx, uint32_t shmid) {
    char buffer1[10];
    char buffer2[10];

    const char *aptrs[10];
    aptrs[0]=PIN;
    aptrs[1]="-t";
    aptrs[2]="obj-intel64/libsim.so";
    aptrs[3]="-procIdx";
    snprintf(buffer1, sizeof(buffer1), "%d", procIdx);
    aptrs[4]=buffer1;
    aptrs[5]="-shmid";
    snprintf(buffer2, sizeof(buffer2), "%d", shmid);
    aptrs[6]=buffer2;
    aptrs[7]="--";
    aptrs[8]=COMMAND;
    aptrs[9]=nullptr;

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


int eraseChild(int pid) {
    for (int i = 0; i < 8; i++) {
        if (childInfo[i].pid == pid) {
            assert_msg(childInfo[i].status == PS_RUNNING, "i=%d pid=%d status=%d", i, pid, childInfo[i].status);
            childInfo[i].status = PS_DONE;
            return i;
        }
    }
    panic("Could not erase child!!");
}



int main(int argc, char *argv[])
{
    GlobSimInfo* zinfo = nullptr;

    uint32_t gmSize = 1; /*default 1MB*/
    info("Creating global segment, %d MBs", gmSize);
    int shmid = gm_init(((size_t)gmSize) << 20 /*MB to Bytes*/);
    info("Global segment shmid = %d", shmid);

    //sleep(180);


    for (uint32_t procIdx = 0; procIdx < numProcs; procIdx++) {
        LaunchProcess(procIdx, shmid);
    }  



    while (getNumChildren() > 0) {
        if (!gm_isready()) {
            usleep(1000);  // wait till proc idx 0 initializes everyhting
            continue;
        }

        if (zinfo == nullptr) {
            zinfo = static_cast<GlobSimInfo*>(gm_get_glob_ptr());
            printf("%s","Attached to global heap\n");
        }
        //This solves a weird race in multiprocess where SIGCHLD does not always fire...
        int cpid = -1;
        while ((cpid = waitpid(-1, nullptr, WNOHANG)) > 0) {
            eraseChild(cpid);
            info("Child %d done (in-loop catch)", cpid);
        }        

    } 



 
   return 0;
}



























