#define _POSIX_C_SOURCE 200809L

#include "arena.h"

#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef union {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
} semun_t;

static volatile sig_atomic_t g_running = 1;
static int g_msgid = -1;
static int g_semid = -1;
static int g_shm_accounts_id = -1;
static int g_shm_runtime_id = -1;
static int g_shm_aux_id = -1;
static ArenaAccounts *g_accounts = NULL;
static ArenaRuntime *g_runtime = NULL;
static ArenaAux *g_aux = NULL;

static const Weapon g_weapons[] = {
    {"Wood Sword", 100, 5},
    {"Iron Sword", 300, 15},
    {"Steel Axe", 600, 30},
    {"Demon Blade", 1500, 60},
    {"God Slayer", 5000, 150}
};

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000LL);
}

static void sleep_ms(int ms) {
    struct timespec req;
    req.tv_sec = ms / 1000;
    req.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&req, NULL);
}

static void lock_sem(void) {
    struct sembuf op = {0, -1, SEM_UNDO};
    semop(g_semid, &op, 1);
}

static void unlock_sem(void) {
    struct sembuf op = {0, 1, SEM_UNDO};
    semop(g_semid, &op, 1);
}

static int calc_base_damage(int xp, int weapon_bonus) {
    return BASE_DAMAGE + (xp / 50) + weapon_bonus;
}

static int calc_base_health(int xp) {
    return BASE_HEALTH + (xp / 10);
}

static void add_log(BattleRoom *room, const char *msg) {
    int idx = (room->log_head + room->log_count) % LOG_LINES;
    if (room->log_count == LOG_LINES) {
        room->log_head = (room->log_head + 1) % LOG_LINES;
        idx = (room->log_head + room->log_count - 1) % LOG_LINES;
    } else {
        room->log_count++;
    }
    snprintf(room->log[idx], LOG_LEN, "%s", msg);
}

static void append_history(Account *acc, const char *opponent, int win, int xp_gain, int gold_gain) {
    int idx = acc->history_count;
    if (idx >= MAX_HISTORY) {
        memmove(&acc->history[0], &acc->history[1], sizeof(MatchHistory) * (MAX_HISTORY - 1));
        idx = MAX_HISTORY - 1;
        acc->history_count = MAX_HISTORY;
    } else {
        acc->history_count++;
    }

    snprintf(acc->history[idx].opponent, MAX_NAME, "%s", opponent);
    acc->history[idx].win = win;
    acc->history[idx].xp_gain = xp_gain;
    acc->history[idx].gold_gain = gold_gain;
    acc->history[idx].timestamp = time(NULL);
}

static void send_response(pid_t pid, int status, int code, int user_index, int room_id, const char *text) {
    ArenaResponse resp;
    memset(&resp, 0, sizeof(resp));
    resp.mtype = pid;
    resp.status = status;
    resp.code = code;
    resp.user_index = user_index;
    resp.room_id = room_id;
    if (text) {
        snprintf(resp.text, sizeof(resp.text), "%s", text);
    }
    msgsnd(g_msgid, &resp, sizeof(resp) - sizeof(long), 0);
}

static int find_user_index(const char *username) {
    int i;
    for (i = 0; i < MAX_USERS; i++) {
        if (g_accounts->users[i].used && strcmp(g_accounts->users[i].username, username) == 0) {
            return i;
        }
    }
    return -1;
}

static int create_user(const char *username, const char *password) {
    int i;
    for (i = 0; i < MAX_USERS; i++) {
        if (!g_accounts->users[i].used) {
            Account *acc = &g_accounts->users[i];
            memset(acc, 0, sizeof(*acc));
            acc->used = 1;
            snprintf(acc->username, MAX_NAME, "%s", username);
            snprintf(acc->password, MAX_PASS, "%s", password);
            acc->xp = 0;
            acc->gold = DEFAULT_GOLD;
            acc->weapon_bonus = 0;
            acc->active_pid = 0;
            acc->state = USER_OFFLINE;
            acc->history_count = 0;
            return i;
        }
    }
    return -1;
}

static Account *require_user(ArenaMessage *msg, int *idx_out) {
    int idx = msg->user_index;
    if (idx < 0 || idx >= MAX_USERS || !g_accounts->users[idx].used) {
        send_response(msg->pid, 1, RESP_ERR, -1, -1, "Invalid user");
        return NULL;
    }
    if (idx_out) {
        *idx_out = idx;
    }
    return &g_accounts->users[idx];
}

