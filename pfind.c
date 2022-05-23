/* Submitter: Adam
 * Tel Aviv University
 * Operating Systems, 2022A
 ===========================
 * Parallel File Find
 * Compile with: gcc -O3 -D_POSIX_C_SOURCE=200809 -Wall -std=c11 -pthread pfind.c 
 */




/****************************** HIGHLIGHTS: TODO ****************************
	1. resolve memory issue at line 326
	2. sid tester is acting up at unsearchable dir test run
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
	/* responsible for directory entries */
	char* path;
	DIR *dir;
	
	/* responsible for thread entries */
	struct queue_entry_t *dir_queue_entry; // the directory it needs to process
	cnd_t thread_queue_not_empty_event; // the condition variable for a directory being allocated
	
	/* api-required field for FIFO queue */
	struct queue_entry_t *next;
} queue_entry_t;

typedef struct queue_t {
	queue_entry_t *head;
	queue_entry_t *tail;
	atomic_uint size;
} queue_t;
/*******************************************************************/


static atomic_uint tmp = 0;


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
static mtx_t protocol_lock; // mutual excluder for queue operations
static mtx_t all_threads_created_event_lock; // a lock for bounding the thread creation condition-variable use
static cnd_t all_threads_created_event; // condition for identifying a thread creation
static mtx_t threads_start_event_lock; // a lock for the following condition variable
static cnd_t threads_start_event; // condition for identifying if searching threads are allowed to start working
/***********************************************************************/





/****************************************************************************************/
/****************************** MAIN-THREAD: PROGRAM CONTROL ****************************/
/****************************************************************************************/
/* @user:	main-thread -> used to initialize all locks and condition variables
 * @action: initializes all globally accessible locks and condition variables
 * @errors:	NONE */
void init_peripherals(void);

/* @user:	main-thread -> used to destroy all locks and condition variables
 * @action: destroys all globally accessible locks and condition variables
 * @errors:	NONE */
void destroy_peripherals(void);

/* @user: 	main-thread -> used to create workers to search for files
 *						   whose name include <pattern>
 * @action: creates <num_threads> threads whose start address is <thrd_reap_directories>.
 *			passes <pattern> to <thrd_reap_directories> as the pattern needed to be found
 *			in files processes by this thread
 * @ret: 	success <-> 0
 * @errors:	NON-TERMINATING-ONLY */
int atomic_create_threads(unsigned int num_threads, thrd_t thread_ids[], char pattern[]);

/* @user: 	main-thread -> used to block the main thread until termination of all threads
 * @action: accepts a range of thread-ids and joins all those threads
 * @ret: 	success <-> 0
 * @errors:	NON-TERMINATING-ONLY */
int atomic_join_threads(unsigned int num_threads, thrd_t thread_ids[]);

/* @user: 	main-thread -> uses function's pointer as the starting position for threads it creates
 * @action: reaps directories and processes them, using the <sync_dequeue> and <dir_enum> api.
 *			tries to find files whose name include <pattern> in them */
int thrd_reap_directories(void* pattern);
/****************************************************************************************/






/*****************************************************************************************/
/****************************** LOCAL-THREAD: PROGRAM CONTROL ****************************/
/*****************************************************************************************/
/* @user: 	thread-local -> used only upon having all work processed (no more directories)
 * @action: releases all sleeping thread's locks, forcing progress
 * @ret:	VOID
 * @errors:	NONE */
void release_all_threads(void);

/* @user: 	thread-local -> used to block the thread until all threads have been launched
 * @action: block the code of the thread until all threads have launched
 * @ret:	VOID
 * @errors:	NONE */
void start_fence(void);
/****************************************************************************************/






/*************************************************************************/
/****************************** PROTOCOL'S API ***************************/
/*************************************************************************/
/* @user: 	thread-local -> used to enqueue new directories found into the protocol's FIFO Queue
 * @action: grabs a lock over the protocol, and inserts the new directory into the FIFO Queue. 
 *			Then, this function also signals the condition variable of the first entry it
 *			dequeues from <waiting_threads_queue> - to wake up the first-to-sleep thread.
 * @ret:	VOID
 * @errors:	NONE */
void sync_enqueue(queue_entry_t *entry);

