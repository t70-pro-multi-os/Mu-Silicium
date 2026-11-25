// SPDX-License-Identifier: BSD-2-Clause-Patent
// Early userspace for the exact stock kernel. Does not mount persistent storage.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t Stopping;
static void Stop(int Number) { (void)Number; Stopping=1; }
static void ChildChanged(int Number) { (void)Number; }

static int WatchChildren(void) {
  struct sigaction Action={0};
  Action.sa_handler=Stop;
  if (sigemptyset(&Action.sa_mask) || sigaction(SIGTERM,&Action,NULL) ||
      sigaction(SIGINT,&Action,NULL)) return -1;
  Action.sa_handler=ChildChanged;
  return sigaction(SIGCHLD,&Action,NULL);
}

static pid_t WaitForChild(int *Status,int HonorStop) {
  sigset_t Watched,Old;
  pid_t Result=-1;
  if (sigemptyset(&Watched) || sigaddset(&Watched,SIGTERM) ||
      sigaddset(&Watched,SIGINT) || sigaddset(&Watched,SIGCHLD) ||
      sigprocmask(SIG_BLOCK,&Watched,&Old)) return -1;
  for (;;) {
    if (HonorStop && Stopping) { errno=EINTR; break; }
    Result=waitpid(-1,Status,WNOHANG);
    if (Result<0 && errno==EINTR) continue;
    if (Result!=0) break;
    // Atomically enable signals and sleep; a pending STOP or SIGCHLD
    // between the check above and sleep cannot be lost.
    if (sigsuspend(&Old)<0 && errno!=EINTR) { Result=-1; break; }
    Result=-1;
  }
  int Error=errno;
  if (sigprocmask(SIG_SETMASK,&Old,NULL)) return -1;
  errno=Error;
  return Result;
}

