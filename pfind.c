/* Submitter: Adam
 * Tel Aviv University
 * Operating Systems, 2022A
 ===========================
 * Parallel File Find
 * Compile with: gcc -O3 -D_POSIX_C_SOURCE=200809 -Wall -std=c11 -pthread pfind.c 
 */




/****************************** HIGHLIGHTS: TODO ****************************
	1. If queue is empty, check if all other threads are waiting. If so, terminate everything. 
	2. Don't busy wait. Use yield_cpu if waiting for dequeue of empty queue.
	3. Use readdir() to iterate through each dirent directory entry (if "." or ".." ignore). If a directory entry succeeds a opendir()
	call, then it can be searched and should be added to the queue - in this case if threads are sleeping waiting for work, wake on up.
	from the given root directory at the main function. Assume the path is no longer than PATH_MAX.
	
	5. If an error occurs in the main thread, print an error to stderr and exit the program with exit code 1.
	6. If an error occurs in a searching thread, print an error to stderr and exit the thread only.
	7. The program should exit if all searching thread exited, or if no directories are left to search (all threads waiting).
	11. No need to check for errors in calls to mtx_* or cnd* functions (currently irrelevant)
	12. no deadlocks (currently irrelevant)
	13. The first to sleep, the one to get the first directory in the queue (currently irrelevant)
	14. The thread should sleep only if the queue was empty upon arrival (currently irrelevant)
****************************** HIGHLIGHTS: TODO ****************************/


#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdatomic.h>
#include <pthread.h>
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
	struct queue_entry_t *next;
} queue_entry_t;

typedef struct queue_t {
	queue_entry_t head;
	queue_entry_t tail;
	unsigned int size;
} queue_t;
/*******************************************************************/





/****************************** GLOBAL VARS ****************************/
static queue_t dir_queue = {0};
static atomic_uint pattern_matches = 0; // the number of files that have been found to contain the pattern
static atomic_uint waiting_threads = 0; // the number of threads that are currently waiting for tasks
static atomic_uint running_threads = 0; // the number of threads that haven't encountered an error
static pthread_rwlock_t shared_objects_rw_lock; // single-writer multiple readers lock for all shared objects
static pthread_mutex_t queue_lock; // mutual excluder for queue operations (should find a better use than that)
static pthread_cond_t queue_not_empty; // condition for a non-empty queue
static pthread_mutex_t thread_creation_lock;
static pthread_cond_t thread_created; // condition for identifying a thread creation
/***********************************************************************/





/****************************** AUXILIARY FUNCTIONS DECLARATIONS ****************************/
/* Handle an error, print the corresponding error message, and terminate if needed. Thread-safe
 * since `perror` is thread-safe. */
void print_err(char* error_message, bool thrd_exit, bool program_exit);

/* Accepts a number of desired threads, and tries to create them while saving their thread-ids
 * into the given array of <pthread_t>-s.
 
 * Doesn't synchronize anything (allows for termination upon finding an error)
 * On success, returns 0. */
int atomic_create_threads(unsigned int num_threads, pthread_t* thread_ids, char pattern[]);

/* Accepts an array of thread_ids, and joins every thread by its thread-id. */
int join_threads(pthread_t* thread_ids);

/* A function dedicated to be ran by the auxiliar threads. This function fetches a directory
 * from the aforesaid queue and enumerates it for files who hold the pattern we're search for */ 
void *thrd_reap_directories(void* pattern);

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
void append_path(char path[], char dirent_name[], char** new_path);

/* A function dedicated to be ran by an auxiliary thread. This function accepts a string (the 
 * directory name in our case), and enqueues it atomically into the given queue. 
  
 * On success, returns 0. */
int enqueue(queue_t queue, queue_entry_t *entry);

/* A function dedicated to be ran by an auxiliary thread. This function accepts a string (the 
 * directory name in our case), and atomically dequeues the first entry of the given queue 
 * into the string. 
  
 * On success, returns 0. */
int dequeue(queue_t queue, queue_entry_t *entry);

/* Checks if all searching threads that are alive are waiting for tasks.
 * If so, the program shall finish due to no work left, therefore this function
 * will return true. Else, returns false. */
bool is_finished(void);
/*******************************************************************************************/





/****************************** AUXILIARY FUNCTIONS DEFINITIONS ****************************/
void print_err(char* error_message, bool thrd_exit, bool program_exit) {
	int tmp_errno = errno;
	perror(error_message); // this basically prints error_message, with <strerror(errno)> appended to it */
	errno = tmp_errno;
	
	if (thrd_exit) {
		pthread_rwlock_rdlock(&shared_objects_rw_lock); // engulfing write with wr lock
		waiting_threads--;
		running_threads--;
		pthread_rwlock_unlock(&shared_objects_rw_lock); // degulfing write with wr lock
		pthread_exit(NULL); // exiting the thread
	}
	
	if (program_exit) {
		exit(1);
	}
}

