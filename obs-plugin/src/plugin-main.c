#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "Config.h"

#ifdef MOCK_OBS
#define OBS_DECLARE_MODULE() int obs_module_init() { return 1; } void obs_module_unload() {}
#else
#include <obs-module.h>
#endif

OBS_DECLARE_MODULE()

void connect_to_ipc() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        printf("[OBS Plugin] Error creating IPC socket.\\n");
        return;
    }

    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(DESKTOP_IPC_PORT);

    if (inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr) <= 0) {
        printf("[OBS Plugin] Invalid IPC address.\\n");
        close(sock);
        return;
    }

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        printf("[OBS Plugin] Connection to IPC (port %d) failed.\\n", DESKTOP_IPC_PORT);
        close(sock);
        return;
    }

    printf("[OBS Plugin] Connected to IPC successfully.\\n");
    close(sock);
}

int main() {
    printf("[OBS Plugin] OBS Plugin Stub initializing...\\n");
    connect_to_ipc();
    return 0;
}
