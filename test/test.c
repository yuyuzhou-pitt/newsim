#include<stdio.h>

void main()
{
int i;
for (i =0; i<10; i++) {
__asm__ ("lfence");
   int a = 3;
   int b = 4;
   a = a+b;
__asm__ ("sfence");
}
}
