/* One seat's continuously encoded H.264/Opus stream. Only encoded samples
 * cross FD 4; FD 5 carries Start and IDR controls. No raw frames leave here. */
#define _GNU_SOURCE
#include "capture-gpu.h"
#include "encoder-gpu.h"
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <gst/video/video-event.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define MAX_VIDEO (16u*1024u*1024u-32u)
#define BITRATE_KBPS 8000u
static volatile sig_atomic_t stopping;
static gint bad_import;
static void stop(int number) { (void)number; stopping = 1; }
static uint64_t monotonic_ns(void) {
  struct timespec value;
  return clock_gettime(CLOCK_MONOTONIC,&value) ? 0 : (uint64_t)value.tv_sec*1000000000u+(uint64_t)value.tv_nsec;
}
static void be16(unsigned char *p,uint16_t v) { p[0]=v>>8; p[1]=v; }
static void be32(unsigned char *p,uint32_t v) { for(int i=3;i>=0;--i){p[i]=v;v>>=8;} }
static void be64(unsigned char *p,uint64_t v) { for(int i=7;i>=0;--i){p[i]=v;v>>=8;} }
static gboolean write_all(int fd,const void *data,size_t length,uint64_t deadline) {
  const unsigned char *p=data;
  while(length && !stopping && monotonic_ns()<deadline) {
    ssize_t n=write(fd,p,length);
    if(n>0){p+=n;length-=(size_t)n;continue;}
    if(n<0 && errno==EINTR)continue;
    if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) {
      struct pollfd wait={.fd=fd,.events=POLLOUT};
      if(poll(&wait,1,20)<0 && errno!=EINTR)return FALSE;
      continue;
    }
    return FALSE;
  }
  return length==0;
}
static gboolean packet(int fd,unsigned kind,const void *prefix,size_t prefix_size,const void *data,size_t size) {
  unsigned char header[12]={'P','M','E','1',0};header[4]=kind;
  be32(header+8,(uint32_t)(prefix_size+size));
  const uint64_t deadline=monotonic_ns()+2000000000u;
  return write_all(fd,header,sizeof(header),deadline) && write_all(fd,prefix,prefix_size,deadline) && write_all(fd,data,size,deadline);
}
static unsigned number(const char *text,unsigned maximum) {
  char *end=NULL;errno=0;unsigned long result=strtoul(text,&end,10);
  return errno || !*text || *end || result>maximum ? 0 : (unsigned)result;
}
static gboolean token(const char *name) {
  if(!*name || strlen(name)>128)return FALSE;
  for(const char *p=name;*p;++p)if(!g_ascii_isalnum(*p) && *p!='-' && *p!='_')return FALSE;
  return TRUE;
}
static int pin_socket(const char *path) {
  struct stat parent,status;
  if(lstat("/run/polaris",&parent) || !S_ISDIR(parent.st_mode) || parent.st_uid!=geteuid() || (parent.st_mode&07777)!=0700)return -1;
  int fd=open(path,O_PATH|O_NOFOLLOW|O_CLOEXEC);
  if(fd<0)return -1;
  if(fstat(fd,&status) || !S_ISSOCK(status.st_mode) || status.st_uid!=geteuid() ||
    ((status.st_mode&07777)!=0600 && (status.st_mode&07777)!=0700)){close(fd);return -1;}
  return fd;
}
static gboolean capture_name(const char *name) {
  const char prefix[]="/run/polaris/polaris-frames-";
  if(strlen(name)!=sizeof(prefix)-1+64 || strncmp(name,prefix,sizeof(prefix)-1))return FALSE;
  for(const char *p=name+sizeof(prefix)-1;*p;++p)if(!(*p>='0'&&*p<='9')&&!(*p>='a'&&*p<='f'))return FALSE;
  return TRUE;
}
static GstPadProbeReturn check_import(GstPad *pad,GstPadProbeInfo *info,gpointer unused) {
  (void)unused;GstBuffer *buffer=GST_PAD_PROBE_INFO_BUFFER(info);
  if(!buffer || !matching_texture_target(pad,buffer)) { g_atomic_int_set(&bad_import,1);return GST_PAD_PROBE_DROP; }
  return GST_PAD_PROBE_OK;
}
static gboolean bus_failed(GstBus *bus) {
  GstMessage *message=gst_bus_pop_filtered(bus,GST_MESSAGE_ERROR|GST_MESSAGE_EOS);
  if(!message)return FALSE;
  if(GST_MESSAGE_TYPE(message)==GST_MESSAGE_ERROR) {
    GError *error=NULL;gst_message_parse_error(message,&error,NULL);
    fprintf(stderr,"encoder pipeline: %.200s\n",error?error->message:"unknown failure");
    if(error)g_error_free(error);
  }
  gst_message_unref(message);return TRUE;
}
/* Access-unit caps alone do not prove an IDR or the advertised H.264 profile.
 * Inspect byte-stream NAL headers, including SPS fields, before publishing. */
