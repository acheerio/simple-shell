/*
* smallsh.c
* Alice O'Herin
* CS344 Winter 2019
* Mar 3, 2019
*/

/* 
* Simple shell program
*/


/********************
 * LIBRARIES
 ********************/
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>


/********************
 * MACROS
 ********************/
#define MAXARGS 512						// max number of arguments including command
#define ARGLEN 100						// max length of each argument (inc input/output files)
#define LINELEN 2048					// max input length (includes null terminator)
#define MAXBGPROC 256					// max number of running background processes

/********************
 * STRUCTS AND ENUMS
 ********************/
typedef enum bool {FALSE, TRUE} bool;


/********************
 * GLOBAL VARIABLES
 ********************/
int bgPids[MAXBGPROC];					// array of current background proc ids
int bgPidCount = 0;						// count of current background processes
int status = 0;							// exit status for most recent foreground proc
bool bgallow = TRUE;					// mode, whether background processes allowed
bool fg_running = FALSE;				// whether current foreground process is running
bool fg_chmode_pending = FALSE;			// signal received, fg-only mode needs to be toggled


/********************
 * FUNCTION DECLARATIONS
 ********************/
 int getInput(char *str);
 void replacePid(char *str);
 int parseInput(char *str, char *args[], char *in, char *out, bool *bg);
 void printExit(int exmethod, bool fg);
 int run_bic(char *command, char *arg);
 void killbgp();
 void rmBgPid(int pid);
 void changeFgMode();
 void run_all(char *args[], char *src, char *dest, bool bg);
 void checkBg();
 void catchSIGTSTP(int signo);
 void cleanup(char *args[], int arg_count);
 
 
/* NAME
 *  getInput
 * SYNOPSYS 
 * 	Gets input string ending in newline
 *	Returns -1 if getline failed or comment (#) or blank (newline only)
 *  Else removes newline, passes string back by argument, returns 0
 */
// does not remove a line of only spaces
int getInput(char *str)
{
	// get user input
	char *line = NULL;
	size_t len = 0;
	int n = getline(&line, &len, stdin);
	// check for getline failed, comment, or blank line
	if (n == -1 || line[0] == '#' || line[0] == '\n')
	{
		str[0] = '\0';
		free(line);
		return -1;
	}
	// otherwise, remove newline and return updated str
	else
	{		
		// remove newline
		line[strcspn(line, "\n")] = '\0';
		// copy, clean up, return
		strcpy(str, line);
		free(line);
		return 0;
	}
}


/* NAME
 *  replacePid
 * SYNOPSYS 
 * 	replaces instances of $$ in provided string with process ID
 *  assumes given str has space to hold expanded string with PIDs
 */
void replacePid(char *str)
{
	char temp[strlen(str)];		// temp holds newly expanded string
	memset(temp, '\0', sizeof(temp));
	int i;						// counter for original string
	int ti = 0;					// counter for expanded string
	int pid = getpid();			// pid as int
	char id[8];					// pid as string, max PID by default is 32768
	sprintf(id, "%d", pid);		// convert pid int -> string
	
	// insert PID for every instance of $$
	for (i = 0; i < strlen(str) + 1; i++)
	{
		// if not $ character, copy directly
		if (str[i] != '$')
		{
			temp[ti] = str[i];
			ti++;
		}
		// if find $ char
		else
		{
			// if next char is also $
			if (i < strlen(str) && str[i + 1] == '$')
			{
				// copy pid into temp, exclude null terminator
				int j;
				for (j = 0; j < strlen(id); j++)
				{
					temp[ti] = id[j];
					ti++;
				}
				i++;
			}
		}
	}
	strcpy(str, temp);
}


/* NAME
 *  parseInput
 * SYNOPSYS 
 * 	parses string for input file, output file, background mode
 *	and arguments. Returns number of arguments (incudes command).
 *  Expected Format: command [arg1 arg2 ...] [< input_file] [> output_file] [&]
 */
