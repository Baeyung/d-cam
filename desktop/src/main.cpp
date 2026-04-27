#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "Config.h"

// Returns true if parsing succeeds
bool read_exact(int fd, char* buf, size_t size) {
    size_t total_read = 0;
    while (total_read < size) {
        ssize_t n = read(fd, buf + total_read, size - total_read);
        if (n <= 0) return false;
        total_read += n;
    }
    return true;
}

void handle_client(int client_fd) {
    std::cout << "[Desktop] Client connected." << std::endl;
    while (true) {
        char header[9];
        if (!read_exact(client_fd, header, 9)) {
            std::cout << "[Desktop] Client disconnected or error." << std::endl;
            break;
        }

        if (std::memcmp(header, "DROI", 4) != 0) {
            std::cout << "[Desktop] Invalid magic bytes, disconnecting." << std::endl;
            break;
        }

        uint8_t type = header[4];
        uint32_t length;
        std::memcpy(&length, header + 5, 4);
        length = ntohl(length); // Big-endian to host

        std::cout << "[Desktop] Received DROI message. Type: " << (int)type << ", Length: " << length << std::endl;

        if (length > 0) {
            std::vector<char> payload(length);
            if (!read_exact(client_fd, payload.data(), length)) {
                std::cout << "[Desktop] Failed to read payload." << std::endl;
                break;
            }
        }
    }
    close(client_fd);
}

int main() {
    std::cout << "[Desktop] Setting up ADB reverse tunnel..." << std::endl;
    // For production, we'd use fork/exec and check ADB state, but system() works for boilerplate
    int sys_ret = std::system(("adb reverse tcp:" + std::to_string(PHONE_PORT) + " tcp:" + std::to_string(DESKTOP_PORT)).c_str());
    if (sys_ret != 0) {
        std::cerr << "[Desktop] Warning: ADB reverse failed (is device connected?). Continuing anyway..." << std::endl;
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        std::cerr << "[Desktop] Failed to create socket." << std::endl;
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(DESKTOP_PORT);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[Desktop] Bind failed on port " << DESKTOP_PORT << std::endl;
        return 1;
    }

    if (listen(server_fd, 5) < 0) {
        std::cerr << "[Desktop] Listen failed." << std::endl;
        return 1;
    }

    std::cout << "[Desktop] Listening on 127.0.0.1:" << DESKTOP_PORT << std::endl;

    while (true) {
        int client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd < 0) {
            std::cerr << "[Desktop] Accept failed." << std::endl;
            continue;
        }
        std::thread(handle_client, client_fd).detach();
    }

    close(server_fd);
    return 0;
}
