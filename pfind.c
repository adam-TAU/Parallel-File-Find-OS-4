


#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdatomic.h>
#include <pthreads.h>







/* Submitter: Adam
 * Tel Aviv University
 * Operating Systems, 2022A
 ===========================
 * Parallel File Find
 * Compile with: gcc -O3 -D_POSIX_C_SOURCE=200809 -Wall -std=c11 -pthread pfind.c 
 */






/****************************** AUXILIARY FUNCTIONS DECLARATIONS ****************************/
void print_err(char* error_message, bool terminate);
/*******************************************************************************************/









/****************************** AUXILIARY FUNCTIONS DEFINITIONS ****************************/

/*******************************************************************************************/











/****************************** MAIN ****************************/
int main(int args, char* argv[]) {
	
	// checking for the correct amount of arguments
	if (args != 3) {
		print_err("Not enough arguments!", true);
	}
	
	// fetching data
	char* root_dir = argv[1];
	char* pattern = argv[2];
	unsigned int num_threads = atoi(argv[3]);
	
	//
	
	
}
/***************************************************************/
