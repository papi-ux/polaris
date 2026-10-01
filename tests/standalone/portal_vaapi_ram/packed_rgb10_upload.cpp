// #171: production PipeWire staging and graphics::load_ram at the real GL call site.
// Default: strict GL endpoint recorder. --software-egl: surfaceless llvmpipe texture readback.
// Neither mode starts portal capture, a VAAPI encoder, a host stream or a physical display.
#include "src/platform/linux/graphics.h"
#include "src/platform/linux/pipewire_capture.h"
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include <boost/log/utility/setup/console.hpp>

namespace {
  using rgb_t = std::array<double, 3>;
  constexpr int width = 2, height = 2;
  constexpr std::array<GLenum, 6> state_names {
    GL_UNPACK_ROW_LENGTH, GL_UNPACK_ALIGNMENT, GL_UNPACK_SWAP_BYTES,
    GL_UNPACK_SKIP_ROWS, GL_UNPACK_SKIP_PIXELS, GL_PIXEL_UNPACK_BUFFER_BINDING
  };
  using unpack_t = std::array<GLint, state_names.size()>;
  constexpr unpack_t dirty_unpack {7, 8, 1, 3, 5, 77};
  struct endpoint_t {
    unpack_t state {0, 1, 0, 0, 0, 0};
    GLenum format = 0, type = 0, next_error = GL_NO_ERROR;
    bool inject_error = false;
    int calls = 0;
    std::vector<rgb_t> pixels = std::vector<rgb_t>(width * height);
  } endpoint;
  bool software_egl = false;

