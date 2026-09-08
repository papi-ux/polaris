-- Route private PCM streams to the sink captured by the audio provider before
-- it admits any launcher. Names alone cannot authorize a replacement node.
local lutils = require ("linking-utils")
local sink = os.getenv ("POLARIS_AUDIO_SINK")
local serial = os.getenv ("POLARIS_AUDIO_NODE_SERIAL")
assert (sink and #sink > 0 and #sink <= 128 and
    sink:match ("^[A-Za-z0-9][A-Za-z0-9_.%-]*$"), "invalid allocated sink")
assert (serial and #serial <= 20 and serial:match ("^[1-9][0-9]*$"),
    "invalid allocated sink serial")

SimpleEventHook {
  name = "linking/polaris-allocated-target",
  before = "linking/prepare-link",
  interests = {
    EventInterest { Constraint { "event.type", "=", "select-target" } },
  },
  execute = function (event)
    local _, om, si, props, flags = lutils:unwrap_select_target_event (event)
    event:set_data ("target", nil)
    flags.has_defined_target = false
    flags.has_node_defined_target = false
    flags.can_passthrough = false
    local class = props ["media.class"]
    if class ~= "Stream/Output/Audio" and class ~= "Stream/Input/Audio" then
      event:stop_processing ()
      return
    end

    local selected, count = nil, 0
    for candidate in om:iterate { type = "SiLinkable" } do
      local target = candidate.properties
      if target ["media.class"] == "Audio/Sink" and
          target ["node.name"] == sink then
        count = count + 1
        if tostring (target ["object.serial"]) == serial then
          selected = candidate
        end
      end
    end
    local requested = props ["target.object"]
    if count == 1 and selected and (requested == sink or requested == serial) and
        lutils.canLink (props, selected) then
      flags.has_defined_target = true
      flags.has_node_defined_target = true
      event:set_data ("target", selected)
      return
    end

    -- No fallback, metadata-driven movement, or arbitrary device selection.
    local node = si:get_associated_proxy ("node")
    lutils.sendClientError (event, node, -2, "allocated audio target unavailable")
    node:request_destroy ()
    event:stop_processing ()
  end,
}:register ()
