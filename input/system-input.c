#include <unistd.h>
#include <stdio.h>
int main(int argc,char**argv){if(setgid(1000)||setuid(1000)){perror("uid");return 1;}execvp(argv[1],argv+1);perror("exec");return 1;}
