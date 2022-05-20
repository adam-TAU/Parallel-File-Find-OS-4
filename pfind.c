/* Submitter: Adam
 * Tel Aviv University
 * Operating Systems, 2022A
 ===========================
 * Parallel File Find
 * Compile with: gcc -O3 -D_POSIX_C_SOURCE=200809 -Wall -std=c11 -pthread pfind.c 
 */




/****************************** HIGHLIGHTS: TODO ****************************
	1. The program shall distribute work in FIFO order to sleeping threads
	2. No deadlocks (there are right now)
	3. All threads should exit upon no work left (all threads waiting)
	4. cleanup code
****************************** HIGHLIGHTS: TODO ****************************/


#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdatomic.h>
#include <threads.h>
#include <limits.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdlib.h>

#define bool int
#define false 0
#define true 1


/****************************** STRUCTS ****************************/
typedef struct queue_entry_t {
	char* path;
	DIR *dir;
	cnd_t queue_not_empty_event;
	struct queue_entry_t *next;
} queue_entry_t;

typedef struct queue_t {
	queue_entry_t *head;
	atomic_uint size;
} queue_t;
/*******************************************************************/





/****************************** GLOBAL VARS ****************************/
static queue_t dir_queue = {0};
static queue_t waiting_threads_queue = {0};
static atomic_uint num_threads = 0; // the number of threads that were desired to be launched
static atomic_uint pattern_matches = 0; // the number of files that have been found to contain the pattern
static atomic_uint failed_threads = 0; // the number of threads that have died due to an error
static atomic_uint launched_threads = 0; // the number of threads that have been started
static atomic_uint working_threads = 0; // the number of threads that have just dequeued an entry from the directory queue and are still processing it
static atomic_uint waiting_threads = 0; // indicated the number of threads that are waiting for work (i.e., went to sleep for work and have yet to be woken up)
static atomic_int threads_started = false; // used as an indication for if the <threads_start> condition has already met
static atomic_int threads_finished = false; // used as an indication for if all of the threads finished their work (i.e., all threads are waiting)
static mtx_t queue_lock; // mutual excluder for queue operations
static mtx_t all_threads_created_event_lock; // a lock for bounding the thread creation condition-variable use
static cnd_t all_threads_created_event; // condition for identifying a thread creation
static mtx_t threads_start_event_lock; // a lock for the following condition variable
static cnd_t threads_start_event; // condition for identifying if searching threads are allowed to start working
/***********************************************************************/





/****************************** AUXILIARY FUNCTIONS DECLARATIONS ****************************/
/* Handle an error, print the corresponding error message, and terminate if needed. Thread-safe
 * since `perror` is thread-safe. */
void print_err(char* error_message, bool thread_exit, bool program_exit);

/* Initializes global condition variables and locks */
void init_peripherals(void);

/* Destroys global condition variables and locks */
void destroy_peripherals(void);

/* Accepts a number of desired threads, and tries to create them while saving their thread-ids
 * into the given array of <thrd_t>-s.
 
 * Doesn't synchronize anything (allows for termination upon finding an error)
 * On success, returns 0. */
int atomic_create_threads(unsigned int num_threads, thrd_t* thread_ids, char pattern[]);

/* Used to release all waiting threads from their waiting-process. This must be used only upon
 * finishing all work distributed among the threads */
void release_all_threads(void);

/* A function dedicated to be ran by the auxiliar threads. This function fetches a directory
 * from the aforesaid queue and enumerates it for files who hold the pattern we're search for */ 
int thrd_reap_directories(void* pattern);

/* A function dedicated to be ran by an auxiliary thread. This function accepts a directory,
 * enumerates it, prints pattern-matched files in the directory, and potentially enters new 
 * directory queue entries into the waiting directory queue. 

 * Doesn't synchronize anything (allows for termination upon finding an error)
 * On success, returns 0. */
void dir_enum(DIR *dir, char path[], char pattern[]);

/* A function dedicated to handle a new directory that has been fonud in the root directory
 * search tree.
  
 * Doesn't synchronize anything (allows for termination upon finding an error)
 * On success, returns 0. */
void handle_new_dir(char path[]);

/* A function dedicated to handle a new file that has been fonud in the root directory
 * search tree.
  
 * Doesn't synchronize anything (allows for termination upon finding an error)
 * On success, returns 0. */
