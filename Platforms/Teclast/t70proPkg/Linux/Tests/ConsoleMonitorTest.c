// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual monitor lifecycle; no device enumeration or DRM/input device open.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <errno.h>
#include <libudev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
static int Result;
static mode_t NodeMode;
static dev_t NodeDevice;
static unsigned Checks,Channels;
#define CHECK(X) do { ++Checks;if(!(X)){fprintf(stderr,"%u: %s\n",__LINE__,#X);abort();} } while(0)
static int TestLstat(const char *Path,struct stat *S) {
  CHECK(!strcmp(Path,"/fixture/event0"));
  if (Result) { errno=ENOENT;return -1; }
  memset(S,0,sizeof(*S));S->st_mode=NodeMode;S->st_rdev=NodeDevice;return 0;
}
static struct udev_monitor *TestChannel(struct udev *U,const char *Channel) {
  CHECK(!strcmp(Channel,"kernel"));Channels++;
  return udev_monitor_new_from_netlink(U,Channel);
}
#define lstat TestLstat
#define udev_monitor_new_from_netlink TestChannel
#include "uterm_monitor.c"
#undef lstat
#undef udev_monitor_new_from_netlink
static void Event(struct uterm_monitor *M,struct uterm_monitor_event *E,void *Data) {
  (void)M;(void)E;(void)Data;CHECK(0);
}
int main(void) {
  Result=-1;CHECK(!monitor_node_ready("/fixture/event0",makedev(13,64)));
  Result=0;NodeMode=S_IFREG|0600;NodeDevice=makedev(13,64);
  CHECK(!monitor_node_ready("/fixture/event0",makedev(13,64)));
  NodeMode=S_IFLNK|0777;CHECK(!monitor_node_ready("/fixture/event0",makedev(13,64)));
  NodeMode=S_IFCHR|0600;NodeDevice=makedev(13,65);
  CHECK(!monitor_node_ready("/fixture/event0",makedev(13,64)));
  NodeDevice=makedev(13,64);CHECK(monitor_node_ready("/fixture/event0",makedev(13,64)));
  struct ev_eloop *Loop;struct uterm_monitor *Monitor=NULL;
  CHECK(!ev_eloop_new(&Loop,NULL,NULL));
  CHECK(!uterm_monitor_new(&Monitor,Loop,Event,false,NULL));
  CHECK(Channels==1&&Monitor&&Monitor->rescan_timer);
  CHECK(ev_timer_is_enabled(Monitor->rescan_timer)&&ev_timer_is_bound(Monitor->rescan_timer));
  // Do not dispatch: a real sysfs scan is intentionally outside this check.
  uterm_monitor_unref(Monitor);ev_eloop_unref(Loop);
  printf("console monitor: %u checks PASS; raw kernel channel, delayed/wrong/symlink node rejection and timer lifecycle; no device enumeration\n",Checks);
  return 0;
}
