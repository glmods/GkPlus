#include "Actors.h"

#include "Core.h"

namespace gk {
Actors *GetActorsTable() {
  Actors *table;
  GetObjectAtOffset(table, 0x007ba0d8);
  return table;
}

Actor *GetActorById(int id) {
  FastCall<Actor *, int> fn;
  GetObjectAtOffset(fn, 0x0044e0b0);
  return fn(id);
}
} // namespace gk
