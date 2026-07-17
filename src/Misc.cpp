#include "Misc.h"
#include "Actors.h"
#include "Core.h"
#include "GLS.h"
#include "LuaEngine.h"

#include <array>
#include <string_view>

namespace gk {
static std::array<std::string_view, 6> game_modes = {
    "single_player", "cooperative", "last_man_standing",
    "president",     "deathmatch",  "capture_the_flag",
};

static int *GameMode;

static int *GameState;

static Actor **ActorUnderCursor;

static struct {
  int CDMusicVolume;
  int BattleMusicVolume;
  int CinematicsVolume;
  int SoundEffectsVolume;
  int AreHintsOn;
  int IsFriendlyFireOn;
  int AreFriendlyMinesOn;
  int IsAutoCrouchOn;
} *Settings;

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
  GetObjectAtOffset(Settings, 0x006abe04);
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
        lua_createtable(L, list->count, 0);
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