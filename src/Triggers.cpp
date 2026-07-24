#include "Triggers.h"

#include "Core.h"

namespace gk {
TriggerList *CopyList(TriggerList *dst, TriggerList *src) {
  ThisCall<TriggerList *, TriggerList *, TriggerList *> fn;
  GetObjectAtOffset(fn, 0x0044c950);
  return fn(dst, src);
}

TriggerList *InitList(TriggerList *list) {
  ThisCall<TriggerList *, TriggerList *> fn;
  GetObjectAtOffset(fn, 0x0044ca10);
  return fn(list);
}

TriggerList *InitListWithActorName(TriggerList *list, const char **name) {
  ThisCall<TriggerList *, TriggerList *, const char **> fn;
  GetObjectAtOffset(fn, 0x0044c900);
  return fn(list, name);
}

ITrigger *CreateTrigger(TriggerList *list, const char **actor_name) {
  ThisCall<ITrigger *, TriggerList *, const char **> fn;
  GetObjectAtOffset(fn, 0x0044e8c0);
  return fn(list, actor_name);
}

void DeleteList(TriggerList *list) {
  ThisCall<void, TriggerList *> fn;
  GetObjectAtOffset(fn, 0x0044ce40);
  fn(list);
}

void RegisterTriggers(TriggerKind kind, Vec3 *coords, long long time_or_radius,
                      TriggerList targets, const unsigned char *script,
                      int team) {
  FastCall<void, TriggerKind, Vec3 *, long long, TriggerList,
           const unsigned char *, int>
      fn;
  GetObjectAtOffset(fn, 0x0043e240);
  fn(kind, coords, time_or_radius, targets, script, team);
}

TriggerData *RemoveTrigger(TriggerData *trigger, char c) {
  ThisCall<TriggerData *, TriggerData *, char> fn;
  GetObjectAtOffset(fn, 0x0050c400);
  return fn(trigger, c);
}
} // namespace gk
