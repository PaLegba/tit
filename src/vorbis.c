/* stb_vorbis lives in its own translation unit: its static helpers collide
   with minimp3's inside player.c. */
/* stb_vorbis needs an alloca declaration on macOS; BSD stdlib.h has it. */
#ifdef __APPLE__
#include <alloca.h>
#endif
#include "stb_vorbis.c"
