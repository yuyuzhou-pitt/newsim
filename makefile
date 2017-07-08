PIN_HOME=/home/yuyuzhou/epb/pin/pin-2.10-45467-gcc.3.4.6-ia32_intel64-linux/

epb: newsim
	g++ -std=c++0x epb.h epb.cpp -o epb

newsim:
	mkdir obj-intel64
	g++ -c -Wall -std=c++11 -Werror -Wno-unknown-pragmas  -O3 -fomit-frame-pointer -DBIGARRAY_MULTIPLIER=1 -DUSING_XED  -fno-strict-aliasing -I$(PIN_HOME)/source/Include -I$(PIN_HOME)/source/InstLib -I$(PIN_HOME)/extras/xed2-intel64/include -I$(PIN_HOME)/extras/components/include -I$(PIN_HOME)/source/include -I$(PIN_HOME)/source/include/gen -fno-stack-protector -DTARGET_IA32E -DHOST_IA32E -fPIC -DTARGET_LINUX -O3 -fomit-frame-pointer -o obj-intel64/libsim.o libsim.cpp
	g++  -Wl,--hash-style=sysv -shared -Wl,-Bsymbolic -Wl,--version-script=$(PIN_HOME)/source/include/pintool.ver -L$(PIN_HOME)/source/Lib/ -L$(PIN_HOME)/source/ExtLib/ -L$(PIN_HOME)/extras/xed2-intel64/lib -L$(PIN_HOME)/intel64/lib -L$(PIN_HOME)/intel64/lib-ext  -o obj-intel64/libsim.so obj-intel64/libsim.o -L$(PIN_HOME)/source/Lib/ -L$(PIN_HOME)/source/ExtLib/ -L$(PIN_HOME)/extras/xed2-intel64/lib -L$(PIN_HOME)/intel64/lib -L$(PIN_HOME)/intel64/lib-ext -lpin  -lxed -ldwarf -lelf -ldl

clean:
	rm -rf obj-intel64 epb *.log *.out
