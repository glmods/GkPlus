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

void AddTriggerToGlobalList(TriggerKind kind, Vec3 *coords,
                            long long time_or_radius, TriggerList targets,
                            const unsigned char *script, int team) {
  FastCall<void, TriggerKind, Vec3 *, long long, TriggerList,
           const unsigned char *, int>
      fn;
  GetObjectAtOffset(fn, 0x0043e240);
  fn(kind, coords, time_or_radius, targets, script, team);
}

TriggerData *RemoveTriggerFromGlobalList(TriggerData *trigger, char free_flag) {
  ThisCall<TriggerData *, TriggerData *, char> fn;
  GetObjectAtOffset(fn, 0x0050c400);
  return fn(trigger, free_flag);
}

TriggerListHead *GetTriggerList() {
  TriggerListHead *head = nullptr;
  GetObjectAtOffset(head, 0x006af858);
  return head;
}

TriggerData *LastRegisteredTrigger() {
  TriggerListHead *head = GetTriggerList();
  if (!head || !head->sentinel) {
    return nullptr;
  }
  // The insert is at the tail, so `prev` of the sentinel is the newest node.
  // Empty list: prev points back at the sentinel, which is a bare
  // List_Member_Base carrying no `data` - reading one off it is the heap
  // over-read src/List.h exists to prevent, hence the explicit compare.
  TriggerNodeBase *tail = head->sentinel->prev;
  if (!tail || tail == head->sentinel) {
    return nullptr;
  }
  return entry_of(tail)->data;
}

bool TriggerIsRegistered(const TriggerData *trigger) {
  TriggerListHead *head = GetTriggerList();
  if (!trigger || !head || !head->sentinel) {
    return false;
  }
  // Bounded by `count` as well as by the sentinel: this walks a list the
  // executor can relink, and an unbounded loop over a torn `next` chain would
  // hang the calling thread rather than report a miss. Callers take an
  // ExecutorPause, which is what makes the walk itself sound; the bound is the
  // backstop for the case where they did not.
  const TriggerNodeBase *sentinel = head->sentinel;
  int budget = head->count + 1;
  for (TriggerNodeBase *node = sentinel->next;
       node && node != sentinel && budget-- > 0; node = node->next) {
    if (entry_of(node)->data == trigger) {
      return true;
    }
  }
  return false;
}
} // namespace gk
