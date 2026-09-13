#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

/* Steam compatibility only. The worker's broker owns a preallocated gamepad.
 * No operation opens a kernel uinput endpoint or chooses a host device.
 * The kernel/container device boundary remains authoritative even if a
 * workload bypasses this library. Both ELF ABIs use the same 24-byte protocol. */
/* Steam's nested runtimes may carry older glibc. Keep the original dlsym
 * ABI and explicit libdl/libpthread dependencies instead of requiring 2.34. */
extern void *polaris_dlsym(void *,const char *);
#if defined(__x86_64__)
__asm__(".symver polaris_dlsym,dlsym@GLIBC_2.2.5");
#elif defined(__i386__)
__asm__(".symver polaris_dlsym,dlsym@GLIBC_2.0");
#else
#error Unsupported Steam input library ABI
#endif

#define MAX_HANDLES 64
#define MAX_CONTROLLERS 8
#define MAX_EVENTS 128
struct controller {
  unsigned refs;
  bool configured, created;
  uint32_t sequence;
  unsigned evbits;
  uint16_t buttons;
  unsigned keys;
  bool axes_set[ABS_CNT];
  struct input_absinfo ranges[ABS_CNT];
  int16_t sticks[4];
  uint8_t triggers[2];
  int8_t hats[2];
};
struct handle { int fd; struct controller *controller; };
static struct controller controllers[MAX_CONTROLLERS];
static struct handle handles[MAX_HANDLES];
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

/* Preserve libc behavior (including cancellation and 32-bit large-file locks)
 * for ordinary descriptors. Early loader calls use the syscall fallback until
 * the constructor resolves the next symbols. */
static int (*next_openat)(int,const char *,int,...);
static int (*next_ioctl)(int,unsigned long,...);
static ssize_t (*next_write)(int,const void *,size_t);
static ssize_t (*next_writev)(int,const struct iovec *,int);
static int (*next_close)(int);
static int (*next_fcntl)(int,int,...);
static int (*next_fcntl64)(int,int,...);
static int failure(int error) { errno=error; return -1; }
static struct handle *lookup(int fd) {
  for (unsigned i=0;i<MAX_HANDLES;i++) if (handles[i].controller && handles[i].fd==fd) return &handles[i];
  return NULL;
}
static struct handle *empty_handle(void) {
  for (unsigned i=0;i<MAX_HANDLES;i++) if (!handles[i].controller) return &handles[i];
  return NULL;
}
static void forget(struct handle *h) {
  if (!h) return;
  struct controller *c=h->controller;
  h->controller=NULL;
  if (--c->refs==0) memset(c,0,sizeof(*c));
}
static bool uinput_path(const char *path) {
  return path && (!strcmp(path,"/dev/uinput") || !strcmp(path,"/dev/input/uinput"));
}
static bool enabled(void) {
  const char *path=getenv("POLARIS_STEAM_INPUT_SOCKET");
  return path && path[0]=='/' && strlen(path)<sizeof(((struct sockaddr_un *)0)->sun_path);
}
static int compat_open(void) {
  if (!enabled()) return failure(ENOENT);
  pthread_mutex_lock(&mutex);
  struct handle *h=empty_handle();
  struct controller *c=NULL;
  for (unsigned i=0;i<MAX_CONTROLLERS;i++) if (!controllers[i].refs) { c=&controllers[i]; break; }
  if (!h || !c) { pthread_mutex_unlock(&mutex); return failure(EMFILE); }
  int fd=(int)syscall(SYS_eventfd2,0,EFD_CLOEXEC|EFD_NONBLOCK);
  if (fd>=0) { memset(c,0,sizeof(*c)); c->refs=1; h->fd=fd; h->controller=c; }
  pthread_mutex_unlock(&mutex);
  return fd;
}
static int open_common(int dirfd,const char *path,int flags,mode_t mode) {
  if (uinput_path(path) && enabled()) {
    if ((flags&O_ACCMODE)==O_RDONLY || (flags&(O_CREAT|O_TRUNC|O_DIRECTORY|O_PATH))) return failure(EINVAL);
    return compat_open();
  }
  return next_openat ? next_openat(dirfd,path,flags,mode) : (int)syscall(SYS_openat,dirfd,path,flags,mode);
}
#define OPEN_BODY(dirfd) \
  mode_t mode=0; if ((flags&O_CREAT) || (flags&O_TMPFILE)==O_TMPFILE) { va_list args; va_start(args,flags); mode=va_arg(args,int); va_end(args); } \
  return open_common(dirfd,path,flags,mode)
