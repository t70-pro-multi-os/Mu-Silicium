// SPDX-License-Identifier: BSD-2-Clause-Patent
// Check the production initial-scan wait against isolated Linux children.
#define main boot_main
#include "../Init.c"
#undef main
#include <assert.h>

static pid_t child(int code)
{
  pid_t p=fork();
  assert(p>=0);
  if (p==0) {
    if (code>=0) _exit(code);
    for (;;) pause();
  }
  return p;
}
static void killed(pid_t p)
{
  int status=0;
  assert(waitpid(p,&status,0)==p);
  assert(WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL);
}
int main(void)
{
  assert(WaitForScan(child(0),2000)==0);
  assert(WaitForScan(child(3),2000)<0 && errno==EIO);
  pid_t p=child(-1);
  assert(WaitForScan(p,40)<0 && errno==ETIMEDOUT);
  killed(p);

  p=child(-1);
  Stopping=1;
  assert(WaitForScan(p,2000)<0 && errno==EINTR);
  killed(p);
  Stopping=0;
  // A stale PID must never be signaled once waitpid has reaped it.
  assert(WaitForScan(p,2000)<0 && errno==ECHILD);
  puts("PASS: initial scan success, failure, deadline, stop, stale child");
  return 0;
}
