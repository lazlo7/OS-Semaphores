#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_ITEM_COUNT 1000
#define MIN_ITEM_RANDOM_COUNT 1
#define MAX_ITEM_RANDOM_COUNT 20

#define MIN_ITEM_RANDOM_PRICE 1
#define MAX_ITEM_RANDOM_PRICE 10000

#define MIN_RANDOM_DELAY 1
#define MAX_RANDOM_DELAY 5

// Shared memory that is used to hand over items from stealer to loader.
char const* shm_stolen_item_name = "shm-stolen-item";
// Shared memory that is used to hand over items from loader into the truck.
char const* shm_loaded_items_name = "shm-loaded-items";

// Semaphore that is used to block stealer when loader is busy.
char const* sem_block_stealer_name = "sem-block-stealer";
// Semaphore that is used to block loader when stealer is busy.
char const* sem_block_loader_name = "sem-block-loader";
// Semaphore that is used to trigger observer when a new item is loaded.
char const* sem_block_observer_name = "sem-block-observer";

int getRandomNumber(int from, int to)
{
    return rand() % (to - from + 1) + from;
}

// item_prices should be at least of size MAX_ITEM_COUNT.
int generateItemPrices(int* item_prices)
{
    int const item_count = getRandomNumber(MIN_ITEM_RANDOM_COUNT, MAX_ITEM_RANDOM_COUNT);
    for (int i = 0; i < item_count; i++) {
        item_prices[i] = getRandomNumber(MIN_ITEM_RANDOM_PRICE, MAX_ITEM_RANDOM_PRICE);
    }
    return item_count;
}

void emulateActivity(void)
{
    sleep(getRandomNumber(MIN_RANDOM_DELAY, MAX_RANDOM_DELAY));
}

// aka "Иванов"
int emulateStealer(
    int* item_prices,
    int item_count,
    char const* shm_stolen_item_name,
    char const* sem_block_stealer_name,
    char const* sem_block_loader_name)
{
    // Open shared memory to hand over items to loader.
    int const shm_stolen_item_fd = shm_open(shm_stolen_item_name, O_CREAT | O_RDWR, 0666);
    if (shm_stolen_item_fd == -1 && errno != EEXIST) {
        printf("[Stealer Error] Failed to open shared memory '%s': %s\n", shm_stolen_item_name, strerror(errno));
        return 1;
    }

    int exit_code = 0;
    int* const shm_stolen_item_addr = mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE, MAP_SHARED, shm_stolen_item_fd, 0);
    if (shm_stolen_item_addr == MAP_FAILED) {
        printf("[Stealer Error] Failed to map shared memory '%s': %s\n", shm_stolen_item_name, strerror(errno));
        exit_code = 1;
        goto cleanup_shm_stolen_item_fd;
    }

    // Open semaphores.
    sem_t* const sem_block_stealer = sem_open(sem_block_stealer_name, O_CREAT, 0666, 0);
    if (sem_block_stealer == SEM_FAILED) {
        printf("[Stealer Error] Failed to open semaphore '%s': %s\n", sem_block_stealer_name, strerror(errno));
        exit_code = 1;
        goto cleanup_shm_stolen_item_fd;
    }

    sem_t* const sem_block_loader = sem_open(sem_block_loader_name, O_CREAT, 0666, 0);
    if (sem_block_loader == SEM_FAILED) {
        printf("[Stealer Error] Failed to open semaphore '%s': %s\n", sem_block_loader_name, strerror(errno));
        exit_code = 1;
        goto cleanup_sem_block_stealer;
    }

    printf("[Stealer] Started!\n");

    for (int i = 0; i < item_count; ++i) {
        printf("[Stealer] Stealing a new item (%d item(s) left)...\n", item_count - i);
        emulateActivity();

        printf("[Stealer] Stolen an item with price %d, handing it over to loader...\n", item_prices[i]);

        // Put the item price into shared memory and unblock loader.
        *shm_stolen_item_addr = item_prices[i];
        if (sem_post(sem_block_loader) == -1) {
            printf("[Stealer Error] Failed to unblock loader: %s\n", strerror(errno));
            exit_code = 1;
            goto cleanup_sem_block_loader;
        }

        // Wait for loader to pick the item from us.
        if (sem_wait(sem_block_stealer) == -1) {
            printf("[Stealer Error] Failed to wait for loader: %s\n", strerror(errno));
            exit_code = 1;
            goto cleanup_sem_block_loader;
        }

        printf("[Stealer] Handed over an item to loader!\n");
    }