int open(const char *path,int flags,...) { OPEN_BODY(AT_FDCWD); }
int open64(const char *path,int flags,...) { flags|=O_LARGEFILE; OPEN_BODY(AT_FDCWD); }
int openat(int dirfd,const char *path,int flags,...) { OPEN_BODY(dirfd); }
int openat64(int dirfd,const char *path,int flags,...) { flags|=O_LARGEFILE; OPEN_BODY(dirfd); }
int __open_2(const char *path,int flags) { return open_common(AT_FDCWD,path,flags,0); }
int __open64_2(const char *path,int flags) { return open_common(AT_FDCWD,path,flags|O_LARGEFILE,0); }
int __openat_2(int dirfd,const char *path,int flags) { return open_common(dirfd,path,flags,0); }
int __openat64_2(int dirfd,const char *path,int flags) { return open_common(dirfd,path,flags|O_LARGEFILE,0); }

static int key_index(unsigned code) {
  const unsigned keys[]={BTN_A,BTN_B,BTN_X,BTN_Y,BTN_TL,BTN_TR,BTN_SELECT,BTN_START,BTN_MODE,BTN_THUMBL,BTN_THUMBR};
  for (unsigned i=0;i<sizeof(keys)/sizeof(keys[0]);i++) if (keys[i]==code) return (int)i;
  return -1;
}
static bool axis_supported(unsigned code) {
  return code==ABS_X || code==ABS_Y || code==ABS_RX || code==ABS_RY ||
         code==ABS_Z || code==ABS_RZ || code==ABS_HAT0X || code==ABS_HAT0Y;
}
static bool range_valid(const struct input_absinfo *range) {
  return range->minimum<range->maximum && range->minimum>=-65536 && range->maximum<=65536;
}
static bool setup_valid(const struct input_id *id,const char name[UINPUT_MAX_NAME_SIZE]) {
  return memchr(name,0,UINPUT_MAX_NAME_SIZE) && name[0] && id->bustype==BUS_USB &&
         ((id->vendor==0x28de && id->product==0x11ff) || (id->vendor==0x045e && id->product==0x028e));
}
static int connect_controller(int fd,struct controller *c) {
  if (!c->configured || c->created || !(c->evbits&(1U<<EV_KEY)) || !(c->evbits&(1U<<EV_ABS)) ||
      !(c->keys&1U) || !c->axes_set[ABS_X] || !c->axes_set[ABS_Y]) return failure(EINVAL);
  for (unsigned i=0;i<ABS_CNT;i++) if (c->axes_set[i] && !range_valid(&c->ranges[i])) return failure(EINVAL);
  const char *path=getenv("POLARIS_STEAM_INPUT_SOCKET");
  if (!enabled()) return failure(ENOENT);
  struct sockaddr_un address={.sun_family=AF_UNIX};
  memcpy(address.sun_path,path,strlen(path)+1);
  int channel=(int)syscall(SYS_socket,AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
  if (channel<0) return -1;
  if (syscall(SYS_connect,channel,&address,sizeof(address))<0) {
    int saved=errno; syscall(SYS_close,channel); return failure(saved);
  }
  /* Preserve all aliases created with dup/fcntl, as one uinput file lifetime. */
  for (unsigned i=0;i<MAX_HANDLES;i++) if (handles[i].controller==c) {
    if (syscall(SYS_dup3,channel,handles[i].fd,O_CLOEXEC)<0) {
      int saved=errno; syscall(SYS_shutdown,channel,SHUT_RDWR); syscall(SYS_close,channel); return failure(saved);
    }
  }
  syscall(SYS_close,channel);
  c->buttons=0; memset(c->sticks,0,sizeof(c->sticks));
  memset(c->triggers,0,sizeof(c->triggers)); memset(c->hats,0,sizeof(c->hats));
  c->created=true; c->sequence=1;
  (void)fd;
  return 0;
}
static int controller_ioctl(int fd,struct controller *c,unsigned long request,uintptr_t arg) {
  if (request==UI_GET_VERSION) { if (!arg) return failure(EFAULT); *(int *)arg=5; return 0; }
  if (request==UI_DEV_CREATE) return connect_controller(fd,c);
  if (request==UI_DEV_DESTROY) {
    if (!c->created) return failure(EINVAL);
    syscall(SYS_shutdown,fd,SHUT_RDWR); c->created=false; return 0;
  }
  if (_IOC_TYPE(request)=='U' && _IOC_NR(request)==44 && _IOC_DIR(request)==_IOC_READ) {
    const char *name=getenv("POLARIS_STEAM_INPUT_SYSNAME");
    size_t count=_IOC_SIZE(request);
    if (!c->created || !name || strncmp(name,"input",5) || !name[5] ||
        strspn(name+5,"0123456789")!=strlen(name+5) || !arg || count<=strlen(name)) return failure(EINVAL);
    memcpy((void *)arg,name,strlen(name)+1);
    return (int)strlen(name)+1;
  }
  if (c->created) return failure(EINVAL);
  if (request==UI_SET_EVBIT) {
    if (arg!=EV_KEY && arg!=EV_ABS && arg!=EV_SYN && arg!=EV_FF) return failure(EINVAL);
    c->evbits|=1U<<arg; return 0;
  }
  if (request==UI_SET_KEYBIT) {
    int index=key_index((unsigned)arg); if (index<0) return failure(EINVAL);
    c->keys|=1U<<index; return 0;
  }
  if (request==UI_SET_ABSBIT) {
    if (!axis_supported((unsigned)arg)) return failure(EINVAL);
    c->axes_set[arg]=true; return 0;
  }
  if (request==UI_ABS_SETUP) {
    if (!arg) return failure(EFAULT);
    const struct uinput_abs_setup *setup=(const void *)arg;
    if (!axis_supported(setup->code) || !range_valid(&setup->absinfo)) return failure(EINVAL);
    c->axes_set[setup->code]=true; c->ranges[setup->code]=setup->absinfo; return 0;
  }
  if (request==UI_DEV_SETUP) {
    if (!arg) return failure(EFAULT);
    const struct uinput_setup *setup=(const void *)arg;
    if (!setup_valid(&setup->id,setup->name)) return failure(EINVAL);
    c->configured=true; return 0;
  }
  // Feedback goes from the allocated output through the existing host rumble
  // route. Steam does not get to create a new kernel force-feedback device.
  if (request==UI_SET_FFBIT) return arg<=FF_MAX ? 0 : failure(EINVAL);
  if (request==UI_SET_PHYS) return arg ? 0 : failure(EFAULT); // identity stays host-owned
  return failure(EINVAL);
}
int ioctl(int fd,unsigned long request,...) {
  uintptr_t arg=0;
  if (request!=UI_DEV_CREATE && request!=UI_DEV_DESTROY) {
    va_list args; va_start(args,request); arg=va_arg(args,uintptr_t); va_end(args);
  }
  pthread_mutex_lock(&mutex);
  struct handle *h=lookup(fd);
  if (!h) { pthread_mutex_unlock(&mutex); return next_ioctl ? next_ioctl(fd,request,arg) : (int)syscall(SYS_ioctl,fd,request,arg); }
  int result=controller_ioctl(fd,h->controller,request,arg);
  pthread_mutex_unlock(&mutex);
  return result;
}
static void put16(uint8_t *p,uint16_t value) { p[0]=(uint8_t)value; p[1]=(uint8_t)(value>>8); }
static void put32(uint8_t *p,uint32_t value) { put16(p,(uint16_t)value); put16(p+2,(uint16_t)(value>>16)); }
static int report(int fd,struct controller *c) {
  if (!c->sequence) return failure(EOVERFLOW);
  uint8_t packet[24]={0};
  memcpy(packet,"PSI1",4); put32(packet+4,c->sequence);
  put16(packet+8,c->buttons); memcpy(packet+10,c->triggers,2);
  for (unsigned i=0;i<4;i++) put16(packet+12+i*2,(uint16_t)c->sticks[i]);
  packet[20]=(uint8_t)c->hats[0]; packet[21]=(uint8_t)c->hats[1];
  ssize_t n=send(fd,packet,sizeof(packet),MSG_NOSIGNAL|MSG_DONTWAIT);
  if (n!=(ssize_t)sizeof(packet)) return failure(n<0?errno:EIO);
  c->sequence++;
  return 0;
}
static int apply_event(struct controller *c,const struct input_event *event) {
  if (event->type==EV_SYN) return event->code==SYN_REPORT && event->value==0 ? 0 : failure(EINVAL);
  if (event->type==EV_KEY) {
    int index=key_index(event->code);
    if (index<0 || !(c->keys&(1U<<index)) || (event->value!=0 && event->value!=1)) return failure(EINVAL);
    c->buttons=(uint16_t)((c->buttons&~(1U<<index))|((unsigned)event->value<<index)); return 0;
  }
  if (event->type!=EV_ABS || !axis_supported(event->code) || !c->axes_set[event->code]) return failure(EINVAL);
  const struct input_absinfo *range=&c->ranges[event->code];
  if (!range_valid(range) || event->value<range->minimum || event->value>range->maximum) return failure(EINVAL);
  if (event->code==ABS_HAT0X || event->code==ABS_HAT0Y) {
    if (event->value < -1 || event->value > 1) return failure(EINVAL);
    c->hats[event->code-ABS_HAT0X]=(int8_t)event->value; return 0;
  }
  int64_t value=event->value, min=range->minimum, max=range->maximum;
  if (event->code==ABS_Z || event->code==ABS_RZ) c->triggers[event->code==ABS_RZ]=(uint8_t)((value-min)*255/(max-min));
  else {
    unsigned index=event->code==ABS_X?0:event->code==ABS_Y?1:event->code==ABS_RX?2:3;
    c->sticks[index]=(int16_t)((value-min)*65535/(max-min)-32768);
  }
  return 0;
}
static ssize_t controller_write(int fd,struct controller *c,const void *data,size_t size) {
  if (!data) return failure(EFAULT);
  if (!c->created) {
    if (size!=sizeof(struct uinput_user_dev)) return failure(EINVAL);
    struct uinput_user_dev storage;
    memcpy(&storage,data,sizeof(storage));
    const struct uinput_user_dev *setup=&storage;
    if (!setup_valid(&setup->id,setup->name)) return failure(EINVAL);
    struct controller next=*c;
    for (unsigned i=0;i<ABS_CNT;i++) if (c->axes_set[i]) {
      next.ranges[i]=(struct input_absinfo){.minimum=setup->absmin[i],.maximum=setup->absmax[i]};
      if (!range_valid(&next.ranges[i])) return failure(EINVAL);
    }
    next.configured=true; *c=next; return (ssize_t)size;
  }
  if (!size || size%sizeof(struct input_event) || size>MAX_EVENTS*sizeof(struct input_event)) return failure(EINVAL);
  /* Validate the whole batch before committing or sending any report. */
  struct controller next=*c;
  const uint8_t *events=data;
  struct input_event event;
  size_t count=size/sizeof(event);
  for (size_t i=0;i<count;i++) {
    memcpy(&event,events+i*sizeof(event),sizeof(event));
    if (apply_event(&next,&event)<0) return -1;
  }
  for (size_t i=0;i<count;i++) {
    memcpy(&event,events+i*sizeof(event),sizeof(event));
    if (apply_event(c,&event)<0) return -1;
    if (event.type==EV_SYN && report(fd,c)<0) {
      int saved=errno; syscall(SYS_shutdown,fd,SHUT_RDWR); return failure(saved);
    }
  }
  return (ssize_t)size;
}
ssize_t write(int fd,const void *data,size_t size) {
  pthread_mutex_lock(&mutex);
  struct handle *h=lookup(fd);
  if (!h) { pthread_mutex_unlock(&mutex); return next_write ? next_write(fd,data,size) : syscall(SYS_write,fd,data,size); }
  ssize_t result=controller_write(fd,h->controller,data,size);
  pthread_mutex_unlock(&mutex);
  return result;
}
ssize_t writev(int fd,const struct iovec *iov,int count) {
  pthread_mutex_lock(&mutex);
  struct handle *h=lookup(fd);
  if (!h) { pthread_mutex_unlock(&mutex); return next_writev ? next_writev(fd,iov,count) : syscall(SYS_writev,fd,iov,count); }
  uint8_t buffer[4096]; size_t size=0;
  if (!iov || count<1 || count>16) { pthread_mutex_unlock(&mutex); return failure(EINVAL); }
  for (int i=0;i<count;i++) {
    if (iov[i].iov_len>sizeof(buffer)-size || (!iov[i].iov_base && iov[i].iov_len)) {
      pthread_mutex_unlock(&mutex); return failure(EINVAL);
    }
    memcpy(buffer+size,iov[i].iov_base,iov[i].iov_len); size+=iov[i].iov_len;
  }
  ssize_t result=controller_write(fd,h->controller,buffer,size);
  pthread_mutex_unlock(&mutex);
  return result;
}
int close(int fd) {
  pthread_mutex_lock(&mutex);
  struct handle *h=lookup(fd);
  if (!h) { pthread_mutex_unlock(&mutex); return next_close ? next_close(fd) : (int)syscall(SYS_close,fd); }
  forget(h);
  int result=(int)syscall(SYS_close,fd);
  pthread_mutex_unlock(&mutex);
  return result;
}
static int duplicate(int oldfd,int newfd,int flags,int operation) {
  pthread_mutex_lock(&mutex);
  struct handle *source=lookup(oldfd);
  struct controller *c=source?source->controller:NULL;
  if (c && !empty_handle() && !lookup(newfd)) { pthread_mutex_unlock(&mutex); return failure(EMFILE); }
  int result=operation==0?(int)syscall(SYS_dup,oldfd):
             operation==1?(int)syscall(SYS_dup2,oldfd,newfd):
                          (int)syscall(SYS_dup3,oldfd,newfd,flags);
  if (result>=0 && oldfd!=result) {
    if (c) c->refs++; // retain before a same-controller destination is dropped
    forget(lookup(result));
    if (c) { struct handle *h=empty_handle(); h->fd=result; h->controller=c; }
  }
  pthread_mutex_unlock(&mutex);
  return result;
}
int dup(int fd) { return duplicate(fd,-1,0,0); }
int dup2(int fd,int target) { return duplicate(fd,target,0,1); }
int dup3(int fd,int target,int flags) { return duplicate(fd,target,flags,2); }
static int forward_fcntl(int fd,int command,uintptr_t arg,bool wide) {
  if (wide && next_fcntl64) return next_fcntl64(fd,command,arg);
  if (!wide && next_fcntl) return next_fcntl(fd,command,arg);
#ifdef SYS_fcntl64
  if (wide) return (int)syscall(SYS_fcntl64,fd,command,arg);
#endif
  return (int)syscall(SYS_fcntl,fd,command,arg);
}
static int fcntl_common(int fd,int command,uintptr_t arg,bool wide) {
  pthread_mutex_lock(&mutex);
  struct handle *source=lookup(fd);
  bool copying=source && (command==F_DUPFD || command==F_DUPFD_CLOEXEC);
  if (!copying) { pthread_mutex_unlock(&mutex); return forward_fcntl(fd,command,arg,wide); }
  if (!empty_handle()) { pthread_mutex_unlock(&mutex); return failure(EMFILE); }
  int result=forward_fcntl(fd,command,arg,wide);
  if (result>=0) {
    struct handle *h=empty_handle();
    h->fd=result; h->controller=source->controller; h->controller->refs++;
  }
  pthread_mutex_unlock(&mutex);
  return result;
}
#define FCNTL_BODY(wide) \
  uintptr_t arg=0; \
  if (command!=F_GETFD && command!=F_GETFL && command!=F_GETOWN && command!=F_GETSIG && \
      command!=F_GETLEASE && command!=F_GETPIPE_SZ && command!=F_GET_SEALS) { \
    va_list args; va_start(args,command); arg=va_arg(args,uintptr_t); va_end(args); \
  } \
  return fcntl_common(fd,command,arg,wide)
int fcntl(int fd,int command,...) { FCNTL_BODY(false); }
int fcntl64(int fd,int command,...) { FCNTL_BODY(true); }
static void before_fork(void) { pthread_mutex_lock(&mutex); }
static void after_fork(void) { pthread_mutex_unlock(&mutex); }
static void child_fork(void) {
  // The child's pre-exec copy must not keep Steam's output lease alive.
  for (unsigned i=0;i<MAX_HANDLES;i++) if (handles[i].controller) syscall(SYS_close,handles[i].fd);
  memset(handles,0,sizeof(handles)); memset(controllers,0,sizeof(controllers));
  pthread_mutex_unlock(&mutex);
}
__attribute__((constructor)) static void initialize(void) {
  next_openat=polaris_dlsym(RTLD_NEXT,"openat");
  next_ioctl=polaris_dlsym(RTLD_NEXT,"ioctl");
  next_write=polaris_dlsym(RTLD_NEXT,"write");
  next_writev=polaris_dlsym(RTLD_NEXT,"writev");
  next_close=polaris_dlsym(RTLD_NEXT,"close");
  next_fcntl=polaris_dlsym(RTLD_NEXT,"fcntl");
  next_fcntl64=polaris_dlsym(RTLD_NEXT,"fcntl64");
  pthread_atfork(before_fork,after_fork,child_fork);
}
