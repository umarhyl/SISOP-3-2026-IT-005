#define _POSIX_C_SOURCE 200809L

#include "arena.h"

#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/select.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

typedef union {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
} semun_t;

static int g_msgid = -1;
static int g_semid = -1;
static int g_shm_accounts_id = -1;
static int g_shm_runtime_id = -1;
static int g_shm_aux_id = -1;
static ArenaAccounts *g_accounts = NULL;
static ArenaRuntime *g_runtime = NULL;
static ArenaAux *g_aux = NULL;
static int g_user_index = -1;
static struct termios g_orig_term;
static int g_orig_flags = 0;

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

static void lock_sem(void) {
    struct sembuf op = {0, -1, SEM_UNDO};
    semop(g_semid, &op, 1);
}

static void unlock_sem(void) {
    struct sembuf op = {0, 1, SEM_UNDO};
    semop(g_semid, &op, 1);
}

static void trim_newline(char *s) {
    size_t len = strlen(s);
    if (len > 0 && s[len - 1] == '\n') {
        s[len - 1] = '\0';
    }
}

static void read_line(const char *prompt, char *buf, size_t size) {
    printf("%s", prompt);
    fflush(stdout);
    if (fgets(buf, (int)size, stdin) == NULL) {
        buf[0] = '\0';
        return;
    }
    trim_newline(buf);
}

static int read_int(const char *prompt) {
    char buf[64];
    read_line(prompt, buf, sizeof(buf));
    return atoi(buf);
}

static void enable_raw_mode(void) {
    struct termios raw;
    tcgetattr(STDIN_FILENO, &g_orig_term);
    raw = g_orig_term;
    raw.c_lflag &= (unsigned long)~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    g_orig_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, g_orig_flags | O_NONBLOCK);
}

static void disable_raw_mode(void) {
    tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_term);
    fcntl(STDIN_FILENO, F_SETFL, g_orig_flags);
}

static void send_request(ArenaMessage *msg) {
    msg->mtype = 1;
    msg->pid = getpid();
    msgsnd(g_msgid, msg, sizeof(*msg) - sizeof(long), 0);
}

static int recv_response(ArenaResponse *resp, int flags) {
    return (int)msgrcv(g_msgid, resp, sizeof(*resp) - sizeof(long), getpid(), flags);
}

static Account get_account_snapshot(int idx) {
    Account acc;
    memset(&acc, 0, sizeof(acc));
    lock_sem();
    if (idx >= 0 && idx < MAX_USERS) {
        acc = g_accounts->users[idx];
    }
    unlock_sem();
    return acc;
}

static BattleRoom get_room_snapshot(int room_id) {
    BattleRoom room;
    memset(&room, 0, sizeof(room));
    lock_sem();
    if (room_id >= 0 && room_id < MAX_ROOMS) {
        room = g_runtime->rooms[room_id];
    }
    unlock_sem();
    return room;
}

static void show_profile(void) {
    Account acc = get_account_snapshot(g_user_index);
    int level = (acc.xp / 100) + 1;
    int dmg = BASE_DAMAGE + (acc.xp / 50) + acc.weapon_bonus;
    int hp = BASE_HEALTH + (acc.xp / 10);

    printf("\n=== PROFILE ===\n");
    printf("User : %s\n", acc.username);
    printf("Gold : %d\n", acc.gold);
    printf("Lvl  : %d\n", level);
    printf("XP   : %d\n", acc.xp);
    printf("Dmg  : %d\n", dmg);
    printf("HP   : %d\n", hp);
    printf("Weapon Bonus : %d\n", acc.weapon_bonus);
}

static void show_history(void) {
    Account acc = get_account_snapshot(g_user_index);
    int i;

    printf("\n=== MATCH HISTORY ===\n");
    if (acc.history_count == 0) {
        printf("No history yet.\n");
        return;
    }

    for (i = 0; i < acc.history_count; i++) {
        MatchHistory *h = &acc.history[i];
        char tbuf[32];
        struct tm *tm_info = localtime(&h->timestamp);
        if (tm_info) {
            strftime(tbuf, sizeof(tbuf), "%H:%M:%S", tm_info);
        } else {
            snprintf(tbuf, sizeof(tbuf), "--:--:--");
        }
        printf("%s | %-10s | %s | XP %+d | Gold %+d\n",
               tbuf,
               h->opponent,
               h->win ? "WIN" : "LOSS",
               h->xp_gain,
               h->gold_gain);
    }
}