// Resources cleanup.
cleanup_sem_block_loader:
    sem_close(sem_block_loader);
cleanup_sem_block_stealer:
    sem_close(sem_block_stealer);
cleanup_shm_stolen_item_fd:
    close(shm_stolen_item_fd);

    if (exit_code == 0) {
        printf("[Stealer] Finished!\n");
    }

    return exit_code;
}

// aka "Петров"
int emulateLoader(
    char const* shm_stolen_item_name,
    char const* shm_loaded_items_name,
    char const* sem_block_stealer_name,
    char const* sem_block_loader_name,
    char const* sem_block_observer_name)
{
    // Open shared memory.
    int const shm_stolen_item_fd = shm_open(shm_stolen_item_name, O_CREAT | O_RDONLY, 0666);
    if (shm_stolen_item_fd == -1 && errno != EEXIST) {
        printf("[Loader Error] Failed to open shared memory '%s': %s\n", shm_stolen_item_name, strerror(errno));
        return 1;
    }

    int exit_code = 0;
    int* const shm_stolen_item_addr = mmap(NULL, sizeof(int), PROT_READ, MAP_SHARED, shm_stolen_item_fd, 0);
    if (shm_stolen_item_addr == MAP_FAILED) {
        printf("[Loader Error] Failed to map shared memory '%s': %s\n", shm_stolen_item_name, strerror(errno));
        exit_code = 1;
        goto cleanup_shm_stolen_item_fd;
    }

    int const shm_loaded_items_fd = shm_open(shm_loaded_items_name, O_CREAT | O_RDWR, 0666);
    if (shm_loaded_items_fd == -1 && errno != EEXIST) {
        printf("[Loader Error] Failed to open shared memory '%s': %s\n", shm_loaded_items_name, strerror(errno));
        exit_code = 1;
        goto cleanup_shm_stolen_item_fd;
    }

    // Allocating an array of integers of size MAX_ITEM_COUNT.
    int* const shm_loaded_items_addr = mmap(NULL, sizeof(int) * MAX_ITEM_COUNT, PROT_READ | PROT_WRITE, MAP_SHARED, shm_loaded_items_fd, 0);
    if (shm_loaded_items_addr == MAP_FAILED) {
        printf("[Loader Error] Failed to map shared memory '%s': %s\n", shm_loaded_items_name, strerror(errno));
        exit_code = 1;
        goto cleanup_shm_loaded_items_fd;
    }

    // Open semaphores.
    sem_t* const sem_block_stealer = sem_open(sem_block_stealer_name, O_CREAT, 0666, 0);
    if (sem_block_stealer == SEM_FAILED) {
        printf("[Loader Error] Failed to open semaphore '%s': %s\n", sem_block_stealer_name, strerror(errno));
        exit_code = 1;
        goto cleanup_shm_loaded_items_fd;
    }

    sem_t* const sem_block_loader = sem_open(sem_block_loader_name, O_CREAT, 0666, 0);
    if (sem_block_loader == SEM_FAILED) {
        printf("[Loader Error] Failed to open semaphore '%s': %s\n", sem_block_loader_name, strerror(errno));
        exit_code = 1;
        goto cleanup_sem_block_stealer;
    }

    sem_t* const sem_block_observer = sem_open(sem_block_observer_name, O_CREAT, 0666, 0);
    if (sem_block_observer == SEM_FAILED) {
        printf("[Loader Error] Failed to open semaphore '%s': %s\n", sem_block_observer_name, strerror(errno));
        exit_code = 1;
        goto cleanup_sem_block_loader;
    }

    printf("[Loader] Started!\n");

    // Assume that Loader knows beforehand that there are no more than MAX_ITEM_COUNT items to load.
    for (int loaded_item_count = 0; loaded_item_count < MAX_ITEM_COUNT; ++loaded_item_count) {
        printf("[Loader] Waiting for a new item from stealer...\n");

        // Wait for stealer to put the item into shared memory.
        if (sem_wait(sem_block_loader) == -1) {
            printf("[Loader Error] Failed to wait for stealer: %s\n", strerror(errno));
            exit_code = 1;
            goto cleanup_sem_block_observer;
        }

        // Receive the item from Stealer.
        printf("[Loader] Got a new item info from stealer, receiving it...\n");
        int const stolen_item_price = *shm_stolen_item_addr;
        shm_loaded_items_addr[loaded_item_count] = stolen_item_price;

        // Notify Stealer that we've got the item.
        if (sem_post(sem_block_stealer) == -1) {
            printf("[Loader Error] Failed to unblock stealer: %s\n", strerror(errno));
            exit_code = 1;
            goto cleanup_sem_block_observer;
        }

        printf("[Loader] Received new item info from stealer!");

        // If the item price is negative, it means that Stealer has no more items to hand over.
        if (stolen_item_price < 0) {
            printf("[Loader] No more items to load, notifying observer and exiting...\n");
            if (sem_post(sem_block_observer) == -1) {
                printf("[Loader Error] Failed to unblock observer: %s\n", strerror(errno));
                exit_code = 1;
                goto cleanup_sem_block_observer;
            }

            break;
        }

        // Emulate loading process.
        emulateActivity();

        // Notify Observer that a new item was loaded.
        // Note that, we specifically don't wait for the observer to finish.
        printf("[Loader] Loaded a new item, notifying observer...\n");
        if (sem_post(sem_block_observer) == -1) {
            printf("[Loader Error] Failed to unblock observer: %s\n", strerror(errno));
            exit_code = 1;
            goto cleanup_sem_block_observer;
        }

        printf("[Loader] Notified observer about a new item!\n");
    }

// Resources cleanup.
cleanup_sem_block_observer:
    sem_close(sem_block_observer);
cleanup_sem_block_loader:
    sem_close(sem_block_loader);
cleanup_sem_block_stealer:
    sem_close(sem_block_stealer);
cleanup_shm_loaded_items_fd:
    close(shm_loaded_items_fd);
cleanup_shm_stolen_item_fd:
    close(shm_stolen_item_fd);

    if (exit_code == 0) {
        printf("[Loader] Finished!\n");
    }

    return exit_code;
}