int parseInput(char *str, char *args[], char *in, char *out, bool *bg)
{
	int arg_count = 0;
	bool is_in = FALSE; 	// whether next/upcoming arg is the input source
	bool is_out = FALSE; 	// whether next/upcoming arg is the output destination
	
	// check if last word is "&" for bg mode
	int lastIndex = strlen(str) - 1;
	if ((str[lastIndex] == '&') &&
	(lastIndex == 0 || str[lastIndex - 1] == ' '))
	{
		*bg = TRUE;
	}
	
	// string tokenizer
	char *tok = strtok(str, " ");
	while (tok != NULL)
	{
		if (is_in == TRUE)
		{
			strcpy(in, tok);
			is_in = FALSE;
		}
		else if (is_out == TRUE)
		{
			strcpy(out, tok);
			is_out = FALSE;
		}
		else if (strcmp(tok, "<") == 0)
			is_in = TRUE;
		else if (strcmp(tok, ">") == 0)
			is_out = TRUE;
		// take no additional action for "&" at end of line
		else if ((strcmp(tok, "&") == 0) && *bg == TRUE)
			;
		// & not at end of line is treated as (part of) an argument
		else
		{
			args[arg_count] = malloc(ARGLEN * sizeof(char));
			strcpy(args[arg_count], tok);
			arg_count++;
		}
		tok = strtok(NULL, " ");
	}
	return arg_count;
}


/* NAME
 *  printExit
 * SYNOPSYS 
 * 	prints exit status for status, background or terminated processes.
 */
void printExit(int exmethod, bool nonStatusFg)
{
	// check if normal termination
	if (WIFEXITED(exmethod))
	{
		int exstatus = WEXITSTATUS(exmethod);
		// only print for bg processes or "status"
		if (!nonStatusFg)
		{
			printf("exit value %d\n", exstatus);
			fflush(stdout);
		}
	}
	// else, terminated by signal
	else if (WIFSIGNALED(exmethod))
	{
		int termsig = WTERMSIG(exmethod);
		printf("terminated by signal %d\n", termsig);
		fflush(stdout);
	}
}


/* NAME
 *  run_bic
 * SYNOPSYS 
 * 	Checks to see if command is built-in command "exit" "cd" or "status".
 *  arg is the directory to change directory to, or NULL.
 *	Returns -1 for exit, 0 for cd or status, 1 for no match (not a built-in command)
 */
int run_bic(char *command, char *arg)
{
	if (strcmp(command, "exit") == 0)
	{
		return -1;
	}
	else if (strcmp(command, "cd") == 0)
	{
		int result;
		// cd with no other arguments defaults to HOME directory
		if (arg == NULL)
		{
			result = chdir(getenv("HOME"));
		}
		else
		{
			result = chdir(arg);
		}
		if (result == -1)
		{
			perror(arg);
		}
		return 0;
	}
	else if (strcmp(command, "status") == 0)
	{
		printExit(status, FALSE);
		return 0;
	}
	else
		return 1;
}


/* NAME
 *  killbgp
 * SYNOPSYS 
 * 	Sends SIGKILL to all current background processes
 */
void killbgp()
{
	int i;
	for (i = 0; i < bgPidCount; i++)
	{
		kill(bgPids[i], SIGKILL);
	}
}


/* NAME
 *  rmBgPid
 * SYNOPSYS 
 * 	removes the given process id from the list of
 *	currently running background processes.
 *  processes are not stored in any guaranteed order.
 */
void rmBgPid(int pid)
{
	int i;
	for (i = 0; i < bgPidCount; i++)
	{
		if (bgPids[i] == pid)
		{
			bgPids[i] = bgPids[bgPidCount - 1];
			bgPids[bgPidCount] = 0;
			break;
		}
	}
	bgPidCount--;
}


/* NAME
 *  changeFgMode
 * SYNOPSYS 
 * 	toggles between foreground-only and regular mode which permits
 *	background processes, and prints message to stdout. 
 *  fg_chmode_pending is reset to FALSE.
 */
void changeFgMode()
{
	// if normal mode, change to fg-only mode
	if (bgallow)
	{
		bgallow = 0;
		char* msg = "\nEntering foreground-only mode (& is now ignored)\n";
		write(STDOUT_FILENO, msg, 50);
		fflush(stdout);
	}
	// if fg-only mode, change back to normal mode
	else
	{
		bgallow = 1;
		char* msg = "\nExiting foreground-only mode\n";
		write(STDOUT_FILENO, msg, 30);
		fflush(stdout);
	}
	// signal received at command line, not during fg process
	if (!fg_chmode_pending)
	{
		write(STDOUT_FILENO, ": ", 2);
		fflush(stdout);
	}
	// reset
	fg_chmode_pending = FALSE;
}