static void show_armory(void) {
    int i;
    printf("\n=== ARMORY ===\n");
    for (i = 0; i < (int)(sizeof(g_weapons) / sizeof(g_weapons[0])); i++) {
        printf("%d. %-12s | Cost %d | Bonus %d\n", i + 1, g_weapons[i].name, g_weapons[i].cost, g_weapons[i].bonus);
    }
    printf("0. Back\n");

    int choice = read_int("Choice: ");
    if (choice <= 0) {
        return;
    }

    ArenaMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.cmd = CMD_BUY;
    msg.user_index = g_user_index;
    msg.arg1 = choice - 1;
    send_request(&msg);

    ArenaResponse resp;
    if (recv_response(&resp, 0) >= 0) {
        printf("%s\n", resp.text);
    }
}

static void render_battle_screen(const BattleRoom *room, int my_side, const char *opp_name) {
    int my_hp = (my_side == 1) ? room->hp1 : room->hp2;
    int my_max = (my_side == 1) ? room->max_hp1 : room->max_hp2;
    int opp_hp = (my_side == 1) ? room->hp2 : room->hp1;
    int opp_max = (my_side == 1) ? room->max_hp2 : room->max_hp1;
    int i;

    printf("\033[2J\033[H");
    printf("=== ARENA ===\n\n");
    printf("You    : %d/%d\n", my_hp, my_max);
    printf("Enemy  : %s (%d/%d)\n\n", opp_name, opp_hp, opp_max);
    printf("Actions: [a] attack  [u] ultimate\n\n");
    printf("--- Combat Log ---\n");

    for (i = 0; i < room->log_count; i++) {
        int idx = (room->log_head + i) % LOG_LINES;
        printf("%s\n", room->log[idx]);
    }
    printf("\n");
    fflush(stdout);
}

static void battle_loop(int room_id, const char *opp_name) {
    long long last_send = 0;
    Account acc = get_account_snapshot(g_user_index);
    int my_side = 1;

    enable_raw_mode();

    while (1) {
        BattleRoom room = get_room_snapshot(room_id);
        if (!room.in_use) {
            break;
        }

        if (room.p1 == g_user_index) {
            my_side = 1;
        } else if (room.p2 == g_user_index) {
            my_side = 2;
        }

        render_battle_screen(&room, my_side, opp_name);

        if (room.finished) {
            const char *result = "DEFEAT";
            if ((room.winner == 1 && my_side == 1) || (room.winner == 2 && my_side == 2)) {
                result = "VICTORY";
            }
            printf("Battle ended: %s\n", result);
            printf("Press ENTER to continue...\n");
            fflush(stdout);
            disable_raw_mode();
            getchar();
            return;
        }

        fd_set set;
        struct timeval tv;
        FD_ZERO(&set);
        FD_SET(STDIN_FILENO, &set);
        tv.tv_sec = 0;
        tv.tv_usec = 200000;

        if (select(STDIN_FILENO + 1, &set, NULL, NULL, &tv) > 0) {
            char ch;
            if (read(STDIN_FILENO, &ch, 1) > 0) {
                long long now = now_ms();
                if (now - last_send < COOLDOWN_MS) {
                    continue;
                }

                if (ch == 'a' || ch == 'A') {
                    ArenaMessage msg;
                    memset(&msg, 0, sizeof(msg));
                    msg.cmd = CMD_ATTACK;
                    msg.user_index = g_user_index;
                    msg.room_id = room_id;
                    send_request(&msg);
                    last_send = now;
                } else if (ch == 'u' || ch == 'U') {
                    if (acc.weapon_bonus <= 0) {
                        continue;
                    }
                    ArenaMessage msg;
                    memset(&msg, 0, sizeof(msg));
                    msg.cmd = CMD_ULT;
                    msg.user_index = g_user_index;
                    msg.room_id = room_id;
                    send_request(&msg);
                    last_send = now;
                }
            }
        }
    }

    disable_raw_mode();
}

static void matchmaking(void) {
    ArenaMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.cmd = CMD_MATCH;
    msg.user_index = g_user_index;
    send_request(&msg);

    printf("Searching for opponent...\n");

    long long start = now_ms();
    while (1) {
        ArenaResponse resp;
        int r = recv_response(&resp, IPC_NOWAIT);
        if (r >= 0) {
            if (resp.status != 0) {
                printf("%s\n", resp.text);
                return;
            }

            if (resp.code == RESP_MATCH_FOUND) {
                printf("Match found vs %s\n", resp.text);
                battle_loop(resp.room_id, resp.text);
                return;
            }
            if (resp.code == RESP_MATCH_BOT) {
                printf("Match found vs Wild Beast\n");
                battle_loop(resp.room_id, "Wild Beast");
                return;
            }
        } else if (errno != ENOMSG) {
            perror("msgrcv");
            return;
        }

        if (now_ms() - start > (long long)(MATCH_TIMEOUT_SEC + 5) * 1000LL) {
            printf("No response from server.\n");
            return;
        }

        usleep(100000);
    }
}