// aka "Нечепорук"
int emulateObserver(char const* sem_block_observer_name)
{
    // Open shared memory.
    int const shm_loaded_items_fd = shm_open(shm_loaded_items_name, O_RDWR, 0666);
    if (shm_loaded_items_fd == -1 && errno != EEXIST) {
        printf("[Observer Error] Failed to open shared memory '%s': %s\n", shm_loaded_items_name, strerror(errno));
        return 1;
    }

    int exit_code = 0;
    // Allocating an array of integers of size MAX_ITEM_COUNT.
    int* const shm_loaded_items_addr = mmap(NULL, sizeof(int) * MAX_ITEM_COUNT, PROT_READ | PROT_WRITE, MAP_SHARED, shm_loaded_items_fd, 0);
    if (shm_loaded_items_addr == MAP_FAILED) {
        printf("[Observer Error] Failed to map shared memory '%s': %s\n", shm_loaded_items_name, strerror(errno));
        exit_code = 1;
        goto cleanup_shm_loaded_items_fd;
    }

    // Open semaphore.
    sem_t* const sem_block_observer = sem_open(sem_block_observer_name, O_CREAT, 0666, 0);
    if (sem_block_observer == SEM_FAILED) {
        printf("[Observer Error] Failed to open semaphore '%s': %s\n", sem_block_observer_name, strerror(errno));
        exit_code = 1;
        goto cleanup_shm_loaded_items_fd;
    }

    int total_items_price = 0;
    printf("[Observer] Started!\n");

    // Assume that Loader knows beforehand that there are no more than MAX_ITEM_COUNT items to load.
    for (int loaded_item_count = 0; loaded_item_count < MAX_ITEM_COUNT; ++loaded_item_count) {
        printf("[Observer] No officers around, checking nearby areas...\n");

        // Waiting for loader to notify us about a new item.
        if (sem_wait(sem_block_observer) == -1) {
            printf("[Observer Error] Failed to wait for loader: %s\n", strerror(errno));
            exit_code = 1;
            goto cleanup_sem_block_observer;
        }

        int const loaded_item_price = shm_loaded_items_addr[loaded_item_count];
        if (loaded_item_price < 0) {
            printf("[Observer] No more items to observe, exiting...\n");
            // Clear the negative price.
            shm_loaded_items_addr[loaded_item_count] = 0;
            break;
        }

        // Emulate price calculation process.
        emulateActivity();

        total_items_price += loaded_item_price;

        printf("[Observer] A new item! + %d rubbles, total: %d rubbles for %d items stolen\n",
            loaded_item_price, total_items_price, loaded_item_count + 1);
    }

cleanup_sem_block_observer:
    sem_close(sem_block_observer);
cleanup_shm_loaded_items_fd:
    close(shm_loaded_items_fd);

    if (exit_code == 0) {
        printf("[Observer] Finished!\n");
    }

    return exit_code;
}