static int find_free_room(long long now) {
    int i;
    for (i = 0; i < MAX_ROOMS; i++) {
        if (!g_runtime->rooms[i].in_use) {
            return i;
        }
    }
    for (i = 0; i < MAX_ROOMS; i++) {
        if (g_runtime->rooms[i].in_use && g_runtime->rooms[i].finished &&
            now - g_runtime->rooms[i].finished_ms > 5000) {
            return i;
        }
    }
    return -1;
}

static void remove_from_queue(int user_index) {
    int i;
    for (i = 0; i < g_runtime->waiting_count; i++) {
        if (g_runtime->waiting_users[i] == user_index) {
            int j;
            for (j = i; j < g_runtime->waiting_count - 1; j++) {
                g_runtime->waiting_users[j] = g_runtime->waiting_users[j + 1];
                g_runtime->waiting_since[j] = g_runtime->waiting_since[j + 1];
            }
            g_runtime->waiting_count--;
            break;
        }
    }
}

static int user_in_queue(int user_index) {
    int i;
    for (i = 0; i < g_runtime->waiting_count; i++) {
        if (g_runtime->waiting_users[i] == user_index) {
            return 1;
        }
    }
    return 0;
}

static void setup_room(BattleRoom *room, int room_id, int p1, int p2) {
    Account *acc1 = &g_accounts->users[p1];
    int p1_hp = calc_base_health(acc1->xp);
    int p2_hp = (p2 >= 0) ? calc_base_health(g_accounts->users[p2].xp) : BASE_HEALTH;

    memset(room, 0, sizeof(*room));
    room->in_use = 1;
    room->room_id = room_id;
    room->p1 = p1;
    room->p2 = p2;
    room->hp1 = p1_hp;
    room->max_hp1 = p1_hp;
    room->hp2 = p2_hp;
    room->max_hp2 = p2_hp;

    room->last_action_ms1 = 0;
    room->last_action_ms2 = 0;
    room->finished = 0;
    room->winner = 0;
    room->finished_ms = 0;

    add_log(room, "Battle begins!");
    if (p2 >= 0) {
        char msg[LOG_LEN];
        snprintf(msg, sizeof(msg), "%s vs %s", acc1->username, g_accounts->users[p2].username);
        add_log(room, msg);
    } else {
        add_log(room, "Opponent: " BOT_NAME);
    }
}

static void finish_battle(BattleRoom *room, int winner_side) {
    int p1 = room->p1;
    int p2 = room->p2;

    room->finished = 1;
    room->winner = winner_side;
    room->finished_ms = now_ms();

    if (p1 >= 0) {
        Account *acc = &g_accounts->users[p1];
        const char *opp = (p2 >= 0) ? g_accounts->users[p2].username : BOT_NAME;
        if (winner_side == 1) {
            acc->xp += 50;
            acc->gold += 120;
            append_history(acc, opp, 1, 50, 120);
            add_log(room, "Player 1 wins!");
        } else {
            acc->xp += 15;
            acc->gold += 30;
            append_history(acc, opp, 0, 15, 30);
            add_log(room, "Player 1 loses!");
        }
        acc->state = USER_ONLINE;
    }

    if (p2 >= 0) {
        Account *acc = &g_accounts->users[p2];
        const char *opp = g_accounts->users[p1].username;
        if (winner_side == 2) {
            acc->xp += 50;
            acc->gold += 120;
            append_history(acc, opp, 1, 50, 120);
            add_log(room, "Player 2 wins!");
        } else {
            acc->xp += 15;
            acc->gold += 30;
            append_history(acc, opp, 0, 15, 30);
            add_log(room, "Player 2 loses!");
        }
        acc->state = USER_ONLINE;
    }
}

