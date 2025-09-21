#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

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

static ssize_t read_line(int fd, char* buf, size_t cap) {
    size_t i = 0;
    while (i + 1 < cap) {
        char c;
        ssize_t r = read(fd, &c, 1);
        if (r == 0) break; // EOF
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        buf[i++] = c;
        if (c == '\n') break;
    }
    buf[i] = '\0';
    return (ssize_t)i;
}

// простой парсер long long (десятичный)
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

    // проверка на . или ,
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

// целое -> строка; пишет прямо в buf, возвращает длину
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

int main(int argc, char** argv) {
    if (argc < 2) die("дочь: требуется аргумент имени файла");
    const char* fileName = argv[1];

    int fd = open(fileName, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) die("child: open(file) failed");

    char line[2048];

    for (;;) {
        ssize_t n = read_line(0, line, sizeof(line));
        if (n < 0) die("дочь: не удалось прочитать строку");
        if (n == 0) break;                 // EOF
        if (line[0] == '\n' || line[0] == '\0') break; // пустая строка — конец

        size_t i = 0;
        int found_any = 0;
        long long sum = 0;
        long long val;

        while (1) {
            int st = parse_ll(line, &i, &val);
            if (st == 1) {
                found_any = 1;
                sum += val;
            } else if (st == -1) {
                const char* msg = "ERR: invalid number format\n";
                write_all(1, msg, strlen(msg));
                write_all(fd, msg, strlen(msg));
                found_any = 0; // сбросим, чтобы не писать результат
                break;
            } else {
                break; // st == 0 → конец строки
            }
        }


        if (!found_any) {
            const char* msg = "Бро, ошибка, тут числа нет либо что-то чужеродное\n";
            write_all(1, msg, strlen(msg));
            write_all(fd, msg, strlen(msg));
            continue;
        }

        char out[128];
        int k = 0;
        memcpy(out + k, "sum=", 4);  k += 4;
        k += ll_to_buf(sum, out + k);
        out[k++] = '\n';

        write_all(1, out, (size_t)k);   // в parent
        write_all(fd, out, (size_t)k);  // в файл
    }


    close(fd);
    return 0;
}
