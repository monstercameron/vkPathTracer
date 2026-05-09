-- [user/ragdoll_demo] Phase 2 RAG04 demo helper: logs the joint count once
-- on load and otherwise stays out of the way. Ragdoll activation is set in
-- the scene JSON via `ragdoll: { active: true }`; physics drives the
-- collapse and writes joint world matrices into the scene's joint cache.

local script = {}

function script.on_enable(self, ctx)
  self._logged = false
end

function script.on_update(self, ctx)
  if self._logged then
    return
  end
  self._logged = true
  local entity = self
  if entity == nil then
    return
  end
  -- ctx.world.joint_count is not yet exposed; once the runtime exposes a
  -- ragdoll inspection API the script can log body_count + joint_count.
  -- For now just announce ourselves so the user sees the script ran.
  if entity.log ~= nil then
    entity:log("ragdoll_demo: hero spawned 5m above floor; gravity should collapse stick figure")
  end
end

return script
