#ifndef BESTSPEECH_SPD_AUDIO_H
#define BESTSPEECH_SPD_AUDIO_H

/* Speech Dispatcher installs spd_module_main.h for out-of-tree modules, but
 * that header includes spd_audio.h, which Speech Dispatcher 0.12 does not
 * install. The audio plugin types it needs are all in this one. */
#include <speech-dispatcher/spd_audio_plugin.h>

#endif