static void handle_attack(ArenaMessage *msg, int is_ult) {
    long long now = now_ms();
    int room_id = msg->room_id;
    if (room_id < 0 || room_id >= MAX_ROOMS) {
        return;
    }

    BattleRoom *room = &g_runtime->rooms[room_id];
    if (!room->in_use || room->finished) {
        return;
    }

    int attacker_side = 0;
    int defender_side = 0;
    Account *attacker = NULL;

    if (room->p1 == msg->user_index) {
        attacker_side = 1;
        defender_side = 2;
        attacker = &g_accounts->users[room->p1];
    } else if (room->p2 == msg->user_index) {
        attacker_side = 2;
        defender_side = 1;
        attacker = &g_accounts->users[room->p2];
    } else {
        return;
    }

    if (!attacker || attacker->state != USER_IN_BATTLE) {
        return;
    }

    if (is_ult && attacker->weapon_bonus <= 0) {
        return;
    }

    long long *last_action = (attacker_side == 1) ? &room->last_action_ms1 : &room->last_action_ms2;
    if (now - *last_action < COOLDOWN_MS) {
        return;
    }

    int dmg = calc_base_damage(attacker->xp, attacker->weapon_bonus);
    if (is_ult) {
        dmg *= 3;
    }

    if (defender_side == 1) {
        room->hp1 -= dmg;
        if (room->hp1 < 0) {
            room->hp1 = 0;
        }
    } else {
        room->hp2 -= dmg;
        if (room->hp2 < 0) {
            room->hp2 = 0;
        }
    }

    *last_action = now;

    {
        char msgbuf[LOG_LEN];
        const char *name = attacker->username;
        if (is_ult) {
            snprintf(msgbuf, sizeof(msgbuf), "%s uses ultimate for %d dmg", name, dmg);
        } else {
            snprintf(msgbuf, sizeof(msgbuf), "%s attacks for %d dmg", name, dmg);
        }
        add_log(room, msgbuf);
    }

    if (room->hp1 <= 0) {
        finish_battle(room, 2);
    } else if (room->hp2 <= 0) {
        finish_battle(room, 1);
    }
}

static void bot_tick(void) {
    long long now = now_ms();
    int i;

    for (i = 0; i < MAX_ROOMS; i++) {
        BattleRoom *room = &g_runtime->rooms[i];
        if (!room->in_use || room->finished) {
            continue;
        }
        if (room->p2 != -1) {
            continue;
        }
        if (now - room->last_action_ms2 < COOLDOWN_MS) {
            continue;
        }
        if (room->hp2 <= 0 || room->hp1 <= 0) {
            continue;
        }

        int dmg = 8 + (rand() % 5);
        room->hp1 -= dmg;
        if (room->hp1 < 0) {
            room->hp1 = 0;
        }
        room->last_action_ms2 = now;
        {
            char msgbuf[LOG_LEN];
            snprintf(msgbuf, sizeof(msgbuf), BOT_NAME " hits for %d dmg", dmg);
            add_log(room, msgbuf);
        }

        if (room->hp1 <= 0) {
            finish_battle(room, 2);
        }
    }
}

static void match_timeout_tick(void) {
    long long now = now_ms();
    int i = 0;

    while (i < g_runtime->waiting_count) {
        int user_index = g_runtime->waiting_users[i];
        long long started = g_runtime->waiting_since[i];
        if (now - started < (long long)MATCH_TIMEOUT_SEC * 1000LL) {
            i++;
            continue;
        }

        int room_idx = find_free_room(now);
        if (room_idx < 0) {
            i++;
            continue;
        }

        setup_room(&g_runtime->rooms[room_idx], room_idx, user_index, -1);
        g_accounts->users[user_index].state = USER_IN_BATTLE;

        {
            pid_t pid = g_accounts->users[user_index].active_pid;
            send_response(pid, 0, RESP_MATCH_BOT, user_index, room_idx, "Match found vs Wild Beast");
        }

        remove_from_queue(user_index);
    }
}

static void try_match_queue(void) {
    long long now = now_ms();

    while (g_runtime->waiting_count >= 2) {
        int room_idx = find_free_room(now);
        if (room_idx < 0) {
            return;
        }

        int p1 = g_runtime->waiting_users[0];
        int p2 = g_runtime->waiting_users[1];

        remove_from_queue(p1);
        remove_from_queue(p2);

        setup_room(&g_runtime->rooms[room_idx], room_idx, p1, p2);
        g_accounts->users[p1].state = USER_IN_BATTLE;
        g_accounts->users[p2].state = USER_IN_BATTLE;

        {
            pid_t pid1 = g_accounts->users[p1].active_pid;
            pid_t pid2 = g_accounts->users[p2].active_pid;
            send_response(pid1, 0, RESP_MATCH_FOUND, p1, room_idx, g_accounts->users[p2].username);
            send_response(pid2, 0, RESP_MATCH_FOUND, p2, room_idx, g_accounts->users[p1].username);
        }
    }
}

