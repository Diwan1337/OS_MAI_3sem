#define _GNU_SOURCE
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

#define BUF_IN_SIZE  2048
#define BUF_OUT_SIZE 2048

// ======= утилиты I/O (как в ЛР1) =======
static void die(const char* msg) {
    ssize_t _ __attribute__((unused)) = write(2, msg, strlen(msg));
    ssize_t __ __attribute__((unused)) = write(2, "\n", 1);
    _exit(1);
}

static ssize_t write_all(int fd, const void* buf, size_t n) {
    const char* p = (const char*)buf;
    size_t left = n;
    while (left) {
        ssize_t w = write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        left -= (size_t)w; p += w;
    }
    return (ssize_t)n;
}

// читает одну строку (заканчивается '\n' или EOF). Возвращает длину, 0 при EOF.
static ssize_t read_line(int fd, char* buf, size_t cap) {
    size_t i = 0;
    while (i + 1 < cap) {
        char c;
        ssize_t r = read(fd, &c, 1);
        if (r == 0) break;         // EOF
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        buf[i++] = c;
        if (c == '\n') break;
    }
    buf[i] = '\0';
    return (ssize_t)i;
}

static void chomp(char* s) {
    size_t n = strlen(s);
    if (n && s[n-1] == '\n') s[n-1] = '\0';
}

// ======= структура обмена через shared memory =======
struct shm_data {
    // Родитель пишет в in_buf, ребенок отвечает в out_buf.
    // Семафоры обеспечивают порядок, поэтому отдельные флаги не нужны.
    char in_buf[BUF_IN_SIZE];
    char out_buf[BUF_OUT_SIZE];
};

// ======= main =======
int main(int argc, char** argv, char** envp) {
    (void)argc; (void)argv;

    // 1) спросить имя выходного файла (как в ЛР1)
    const char* prompt1 = "Введите имя файла: ";
    write_all(1, prompt1, strlen(prompt1));
    char fileName[512];
    ssize_t fnlen = read_line(0, fileName, sizeof(fileName));
    if (fnlen < 0) die("не удалось выполнить чтение (fileName)");
    if (fnlen == 0) die("имя файла не указано");
    chomp(fileName);

    // 2) придумать уникальные имена объектов (обязательно по условию)
    pid_t pid_self = getpid();
    char shm_name[128], sem_p_name[128], sem_c_name[128];
    // POSIX требует, чтобы имя shm/sem начиналось с '/'
    // Пример: /shm_sum_12345, /sem_p_12345, /sem_c_12345
    {
        // простое формирование строк без snprintf из stdio.h
        char pidbuf[32]; int k = 0;
        // int -> строка
        {
            long long v = pid_self; char tmp[32]; int n = 0;
            if (v == 0) tmp[n++] = '0';
            while (v > 0) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
            // reverse
            if (n == 0) { pidbuf[k++] = '0'; }
            else { for (int i = n - 1; i >= 0; --i) pidbuf[k++] = tmp[i]; }
            pidbuf[k] = '\0';
        }
        const char* a = "/shm_sum_";   size_t na = strlen(a);
        const char* b = "/sem_p_";     size_t nb = strlen(b);
        const char* c = "/sem_c_";     size_t nc = strlen(c);

        memcpy(shm_name, a, na); memcpy(shm_name + na, pidbuf, k); shm_name[na + k] = '\0';
        memcpy(sem_p_name, b, nb); memcpy(sem_p_name + nb, pidbuf, k); sem_p_name[nb + k] = '\0';
        memcpy(sem_c_name, c, nc); memcpy(sem_c_name + nc, pidbuf, k); sem_c_name[nc + k] = '\0';
    }

    // 3) создать shared memory
    int shm_fd = shm_open(shm_name, O_CREAT | O_RDWR, 0666);
    if (shm_fd < 0) die("shm_open failed");
    if (ftruncate(shm_fd, sizeof(struct shm_data)) < 0) die("ftruncate failed");

    void* map = mmap(NULL, sizeof(struct shm_data), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (map == MAP_FAILED) die("mmap failed");
    struct shm_data* shm = (struct shm_data*)map;

    // 4) создать два семафора:
    //    - sem_parent: родитель -> ребенок (данные готовы к чтению)
    //    - sem_child : ребенок -> родитель (ответ готов)
    sem_t* sem_parent = sem_open(sem_p_name, O_CREAT, 0666, 0);
    if (sem_parent == SEM_FAILED) die("sem_open(sem_parent) failed");
    sem_t* sem_child  = sem_open(sem_c_name, O_CREAT, 0666, 0);
    if (sem_child == SEM_FAILED) die("sem_open(sem_child) failed");

    // 5) запустить child через execve и передать имена объектов + имя файла
    pid_t pid = fork();
    if (pid < 0) die("fork failed");
    if (pid == 0) {
        // argv: child_shm <fileName> <shm_name> <sem_p_name> <sem_c_name>
        char* args[6];
        args[0] = (char*)"child_shm";
        args[1] = fileName;
        args[2] = shm_name;
        args[3] = sem_p_name;
        args[4] = sem_c_name;
        args[5] = NULL;
        execve("./child_shm", args, envp);
        die("execve ./child_shm failed");
    }

    // 6) основной цикл: читать у пользователя -> в shared memory -> сигнал ребенку -> ждать ответ -> печатать
    const char* prompt2 =
        "Введите строку, например: \"12 -3 7\" и нажмите Ентер.\n"
        "Пустая строка для завершения.\n";
    write_all(1, prompt2, strlen(prompt2));

    for (;;) {
        write_all(1, "> ", 2);
        ssize_t n = read_line(0, shm->in_buf, sizeof(shm->in_buf));
        if (n < 0) die("read(user line) failed");

        // EOF: посылаем пустую строку как сигнал завершения
        if (n == 0 || shm->in_buf[0] == '\n' || shm->in_buf[0] == '\0') {
            shm->in_buf[0] = '\0';
            sem_post(sem_parent); // дать ребёнку понять, что пора завершаться
            break;
        }

        // отдать строку ребенку
        if (sem_post(sem_parent) < 0) die("sem_post(parent) failed");

        // ждать ответа
        if (sem_wait(sem_child) < 0) die("sem_wait(child) failed");

        // вывести ответ
        write_all(1, shm->out_buf, strlen(shm->out_buf));
    }

    // дочитать финальные сообщения, если ребенок что-то допишет (здесь не требуется)

    int status = 0;
    waitpid(pid, &status, 0);

    // 7) уборка
    munmap(shm, sizeof(struct shm_data));
    close(shm_fd);
    sem_close(sem_parent);
    sem_close(sem_child);
    sem_unlink(sem_p_name);
    sem_unlink(sem_c_name);
    shm_unlink(shm_name);

    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
