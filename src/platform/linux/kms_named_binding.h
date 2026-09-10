/** @file src/platform/linux/kms_named_binding.h
 * Fresh DRM observations for a named capture's connector and unique plane.
 */
#pragma once
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <xf86drmMode.h>

namespace platf::kms_selection {
  struct binding_t {
    std::string connector_name;
    uint32_t connector_id;
    uint32_t crtc_id;
    uint32_t plane_id;
  };

  struct native_drm_api_t {
    auto connector(int fd, uint32_t id) const {
      return std::unique_ptr<drmModeConnector, decltype(&drmModeFreeConnector)>(drmModeGetConnectorCurrent(fd, id), drmModeFreeConnector);
    }
    auto encoder(int fd, uint32_t id) const {
      return std::unique_ptr<drmModeEncoder, decltype(&drmModeFreeEncoder)>(drmModeGetEncoder(fd, id), drmModeFreeEncoder);
    }
    auto planes(int fd) const {
      return std::unique_ptr<drmModePlaneRes, decltype(&drmModeFreePlaneResources)>(drmModeGetPlaneResources(fd), drmModeFreePlaneResources);
    }
    auto plane(int fd, uint32_t id) const {
      return std::unique_ptr<drmModePlane, decltype(&drmModeFreePlane)>(drmModeGetPlane(fd, id), drmModeFreePlane);
    }
    std::optional<uint64_t> plane_type(int fd, uint32_t id) const {
      const auto properties = std::unique_ptr<drmModeObjectProperties, decltype(&drmModeFreeObjectProperties)>(
        drmModeObjectGetProperties(fd, id, DRM_MODE_OBJECT_PLANE), drmModeFreeObjectProperties);
      if (!properties || properties->count_props > 4096) return std::nullopt;
      for (uint32_t i = 0; i < properties->count_props; ++i) {
        const auto property = std::unique_ptr<drmModePropertyRes, decltype(&drmModeFreeProperty)>(
          drmModeGetProperty(fd, properties->props[i]), drmModeFreeProperty);
        if (!property) return std::nullopt;
        if (std::string_view(property->name) == "type") return properties->prop_values[i];
      }
      return std::nullopt;
    }
  };

  template<class Api>
  bool binding_matches(Api &api, int fd, const binding_t &binding) {
    const auto connector = api.connector(fd, binding.connector_id);
    if (!connector || connector->connection != DRM_MODE_CONNECTED || !connector->encoder_id ||
        connector->connector_id != binding.connector_id) return false;
    const auto *type = drmModeGetConnectorTypeName(connector->connector_type);
    if (!type || std::format("{}-{}", type, connector->connector_type_id) != binding.connector_name) return false;
    const auto encoder = api.encoder(fd, connector->encoder_id);
    if (!encoder || !binding.crtc_id || encoder->crtc_id != binding.crtc_id) return false;
    const auto planes = api.planes(fd);
    if (!planes || !planes->count_planes || planes->count_planes > 4096) return false;
    bool found = false;
    for (uint32_t i = 0; i < planes->count_planes; ++i) {
      const auto plane = api.plane(fd, planes->planes[i]);
      if (!plane) return false;
      if (!plane->fb_id || plane->crtc_id != binding.crtc_id) continue;
      const auto type = api.plane_type(fd, plane->plane_id);
      if (!type) return false;
      if (*type == DRM_PLANE_TYPE_CURSOR) continue;
      if (found || plane->plane_id != binding.plane_id) return false;
      found = true;
    }
    return found;
  }
}