static void h264_headers(const unsigned char *data,size_t size,gboolean *idr,unsigned *profile,unsigned *level) {
  *idr=FALSE;
  for(size_t i=0;i+4<size;++i) {
    if(data[i] || data[i+1] || data[i+2]!=1)continue;
    unsigned type=data[i+3]&31;
    if(type==5)*idr=TRUE;
    if(type==7 && i+6<size){*profile=data[i+4];*level=data[i+6];}
  }
}
static gboolean opus_five_ms(const unsigned char *data,size_t size) {
  if(!size)return FALSE;
  unsigned config=data[0]>>3,code=data[0]&3;
  unsigned frames=code==0?1:code<3?2:size>1?(data[1]&63):0;
  unsigned duration=config>=16?(2500u<<(config&3)):config>=12?(10000u<<(config&1)):(config&3)==3?60000u:(10000u<<(config&3));
  return frames>0 && frames*duration==5000;
}
int main(int argc,char **argv) {
  const gboolean software_test=argc==2 && !strcmp(argv[1],"--self-test");
  const gboolean hardware_test=argc==3 && !strcmp(argv[1],"--self-test-gpu");
  const gboolean synthetic=software_test || hardware_test;
  const gboolean software=software_test || (argc==8 && !strcmp(argv[7],"true"));
  unsigned width=synthetic?640:argc==8?number(argv[4],3840):0;
  unsigned height=synthetic?480:argc==8?number(argv[5],3840):0;
  unsigned refresh=synthetic?60000:argc==8?number(argv[6],240000):0;
  if(width<16 || height<16 || width%2 || height%2 || refresh<1000 ||
    (!synthetic && (!capture_name(argv[1]) || !token(argv[3]) || (strcmp(argv[7],"true") && strcmp(argv[7],"false")))))return 1;
  int output=synthetic?STDOUT_FILENO:4,control=synthetic?STDIN_FILENO:5;
  struct stat output_status,control_status;
  if(fstat(output,&output_status) || fstat(control,&control_status) || !S_ISFIFO(output_status.st_mode) || !S_ISFIFO(control_status.st_mode))return 1;
  if(fcntl(output,F_SETFL,fcntl(output,F_GETFL)|O_NONBLOCK)<0 || fcntl(control,F_SETFL,fcntl(control,F_GETFL)|O_NONBLOCK)<0)return 1;
  struct sigaction action={0};action.sa_handler=stop;
  sigaction(SIGTERM,&action,NULL);sigaction(SIGINT,&action,NULL);signal(SIGPIPE,SIG_IGN);
  g_setenv("GST_REGISTRY_FORK","no",TRUE);g_setenv("GST_GL_PLATFORM","egl",TRUE);g_setenv("GST_GL_API","gles2",TRUE);
  gst_init(NULL,NULL);
  if(!gst_video_meta_get_info())return 1;
  int capture=-1,pulse=-1,result=1;
  struct capture_gpu gpu={.descriptor=-1,.egl=EGL_NO_DISPLAY};
  struct encoder_choice choice={.kind=ENCODER_SOFTWARE};
  if(!synthetic) {
    capture=pin_socket(argv[1]);pulse=pin_socket("/run/polaris/pulse/native");
    if(capture<0 || pulse<0)goto finish;
  }
  if(!software && (!open_gpu(argv[2],&gpu) || !choose_hardware_encoder(gpu.descriptor,&choice))) {
    fprintf(stderr,"no H.264 hardware encoder matches the allocated render device\n");
    goto finish;
  }
  fprintf(stderr,"seat encoder: %s\n",choice.factory?choice.factory:"openh264enc");
  const char *video_head=synthetic?"videotestsrc is-live=true pattern=ball ! videoconvert ! ":
    software?"unixfdsrc name=capture ! videoconvert ! ":"unixfdsrc name=capture ! " CAPTURE_DOWNLOAD_CHAIN;
  const char *audio_head=synthetic?"audiotestsrc is-live=true samplesperbuffer=240 volume=0.05 ! ":"pulsesrc name=audio-source ! ";
  gchar *video_encoder=encoder_description(&choice,BITRATE_KBPS,refresh);
  char *description=g_strdup_printf(
    "%s video/x-raw,format=%s,width=%u,height=%u,framerate=%u/1000 ! %s ! "
    "h264parse config-interval=-1 ! video/x-h264,stream-format=byte-stream,alignment=au,profile=constrained-baseline ! "
    "appsink name=video max-buffers=2 drop=false sync=false async=false enable-last-sample=false "
    "%s audioconvert ! audioresample ! audio/x-raw,format=S16LE,rate=48000,channels=2,layout=interleaved ! "
    "opusenc bitrate=128000 frame-size=5 max-payload-size=1400 audio-type=restricted-lowdelay ! "
    "appsink name=audio max-buffers=8 drop=false sync=false async=false enable-last-sample=false",
    video_head,software?"I420":"NV12",width,height,refresh,video_encoder,audio_head);
  g_free(video_encoder);
  GError *error=NULL;GstElement *pipeline=gst_parse_launch(description,&error);g_free(description);
  if(!pipeline || error){if(error){fprintf(stderr,"encoder construction: %.200s\n",error->message);g_error_free(error);}if(pipeline)gst_object_unref(pipeline);goto finish;}
  if(!synthetic) {
    GstElement *source=gst_bin_get_by_name(GST_BIN(pipeline),"capture");
    char path[64];snprintf(path,sizeof(path),"/proc/self/fd/%d",capture);g_object_set(source,"socket-path",path,NULL);gst_object_unref(source);
    source=gst_bin_get_by_name(GST_BIN(pipeline),"audio-source");
    char *monitor=g_strconcat(argv[3],".monitor",NULL);snprintf(path,sizeof(path),"unix:/proc/self/fd/%d",pulse);
    g_object_set(source,"server",path,"device",monitor,NULL);g_free(monitor);gst_object_unref(source);
  }
  GstPad *import_pads[2]={0};gulong import_probes[2]={0};
  if(!software && !synthetic) {
    GstContext *context=gst_context_new(GST_GL_DISPLAY_CONTEXT_TYPE,TRUE);
    gst_context_set_gl_display(context,GST_GL_DISPLAY(gpu.display));gst_element_set_context(pipeline,context);gst_context_unref(context);
    const char *names[]={"upload","convert"};
    for(unsigned i=0;i<2;++i){GstElement *element=gst_bin_get_by_name(GST_BIN(pipeline),names[i]);import_pads[i]=gst_element_get_static_pad(element,"src");import_probes[i]=gst_pad_add_probe(import_pads[i],GST_PAD_PROBE_TYPE_BUFFER,check_import,NULL,NULL);gst_object_unref(element);}
  }
  GstElement *encoder=gst_bin_get_by_name(GST_BIN(pipeline),"encoder");
  GstElement *sinks[]={gst_bin_get_by_name(GST_BIN(pipeline),"video"),gst_bin_get_by_name(GST_BIN(pipeline),"audio")};
  GstBus *bus=gst_element_get_bus(pipeline);gst_pipeline_set_auto_flush_bus(GST_PIPELINE(pipeline),FALSE);
  gboolean failed=!software && !encoder_matches(encoder,&choice);
  if(!failed)failed=gst_element_set_state(pipeline,GST_STATE_PLAYING)==GST_STATE_CHANGE_FAILURE;
  gboolean ready=FALSE,started=FALSE,need_idr=TRUE,seen_audio=FALSE,seen_video=FALSE;
  unsigned profile=0,level=0,requests=0;uint64_t indices[2]={0};
  const uint64_t startup_deadline=monotonic_ns()+15000000000ull;
  uint64_t last_frame[2]={monotonic_ns(),monotonic_ns()};
  while(!failed && !stopping) {
    if(bus_failed(bus) || g_atomic_int_get(&bad_import)){failed=TRUE;break;}
    unsigned char commands[32];ssize_t count=read(control,commands,sizeof(commands));
    if(count==0 || (count<0 && errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR)){failed=TRUE;break;}
    for(ssize_t i=0;i<count;++i) {
      if(!ready || (!started && commands[i]!=1) || (started && commands[i]!=2)){failed=TRUE;break;}
      started=TRUE;need_idr=TRUE;
      if(!gst_element_send_event(encoder,gst_video_event_new_upstream_force_key_unit(GST_CLOCK_TIME_NONE,TRUE,++requests))){failed=TRUE;break;}
    }
    if(failed)break;
    for(unsigned stream=0;stream<2 && !failed;++stream) {
      GstSample *sample=gst_app_sink_try_pull_sample(GST_APP_SINK(sinks[stream]),GST_MSECOND);
      if(!sample)continue;
      GstBuffer *buffer=gst_sample_get_buffer(sample);GstMapInfo map;
      if(!buffer || !gst_buffer_map(buffer,&map,GST_MAP_READ)){gst_sample_unref(sample);failed=TRUE;break;}
      last_frame[stream]=monotonic_ns();gboolean idr=FALSE;
      if(!map.size || map.size>(stream?1400:MAX_VIDEO))failed=TRUE;
      else if(stream==0) {
        unsigned current_profile=profile,current_level=level;
        h264_headers(map.data,map.size,&idr,&current_profile,&current_level);
        if(ready && (current_profile!=profile || current_level!=level))failed=TRUE;
        profile=current_profile;level=current_level;
        if(idr && profile==66 && level>=10 && level<=62)seen_video=TRUE;
      } else { if(!opus_five_ms(map.data,map.size))failed=TRUE;else seen_audio=TRUE; }
      if(!failed && !ready && seen_video && seen_audio) {
        if(!software && !encoder_matches(encoder,&choice)){failed=TRUE;gst_buffer_unmap(buffer,&map);gst_sample_unref(sample);break;}
        unsigned char config[32]={1,1,0,0};config[2]=profile;config[3]=level;
        be16(config+4,width);be16(config+6,height);be32(config+8,refresh);be32(config+12,1000);be32(config+16,BITRATE_KBPS);
        config[20]=1;config[21]=2;be16(config+22,5000);be32(config+24,48000);
        failed=!packet(output,1,config,sizeof(config),NULL,0);ready=!failed;
      }
      if(!failed && started && (stream || !need_idr || idr)) {
        if(!stream)need_idr=FALSE;
        unsigned char frame[32]={1,0};frame[1]=idr?1:0;
        be64(frame+8,indices[stream]++);
        /* Capture clock provenance is not established across unixfdsrc. Zero
         * means unknown; never label a presentation timestamp as capture time. */
        be64(frame+24,monotonic_ns());
        failed=!packet(output,stream?3:2,frame,sizeof(frame),map.data,map.size) && !stopping;
      }
      gst_buffer_unmap(buffer,&map);gst_sample_unref(sample);
    }
    const uint64_t now=monotonic_ns();
    if((!ready && now>startup_deadline) || now-last_frame[0]>10000000000ull || now-last_frame[1]>10000000000ull)failed=TRUE;
  }
  if(gst_element_set_state(pipeline,GST_STATE_NULL)!=GST_STATE_CHANGE_SUCCESS)_exit(1);
  if(bus_failed(bus))failed=TRUE;
  for(unsigned i=0;i<2;++i){if(import_pads[i]){gst_pad_remove_probe(import_pads[i],import_probes[i]);gst_object_unref(import_pads[i]);}gst_object_unref(sinks[i]);}
  gst_object_unref(bus);gst_object_unref(encoder);gst_object_unref(pipeline);
  result=failed?1:0;
finish:
  g_free(choice.factory);release_gpu(&gpu);if(capture>=0)close(capture);if(pulse>=0)close(pulse);gst_deinit();return result;
}