static void handle_register(ArenaMessage *msg) {
    if (msg->username[0] == '\0' || msg->password[0] == '\0') {
        send_response(msg->pid, 1, RESP_ERR, -1, -1, "Username or password empty");
        return;
    }

    if (find_user_index(msg->username) >= 0) {
        send_response(msg->pid, 1, RESP_ERR, -1, -1, "Username already exists");
        return;
    }

    if (create_user(msg->username, msg->password) < 0) {
        send_response(msg->pid, 1, RESP_ERR, -1, -1, "User limit reached");
        return;
    }

    send_response(msg->pid, 0, RESP_REGISTER_OK, -1, -1, "Register success");
}

static void handle_login(ArenaMessage *msg) {
    int idx = find_user_index(msg->username);
    if (idx < 0) {
        send_response(msg->pid, 1, RESP_ERR, -1, -1, "User not found");
        return;
    }

    Account *acc = &g_accounts->users[idx];
    if (strcmp(acc->password, msg->password) != 0) {
        send_response(msg->pid, 1, RESP_ERR, -1, -1, "Wrong password");
        return;
    }

    if (acc->active_pid != 0 && acc->active_pid != msg->pid) {
        send_response(msg->pid, 1, RESP_ERR, -1, -1, "User already logged in");
        return;
    }

    acc->active_pid = msg->pid;
    acc->state = USER_ONLINE;

    send_response(msg->pid, 0, RESP_LOGIN_OK, idx, -1, "Login success");
}

static void handle_logout(ArenaMessage *msg) {
    int idx;
    Account *acc = require_user(msg, &idx);
    if (!acc) {
        return;
    }

    if (acc->state == USER_IN_BATTLE || acc->state == USER_MATCHING) {
        send_response(msg->pid, 1, RESP_ERR, idx, -1, "Cannot logout while busy");
        return;
    }

    acc->active_pid = 0;
    acc->state = USER_OFFLINE;

    send_response(msg->pid, 0, RESP_LOGOUT_OK, idx, -1, "Logout success");
}

static void handle_match(ArenaMessage *msg) {
    int idx;
    Account *acc = require_user(msg, &idx);
    if (!acc) {
        return;
    }

    if (acc->state == USER_MATCHING || acc->state == USER_IN_BATTLE) {
        send_response(msg->pid, 1, RESP_ALREADY_MATCHING, idx, -1, "Already in queue or battle");
        return;
    }

    if (g_runtime->waiting_count >= MAX_MATCH_QUEUE) {
        send_response(msg->pid, 1, RESP_ERR, idx, -1, "Queue full");
        return;
    }

    if (!user_in_queue(idx)) {
        g_runtime->waiting_users[g_runtime->waiting_count] = idx;
        g_runtime->waiting_since[g_runtime->waiting_count] = now_ms();
        g_runtime->waiting_count++;
        acc->state = USER_MATCHING;
    }

    send_response(msg->pid, 0, RESP_OK, idx, -1, "Queued");
    try_match_queue();
}

static void handle_cancel_match(ArenaMessage *msg) {
    int idx;
    Account *acc = require_user(msg, &idx);
    if (!acc) {
        return;
    }

    remove_from_queue(idx);
    acc->state = USER_ONLINE;
    send_response(msg->pid, 0, RESP_OK, idx, -1, "Matchmaking canceled");
}

static void handle_buy(ArenaMessage *msg) {
    int idx;
    int choice = msg->arg1;
    Account *acc = require_user(msg, &idx);
    if (!acc) {
        return;
    }

    if (choice < 0 || choice >= MAX_WEAPONS) {
        send_response(msg->pid, 1, RESP_ERR, idx, -1, "Invalid choice");
        return;
    }

    const Weapon *w = &g_weapons[choice];
    if (acc->gold < w->cost) {
        send_response(msg->pid, 1, RESP_BUY_FAIL, idx, -1, "Not enough gold");
        return;
    }

    acc->gold -= w->cost;
    if (w->bonus > acc->weapon_bonus) {
        acc->weapon_bonus = w->bonus;
    }

    send_response(msg->pid, 0, RESP_BUY_OK, idx, -1, "Purchase success");
}

