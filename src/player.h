#ifndef TIT_PLAYER_H
#define TIT_PLAYER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct Player Player;

Player *player_create(bool null_audio, char *err, size_t errsize);
void player_destroy(Player *p);

int player_open(Player *p, const char *path, char *err, size_t errsize);
void player_close(Player *p);

int player_play(Player *p);
int player_pause(Player *p);
int player_stop(Player *p);
bool player_has_track(const Player *p);
bool player_is_playing(const Player *p);
bool player_is_paused(const Player *p);
bool player_take_ended(Player *p);

void player_seek_frames(Player *p, uint64_t frame);
void player_seek_ms(Player *p, int64_t delta_ms);

uint64_t player_pos_frames(const Player *p);
uint64_t player_len_frames(const Player *p);
int player_hz(const Player *p);
int player_channels(const Player *p);
int player_bitrate(const Player *p);

void player_set_volume(Player *p, float volume);
float player_volume(const Player *p);

#endif
