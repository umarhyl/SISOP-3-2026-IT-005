#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <time.h>
#include "protocol.h"

struct UserNode {
    int socket_fd;
    char uname[50];
    int is_admin;
    int is_logged_in;
};

struct UserNode user_list[MAX_USERS];
time_t time_started;

void write_log(const char* actor, const char* info) {
    FILE *file = fopen("history.log", "a");
    if (!file) return;
    
    time_t curr_time = time(NULL);
    struct tm *t = localtime(&curr_time);
    char ts[30];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", t);
    
    fprintf(file, "[%s] [%s] [%s]\n", ts, actor, info);
    fclose(file);
}

void send_to_all(DataPacket *paket, int sender_socket) {
    for (int i = 0; i < MAX_USERS; i++) {
        if (user_list[i].socket_fd != 0 && 
            user_list[i].socket_fd != sender_socket && 
            user_list[i].is_logged_in) {
            send(user_list[i].socket_fd, paket, sizeof(DataPacket), 0);
        }
    }
}

int check_duplicate_name(const char* name) {
    for (int i = 0; i < MAX_USERS; i++) {
        if (user_list[i].is_logged_in && strcmp(user_list[i].uname, name) == 0) {
            return 1;
        }
    }
    return 0;
}

int main() {
    int server_fd, client_fd, fd_max;
    struct sockaddr_in address;
    fd_set fd_pool, read_fds;
    DataPacket paket;

    memset(user_list, 0, sizeof(user_list));
    time_started = time(NULL);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(SERVER_PORT);

    bind(server_fd, (struct sockaddr *)&address, sizeof(address));
    listen(server_fd, 10);

    FD_ZERO(&fd_pool);
    FD_SET(server_fd, &fd_pool);
    fd_max = server_fd;

    write_log("System", "SERVER ONLINE");

    while (1) {
        read_fds = fd_pool;
        if (select(fd_max + 1, &read_fds, NULL, NULL, NULL) < 0) continue;

        for (int i = 0; i <= fd_max; i++) {
            if (!FD_ISSET(i, &read_fds)) continue;

            if (i == server_fd) {
                client_fd = accept(server_fd, NULL, NULL);
                FD_SET(client_fd, &fd_pool);
                if (client_fd > fd_max) fd_max = client_fd;
                
                for (int j = 0; j < MAX_USERS; j++) {
                    if (user_list[j].socket_fd == 0) {
                        user_list[j].socket_fd = client_fd;
                        break;
                    }
                }
            } else {
                int recv_size = recv(i, &paket, sizeof(DataPacket), 0);
                
                int idx = -1;
                for (int j = 0; j < MAX_USERS; j++) {
                    if (user_list[j].socket_fd == i) idx = j;
                }

                if (recv_size <= 0 || paket.cmd == CMD_QUIT) {
                    if (user_list[idx].is_logged_in) {
                        char msg[BUF_SIZE];
                        sprintf(msg, "User '%s' disconnected", user_list[idx].uname);
                        write_log("System", msg);
                    }
                    close(i);
                    FD_CLR(i, &fd_pool);
                    user_list[idx].socket_fd = 0;
                    user_list[idx].is_logged_in = 0;
                    continue;
                }

                switch (paket.cmd) {
                    case CMD_LOGIN:
                    case CMD_LOGIN_ADMIN:
                        if (check_duplicate_name(paket.username)) {
                            paket.cmd = CMD_FAILED;
                            strcpy(paket.text, "The identity is already synchronized in The Wired.");
                            send(i, &paket, sizeof(DataPacket), 0);
                        } else {
                            strcpy(user_list[idx].uname, paket.username);
                            user_list[idx].is_logged_in = 1;
                            user_list[idx].is_admin = (paket.cmd == CMD_LOGIN_ADMIN);
                            
                            paket.cmd = CMD_SUCCESS;
                            send(i, &paket, sizeof(DataPacket), 0);

                            char msg[BUF_SIZE];
                            sprintf(msg, "User '%s' connected", paket.username);
                            write_log("System", msg);
                        }
                        break;

                    case CMD_MSG:
                        {
                            char msg[BUF_SIZE];
                            sprintf(msg, "[%s]: %s", paket.username, paket.text);
                            write_log("User", msg);
                            send_to_all(&paket, i);
                        }
                        break;

                    case CMD_REQ_USERS:
                        if (user_list[idx].is_admin) {
                            write_log("Admin", "RPC_GET_USERS");
                            int total = 0;
                            for (int j = 0; j < MAX_USERS; j++) {
                                if (user_list[j].is_logged_in && !user_list[j].is_admin) total++;
                            }
                            DataPacket rep;
                            rep.cmd = CMD_INFO;
                            sprintf(rep.text, "Active entities (excluding admin): %d", total);
                            send(i, &rep, sizeof(DataPacket), 0);
                        }
                        break;

                    case CMD_REQ_UPTIME:
                        if (user_list[idx].is_admin) {
                            write_log("Admin", "RPC_GET_UPTIME");
                            DataPacket rep;
                            rep.cmd = CMD_INFO;
                            sprintf(rep.text, "Server uptime: %ld seconds", time(NULL) - time_started);
                            send(i, &rep, sizeof(DataPacket), 0);
                        }
                        break;

                    case CMD_HALT:
                        if (user_list[idx].is_admin) {
                            write_log("Admin", "RPC_SHUTDOWN");
                            write_log("System", "EMERGENCY SHUTDOWN INITIATED");
                            DataPacket rep;
                            rep.cmd = CMD_INFO;
                            strcpy(rep.text, "Initiating emergency shutdown...");
                            send_to_all(&rep, i);
                            exit(0);
                        }
                        break;
                        
                    default:
                        break;
                }
            }
        }
    }
    return 0;
}