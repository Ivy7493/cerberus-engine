#include "eng/audio/Audio.h"

#include "eng/core/Log.h"

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <filesystem>

namespace eng {

Audio::Audio() {
    auto* engine = new ma_engine;
    if (ma_engine_init(nullptr, engine) != MA_SUCCESS) {
        ENG_LOG("[audio] no audio device - sound disabled");
        delete engine;
        return;
    }
    m_engine = engine;
}

Audio::~Audio() {
    for (auto& [name, clip] : m_clips)
        for (auto& v : clip.voices) ma_sound_uninit(v.get());
    if (m_music) ma_sound_uninit(m_music.get());
    if (m_engine) {
        ma_engine_uninit(m_engine);
        delete m_engine;
    }
}

void Audio::load(const std::string& name, const std::string& path, int voices) {
    if (!m_engine || !std::filesystem::exists(path)) return;
    Clip clip;
    for (int i = 0; i < voices; ++i) {
        auto sound = std::make_unique<ma_sound>();
        if (ma_sound_init_from_file(m_engine, path.c_str(), MA_SOUND_FLAG_DECODE, nullptr, nullptr,
                                    sound.get()) != MA_SUCCESS) {
            ENG_LOG("[audio] failed to load '" << path << "'");
            return;
        }
        clip.voices.push_back(std::move(sound));
    }
    m_clips[name] = std::move(clip);
}

void Audio::play(const std::string& name, float volume, float pitch) {
    auto it = m_clips.find(name);
    if (it == m_clips.end() || it->second.voices.empty()) return;
    Clip& clip = it->second;
    ma_sound* s = clip.voices[clip.next].get();
    clip.next = (clip.next + 1) % clip.voices.size();
    ma_sound_seek_to_pcm_frame(s, 0);
    ma_sound_set_volume(s, volume);
    ma_sound_set_pitch(s, pitch);
    ma_sound_start(s);
}

void Audio::playMusic(const std::string& path, float volume) {
    if (!m_engine) return;
    if (path == m_musicPath && m_music) {
        setMusicVolume(volume);
        return;
    }
    stopMusic();
    if (!std::filesystem::exists(path)) return;
    m_music = std::make_unique<ma_sound>();
    if (ma_sound_init_from_file(m_engine, path.c_str(), MA_SOUND_FLAG_STREAM, nullptr, nullptr,
                                m_music.get()) != MA_SUCCESS) {
        m_music.reset();
        return;
    }
    m_musicPath = path;
    ma_sound_set_looping(m_music.get(), MA_TRUE);
    ma_sound_set_volume(m_music.get(), volume);
    ma_sound_start(m_music.get());
}

void Audio::stopMusic() {
    m_musicPath.clear();
    if (!m_music) return;
    ma_sound_uninit(m_music.get());
    m_music.reset();
}

void Audio::setMusicVolume(float volume) {
    if (m_music) ma_sound_set_volume(m_music.get(), volume);
}

void Audio::setMasterVolume(float volume) {
    if (m_engine) ma_engine_set_volume(m_engine, volume);
}

}  // namespace eng
