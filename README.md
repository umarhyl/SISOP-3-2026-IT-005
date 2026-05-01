# SISOP-3-2026-IT-005

|              |            |
| ------------ | ---------- |
| Nama         | Umar       |
| NRP          | 5027251005 |
| Kode Asisten | KENZ       |

# Struktur Repositori:

```SISOP-3-2026-IT-005/
├── soal_1/
│   ├── protocol.h
│   ├── wired.c
│   └── navi.c
├── soal_2/
│   ├── arena.h
│   ├── orion.c
│   ├── eternal.c
│   └── Makefile
├── assets/
└── README.md
```

# Reporting

## Soal 1 - Present Day, Present Time

Pada soal nomor 1 diminta untuk membangun sebuah infrastruktur komunikasi jaringan tersentralisasi (client-server) menggunakan TCP/IP Socket. Server diberi nama The Wired (wired.c) dan aplikasinya bernama NAVI (navi.c). Program ini harus memiliki beberapa spesifikasi:

1. Membuat protokol komunikasi via port yang ditentukan.

2. NAVI dapat mengirim pesan dan mendengarkan pesan masuk secara asinkron tanpa menggunakan fork().

3. Server berskala tinggi yang mampu mendeteksi banyak klien sekaligus secara mulus (multiplexing).

4. Penanganan identitas unik, di mana klien dengan nama yang sama akan ditolak.

5. Mekanisme Broadcast yang dikendalikan oleh server.

6. Entitas khusus pengelola jaringan (The Knights) yang menggunakan otentikasi password untuk mengakses fitur RPC (Remote Procedure Call) seperti mengecek entitas aktif, uptime server, dan mematikan server.

7. Implementasi sistem Logging permanen di history.log dengan penanda waktu.

Berikut bagian-bagian penyelesaian mendetail untuk soal 1:

1. Protokol Komunikasi (`protocol.h`)

Penyatuan kerangka data antara client dan server dibungkus dalam struct `DataPacket`.

```c
typedef enum {
    CMD_LOGIN,
    CMD_LOGIN_ADMIN,
    CMD_SUCCESS,
    // ... commands lainnya ...
    CMD_INFO
} CommandType;

typedef struct {
    CommandType cmd;
    char username[50];
    char text[BUF_SIZE];
} DataPacket;
```

Setiap data yang melintasi socket TCP akan berukuran pasti (seukuran `DataPacket`) dan memiliki header instruksi (`CommandType`). Ini membuat komunikasi sangat terstruktur. Identifikasi jenis permintaan dikontrol oleh field `cmd` alih-alih me-parsing string manual.

2. Client (`navi.c`)

Sesuai permintaan spesifikasi, `navi` dapat mendengarkan server dan menunggu input keyboard user dalam satu waktu secara asinkron (tanpa fungsi `fork`). Hal ini diwujudkan dengan teknik I/O Multiplexing (`select`).

```c
    while (is_running) {
        FD_ZERO(&read_fds);
        FD_SET(STDIN_FILENO, &read_fds); // File descriptor input keyboard
        FD_SET(client_socket, &read_fds); // File descriptor socket server

        if (select(client_socket + 1, &read_fds, NULL, NULL, NULL) < 0) break; 

        if (FD_ISSET(client_socket, &read_fds)) {
            // Menerima transmisi dari The Wired (Server)
        }
        if (FD_ISSET(STDIN_FILENO, &read_fds)) {
            // Menerima input ketikan dari pengguna (Keyboard)
        }
    }
```

Logikanya program menggunakan makro `FD_SET` untuk mendaftarkan Standard Input (keyboard) dan soket server ke dalam satu himpunan (set). Fungsi `select()` akan memblokir (menahan) eksekusi sampai ada salah satu kanal yang mengirim data. Jika server mengirim pesan (seperti pesan obrolan orang lain), blok pertama dieksekusi. Jika user mengetik, blok kedua dieksekusi.

3. Server (`wired.c`)

Sama seperti client, server juga tidak mengandalkan multithreading atau multi-processing (`fork`), melainkan menggunakan `select()`.

```c
        read_fds = fd_pool;
        if (select(fd_max + 1, &read_fds, NULL, NULL, NULL) < 0) continue;

        for (int i = 0; i <= fd_max; i++) {
            if (!FD_ISSET(i, &read_fds)) continue;

            if (i == server_fd) {
                // Menangani koneksi NAVI baru
                client_fd = accept(server_fd, NULL, NULL);
                FD_SET(client_fd, &fd_pool);
                // ... simpan ke user_list
            } else {
                // Menangani pesan, perintah, atau diskoneksi dari NAVI yang sudah ada
                int recv_size = recv(i, &paket, sizeof(DataPacket), 0);
                if (recv_size <= 0 || paket.cmd == CMD_QUIT) {
                    // Diskoneksi bersih
                }
            }
        }
```

