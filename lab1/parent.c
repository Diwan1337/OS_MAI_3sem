#define _GNU_SOURCE
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
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
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        left -= (size_t)w;
        p += w;
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

// обрезать завершающий \n, если есть
static void chomp(char* s) {
    size_t n = strlen(s);
    if (n && s[n-1] == '\n') s[n-1] = '\0';
}

int main(int argc, char** argv, char** envp) {
    (void)argc; (void)argv;

    const char* prompt1 = "Введите имя файла: ";
    write_all(1, prompt1, strlen(prompt1));
    char fileName[512];
    ssize_t fnlen = read_line(0, fileName, sizeof(fileName));
    if (fnlen < 0) die("не удалось выполнить чтение (fileName)");
    if (fnlen == 0) die("имя файла не указано");
    chomp(fileName);

    int p1[2], p2[2];
    if (pipe(p1) < 0) die("pipe1 failed");
    if (pipe(p2) < 0) die("pipe2 failed");

    pid_t pid = fork();
    if (pid < 0) die("fork failed");

    if (pid == 0) {
        // child (запустит отдельную программу ./child)
        // p1: parent->child (stdin ребёнка)  => читаем с p1[0]
        // p2: child->parent (stdout ребёнка) => пишем в p2[1]
        if (dup2(p1[0], 0) < 0) die("dup2 p1->stdin failed");
        if (dup2(p2[1], 1) < 0) die("dup2 p2->stdout failed");

        // закрыть лишнее
        close(p1[0]); close(p1[1]);
        close(p2[0]); close(p2[1]);

        // argv для execve: child fileName
        char* args[3];
        args[0] = (char*)"child";
        args[1] = fileName;
        args[2] = NULL;

        // запускаем исполняемый файл "child" из текущего каталога
        execve("./child", args, envp);
        // если вернулись — ошибка
        die("execve ./child failed");
    }

    // parent
    close(p1[0]); // не читаем из pipe1
    close(p2[1]); // не пишем в pipe2

    const char* prompt2 =
        "Введите строку, например: \"12 -3 7\" и нажмите Ентер.\n"
        "Пустая строка для завершения.\n";
    write_all(1, prompt2, strlen(prompt2));

    char inbuf[2048];
    char outbuf[2048];

    for (;;) {
        write_all(1, "> ", 2);
        ssize_t n = read_line(0, inbuf, sizeof(inbuf));
        if (n < 0) die("read(user line) failed");
        if (n == 0) { // EOF
            // закрываем запись — ребёнок увидит EOF
            close(p1[1]);
            break;
        }

        // пустая строка — завершить
        if (inbuf[0] == '\n' || inbuf[0] == '\0') {
            close(p1[1]);
            break;
        }

        // отправляем строку ребёнку
        if (write_all(p1[1], inbuf, (size_t)n) < 0) die("не удалось выполнить запись в дочерний файл");

        // ждём ответ одной строкой и печатаем пользователю
        ssize_t m = read_line(p2[0], outbuf, sizeof(outbuf));
        if (m < 0) die("не удалось выполнить чтение из дочернего файла");
        if (m == 0) {
            write_all(1, "(child closed pipe)\n", 20);
            break;
        }
        write_all(1, outbuf, (size_t)m);
    }

    // дочитать всё, что осталось у ребёнка (на случай буфера)
    for (;;) {
        ssize_t m = read(p2[0], outbuf, sizeof(outbuf));
        if (m < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (m == 0) break;
        write_all(1, outbuf, (size_t)m);
    }

    close(p2[0]);
    close(p1[1]);

    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
