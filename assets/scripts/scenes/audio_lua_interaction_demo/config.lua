return {
  player = "Lua Audio Player",
  player_script = "assets/scripts/audio_interaction_demo.lua",
  interactables = {
    pickup = {
      entity = "Pickup Audio Marker",
      sound = "pickup.collect",
      prompt = "E  Pickup",
      radius = 1.8,
    },
    terminal = {
      entity = "Terminal Audio Marker",
      sound = "objective.terminal.disabled",
      prompt = "E  Terminal",
      radius = 2.0,
    },
    ambience = {
      entity = "Ambient Forest Emitter",
      sound = "ambience.forest",
      prompt = "E  Listen",
      radius = 3.0,
    },
  },
}
