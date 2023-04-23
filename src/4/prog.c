#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <signal.h>
#include <stdbool.h>
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
    int shm_stolen_item_fd = shm_open(shm_stolen_item_name, O_CREAT | O_RDWR, 0666);
    if (shm_stolen_item_fd == -1 && errno != EEXIST) {
        printf("[Stealer Error] Failed to open shared memory '%s': %s\n", shm_stolen_item_name, strerror(errno));
        return 1;
    }

    int exit_code = 0;
    // Truncate memory size.
    if (ftruncate(shm_stolen_item_fd, sizeof(int)) == -1) {
        printf("[Stealer Error] Failed to truncate memory: %s\n", strerror(errno));
        exit_code = 1;
        goto cleanup_shm_stolen_item_fd;
    }

    int* shm_stolen_item_addr = mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE, MAP_SHARED, shm_stolen_item_fd, 0);
    if (shm_stolen_item_addr == MAP_FAILED) {
        printf("[Stealer Error] Failed to map shared memory '%s': %s\n", shm_stolen_item_name, strerror(errno));
        exit_code = 1;
        goto cleanup_shm_stolen_item_fd;
    }

    // Open semaphores.
    sem_t* sem_block_stealer = sem_open(sem_block_stealer_name, O_CREAT, 0666, 0);
    if (sem_block_stealer == SEM_FAILED) {
        printf("[Stealer Error] Failed to open semaphore '%s': %s\n", sem_block_stealer_name, strerror(errno));
        exit_code = 1;
        goto cleanup_shm_stolen_item_fd;
    }

    sem_t* sem_block_loader = sem_open(sem_block_loader_name, O_CREAT, 0666, 0);
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
    int shm_stolen_item_fd = shm_open(shm_stolen_item_name, O_CREAT | O_RDWR, 0666);
    if (shm_stolen_item_fd == -1 && errno != EEXIST) {
        printf("[Loader Error] Failed to open shared memory '%s': %s\n", shm_stolen_item_name, strerror(errno));
        return 1;
    }

    int exit_code = 0;
    // Truncate memory size.
    if (ftruncate(shm_stolen_item_fd, sizeof(int)) == -1) {
        printf("[Loader Error] Failed to truncate memory 1: %s\n", strerror(errno));
        exit_code = 1;
        goto cleanup_shm_stolen_item_fd;
    }

    int* shm_stolen_item_addr = mmap(NULL, sizeof(int), PROT_READ, MAP_SHARED, shm_stolen_item_fd, 0);
    if (shm_stolen_item_addr == MAP_FAILED) {
        printf("[Loader Error] Failed to map shared memory '%s': %s\n", shm_stolen_item_name, strerror(errno));
        exit_code = 1;
        goto cleanup_shm_stolen_item_fd;
    }

    int shm_loaded_items_fd = shm_open(shm_loaded_items_name, O_CREAT | O_RDWR, 0666);
    if (shm_loaded_items_fd == -1 && errno != EEXIST) {
        printf("[Loader Error] Failed to open shared memory '%s': %s\n", shm_loaded_items_name, strerror(errno));
        exit_code = 1;
        goto cleanup_shm_stolen_item_fd;
    }

    // Truncate memory size.
    if (ftruncate(shm_loaded_items_fd, sizeof(int) * MAX_ITEM_COUNT) == -1) {
        printf("[Loader Error] Failed to truncate memory 2: %s\n", strerror(errno));
        exit_code = 1;
        goto cleanup_shm_loaded_items_fd;
    }

    // Allocating an array of integers of size MAX_ITEM_COUNT.
    int* shm_loaded_items_addr = mmap(NULL, sizeof(int) * MAX_ITEM_COUNT, PROT_READ | PROT_WRITE, MAP_SHARED, shm_loaded_items_fd, 0);
    if (shm_loaded_items_addr == MAP_FAILED) {
        printf("[Loader Error] Failed to map shared memory '%s': %s\n", shm_loaded_items_name, strerror(errno));
        exit_code = 1;
        goto cleanup_shm_loaded_items_fd;
    }

    // Open semaphores.
    sem_t* sem_block_stealer = sem_open(sem_block_stealer_name, O_CREAT, 0666, 0);
    if (sem_block_stealer == SEM_FAILED) {
        printf("[Loader Error] Failed to open semaphore '%s': %s\n", sem_block_stealer_name, strerror(errno));
        exit_code = 1;
        goto cleanup_shm_loaded_items_fd;
    }

    sem_t* sem_block_loader = sem_open(sem_block_loader_name, O_CREAT, 0666, 0);
    if (sem_block_loader == SEM_FAILED) {
        printf("[Loader Error] Failed to open semaphore '%s': %s\n", sem_block_loader_name, strerror(errno));
        exit_code = 1;
        goto cleanup_sem_block_stealer;
    }

    sem_t* sem_block_observer = sem_open(sem_block_observer_name, O_CREAT, 0666, 0);
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

        printf("[Loader] Received new item info from stealer!\n");

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
    int shm_loaded_items_fd = shm_open(shm_loaded_items_name, O_CREAT | O_RDWR, 0666);
    if (shm_loaded_items_fd == -1 && errno != EEXIST) {
        printf("[Observer Error] Failed to open shared memory '%s': %s\n", shm_loaded_items_name, strerror(errno));
        return 1;
    }

    int exit_code = 0;
    // Truncate memory size.
    if (ftruncate(shm_loaded_items_fd, sizeof(int) * MAX_ITEM_COUNT) == -1) {
        printf("[Observer Error] Failed to truncate memory: %s\n", strerror(errno));
        exit_code = 1;
        goto cleanup_shm_loaded_items_fd;
    }

    // Allocating an array of integers of size MAX_ITEM_COUNT.
    int* shm_loaded_items_addr = mmap(NULL, sizeof(int) * MAX_ITEM_COUNT, PROT_READ | PROT_WRITE, MAP_SHARED, shm_loaded_items_fd, 0);
    if (shm_loaded_items_addr == MAP_FAILED) {
        printf("[Observer Error] Failed to map shared memory '%s': %s\n", shm_loaded_items_name, strerror(errno));
        exit_code = 1;
        goto cleanup_shm_loaded_items_fd;
    }

    // Open semaphore.
    sem_t* sem_block_observer = sem_open(sem_block_observer_name, O_CREAT, 0666, 0);
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

        int loaded_item_price = shm_loaded_items_addr[loaded_item_count];
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