int finished_child_process_count = 0;

// Forked children pid's, used in onChildTerminated callback.
pid_t stealer_pid;
pid_t loader_pid;
pid_t observer_pid;

void onChildTerminated(int signum)
{
    (void)signum;


    int status;
    pid_t const exited_pid = wait(&status);
    if (exited_pid == -1) {
        printf("[Error] Failed to wait for child process: %s\n", strerror(errno));
        return;
    }

    if (WEXITSTATUS(status) == 0) {
        ++finished_child_process_count;
        return;
    }

    // An error must have occurred. Kill all other child processes.
    if (exited_pid == stealer_pid) {
        printf("[Error] Stealer failed, killing loader and observer...\n");
        kill(loader_pid, SIGKILL);
        kill(observer_pid, SIGKILL);
    } else if (exited_pid == loader_pid) {
        printf("[Error] Loader failed, killing stealer and observer...\n");
        kill(stealer_pid, SIGKILL);
        kill(observer_pid, SIGKILL);
    } else if (exited_pid == observer_pid) {
        printf("[Error] Observer failed, killing stealer and loader...\n");
        kill(stealer_pid, SIGKILL);
        kill(loader_pid, SIGKILL);
    }

    exit(1);
}

int main(int argc, char** argv)
{
    // Parse item prices.
    int item_count = argc - 1;
    if (item_count > MAX_ITEM_COUNT) {
        printf("[Error] Too many items: %d (max %d)\n", item_count, MAX_ITEM_COUNT);
        return 1;
    }

    srand(time(NULL));
    int item_prices[MAX_ITEM_COUNT];
    if (item_count == 0) {
        item_count = generateItemPrices(item_prices);
    } else {
        for (int i = 0; i < item_count; ++i) {
            sscanf(argv[i + 1], "%d", &item_prices[i]);
        }
    }

    printf("Military intel tells us that there are %d items to steal with prices: \n", item_count);
    for (int i = 0; i < item_count; ++i) {
        printf("%d", item_prices[i]);
        if (i + 1 != item_count) {
            printf(", ");
        }
    }
    printf("\n");

    // Register SIGCHLD handler.
    if (signal(SIGCHLD, onChildTerminated) == SIG_ERR) {
        printf("[Error] Failed to register SIGCHLD handler: %s\n", strerror(errno));
        return 1;
    }

    stealer_pid = fork();
    if (stealer_pid == -1) {
        printf("[Error] Failed to fork for stealer: %s\n", strerror(errno));
        return 1;
    }

    if (stealer_pid == 0) {
        // Stealer.
        return emulateStealer(item_prices, item_count, shm_stolen_item_name, sem_block_stealer_name, sem_block_loader_name);
    }

    loader_pid = fork();
    if (loader_pid == -1) {
        printf("[Error] Failed to fork for loader: %s\n", strerror(errno));
        return 1;
    }

    if (loader_pid == 0) {
        // Loader.
        return emulateLoader(shm_stolen_item_name, shm_loaded_items_name, sem_block_stealer_name, sem_block_loader_name, sem_block_observer_name);
    }

    observer_pid = fork();
    if (observer_pid == -1) {
        printf("[Error] Failed to fork for observer: %s\n", strerror(errno));
        return 1;
    }

    if (observer_pid == 0) {
        // Observer.
        return emulateObserver(sem_block_observer_name);
    }

    // Wait for children to finish.
    while (finished_child_process_count < 3) {
    }

    return 0;
}