/* NAME
 *  run_all
 * SYNOPSYS 
 * 	forks to redirect input/out, then calls exec() using args[].
 *	if src and dest are not specified for a background process,
 *  input and output will both be redirected to /dev/null.
 */
void run_all(char *args[], char *src, char *dest, bool bg)
{
	pid_t childPid = -5;
	int childExit = -5;
	int fd_in = -5;
	int fd_out = -5;
	int result;
	
	// fg_running set before fork() for parent to access
	if (!bgallow || !bg)
		fg_running = TRUE;
	
	childPid = fork();
	switch (childPid)
	{
		// error when forking
		case -1:
		{
			perror("fork()");
			return;
		}
		// child process
		case 0:
		{
			// ignore Ctrl-Z (SIGTSTP) (to be inherited in exec()), don't stop
			struct sigaction SIGTSTP_action = {0};
			SIGTSTP_action.sa_handler = SIG_IGN;
			sigfillset(&SIGTSTP_action.sa_mask);
			SIGTSTP_action.sa_flags = SA_RESTART;
			sigaction(SIGTSTP, &SIGTSTP_action, NULL);
			
			// allow fg processes to terminate at Ctrl-C (SIGINT)
			if (!bgallow || !bg)
			{
				// printf("Setting SIGINT to default\n");
				struct sigaction SIGINT_action = {0};
				SIGINT_action.sa_handler = SIG_DFL;
				sigfillset(&SIGINT_action.sa_mask);
				SIGINT_action.sa_flags = SA_RESTART;
				sigaction(SIGINT, &SIGINT_action, NULL);
			}
			
			// input redirection
			// if source unspecified for bg process, open stream /dev/null
			if (src == NULL || (strcmp(src, "") == 0))
			{
				if (bgallow && bg)
				{
					fd_in = open("/dev/null", O_RDONLY);
					if (fd_in == -1)
					{
						perror("open() /dev/null");
						exit(1);
					}
				}
			}
			// else, if source is specified, open file
			else
			{
				fd_in = open(src, O_RDONLY);
				if (fd_in == -1)
				{
					fprintf(stderr, "cannot open %s for input\n", src);
					exit(1);
				}
			}
			// if source was changed, either to dev/null or specified path, redirect
			if (fd_in != -5)
			{
				// close file stream on execution
				fcntl(fd_in, F_SETFD, FD_CLOEXEC);
				// redirect input
				result = dup2(fd_in, 0);
				if (result == -1)
				{
					perror("dup2() src");
					exit(1);
				}
			}
			
			// output redirection
			// if dest unspecified for bg process, open stream /dev/null
			if (dest == NULL || (strcmp(dest, "") == 0))
			{
				if (bgallow && bg)
				{
					fd_out = open("/dev/null", O_WRONLY);
					if (fd_out == -1)
					{
						perror("open() /dev/null");
						exit(1);
					}
				}
			}
			// else, if dest is specified, open file
			else
			{
				fd_out = open(dest, O_WRONLY | O_TRUNC | O_CREAT, S_IRWXU);
				if (fd_out == -1)
				{
					perror("open() dest");
					exit(1);
				}				
			}
			// if dest was changed, either to dev/null or specified path, redirect
			if (fd_out != -5)
			{
				fcntl(fd_out, F_SETFD, FD_CLOEXEC);
				result = dup2(fd_out, 1);
				if (result == -1)
				{
					perror("dup2() dest");
					exit(1);
				}
			}
			
			// exec() command and arguments
			if (execvp(args[0], args) < 0) {
				perror(args[0]);
				exit(1);
			}
			break;
		}
		// parent process
		default:
		{
			// if child process is running in bg mode, report pid, add to array
			if (bgallow && bg)
			{
				// could be confusing if cmd is: badfile &
				// the error msg may return before the bg pid
				printf("background pid is %d\n", childPid);
				fflush(stdout);
				bgPids[bgPidCount] = childPid;
				bgPidCount++;
			}
			// wait for foreground process to complete, print status if terminated
			else
			{
				childPid = waitpid(childPid, &status, 0);
				fg_running = FALSE;
				printExit(status, TRUE);
			}
			// toggle fg/bg mode change if SIGTSTP raised during process
			if (fg_chmode_pending)
			{
				changeFgMode();
			}
		}
	}
	return;
}