// Since these pid's are assigned in main, but we use them in SIGINT handler,
// which can be called even before the pid's are assigned, we need to prevent this
// race condition by introducing defined bool variables.
bool stealer_pid_defined = false;
bool loader_pid_defined = false;
bool observer_pid_defined = false;

// This helper function only kills a forked child if it's bool 'defined' value is true.
void killForked(pid_t forked)
{
    if ((forked == stealer_pid && stealer_pid_defined)
        || (forked == loader_pid && loader_pid_defined)
        || (forked == observer_pid && observer_pid_defined)) {
        kill(forked, SIGKILL);
    }
}

// Performs a cleanup of all shm and semaphores.
// Note that even if this function gets called when some of the shm/semaphores
// don't event exist, shm_unlink() and sem_unlink() will just fail silently.
void cleanup(void)
{
    // Unlink all shm.
    shm_unlink(shm_stolen_item_name);
    shm_unlink(shm_loaded_items_name);

    // Unlink all semaphores.
    sem_unlink(sem_block_stealer_name);
    sem_unlink(sem_block_loader_name);
    sem_unlink(sem_block_observer_name);
}

void onChildTerminated(int signum)
{
    (void)signum;

    int status;
    pid_t exited_pid = wait(&status);
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
        killForked(loader_pid);
        killForked(observer_pid);
    } else if (exited_pid == loader_pid) {
        printf("[Error] Loader failed, killing stealer and observer...\n");
        killForked(stealer_pid);
        killForked(observer_pid);
    } else if (exited_pid == observer_pid) {
        printf("[Error] Observer failed, killing stealer and loader...\n");
        killForked(stealer_pid);
        killForked(loader_pid);
    }

    // Cleanup resources.
    cleanup();
    printf("Cleaned up resources\n");

    // Exit with error.
    printf("Exit.\n");
    exit(1);
}

void onInterruptReceived(int signum)
{
    printf("SIGINT Received, killing all forked processes, cleaning up resources and exiting...\n");
    (void)signum;

    // Kill all forked processes.
    killForked(stealer_pid);
    killForked(loader_pid);
    killForked(observer_pid);
    printf("Killed all forked processes\n");

    // Cleanup resources.
    cleanup();
    printf("Cleaned up resources\n");

    // Exit with success (we don't consider a SIGINT an error).
    printf("Exit.\n");
    exit(0);
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
    stealer_pid_defined = true;

    loader_pid = fork();
    if (loader_pid == -1) {
        printf("[Error] Failed to fork for loader: %s\n", strerror(errno));
        return 1;
    }

    if (loader_pid == 0) {
        // Loader.
        return emulateLoader(shm_stolen_item_name, shm_loaded_items_name, sem_block_stealer_name, sem_block_loader_name, sem_block_observer_name);
    }
    loader_pid_defined = true;

    observer_pid = fork();
    if (observer_pid == -1) {
        printf("[Error] Failed to fork for observer: %s\n", strerror(errno));
        return 1;
    }

    if (observer_pid == 0) {
        // Observer.
        return emulateObserver(sem_block_observer_name);
    }
    observer_pid_defined = true;

    // Register SIGINT handler.
    // On SIGINT, we should kill all forked processes, exit the program.
    // Note that we register a SIGINT handler only after all processes have been forked,
    // so that only the main process would have this handler.
    // When forking, a child process will inherit all of the parent's signal's handlers,
    // and we don't want children to have SIGINt handlers.
    if (signal(SIGINT, onInterruptReceived)) {
        printf("[Error] Failed to register SIGINT handler: %s\n", strerror(errno));
        return 1;
    }

    // Wait for children to finish.
    while (finished_child_process_count < 3) {
    }

    return 0;
}
