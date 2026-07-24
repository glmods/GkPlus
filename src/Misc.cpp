#include "Misc.h"
#include "Actors.h"
#include "Core.h"
#include "GLS.h"
#include "LuaEngine.h"

#include <array>
#include <cstddef>
#include <string_view>

namespace gk {
static std::array<std::string_view, 6> game_modes = {
    "single_player", "cooperative", "last_man_standing",
    "president",     "deathmatch",  "capture_the_flag",
};

static int *GameMode;

static int *GameState;

static Actor **ActorUnderCursor;

// The GLKeys "data" block: the 0x50-byte settings struct ReadGLKeys parses from
// Scripts\GLkeys.cfg and WinMain blits (five MOVUPS) onto the global block at
// 0x006abdd0. The on-disk order is scrambled and interleaves seven unrelated
// 0x007b9cxx globals; this is the in-memory layout. Full field-by-field notes,
// the on-disk order, and the persistence quirks are in menu_system_notes.md.
//
// The persisted block is exactly +0x00..+0x4f. IsAutoCrouchOn and BandwidthUse
// are contiguous in .data right after it but live *outside* the block, so they
// are saved nowhere and reset to their .data defaults every launch.
struct GLKeysSettings {
  int UnusedPrefToggle;       // 0x00  dead: menu 26 case 6 is unreachable
  int LinearMipmapOn;         // 0x04  video toggle
  int AnisotropicFilteringOn; // 0x08  video toggle (auto-cleared if ValidateDevice fails)
  int TripleBufferingOn;      // 0x0c  video toggle
  int Use32BitTextures;       // 0x10  video toggle
  int EnableRenderFlag0x400;  // 0x14  not persisted; WinMain hardcodes to 1
  int DepthStencilBits;       // 0x18  discarded on load (renegotiated per device)
  int VramTextureReduction;   // 0x1c  auto-computed from VRAM, not a user setting
  int TextureDetail;          // 0x20  video multi-value
  int ShadowQuality;          // 0x24  video multi-value (menu binds PendingShadowQuality)
  int ColourDepthIndex;       // 0x28  discarded on load (silently resets to 32-bit)
  int DynamicLightsOn;        // 0x2c  video toggle
  int ParticleFx;             // 0x30  video multi-value
  int CDMusicVolume;          // 0x34  menu 25 Audio, 0..9
  int BattleMusicVolume;      // 0x38  menu 25 Audio, 0..9
  int CinematicsVolume;       // 0x3c  menu 25 Audio, 0..9
  int SoundEffectsVolume;     // 0x40  menu 25 Audio, 0..9
  int AreHintsOn;             // 0x44  menu 26 Prefs, 0/1
  int IsFriendlyFireOn;       // 0x48  menu 26 Prefs, 0/1
  int AreFriendlyMinesOn;     // 0x4c  menu 26 Prefs, 0/1  <-- end of persisted 0x50 block
  int IsAutoCrouchOn;         // 0x50  menu 26 Prefs, 0/1; not persisted (past the block)
  int BandwidthUse;           // 0x54  menu 26 Prefs; MP net throttle 0..9, menu shows
                              //       9 - value; gates optional net updates and caps the
                              //       send backlog at 0x14 - value; not persisted
};
static_assert(sizeof(GLKeysSettings) == 0x58);
static_assert(offsetof(GLKeysSettings, CDMusicVolume) == 0x34);
static_assert(offsetof(GLKeysSettings, AreFriendlyMinesOn) == 0x4c);
static_assert(offsetof(GLKeysSettings, IsAutoCrouchOn) == 0x50);
static GLKeysSettings *Settings;

static int *BattleNumber;

static int *EPWEnabled;

static int *GameDifficulty;

static struct {
  int IsGodMode;
  int IsInfiniteAmmo;
} *Cheats;

static int *Foobar;

static std::array<std::string_view, 16> gls_section_names = {
    "unknown",     "shape",     "hierarchy",
    "pgenerator",  "light",     "projectile",
    "destructibility", "frag_data", "replace_destructibility",
    "role",        "character", "ammo",
    "ammo_info",   "camera_track", "map",
    "directory",
};

MiscModule::MiscModule(lua_State *L) : Module{L} {
  GetObjectAtOffset(GameMode, 0x007b9e28);
  GetObjectAtOffset(GameState, 0x006b02b4);
  GetObjectAtOffset(ActorUnderCursor, 0x007b68e8);
  GetObjectAtOffset(Settings, 0x006abdd0);
  GetObjectAtOffset(BattleNumber, 0x006a79b4);
  GetObjectAtOffset(EPWEnabled, 0x006a3001);
  GetObjectAtOffset(GameDifficulty, 0x007b9cc4);
  GetObjectAtOffset(Cheats, 0x007b9c70);
  GetObjectAtOffset(Foobar, 0x007b9df0);
}

struct State {
  static constexpr const char *metatable_name = "GameState";
  static void setup_metatable(lua_State *L) {}

  static std::optional<ActorWrapper> get_actor_under_cursor() {
    if (*ActorUnderCursor) {
      return {*ActorUnderCursor};
    } else {
      return std::nullopt;
    }
  }

  using fields = Lua::Fields<
      Lua::StaticSlot<"game_mode", int, &GameMode>,
      Lua::StaticSlot<"game_state", int, &GameState>,
      Lua::StaticSlot<"battle_number", int, &BattleNumber>,
      Lua::StaticSlot<"game_difficulty", int, &GameDifficulty>,
      Lua::StaticGetter<"actor_under_cursor", std::optional<ActorWrapper>,
                        &get_actor_under_cursor>,
      Lua::StaticSlot<"foobar", int, &Foobar>,
      Lua::StaticFunction<"parse_gls", [](lua_State *L) {
        auto file = Lua::to<const char *>(L, 1);
        auto convert = lua_toboolean(L, 2) != 0;
        auto *list = gls::LoadGLS(file);
        if (!list) {
          lua_pushnil(L);
          return 1;
        }
        lua_createtable(L, list->size(), 0);
        int index = 1;
        for (auto *thing : *list) {
          lua_createtable(L, 0, 3);
          auto type = thing ? thing->type() : gls::SectionType::Unknown;
          auto type_name = gls_section_names[static_cast<size_t>(type)];
          lua_pushlstring(L, type_name.data(), type_name.size());
          lua_setfield(L, -2, "type");
          if (thing && type != gls::SectionType::Unknown) {
            if (auto name = thing->get_string(gls::FieldId::Name)) {
              lua_pushlstring(L, name->data(), name->size());
              lua_setfield(L, -2, "name");
            }
            if (auto id = thing->get_string(gls::FieldId::Identifier)) {
              lua_pushlstring(L, id->data(), id->size());
              lua_setfield(L, -2, "identifier");
            }
          }
          lua_rawseti(L, -2, index++);
        }
        if (convert) {
          gls::ConvertParsedObjects(list);
        }
        gls::FreeParsedObjectList(list);
        return 1;
      }>>;
};

int MiscModule::Register(lua_State *L) {
  Lua::Create<State>(L);
  return 1;
}
} // namespace gk