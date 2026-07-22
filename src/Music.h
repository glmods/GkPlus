#pragma once

#include "LuaEngine.h"
#include "Module.h"

#include <optional>

namespace gk {
struct MusicTrack;

// The Bink music-track object stored in TheMusicTrack @ 0x007f5bdc. Only one
// exists at a time - every call site overwrites the global with a freshly
// malloc'd 0x20 bytes. The full layout lives in Music.cpp; see
// threading_model_notes.md for the streaming thread and the volume scales.
struct MusicTrackWrapper final {
  static constexpr const char *metatable_name = "MusicTrack";
  static void setup_metatable(lua_State *L);

  MusicTrack *track;

  MusicTrackWrapper(MusicTrack *track);

  bool operator==(const MusicTrackWrapper &) const;

  int to_string(lua_State *L) const;

  // Bink scale, 0..0x8000. The setter goes through the game's own setter, so a
  // track that is already streaming picks the change up immediately.
  int get_volume();
  void set_volume(int volume);
  bool get_looping();
  bool get_playing();

  // play(path[, loop]) - path is relative to the Gunlok directory.
  int play(lua_State *L);
  int stop(lua_State *L);

  using type = MusicTrackWrapper;
  using fields = Lua::Fields<
      Lua::GetterSetter<"volume", &type::get_volume, &type::set_volume>,
      Lua::Getter<"looping", &type::get_looping>,
      Lua::Getter<"playing", &type::get_playing>,
      Lua::Function<"play", type, &type::play>,
      Lua::Function<"stop", type, &type::stop>>;
};

class MusicModule final : public Module<MusicModule> {
public:
  static constexpr const char *module_name = "gk.music";

  MusicModule(lua_State *L);
  ~MusicModule();

  int Register(lua_State *L);
};
} // namespace gk