void handle_new_file(char filename[], char path[], char pattern[]);

/* A function dedicated to append the name of a dirent to the current path of its directory. 
 
 * Doesn't synchronize anything (allows for termination upon finding an error)
 * On success, returns 0. */
void append_path(char path[], char dirent_name[], char* new_path[]);

/* Regular naive enqueue-ing of an entry into a FIFO queue. No synchronization involved */
void enqueue(queue_t *queue, queue_entry_t *entry);

/* Regular naive dequeue-ing of an entry from a FIFO queue. No synchronization involved */
void dequeue(queue_t *queue, queue_entry_t **entry);

/* A function dedicated to be ran by an auxiliary thread. This function accepts a string (the 
 * directory name in our case), and enqueues it synchronously into the given queue. */
void sync_enqueue(queue_entry_t *entry);

/* A function dedicated to be ran by an auxiliary thread. This function accepts a string (the 
 * directory name in our case), and synchronously dequeues the first entry of the given queue 
 * into the string. */
void sync_dequeue(queue_entry_t **entry, queue_entry_t *thread_cv_entry);

/* Checks if all searching threads that are alive are waiting for tasks.
 * If so, the program shall finish due to no work left, therefore this function
 * will return true. Else, returns false. This function must not be placed inbetween 
 * the point of acquiring a directory to search, and the end of the enumeration of
 * the directory, and must be powered between the <queue_lock> lock. */
bool is_finished(void);
/*******************************************************************************************/





/****************************** AUXILIARY FUNCTIONS DEFINITIONS ****************************/
void print_err(char* error_message, bool thread_exit, bool program_exit) {
	int tmp_errno = errno;
	perror(error_message); // this basically prints error_message, with <strerror(errno)> appended to it */
	errno = tmp_errno;
	
	if (thread_exit) {
		failed_threads++;
		thrd_exit(1); // exiting the thread
	}
	
	if (program_exit) {
		exit(1);
	}
}

void init_peripherals(void) {
	mtx_init(&queue_lock, mtx_plain);
	mtx_init(&all_threads_created_event_lock, mtx_plain);
	mtx_init(&threads_start_event_lock, mtx_plain);
	cnd_init(&all_threads_created_event);
	cnd_init(&threads_start_event);
}

void destroy_peripherals(void) {
	mtx_destroy(&queue_lock);
	mtx_destroy(&all_threads_created_event_lock);
	mtx_destroy(&threads_start_event_lock);
	cnd_destroy(&all_threads_created_event);
	cnd_destroy(&threads_start_event);
}

int atomic_create_threads(unsigned int num_threads, thrd_t* thread_ids, char pattern[]) {
	unsigned int i;
	
	/* Creating the threads */
	for (i = 0; i < num_threads; i++) {
		if (0 != thrd_create(&thread_ids[i], thrd_reap_directories, (void*) pattern)) {
			return -1;
		}
	}
	
	/* Waiting for the last thread created to signal that he's the last and all have been created */
	mtx_lock(&all_threads_created_event_lock);
	if (launched_threads != num_threads) {
		cnd_wait(&all_threads_created_event, &all_threads_created_event_lock);
	}
	mtx_unlock(&all_threads_created_event_lock);
	
	return 0;
}

void release_all_threads(void) {
	queue_entry_t* curr = waiting_threads_queue.head;
	
	while (NULL != curr) {
		cnd_signal(&curr->queue_not_empty_event);
		curr = curr->next;
	}
}

