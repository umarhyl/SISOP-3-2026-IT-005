#ifndef PROTOCOL_H
#define PROTOCOL_H

#define SERVER_PORT 8080
#define MAX_USERS 100
#define BUF_SIZE 1024

typedef enum {
    CMD_LOGIN,
    CMD_LOGIN_ADMIN,
    CMD_SUCCESS,
    CMD_FAILED,
    CMD_MSG,
    CMD_QUIT,
    CMD_REQ_USERS,
    CMD_REQ_UPTIME,
    CMD_HALT,
    CMD_INFO
} CommandType;

typedef struct {
    CommandType cmd;
    char username[50];
    char text[BUF_SIZE];
} DataPacket;

#endif