  std::size_t state_index(GLenum name) {
    auto found = std::find(state_names.begin(), state_names.end(), name);
    if (found == state_names.end()) throw std::runtime_error("unexpected unpack field");
    return found - state_names.begin();
  }
  void GLAD_API_PTR gen_textures(GLsizei count, GLuint *ids) {for (int i=0;i<count;++i) ids[i]=100+i;}
  void GLAD_API_PTR delete_textures(GLsizei, const GLuint *) {}
  void GLAD_API_PTR bind_texture(GLenum, GLuint) {}
  void GLAD_API_PTR parameter_i(GLenum, GLenum, GLint) {}
  void GLAD_API_PTR parameter_fv(GLenum, GLenum, const GLfloat *) {}
  void GLAD_API_PTR storage(GLenum, GLsizei, GLenum, GLsizei w, GLsizei h) {
    if (w!=width || h!=height) throw std::runtime_error("unexpected allocated input texture size");
  }
  void GLAD_API_PTR pixel_store(GLenum name, GLint value) {endpoint.state[state_index(name)]=value;}
  void GLAD_API_PTR get_integer(GLenum name, GLint *value) {*value=endpoint.state[state_index(name)];}
  void GLAD_API_PTR bind_buffer(GLenum target, GLuint value) {
    if (target!=GL_PIXEL_UNPACK_BUFFER) throw std::runtime_error("unexpected buffer target");
    endpoint.state.back()=value;
  }
  GLenum GLAD_API_PTR get_error() {auto error=endpoint.next_error;endpoint.next_error=GL_NO_ERROR;return error;}
  void GLAD_API_PTR upload(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h,
                           GLenum format, GLenum type, const void *data) {
    if (target!=GL_TEXTURE_2D || level || x || y<0 || y+h>height || w!=width || !data ||
        (format!=GL_RGBA && format!=GL_BGRA)) throw std::runtime_error("unexpected GL upload geometry");
    ++endpoint.calls;endpoint.format=format;endpoint.type=type;
    if (endpoint.state[3] || endpoint.state[4] || endpoint.state[5])
      throw std::runtime_error("CPU pointer upload retained skip offsets or a PBO");
    if (endpoint.inject_error) {endpoint.next_error=GL_INVALID_OPERATION;return;}
    const int row_pixels=endpoint.state[0]?endpoint.state[0]:w;
    const int row_bytes=row_pixels*4;
    const int stride=((row_bytes+endpoint.state[1]-1)/endpoint.state[1])*endpoint.state[1];
    const auto *bytes=static_cast<const std::uint8_t *>(data);
    for (int row=0;row<h;++row) for (int column=0;column<w;++column) {
      const auto *p=bytes+row*stride+column*4;
      rgb_t channels;
      if (type==GL_UNSIGNED_BYTE) channels={p[0]/255.0,p[1]/255.0,p[2]/255.0};
      else if (type==GL_UNSIGNED_INT_2_10_10_10_REV) {
        std::uint32_t word;std::memcpy(&word,p,4);
        if (endpoint.state[2]) word=std::byteswap(word);
        channels={double(word&1023u)/1023,double((word>>10)&1023u)/1023,double((word>>20)&1023u)/1023};
      } else throw std::runtime_error("unsupported transfer type in endpoint oracle");
      if (format==GL_BGRA) std::swap(channels[0],channels[2]);
      endpoint.pixels[(row+y)*width+column]=channels;
    }
  }
  unpack_t unpack_state() {
    unpack_t values;
    for (int i=0;i<state_names.size();++i) gl::ctx.GetIntegerv(state_names[i],&values[i]);
    return values;
  }
  void set_unpack(const unpack_t &values) {
    for (int i=0;i<state_names.size()-1;++i) gl::ctx.PixelStorei(state_names[i],values[i]);
    gl::ctx.BindBuffer(GL_PIXEL_UNPACK_BUFFER,values.back());
  }
  void allocate(egl::sws_t &converter, bool packed) {
    converter.in_width=width;converter.in_height=height;
    converter.tex=gl::tex_t::make(1);
    gl::ctx.BindTexture(GL_TEXTURE_2D,converter.tex[0]);
    gl::ctx.TexStorage2D(GL_TEXTURE_2D,1,packed?GL_RGB10_A2:GL_RGBA8,width,height);
    if (gl::ctx.GetError()!=GL_NO_ERROR) throw std::runtime_error("input texture allocation failed");
  }
  void write_le(std::uint8_t *p,std::uint32_t word) {
    for (int i=0;i<4;++i) p[i]=(word>>(i*8))&255u;
  }
  nlohmann::json exercise(std::uint32_t spa,int destination_stride,const char *name,bool dirty=false,bool legacy=false) {
    constexpr int source_stride=16,chunk_offset=5;
    const bool packed=spa==SPA_VIDEO_FORMAT_xBGR_210LE || spa==SPA_VIDEO_FORMAT_xRGB_210LE;
    const std::array<std::array<unsigned,3>,4> sample=packed?
      std::array<std::array<unsigned,3>,4>{{{1023,341,682},{17,1000,89},{511,67,900},{123,456,789}}}:
      std::array<std::array<unsigned,3>,4>{{{251,79,23},{13,199,101},{67,41,211},{99,159,201}}};
    std::vector<std::uint8_t> source(chunk_offset+source_stride*height,0xe7);
    for (int i=0;i<4;++i) {
      auto *p=source.data()+chunk_offset+(i/2)*source_stride+(i%2)*4;
      auto [r,g,b]=sample[i];
      if (spa==SPA_VIDEO_FORMAT_xBGR_210LE) write_le(p,r|(g<<10)|(b<<20)|0xc0000000u);
      else if (spa==SPA_VIDEO_FORMAT_xRGB_210LE) write_le(p,b|(g<<10)|(r<<20)|0xc0000000u);
      else if (spa==SPA_VIDEO_FORMAT_RGBx) {p[0]=r;p[1]=g;p[2]=b;p[3]=255;}
      else {p[0]=b;p[1]=g;p[2]=r;p[3]=255;}
    }
    // Explicit LE byte literals independently fence the packing/endian oracle.
    if (packed) {
      constexpr std::array<std::uint8_t,4> xbgr_le {0xff,0x57,0xa5,0xea};
      constexpr std::array<std::uint8_t,4> xrgb_le {0xaa,0x56,0xf5,0xff};
      const auto &literal=spa==SPA_VIDEO_FORMAT_xBGR_210LE?xbgr_le:xrgb_le;
      if (!std::equal(literal.begin(),literal.end(),source.begin()+chunk_offset))
        throw std::runtime_error("packed LE source does not match independent byte literal");
    }
    std::vector<std::uint8_t> destination(destination_stride*height,0xcd);
    const bool copied=pipewire_capture::copy_memptr_frame_to_bgra(source.data(),source.size(),chunk_offset,
      source_stride*(height-1)+width*4,width,height,source_stride,spa,destination.data(),destination_stride);
    if (!copied) throw std::runtime_error("production PipeWire copy rejected valid fixture");
    platf::img_t image;
    image.data=destination.data();image.width=width;image.height=height;image.pixel_pitch=4;image.row_pitch=destination_stride;
    image.frame_metadata=legacy?platf::frame_metadata_t{}:pipewire_capture::cpu_frame_metadata(spa);
    egl::sws_t converter {};
    endpoint={};allocate(converter,packed);
    auto wanted=dirty?dirty_unpack:unpack_t{0,1,0,0,0,0};
    GLuint pbo=0;
    if (software_egl && dirty) {
      gl::ctx.GenBuffers(1,&pbo);wanted.back()=pbo;
      gl::ctx.BindBuffer(GL_PIXEL_UNPACK_BUFFER,pbo);
      std::array<std::uint8_t,256> poison {};poison.fill(0xde);
      gl::ctx.BufferData(GL_PIXEL_UNPACK_BUFFER,poison.size(),poison.data(),GL_STATIC_DRAW);
    }
    set_unpack(wanted);
    const int status=converter.load_ram(image);
    const auto restored=unpack_state();
    std::vector<rgb_t> actual;
    if (software_egl) {
      std::array<float,width*height*4> rgba {};
      gl::ctx.PixelStorei(GL_PACK_ALIGNMENT,1);
      gl::ctx.GetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_FLOAT,rgba.data());
      if (gl::ctx.GetError()!=GL_NO_ERROR) throw std::runtime_error("software EGL readback failed");
      for (int i=0;i<width*height;++i) actual.push_back({rgba[i*4],rgba[i*4+1],rgba[i*4+2]});
      gl::ctx.BindBuffer(GL_PIXEL_UNPACK_BUFFER,0);
      if (pbo) gl::ctx.DeleteBuffers(1,&pbo);
    } else actual=endpoint.pixels;
    set_unpack({0,1,0,0,0,0});
    double max_error=0;
    std::vector<rgb_t> expected;
    for (int i=0;i<4;++i) {
      rgb_t pixel={sample[i][0]/double(packed?1023:255),sample[i][1]/double(packed?1023:255),sample[i][2]/double(packed?1023:255)};
      expected.push_back(pixel);
      for (int c=0;c<3;++c) max_error=std::max(max_error,std::abs(pixel[c]-actual.at(i)[c]));
    }
    bool padding_unchanged=true;
    for (int row=0;row<height;++row) for (int byte=width*4;byte<destination_stride;++byte)
      padding_unchanged &= destination[row*destination_stride+byte]==0xcd;
    return {{"name",name},{"spa_format",spa},{"capture_metadata_format",platf::from_frame_format(image.frame_metadata.format)},
      {"ram_pixel_layout",static_cast<int>(image.frame_metadata.ram_pixel_layout)},{"copied",copied},
      {"source_stride",source_stride},{"destination_stride",destination_stride},{"load_status",status},
      {"upload_format",endpoint.format},{"upload_type",endpoint.type},{"upload_calls",endpoint.calls},
      {"actual_rgb",actual},{"expected_rgb",expected},{"maximum_channel_error",max_error},
      {"dirty_unpack",wanted},{"restored_unpack",restored},{"padding_unchanged",padding_unchanged},
      {"pass",status==0 && max_error<0.000001 && restored==wanted && padding_unchanged}};
  }

  template<class Mutate> nlohmann::json reject(const char *name,Mutate mutate,int expected_status=-1) {
    std::array<std::uint8_t,16> bytes {};
    platf::img_t image;
    image.data=bytes.data();image.width=width;image.height=height;image.pixel_pitch=4;image.row_pitch=8;
    image.frame_metadata=pipewire_capture::cpu_frame_metadata(SPA_VIDEO_FORMAT_xBGR_210LE);
    egl::sws_t converter {};endpoint={};allocate(converter,true);set_unpack(dirty_unpack);
    mutate(image,converter);
    const auto status=converter.load_ram(image);
    const auto restored=unpack_state();
    return {{"name",name},{"load_status",status},{"expected_status",expected_status},{"upload_calls",endpoint.calls},
      {"loaded_texture",converter.loaded_texture},{"restored_unpack",restored},
      {"pass",status==expected_status && endpoint.calls==0 && converter.loaded_texture==0 && restored==dirty_unpack}};
  }

  nlohmann::json injected_error(const char *name,int stride) {
    std::array<std::uint8_t,24> bytes {};
    platf::img_t image;
    image.data=bytes.data();image.width=width;image.height=height;image.pixel_pitch=4;image.row_pitch=stride;
    image.frame_metadata=pipewire_capture::cpu_frame_metadata(SPA_VIDEO_FORMAT_xBGR_210LE);
    egl::sws_t converter {};endpoint={};allocate(converter,true);set_unpack(dirty_unpack);endpoint.inject_error=true;
    const auto status=converter.load_ram(image);
    const auto restored=unpack_state();
    return {{"name",name},{"load_status",status},{"upload_calls",endpoint.calls},{"restored_unpack",restored},
      {"loaded_texture",converter.loaded_texture},{"pass",status==-1 && endpoint.calls==1 && converter.loaded_texture==0 && restored==dirty_unpack}};
  }

  struct software_context_t {
    EGLDisplay display=EGL_NO_DISPLAY;
    EGLContext context=EGL_NO_CONTEXT;
    software_context_t() {
      if (!gladLoaderLoadEGL(EGL_NO_DISPLAY)) throw std::runtime_error("could not load EGL");
      if (!eglGetPlatformDisplay) throw std::runtime_error("EGL platform display unavailable");
      constexpr EGLenum surfaceless_mesa=0x31dd;
      display=eglGetPlatformDisplay(surfaceless_mesa,EGL_DEFAULT_DISPLAY,nullptr);
      if (display==EGL_NO_DISPLAY || !eglInitialize(display,nullptr,nullptr) || !gladLoaderLoadEGL(display) || !eglBindAPI(EGL_OPENGL_API))
        throw std::runtime_error("could not initialize surfaceless desktop EGL");
      constexpr EGLint attributes[]={EGL_CONTEXT_MAJOR_VERSION,3,EGL_CONTEXT_MINOR_VERSION,3,EGL_CONTEXT_OPENGL_PROFILE_MASK,EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT,EGL_NONE};
      context=eglCreateContext(display,nullptr,EGL_NO_CONTEXT,attributes);
      if (context==EGL_NO_CONTEXT || !eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,context) || !gladLoaderLoadGLContext(&gl::ctx))
        throw std::runtime_error("could not create software desktop GL context");
      const std::string renderer=reinterpret_cast<const char *>(gl::ctx.GetString(GL_RENDERER));
      if (renderer.find("llvmpipe")==std::string::npos) throw std::runtime_error("software probe must use llvmpipe, got "+renderer);
    }
    ~software_context_t() {
      if (context!=EGL_NO_CONTEXT) {eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);eglDestroyContext(display,context);}
      if (display!=EGL_NO_DISPLAY) eglTerminate(display);
    }
  };
}

