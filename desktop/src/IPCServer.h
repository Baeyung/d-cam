#pragma once
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <iostream>
#include <cstring>
#include "Config.h"

class IPCServer {
private:
    int server_fd;
    int client_fd = -1;

public:
    bool start() {
        server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd == -1) return false;

        int opt = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons(DESKTOP_IPC_PORT);

        if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) return false;
        if (listen(server_fd, 1) < 0) return false;

        std::cout << "[IPC] Listening for OBS plugin on port " << DESKTOP_IPC_PORT << std::endl;
        return true;
    }

    void accept_client() {
        if (client_fd != -1) close(client_fd);
        client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd >= 0) {
            std::cout << "[IPC] OBS Plugin connected." << std::endl;
        }
    }

    void send_frame(uint32_t width, uint32_t height, uint64_t timestamp, const std::vector<uint8_t>& frame_data) {
        if (client_fd < 0) return;

        // Header: width (4), height (4), timestamp (8), length (4)
        uint32_t len = frame_data.size();
        std::vector<uint8_t> header(20);
        memcpy(header.data(), &width, 4);
        memcpy(header.data() + 4, &height, 4);
        memcpy(header.data() + 8, &timestamp, 8);
        memcpy(header.data() + 16, &len, 4);

        if (send(client_fd, header.data(), 20, MSG_NOSIGNAL) < 0) {
            close(client_fd);
            client_fd = -1;
            return;
        }

        if (send(client_fd, frame_data.data(), len, MSG_NOSIGNAL) < 0) {
            close(client_fd);
            client_fd = -1;
        }
    }

    ~IPCServer() {
        if (client_fd != -1) close(client_fd);
        if (server_fd != -1) close(server_fd);
    }
};
