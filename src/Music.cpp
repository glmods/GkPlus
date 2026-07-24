#include "Music.h"

#include "Core.h"
#include "DetourUtils.h"

namespace gk {
namespace {
// Bink's volume scale runs 0..0x8000. The Audio menu's 0..9 steps are converted
// with these multipliers, so battle/menu/cinematic music tops out one step below
// full scale while CD audio uses the whole 16-bit range.
constexpr int BinkVolumePerStep = 0xe38;

int *BattleMusicVolume;

// MusicTrack_Ctor is __fastcall with only `this` in ECX, so a member function
// pointer models it exactly and lets the hook assign the field by name.
MusicTrack *(MusicTrack::*MusicTrackCtor)() = nullptr;
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

MusicTrack *GetCurrentMusicTrack() {
  MusicTrack **the_track;
  GetObjectAtOffset(the_track, 0x007f5bdc);
  return *the_track;
}

int GetBattleMusicVolume() {
  int *v;
  GetObjectAtOffset(v, 0x006abe08);
  return *v;
}

void SetBattleMusicVolume(int volume) {
  int *v;
  GetObjectAtOffset(v, 0x006abe08);
  *v = volume;
}

char SetMusicVolume(MusicTrack *track, int volume) {
  ThisCall<char, MusicTrack *, int> fn;
  GetObjectAtOffset(fn, 0x00587ca0);
  return fn(track, volume);
}

int PlayMusic(MusicTrack *track, const char *path, bool loop) {
  ThisCall<int, MusicTrack *, const char *, bool> fn;
  GetObjectAtOffset(fn, 0x00587b60);
  return fn(track, path, loop);
}

int StopMusic(MusicTrack *track) {
  ThisCall<int, MusicTrack *> fn;
  GetObjectAtOffset(fn, 0x00587bf0);
  return fn(track);
}

MusicSystem::MusicSystem() {
  GetObjectAtOffset(BattleMusicVolume, 0x006abe08);
  GetObjectAtOffset(MusicTrackCtor, 0x00587b10);
  DetourAttach(&MusicTrackCtor, &MusicTrack::HookedCtor);
}

MusicSystem::~MusicSystem() {
  DetourDetach(&MusicTrackCtor, &MusicTrack::HookedCtor);
}
} // namespace gk
