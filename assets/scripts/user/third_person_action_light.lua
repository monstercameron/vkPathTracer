-- [user/third_person_action_light] No-op light script for the third-person action demo (keeps lighting static).
local script = {}

function script.on_update(self, ctx)
  -- Keep game mode cheap: authored lighting is static, so this script does
  -- not emit per-frame light commands that would require renderer light rebuilds.
  return
end

return script
