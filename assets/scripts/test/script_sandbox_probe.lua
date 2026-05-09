-- [test/script_sandbox_probe] Test fixture: invokes disallowed `require` to verify sandbox rejection.
local script = {}

function script.on_update()
  return require("not_allowed")
end

return script
