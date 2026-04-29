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
static int g_ui_ansi = 0;
static int g_in_alt_screen = 0;

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

static const char *ui(const char *code) {
    return g_ui_ansi ? code : "";
}

static void detect_ui_mode(void) {
    const char *term = getenv("TERM");
    g_ui_ansi = isatty(STDOUT_FILENO) && term && strcmp(term, "dumb") != 0;
}

static void sleep_ms(int ms) {
    struct timespec req;
    req.tv_sec = ms / 1000;
    req.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&req, NULL);
}

static int calc_base_damage(const Account *acc) {
    return BASE_DAMAGE + (acc->xp / 50) + acc->weapon_bonus;
}

static int calc_base_health(const Account *acc) {
    return BASE_HEALTH + (acc->xp / 10);
}

static const Weapon *get_equipped_weapon(int weapon_bonus) {
    const Weapon *equipped = NULL;
    int i;
    for (i = 0; i < MAX_WEAPONS; i++) {
        if (g_weapons[i].bonus <= weapon_bonus) {
            equipped = &g_weapons[i];
        }
    }
    return equipped;
}

static void clear_screen(void) {
    if (g_ui_ansi) {
        printf("\033[2J\033[H");
    } else {
        printf("\n");
    }
}

static void enter_alt_screen(void) {
    if (!g_ui_ansi || g_in_alt_screen) {
        return;
    }
    printf("\033[?1049h\033[?25l");
    fflush(stdout);
    g_in_alt_screen = 1;
}

static void leave_alt_screen(void) {
    if (!g_in_alt_screen) {
        return;
    }
    printf("\033[?25h\033[?1049l");
    fflush(stdout);
    g_in_alt_screen = 0;
}

static void print_banner(void) {
    printf("%s _____ _      _   _ __  __    _    ____  \n%s", ui("\033[33m"), ui("\033[0m"));
    printf("%s| ____| |    | | | |  \\/  |  / \\  |  _ \\ \n%s", ui("\033[33m"), ui("\033[0m"));
    printf("%s|  _| | |    | | | | |\\/| | / _ \\ | |_) |\n%s", ui("\033[33m"), ui("\033[0m"));
    printf("%s| |___| |___ | |_| | |  | |/ ___ \\|  _ < \n%s", ui("\033[36m"), ui("\033[0m"));
    printf("%s|_____|_____| \\___/|_|  |_/_/   \\_\\_| \\_\\\n%s", ui("\033[36m"), ui("\033[0m"));
    printf("%s                  EL UMAR%s\n\n", ui("\033[36m"), ui("\033[0m"));
}

static void draw_meter(const char *label, int current, int max, int width, char fill) {
    int i;
    int safe_max = (max > 0) ? max : 1;
    int safe_current = current;
    int filled;

    if (safe_current < 0) {
        safe_current = 0;
    }
    if (safe_current > safe_max) {
        safe_current = safe_max;
    }
    filled = (safe_current * width) / safe_max;

    printf("%-10s[", label);
    for (i = 0; i < width; i++) {
        putchar(i < filled ? fill : ' ');
    }
    printf("] %d/%d\n", safe_current, max);
}