int thrd_reap_directories(void* pattern) {
	queue_entry_t *curr_dir_entry;
	
	// creating a dedicated condition variable for this thread to use to be woken up for work with
	queue_entry_t thread_cv_entry;
	cnd_init(&thread_cv_entry.queue_not_empty_event);
	
	// Signal the main thread about the thread creation 
	mtx_lock(&all_threads_created_event_lock);
	launched_threads++;
	if (launched_threads == num_threads) {
		cnd_signal(&all_threads_created_event);
	}
	mtx_unlock(&all_threads_created_event_lock);
	
	// Wait for signal from main thread to start searching
	mtx_lock(&threads_start_event_lock);
	if (!threads_started) {
		cnd_wait(&threads_start_event, &threads_start_event_lock);
	}
	mtx_unlock(&threads_start_event_lock);
	
	/* Start searching */
	while (true) {
		sync_dequeue(&curr_dir_entry, &thread_cv_entry); // thread-safe
		if (NULL != curr_dir_entry) { // sometimes sync_dequeue will dequeue an empty queue for the sake of exiting
			dir_enum(curr_dir_entry->dir, curr_dir_entry->path, (char*) pattern); // thread-safe
			working_threads--;
			free(curr_dir_entry); // free-ing un-used memory
		}
		
		/* Either another thread has freed this thread's lock to finish it, 
		 * or this thread needs to check for if there is no work left */
		mtx_lock(&queue_lock);
		if (threads_finished) { // if a thread has already indicated to everyone that the work is done
			mtx_unlock(&queue_lock);
			break;
		}
		
		if (is_finished()) { // if no thread has indicated that the work is done
			threads_finished = true;
			release_all_threads();
			mtx_unlock(&queue_lock);
			break;
		}
		mtx_unlock(&queue_lock);
	}
	
	// destroy the thread-specific lock and exit
	cnd_destroy(&thread_cv_entry.queue_not_empty_event);
	thrd_exit(1); 
}

void dir_enum(DIR *dir, char path[], char pattern[]) {
	struct stat entry_statbuf; // used to store data induced from `stat`-ing files
	struct dirent *entry;
	
	while ( NULL != (entry = readdir(dir)) ) { // reading next dirent // error here
		/* Eliminating recurssive dir entries */
		if ( (strcmp(entry->d_name, ".") == 0) || (strcmp(entry->d_name, "..") == 0) ) continue;
		
		/* Joining the path of the directory and the <dirent>'s */
		char* entry_path;
		append_path(path, entry->d_name, &entry_path);
		
		/* Getting the file type of the dirent */
		if (0 != lstat(entry_path, &entry_statbuf)) {
			print_err("Error with `stat`-ing a dirent", true, false);
		}
		
		/* If the entry points to a directory */
		if (S_ISDIR(entry_statbuf.st_mode)) {
			handle_new_dir(entry_path);
			continue;
		}
		
		/* If the entry points to a file */
		else if (S_ISREG(entry_statbuf.st_mode)) {
			handle_new_file(entry->d_name, entry_path, pattern);
			continue;
		}
	}	
	
	// close the open dirent after use to free up the fd table
	if (0 != closedir(dir)) print_err("Error with closing the open directory", true, false);
}

void handle_new_dir(char path[]) {
	queue_entry_t *new_dir;
	new_dir = malloc(sizeof(queue_entry_t));
	new_dir->path = path;
	
	if ( NULL == (new_dir->dir = opendir(new_dir->path)) ) { // if an error occurred
	
		if (errno != EACCES) { // errors other than no permissions are treated as errors
			print_err("Error with using the `opendir` command on a new found directory", true, false);
		} else { // simple permissions denial stdout message
			printf("Directory %s: Permission denied.\n", path); 
		}
		
	} else { // if opendir succeeded, we must have enough permissions to search the directory, so we enqueue it
		sync_enqueue(new_dir);
	}
}

void handle_new_file(char filename[], char path[], char pattern[]) {
	if (NULL != strstr(filename, pattern)) {
		pattern_matches++;
		printf("%s\n", path);
	}
}

void append_path(char path[], char dirent_name[], char* new_path[]) {
	unsigned int new_path_len = strlen(path) + strlen(dirent_name) + 2;
	
	*new_path = (char*) malloc( sizeof(char) * new_path_len );
	sprintf(*new_path, "%s/%s", path, dirent_name); // buffer-overflow vulnerability
	(*new_path)[new_path_len - 1] = 0;
}

void enqueue(queue_t *queue, queue_entry_t *entry) {
	entry->next = queue->head;
	queue->head = entry;
	queue->size++;
}

void dequeue(queue_t *queue, queue_entry_t **entry) {
	if (queue->size == 0) {
		*entry = NULL;
	} else if (entry != NULL) {
		*entry = queue->head;
		queue->head = queue->head->next;
		queue->size--;
	}
}