static int Directory(const char *Name,mode_t Mode) {
  struct stat S;
  if (mkdir(Name,Mode)==0) return 0;
  if (errno!=EEXIST || lstat(Name,&S)<0 || !S_ISDIR(S.st_mode)) return -1;
  return 0;
}
static int Node(const char *Name,unsigned Major,unsigned Minor,mode_t Mode) {
  struct stat S;
  if (mknod(Name,S_IFCHR|Mode,makedev(Major,Minor))==0) return 0;
  if (errno!=EEXIST || lstat(Name,&S)<0 || !S_ISCHR(S.st_mode) ||
      S.st_rdev!=makedev(Major,Minor)) return -1;
  return chmod(Name,Mode);
}
static int Filesystems(void) {
  const unsigned long Safe=MS_NOSUID|MS_NOEXEC|MS_NODEV;
  if (mount(NULL,"/",NULL,MS_REC|MS_PRIVATE,NULL)<0 && errno!=EINVAL) return -1;
  if (Directory("/proc",0555) || Directory("/sys",0555) ||
      Directory("/dev",0755) || Directory("/run",0755) || Directory("/tmp",01777)) return -1;
  if (mount("proc","/proc","proc",Safe,NULL) ||
      mount("sysfs","/sys","sysfs",Safe|MS_RDONLY,NULL) ||
      mount("tmpfs","/dev","tmpfs",MS_NOSUID|MS_NOEXEC,"mode=0755,size=16M") ||
      mount("tmpfs","/run","tmpfs",Safe,"mode=0755,size=16M") ||
      mount("tmpfs","/tmp","tmpfs",Safe,"mode=1777,size=32M")) return -1;
  if (Node("/dev/console",5,1,0600) || Node("/dev/null",1,3,0666) ||
      Node("/dev/zero",1,5,0666) || Node("/dev/tty",5,0,0666) ||
      Node("/dev/random",1,8,0666) || Node("/dev/urandom",1,9,0666)) return -1;
  if (Directory("/dev/pts",0755) || Directory("/dev/shm",01777) ||
      mount("devpts","/dev/pts","devpts",MS_NOSUID|MS_NOEXEC,
            "newinstance,ptmxmode=0666,mode=0620,gid=5")) return -1;
  if (symlink("pts/ptmx","/dev/ptmx") || symlink("/proc/self/fd","/dev/fd") ||
      symlink("/proc/self/fd/0","/dev/stdin") || symlink("/proc/self/fd/1","/dev/stdout") ||
      symlink("/proc/self/fd/2","/dev/stderr")) return -1;
  return 0;
}
static int WaitForScan(pid_t Child,unsigned TimeoutMs) {
  struct timespec Start,Now,Pause={0,25000000};
  if (clock_gettime(CLOCK_MONOTONIC,&Start)) goto cancel;
  for (;;) {
    int Status;
    pid_t Result=waitpid(Child,&Status,WNOHANG);
    if (Result==Child) {
      if (WIFEXITED(Status) && WEXITSTATUS(Status)==0) return 0;
      errno=EIO;
      return -1;
    }
    if (Result<0 && errno!=EINTR) return -1;
    if (Stopping) { errno=EINTR; break; }
    if (clock_gettime(CLOCK_MONOTONIC,&Now)) break;
    long long ElapsedNs=(long long)(Now.tv_sec-Start.tv_sec)*1000000000LL+
      Now.tv_nsec-Start.tv_nsec;
    if (ElapsedNs>=TimeoutMs*1000000LL) { errno=ETIMEDOUT; break; }
    (void)nanosleep(&Pause,NULL);
  }
cancel: ;
  // An uninterruptible scan must not withhold rescue or the startup workers.
  // PID1's ordinary reaper will collect it after the pending kill takes effect.
  int Error=errno;
  (void)kill(Child,SIGKILL);
  errno=Error;
  return -1;
}
static int InitialScan(void) {
  pid_t Child=fork();
  if (Child<0) return -1;
  if (Child==0) {
    execl("/bin/busybox","mdev","-s",(char *)NULL);
    _exit(127);
  }
  return WaitForScan(Child,10000);
}
#define CMDLINE_LIMIT 8192U
#define SELECTION_LIMIT 256U
/* Parse exactly one literal argument. The generated worker separately validates
 * all feature names before loading the first module; no shell evaluation. */