static void cleanup_ipc(void) {
    if (g_runtime) {
        lock_sem();
        g_runtime->server_ready = 0;
        g_runtime->server_pid = 0;
        unlock_sem();
    }

    if (g_msgid >= 0) {
        msgctl(g_msgid, IPC_RMID, NULL);
        g_msgid = -1;
    }

    if (g_semid >= 0) {
        semctl(g_semid, 0, IPC_RMID);
        g_semid = -1;
    }

    if (g_accounts) {
        shmdt(g_accounts);
        g_accounts = NULL;
    }
    if (g_runtime) {
        shmdt(g_runtime);
        g_runtime = NULL;
    }
    if (g_aux) {
        shmdt(g_aux);
        g_aux = NULL;
    }
}

static void handle_signal(int sig) {
    (void)sig;
    g_running = 0;
}

int main(void) {
    srand((unsigned int)time(NULL));

    g_shm_accounts_id = shmget(SHM_ACCOUNTS_KEY, sizeof(ArenaAccounts), IPC_CREAT | 0666);
    g_shm_runtime_id = shmget(SHM_RUNTIME_KEY, sizeof(ArenaRuntime), IPC_CREAT | 0666);
    g_shm_aux_id = shmget(SHM_AUX_KEY, sizeof(ArenaAux), IPC_CREAT | 0666);
    if (g_shm_accounts_id < 0 || g_shm_runtime_id < 0 || g_shm_aux_id < 0) {
        perror("shmget");
        return 1;
    }

    g_accounts = (ArenaAccounts *)shmat(g_shm_accounts_id, NULL, 0);
    g_runtime = (ArenaRuntime *)shmat(g_shm_runtime_id, NULL, 0);
    g_aux = (ArenaAux *)shmat(g_shm_aux_id, NULL, 0);
    if (g_accounts == (void *)-1 || g_runtime == (void *)-1 || g_aux == (void *)-1) {
        perror("shmat");
        return 1;
    }

    g_semid = semget(SEM_KEY, 1, IPC_CREAT | 0666);
    if (g_semid < 0) {
        perror("semget");
        return 1;
    }

    {
        semun_t semval;
        semval.val = 1;
        semctl(g_semid, 0, SETVAL, semval);
    }

    g_msgid = msgget(MSG_KEY, IPC_CREAT | 0666);
    if (g_msgid < 0) {
        perror("msgget");
        return 1;
    }

    lock_sem();
    if (g_runtime->magic != ARENA_MAGIC) {
        memset(g_accounts, 0, sizeof(*g_accounts));
        memset(g_runtime, 0, sizeof(*g_runtime));
        g_runtime->magic = ARENA_MAGIC;
    }

    if (g_runtime->server_ready && g_runtime->server_pid > 0) {
        if (kill(g_runtime->server_pid, 0) == 0) {
            unlock_sem();
            fprintf(stderr, "Orion already running (PID %d)\n", g_runtime->server_pid);
            return 1;
        }
    }

    g_runtime->server_ready = 1;
    g_runtime->server_pid = getpid();
    unlock_sem();

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    printf("Orion is ready (PID: %d)\n", getpid());

    while (g_running) {
        ArenaMessage msg;
        ssize_t n = msgrcv(g_msgid, &msg, sizeof(msg) - sizeof(long), 1, IPC_NOWAIT);
        if (n >= 0) {
            lock_sem();
            switch (msg.cmd) {
                case CMD_REGISTER:
                    handle_register(&msg);
                    break;
                case CMD_LOGIN:
                    handle_login(&msg);
                    break;
                case CMD_LOGOUT:
                    handle_logout(&msg);
                    break;
                case CMD_MATCH:
                    handle_match(&msg);
                    break;
                case CMD_CANCEL_MATCH:
                    handle_cancel_match(&msg);
                    break;
                case CMD_ATTACK:
                    handle_attack(&msg, 0);
                    break;
                case CMD_ULT:
                    handle_attack(&msg, 1);
                    break;
                case CMD_BUY:
                    handle_buy(&msg);
                    break;
                default:
                    send_response(msg.pid, 1, RESP_ERR, -1, -1, "Unknown command");
                    break;
            }
            unlock_sem();
        } else if (errno != ENOMSG) {
            perror("msgrcv");
        }

        lock_sem();
        match_timeout_tick();
        bot_tick();
        unlock_sem();

        sleep_ms(100);
    }

    cleanup_ipc();
    return 0;
}
