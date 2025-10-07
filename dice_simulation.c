#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <sys/mman.h>  // Для mmap/munmap

// Простая функция для преобразования числа в строку
static int int_to_str(long num, char *buf, int buf_size) {
    int i = 0;
    int is_negative = 0;
    
    if (num < 0) {
        is_negative = 1;
        num = -num;
    }
    
    if (num == 0) {
        buf[i++] = '0';
    } else {
        char temp[32];
        int j = 0;
        while (num > 0 && j < 32) {
            temp[j++] = '0' + (num % 10);
            num /= 10;
        }
        if (is_negative) temp[j++] = '-';
        while (j > 0 && i < buf_size - 1) {
            buf[i++] = temp[--j];
        }
    }
    buf[i] = '\0';
    return i;
}

// Простая функция для преобразования строки в число
static long str_to_long(const char *str) {
    long result = 0;
    int sign = 1;
    
    if (*str == '-') {
        sign = -1;
        str++;
    }
    
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }
    
    return result * sign;
}

// Функция для вывода строки через системный вызов write
static void print_str(const char *str) {
    ssize_t result = write(STDOUT_FILENO, str, strlen(str));
    (void)result; // Подавляем предупреждение
}

// Функция для вывода числа
static void print_num(long num) {
    char buf[32];
    int_to_str(num, buf, 32);
    print_str(buf);
}

// Функция для вывода double (упрощенно)
static void print_double(double num, int precision) {
    long int_part = (long)num;
    print_num(int_part);
    print_str(".");
    
    double frac_part = num - int_part;
    if (frac_part < 0) frac_part = -frac_part;
    
    for (int i = 0; i < precision; i++) {
        frac_part *= 10;
        int digit = (int)frac_part;
        print_num(digit);
        frac_part -= digit;
    }
}

typedef struct {
    size_t thread_id;
    size_t experiments_per_thread;
    int K;
    int current_round;
    int player1_score;
    int player2_score;
    size_t local_player1_wins;
    size_t local_player2_wins;
    size_t local_draws;
} ThreadArgs;

// Простой линейный конгруэнтный генератор вместо rand_r
static unsigned int my_rand(unsigned int *seed) {
    *seed = (*seed * 1103515245 + 12345) & 0x7fffffff;
    return *seed;
}

// Функция броска двух костей
static int roll_two_dice(unsigned int *seed) {
    return (my_rand(seed) % 6 + 1) + (my_rand(seed) % 6 + 1);
}

// Функция симуляции одной игры
static void simulate_game(int K, int current_round, int p1_score, int p2_score,
                   int *p1_wins, int *p2_wins, int *draws, unsigned int *seed) {
    int player1 = p1_score;
    int player2 = p2_score;
    
    for (int round = current_round; round < K; round++) {
        player1 += roll_two_dice(seed);
        player2 += roll_two_dice(seed);
    }
    
    if (player1 > player2) (*p1_wins)++;
    else if (player2 > player1) (*p2_wins)++;
    else (*draws)++;
}

// Рабочая функция потока
static void *worker_thread(void *_args) {
    ThreadArgs *args = (ThreadArgs *)_args;
    unsigned int seed = (unsigned int)time(NULL) ^ (args->thread_id << 16);
    
    for (size_t i = 0; i < args->experiments_per_thread; i++) {
        int p1_wins = 0, p2_wins = 0, draws = 0;
        simulate_game(args->K, args->current_round, 
                     args->player1_score, args->player2_score,
                     &p1_wins, &p2_wins, &draws, &seed);
        
        args->local_player1_wins += p1_wins;
        args->local_player2_wins += p2_wins;
        args->local_draws += draws;
    }
    
    return NULL;
}