int main(int argc,char **argv) {
  boost::log::add_console_log(std::cerr);
  software_egl=argc==2 && std::string(argv[1])=="--software-egl";
  std::optional<software_context_t> context;
  nlohmann::json results=nlohmann::json::array();
  try {
    if (software_egl) context.emplace();
    else {
      gl::ctx.GenTextures=gen_textures;gl::ctx.DeleteTextures=delete_textures;gl::ctx.BindTexture=bind_texture;
      gl::ctx.TexParameteri=parameter_i;gl::ctx.TexParameterfv=parameter_fv;gl::ctx.TexStorage2D=storage;
      gl::ctx.TexSubImage2D=upload;gl::ctx.PixelStorei=pixel_store;gl::ctx.GetIntegerv=get_integer;
      gl::ctx.GetError=get_error;gl::ctx.BindBuffer=bind_buffer;
    }
    results.push_back(exercise(SPA_VIDEO_FORMAT_BGRx,8,"BGRx8_tight_control"));
    results.push_back(exercise(SPA_VIDEO_FORMAT_RGBx,8,"RGBx8_normalized_control"));
    results.push_back(exercise(SPA_VIDEO_FORMAT_xBGR_210LE,8,"xBGR210LE_asymmetric_words"));
    results.push_back(exercise(SPA_VIDEO_FORMAT_xRGB_210LE,8,"xRGB210LE_asymmetric_words"));
    results.push_back(exercise(SPA_VIDEO_FORMAT_BGRx,12,"BGRx8_padded_destination_rows"));
    results.push_back(exercise(SPA_VIDEO_FORMAT_xBGR_210LE,12,"xBGR210LE_padded_destination_rows"));
    results.push_back(exercise(SPA_VIDEO_FORMAT_xRGB_210LE,12,"xRGB210LE_padded_destination_rows"));
    results.push_back(exercise(SPA_VIDEO_FORMAT_BGRx,11,"BGRx8_odd_padding_dirty_state",true));
    results.push_back(exercise(SPA_VIDEO_FORMAT_xBGR_210LE,11,"xBGR210LE_odd_padding_dirty_state",true));
    results.push_back(exercise(SPA_VIDEO_FORMAT_xRGB_210LE,11,"xRGB210LE_odd_padding_dirty_state",true));
    results.push_back(exercise(SPA_VIDEO_FORMAT_xBGR_210LE,12,"xBGR210LE_padded_dirty_state",true));
    results.push_back(exercise(SPA_VIDEO_FORMAT_xRGB_210LE,12,"xRGB210LE_padded_dirty_state",true));
    results.push_back(exercise(SPA_VIDEO_FORMAT_BGRx,8,"legacy_unspecified_BGRA8_primer_contract",true,true));
    if (!software_egl) {
      results.push_back(reject("null_cpu_pointer",[](auto &i,auto &){i.data=nullptr;}));
      results.push_back(reject("zero_width",[](auto &i,auto &){i.width=0;}));
      results.push_back(reject("negative_height",[](auto &i,auto &){i.height=-1;}));
      results.push_back(reject("width_exceeds_allocated_texture",[](auto &i,auto &){i.width=3;i.row_pitch=12;}));
      results.push_back(reject("height_exceeds_allocated_texture",[](auto &i,auto &){i.height=3;}));
      results.push_back(reject("partial_frame_cannot_retain_old_texture_pixels",[](auto &i,auto &){i.width=1;}));
      results.push_back(reject("pixel_pitch_not_four",[](auto &i,auto &){i.pixel_pitch=8;}));
      results.push_back(reject("stride_shorter_than_row",[](auto &i,auto &){i.row_pitch=7;}));
      results.push_back(reject("negative_stride",[](auto &i,auto &){i.row_pitch=-8;}));
      results.push_back(reject("width_multiplication_overflow",[](auto &i,auto &s){i.width=std::numeric_limits<int>::max();s.in_width=i.width;}));
      results.push_back(reject("missing_allocated_texture",[](auto &,auto &s){s.tex=gl::tex_t{};}));
      results.push_back(reject("contradictory_packed_format",[](auto &i,auto &){i.frame_metadata.format=platf::frame_format_e::bgra8;},platf::convert_capture_unreadable));
      results.push_back(reject("contradictory_packed_gpu_residency",[](auto &i,auto &){i.frame_metadata.residency=platf::frame_residency_e::gpu;},platf::convert_capture_unreadable));
      results.push_back(reject("contradictory_packed_dmabuf_transport",[](auto &i,auto &){i.frame_metadata.transport=platf::frame_transport_e::dmabuf;},platf::convert_capture_unreadable));
      results.push_back(reject("contradictory_BGRA8_format",[](auto &i,auto &){i.frame_metadata.ram_pixel_layout=platf::ram_pixel_layout_e::bgra8;},platf::convert_capture_unreadable));
      results.push_back(reject("unknown_explicit_packing",[](auto &i,auto &){i.frame_metadata.ram_pixel_layout=static_cast<platf::ram_pixel_layout_e>(999);},platf::convert_capture_unreadable));
      results.push_back(injected_error("GL_error_restores_unpack_and_PBO",8));
      results.push_back(injected_error("odd_stride_GL_error_stops_first_row_and_restores",11));
    }
  } catch (const std::exception &error) {std::cerr<<error.what()<<'\n';return 2;}
  const auto passed=std::count_if(results.begin(),results.end(),[](const auto &r){return r.at("pass").template get<bool>();});
  nlohmann::json report {{"fixture_only",true},{"production_upload_invoked",true},{"physical_GPU_or_stream_created",false},
    {"software_egl_readback",software_egl},{"tests",results.size()},{"passed",passed},{"failed",results.size()-passed},{"results",results}};
  if (software_egl) {report["renderer"]=reinterpret_cast<const char *>(gl::ctx.GetString(GL_RENDERER));report["GL_version"]=reinterpret_cast<const char *>(gl::ctx.GetString(GL_VERSION));}
  std::cout<<report.dump(2)<<'\n';
  return passed==results.size()?0:1;
}
