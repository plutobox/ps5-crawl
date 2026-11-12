#include "AppHdr.h"

#include "sound.h"

#include "branch.h"
#include "files.h"
#include "libutil.h"
#include "options.h"
#include "player.h"
#include "state.h"
#include "unicode.h"

#ifdef USE_SOUND

#if defined(WINMM_PLAY_SOUNDS)
#    include <windows.h>
#    ifndef SND_ASYNC
#        define SND_ASYNC 0x0001
#    endif
#    ifndef SND_NODEFAULT
#        define SND_NODEFAULT 0x0002
#    endif
#elif defined(USE_SDL)
#    ifdef __ANDROID__
#        include <SDL_mixer.h>
#    else
#        include <SDL2/SDL_mixer.h>
#    endif
#    define SDL_MIXER_SOUND
// Each sound is decoded once and kept, so sounds can overlap; past this many,
// the ones not playing are let go.
static const size_t MAX_CACHED_SOUNDS = 64;
static map<string, Mix_Chunk*> sdl_sounds;

static Mix_Chunk *_sdl_sound(const string &file)
{
    auto it = sdl_sounds.find(file);
    if (it != sdl_sounds.end())
        return it->second;

    if (sdl_sounds.size() >= MAX_CACHED_SOUNDS)
    {
        set<Mix_Chunk*> playing;
        for (int i = 0; i < Mix_AllocateChannels(-1); i++)
            if (Mix_Playing(i))
                playing.insert(Mix_GetChunk(i));
        for (auto i = sdl_sounds.begin(); i != sdl_sounds.end();)
        {
            if (playing.count(i->second))
                ++i;
            else
            {
                if (i->second)
                    Mix_FreeChunk(i->second);
                i = sdl_sounds.erase(i);
            }
        }
    }

    // A file that won't load stays null, and isn't tried again.
    Mix_Chunk *chunk = Mix_LoadWAV(OUTS(file));
    if (!chunk)
        printf("Sound: can't load %s: %s\n", file.c_str(), Mix_GetError());
    sdl_sounds[file] = chunk;
    return chunk;
}

static map<string, vector<string>> music_tracks;
static string music_file;
static Mix_Music *music = nullptr;
static bool music_ducked = false;
#endif

// Plays a sound based on the given message; it must match a regex pattern
// in an option file for a sound to be played.
void parse_sound(const string& message)
{
    play_sound(check_sound_patterns(message));
}

// This function will return the sound_mapping it finds that matches
// the given string. If none is found, then a sound mapping with an empty
// string for the soundfile is returned.
//
// The rational for NOT playing sounds within this function is to enable
// conditional behaviour from the calling function, i.e. only do something
// when a sound mapping is found.
sound_mapping check_sound_patterns(const string& message)
{
    sound_mapping matched_sound;

    matched_sound.pattern = "";
    matched_sound.soundfile = "";
    matched_sound.interrupt_game = false;

    for (const sound_mapping &sound : Options.sound_mappings)
    {
        // Maybe we should allow message channel matching as for
        // force_more_message?
        if (sound.pattern.matches(message))
        {
            //play_sound(sound.soundfile.c_str(), sound.interrupt_game);
            matched_sound = sound;
            break;
        }
    }

    return matched_sound;
}

void play_sound(sound_mapping sound_data)
{
    if (!sound_data.soundfile.empty())
    {
        const string sf_path = catpath(Options.sound_file_path, sound_data.soundfile);
        play_sound(sf_path.c_str(), sound_data.interrupt_game);
    }
}

// TODO: Make interrupt_game apply to any sound-playing method, not just SOUND_PLAY_COMMAND
// TODO: Add in-game option for disabling interrupt sounds (and sounds in general)
void play_sound(const char *file, bool interrupt_game)
{
    UNUSED(interrupt_game);

    if (Options.sounds_on)
    {
#if defined(WINMM_PLAY_SOUNDS)
        // Check whether file exists, is readable, etc.?
        if (file && *file)
            sndPlaySoundW(OUTW(file), SND_ASYNC | SND_NODEFAULT);

#elif defined(SOUND_PLAY_COMMAND)
        char command[255];
        command[0] = 0;
        if (file && *file && (strlen(file) + strlen(SOUND_PLAY_COMMAND) < 255)
            && shell_safe(file))
        {
    #if defined(HOLD_SOUND_PLAY_COMMAND)
            if (interrupt_game)
                snprintf(command, sizeof command, HOLD_SOUND_PLAY_COMMAND, file);
            else
    #endif
                snprintf(command, sizeof command, SOUND_PLAY_COMMAND, file);

            system(OUTS(command));
        }
#elif defined(USE_SDL)
        static int last_channel = -1;

        if (!file || !*file)
            return;
        if (Options.one_SDL_sound_channel
             && last_channel != -1
             && Mix_Playing(last_channel))
        {
            Mix_HaltChannel(last_channel);
        }

        if (Mix_Chunk *chunk = _sdl_sound(file))
            last_channel = Mix_PlayChannel(-1, chunk, 0);
#endif
    } // End if (Options.sounds_on)
}

void clear_music()
{
#ifdef SDL_MIXER_SOUND
    music_tracks.clear();
#endif
}

void set_music(const string &key, const vector<string> &files)
{
#ifdef SDL_MIXER_SOUND
    music_tracks.emplace(key, files);
#else
    UNUSED(key, files);
#endif
}

#ifdef SDL_MIXER_SOUND
static void _stop_music()
{
    if (music)
    {
        Mix_HaltMusic();
        Mix_FreeMusic(music);
        music = nullptr;
    }
    music_file.clear();
}

// Music sits under the sound effects, and lower still at low hit points.
static int _music_volume()
{
    return MIX_MAX_VOLUME * (music_ducked ? 2 : 5) / 10;
}
#endif

void reload_music()
{
#ifdef SDL_MIXER_SOUND
    _stop_music();
#endif
    update_music();
}

void update_music()
{
#ifdef SDL_MIXER_SOUND
    if (!Options.sounds_on || !Options.music_on || music_tracks.empty())
    {
        _stop_music();
        return;
    }

    const bool in_game = crawl_state.need_save;
    const bool ducked = in_game && you.hp_max > 0
                        && you.hp * 100 < you.hp_max * 30;
    if (ducked != music_ducked)
    {
        music_ducked = ducked;
        Mix_VolumeMusic(_music_volume());
    }

    // A branch without a track of its own keeps what is playing.
    const string key = in_game ? branches[you.where_are_you].abbrevname
                               : "menu";
    const auto track = music_tracks.find(key);
    if (track == music_tracks.end())
        return;
    // A track not there yet (it may be downloaded later) is its first file.
    string file = track->second.front();
    for (const string &f : track->second)
        if (file_exists(f))
        {
            file = f;
            break;
        }
    if (file == music_file)
        return;

    _stop_music();
    // A track that won't load isn't tried again until the next one.
    music_file = file;
    music = Mix_LoadMUS(OUTS(music_file));
    if (!music)
    {
        printf("Music: can't load %s: %s\n", music_file.c_str(),
               Mix_GetError());
        return;
    }
    Mix_VolumeMusic(_music_volume());
    Mix_FadeInMusic(music, -1, 1500);
#endif
}

#endif // USE_SOUND