void sync_enqueue(queue_entry_t *dir_queue_entry) {
	queue_entry_t *thread_cv_entry;
	
	/* Synchronization block start (can't terminate upWon error, must engolf with status var */
	mtx_lock(&queue_lock);
	
	// Add directory entry to queue
	enqueue(&dir_queue, dir_queue_entry);
	
	// dequeue the first thread who went to sleep and wake it up
	dequeue(&waiting_threads_queue, &thread_cv_entry);
	if (NULL != thread_cv_entry) { // if no threads are waiting, then there's no need signal anyone
		cnd_signal(&thread_cv_entry->queue_not_empty_event);
	}
	
	mtx_unlock(&queue_lock);
	/* Synchronization block end */
}

void sync_dequeue(queue_entry_t **entry, queue_entry_t *thread_cv_entry) {
	
	/* Synchronization block start */
	mtx_lock(&queue_lock);
	
	if (waiting_threads != 0) { // if there are threads that are sleeping, we must let them finish their work first
		mtx_unlock(&queue_lock);
		goto yield_cpu;
	}
	
	if (dir_queue.size == 0 && !threads_finished) { // don't sleep unless the queue is empty upon arrival and there is still work left
		// adding this thread to the waiting list
		waiting_threads++;
		enqueue(&waiting_threads_queue, thread_cv_entry);
		while (dir_queue.size == 0 && !threads_finished) { // stop if the queue is not empty or if the work is done
			// printf("sleeping: %lu, working: %u, waiting: %u, tasks: %u\n", thrd_current(), working_threads, waiting_threads_queue.size, dir_queue.size);
			cnd_wait(&thread_cv_entry->queue_not_empty_event, &queue_lock); // sleep
			// printf("awakening: %lu, working: %u, waiting: %u, tasks: %u\n", thrd_current(), working_threads, waiting_threads_queue.size, dir_queue.size);
		}
		waiting_threads--;
	}
	
	// remove the directory at the head of the FIFO queue
	// printf("processing: %lu, working: %u, waiting: %u, tasks: %u\n", thrd_current(), working_threads, waiting_threads_queue.size, dir_queue.size);
	working_threads++;
	dequeue(&dir_queue, entry);
	
	mtx_unlock(&queue_lock);
	/* Synchronization block end */
	
	return;
	
yield_cpu:
	*entry = NULL;
	thrd_yield();
}

bool is_finished(void) {
	// all other threads are waiting and queue is empty (must be called after finishing the own work of the thread)
	bool ret = (working_threads == 0) && (dir_queue.size == 0); 
	return ret;
}
/*******************************************************************************************/





/****************************** MAIN ****************************/
int main(int args, char* argv[]) {
	int exit_status = 0;
	
	// checking for the correct amount of arguments
	if (args != 4) {
		print_err("Not enough arguments", false, true); // verify correpondentness to instructions
		exit(1);
	}
	
	// fetching data
	char* root_dir_path = argv[1];
	char* pattern = argv[2];
	num_threads = atoi(argv[3]);
	
	// create array to store thread-ids in
	thrd_t thread_ids[num_threads];
	
	// initializaing locks and condition variables
	init_peripherals();
	
	// creating and enqueue-ing the root directory queue entry
	queue_entry_t *root_dir;
	root_dir = malloc(sizeof(queue_entry_t));
	root_dir->path = root_dir_path;
	root_dir->next = NULL;
	if ( NULL == (root_dir->dir = opendir(root_dir->path)) ) {
		print_err("Error with opening the root directory", false, true);
	}
	enqueue(&dir_queue, root_dir);
	
	// create all threads and wait for all of them to be created
	if ( 0 > atomic_create_threads(num_threads, thread_ids, pattern) ) {
		print_err("Error with successfuly creating the threads", false, true);
	}
	
	// signal the threads to start working
	threads_started = true;
	cnd_broadcast(&threads_start_event);
	
	// wait for all threads to be finish working (if all are waiting, one of the searching threads will terminate the whole program)
	unsigned int i;
	for (i = 0; i < num_threads; i++) {
		thrd_join(thread_ids[i], NULL);
	}
	
	// destroying locks and condition variables
	destroy_peripherals();
	
	// print the amount of matches files
	printf("Done searching, found %d files\n", pattern_matches);
	
	// verifying that no threads have failed during the searching
	if (failed_threads > 0) {
		exit_status = 1;
	}

	// exiting
	exit(exit_status);
}
/***************************************************************/
