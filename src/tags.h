// tags.h - a music file's title, artist and album (ID3)
#pragma once
#include <stdbool.h>

typedef struct { char title[160], artist[160], album[160]; } track_tags;

bool tags_read(const char *path, track_tags *t);
