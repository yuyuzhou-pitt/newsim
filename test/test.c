#include<stdio.h>

int a, b;

void hello()
{
   a = 3; 
   b = 4;
}


void main()
{
int i;

for (i =0; i<10; i++) {
__asm__ ("lfence");
   hello();
__asm__ ("sfence");
}
}
