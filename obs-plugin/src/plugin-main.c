#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include "Config.h"

#ifdef MOCK_OBS
#define OBS_DECLARE_MODULE() int obs_module_init() { return 1; } void obs_module_unload() {}
#else
#include <obs-module.h>
#endif

OBS_DECLARE_MODULE()

int ipc_socket = -1;
pthread_t receive_thread;
volatile int keep_running = 1;

int read_exact(int fd, void* buf, size_t size) {
    size_t total_read = 0;
    while (total_read < size) {
        ssize_t n = read(fd, (char*)buf + total_read, size - total_read);
        if (n <= 0) return 0;
        total_read += n;
    }
    return 1;
}

void* receive_frames(void* arg) {
    while (keep_running) {
        if (ipc_socket < 0) {
            sleep(1);
            continue;
        }

        uint32_t header[5];
        if (!read_exact(ipc_socket, header, 20)) {
            printf("[OBS Plugin] IPC disconnected.\n");
            close(ipc_socket);
            ipc_socket = -1;
            continue;
        }

        uint32_t len = header[4];
        if (len > 0 && len < 10000000) {
            uint8_t* buffer = (uint8_t*)malloc(len);
            if (buffer) {
                if (read_exact(ipc_socket, buffer, len)) {
                    printf("[OBS Plugin] Received frame: %dx%d, size: %d\n", header[0], header[1], len);
                    // Here we would call obs_source_output_video()
                }
                free(buffer);
            }
        }
    }
    return NULL;
}

void connect_to_ipc() {
    ipc_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (ipc_socket < 0) {
        printf("[OBS Plugin] Error creating IPC socket.\n");
        return;
    }

    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(DESKTOP_IPC_PORT);

    if (inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr) <= 0) {
        close(ipc_socket);
        ipc_socket = -1;
        return;
    }

    if (connect(ipc_socket, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        printf("[OBS Plugin] Connection to IPC (port %d) failed.\n", DESKTOP_IPC_PORT);
        close(ipc_socket);
        ipc_socket = -1;
        return;
    }

    printf("[OBS Plugin] Connected to IPC successfully.\n");
}

int main() {
    printf("[OBS Plugin] Initializing DroidCam Source...\n");
    connect_to_ipc();
    pthread_create(&receive_thread, NULL, receive_frames, NULL);

    sleep(2);
    keep_running = 0;
    pthread_join(receive_thread, NULL);
    return 0;
}
