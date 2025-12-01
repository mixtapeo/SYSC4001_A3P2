#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <sys/sem.h>    // System V semaphores
#include <time.h>

#define NUM_QUESTIONS 5
#define MAX_EXAMS     20
#define SHM_KEY       0x2025  

typedef struct {
    char rubric[NUM_QUESTIONS];          
    int student_number;                
    int question_marked[NUM_QUESTIONS];  // 0 = not marked, 1 = marked
    int current_exam;                  
    int total_exams;                    
    int terminate;                    
} SharedData;

// We use one binary semaphore as a mutex to protect shared memory.
static int semid = -1;

// P() – wait
static void sem_wait_mutex(void) {
    struct sembuf op;
    op.sem_num = 0;
    op.sem_op  = -1; 
    op.sem_flg = 0;
    if (semop(semid, &op, 1) == -1) {
        perror("semop P");
        exit(1);
    }
}

// V() – signal
static void sem_signal_mutex(void) {
    struct sembuf op;
    op.sem_num = 0;
    op.sem_op  = 1;  
    op.sem_flg = 0;
    if (semop(semid, &op, 1) == -1) {
        perror("semop V");
        exit(1);
    }
}

// random sleep 
static void random_sleep_ms(int min_ms, int max_ms) {
    int ms = min_ms + rand() % (max_ms - min_ms + 1);
    usleep(ms * 1000);
}

// load rubric from rubric.txt into shared memory
static void load_rubric(SharedData *shm) {
    FILE *f = fopen("rubric.txt", "r");
    if (!f) {
        perror("rubric.txt");
        exit(1);
    }
    for (int i = 0; i < NUM_QUESTIONS; i++) {
        int q;
        char letter;
        if (fscanf(f, "%d , %c", &q, &letter) != 2) {
            fprintf(stderr, "Error reading rubric line %d\n", i + 1);
            fclose(f);
            exit(1);
        }
        shm->rubric[i] = letter;
    }
    fclose(f);
}

// save current rubric back to rubric.txt
static void save_rubric(SharedData *shm) {
    FILE *f = fopen("rubric.txt", "w");
    if (!f) {
        perror("rubric.txt write");
        return;
    }
    for (int i = 0; i < NUM_QUESTIONS; i++) {
        fprintf(f, "%d, %c\n", i + 1, shm->rubric[i]);
    }
    fclose(f);
}

// load exam from examXX.txt
static void load_exam(SharedData *shm, int exam_index) {
    char filename[64];
    snprintf(filename, sizeof(filename), "exams/exam%02d.txt", exam_index);

    FILE *f = fopen(filename, "r");
    if (!f) {
        perror(filename);
        shm->terminate = 1;
        return;
    }

    int student;
    if (fscanf(f, "%d", &student) != 1) {
        fprintf(stderr, "Error reading student number from %s\n", filename);
        fclose(f);
        shm->terminate = 1;
        return;
    }
    fclose(f);

    // update shared exam info
    sem_wait_mutex();
    shm->student_number = student;
    shm->current_exam   = exam_index;
    for (int i = 0; i < NUM_QUESTIONS; i++) {
        shm->question_marked[i] = 0;
    }
    sem_signal_mutex();

    printf("[LOADER] Loaded %s (student %04d)\n", filename, student);
    fflush(stdout);
}