Loop utama pada server memeriksa setiap kemungkinan jalur data. Jika aliran data masuk berasal dari `server_fd` (master socket yang me-listen), artinya ada klien baru mendaftar (`accept`). Jika berasal dari file descriptor yang lain (`i`), artinya itu adalah data masuk atau sinyal diskoneksi dari klien yang sudah terhubung sebelumnya.

4. Check Nama Unik

```c
int check_duplicate_name(const char* name) {
    for (int i = 0; i < MAX_USERS; i++) {
        if (user_list[i].is_logged_in && strcmp(user_list[i].uname, name) == 0) {
            return 1;
        }
    }
    return 0;
}
```

Saat menerima command `CMD_LOGIN`, `wired` memanggil fungsi `check_duplicate_name()`. Jika identitas tersebut telah tersinkronisasi di The Wired, server akan merespon dengan `CMD_FAILED` sehingga `navi` akan ditolak untuk bergabung.

5. Mekanisme Broadcast

```c
void send_to_all(DataPacket *paket, int sender_socket) {
    for (int i = 0; i < MAX_USERS; i++) {
        if (user_list[i].socket_fd != 0 && 
            user_list[i].socket_fd != sender_socket && 
            user_list[i].is_logged_in) {
            send(user_list[i].socket_fd, paket, sizeof(DataPacket), 0);
        }
    }
}
```

Jika `wired` menerima `CMD_MSG`, fungsi ini akan iterasi melalui `user_list` dan memanggil fungsi `send()` ke semua pengguna yang dalam mode logged_in, kecuali soket pengirim itu sendiri (`sender_socket`).

6. Admin "The Knights"

Apabila user menggunakan identitas "The Knights", program akan meminta kata sandi: "protocol7". Ini membuka akses RPC yang khusus diterjemahkan di dalam server.

```c
                    case CMD_REQ_UPTIME:
                        if (user_list[idx].is_admin) {
                            write_log("Admin", "RPC_GET_UPTIME");
                            DataPacket rep;
                            rep.cmd = CMD_INFO;
                            sprintf(rep.text, "Server uptime: %ld seconds", time(NULL) - time_started);
                            send(i, &rep, sizeof(DataPacket), 0);
                        }
                        break;
```

Pesan perintah dengan header khusus (`CMD_REQ_UPTIME`, `CMD_REQ_USERS`, `CMD_HALT`) tidak dimasukkan ke jalur broadcast obrolan. Server mencegat sinyal ini, memverifikasi `is_admin`, dan mengirim balasan secara private hanya ke soket admin yang memintanya dalam bentuk `CMD_INFO`.

7. Logging

```c
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
```

Setiap koneksi baru, pesan ter-broadcast, panggilan RPC, maupun diskoneksi (`/exit` atau putus koneksi mendadak) akan memicu fungsi `write_log`. Fungsi ini menulis string dengan mode append (`"a"`) bersama penanda waktu ke dalam `history.log`.

#### Output

![Output Soal 1](assets/image.png)

---

## Soal 2 - The Eternal of Eterion

Pada soal nomor 2 diminta untuk membuat program yang menyimulasikan arena pertempuran bernama Eterion. Program ini menggunakan mekanisme IPC (Inter-Process Communication) untuk saling berkomunikasi, dengan beberapa tugas dan ketentuan khusus, yaitu:

1. Membuat file arena.h sebagai pusat konfigurasi, struct, dan kunci (key) Shared Memory.

2. Membuat program orion.c yang bertindak sebagai server untuk memproses logika pertempuran.

3. Membuat program eternal.c yang bertindak sebagai client (disebut prajurit).
4. Menyediakan sistem registrasi dan login prajurit secara persisten menggunakan Shared Memory.

5. Setiap akun baru akan mendapatkan status awal: Gold 150, Lvl 1, dan XP 0.

6. Mengimplementasikan sistem Matchmaking selama 35 detik. Jika tidak ada lawan, pemain akan melawan monster (bot) bernama "Wild Beast".

7. Pertempuran berlangsung secara Realtime Asynchronous (bukan turn-based). Penyerangan dilakukan dengan menekan tombol a (Attack) atau u (Ultimate) dengan cooldown 1 detik.

