// Include the real internal va_ram_t class; no extracted conversion helper.
// No VAAPI init/set_frame or render-device call is made by this fixture.
#include "src/platform/linux/vaapi.cpp"
#include "src/platform/linux/pipewire_capture.h"
#include <iostream>
#include <nlohmann/json.hpp>
#include <boost/log/utility/setup/console.hpp>
namespace config {video_t video {};}
namespace gbm {device_destroy_fn device_destroy=nullptr;create_device_fn create_device=nullptr;}
namespace {
  int uploads=0,draws=0,framebuffers=0;
  bool upload_error=false,conversion_error=false;
  GLenum next_error=GL_NO_ERROR;
  std::array<GLint,6> unpack {0,1,0,0,0,0};
  constexpr std::array<GLenum,6> fields {GL_UNPACK_ROW_LENGTH,GL_UNPACK_ALIGNMENT,GL_UNPACK_SWAP_BYTES,
    GL_UNPACK_SKIP_ROWS,GL_UNPACK_SKIP_PIXELS,GL_PIXEL_UNPACK_BUFFER_BINDING};
  void GLAD_API_PTR gen(GLsizei n,GLuint *ids) {for(int i=0;i<n;++i)ids[i]=100+i;}
  void GLAD_API_PTR destroy(GLsizei,const GLuint *) {}
  void GLAD_API_PTR bind(GLenum,GLuint) {}
  void GLAD_API_PTR parameter(GLenum,GLenum,GLint) {}
  void GLAD_API_PTR parameterfv(GLenum,GLenum,const GLfloat *) {}
  void GLAD_API_PTR pixelstore(GLenum f,GLint v) {unpack[std::find(fields.begin(),fields.end(),f)-fields.begin()]=v;}
  void GLAD_API_PTR getinteger(GLenum f,GLint *v) {*v=unpack[std::find(fields.begin(),fields.end(),f)-fields.begin()];}
  void GLAD_API_PTR bindbuffer(GLenum,GLuint v) {unpack.back()=v;}
  GLenum GLAD_API_PTR geterror() {auto e=next_error;next_error=GL_NO_ERROR;return e;}
  void GLAD_API_PTR upload(GLenum,GLint,GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,const void *) {
    ++uploads;if(upload_error)next_error=GL_INVALID_OPERATION;
  }
  void GLAD_API_PTR framebuffer(GLenum,GLuint) {++framebuffers;}
  void GLAD_API_PTR drawbuffers(GLsizei,const GLenum *) {}
  GLenum GLAD_API_PTR checkframebuffer(GLenum) {return conversion_error?GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT:GL_FRAMEBUFFER_COMPLETE;}
  void GLAD_API_PTR useprogram(GLuint) {}
  void GLAD_API_PTR viewport(GLint,GLint,GLsizei,GLsizei) {}
  void GLAD_API_PTR draw(GLenum,GLint,GLsizei) {++draws;}
  void GLAD_API_PTR flush() {}
  nlohmann::json exercise(const char *name,int kind,int expected_status,int expected_uploads,int expected_draws) {
    uploads=draws=framebuffers=0;next_error=GL_NO_ERROR;upload_error=kind==1;conversion_error=kind==2;
    va::va_ram_t device;
    device.sws={};device.sws.in_width=2;device.sws.in_height=2;device.sws.out_width=2;device.sws.out_height=2;
    device.sws.tex=gl::tex_t::make(1);
    device.nv12->buf=gl::frame_buf_t::make(2);
    std::array<std::uint8_t,16> bytes {};
    platf::img_t image;
    image.data=bytes.data();image.width=2;image.height=2;image.pixel_pitch=4;image.row_pitch=8;
    if(kind==3) {
      image.frame_metadata=pipewire_capture::cpu_frame_metadata(SPA_VIDEO_FORMAT_xBGR_210LE);
      image.frame_metadata.format=platf::frame_format_e::bgra8;
    }
    if(kind==4)image.data=nullptr;
    const auto status=device.convert(image); // Actual production va_ram_t::convert.
    return {{"name",name},{"status",status},{"expected_status",expected_status},{"uploads",uploads},
      {"draws",draws},{"framebuffer_binds",framebuffers},{"pass",status==expected_status && uploads==expected_uploads && draws==expected_draws}};
  }
}
int main() {
  boost::log::add_console_log(std::cerr);
  gl::ctx.GenTextures=gen;gl::ctx.DeleteTextures=destroy;gl::ctx.BindTexture=bind;
  gl::ctx.TexParameteri=parameter;gl::ctx.TexParameterfv=parameterfv;gl::ctx.PixelStorei=pixelstore;
  gl::ctx.GetIntegerv=getinteger;gl::ctx.BindBuffer=bindbuffer;gl::ctx.GetError=geterror;gl::ctx.TexSubImage2D=upload;
  gl::ctx.GenFramebuffers=gen;gl::ctx.DeleteFramebuffers=destroy;gl::ctx.BindFramebuffer=framebuffer;
  gl::ctx.DrawBuffers=drawbuffers;gl::ctx.CheckFramebufferStatus=checkframebuffer;gl::ctx.UseProgram=useprogram;
  gl::ctx.Viewport=viewport;gl::ctx.DrawArrays=draw;gl::ctx.Flush=flush;
  nlohmann::json results=nlohmann::json::array();
  results.push_back(exercise("valid_legacy_primer_upload_converts",0,0,1,2));
  results.push_back(exercise("GL_upload_failure_skips_conversion",1,-1,1,0));
  results.push_back(exercise("actual_sws_conversion_failure_propagates",2,-1,1,0));
  results.push_back(exercise("permanent_bad_packing_propagates_end_stream_status",3,platf::convert_capture_unreadable,0,0));
  results.push_back(exercise("invalid_frame_skips_conversion",4,-1,0,0));
  const auto passed=std::count_if(results.begin(),results.end(),[](const auto &r){return r.at("pass").template get<bool>();});
  std::cout<<nlohmann::json{{"fixture_only",true},{"production_va_ram_convert_invoked",true},{"VAAPI_or_stream_created",false},
    {"tests",results.size()},{"passed",passed},{"failed",results.size()-passed},{"results",results}}.dump(2)<<'\n';
  return passed==results.size()?0:1;
}
