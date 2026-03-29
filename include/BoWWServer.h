#pragma once
#include "BoWWServerDefs.h"
#include "ConfigManager.h"
#include "ClientSession.h"
#include "GroupController.h"
#include "AlsaSinkManager.h"
#include "MDNSService.h"

#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <atomic>

namespace boww {

    class BoWWServer {
    public:
        BoWWServer(std::string config_dir, std::string model_path, bool debug = false);
        ~BoWWServer();

        void Run(uint16_t port);
        void Stop(); 
        void SendJSON(ConnectionHdl hdl, const nlohmann::json& j);

    private:
        void OnOpen(ConnectionHdl hdl);
        void OnClose(ConnectionHdl hdl);
        void OnMessage(ConnectionHdl hdl, ServerType::message_ptr msg);

        ServerType endpoint_;
        std::map<ConnectionHdl, std::shared_ptr<ClientSession>, std::owner_less<ConnectionHdl>> sessions_;
        std::mutex sessions_mutex_;

        ConfigManager config_manager_;
        std::string config_dir_;
        std::string model_path_; // <--- NEW: Server just holds the path now

        std::map<std::string, std::shared_ptr<GroupController>> groups_;
        std::mutex group_mutex_;

        AlsaSinkManager alsa_manager_;
        MDNSService mdns_service_;

        bool debug_mode_;
        std::atomic<bool> running_{false};
        std::thread ticker_thread_;
    };
}
