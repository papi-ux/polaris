#define _GNU_SOURCE
#include <dlfcn.h>
#include <linux/input.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
/* A second preload library models evdev readback for compatibility tests only.
 * The produced worker never contains this library. */
int ioctl(int fd,unsigned long request,...) {
  va_list args; va_start(args,request);
  uintptr_t arg=va_arg(args,uintptr_t); va_end(args);
  const char *configured=getenv("POLARIS_TEST_EVDEV_FD");
  if (configured && fd==atoi(configured)) {
    if (request==EVIOCGID) {
      struct input_id id={.bustype=BUS_USB,.vendor=0x28de,.product=0x11ff};
      if (getenv("POLARIS_TEST_WRONG_ID")) id.product=0x1102;
      memcpy((void *)arg,&id,sizeof(id)); return 0;
    }
    if (_IOC_TYPE(request)=='E' && _IOC_NR(request)==6 && _IOC_DIR(request)==_IOC_READ) {
      const char *name=getenv("POLARIS_TEST_KERNEL_NAME");
      size_t n=strlen(name)+1;
      if (n>_IOC_SIZE(request)) n=_IOC_SIZE(request);
      memcpy((void *)arg,name,n); return (int)n;
    }
  }
  int (*next)(int,unsigned long,...)=dlsym(RTLD_NEXT,"ioctl");
  return next(fd,request,arg);
}
