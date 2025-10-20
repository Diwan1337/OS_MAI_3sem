#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

#define BUF_IN_SIZE  2048
#define BUF_OUT_SIZE 2048

// ===== утилиты I/O (как в ЛР1) =====
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
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        left -= (size_t)w; p += w;
    }
    return (ssize_t)n;
}

// ---- парсер целых и int -> строка (как в твоей child.c) ----
static int parse_ll(const char* s, size_t* i, long long* out) {
    while (s[*i] == ' ' || s[*i] == '\t' || s[*i] == '\r') (*i)++;

    int sign = 1;
    if (s[*i] == '+' || s[*i] == '-') {
        if (s[*i] == '-') sign = -1;
        (*i)++;
    }

    if (s[*i] < '0' || s[*i] > '9') return 0;

    long long val = 0;
    int found_digit = 0;
    while (s[*i] >= '0' && s[*i] <= '9') {
        int d = s[*i] - '0';
        val = val * 10 + d;
        (*i)++;
        found_digit = 1;
    }

    // запрет дробных: если . или , — считаем ошибкой, как в ЛР1
    if (s[*i] == '.' || s[*i] == ',') {
        *out = 0;
        while (s[*i] && s[*i] != ' ' && s[*i] != '\n') (*i)++;
        errno = EINVAL;
        return -1;
    }

    if (!found_digit) return 0;
    *out = val * sign;
    return 1;
}

static int ll_to_buf(long long v, char* buf) {
    char tmp[32];
    int neg = v < 0;
    unsigned long long x = neg ? (unsigned long long)(-(v+1)) + 1ULL : (unsigned long long)v;
    int n = 0;
    do {
        tmp[n++] = (char)('0' + (x % 10ULL));
        x /= 10ULL;
    } while (x);
    int k = 0;
    if (neg) buf[k++] = '-';
    for (int i = n - 1; i >= 0; --i) buf[k++] = tmp[i];
    return k;
}

// ===== структура shared memory =====
struct shm_data {
    char in_buf[BUF_IN_SIZE];
    char out_buf[BUF_OUT_SIZE];
};

int main(int argc, char** argv) {
    if (argc < 5) die("usage: child_shm <fileName> <shm_name> <sem_p_name> <sem_c_name>");

    const char* fileName   = argv[1];
    const char* shm_name   = argv[2];
    const char* sem_p_name = argv[3];
    const char* sem_c_name = argv[4];

    // открыть файл на дозапись (как в ЛР1)
    int fd = open(fileName, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) die("child: open(file) failed");

    // подключиться к shared memory и семафорам
    int shm_fd = shm_open(shm_name, O_RDWR, 0666);
    if (shm_fd < 0) die("child: shm_open failed");

    void* map = mmap(NULL, sizeof(struct shm_data), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (map == MAP_FAILED) die("child: mmap failed");
    struct shm_data* shm = (struct shm_data*)map;

    sem_t* sem_parent = sem_open(sem_p_name, 0);
    if (sem_parent == SEM_FAILED) die("child: sem_open(parent) failed");
    sem_t* sem_child  = sem_open(sem_c_name, 0);
    if (sem_child == SEM_FAILED) die("child: sem_open(child) failed");

    // рабочий цикл: ждать строку -> посчитать сумму -> отдать ответ
    for (;;) {
        if (sem_wait(sem_parent) < 0) die("child: sem_wait(parent) failed");

        // сигнал на завершение: пустая строка
        if (shm->in_buf[0] == '\0' || shm->in_buf[0] == '\n') {
            break;
        }

        // разбор строки и сумма
        size_t i = 0;
        int found_any = 0;
        long long sum = 0;
        long long val;

        while (1) {
            int st = parse_ll(shm->in_buf, &i, &val);
            if (st == 1) {
                found_any = 1;
                sum += val;
            } else if (st == -1) {
                const char* msg = "ERR: invalid number format\n";
                // вернуть ответ в shared memory
                size_t L = strlen(msg);
                memcpy(shm->out_buf, msg, L+1);
                // продублировать в файл (как в ЛР1)
                write_all(fd, msg, L);
                // сообщить родителю
                sem_post(sem_child);
                found_any = 0;
                goto next_iter;
            } else {
                break; // конец токенов
            }
        }

        if (!found_any) {
            const char* msg = "Бро, ошибка, тут числа нет либо что-то чужеродное\n";
            size_t L = strlen(msg);
            memcpy(shm->out_buf, msg, L+1);
            write_all(fd, msg, L);
            sem_post(sem_child);
            goto next_iter;
        }

        // подготовить "sum=<value>\n"
        {
            char out[128];
            int k = 0;
            memcpy(out + k, "sum=", 4); k += 4;
            k += ll_to_buf(sum, out + k);
            out[k++] = '\n';

            // в shared memory
            if ((size_t)k >= BUF_OUT_SIZE) k = (int)BUF_OUT_SIZE - 1;
            memcpy(shm->out_buf, out, (size_t)k);
            shm->out_buf[k] = '\0';

            // в файл
            write_all(fd, out, (size_t)k);
        }

        // отдать сигнал родителю: ответ готов
        sem_post(sem_child);

    next_iter:
        ;
    }

    // финал
    munmap(shm, sizeof(struct shm_data));
    close(shm_fd);
    sem_close(sem_parent);
    sem_close(sem_child);
    close(fd);
    return 0;
}