/* NAME
 *  checkBg
 * SYNOPSYS 
 * 	checks for completed background processes
 *  updates array, prints status
 */
void checkBg()
{
	int method;
	pid_t check;
	while ((check = waitpid(-1, &method, WNOHANG)) > 0)
	{
		printf("background pid %d is done: ", check);
		fflush(stdout);
		rmBgPid(check);
		printExit(method, FALSE);
	}
}


/* NAME
 *  catchSIGTSTP
 * SYNOPSYS 
 * 	signal handler for SIGTSTP (Ctrl-Z)
 *	changes to foreground-only mode or (if foreground process running)
 *  sets fg_chmode_pending to TRUE
 */
void catchSIGTSTP(int signo)
{
	if (!fg_running)
	{
		changeFgMode();
	}
	else
		fg_chmode_pending = TRUE;
}


/* NAME
 *  cleanup
 * SYNOPSYS 
 * 	frees memory in args array
 */
void cleanup(char *args[], int arg_count)
{
	// cleanup
	int j;
	for (j = 0; j < arg_count; j++)
	{
		free(args[j]);
		args[j] = NULL;
	}
}


/********************
 * MAIN
 ********************/
/* NAME
 *  main
 * SYNOPSYS 
 * 	simple bash shell, prompts with ": ",
 *  checks bg processes for completion before prompt
 */
int main(int argc, char *argv[]) {
	
	// printf("Process ID: %d\n", getpid());
		
	// Ctrl-C does not terminate smallsh
	struct sigaction SIGINT_action = {0};
	SIGINT_action.sa_handler = SIG_IGN;
	sigfillset(&SIGINT_action.sa_mask);
	SIGINT_action.sa_flags = SA_RESTART;
	sigaction(SIGINT, &SIGINT_action, NULL);
	
	// Ctrl-Z toggles foreground-only mode on/off
	struct sigaction SIGTSTP_action = {0};
	SIGTSTP_action.sa_handler = catchSIGTSTP;
	sigfillset(&SIGTSTP_action.sa_mask);
	SIGTSTP_action.sa_flags = SA_RESTART;
	sigaction(SIGTSTP, &SIGTSTP_action, NULL);
	
	char line[LINELEN];
	char *args[MAXARGS + 2];		// arg[0] is command, e.g. "ls", ends with NULL
	char in[100];					// input redirect source
	char out[100];					// output redirect destination
	bool bg;						// run in bg
	int arg_count = 0;					// number of arguments
	
	// initialize array
	int i;
	for (i = 0; i < MAXARGS + 2; i++)
	{
		args[i] = NULL;
	}
	
	// loop prompt
	while (1)
	{
		cleanup(args, arg_count);
		// check for completed bg processes
		checkBg();
		
		// initialize, reset variables
		memset(line, '\0', sizeof(line));
		memset(in, '\0', sizeof(in));
		memset(out, '\0', sizeof(out));
		bg = FALSE;
		arg_count = 0;
		
		// prompt for input
		printf(": ");
		fflush(stdout);
		if (getInput(line) == -1)
			continue;
		
		// implied else: if successfully got input, replace $$ with pid
		replacePid(line);
		
		// parse input for input, output, bg mode, arguments
		arg_count = parseInput(line, args, in, out, &bg);
		// if the input was only spaces, continue
		if (arg_count == 0)
			continue;
		
		// check for / run built-in commands, if any
		int builtin_cmd = run_bic(args[0], args[1]);
		// exit was called
		if (builtin_cmd == -1) {
			killbgp();
			break;
		}
		
		// if no built-in command matched, run using PATH
		if (builtin_cmd == 1) {
			run_all(args, in, out, bg);
		}
	}
	cleanup(args, arg_count);
	return 0;
}