/* @user: 	thread-local -> used to dequeue a directory from the protocol's FIFO Queue
 * @action: grabs a lock over the protocol, and tries to dequeue a directory from the
 *			FIFO Queue. If there are none, it goes to sleep. If there are already sleeping
 *			threads, it must go to sleep in order to maintain FIFO order of distribution of
 *			work. In case it goes to sleep, it enqueues itself to the <waiting_threads_queue>
 *			so that it will wake up after all other sleeping threads have woke up, and also
 *			increments the <waiting_threads> variable which indicates the amount of threads
 *			which are currently awaiting a directory to process 
 * @ret:	VOID
 * @errors:	NONE */
void sync_dequeue(queue_entry_t *thread_entry);

/* @user:	thread-local -> used to check if to terminate the thread or other threads
 * @action:	grabs a lock over the protocol, and checks if another thread has indicated
 *			to all threads through the global variable <threads_finished> that there is
 *			no more work to be done. If so, this thread will exit. Otherwise, this
 *			thread will check itself if the work is done, through <is_finished>. If
 *			it turns out that the work is done,	it will update the <threads_finished>
 *			global variable to <true> and release all of the sleeping threads (through
 *			reaping through <waiting_threads_queue>'s entries' condition variables).
 * @ret:	true <-> (<threads_finished> == true) || (is_finished() == true)
 * @errors:	NONE */
bool check_to_terminate(void);
/*************************************************************************/






/*************************************************************************************************/
/****************************** PROTOCOL'S API'S AUXILIARY MODULE ********************************/
/*************************************************************************************************/
/* @user: 	thread-local -> used to enumerate a directory for new directory entries or file
 *						  	pattern matches. 
 * @action: accepts a directory pointer <dir>, the directory path <dir_path>m and the aforesaid
 			pattern named <pattern>. In case we find in the directory scan a <dirent> which is a
 			directory, we'll first check if we have permissions to search it. If we do, we insert
 			this direcotry into the protocol's FIFO Queue using the <sync_enqueue> function.
 			If it isn't a directory, we then try to find <pattern> in the <dirent>'s <d_name>.
 			If we do, we print the path to this <dirent>. Handling of a <dirent> is done by 
 			<handle_dirent>.
 * @ret:	VOID
 * @errors:	TERMINATING-ONLY */
void dir_enum(DIR *dir, char dir_path[], char pattern[]);

/* @user: 	<dir_enum> -> used to process new <dirent>-s found in <dir_enum>							
 * @action: accepts a <dirent> of path <dir_path>/<dirent_name>, and handles the <dirent> accordingly
 *			using <handle_new_dir> and <handle_new_file>:
 *			In case of a permissive directory, it inserts it into the protocol's FIFO Queue using 
 *			<sync_enqueue>. Otherwise, it searches for <pattern> in <dirent_name> - and if found, it
 *			prints the abstract path of that <dirent>.
 * @ret:	VOID
 * @errors:	TERMINATING-ONLY */
void handle_dirent(char dir_path[], char dirent_name[], char pattern[]);

/* @user: 	<handle_dirent> -> used to process directories passed from <handle_dirent>
 * @action:	accepts a path to a directory, checks if a premissive directory, and if so
 *			it inserts it into the protocol's FIFO Queue. 
 * @ret: 	success <-> 0
 * @errors:	TERMINATING-ONLY */
int handle_new_dir(char *dirent_path);

/* @user: 	<handle_dirent> -> used to process non-directories passed from <handle_dirent>
 * @action:	accepts the name of a non-directory <dirent>, its full path, and <pattern>.
 *			If <dirent_name> includes <pattern> in it, <dirent_full_path> will be printer out
 *			and <pattern_matches> would be incremented. 
 * @ret: 	VOID
 * @errors:	NONE */
void handle_new_file(char dirent_name[], char dirent_path[], char pattern[]);

/* @user: 	<check_to_terminate> -> used to measure if all of the work of the program has been done
 * @ret: 	true <-> work of program is done (no more directories to search)
 * @errors:	NONE */
bool is_finished(void);
/*************************************************************************************************/





/******************************************************************************************/
/****************************** REGULAR AUXILIARY FUNCTIONS ********************************/
/******************************************************************************************/
/* @action:	prints the <error_message> through `perror()`. Also, it may exit the thread/program
 *			according to the arguments <thread_exit> and <main_exit>. In case of a thread exit, 
 *			it updates the amount of working threads (since errors occur in a local thread only upon). 
 * @ret:	VOID
 * @errors:	NONE */
void print_err(char error_message[], bool thread_exit, bool main_exit);

/* @action: unsynchronized enqueue of <entry> into <queue>
 * @ret:	VOID
 * @errors:	NONE */