// Последовательная версия
static double sequential_monte_carlo(int K, int current_round, int p1_score, 
                             int p2_score, size_t num_experiments) {
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    unsigned int seed = (unsigned int)time(NULL);
    size_t p1_wins = 0, p2_wins = 0, draws = 0;
    
    for (size_t i = 0; i < num_experiments; i++) {
        int p1 = 0, p2 = 0, d = 0;
        simulate_game(K, current_round, p1_score, p2_score, &p1, &p2, &d, &seed);
        p1_wins += p1;
        p2_wins += p2;
        draws += d;
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    double time_ms = (end.tv_sec - start.tv_sec) * 1000.0 + 
                     (end.tv_nsec - start.tv_nsec) / 1000000.0;
    
    print_str("Player 1 wins: ");
    print_double(100.0 * p1_wins / num_experiments, 2);
    print_str("%\n");
    
    print_str("Player 2 wins: ");
    print_double(100.0 * p2_wins / num_experiments, 2);
    print_str("%\n");
    
    print_str("Draws: ");
    print_double(100.0 * draws / num_experiments, 2);
    print_str("%\n");
    
    return time_ms;
}

// Параллельная версия
static double parallel_monte_carlo(int K, int current_round, int p1_score, 
                           int p2_score, size_t num_experiments, 
                           size_t num_threads) {
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    // Аллокация через mmap вместо malloc
    pthread_t *threads = mmap(NULL, num_threads * sizeof(pthread_t),
                              PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ThreadArgs *args = mmap(NULL, num_threads * sizeof(ThreadArgs),
                            PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    
    size_t experiments_per_thread = num_experiments / num_threads;
    size_t remainder = num_experiments % num_threads;
    
    for (size_t i = 0; i < num_threads; i++) {
        args[i].thread_id = i;
        args[i].experiments_per_thread = experiments_per_thread + (i < remainder ? 1 : 0);
        args[i].K = K;
        args[i].current_round = current_round;
        args[i].player1_score = p1_score;
        args[i].player2_score = p2_score;
        args[i].local_player1_wins = 0;
        args[i].local_player2_wins = 0;
        args[i].local_draws = 0;
        
        pthread_create(&threads[i], NULL, worker_thread, &args[i]);
    }
    
    size_t total_p1 = 0, total_p2 = 0, total_d = 0;
    for (size_t i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
        total_p1 += args[i].local_player1_wins;
        total_p2 += args[i].local_player2_wins;
        total_d += args[i].local_draws;
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    double time_ms = (end.tv_sec - start.tv_sec) * 1000.0 + 
                     (end.tv_nsec - start.tv_nsec) / 1000000.0;
    
    print_str("Player 1 wins: ");
    print_double(100.0 * total_p1 / num_experiments, 2);
    print_str("%\n");
    
    print_str("Player 2 wins: ");
    print_double(100.0 * total_p2 / num_experiments, 2);
    print_str("%\n");
    
    print_str("Draws: ");
    print_double(100.0 * total_d / num_experiments, 2);
    print_str("%\n");
    
    munmap(threads, num_threads * sizeof(pthread_t));
    munmap(args, num_threads * sizeof(ThreadArgs));
    
    return time_ms;
}

int main(int argc, char **argv) {
    if (argc < 6) {
        print_str("Usage: <K> <current_round> <p1_score> <p2_score> <experiments> [threads]\n");
        return 1;
    }
    
    int K = (int)str_to_long(argv[1]);
    int current_round = (int)str_to_long(argv[2]);
    int p1_score = (int)str_to_long(argv[3]);
    int p2_score = (int)str_to_long(argv[4]);
    size_t experiments = (size_t)str_to_long(argv[5]);
    size_t num_threads = (argc > 6) ? (size_t)str_to_long(argv[6]) : 1;
    
    long num_cores = sysconf(_SC_NPROCESSORS_ONLN);
    print_str("Number of logical processors: ");
    print_num(num_cores);
    print_str("\n");
    
    print_str("\n--- Running with ");
    print_num(num_threads);
    print_str(" threads ---\n");
    
    double time_ms;
    if (num_threads == 1) {
        time_ms = sequential_monte_carlo(K, current_round, p1_score, p2_score, experiments);
    } else {
        time_ms = parallel_monte_carlo(K, current_round, p1_score, p2_score, experiments, num_threads);
    }
    
    print_str("Time: ");
    print_double(time_ms, 2);
    print_str(" ms\n");
    
    return 0;
}
