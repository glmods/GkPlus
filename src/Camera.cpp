#include "Camera.h"

#include "Core.h"
#include "LuaEngine.h"
#include "Math.h"

namespace gk {

static Vec3 *CameraPosition;
static float *CameraDistance;
static float *MaxCameraDistance;

struct Camera {
  static constexpr const char *metatable_name = "Camera";
  static void setup_metatable(lua_State *L) {}

  Vec3 get_position() { return *CameraPosition; }
  void set_position(Vec3 pos) { *CameraPosition = pos; }

  float get_distance() { return *CameraDistance; }
  void set_distance(float dist) { *CameraDistance = dist; }

  float get_max_distance() { return *CameraDistance; }
  void set_max_distance(float dist) { *CameraDistance = dist; }

  using type = Camera;
  using fields = Lua::Fields<
      Lua::GetterSetter<"position", &type::get_position, &type::set_position>,
      Lua::GetterSetter<"distance", &type::get_distance, &type::set_distance>,
      Lua::GetterSetter<"max_distance", &type::get_max_distance,
                        &type::set_max_distance>>;
};

CameraModule::CameraModule(lua_State *L) : Module{L} {
  GetObjectAtOffset(CameraPosition, 0x007b4e0c);
  GetObjectAtOffset(CameraDistance, 0x007b3e78);
  GetObjectAtOffset(MaxCameraDistance, 0x006a5748);
}

int CameraModule::Register(lua_State *L) {
  Lua::Create<Camera>(L);
  return 1;
}

} // namespace gk