void enqueue(queue_t *queue, queue_entry_t *entry);

/* @action: unsynchronized dequeue from the FIFO queue <queueu> to <entry>
 * @ret:	VOID
 * @errors:	NONE */
void dequeue(queue_t *queue, queue_entry_t **entry);

/* @action:	accepts abstract path to directory <path>, a dirent's name <dirent_name>,
 *			and a pointer to a string that we will store the conjoined path into.
 * @ret:	VOID
 * @errors:	NONE */
void append_path(char dir_path[], char dirent_name[], char** dirent_path);
/******************************************************************************************/







/****************************** MAIN-THREAD: PROGRAM CONTROL ****************************/
void init_peripherals(void) {
	mtx_init(&protocol_lock, mtx_plain);
	mtx_init(&all_threads_created_event_lock, mtx_plain);
	mtx_init(&threads_start_event_lock, mtx_plain);
	cnd_init(&all_threads_created_event);
	cnd_init(&threads_start_event);
}

void destroy_peripherals(void) {
	mtx_destroy(&protocol_lock);
	mtx_destroy(&all_threads_created_event_lock);
	mtx_destroy(&threads_start_event_lock);
	cnd_destroy(&all_threads_created_event);
	cnd_destroy(&threads_start_event);
}

int atomic_create_threads(unsigned int num_threads, thrd_t thread_ids[], char pattern[]) {
	unsigned int i;
	
	/* Creating the threads */
	for (i = 0; i < num_threads; i++) {
		if (thrd_success != thrd_create(&thread_ids[i], thrd_reap_directories, (void*) pattern)) {
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

int atomic_join_threads(unsigned int num_threads, thrd_t thread_ids[]) {
	unsigned int i;
	for (i = 0; i < num_threads; i++) {
		if ( thrd_success != thrd_join(thread_ids[i], NULL) ) {
			return -1;
		}
	}
	
	return 0;
}

int thrd_reap_directories(void* pattern) {

	/* creating a dedicated condition variable for this thread to use to be woken up for work with */
	queue_entry_t *thread_entry;
	thread_entry = malloc(sizeof(queue_entry_t));
	thread_entry->dir_queue_entry = NULL;
	cnd_init(&thread_entry->thread_queue_not_empty_event);
	
	/* block the thread code until we are able to start working */
	start_fence();
	
	/* Start working */
	while (true) {
		// thread-safe dequeue-ing of a directory from the protocol's FIFO Queue
		sync_dequeue(thread_entry);
		
		// handling of the directory we have just dequeued
		if (NULL != thread_entry->dir_queue_entry) { // avoiding dequeue-ing of an empty queue
			// enumerate new deququed directory
			dir_enum(thread_entry->dir_queue_entry->dir, thread_entry->dir_queue_entry->path, (char*) pattern); // thread-safe
			
			// free-ing un-used memory of the processed directory entry
			// if (NULL != thread_entry->dir_queue_entry->path) free(thread_entry->dir_queue_entry->path); // problematic (memory-wise)
			free(thread_entry->dir_queue_entry); // no more need to the directory queue entry
			thread_entry->dir_queue_entry = NULL; // nullify the directory fetched to process
		}
		
		// checking if we need to terminate the reaping of this thread or other threads'
		if (check_to_terminate()) break;
	}
	
	/* destroy the thread-specific lock and exit */
	cnd_destroy(&thread_entry->thread_queue_not_empty_event);
	thrd_exit(1); 
}
/****************************************************************************************/









/****************************** LOCAL-THREAD: PROGRAM CONTROL ****************************/
void release_all_threads(void) {
	queue_entry_t* curr = waiting_threads_queue.head;
	
	while (NULL != curr) {
		cnd_signal(&curr->thread_queue_not_empty_event);
		curr = curr->next;
	}
}

void start_fence(void) {
	// signal that this thread has been launched
	mtx_lock(&all_threads_created_event_lock);
	launched_threads++;
	if (launched_threads == num_threads) {
		cnd_signal(&all_threads_created_event);
	}
	mtx_unlock(&all_threads_created_event_lock);
	
	// wait for signal from the main thread that all threads have launched
	mtx_lock(&threads_start_event_lock);
	if (!threads_started) {
		cnd_wait(&threads_start_event, &threads_start_event_lock);
	}
	mtx_unlock(&threads_start_event_lock);
}
/*************************************************************************/









/**************************************** PROTOCOL'S API *************************************/
void sync_enqueue(queue_entry_t *dir_queue_entry) {
	queue_entry_t *thread_entry;
	
	/* Synchronization block start */
	mtx_lock(&protocol_lock);
		
	if (waiting_threads_queue.size == 0) { // if no threads are sleeping, simply enqueue the new directory to the <dir_queue>
		enqueue(&dir_queue, dir_queue_entry);
		
	} else { // if there are sleeping threads, we must serve the currently enqueuq-ed directory to the first sleeping thread	
		// dequeue the first thread who went to sleep
		dequeue(&waiting_threads_queue, &thread_entry);
		thread_entry->dir_queue_entry = dir_queue_entry; // attaching the new enqueued directory to the very first sleeping thread
		cnd_signal(&thread_entry->thread_queue_not_empty_event); // waking the very first sleeping thread of the queue
		working_threads++; // a thread with a dedicated and valid directory he needs to enumerate is considered working
	}
	
	mtx_unlock(&protocol_lock);
	/* Synchronization block end */
}

void sync_dequeue(queue_entry_t *thread_entry) {
	
	/* Synchronization block start */
	mtx_lock(&protocol_lock);
	
	
	if ( !threads_finished ) { // if there is more work to do, find a directory to process
		if ( dir_queue.size != 0 ) { // if there are directories available process, remove first directory at <dir_queue>
			dequeue(&dir_queue, &thread_entry->dir_queue_entry);
			working_threads++; // we have a directory to enumerate, hence this thread is working
		} else { // if there are no directories currently available to process, go to sleep
			// adding this thread to the waiting list
			waiting_threads++;
			enqueue(&waiting_threads_queue, thread_entry);
			
			// wait for an enqueuer to wake you up with a task
			// printf("sleeping: %lu, working: %u, waiting: %u, tasks: %u\n", thrd_current(), working_threads, waiting_threads_queue.size, dir_queue.size);
			cnd_wait(&thread_entry->thread_queue_not_empty_event, &protocol_lock);
			// printf("awakening: %lu, working: %u, waiting: %u, tasks: %u\n", thrd_current(), working_threads, waiting_threads_queue.size, dir_queue.size);
			waiting_threads--;
		}
	}
	

	// printf("processing: %lu, working: %u, waiting: %u, tasks: %u\n", thrd_current(), working_threads, waiting_threads_queue.size, dir_queue.size);
	/* Synchronization block end */
	mtx_unlock(&protocol_lock);
}

bool check_to_terminate(void) {
	mtx_lock(&protocol_lock);
	
	// after enumerating the directory, we had done working, so we decrease this global variable (which was incremented at sync_dequeue)
	working_threads--;
	
	if (threads_finished) { // if a thread has already indicated to everyone that the work is done
		mtx_unlock(&protocol_lock);
		return true;
	}
	
	if (is_finished()) { // if no thread has indicated that the work is done
		threads_finished = true;
		release_all_threads();
		mtx_unlock(&protocol_lock);
		return true;
	}
	mtx_unlock(&protocol_lock);
	
	return false;
}
/*********************************************************************************************/









/****************************** PROTOCOL'S API'S AUXILIARY MODULE ********************************/
void dir_enum(DIR *dir, char dir_path[], char pattern[]) {
	struct dirent *entry;
	
	while ( NULL != (entry = readdir(dir)) ) { // reading next dirent // error here
		/* Eliminating recurssive dir entries */
		if ( (strcmp(entry->d_name, ".") == 0) || (strcmp(entry->d_name, "..") == 0) ) continue;
		
		/* Handling the entry in-itsef */
		handle_dirent(dir_path, entry->d_name, pattern);
	}	
	
	// close the open dirent after use to free up the fd table
	if (0 != closedir(dir)) print_err("Thread: Couldn't close an open dirent", true, false);
}

void handle_dirent(char dir_path[], char dirent_name[], char pattern[]) {
	struct stat dirent_statbuf; // used to store data induced from `stat`-ing files

	tmp++;

	/* Joining the path of the directory and the <dirent>'s */
	char* dirent_path;
	append_path(dir_path, dirent_name, &dirent_path);
	
	/* Getting the file type of the dirent */
	if (0 != lstat(dirent_path, &dirent_statbuf)) { // TODO: change to stat
		print_err("Error with `stat`-ing a dirent", true, false);
	}
	
	/* handle new entry */
	if (S_ISDIR(dirent_statbuf.st_mode)) { //If the entry points to a directory
		handle_new_dir(dirent_path); // handles the dirent_full_path memory
	} else { // If the entry doesn't point to a directory
		handle_new_file(dirent_name, dirent_path, pattern); // handles the dirent_full_path memory
	}
}

int handle_new_dir(char *dirent_path) {
	queue_entry_t *new_dir;
	new_dir = (queue_entry_t*) calloc(1, sizeof(queue_entry_t));
	new_dir->path = dirent_path;
	
	if ( NULL == (new_dir->dir = opendir(new_dir->path)) ) { // if an error occurred
	
		free(dirent_path); // no more use to the dirent_path
		free(new_dir); // no more use to the new directory's queue entry
	
		if (errno != EACCES) { // errors other than no permissions are treated as errors
			print_err("Error with using the `opendir` command on a new found directory", true, false);
		} else { // simple permissions denial stdout message
			printf("Directory %s: Permission denied.\n", dirent_path);
			return -1;
		}
		
	} else { // if opendir succeeded, we must have enough permissions to search the directory, so we enqueue it
		sync_enqueue(new_dir);
	}
	
	return 0;
}

void handle_new_file(char dirent_name[], char dirent_path[], char pattern[]) {
	// strstr is thread-safe
	if (NULL != strstr(dirent_name, pattern)) { // if <pattern> is in <dirent_name>
		pattern_matches++;
		printf("%s\n", dirent_path);
		free(dirent_path); // no more use to the dirent_path
	}
}

bool is_finished(void) {
	// all other threads are waiting and queue is empty (must be called after finishing the own work of the thread)
	bool ret = (working_threads == 0) && (dir_queue.size == 0);
	return ret;
}
/*******************************************************************************************/









/****************************** REGULAR AUXILIARY FUNCTIONS ********************************/
void print_err(char error_message[], bool thread_exit, bool main_exit) {
	int tmp_errno = errno;
	perror(error_message); // this basically prints error_message, with <strerror(errno)> appended to it */
	errno = tmp_errno;
	
	if (thread_exit) {	
		mtx_lock(&protocol_lock);
		// check if there is need to terminate the program after exiting this thread
		check_to_terminate();
		mtx_unlock(&protocol_lock);
		failed_threads++;
		thrd_exit(1); // exiting the thread
	}
	
	if (main_exit) {
		exit(1);
	}
}

void enqueue(queue_t *queue, queue_entry_t *entry) {
	if (queue->size == 0) {
		queue->head = entry;
		
	} else {
		queue->tail->next = entry;
	}
	
	queue->tail = entry;
	queue->tail->next = NULL;
	queue->size++;
}

void dequeue(queue_t *queue, queue_entry_t **entry) {
	if (queue->size == 0) {
		*entry = NULL;
	} else {
		*entry = queue->head;
		queue->head = queue->head->next;
		queue->size--;
	}
}

void append_path(char dir_path[], char dirent_name[], char** dirent_path) {
	unsigned int dirent_path_len = strlen(dir_path) + 1 + strlen(dirent_name) + 1; // 1 for the backslash and another for the null-terminator
	
	// allocating memory for the new path
	*dirent_path = malloc( sizeof(char) * dirent_path_len );
	if (NULL == *dirent_path) print_err("Thread: Couldn't allocate memory", true, false); 
	
	// building the new path
	sprintf(*dirent_path, "%s/%s", dir_path, dirent_name); // buffer-overflow vulnerability
	(*dirent_path)[dirent_path_len - 1] = 0;
}
/*******************************************************************************************/








/****************************** MAIN ****************************/
int main(int args, char* argv[]) {
	int exit_status = 0;
	
	// checking for the correct amount of arguments
	if (args != 4) {
		errno = EINVAL;
		print_err("Main Thread: Not enough arguments", false, true); // verify correpondentness to instructions
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
		print_err("Main Thread: Couldn't open root directory", false, true);
	}
	enqueue(&dir_queue, root_dir);
	
	// create all threads and wait for all of them to be created
	if ( 0 > atomic_create_threads(num_threads, thread_ids, pattern) ) {
		print_err("Main Thread: Couldn't create a thread", false, true);
	}
	
	// signal the threads to start working
	threads_started = true;
	cnd_broadcast(&threads_start_event);
	
	// wait for all threads to be finish working (if all are waiting, one of the searching threads will terminate the whole program)
	if (0 != atomic_join_threads(num_threads, thread_ids)) print_err("Main Thread: Couldn't join a thread", false, false);
	
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
