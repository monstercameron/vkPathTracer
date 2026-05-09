-- [test/script_wall_clock_probe] Test fixture: tight infinite loop that, when
-- paired with a large instruction_budget, trips the 50 ms wall-clock deadline
-- in LuaInstructionBudgetHook before the per-script instruction cap is reached.
local script = {}

function script.on_update()
  while true do
  end
end

return script
