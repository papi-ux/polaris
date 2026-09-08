/* Private offline workload acceptance, not production seat status or media. */
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <stdint.h>
#include <stdio.h>

static int visited, matches, invalid;
static unsigned long observed[6];
static void inspect(Display *display,Window window,Atom property,unsigned depth) {
  if (++visited>256 || depth>4) { invalid=1; return; }
  Atom type; int format; unsigned long count,remaining; unsigned char *data=NULL;
  if (XGetWindowProperty(display,window,property,0,6,False,XA_CARDINAL,&type,&format,&count,&remaining,&data)==Success && type!=None) {
    if (type!=XA_CARDINAL || format!=32 || count!=6 || remaining || !data) invalid=1;
    else {
      unsigned long *values=(unsigned long *)data;
      if (values[0]!=1 || !values[4] || values[5]<=1) invalid=1;
      for (int i=0;i<6;++i) { if (values[i]>UINT32_MAX) invalid=1; observed[i]=values[i]; }
      ++matches;
    }
  }
  if (data) XFree(data);
  Window root,parent,*children=NULL; unsigned count_children=0;
  if (!XQueryTree(display,window,&root,&parent,&children,&count_children)) { invalid=1; return; }
  if (count_children>256) invalid=1;
  else for (unsigned i=0;i<count_children && !invalid;++i) inspect(display,children[i],property,depth+1);
  if (children) XFree(children);
}
int main(int argc,char **argv) {
  (void)argv; if (argc!=1) return 1;
  Display *display=XOpenDisplay(NULL); if (!display) return 1;
  Atom property=XInternAtom(display,"_POLARIS_INPUT_STATE_V1",True);
  if (property!=None) inspect(display,DefaultRootWindow(display),property,0);
  XCloseDisplay(display);
  if (invalid || matches!=1) return 1;
  printf("{\"keyboard\":%lu,\"pointer\":%lu,\"gamepad\":%lu,\"frames\":%lu,\"pid\":%lu}\n",
    observed[1],observed[2],observed[3],observed[4],observed[5]);
  return 0;
}
