-- [test/script_stack_overflow_probe] Test fixture: unbounded recursion in on_update;
-- Lua should raise a stack-overflow error that the runtime catches via lua_resume.
local function r() return r() end
function on_update(self) r() end
local script = {}
function script.on_update(self) r() end
return script
