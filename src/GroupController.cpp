#include "GroupController.h"
#include <iostream>
#include <chrono>
#include <cmath>
#include <ctime>
#include <sstream>

namespace boww {

    GroupController::GroupController(GroupConfig config, std::string model_path, ServerConfig server_config, AlsaSinkManager& alsa_manager, bool debug_mode)
        : config_(std::move(config)), server_config_(server_config), tflite_runner_(model_path), alsa_manager_(alsa_manager), debug_mode_(debug_mode),
          sliding_window_(640, 0.0f), clean_window_(640, 0.0f), current_mfccs_(20, 0.0f), vad_averager_(15) 
    {
        std::cout << "[Group: " << config_.name << "] Initialized with isolated TFLite instance.\n";
    }

    GroupController::~GroupController() {
        if (state_ == GroupState::STREAMING) {
            if (is_recording_to_file_) wav_writer_.CloseAndPublish();
            if (is_live_streaming_) alsa_manager_.CloseLiveStream(config_.output_target);
        }
    }

    void GroupController::HandleConfidenceScore(std::shared_ptr<ClientSession> client, float score, int frame_count) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (state_ == GroupState::IDLE) {
            state_ = GroupState::ARBITRATING;
            arbitration_start_time_ = std::chrono::steady_clock::now();
            best_candidate_ = client;
            best_score_ = score;
            best_frame_count_ = frame_count;
            
            current_candidates_.clear();
            arbitration_buffers_.clear();
            current_candidates_.push_back(client);
            
            std::cout << "[Group: " << config_.name << "] Candidate: " << client->GetID() 
                      << " Score: " << score << " (Frames: " << frame_count << ")\n";
            std::cout << "[Group: " << config_.name << "] Arbitration started.\n";
        } else if (state_ == GroupState::ARBITRATING) {
            current_candidates_.push_back(client);
            if (score > best_score_) {
                best_candidate_ = client;
                best_score_ = score;
                best_frame_count_ = frame_count;
            }
        }
    }

    void GroupController::ProcessVADFrames(const std::vector<int16_t>& pcm_data, bool update_session) {
        const int HOP_STEP = 320;
        const int TF_FRAME_LENGTH = 640;

        for (size_t i = 0; i < pcm_data.size(); i += HOP_STEP) {
            if (i + HOP_STEP > pcm_data.size()) break;

            std::vector<float> hop_in(HOP_STEP);
            for (int j = 0; j < HOP_STEP; ++j) hop_in[j] = static_cast<float>(pcm_data[i + j]) / 32768.0f;

            std::memmove(sliding_window_.data(), sliding_window_.data() + HOP_STEP, (TF_FRAME_LENGTH - HOP_STEP) * sizeof(float));
            std::memcpy(sliding_window_.data() + (TF_FRAME_LENGTH - HOP_STEP), hop_in.data(), HOP_STEP * sizeof(float));

            float sum = 0.0f;
            for (float f : sliding_window_) sum += f;
            float mean = sum / TF_FRAME_LENGTH;
            for (int j = 0; j < TF_FRAME_LENGTH; ++j) clean_window_[j] = sliding_window_[j] - mean;

            feature_extractor_.compute_mfcc_features(clean_window_, current_mfccs_);
            std::vector<float> scores = tflite_runner_.infer(current_mfccs_);

            if (scores.size() >= 3) {
                float inv_vad = 1.0f - scores[2]; 
                float smoothed_vad = 0.0f;
                vad_averager_.process(inv_vad, smoothed_vad);

                if (update_session && best_candidate_ && smoothed_vad > config_.vad_threshold) {
                    best_candidate_->UpdateLastVoiceTime();
                }

                static int print_throttle = 0;
                if (debug_mode_ && update_session) {
                    if (++print_throttle % 5 == 0) {
                        int bars = static_cast<int>(smoothed_vad * 40.0f);
                        std::string bar_str(std::min(bars, 40), '#');
                        bar_str.append(40 - std::min(bars, 40), '-');
                        std::cout << "\r[VAD: " << bar_str << "] " << smoothed_vad << "   " << std::flush;
                    }
                }
            }
        }
    }
    
    // --- NEW: Supports "ratio", "leading", and "average" modes ---
    bool GroupController::ValidateAuthoritativeWakeword(const std::vector<int16_t>& pcm_buffer, int client_frame_count) {
        if (!config_.auth_ww.enabled) return true; 
        
        std::cout << "[Group: " << config_.name << "] Verifying pre-roll buffer (Mode: " << config_.auth_ww.type << ")...\n";
        
        const int HOP_STEP = 320;
        const int TF_FRAME_LENGTH = 640;
        
        std::vector<float> test_sliding_window(640, 0.0f);
        std::vector<float> test_clean_window(640, 0.0f);
        std::vector<float> test_mfccs(20, 0.0f);
        
        tflite_runner_.reset_states();
        
        // --- 1. pure 'Ratio' Mode ---
        if (config_.auth_ww.type == "ratio") {
            int above_threshold_count = 0;
            
            for (size_t i = 0; i < pcm_buffer.size(); i += HOP_STEP) {
                if (i + HOP_STEP > pcm_buffer.size()) break;

                std::vector<float> hop_in(HOP_STEP);
                for (int j = 0; j < HOP_STEP; ++j) hop_in[j] = static_cast<float>(pcm_buffer[i + j]) / 32768.0f;

                std::memmove(test_sliding_window.data(), test_sliding_window.data() + HOP_STEP, (TF_FRAME_LENGTH - HOP_STEP) * sizeof(float));
                std::memcpy(test_sliding_window.data() + (TF_FRAME_LENGTH - HOP_STEP), hop_in.data(), HOP_STEP * sizeof(float));

                float sum = 0.0f;
                for (float f : test_sliding_window) sum += f;
                float mean = sum / TF_FRAME_LENGTH;
                for (int j = 0; j < TF_FRAME_LENGTH; ++j) test_clean_window[j] = test_sliding_window[j] - mean;

                feature_extractor_.compute_mfcc_features(test_clean_window, test_mfccs);
                std::vector<float> scores = tflite_runner_.infer(test_mfccs);

                float raw_prob = scores.empty() ? 0.0f : scores[0]; 
                
                if (raw_prob >= config_.auth_ww.threshold) {
                    above_threshold_count++;
                }
            }
            
            tflite_runner_.reset_states();
            
            float calc_ratio = (client_frame_count > 0) ? (static_cast<float>(above_threshold_count) / client_frame_count) : 0.0f;
            
            if (debug_mode_) {
                std::cout << "[Group: " << config_.name << "] Ratio Check: " << above_threshold_count 
                          << " passing frames / " << client_frame_count << " client frames = " 
                          << calc_ratio << " (Target: " << config_.auth_ww.ratio << ")\n";
            }
            
            return (calc_ratio >= config_.auth_ww.ratio);
        }
        
        // --- 2. ADSR Envelopes ("leading" or "average") ---
        WindowAverager test_averager(config_.auth_ww.hold);
        
        int attack_counter = 0;
        float smoothed_peak = 0.0f;
        int current_frame_count = 0;
        bool was_armed = false;
        bool passed_validation = false;

        for (size_t i = 0; i < pcm_buffer.size(); i += HOP_STEP) {
            if (i + HOP_STEP > pcm_buffer.size()) break;

            std::vector<float> hop_in(HOP_STEP);
            for (int j = 0; j < HOP_STEP; ++j) hop_in[j] = static_cast<float>(pcm_buffer[i + j]) / 32768.0f;

            std::memmove(test_sliding_window.data(), test_sliding_window.data() + HOP_STEP, (TF_FRAME_LENGTH - HOP_STEP) * sizeof(float));
            std::memcpy(test_sliding_window.data() + (TF_FRAME_LENGTH - HOP_STEP), hop_in.data(), HOP_STEP * sizeof(float));

            float sum = 0.0f;
            for (float f : test_sliding_window) sum += f;
            float mean = sum / TF_FRAME_LENGTH;
            for (int j = 0; j < TF_FRAME_LENGTH; ++j) test_clean_window[j] = test_sliding_window[j] - mean;

            feature_extractor_.compute_mfcc_features(test_clean_window, test_mfccs);
            std::vector<float> scores = tflite_runner_.infer(test_mfccs);

            float raw_prob = scores.empty() ? 0.0f : scores[0]; 
            float smoothed_prob = 0.0f;
            test_averager.process(raw_prob, smoothed_prob);

            if (!was_armed) {
                if (raw_prob >= config_.auth_ww.threshold) {
                    attack_counter++;
                    if (attack_counter >= config_.auth_ww.attack) {
                        was_armed = true;
                        smoothed_peak = smoothed_prob;
                        current_frame_count = attack_counter;
                    }
                } else {
                    attack_counter = 0;
                }
            } 
            else { // ARMED
                current_frame_count++;
                if (smoothed_prob > smoothed_peak) smoothed_peak = smoothed_prob;
                
                bool should_release = false;
                
                if (config_.auth_ww.type == "leading") {
                    float release_threshold = std::max(0.05f, smoothed_peak - config_.auth_ww.decay);
                    if (smoothed_prob <= release_threshold) should_release = true;
                } else if (config_.auth_ww.type == "average") {
                    if (smoothed_peak >= config_.auth_ww.threshold) {
                        if (smoothed_prob < (config_.auth_ww.threshold - config_.auth_ww.decay)) should_release = true;
                    } else if (current_frame_count >= config_.auth_ww.hold) {
                        should_release = true; 
                    }
                }

                if (should_release) {
                    if (config_.auth_ww.type == "leading") {
                        passed_validation = true; 
                    } else if (config_.auth_ww.type == "average" && smoothed_peak >= config_.auth_ww.threshold) {
                        passed_validation = true;
                    }
                    break; // Validation finished (either pass or fail). Exit the loop.
                }
            }
        }
        
        tflite_runner_.reset_states(); // Wipe memory for the real VAD loop
        return passed_validation;
    }

    void GroupController::HandleAudioStream(std::shared_ptr<ClientSession> client, std::vector<int16_t>& pcm_data) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (state_ == GroupState::ARBITRATING) {
            auto& buffer = arbitration_buffers_[client->GetID()];
            buffer.insert(buffer.end(), pcm_data.begin(), pcm_data.end());
            return; 
        }

        if (state_ != GroupState::STREAMING || active_client_guid_ != client->GetID()) return; 

        if (config_.use_agc) agc_engine_.Process(pcm_data);

        if (is_recording_to_file_) wav_writer_.Write(pcm_data);
        if (is_live_streaming_) alsa_manager_.WriteLiveStream(config_.output_target, pcm_data);

        ProcessVADFrames(pcm_data, true);
    }

    void GroupController::OnTick() {
        std::lock_guard<std::mutex> lock(mutex_);
        auto now = std::chrono::steady_clock::now();

        if (state_ == GroupState::ARBITRATING) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - arbitration_start_time_).count();
            
            if (elapsed >= config_.arbitration_timeout_ms) {
                if (best_candidate_) {
                    
                    // --- Intercept and run the Authoritative Check with the reported frames ---
                    bool passed_auth_check = true;
                    if (arbitration_buffers_.count(best_candidate_->GetID())) {
                        auto& pre_roll = arbitration_buffers_[best_candidate_->GetID()];
                        passed_auth_check = ValidateAuthoritativeWakeword(pre_roll, best_frame_count_);
                    }
                    
                    if (!passed_auth_check) {
                        std::cout << "[Group: " << config_.name << "] \xE2\x9D\x8C Authoritative check FAILED. Aborting trigger.\n";
                        for (auto& candidate : current_candidates_) {
                            candidate->SendStopSignal(); // Tell everyone to stop streaming
                        }
                        current_candidates_.clear();
                        arbitration_buffers_.clear();
                        best_candidate_ = nullptr;
                        state_ = GroupState::IDLE;
                        return; // Exit completely. Do not open ALSA.
                    }

                    // --- Check Passed (or Disabled). Proceed normally. ---
                    std::cout << "[Group: " << config_.name << "] \xE2\x9C\x85 Authoritative check PASSED.\n";
                    active_client_guid_ = best_candidate_->GetID();
                    state_ = GroupState::STREAMING;
                    
                    std::cout << "[Group: " << config_.name << "] Winner: " << active_client_guid_ << "\n";
                    
                    auto in_time_t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
                    std::stringstream ss;
                    ss << config_.name << "_" << active_client_guid_ << "_" << in_time_t;
                    current_base_filename_ = ss.str();

                    current_metadata_["group"] = config_.name;
                    current_metadata_["guid"] = active_client_guid_;
                    current_metadata_["timestamp"] = in_time_t;
                    current_metadata_["file"] = current_base_filename_ + ".wav";
                    current_metadata_["output_mode"] = (config_.output_type == OutputType::ALSA) ? "alsa" : "file";
                    if (config_.output_type == OutputType::ALSA) current_metadata_["device"] = config_.output_target;
                    current_metadata_["played_from_queue"] = false;

                    is_live_streaming_ = false;
                    is_recording_to_file_ = false;

                    if (config_.output_type == OutputType::ALSA) {
                        is_live_streaming_ = alsa_manager_.RequestLiveStream(config_.output_target);
                        if (!is_live_streaming_) is_recording_to_file_ = true; 
                    } else {
                        is_recording_to_file_ = true; 
                    }

                    if (is_recording_to_file_) {
                        std::string temp_wav = server_config_.temp_dir + current_base_filename_ + ".wav";
                        std::string dest_wav;
                        std::string dest_json;

                        if (config_.output_type == OutputType::ALSA) {
                            dest_wav = server_config_.temp_dir + current_base_filename_ + "_queued.wav";
                            dest_json = server_config_.temp_dir + current_base_filename_ + "_queued.json";
                        } else {
                            dest_wav = server_config_.dest_dir + current_base_filename_ + ".wav";
                            dest_json = server_config_.dest_dir + current_base_filename_ + ".json";
                        }
                        
                        wav_writer_.Open(temp_wav, dest_wav, dest_json, current_metadata_, config_.sample_rate, config_.channels);
                    }

                    std::fill(sliding_window_.begin(), sliding_window_.end(), 0.0f);
                    vad_averager_.reset(0.0f);

                    if (arbitration_buffers_.count(active_client_guid_)) {
                        auto& pre_roll = arbitration_buffers_[active_client_guid_];
                        if (!pre_roll.empty()) {
                            if (config_.use_agc) agc_engine_.Process(pre_roll);
                            
                            if (is_recording_to_file_) wav_writer_.Write(pre_roll);
                            if (is_live_streaming_) alsa_manager_.WriteLiveStream(config_.output_target, pre_roll);
                            
                            ProcessVADFrames(pre_roll, false); 
                        }
                    }
                    
                    vad_averager_.reset(1.0f);
                    best_candidate_->UpdateLastVoiceTime();

                    arbitration_buffers_.clear(); 
                    best_candidate_->SendStartSignal(); 
                    
                    for (auto& candidate : current_candidates_) {
                        if (candidate->GetID() != active_client_guid_) candidate->SendStopSignal();
                    }
                    current_candidates_.clear();
                    
                } else {
                    state_ = GroupState::IDLE; 
                }
            }
        } 
        else if (state_ == GroupState::STREAMING) {
            if (best_candidate_) {
                long silence_ms = best_candidate_->GetTimeSinceLastVoiceMs();
                
                if (silence_ms >= config_.vad_no_voice_ms) {
                    if (debug_mode_) std::cout << "\n";
                    std::cout << "[Group: " << config_.name << "] VAD Timeout (" << silence_ms << "ms). Stopping.\n";
                    
                    if (config_.output_type == OutputType::ALSA) {
                        if (is_live_streaming_) {
                            alsa_manager_.CloseLiveStream(config_.output_target);
                        } else if (is_recording_to_file_) {
                            wav_writer_.CloseAndPublish(); 
                            
                            std::string queued_wav = server_config_.temp_dir + current_base_filename_ + "_queued.wav";
                            std::string queued_json = server_config_.temp_dir + current_base_filename_ + "_queued.json";
                            alsa_manager_.QueueWavFile(config_.output_target, queued_wav, queued_json, current_metadata_);
                        }
                    } else if (config_.output_type == OutputType::FILE) {
                        if (is_recording_to_file_) wav_writer_.CloseAndPublish();
                    }

                    best_candidate_->SendStopSignal();
                    state_ = GroupState::IDLE;
                    active_client_guid_ = "";
                    best_candidate_ = nullptr;
                    is_live_streaming_ = false;
                    is_recording_to_file_ = false;
                }
            }
        }
    }
}
