#include <iostream>
#include <string>
#include <getopt.h>
#include <csignal>
#include <atomic>
#include "BoWWServer.h"

// Global pointer for the signal handler to access the running server
boww::BoWWServer* g_server = nullptr;

void signal_handler(int signum) {
    std::cout << "\n[!] Caught shutdown signal. Stopping server gracefully...\n";
    if (g_server) {
        g_server->Stop();
    }
}

void print_usage(const char* prog_name) {
    std::cout << "\nBoWWServer (x86_64 TFLite Edition)\n"
              << "Usage: " << prog_name << " [OPTIONS]\n\n"
              << "Options:\n"
              << "  -c <dir>       Path to config directory containing clients.yaml (default: ./)\n"
              << "  -p <port>      WebSocket Port (default: 9002)\n"
              << "  -m <filepath>  Path to TFLite model (default: models/hey_jarvis_f32.tflite)\n"
              << "  -d             Enable Debug Mode (verbose logs and VAD telemetry)\n"
              << "  -h             Show this help message\n\n";
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::string config_dir = "./";
    std::string model_path = "models/hey_jarvis_f32.tflite"; 
    uint16_t port = 9002;
    bool debug_mode = false;

    int opt;
    while ((opt = getopt(argc, argv, "c:p:m:dh")) != -1) {
        switch (opt) {
            case 'c': 
                config_dir = optarg; 
                if (!config_dir.empty() && config_dir.back() != '/' && config_dir.back() != '\\') {
                    config_dir += "/";
                }
                break;
            case 'p': port = std::stoi(optarg); break;
            case 'm': model_path = optarg; break;
            case 'd': debug_mode = true; break;
            case 'h': print_usage(argv[0]); return 0;
            default: print_usage(argv[0]); return 1;
        }
    }

    try {
        boww::BoWWServer server(config_dir, model_path, debug_mode);
        g_server = &server; 
        
        // This will block until g_server->Stop() is triggered by the signal handler
        server.Run(port);

        g_server = nullptr; 
        std::cout << "[Server] Shutdown complete.\n";
    } catch (const std::exception& e) {
        std::cerr << "[!] Fatal Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