static void logout_user(void) {
    ArenaMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.cmd = CMD_LOGOUT;
    msg.user_index = g_user_index;
    send_request(&msg);

    ArenaResponse resp;
    if (recv_response(&resp, 0) >= 0) {
        printf("%s\n", resp.text);
        if (resp.status == 0) {
            g_user_index = -1;
        }
    }
}

static void user_menu(void) {
    while (g_user_index >= 0) {
        show_profile();
        printf("\n1. Battle\n2. Armory\n3. History\n4. Logout\n");
        int choice = read_int("Choice: ");
        switch (choice) {
            case 1:
                matchmaking();
                break;
            case 2:
                show_armory();
                break;
            case 3:
                show_history();
                break;
            case 4:
                logout_user();
                break;
            default:
                printf("Invalid choice.\n");
                break;
        }
    }
}

static void register_user(void) {
    char username[MAX_NAME];
    char password[MAX_PASS];

    read_line("Username: ", username, sizeof(username));
    read_line("Password: ", password, sizeof(password));

    ArenaMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.cmd = CMD_REGISTER;
    snprintf(msg.username, MAX_NAME, "%s", username);
    snprintf(msg.password, MAX_PASS, "%s", password);
    send_request(&msg);

    ArenaResponse resp;
    if (recv_response(&resp, 0) >= 0) {
        printf("%s\n", resp.text);
    }
}

static void login_user(void) {
    char username[MAX_NAME];
    char password[MAX_PASS];

    read_line("Username: ", username, sizeof(username));
    read_line("Password: ", password, sizeof(password));

    ArenaMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.cmd = CMD_LOGIN;
    snprintf(msg.username, MAX_NAME, "%s", username);
    snprintf(msg.password, MAX_PASS, "%s", password);
    send_request(&msg);

    ArenaResponse resp;
    if (recv_response(&resp, 0) >= 0) {
        printf("%s\n", resp.text);
        if (resp.status == 0 && resp.code == RESP_LOGIN_OK) {
            g_user_index = resp.user_index;
            user_menu();
        }
    }
}

static int connect_ipc(void) {
    g_shm_accounts_id = shmget(SHM_ACCOUNTS_KEY, sizeof(ArenaAccounts), 0666);
    g_shm_runtime_id = shmget(SHM_RUNTIME_KEY, sizeof(ArenaRuntime), 0666);
    g_shm_aux_id = shmget(SHM_AUX_KEY, sizeof(ArenaAux), 0666);
    if (g_shm_accounts_id < 0 || g_shm_runtime_id < 0 || g_shm_aux_id < 0) {
        return -1;
    }

    g_accounts = (ArenaAccounts *)shmat(g_shm_accounts_id, NULL, 0);
    g_runtime = (ArenaRuntime *)shmat(g_shm_runtime_id, NULL, 0);
    g_aux = (ArenaAux *)shmat(g_shm_aux_id, NULL, 0);
    if (g_accounts == (void *)-1 || g_runtime == (void *)-1 || g_aux == (void *)-1) {
        return -1;
    }

    g_semid = semget(SEM_KEY, 1, 0666);
    if (g_semid < 0) {
        return -1;
    }

    g_msgid = msgget(MSG_KEY, 0666);
    if (g_msgid < 0) {
        return -1;
    }

    return 0;
}

int main(void) {
    if (connect_ipc() != 0) {
        printf("Orion are you there?\n");
        return 1;
    }

    lock_sem();
    if (!g_runtime->server_ready) {
        unlock_sem();
        printf("Orion are you there?\n");
        return 1;
    }
    unlock_sem();

    while (1) {
        printf("\n=== ETERION GATE ===\n");
        printf("1. Register\n2. Login\n3. Exit\n");
        int choice = read_int("Choice: ");
        switch (choice) {
            case 1:
                register_user();
                break;
            case 2:
                login_user();
                break;
            case 3:
                printf("Bye.\n");
                return 0;
            default:
                printf("Invalid choice.\n");
                break;
        }
    }
}