8. Menghitung reward (XP dan Gold) setelah pertempuran usai, dengan sistem level yang meningkat tiap kelipatan 100 XP.

Berikut bagian-bagian penyelesaian soal 2:

1. Struktur Data Shared Memory (`arena.h`)

```c
typedef struct {
    Account users[MAX_USERS];
} ArenaAccounts;

typedef struct {
    int magic;
    int server_ready;
    pid_t server_pid;
    int waiting_count;
    int waiting_users[MAX_MATCH_QUEUE];
    long long waiting_since[MAX_MATCH_QUEUE];
    BattleRoom rooms[MAX_ROOMS];
} ArenaRuntime;
```

Terdapat pemisahan memori yang jelas antara ArenaAccounts (untuk data persisten prajurit seperti XP, Gold, status login) dan ArenaRuntime (untuk state server yang terus berubah secara volatil seperti antrean matchmaking dan status BattleRoom). Pemisahan blok memori ini memudahkan manajemen memori dan menjaga persistensi akun meskipun terjadi reset pertempuran.

2. Inisialisasi dan Koneksi IPC
```c
    g_shm_accounts_id = shmget(SHM_ACCOUNTS_KEY, sizeof(ArenaAccounts), IPC_CREAT | 0666);
    g_shm_runtime_id = shmget(SHM_RUNTIME_KEY, sizeof(ArenaRuntime), IPC_CREAT | 0666);
    g_shm_aux_id = shmget(SHM_AUX_KEY, sizeof(ArenaAux), IPC_CREAT | 0666);
    
    g_accounts = (ArenaAccounts *)shmat(g_shm_accounts_id, NULL, 0);
    g_runtime = (ArenaRuntime *)shmat(g_shm_runtime_id, NULL, 0);
    
    g_semid = semget(SEM_KEY, 1, IPC_CREAT | 0666);
    g_msgid = msgget(MSG_KEY, IPC_CREAT | 0666);
```

Logikanya adalah `orion` membuat memori bersama menggunakan `shmget` dengan parameter `IPC_CREAT` (alokasi di level sistem operasi). Kemudian `orion` dan `eternal` mengaitkan (attach) memori tersebut ke address space lokal mereka menggunakan `shmat`.

- Message Queue (`msgget`): Digunakan untuk saling mengirim perintah (request/response) dengan struct `ArenaMessage` yang terstruktur (memiliki field `cmd`, `user_index`, dll).

- Semaphore (`semget`): Digunakan sebagai pengunci akses (mutex/lock).
Jika `eternal` dijalankan sebelum `orion` siap (mengecek status g_runtime->server_ready), maka program akan langsung mencetak error `Orion are you there?`.

3. Sinkronisasi Data Mencegah Race Condition (Semaphore)

Karena `orion` memproses request dari banyak `eternal` (klien), operasi baca-tulis harus dikunci agar tidak bentrok.

```c
static void lock_sem(void) {
    struct sembuf op = {0, -1, SEM_UNDO};
    semop(g_semid, &op, 1);
}

static void unlock_sem(void) {
    struct sembuf op = {0, 1, SEM_UNDO};
    semop(g_semid, &op, 1);
}
```

Jadi sebelum membaca pesan (`msgrcv`) dari Message Queue dan memodifikasi health points (HP) atau antrean, `orion` akan memanggil `lock_sem()`. Ini akan mengurangi nilai semaphore menjadi 0 sehingga process lain yang mencoba mengakses memori yang sama harus menunda eksekusinya. Setelah pembaruan state selesai (misal setelah kalkulasi damage), server memanggil `unlock_sem()` agar memori dapat diakses kembali. `SEM_UNDO` memastikan bahwa jika proses `orion` tiba-tiba crash, sistem operasi otomatis melepaskan kuncian (lock) tersebut.

4. Autentikasi Prajurit (Register & Login)

```c
static void handle_register(ArenaMessage *msg) {
    if (find_user_index(msg->username) >= 0) {
        send_response(msg->pid, 1, RESP_ERR, -1, -1, "Username already exists");
        return;
    }
    // create_user(msg->username, msg->password)
    send_response(msg->pid, 0, RESP_REGISTER_OK, -1, -1, "Register success");
}
```

`Orion` menerima struktur pesan (`msg.cmd = CMD_REGISTER`) dari `eternal`. Server memvalidasi apakah nama pengguna (username) unik dengan mencarinya melalui iterasi di dalam array `g_accounts->users` pada Shared Memory. Jika unik, `create_user` dipanggil dan memberikan nilai inisial Lvl 1 (XP 0) dan Gold 150. Login (CMD_LOGIN) bekerja serupa dengan mencocokkan password dan mencegah login ganda melalui validasi `acc->active_pid != 0`.

