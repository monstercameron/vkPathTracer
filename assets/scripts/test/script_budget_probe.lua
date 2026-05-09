-- [test/script_budget_probe] Test fixture: infinite loop in on_update that should trip the instruction budget enforcement.
local script = {}

function script.on_update()
  while true do
  end
end

return script
