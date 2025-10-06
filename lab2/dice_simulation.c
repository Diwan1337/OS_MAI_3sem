#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

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

// Функция броска двух костей с thread-local seed
int roll_two_dice(unsigned int *seed) {
    return (rand_r(seed) % 6 + 1) + (rand_r(seed) % 6 + 1);
}

// Функция симуляции одной игры
void simulate_game(int K, int current_round, int p1_score, int p2_score,
                   int *p1_wins, int *p2_wins, int *draws, unsigned int *seed) {
    int player1 = p1_score;
    int player2 = p2_score;
    
    // Симулируем оставшиеся раунды
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
    
    // Каждый поток имеет свой независимый seed
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
double sequential_monte_carlo(int K, int current_round, int p1_score, 
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
    
    printf("Player 1 wins: %.2f%%\n", 100.0 * p1_wins / num_experiments);
    printf("Player 2 wins: %.2f%%\n", 100.0 * p2_wins / num_experiments);
    printf("Draws: %.2f%%\n", 100.0 * draws / num_experiments);
    
    return time_ms;
}

// Параллельная версия
double parallel_monte_carlo(int K, int current_round, int p1_score, 
                           int p2_score, size_t num_experiments, 
                           size_t num_threads) {
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    pthread_t *threads = malloc(num_threads * sizeof(pthread_t));
    ThreadArgs *args = malloc(num_threads * sizeof(ThreadArgs));
    
    size_t experiments_per_thread = num_experiments / num_threads;
    size_t remainder = num_experiments % num_threads;
    
    // Создание потоков
    for (size_t i = 0; i < num_threads; i++) {
        args[i].thread_id = i;
        // Распределяем остаток экспериментов между первыми потоками
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
    
    // Ожидание завершения и агрегация результатов
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
    
    printf("Player 1 wins: %.2f%%\n", 100.0 * total_p1 / num_experiments);
    printf("Player 2 wins: %.2f%%\n", 100.0 * total_p2 / num_experiments);
    printf("Draws: %.2f%%\n", 100.0 * total_d / num_experiments);
    
    free(threads);
    free(args);
    
    return time_ms;
}

int main(int argc, char **argv) {
    if (argc < 6) {
        printf("Usage: %s <K> <current_round> <p1_score> <p2_score> <experiments> [threads]\n", argv[0]);
        printf("Example: %s 10 5 30 25 10000000 4\n", argv[0]);
        return 1;
    }
    
    int K = atoi(argv[1]);
    int current_round = atoi(argv[2]);
    int p1_score = atoi(argv[3]);
    int p2_score = atoi(argv[4]);
    size_t experiments = atol(argv[5]);
    size_t num_threads = (argc > 6) ? atol(argv[6]) : 1;
    
    // Получение количества логических ядер
    long num_cores = sysconf(_SC_NPROCESSORS_ONLN);
    printf("Number of logical processors: %ld\n", num_cores);
    
    printf("\n--- Running with %zu threads ---\n", num_threads);
    double time_ms;
    
    if (num_threads == 1) {
        time_ms = sequential_monte_carlo(K, current_round, p1_score, p2_score, experiments);
    } else {
        time_ms = parallel_monte_carlo(K, current_round, p1_score, p2_score, experiments, num_threads);
    }
    
    printf("Time: %.2f ms\n", time_ms);
    
    return 0;
}
