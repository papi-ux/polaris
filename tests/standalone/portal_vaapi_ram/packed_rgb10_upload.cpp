// #171: invoke the production PipeWire CPU copy and sws_t::load_ram, observing its GL endpoint.
// No GPU, capture session or host stream is created. Mock GL interprets the submitted API packing.
#include "src/platform/linux/graphics.h"
#include "src/platform/linux/pipewire_capture.h"
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <nlohmann/json.hpp>

namespace {
  using rgb_t = std::array<double, 3>;
  struct endpoint_t {
    GLint row_length = 0;
    GLint alignment = 1;  // Actual EGL context initialization sets UNPACK_ALIGNMENT=1.
    GLint swap_bytes = 0;
    GLenum format = 0;
    GLenum type = 0;
    std::vector<rgb_t> pixels;
  } endpoint;

  void GLAD_API_PTR gen_textures(GLsizei count, GLuint *ids) {
    for (GLsizei i = 0; i < count; ++i) ids[i] = 100 + i;
  }
  void GLAD_API_PTR delete_textures(GLsizei, const GLuint *) {}
  void GLAD_API_PTR bind_texture(GLenum, GLuint) {}
  void GLAD_API_PTR texture_parameter_i(GLenum, GLenum, GLint) {}
  void GLAD_API_PTR texture_parameter_fv(GLenum, GLenum, const GLfloat *) {}
  void GLAD_API_PTR pixel_store(GLenum pname, GLint value) {
    if (pname == GL_UNPACK_ROW_LENGTH) endpoint.row_length = value;
    if (pname == GL_UNPACK_ALIGNMENT) endpoint.alignment = value;
    if (pname == GL_UNPACK_SWAP_BYTES) endpoint.swap_bytes = value;
  }
  void GLAD_API_PTR get_integer(GLenum pname, GLint *value) {
    if (pname == GL_UNPACK_ROW_LENGTH) *value = endpoint.row_length;
    else if (pname == GL_UNPACK_ALIGNMENT) *value = endpoint.alignment;
    else if (pname == GL_UNPACK_SWAP_BYTES) *value = endpoint.swap_bytes;
    else *value = 0;
  }
  GLenum GLAD_API_PTR no_error() { return GL_NO_ERROR; }
  void GLAD_API_PTR upload(GLenum target, GLint level, GLint x, GLint y, GLsizei width, GLsizei height,
                       GLenum format, GLenum type, const void *data) {
    if (target != GL_TEXTURE_2D || level || x || y || !data ||
        (format != GL_RGBA && format != GL_BGRA)) throw std::runtime_error("unexpected GL upload");
    endpoint.format = format; endpoint.type = type; endpoint.pixels.clear();
    const int row_pixels = endpoint.row_length ? endpoint.row_length : width;
    const int row_bytes = row_pixels * 4;
    const int stride = ((row_bytes + endpoint.alignment - 1) / endpoint.alignment) * endpoint.alignment;
    const auto *bytes = static_cast<const std::uint8_t *>(data);
    for (int row = 0; row < height; ++row) for (int column = 0; column < width; ++column) {
      const auto *p = bytes + row * stride + column * 4;
      rgb_t channels;
      if (type == GL_UNSIGNED_BYTE) {
        channels = {p[0] / 255.0, p[1] / 255.0, p[2] / 255.0};
      } else if (type == GL_UNSIGNED_INT_2_10_10_10_REV) {
        std::uint32_t word;std::memcpy(&word,p,4);
        if (endpoint.swap_bytes) word = std::byteswap(word);
        channels = {double(word & 1023u) / 1023, double((word >> 10) & 1023u) / 1023,
                    double((word >> 20) & 1023u) / 1023};
      } else throw std::runtime_error("unsupported transfer type in endpoint oracle");
      if (format == GL_BGRA) std::swap(channels[0],channels[2]);
      endpoint.pixels.push_back(channels);
    }
  }

  void write_le(std::uint8_t *p, std::uint32_t word) {
    for (int i = 0; i < 4; ++i) p[i] = (word >> (i * 8)) & 255u;
  }

