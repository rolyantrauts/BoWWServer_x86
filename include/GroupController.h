#pragma once
#include "BoWWServerDefs.h"
#include "ClientSession.h"
#include "SimpleAGC.h"
#include "WavWriter.h"
#include "AlsaSinkManager.h"
#include "feature_extract.h"
#include "tflite_runner.h"
#include <memory>
#include <vector>
#include <map>
#include <mutex>
#include <chrono>
#include <nlohmann/json.hpp>
#include <string>

namespace boww {

    class WindowAverager {
    private:
        std::vector<float> window_;
        int head_;
        float current_sum_;

    public:
        WindowAverager(int size) : window_(size, 0.0f), head_(0), current_sum_(0.0f) {}

        void process(float new_prob, float& out_smoothed_prob) {
            current_sum_ -= window_[head_];
            window_[head_] = new_prob;
            current_sum_ += new_prob;
            head_ = (head_ + 1) % window_.size();
            out_smoothed_prob = current_sum_ / static_cast<float>(window_.size());
        }

        void reset(float val = 0.0f) {
            std::fill(window_.begin(), window_.end(), val);
            current_sum_ = val * window_.size();
        }
    };

    enum class GroupState { IDLE, ARBITRATING, STREAMING };

    class GroupController {
    public:
        GroupController(GroupConfig config, std::string model_path, ServerConfig server_config, AlsaSinkManager& alsa_manager, bool debug_mode = false);
        ~GroupController(); 

        void HandleConfidenceScore(std::shared_ptr<ClientSession> session, float score, int frame_count); 
        void HandleAudioStream(std::shared_ptr<ClientSession> session, std::vector<int16_t>& pcm_data); 
        void OnTick();
        
        GroupConfig GetConfig() const { return config_; }

    private:
        GroupConfig config_;
        ServerConfig server_config_;
        TFLiteRunner tflite_runner_; 
        FeatureExtractor feature_extractor_;
        AlsaSinkManager& alsa_manager_;
        bool debug_mode_;
        std::mutex mutex_;
        
        WavWriter wav_writer_;
        SimpleAGC agc_engine_;

        std::vector<float> sliding_window_;
        std::vector<float> clean_window_;
        std::vector<float> current_mfccs_;
        WindowAverager vad_averager_;

        GroupState state_ = GroupState::IDLE;
        std::chrono::steady_clock::time_point arbitration_start_time_;
        
        std::shared_ptr<ClientSession> best_candidate_ = nullptr;
        float best_score_ = 0.0f;
        int best_frame_count_ = 0; 
        std::string active_client_guid_ = "";
        
        std::vector<std::shared_ptr<ClientSession>> current_candidates_;
        std::map<std::string, std::vector<int16_t>> arbitration_buffers_;

        bool is_live_streaming_ = false;
        bool is_recording_to_file_ = false; 
        
        std::string current_base_filename_;
        nlohmann::json current_metadata_;

        void ProcessVADFrames(const std::vector<int16_t>& pcm_data, bool update_session);
        
        // --- NEW: Accept client frame count for the 'ratio' calculation ---
        bool ValidateAuthoritativeWakeword(const std::vector<int16_t>& pcm_buffer, int client_frame_count);
    };
}
