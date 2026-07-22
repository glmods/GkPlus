#include "Music.h"

#include "Core.h"
#include "DetourUtils.h"
#include "LuaEngine.h"

#include <cassert>
#include <cstddef>
#include <cstdint>

namespace gk {
namespace {
// Bink's volume scale runs 0..0x8000. The Audio menu's 0..9 steps are converted
// with these multipliers, so battle/menu/cinematic music tops out one step below
// full scale while CD audio uses the whole 16-bit range.
constexpr int BinkVolumePerStep = 0xe38;

MusicTrack **TheMusicTrack;
int *BattleMusicVolume;
} // namespace

// The 0x20-byte object built by MusicTrack_Ctor @ 0x00587b10. `device` is the
// DirectSound device captured at construction; when it is null every operation
// on the track early-outs, which is how the game copes with a machine that has
// no sound. `stop_event` doubles as PlayMusicThread's stop flag - StopMusicTrack
// stores an event HANDLE there and waits on it.
struct MusicTrack {
  int field0x00; // PlayMusicTrack sets 1 once BinkOpen succeeds; no reader found
  void *bink;    // BinkOpen handle; BinkClose zeroes it
  unsigned thread_id;
  void *thread;     // null while stopped
  void *stop_event; // non-null tells the streaming thread to exit
  void *device;
  int volume;
  bool looping;
  uint8_t pad[3];

  MusicTrack *HookedCtor();
};
static_assert(offsetof(MusicTrack, volume) == 0x18);
static_assert(offsetof(MusicTrack, looping) == 0x1c);
static_assert(sizeof(MusicTrack) == 0x20);

namespace {
// MusicTrack_Ctor is __fastcall with only `this` in ECX, so a member function
// pointer models it exactly and lets the hook assign the field by name.
MusicTrack *(MusicTrack::*MusicTrackCtor)() = nullptr;

using TSetVolume = ThisCall<char, MusicTrack *, int>;
TSetVolume SetVolume;

using TPlay = ThisCall<int, MusicTrack *, const char *, bool>;
TPlay Play;

using TStop = ThisCall<int, MusicTrack *>;
TStop Stop;
} // namespace

// The constructor hardcodes `volume` to full scale, and four of its callers -
// the front-end `track1.bik`, both `2a.bik` sites and `victory.bik` - play the
// track without ever setting a volume, so the saved Battle Music Volume was
// ignored until something else touched the track. Seed the field from the
// setting instead. The three callers that do set their own volume (cinematics,
// the Audio menu slider, battle music) overwrite it immediately and are
// unaffected. See threading_model_notes.md.
MusicTrack *MusicTrack::HookedCtor() {
  auto *track = (this->*MusicTrackCtor)();
  track->volume = *BattleMusicVolume * BinkVolumePerStep;
  return track;
}

// --- MusicTrackWrapper -------------------------------------------------------

bool MusicTrackWrapper::operator==(const MusicTrackWrapper &) const = default;

MusicTrackWrapper::MusicTrackWrapper(MusicTrack *track) : track(track) {
  assert(track);
}

void MusicTrackWrapper::setup_metatable(lua_State *L) {}

int MusicTrackWrapper::to_string(lua_State *L) const {
  lua_pushfstring(L, "<MusicTrack %p>", track);
  return 1;
}

int MusicTrackWrapper::get_volume() { return track->volume; }

void MusicTrackWrapper::set_volume(int volume) { SetVolume(track, volume); }

bool MusicTrackWrapper::get_looping() { return track->looping; }

bool MusicTrackWrapper::get_playing() { return track->thread != nullptr; }

int MusicTrackWrapper::play(lua_State *L) {
  auto path = Lua::check<const char *>(L, 2);
  bool loop = lua_toboolean(L, 3) != 0;

  lua_pushboolean(L, Play(track, path, loop) != 0);
  return 1;
}

int MusicTrackWrapper::stop(lua_State *L) {
  lua_pushboolean(L, Stop(track) != 0);
  return 1;
}

namespace {
struct Music {
  static constexpr const char *metatable_name = "Music";
  static void setup_metatable(lua_State *L) {}

  static std::optional<MusicTrackWrapper> get_current() {
    if (*TheMusicTrack) {
      return {*TheMusicTrack};
    } else {
      return std::nullopt;
    }
  }

  using fields =
      Lua::Fields<Lua::StaticGetter<"current", std::optional<MusicTrackWrapper>,
                                    &get_current>,
                  Lua::StaticSlot<"battle_volume", int, &BattleMusicVolume>>;
};
} // namespace

MusicModule::MusicModule(lua_State *L) : Module{L} {
  GetObjectAtOffset(TheMusicTrack, 0x007f5bdc);
  GetObjectAtOffset(BattleMusicVolume, 0x006abe08);
  GetObjectAtOffset(MusicTrackCtor, 0x00587b10);
  GetObjectAtOffset(SetVolume, 0x00587ca0);
  GetObjectAtOffset(Play, 0x00587b60);
  GetObjectAtOffset(Stop, 0x00587bf0);

  DetourAttach(&MusicTrackCtor, &MusicTrack::HookedCtor);
}

MusicModule::~MusicModule() {
  DetourDetach(&MusicTrackCtor, &MusicTrack::HookedCtor);
}

int MusicModule::Register(lua_State *L) {
  Lua::Create<Music>(L);
  return 1;
}
} // namespace gk