static int SelectArgument(const char *Line,size_t Bytes,const char *Key,char *Selection,size_t Capacity) {
  size_t I=0,K=strlen(Key); int Found=0;
  if (!Line || !Selection || Capacity<5 || Bytes>CMDLINE_LIMIT) return -1;
  memcpy(Selection,"none",5);
  while (I<Bytes) {
    size_t Begin,End,Length;
    if (!Line[I]) return -1;
    if (Line[I]==' ' || Line[I]=='\t' || Line[I]=='\r' || Line[I]=='\n') { I++;continue; }
    Begin=I;
    while (I<Bytes && Line[I]!=' ' && Line[I]!='\t' && Line[I]!='\r' && Line[I]!='\n') {
      if (!Line[I]) return -1;
      I++;
    }
    End=I;
    if (End-Begin<K || memcmp(Line+Begin,Key,K)) continue;
    Length=End-Begin-K;
    if (Found++ || !Length || Length>SELECTION_LIMIT || Length>=Capacity) return -1;
    memcpy(Selection,Line+Begin+K,Length);Selection[Length]=0;
  }
  return 0;
}
static int SelectStartup(const char *Line,size_t Bytes,char *Selection,size_t Capacity) {
  return SelectArgument(Line,Bytes,"teclast.modules=",Selection,Capacity);
}
static int SelectConsole(const char *Line,size_t Bytes,char *Selection,size_t Capacity) {
  if (SelectArgument(Line,Bytes,"teclast.console=",Selection,Capacity)) return -1;
  return strcmp(Selection,"none") && strcmp(Selection,"drm") &&
    strcmp(Selection,"desktop-vulkan") && strcmp(Selection,"desktop-pixman")?-1:0;
}
static int ReadSelection(int Console,char *Selection,size_t Capacity) {
  char Line[CMDLINE_LIMIT+1]; size_t Bytes=0; int Fd=open("/proc/cmdline",O_RDONLY|O_CLOEXEC);
  if (Fd<0) return -1;
  while (Bytes<sizeof(Line)) {
    ssize_t N=read(Fd,Line+Bytes,sizeof(Line)-Bytes);
    if (N<0 && errno==EINTR) continue;
    if (N<0) { close(Fd);return -1; }
    if (!N) break;
    Bytes+=(size_t)N;
  }
  close(Fd);
  return Console?SelectConsole(Line,Bytes,Selection,Capacity):
    SelectStartup(Line,Bytes,Selection,Capacity);
}
static int StartupSelection(char *Selection,size_t Capacity) {
  return ReadSelection(0,Selection,Capacity);
}
static pid_t Startup(void) {
  char Selection[SELECTION_LIMIT+1]; pid_t P;
  if (StartupSelection(Selection,sizeof(Selection))) {
    fputs("Invalid module startup selection; no modules started\n",stderr);return -1;
  }
  if (Stopping) return -1;
  P=fork();
  if (P==0) {
    signal(SIGTERM,SIG_DFL);signal(SIGINT,SIG_DFL);
    if (Stopping) _exit(143);
    struct rlimit Limit={512U*1024U,512U*1024U}; int Fd;
    if (setsid()<0 || setrlimit(RLIMIT_FSIZE,&Limit)<0) _exit(126);
    umask(077);
    Fd=open("/run/teclast-startup.log",O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC|O_NOFOLLOW,0600);
    if (Fd<0) _exit(126);
    if (dup2(Fd,1)<0 || dup2(Fd,2)<0) _exit(126);
    if (Fd>2) close(Fd);
    execl("/sbin/teclast-start","teclast-start",Selection,(char *)NULL);_exit(127);
  }
  return P;
}
static void ConsoleStatus(const char *Text) {
  struct stat S; size_t Done=0,N=strlen(Text);
  int Fd=open("/run/teclast-console.state",O_CREAT|O_APPEND|O_WRONLY|O_CLOEXEC|O_NOFOLLOW,0600);
  if (Fd<0) return;
  if (fstat(Fd,&S) || !S_ISREG(S.st_mode) || S.st_nlink!=1 || S.st_size>1024) {
    close(Fd);return;
  }
  while (Done<N) {
    ssize_t W=write(Fd,Text+Done,N-Done);
    if (W<0 && errno==EINTR) continue;
    if (W<=0) break;
    Done+=(size_t)W;
  }
  close(Fd);
}
static pid_t Console(void) {
  char Selection[SELECTION_LIMIT+1];
  if (ReadSelection(1,Selection,sizeof(Selection))) {
    ConsoleStatus("invalid-selection\n");return -1;
  }
  if (!strcmp(Selection,"none")) { ConsoleStatus("disabled\n");return -1; }
  if (Stopping) return -1;
  pid_t P=fork();
  if (P==0) {
    signal(SIGTERM,SIG_DFL);signal(SIGINT,SIG_DFL);
    if (Stopping) _exit(143);
    struct rlimit Limit={512U*1024U,512U*1024U};
    int Fd;
    if (setsid()<0 || setrlimit(RLIMIT_FSIZE,&Limit)<0) _exit(126);
    umask(077);
    Fd=open("/run/teclast-console.log",O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC|O_NOFOLLOW,0600);
    if (Fd<0 || dup2(Fd,1)<0 || dup2(Fd,2)<0) _exit(126);
    if (Fd>2) close(Fd);
    int Null=open("/dev/null",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if (Null<0 || dup2(Null,0)<0) _exit(126);
    if (Null>2) close(Null);
    if (clearenv() || setenv("PATH","/bin:/sbin",1) ||
        setenv("HOME","/",1) || setenv("LANG","C.UTF-8",1)) _exit(126);
    ConsoleStatus("process-starting\n");
    if (!strncmp(Selection,"desktop-",8)) {
      execl("/sbin/teclast-desktop","teclast-desktop",Selection+8,(char *)NULL);
      perror("desktop exec");_exit(127);
    }
    execl("/usr/libexec/kmscon/kmscon","kmscon","--configdir","/etc/kmscon",
      "--login","/sbin/teclast-shell",(char *)NULL);
    perror("kmscon exec");_exit(127);
  }
  if (P<0) ConsoleStatus("fork-failed\n");
  return P;
}
static pid_t DeviceManager(void) {
  char Selection[SELECTION_LIMIT+1];
  int Desktop=!ReadSelection(1,Selection,sizeof(Selection)) && !strncmp(Selection,"desktop-",8);
  /* Coldplug writes only this new Linux instance's sysfs uevent files. */
  if (Desktop && mount(NULL,"/sys",NULL,MS_REMOUNT|MS_NOSUID|MS_NOEXEC|MS_NODEV,NULL)) {
    perror("desktop sysfs remount");return -1;
  }
  pid_t P=fork();
  if (P==0) {
    signal(SIGTERM,SIG_DFL);signal(SIGINT,SIG_DFL);
    if (setsid()<0) _exit(126);
    if (Desktop) {
      struct rlimit Limit={512U*1024U,512U*1024U};
      int Fd=open("/run/teclast-devices.log",O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC|O_NOFOLLOW,0600);
      if (setrlimit(RLIMIT_FSIZE,&Limit)<0 || Fd<0 || dup2(Fd,1)<0 || dup2(Fd,2)<0) _exit(126);
      if (Fd>2) close(Fd);
      if (clearenv() || setenv("PATH","/bin:/sbin:/usr/bin:/usr/sbin",1) ||
          setenv("SYSTEMD_LOG_TARGET","console",1) || setenv("SYSTEMD_LOG_LEVEL","warning",1)) _exit(126);
      execl("/usr/lib/systemd/systemd-udevd","systemd-udevd","--resolve-names=never",
        "--children-max=4","--event-timeout=20",(char *)NULL);
    } else execl("/bin/busybox","mdev","-df",(char *)NULL);
    _exit(127);
  }
  return P;
}
static int WriteReady(int NamespaceCheck) {
  int Fd=open("/run/teclast-early-userspace.json",O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,0600);
  const char *Text=NamespaceCheck?
    "{\"mode\":\"builder PID/mount namespace check\",\"pid1\":true,\"persistent_storage_mounted\":false,\"modules_loaded\":false}\n":
    "{\"mode\":\"early userspace\",\"pid1\":true,\"persistent_storage_mounted\":false,\"module_startup\":\"see /run/teclast-startup/state\"}\n";
  if (Fd<0) return -1;
  size_t N=strlen(Text),Done=0;
  while (Done<N) {
    ssize_t W=write(Fd,Text+Done,N-Done);
    if (W<0 && errno==EINTR) continue;
    if (W<=0) { close(Fd); return -1; }
    Done+=(size_t)W;
  }
  return close(Fd);
}
static pid_t Shell(void) {
  pid_t P=fork();
  if (P==0) {
    int Fd;
    if (setsid()<0) _exit(126);
    Fd=open("/dev/console",O_RDWR|O_NOCTTY);
    if (Fd<0) _exit(126);
    (void)ioctl(Fd,TIOCSCTTY,0);
    for (int I=0;I<3;I++) if (dup2(Fd,I)<0) _exit(126);
    if (Fd>2) close(Fd);
    execl("/bin/sh","sh","-i",(char *)NULL); _exit(127);
  }
  return P;
}
static int NamespaceCheck(void) {
  struct stat S; struct statfs F;
  const char *Paths[]={"/proc","/sys","/dev","/dev/pts","/run","/tmp"};
  const long Types[]={0x9fa0,0x62656572,0x01021994,0x1cd1,0x01021994,0x01021994};
  for (unsigned I=0;I<sizeof(Paths)/sizeof(Paths[0]);I++)
    if (statfs(Paths[I],&F) || F.f_type!=Types[I]) return 1;
  if (lstat("/dev/console",&S) || !S_ISCHR(S.st_mode) ||
      major(S.st_rdev)!=5 || minor(S.st_rdev)!=1) return 1;
  if (lstat("/dev/ptmx",&S) || !S_ISLNK(S.st_mode)) return 1;
  int Fd=open("/dev/null",O_RDWR|O_CLOEXEC); char B=1;
  if (Fd<0 || write(Fd,&B,1)!=1 || read(Fd,&B,1)!=0) return 1;
  close(Fd);
  puts("early userspace: actual namespace PID1, mounts, device nodes, devpts, tmpfs and /dev/null passed; no module/device discovery or persistent mount");
  return 0;
}
int main(int Argc,char **Argv) {
  int Check=Argc==2 && !strcmp(Argv[1],"--namespace-check");
  if (Argc==2 && !strcmp(Argv[1],"--version")) { puts("T70Pro early userspace 1"); return 0; }
  if (getpid()!=1 || (Argc!=1 && !Check)) {
    fputs("Run as PID1 of an initramfs or isolated builder namespace.\n",stderr); return 64;
  }
  umask(0);
  if (Filesystems()<0 || WriteReady(Check)<0) {
    perror("early userspace setup");
    if (Check) return 1;
    for (;;) pause();
  }
  if (Check) return NamespaceCheck();
  if (WatchChildren()) { perror("register early userspace signals"); for (;;) pause(); }
  // The rescue console needs only the static nodes already created above.
  pid_t Child=Shell();
  // Stock has no devtmpfs or uevent-helper. mdev uses netlink, not /proc/sys/kernel/hotplug.
  if (!Stopping && InitialScan()<0) perror("initial device-node scan");
  pid_t Mdev=Stopping?-1:DeviceManager();
  if (Mdev<0 && !Stopping) fputs("device manager was not started\n",stderr);
  /* Independent worker: a slow or failed module cannot withhold the rescue shell. */
  pid_t Worker=Startup();
  if (Worker<0 && !Stopping) fputs("module startup worker was not started\n",stderr);
  pid_t Graphical=Console();
  while (!Stopping) {
    int Status; pid_t P=WaitForChild(&Status,1);
    if (P<0) { if (errno==EINTR) continue; sleep(1); continue; }
    if (P==Worker) {
      fprintf(stderr,"module startup worker exited: %d; see /run/teclast-startup\n",
        WIFEXITED(Status)?WEXITSTATUS(Status):128+(WIFSIGNALED(Status)?WTERMSIG(Status):0));
      Worker=-1;
    }
    if (P==Graphical) {
      char Result[64];
      snprintf(Result,sizeof(Result),"exited:%d\n",
        WIFEXITED(Status)?WEXITSTATUS(Status):128+(WIFSIGNALED(Status)?WTERMSIG(Status):0));
      ConsoleStatus(Result);Graphical=-1;
    }
    if (P==Mdev) {
      fputs("device manager exited; it was not restarted automatically\n",stderr);Mdev=-1;
    }
    if (P==Child) {
      Child=-1;
      if (!Stopping) sleep(2);
      if (!Stopping) Child=Shell();
    }
  }
  if (Graphical>0) { kill(-Graphical,SIGTERM);kill(Graphical,SIGTERM); }
  if (Child>0) kill(Child,SIGTERM);
  if (Mdev>0) { kill(-Mdev,SIGTERM);kill(Mdev,SIGTERM); }
  if (Worker>0) { kill(-Worker,SIGTERM);kill(Worker,SIGTERM); }
  // No reboot/poweroff or disk synchronization is requested by this PID1.
  for (;;) {
    if (WaitForChild(NULL,0)<0 && errno!=EINTR) pause();
  }
}
