local script = {}

function script.on_update(self, ctx)
  -- Keep playable mode cheap: authored lighting is static, so this script does
  -- not emit per-frame light commands that would require renderer light rebuilds.
  return
end

return script
