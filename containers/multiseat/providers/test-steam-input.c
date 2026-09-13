#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

static void *blocked_writer(void *arg) {
  char byte=1;
  ssize_t result=write(*(int *)arg,&byte,1);
  assert(result==1);
  return NULL;
}
static void ordinary_io(void) {
  char path[]="/tmp/polaris-steam-libc-XXXXXX", data[4]={0};
  int fd=mkstemp(path); assert(fd>=0); assert(unlink(path)==0);
  assert(write(fd,"abc",3)==3 && lseek(fd,0,SEEK_SET)==0);
  assert(read(fd,data,3)==3 && !strcmp(data,"abc"));
  struct flock64 lock={.l_type=F_WRLCK,.l_whence=SEEK_SET,.l_start=INT64_C(1)<<33,.l_len=1};
  assert(fcntl64(fd,F_SETLK64,&lock)==0);
  int copy=fcntl64(fd,F_DUPFD_CLOEXEC,32);
  assert(copy>=32 && (fcntl(copy,F_GETFD)&FD_CLOEXEC));
  assert(close(copy)==0 && close(fd)==0);
  int pipefd[2]; assert(pipe2(pipefd,O_NONBLOCK)==0);
  char buffer[4096]={0};
  while (write(pipefd[1],buffer,sizeof(buffer))>0) {}
  assert(errno==EAGAIN);
  assert(fcntl(pipefd[1],F_SETFL,0)==0);
  pthread_t thread; assert(pthread_create(&thread,NULL,blocked_writer,&pipefd[1])==0);
  usleep(10000); assert(pthread_cancel(thread)==0);
  void *result=NULL;
  assert(pthread_join(thread,&result)==0 && result==PTHREAD_CANCELED);
  assert(close(pipefd[0])==0 && close(pipefd[1])==0);
}
static const unsigned axes[]={ABS_X,ABS_Y,ABS_RX,ABS_RY,ABS_Z,ABS_RZ,ABS_HAT0X,ABS_HAT0Y};
static const unsigned keys[]={BTN_A,BTN_B,BTN_X,BTN_Y,BTN_TL,BTN_TR,BTN_SELECT,BTN_START,BTN_MODE,BTN_THUMBL,BTN_THUMBR};
static void limits(unsigned code,int *min,int *max) {
  *min=code==ABS_Z || code==ABS_RZ ? 0 : code==ABS_HAT0X || code==ABS_HAT0Y ? -1 : -32768;
  *max=code==ABS_Z || code==ABS_RZ ? 255 : code==ABS_HAT0X || code==ABS_HAT0Y ? 1 : 32767;
}
static void configure(int fd,int legacy) {
  assert(ioctl(fd,UI_SET_EVBIT,EV_KEY)==0);
  assert(ioctl(fd,UI_SET_EVBIT,EV_ABS)==0);
  for (unsigned i=0;i<sizeof(keys)/sizeof(keys[0]);i++) assert(ioctl(fd,UI_SET_KEYBIT,keys[i])==0);
  for (unsigned i=0;i<sizeof(axes)/sizeof(axes[0]);i++) assert(ioctl(fd,UI_SET_ABSBIT,axes[i])==0);
  if (legacy) {
    struct uinput_user_dev setup={.id={.bustype=BUS_USB,.vendor=0x28de,.product=0x11ff}};
    strcpy(setup.name,"Steam test controller");
    for (unsigned i=0;i<sizeof(axes)/sizeof(axes[0]);i++) limits(axes[i],&setup.absmin[axes[i]],&setup.absmax[axes[i]]);
    assert(write(fd,&setup,sizeof(setup))==(ssize_t)sizeof(setup));
  } else {
    struct uinput_setup setup={.id={.bustype=BUS_USB,.vendor=0x28de,.product=0x11ff}};
    strcpy(setup.name,"Steam test controller");
    assert(ioctl(fd,UI_DEV_SETUP,&setup)==0);
    for (unsigned i=0;i<sizeof(axes)/sizeof(axes[0]);i++) {
      struct uinput_abs_setup abs={.code=axes[i]};
      limits(axes[i],&abs.absinfo.minimum,&abs.absinfo.maximum);
      assert(ioctl(fd,UI_ABS_SETUP,&abs)==0);
    }
  }
}
int main(int argc,char **argv) {
  assert(argc==2);
  ordinary_io();
  assert(open("/dev/uinput",O_RDONLY)<0 && errno==EINVAL);
  assert(open("/dev/uinput",O_RDWR|O_PATH)<0 && errno==EINVAL);
  int fd=open64("/dev/input/uinput",O_RDWR|O_NONBLOCK);
  assert(fd>=0);
  int version=0; assert(ioctl(fd,UI_GET_VERSION,&version)==0 && version==5);
  assert(ioctl(fd,UI_DEV_CREATE)<0 && errno==EINVAL);
  assert(ioctl(fd,UI_SET_KEYBIT,KEY_POWER)<0 && errno==EINVAL);
  configure(fd,!strcmp(argv[1],"legacy"));
  int alias=fcntl64(fd,F_DUPFD_CLOEXEC,32);
  assert(alias>=32);
  int alias2=dup(fd); assert(alias2>=0);
  assert(ioctl(alias,UI_DEV_CREATE)==0);
  char name[64]={0};
  assert(ioctl(fd,UI_GET_SYSNAME(sizeof(name)),name)>0 && !strcmp(name,"input123"));
  assert(close(fd)==0 && close(alias2)==0);
  fd=alias;
  pid_t child=fork(); assert(child>=0);
  if (!child) { assert(fcntl(fd,F_GETFD)<0 && errno==EBADF); _exit(0); }
  int status=0; assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
  /* A valid SYN followed by a forbidden key must not leak a partial packet. */
  struct input_event bad[]={{.type=EV_SYN,.code=SYN_REPORT},{.type=EV_KEY,.code=KEY_POWER,.value=1}};
  assert(write(fd,bad,sizeof(bad))<0 && errno==EINVAL);
  struct input_event events[]={
    {.type=EV_KEY,.code=BTN_A,.value=1},
    {.type=EV_ABS,.code=ABS_X,.value=32767},
    {.type=EV_ABS,.code=ABS_Y,.value=-32768},
    {.type=EV_ABS,.code=ABS_RX,.value=0},
    {.type=EV_ABS,.code=ABS_RY,.value=-1},
    {.type=EV_ABS,.code=ABS_Z,.value=255},
    {.type=EV_ABS,.code=ABS_RZ,.value=128},
    {.type=EV_ABS,.code=ABS_HAT0X,.value=1},
    {.type=EV_ABS,.code=ABS_HAT0Y,.value=-1},
    {.type=EV_SYN,.code=SYN_REPORT}
  };
  struct iovec pieces[]={{events,3},{(char *)events+3,sizeof(events)-3}};
  assert(writev(fd,pieces,2)==(ssize_t)sizeof(events));
  for (unsigned i=0;i<sizeof(events)/sizeof(events[0]);i++) events[i].value=0;
  assert(write(fd,events,sizeof(events))==(ssize_t)sizeof(events));
  if (!strcmp(argv[1],"destroy")) assert(ioctl(fd,UI_DEV_DESTROY)==0);
  assert(close(fd)==0);
  puts("Steam compatibility and libc forwarding passed");
  return 0;
}