4. Sistem Matchmaking dan Bot (Wild Beast)

```c
static void match_timeout_tick(void) {
    long long now = now_ms();
    int i = 0;

    while (i < g_runtime->waiting_count) {
        long long started = g_runtime->waiting_since[i];
        if (now - started < (long long)MATCH_TIMEOUT_SEC * 1000LL) {
            i++;
            continue;
        }
        
        int room_idx = find_free_room(now);
        setup_room(&g_runtime->rooms[room_idx], room_idx, user_index, -1);
        
        send_response(pid, 0, RESP_MATCH_BOT, user_index, room_idx, "Match found vs Wild Beast");
        remove_from_queue(user_index);
    }
}
```

Pemain yang menekan menu "Battle" akan didaftarkan ke array `waiting_users`. Server secara continuous memanggil fungsi background `match_timeout_tick()` di dalam while loop utamanya. Jika selisih waktu antrean melebihi 35 detik (`MATCH_TIMEOUT_SEC`), prajurit dialihkan ke BattleRoom kosong melawan entitas -1 (bot). Logika Bot dijalankan melalui fungsi `bot_tick()` yang akan mengurangi HP prajurit secara acak antara 8-12 damage setiap detiknya jika lawannya adalah bot.

6. Asynchronous (Tanpa Turn-Based)

Untuk membuat pertarungan murni berjalan secara real-time tanpa mengharuskan prajurit menekan tombol 'Enter', eternal.c memodifikasi flag terminal.

```c
static void enable_raw_mode(void) {
    struct termios raw;
    tcgetattr(STDIN_FILENO, &g_orig_term);
    raw = g_orig_term;
    raw.c_lflag &= (unsigned long)~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    g_orig_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, g_orig_flags | O_NONBLOCK);
}

```
Fungsi ini menonaktifkan mode `ICANON` (canonical) dan `ECHO` pada sistem terminal POSIX, mengubahnya menjadi Raw Mode. Dipadukan dengan flag `O_NONBLOCK`, input keyboard dapat langsung terbaca character-by-character menggunakan fungsi multiplexing `select()` dengan timeout hanya `100ms` (pendekatan rendering game loop). Jika ditekan tombol `a` (Attack) atau `u` (Ultimate) dan waktu `cooldown` telah terpenuhi (1 detik), klien segera menembakkan pesan perintah `CMD_ATTACK/CMD_ULT` ke server.

7. Reward (XP & Gold) dan Armory

Setelah pertarungan usai dengan cek room->hp1 <= 0 atau room->hp2 <= 0, fungsi finish_battle() akan menentukan reward.

Penjelasan Reward: Pemenang menerima stat tinggi yang akan memengaruhi kalkulasi damage dan health base menggunakan rumus bawaan (Lvl = XP/100 + 1). History pertandingan juga direkam menggunakan circular list di fungsi append_history().

Pembelian Senjata (Armory): Prajurit dapat membeli senjata untuk meningkatkan bonus demage dan membuka skill ultimate (hanya terbuka jika punya senjata).

```c
static void finish_battle(BattleRoom *room, int winner_side) {
    // Menang: +50 XP, +120 Gold
    // Kalah: +15 XP, +30 Gold
    if (winner_side == 1) {
        acc->xp += 50;
        acc->gold += 120;
        append_history(acc, opp, 1, 50, 120);
    } else {
        acc->xp += 15;
        acc->gold += 30;
        append_history(acc, opp, 0, 15, 30);
    }
}
```

```c
static void handle_buy(ArenaMessage *msg) {
    const Weapon *w = &g_weapons[choice];
    if (acc->gold < w->cost) {
        send_response(msg->pid, 1, RESP_BUY_FAIL, idx, -1, "Not enough gold");
        return;
    }
    acc->gold -= w->cost;
    acc->weapon_bonus = w->bonus; // Status tersimpan di shared memory
    send_response(msg->pid, 0, RESP_BUY_OK, idx, -1, "Purchase success");
}
```

Gold yang mencukupi akan dikurangi dan nilai weapon_bonus milik klien dimodifikasi secara permanen, memungkinkan kalkulator penyerangan di dalam server menambahkan damage point tambahan untuk klien tersebut ke depannya.

Untuk script lengkap dapat dilihat pada file [`soal_2/orion.c`](soal_2/orion.c) dan [`soal_2/eternal.c`](soal_2/eternal.c).