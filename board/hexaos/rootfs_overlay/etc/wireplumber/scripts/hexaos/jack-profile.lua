-- hexaOS: speaker or headphone profile, by the headphone jack
--
-- Some sound cards (Intel SOF laptops with UCM) cannot play on the speakers and
-- the headphone jack at the same time, so they have two profiles, one with the
-- Speaker output and one with Headphones. WirePlumber picks the profile with the
-- highest priority among the available ones; once an HDMI screen is plugged in
-- both are available, it picks the headphone one and the speakers are gone.
--
-- Here: headphones plugged in -> best profile with Headphones, otherwise the
-- best profile with Speaker. Runs before the stored and the default choice, and
-- again whenever a jack changes (EnumRoute). Cards with one profile holding
-- both outputs, and other devices, are left to WirePlumber.

cutils = require ("common-utils")
log = Log.open_topic ("s-device")

-- Output route whose name has WORD ("[Out] Speaker", "[Out] Headphones")
local function output_route (device, word)
  for p in device:iterate_params ("EnumRoute") do
    local r = cutils.parseParam (p, "EnumRoute")
    if r and r.direction == "Output" and r.name and r.name:find (word, 1, true) then
      return r
    end
  end
  return nil
end

-- Speaker and Headphones routes, only when no profile has both
local function split_routes (device)
  if device.properties ["device.api"] ~= "alsa" then
    return nil
  end
  local spk = output_route (device, "Speaker")
  local hp = output_route (device, "Headphones")
  if not spk or not hp or not spk.profiles or not hp.profiles then
    return nil
  end
  for _, a in pairs (spk.profiles) do
    for _, b in pairs (hp.profiles) do
      if a == b then
        return nil
      end
    end
  end
  return spk, hp
end

-- Highest priority usable profile among the indexes of a route
local function best_profile (device, indexes)
  local wanted, best = {}, nil
  for _, v in pairs (indexes) do
    wanted [v] = true
  end
  for p in device:iterate_params ("EnumProfile") do
    local pr = cutils.parseParam (p, "EnumProfile")
    if pr and wanted [pr.index] and pr.available ~= "no" and
        pr.name ~= "off" and pr.name ~= "pro-audio" then
      if best == nil or pr.priority > best.priority then
        best = pr
      end
    end
  end
  return best
end

SimpleEventHook {
  name = "hexaos/find-jack-profile",
  before = "device/find-stored-profile",
  interests = {
    EventInterest {
      Constraint { "event.type", "=", "select-profile" },
    },
  },
  execute = function (event)
    if event:get_data ("selected-profile") then
      return
    end
    local device = event:get_subject ()
    local spk, hp = split_routes (device)
    if not spk then
      return
    end
    local route = (hp.available == "yes") and hp or spk
    local profile = best_profile (device, route.profiles)
    if profile then
      log:info (device, string.format ("hexaOS: %s -> profile '%s'",
          route.name, profile.name))
      event:set_data ("selected-profile", profile)
    end
  end
}:register ()

-- A jack changed (headphones in or out): choose the profile again
SimpleEventHook {
  name = "hexaos/jack-changed",
  interests = {
    EventInterest {
      Constraint { "event.type", "=", "device-params-changed" },
      Constraint { "event.subject.param-id", "=", "EnumRoute" },
    },
  },
  execute = function (event)
    local device = event:get_subject ()
    if split_routes (device) then
      event:get_source ():call ("push-event", "select-profile", device, nil)
    end
  end
}:register ()
