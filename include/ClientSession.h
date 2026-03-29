#pragma once
#include <string>
#include <memory>
#include <chrono>
#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>
#include "BoWWServerDefs.h"

namespace boww {
    using ServerType = websocketpp::server<websocketpp::config::asio>;
    using ConnectionHdl = websocketpp::connection_hdl;

    class BoWWServer; 

    class ClientSession {
    public:
        ClientSession(ConnectionHdl hdl, BoWWServer* server) 
            : hdl_(hdl), server_(server), is_authenticated_(false) {}

        void SetGUID(const std::string& guid, const std::string& group) {
            guid_ = guid;
            group_ = group;
            is_authenticated_ = true;
        }

        void AssignTempID(const std::string& temp_id) { temp_id_ = temp_id; }

        bool IsAuthenticated() const { return is_authenticated_; }
        std::string GetID() const { return guid_.empty() ? temp_id_ : guid_; }
        std::string GetGroup() const { return group_; }
        ConnectionHdl GetHandle() const { return hdl_; }

        void UpdateLastVoiceTime() { last_voice_time_ = std::chrono::steady_clock::now(); }
        
        long GetTimeSinceLastVoiceMs() const {
            auto now = std::chrono::steady_clock::now();
            return std::chrono::duration_cast<std::chrono::milliseconds>(now - last_voice_time_).count();
        }

        void SendStartSignal();
        void SendStopSignal();

    private:
        ConnectionHdl hdl_;
        BoWWServer* server_;
        std::string guid_;
        std::string group_;
        std::string temp_id_;
        bool is_authenticated_;
        std::chrono::steady_clock::time_point last_voice_time_;
    };
}