int atomic_create_threads(unsigned int num_threads, pthread_t* thread_ids, char pattern[]) {
	unsigned int i;
	
	/* Creating the threads */
	for (i = 0; i < num_threads; i++) {
		if (0 != pthread_create(&thread_ids[i], NULL, thrd_reap_directories, (void*) pattern)) {
			return -1;
		}
	}
	
	/* Waiting for all of them to signal that they've been created */
	for (i = 0; i < num_threads; i++) {
		pthread_mutex_lock(&thread_creation_lock);
		if (running_threads != num_threads) {
			pthread_cond_wait(&thread_created, &thread_creation_lock);
		}
		pthread_mutex_unlock(&thread_creation_lock);
	}
	
	return 0;
}

int join_threads(pthread_t* thread_ids) {
	return 0;
}

void *thrd_reap_directories(void* pattern) {
	queue_entry_t curr_dir_entry;
	
	while (true) {
		dequeue(dir_queue, &curr_dir_entry); // thread-safe
		dir_enum(curr_dir_entry.dir, curr_dir_entry.path, (char*) pattern); // thread-safe
	}
}

void dir_enum(DIR *dir, char path[], char pattern[]) {
	struct dirent *entry;

	while ( NULL != (entry = readdir(dir)) ) { // reading next dirent
		/* Eliminating recurssive dir entries */
		if ( (strcmp(entry->d_name, ".") == 0) || (strcmp(entry->d_name, "..") == 0) ) continue;
		
		/* Joining the path of the directory and the <dirent>'s */
		char* entry_path;
		append_path(path, entry->d_name, &entry_path);
		
		/* Getting the file type of the dirent */
		struct stat entry_statbuf;
		if (0 != stat(entry_path, &entry_statbuf)) {
			print_err("Error `stat`-ing a dirent", true, false);
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
}

void handle_new_dir(char path[]) {
	queue_entry_t new_dir;

	if ( NULL == (new_dir.dir = opendir(path)) ) { // if an error occurred
	
		if (errno != EACCES) { // errors other than no permissions are treated as errors
			print_err("Error with using the `opendir` command on a new found directory", true, false);
		} else { // simple permissions denial stdout message
			printf("Directory %s: Permission denied.\n", path); 
		}
		
	} else { // if opendir succeeded, we must have enough permissions to search the directory, so we enqueue it
		new_dir.path = path;
		enqueue(dir_queue, &new_dir);
	}
}

void handle_new_file(char filename[], char path[], char pattern[]) {
	if (NULL != strstr(filename, pattern)) {
		pattern_matches++;
		printf("%s\n", path);
	}
}

void append_path(char path[], char dirent_name[], char** new_path) {
	*new_path = (char*) malloc( sizeof(char) * (strlen(path) + strlen(dirent_name) + 1) );
	sprintf(*new_path, "%s/%s", path, dirent_name);
}

int enqueue(queue_t queue, queue_entry_t *entry) {
	int status = 0;
	
	/* Synchronization block start (can't terminate upon error, must engolf with status var */
	pthread_mutex_lock(&queue_lock);
	/* Add entry to queue */
	pthread_cond_signal(&queue_not_empty);
	pthread_mutex_unlock(&queue_lock);
	/* Synchronization block end */
	
	return status;
	
}

int dequeue(queue_t queue, queue_entry_t *entry) {
	int status = 0;

	/* Synchronization block start (can't terminate upon error, must engolf with status var */
	pthread_mutex_lock(&queue_lock);
	while (true) {
		pthread_cond_wait(&queue_not_empty, &queue_lock);
	}
	/* remove entry from queue into given entry pointer */
	pthread_mutex_unlock(&queue_lock);
	/* Synchronization block end */
	
	return status;
}

bool is_finished(void) {
	bool ret = false;

	/* Safely read the amount of running threads and the amount of waiting threads */
	pthread_rwlock_rdlock(&shared_objects_rw_lock);
	ret = (waiting_threads == running_threads) ? true : false;
	pthread_rwlock_unlock(&shared_objects_rw_lock);
	
	/* Return value */
	return ret;
}
/*******************************************************************************************/





/****************************** MAIN ****************************/
int main(int args, char* argv[]) {
	
	// checking for the correct amount of arguments
	if (args != 3) {
		print_err("Not enough arguments!", false, true); // verify correpondentness to instructions
		exit(1);
	}
	
	// fetching data
	char* root_dir_path = argv[1];
	char* pattern = argv[2];
	unsigned int num_threads = atoi(argv[3]);
	
	// parsing the data into our thread-global variables
	running_threads = num_threads;
	waiting_threads = 0;
	
	// create array to store thread-ids in
	pthread_t thread_ids[num_threads];
	
	// create all threads and wait for all of them to be created
	if ( 0 > atomic_create_threads(num_threads, thread_ids, pattern) ) {
		print_err("Error with successfuly creating the threads", false, true);
	}
	
	// enqueue the root directory
	queue_entry_t root_dir;
	root_dir.path = root_dir_path;
	root_dir.next = NULL;
	enqueue(dir_queue, &root_dir);
	
	// wait for all threads to be waiting for work / running_threads == 0 (in that case we are able to finish the program
	join_threads(thread_ids);
	
	// print the amount of matches files
	printf("Done searching, found %d files\n", pattern_matches);
	
	// exit with the correct exit status
	if (running_threads < num_threads) {
		exit(1);
	}
	exit(0);
}
/***************************************************************/