  nlohmann::json exercise(std::uint32_t spa, int destination_stride, const char *name) {
    constexpr int width=2,height=2,source_stride=16,chunk_offset=5;
    const bool packed=spa==SPA_VIDEO_FORMAT_xBGR_210LE || spa==SPA_VIDEO_FORMAT_xRGB_210LE;
    const std::array<std::array<unsigned,3>,4> sample = packed ?
      std::array<std::array<unsigned,3>,4>{{{1023,341,682},{17,1000,89},{511,67,900},{123,456,789}}} :
      std::array<std::array<unsigned,3>,4>{{{251,79,23},{13,199,101},{67,41,211},{99,159,201}}};
    std::vector<std::uint8_t> source(chunk_offset + source_stride*height,0xe7);
    for (int i=0;i<4;++i) {
      auto *p=source.data()+chunk_offset+(i/2)*source_stride+(i%2)*4;
      auto [r,g,b]=sample[i];
      if (spa==SPA_VIDEO_FORMAT_xBGR_210LE) write_le(p,r|(g<<10)|(b<<20));
      else if (spa==SPA_VIDEO_FORMAT_xRGB_210LE) write_le(p,b|(g<<10)|(r<<20));
      else if (spa==SPA_VIDEO_FORMAT_RGBx) {p[0]=r;p[1]=g;p[2]=b;p[3]=255;}
      else {p[0]=b;p[1]=g;p[2]=r;p[3]=255;}
    }
    std::vector<std::uint8_t> destination(destination_stride*height,0xcd);
    const bool copied=pipewire_capture::copy_memptr_frame_to_bgra(source.data(),source.size(),chunk_offset,
      source_stride*(height-1)+width*4,width,height,source_stride,spa,destination.data(),destination_stride);
    if (!copied) throw std::runtime_error("production PipeWire copy rejected valid fixture");
    platf::img_t image;
    image.data=destination.data();image.width=width;image.height=height;
    image.pixel_pitch=4;image.row_pitch=destination_stride;
    image.frame_metadata=pipewire_capture::cpu_frame_metadata(spa);
    egl::sws_t converter {};
    converter.tex=gl::tex_t::make(1);
    endpoint={};endpoint.alignment=1;
    converter.load_ram(image);  // Exact production graphics.cpp used by va_ram_t::convert.
    double max_error=0;
    std::vector<rgb_t> expected;
    for (int i=0;i<4;++i) {
      rgb_t pixel={sample[i][0]/double(packed?1023:255),sample[i][1]/double(packed?1023:255),sample[i][2]/double(packed?1023:255)};
      expected.push_back(pixel);
      for (int c=0;c<3;++c) max_error=std::max(max_error,std::abs(pixel[c]-endpoint.pixels.at(i)[c]));
    }
    return {{"name",name},{"spa_format",spa},{"capture_metadata_format",platf::from_frame_format(image.frame_metadata.format)},
      {"copied",copied},{"source_stride",source_stride},{"destination_stride",destination_stride},
      {"upload_format",endpoint.format},{"upload_type",endpoint.type},{"actual_rgb",endpoint.pixels},
      {"expected_rgb",expected},{"maximum_channel_error",max_error},{"pass",max_error<0.000001}};
  }
}

int main() {
  // GL entry points are a strict call-site recorder, not a replacement RAM-upload helper.
  gl::ctx.GenTextures=gen_textures;gl::ctx.DeleteTextures=delete_textures;gl::ctx.BindTexture=bind_texture;
  gl::ctx.TexParameteri=texture_parameter_i;gl::ctx.TexParameterfv=texture_parameter_fv;
  gl::ctx.TexSubImage2D=upload;gl::ctx.PixelStorei=pixel_store;gl::ctx.GetIntegerv=get_integer;gl::ctx.GetError=no_error;
  nlohmann::json results=nlohmann::json::array();
  try {
    results.push_back(exercise(SPA_VIDEO_FORMAT_BGRx,8,"BGRx8_tight_control"));
    results.push_back(exercise(SPA_VIDEO_FORMAT_RGBx,8,"RGBx8_normalized_control"));
    results.push_back(exercise(SPA_VIDEO_FORMAT_xBGR_210LE,8,"xBGR210LE_asymmetric_words"));
    results.push_back(exercise(SPA_VIDEO_FORMAT_xRGB_210LE,8,"xRGB210LE_asymmetric_words"));
    results.push_back(exercise(SPA_VIDEO_FORMAT_BGRx,12,"BGRx8_padded_destination_rows"));
    results.push_back(exercise(SPA_VIDEO_FORMAT_xBGR_210LE,12,"xBGR210LE_padded_destination_rows"));
    results.push_back(exercise(SPA_VIDEO_FORMAT_xRGB_210LE,12,"xRGB210LE_padded_destination_rows"));
  } catch (const std::exception &error) {std::cerr<<error.what()<<'\n';return 2;}
  const auto passed=std::count_if(results.begin(),results.end(),[](const auto &r){return r.at("pass").template get<bool>();});
  std::cout<<nlohmann::json{{"fixture_only",true},{"production_upload_invoked",true},
    {"gpu_or_stream_created",false},{"tests",results.size()},{"passed",passed},
    {"failed",results.size()-passed},{"results",results}}.dump(2)<<'\n';
  return passed==results.size()?0:1;
}
