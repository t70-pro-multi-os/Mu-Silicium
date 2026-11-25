// SPDX-License-Identifier: BSD-2-Clause-Patent
#define _GNU_SOURCE
#include <assert.h>
#include <signal.h>
#include <sys/wait.h>
static pid_t MockWaitpid(pid_t P,int *Status,int Flags);
#define waitpid MockWaitpid
#define main init_main
#include "../Init.c"
#undef waitpid
#undef main

static int InjectStop;
static pid_t MockWaitpid(pid_t P,int *Status,int Flags)
{
  assert(Flags==WNOHANG);
  if (InjectStop) {
    InjectStop=0;
    // The signal arrives after the stop check but before the sleeping call.
    assert(raise(SIGTERM)==0 && !Stopping);
    return 0;
  }
  return waitpid(P,Status,Flags);
}

int main(void)
{
  int Status;
  setvbuf(stdout,NULL,_IONBF,0);
  alarm(4);
  assert(WatchChildren()==0);
  InjectStop=1;
  assert(WaitForChild(&Status,1)<0 && errno==EINTR && Stopping && !InjectStop);
  Stopping=0;
  raise(SIGINT);
  assert(Stopping); // The caller's original mask was restored.
  Stopping=0;
  pid_t P=fork();
  assert(P>=0);
  if (P==0) { usleep(25000); _exit(9); }
  assert(WaitForChild(&Status,1)==P && WIFEXITED(Status) && WEXITSTATUS(Status)==9);
  assert(WaitForChild(&Status,1)<0 && errno==ECHILD);
  alarm(0);
  puts("PASS: shutdown and child-exit wake without a lost signal");
  return 0;
}
