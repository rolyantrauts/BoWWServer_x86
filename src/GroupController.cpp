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
        std::cout << "[Group: " << config_.name << "] Initialized with isolated TFLite VAD instance.\n";
    }

    GroupController::~GroupController() {
        if (state_ == GroupState::STREAMING) {
            if (is_recording_to_file_) wav_writer_.CloseAndPublish();
            if (is_live_streaming_) alsa_manager_.CloseLiveStream(config_.output_target);
        }
    }

    void GroupController::HandleConfidenceScore(std::shared_ptr<ClientSession> client, float score) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (state_ == GroupState::IDLE) {
            state_ = GroupState::ARBITRATING;
            arbitration_start_time_ = std::chrono::steady_clock::now();
            best_candidate_ = client;
            best_score_ = score;
            current_candidates_.clear();
            arbitration_buffers_.clear();
            current_candidates_.push_back(client);
            std::cout << "[Group: " << config_.name << "] Candidate: " << client->GetID() << " Score: " << score << "\n";
            std::cout << "[Group: " << config_.name << "] Arbitration started.\n";
        } else if (state_ == GroupState::ARBITRATING) {
            current_candidates_.push_back(client);
            if (score > best_score_) {
                best_candidate_ = client;
                best_score_ = score;
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

                // <--- NEW: Print Throttler to save CPU on SSH redrawing
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
