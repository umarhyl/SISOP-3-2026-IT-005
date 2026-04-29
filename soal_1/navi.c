#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <signal.h>
#include "protocol.h"

int is_running = 1;

void handle_signal(int sig) {
    is_running = 0;
}

void show_prompt(int is_admin) {
    if (is_admin) printf("Command >> ");
    else printf("> ");
    fflush(stdout);
}

int main() {
    int client_socket;
    struct sockaddr_in serv_addr;
    DataPacket paket;
    char input_name[50];
    int admin_mode = 0;

    client_socket = socket(AF_INET, SOCK_STREAM, 0);
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    serv_addr.sin_port = htons(SERVER_PORT);

    if (connect(client_socket, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        puts("Connection failed.");
        return 1;
    }

    while (1) {
        printf("Enter your name: ");
        fgets(input_name, 50, stdin);
        input_name[strcspn(input_name, "\n")] = 0;

        if (strcmp(input_name, "The Knights") == 0) {
            char pwd[50];
            printf("Enter Password: ");
            fgets(pwd, 50, stdin);
            pwd[strcspn(pwd, "\n")] = 0;

            if (strcmp(pwd, "protocol7") == 0) {
                admin_mode = 1;
                paket.cmd = CMD_LOGIN_ADMIN;
            } else {
                puts("[System] Authentication failed.");
                continue;
            }
        } else {
            admin_mode = 0;
            paket.cmd = CMD_LOGIN;
        }

        strcpy(paket.username, input_name);
        send(client_socket, &paket, sizeof(DataPacket), 0);
        recv(client_socket, &paket, sizeof(DataPacket), 0);

        if (paket.cmd == CMD_SUCCESS) {
            if (admin_mode) {
                printf("\n[System] Authentication Successful. Granted Admin privileges.\n\n");
                printf("=== THE KNIGHTS CONSOLE ===\n");
                printf("1. Check Active Entities (Users)\n");
                printf("2. Check Server Uptime\n");
                printf("3. Execute Emergency Shutdown\n");
                printf("4. Disconnect\n");
            } else {
                printf("--- Welcome to The Wired, %s ---\n", input_name);
            }
            break;
        } else {
            printf("[System] %s\n", paket.text);
        }
    }

    signal(SIGINT, handle_signal);
    fd_set read_fds;
    show_prompt(admin_mode);

    while (is_running) {
        FD_ZERO(&read_fds);
        FD_SET(STDIN_FILENO, &read_fds);
        FD_SET(client_socket, &read_fds);

        if (select(client_socket + 1, &read_fds, NULL, NULL, NULL) < 0) break; 

        if (FD_ISSET(client_socket, &read_fds)) {
            if (recv(client_socket, &paket, sizeof(DataPacket), 0) <= 0) {
                printf("\n[System] Connection lost.\n");
                break;
            }
            
            printf("\33[2K\r"); 
            if (paket.cmd == CMD_MSG) {
                printf("[%s]: %s\n", paket.username, paket.text);
            } else if (paket.cmd == CMD_INFO) {
                printf("[Server]: %s\n", paket.text);
            }
            show_prompt(admin_mode);
        }

        if (FD_ISSET(STDIN_FILENO, &read_fds)) {
            char buffer[BUF_SIZE];
            if (fgets(buffer, BUF_SIZE, stdin) == NULL) break;
            buffer[strcspn(buffer, "\n")] = 0;

            if (admin_mode) {
                int menu = atoi(buffer);
                switch (menu) {
                    case 1: paket.cmd = CMD_REQ_USERS; break;
                    case 2: paket.cmd = CMD_REQ_UPTIME; break;
                    case 3: paket.cmd = CMD_HALT; break;
                    case 4: is_running = 0; continue;
                    default: paket.cmd = -1; break;
                }

                if (paket.cmd != -1) {
                    send(client_socket, &paket, sizeof(DataPacket), 0);
                }
            } else {
                if (strcmp(buffer, "/exit") == 0) break;
                if (strlen(buffer) > 0) {
                    paket.cmd = CMD_MSG;
                    strcpy(paket.username, input_name);
                    strcpy(paket.text, buffer);
                    send(client_socket, &paket, sizeof(DataPacket), 0);
                }
            }
            show_prompt(admin_mode);
        }
    }

    printf("\n[System] Disconnecting from The Wired...\n");
    paket.cmd = CMD_QUIT;
    send(client_socket, &paket, sizeof(DataPacket), 0);
    close(client_socket);

    return 0;
}