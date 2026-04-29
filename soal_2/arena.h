#ifndef ARENA_H
#define ARENA_H

#include <sys/types.h>

#define MAX_USERS 64
#define MAX_NAME 32
#define MAX_PASS 32
#define MAX_HISTORY 20
#define MAX_ROOMS 8
#define MAX_MATCH_QUEUE 16
#define LOG_LINES 5
#define LOG_LEN 96

#define BASE_DAMAGE 10
#define BASE_HEALTH 100
#define DEFAULT_GOLD 150
#define MATCH_TIMEOUT_SEC 35
#define COOLDOWN_MS 1000
#define MAX_WEAPONS 5
#define UI_BAR_WIDTH 24
#define UI_REFRESH_MS 100
#define BOT_NAME "Wild Beast"

#define SHM_ACCOUNTS_KEY 0x00001234
#define SHM_RUNTIME_KEY  0x00005678
#define SHM_AUX_KEY      0x00009012
#define MSG_KEY          0x0000BEEF
#define SEM_KEY          0x0000CAFE
#define ARENA_MAGIC      0xA7E3E1

enum UserState {
    USER_OFFLINE = 0,
    USER_ONLINE,
    USER_MATCHING,
    USER_IN_BATTLE
};

enum CmdType {
    CMD_PING = 1,
    CMD_REGISTER,
    CMD_LOGIN,
    CMD_LOGOUT,
    CMD_MATCH,
    CMD_CANCEL_MATCH,
    CMD_ATTACK,
    CMD_ULT,
    CMD_BUY
};

enum RespCode {
    RESP_OK = 0,
    RESP_ERR = 1,
    RESP_REGISTER_OK,
    RESP_LOGIN_OK,
    RESP_LOGOUT_OK,
    RESP_MATCH_FOUND,
    RESP_MATCH_BOT,
    RESP_BUY_OK,
    RESP_BUY_FAIL,
    RESP_ALREADY_MATCHING,
    RESP_NOT_READY
};

typedef struct {
    char opponent[MAX_NAME];
    int win;
    int xp_gain;
    int gold_gain;
    long timestamp;
} MatchHistory;

typedef struct {
    int used;
    char username[MAX_NAME];
    char password[MAX_PASS];
    int xp;
    int gold;
    int weapon_bonus;
    pid_t active_pid;
    int state;
    MatchHistory history[MAX_HISTORY];
    int history_count;
} Account;

typedef struct {
    Account users[MAX_USERS];
} ArenaAccounts;

typedef struct {
    int in_use;
    int room_id;
    int p1;
    int p2;
    int hp1;
    int hp2;
    int max_hp1;
    int max_hp2;
    long long last_action_ms1;
    long long last_action_ms2;
    int finished;
    int winner;
    long long finished_ms;
    char log[LOG_LINES][LOG_LEN];
    int log_head;
    int log_count;
} BattleRoom;

typedef struct {
    int magic;
    int server_ready;
    pid_t server_pid;
    int waiting_count;
    int waiting_users[MAX_MATCH_QUEUE];
    long long waiting_since[MAX_MATCH_QUEUE];
    BattleRoom rooms[MAX_ROOMS];
} ArenaRuntime;

typedef struct {
    int reserved;
} ArenaAux;

typedef struct {
    long mtype;
    int cmd;
    pid_t pid;
    int user_index;
    int room_id;
    int arg1;
    int arg2;
    char username[MAX_NAME];
    char password[MAX_PASS];
} ArenaMessage;

typedef struct {
    long mtype;
    int status;
    int code;
    int user_index;
    int room_id;
    char text[128];
} ArenaResponse;

typedef struct {
    const char *name;
    int cost;
    int bonus;
} Weapon;

#endif
