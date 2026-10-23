#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct ma_engine;
struct ma_sound;

namespace eng {

// Thin miniaudio wrapper: named one-shot sound effects plus one streamed
// music track. Every call is a silent no-op if the audio device failed to
// open (headless box, no output device) or a clip is missing, so a game
// never has to guard its play() calls.
class Audio {
public:
    Audio();
    ~Audio();

    Audio(const Audio&) = delete;
    Audio& operator=(const Audio&) = delete;

    bool available() const { return m_engine != nullptr; }

    // Decodes `path` up front and keeps `voices` copies so the same clip
    // can overlap itself (rapid-fire shots) without cutting off.
    void load(const std::string& name, const std::string& path, int voices = 4);
    void play(const std::string& name, float volume = 1.0f, float pitch = 1.0f);

    // Streams and loops `path`; replaying the track already playing is a no-op.
    void playMusic(const std::string& path, float volume = 0.5f);
    void stopMusic();
    void setMusicVolume(float volume);

    void setMasterVolume(float volume);

private:
    struct Clip {
        std::vector<std::unique_ptr<ma_sound>> voices;
        size_t next = 0;
    };

    ma_engine* m_engine = nullptr;
    std::unordered_map<std::string, Clip> m_clips;
    std::unique_ptr<ma_sound> m_music;
    std::string m_musicPath;
};

}  // namespace eng