static long long cooldown_left_ms(const BattleRoom *room, int my_side) {
    long long last_action = (my_side == 1) ? room->last_action_ms1 : room->last_action_ms2;
    long long elapsed;
    if (last_action <= 0) {
        return 0;
    }
    elapsed = now_ms() - last_action;
    if (elapsed >= COOLDOWN_MS) {
        return 0;
    }
    return COOLDOWN_MS - elapsed;
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

static void wait_enter(const char *prompt) {
    char dummy[8];
    read_line(prompt, dummy, sizeof(dummy));
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
    const Weapon *equipped = get_equipped_weapon(acc.weapon_bonus);
    int level = (acc.xp / 100) + 1;
    int dmg = calc_base_damage(&acc);
    int hp = calc_base_health(&acc);

    printf("%s+--------------------------- PROFILE ---------------------------+\n%s", ui("\033[36m"), ui("\033[0m"));
    printf("| Name : %-16s  Lvl : %-3d  Gold : %-5d |\n", acc.username, level, acc.gold);
    printf("| XP   : %-16d  Dmg : %-3d  HP   : %-5d |\n", acc.xp, dmg, hp);
    printf("| Weapon : %-52s |\n", equipped ? equipped->name : "None");
    printf("%s+---------------------------------------------------------------+\n%s", ui("\033[36m"), ui("\033[0m"));
}

static void show_history(void) {
    Account acc = get_account_snapshot(g_user_index);
    int i;

    clear_screen();
    print_banner();
    printf("%s-------------------- MATCH HISTORY --------------------%s\n", ui("\033[35m"), ui("\033[0m"));
    printf("%s%-8s %-12s %-6s %-8s %-8s%s\n", ui("\033[35m"), "Time", "Opponent", "Res", "XP", "Gold", ui("\033[0m"));
    if (acc.history_count == 0) {
        printf("No history yet.\n");
        wait_enter("Press ENTER...");
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
        printf("%-8s %-12s %s%-6s%s %+8d %+8d\n",
               tbuf,
               h->opponent,
               h->win ? ui("\033[32m") : ui("\033[31m"),
               h->win ? "WIN" : "LOSS",
               ui("\033[0m"),
               h->xp_gain,
               h->gold_gain);
    }
    wait_enter("\nPress ENTER...");
}

static void show_armory(void) {
    int i;
    Account acc = get_account_snapshot(g_user_index);
    clear_screen();
    print_banner();
    printf("%s------------------------ ARMORY ------------------------%s\n", ui("\033[33m"), ui("\033[0m"));
    printf("Gold: %d\n\n", acc.gold);
    for (i = 0; i < MAX_WEAPONS; i++) {
        printf("%d. %-12s  %4d G  %+4d Dmg\n", i + 1, g_weapons[i].name, g_weapons[i].cost, g_weapons[i].bonus);
    }
    printf("0. Back\n\n");

    {
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
    wait_enter("Press ENTER...");
}

static void show_logged_menu(void) {
    clear_screen();
    print_banner();
    show_profile();
    printf("\n%s+------------------ MENU ------------------+\n%s", ui("\033[36m"), ui("\033[0m"));
    printf("| 1. Battle                                |\n");
    printf("| 2. Armory                                |\n");
    printf("| 3. History                               |\n");
    printf("| 4. Logout                                |\n");
    printf("%s+------------------------------------------+\n%s", ui("\033[36m"), ui("\033[0m"));
}

static void show_guest_menu(void) {
    clear_screen();
    print_banner();
    printf("1. Register\n");
    printf("2. Login\n");
    printf("3. Exit\n");
}

static void show_matchmaking_screen(int sec_left) {
    clear_screen();
    print_banner();
    printf("%sSearching for an opponent... [%2ds]%s\n", ui("\033[31m"), sec_left, ui("\033[0m"));
}

static void show_battle_result(int is_win) {
    printf("\n");
    if (is_win) {
        printf("%s=== VICTORY ===%s\n", ui("\033[32m"), ui("\033[0m"));
    } else {
        printf("%s=== DEFEAT ===%s\n", ui("\033[31m"), ui("\033[0m"));
    }
    printf("Battle ended. Press ENTER to continue...");
    fflush(stdout);
}

static void render_battle_screen(const BattleRoom *room, int my_side, const char *opp_name, const Account *me) {
    const Weapon *equipped = get_equipped_weapon(me->weapon_bonus);
    int my_hp = (my_side == 1) ? room->hp1 : room->hp2;
    int my_max = (my_side == 1) ? room->max_hp1 : room->max_hp2;
    int opp_hp = (my_side == 1) ? room->hp2 : room->hp1;
    int opp_max = (my_side == 1) ? room->max_hp2 : room->max_hp1;
    long long cd_left = cooldown_left_ms(room, my_side);
    int i;

    if (g_ui_ansi) {
        printf("\033[H");
    } else {
        clear_screen();
    }
    printf("%s+------------------------------ ARENA ------------------------------+\n%s", ui("\033[31m"), ui("\033[0m"));
    printf("%s%-10s%s Lvl %-2d\n", ui("\033[36m"), opp_name, ui("\033[0m"), (me->xp / 100) + 1);
    draw_meter("Enemy", opp_hp, opp_max, UI_BAR_WIDTH, '=');
    printf("\n%sVS%s\n\n", ui("\033[35m"), ui("\033[0m"));
    printf("%s%-10s%s Lvl %-2d | Weapon: %s\n", ui("\033[36m"), me->username, ui("\033[0m"), (me->xp / 100) + 1, equipped ? equipped->name : "None");
    draw_meter("You", my_hp, my_max, UI_BAR_WIDTH, '=');
    printf("\n%sCombat Log:%s\n", ui("\033[33m"), ui("\033[0m"));
    for (i = 0; i < LOG_LINES; i++) {
        if (i < room->log_count) {
            int idx = (room->log_head + i) % LOG_LINES;
            printf("> %s\n", room->log[idx]);
        } else {
            printf("> \n");
        }
    }
    printf("\nCD: Atk(%.1fs) | Ult(%.1fs)\n",
           (double)cd_left / 1000.0,
           (me->weapon_bonus > 0) ? ((double)cd_left / 1000.0) : 0.0);
    printf("Action: [A] Attack");
    if (me->weapon_bonus > 0) {
        printf("  [U] Ultimate x3");
    } else {
        printf("  [U] Locked");
    }
    printf("\n");
    printf("%s+-------------------------------------------------------------------+\n%s", ui("\033[31m"), ui("\033[0m"));
    fflush(stdout);
}

static void send_battle_action(int room_id, int cmd) {
    ArenaMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.cmd = cmd;
    msg.user_index = g_user_index;
    msg.room_id = room_id;
    send_request(&msg);
}

static void battle_loop(int room_id, const char *opp_name) {
    Account acc = get_account_snapshot(g_user_index);
    int my_side = 1;
    int last_hp1 = -1, last_hp2 = -1;
    int last_log_head = -1, last_log_count = -1;
    int last_finished = -1, last_winner = -1;
    int last_cd_step = -1, last_side = -1;

    enable_raw_mode();
    enter_alt_screen();
    clear_screen();

    while (1) {
        BattleRoom room = get_room_snapshot(room_id);
        int need_render = 0;
        if (!room.in_use) {
            break;
        }

        if (room.p1 == g_user_index) {
            my_side = 1;
        } else if (room.p2 == g_user_index) {
            my_side = 2;
        }

        {
            int step_ms = g_ui_ansi ? 100 : 1000;
            int cd_step = (int)((cooldown_left_ms(&room, my_side) + step_ms - 1) / step_ms);
            if (room.hp1 != last_hp1 || room.hp2 != last_hp2 ||
                room.log_head != last_log_head || room.log_count != last_log_count ||
                room.finished != last_finished || room.winner != last_winner ||
                my_side != last_side || cd_step != last_cd_step) {
                need_render = 1;
            }
            if (need_render) {
                render_battle_screen(&room, my_side, opp_name, &acc);
                last_hp1 = room.hp1;
                last_hp2 = room.hp2;
                last_log_head = room.log_head;
                last_log_count = room.log_count;
                last_finished = room.finished;
                last_winner = room.winner;
                last_side = my_side;
                last_cd_step = cd_step;
            }
        }

        if (room.finished) {
            disable_raw_mode();
            leave_alt_screen();
            clear_screen();
            show_battle_result((room.winner == 1 && my_side == 1) || (room.winner == 2 && my_side == 2));
            wait_enter("");
            return;
        }

        fd_set set;
        struct timeval tv;
        FD_ZERO(&set);
        FD_SET(STDIN_FILENO, &set);
        tv.tv_sec = 0;
        tv.tv_usec = UI_REFRESH_MS * 1000;

        if (select(STDIN_FILENO + 1, &set, NULL, NULL, &tv) > 0) {
            char ch;
            if (read(STDIN_FILENO, &ch, 1) > 0) {
                if (cooldown_left_ms(&room, my_side) > 0) {
                    continue;
                }

                if (ch == 'a' || ch == 'A') {
                    send_battle_action(room_id, CMD_ATTACK);
                } else if (ch == 'u' || ch == 'U') {
                    if (acc.weapon_bonus <= 0) {
                        continue;
                    }
                    send_battle_action(room_id, CMD_ULT);
                }
            }
        }
    }

    disable_raw_mode();
    leave_alt_screen();
}

static void matchmaking(void) {
    ArenaMessage msg;
    int last_sec = -1;
    memset(&msg, 0, sizeof(msg));
    msg.cmd = CMD_MATCH;
    msg.user_index = g_user_index;
    send_request(&msg);

    long long start = now_ms();
    while (1) {
        ArenaResponse resp;
        long long elapsed_ms = now_ms() - start;
        int sec_left = MATCH_TIMEOUT_SEC - (int)(elapsed_ms / 1000LL);
        if (sec_left < 0) {
            sec_left = 0;
        }

        if (sec_left != last_sec) {
            show_matchmaking_screen(sec_left);
            last_sec = sec_left;
        }

        int r = recv_response(&resp, IPC_NOWAIT);
        if (r >= 0) {
            if (resp.status != 0) {
                printf("%s\n", resp.text);
                wait_enter("Press ENTER...");
                return;
            }

            if (resp.code == RESP_MATCH_FOUND) {
                battle_loop(resp.room_id, resp.text);
                return;
            }
            if (resp.code == RESP_MATCH_BOT) {
                battle_loop(resp.room_id, BOT_NAME);
                return;
            }
        } else if (errno != ENOMSG) {
            perror("msgrcv");
            wait_enter("Press ENTER...");
            return;
        }

        if (now_ms() - start > (long long)(MATCH_TIMEOUT_SEC + 5) * 1000LL) {
            printf("No response from server.\n");
            wait_enter("Press ENTER...");
            return;
        }

        sleep_ms(UI_REFRESH_MS);
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
        wait_enter("Press ENTER...");
    }
}

static void user_menu(void) {
    while (g_user_index >= 0) {
        show_logged_menu();
        int choice = read_int("> Choice: ");
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
    clear_screen();
    print_banner();
    printf("%sCREATE ACCOUNT%s\n", ui("\033[36m"), ui("\033[0m"));

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
        wait_enter("Press ENTER...");
    }
}

static void login_user(void) {
    char username[MAX_NAME];
    char password[MAX_PASS];
    clear_screen();
    print_banner();
    printf("%sLOGIN%s\n", ui("\033[36m"), ui("\033[0m"));

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
        if (resp.status != 0 || resp.code != RESP_LOGIN_OK) {
            wait_enter("Press ENTER...");
        }
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
    detect_ui_mode();

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
        show_guest_menu();
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