// TA process behaviour 
static void ta_process(SharedData *shm, int ta_id) {
    srand((unsigned int)(time(NULL) ^ getpid()));

    while (!shm->terminate) {

        // wait until we have an exam loaded
        if (shm->student_number == 0) {
            random_sleep_ms(50, 100);
            continue;
        }

        // copy current student & exam 
        sem_wait_mutex();
        int student = shm->student_number;
        int exam    = shm->current_exam;
        sem_signal_mutex();

        printf("[TA %d] Working on exam %d (student %04d)\n",
               ta_id, exam, student);
        fflush(stdout);

        // review rubric (added 0.5–1.0 s per question)
        for (int i = 0; i < NUM_QUESTIONS; i++) {

            // read + print current rubric value under mutex
            sem_wait_mutex();
            char old_letter = shm->rubric[i];
            printf("[TA %d] Reviewing rubric Q%d = %c\n",
                   ta_id, i + 1, old_letter);
            fflush(stdout);
            sem_signal_mutex();

            random_sleep_ms(500, 1000);  // 0.5–1.0s

            // 50% chance to "correct" the rubric
            if (rand() % 2 == 0) {
                sem_wait_mutex();
                // re-read in case someone changed it already
                old_letter = shm->rubric[i];
                char new_letter = old_letter + 1;
                shm->rubric[i] = new_letter;
                printf("[TA %d] Correcting rubric Q%d: %c -> %c\n",
                       ta_id, i + 1, old_letter, new_letter);
                fflush(stdout);
                save_rubric(shm);  
                sem_signal_mutex();
            }
        }

        // mark questions (1.0–2.0 s per question) with mutex
        while (!shm->terminate) {
            int q = -1;

            // claim a question atomically
            sem_wait_mutex();
            // check if this exam is still the same student
            if (shm->student_number != student) {
                // Loader moved on to next exam
                sem_signal_mutex();
                break;
            }

            for (int i = 0; i < NUM_QUESTIONS; i++) {
                if (shm->question_marked[i] == 0) {
                    shm->question_marked[i] = 1;
                    q = i;
                    break;
                }
            }
            sem_signal_mutex();

            if (q == -1) {
                // no unmarked questions left for this exam
                printf("[TA %d] All questions marked for student %04d\n",
                       ta_id, student);
                fflush(stdout);
                break;
            }

            // print student number anf question number
            printf("[TA %d] Marking Q%d for student %04d...\n",
                   ta_id, q + 1, student);
            fflush(stdout);

            random_sleep_ms(1000, 2000);  // 1.0–2.0s

            printf("[TA %d] Finished Q%d for student %04d\n",
                   ta_id, q + 1, student);
            fflush(stdout);
        }

        // last student= stop everything
        if (student == 9999) {
            sem_wait_mutex();
            shm->terminate = 1;
            sem_signal_mutex();

            printf("[TA %d] last student (9999) reached. Stopping.\n", ta_id);
            fflush(stdout);
            break;
        }

        // load next exam for 1 TA
        if (ta_id == 1) {
            sem_wait_mutex();
            int next_exam = shm->current_exam + 1;
            int total     = shm->total_exams;
            sem_signal_mutex();

            if (next_exam > total) {
                sem_wait_mutex();
                shm->terminate = 1;
                sem_signal_mutex();

                printf("[TA 1] No more exams. Stopping all TAs.\n");
                fflush(stdout);
                break;
            }

            // load next exam
            load_exam(shm, next_exam);
        } else {
            // other TAs wait until student changes or terminate
            while (!shm->terminate) {
                sem_wait_mutex();
                int current_student = shm->student_number;
                sem_signal_mutex();

                if (current_student != student)
                    break;  // new exam loaded

                random_sleep_ms(50, 100);
            }
        }
    }

    printf("[TA %d] Exiting.\n", ta_id);
    fflush(stdout);
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <num_TAs>\n", argv[0]);
        return 1;
    }

    int num_tas = atoi(argv[1]);
    if (num_tas < 2) {
        fprintf(stderr, "Please run with at least 2 TAs.\n");
        return 1;
    }

    // create shared memory
    int shmid = shmget(SHM_KEY, sizeof(SharedData), IPC_CREAT | 0666);
    if (shmid < 0) {
        perror("shmget");
        return 1;
    }

    SharedData *shm = (SharedData *)shmat(shmid, NULL, 0);
    if (shm == (void *)-1) {
        perror("shmat");
        return 1;
    }

    // initialize shared memory
    memset(shm, 0, sizeof(SharedData));
    shm->total_exams = MAX_EXAMS;
    shm->terminate   = 0;

    // create and init semaphore 
    semid = semget(SHM_KEY + 1, 1, IPC_CREAT | 0666);
    if (semid < 0) {
        perror("semget");
        return 1;
    }

    // SETVAL = 1 
    if (semctl(semid, 0, SETVAL, 1) == -1) {
        perror("semctl SETVAL");
        return 1;
    }

    // load rubric and first exam 
    load_rubric(shm);
    load_exam(shm, 1);

    printf("[MAIN] Starting %d TA processes.\n", num_tas);
    fflush(stdout);

    // fork TA processes
    for (int i = 0; i < num_tas; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return 1;
        } else if (pid == 0) {
            // child: TA process
            ta_process(shm, i + 1);
            shmdt(shm);
            exit(0);
        }
    }

    // parent: wait for all TAs to finish
    for (int i = 0; i < num_tas; i++) {
        wait(NULL);
    }

    printf("[MAIN] All TAs finished. Cleaning up.\n");
    fflush(stdout);

    // detach and remove shared memory and semaphore
    shmdt(shm);
    shmctl(shmid, IPC_RMID, NULL);
    semctl(semid, 0, IPC_RMID);

    return 0;
}
