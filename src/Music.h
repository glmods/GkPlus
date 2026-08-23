#pragma once

#include <cstddef>
#include <cstdint>

namespace gk {
// The 0x20-byte Bink music-track object stored in TheMusicTrack @ 0x007f5bdc.
// Only one exists at a time - every call site overwrites the global with a
// freshly malloc'd 0x20 bytes. `device` is the DirectSound device captured at
// construction; when it is null every operation on the track early-outs, which
// is how the game copes with a machine that has no sound. `stop_event` doubles
// as PlayMusicThread's stop flag. See threading_model_notes.md.
struct MusicTrack {
  int field0x00; // PlayMusicTrack sets 1 once BinkOpen succeeds; no reader found
  void *bink;    // BinkOpen handle; BinkClose zeroes it
  unsigned thread_id;
  void *thread;     // null while stopped
  void *stop_event; // non-null tells the streaming thread to exit
  void *device;
  int volume;    // 0x18 Bink scale, 0..0x8000
  bool looping;  // 0x1c
  uint8_t pad[3];

  // Volume-fix hook entry, installed by MusicSystem (see Music.cpp).
  // MusicTrack_Ctor is __fastcall with only `this` in ECX, so a member function
  // pointer models it exactly and lets the hook assign the field by name.
  MusicTrack *HookedCtor();
};
static_assert(offsetof(MusicTrack, volume) == 0x18);
static_assert(offsetof(MusicTrack, looping) == 0x1c);
static_assert(sizeof(MusicTrack) == 0x20);

// --- Native API --------------------------------------------------------------

// TheMusicTrack @ 0x007f5bdc; null when no track exists.
MusicTrack *GetCurrentMusicTrack();

// Battle Music Volume setting (0..9) @ 0x006abe08.
int GetBattleMusicVolume();
/// Sets the Battle Music Volume setting. \p volume is the front end's 0..9
/// scale, not the Bink 0..0x8000 one SetMusicVolume() takes.
void SetBattleMusicVolume(int volume);

// SetVolume @ 0x00587ca0 (Bink scale 0..0x8000); a streaming track picks the
// change up immediately.
char SetMusicVolume(MusicTrack *track, int volume);
// Play @ 0x00587b60; path is relative to the Gunlok directory.
int PlayMusic(MusicTrack *track, const char *path, bool loop);
// Stop @ 0x00587bf0.
int StopMusic(MusicTrack *track);

// Installs the MusicTrack-constructor volume fix (see Music.cpp). RAII: attaches
// in the ctor, detaches in the dtor - construct/destroy inside a Detours
// transaction.
class MusicSystem {
public:
  MusicSystem();
  ~MusicSystem();
};
} // namespace gk
