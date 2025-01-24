#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>


//struct for command line entry
struct command {

    char commandStr[2048];
    char *args[512];
    char *inputFile;
    char *outputFile;
    bool background;
};

//global var for signal handling purposes
bool foregroundOnly = false;

//prototypes
void prompt(int *lastExitStatus, pid_t bgIDs[], int *bgIDsIdx);
void checkBackgroundStatus(pid_t bgIDs[], int *bgIDsIdx);
bool checkBackground(char *args[]);
void tokenize(char command[], char *args[]);
void checkRedirection(char *args[], char **inputFile, char **outputFile);

bool varExpansion(char *command);

//built in commands
void exitSmallsh();
void changeDirectory(char *args[]);
void getStatus(int *lastExitStatus);

void execCommand(bool hasExpansion, bool background, char *args[], 
    int *lastExitStatus, char *inputFile, char *outputFile, pid_t bgIDs[], int *bgIDsIdx);

/*
* Command prompt
*/
void prompt(int *lastExitStatus, pid_t bgIDs[], int *bgIDsIdx) {

    struct command *input = malloc(sizeof(struct command));

    //initially set "running in background" flag to 0/false (default to foreground)
    input->background = false;

    //initialize no input/output filenames
    input->inputFile = NULL;
    input->outputFile = NULL;

    //BEFORE PROMPTING: check periodically if any bg processes have completed
    checkBackgroundStatus(bgIDs, bgIDsIdx);

    //prompt
    printf(": ");
    fflush(stdout);
    fgets(input->commandStr, sizeof(input->commandStr), stdin);

    //remove trailing newline character that fgets includes
    size_t len = strlen(input->commandStr);
    if (len > 0 && input->commandStr[len - 1] == '\n') {
        input->commandStr[len - 1] = '\0';
    }

    //if empty line or comment, skip/reprompt
    if (input->commandStr[0] == '\0' || input->commandStr[0] == '#') {
        free(input);
        return;
    }

    //check for variable expansion of "$$" (also used later for execCommand)
    bool hasExpansion = varExpansion(input->commandStr);

    //tokenize the user input; split it up into individual arguments using space as delimiter
    tokenize(input->commandStr, input->args);

    //check if input and output redirection is present in command
    checkRedirection(input->args, &(input->inputFile), &(input->outputFile));

    //check for built-in commands
    if (strcmp(input->args[0], "exit") == 0) {
        free(input);
        exitSmallsh();
    } else if (strcmp(input->args[0], "cd") == 0) {
        changeDirectory(input->args);
        free(input);
    } else if (strcmp(input->args[0], "status") == 0) {
        getStatus(lastExitStatus);
        free(input);
    } else {
        //check for background condition; update struct member variable
        input->background = checkBackground(input->args);

        //execute commands (non built-in)
        execCommand(hasExpansion, input->background, input->args, 
        lastExitStatus, input->inputFile, input->outputFile, bgIDs, bgIDsIdx);
        free(input);
    }
}


/*
* Checks for completed background processes and prints termination message
*/
void checkBackgroundStatus(pid_t bgIDs[], int *bgIDsIdx) {
    int bgStatus;
    pid_t currBgID;

    //loop until no more completed child processes
    while ((currBgID = waitpid(-1, &bgStatus, WNOHANG)) > 0) {
        if (WIFEXITED(bgStatus)) {
            printf("background pid %d is done: exit value %d\n", currBgID, WEXITSTATUS(bgStatus));
        } else if (WIFSIGNALED(bgStatus)) {
            printf("background pid %d is done: terminated by signal %d\n", currBgID, WTERMSIG(bgStatus));
        }
        fflush(stdout);

        //rearrange array
        for (int i = 0; i < *bgIDsIdx; i++) {
            if (bgIDs[i] == currBgID) {
                for (int j = i; j < *bgIDsIdx - 1; j++) {
                    bgIDs[j] = bgIDs[j + 1];
                }
                (*bgIDsIdx)--;
                break;
            }
        }
    }
}



/*
* Checks if an ampersand is at end of command, indicating background execution mode
*/
bool checkBackground(char *args[]) {

    int i = 0;

    //navigate to last arg
    while (args[i] != NULL) {
        i++;
    }

    //check for background condition
    if (i > 0 && strcmp(args[i - 1], "&") == 0 &&
        strcmp(args[0], "cd") != 0 &&
        strcmp(args[0], "status") != 0 &&
        strcmp(args[0], "exit") != 0) {

        //remove the & from args list
        args[i - 1] = NULL;
        return true;
    }

    return false;
}


/*
* Extract specific commands from single command string
*/
void tokenize(char command[], char *args[]) {

    char delimiter[] = " ";
    char *token;

    int argIdx = 0;

    //get first token, i.e., first command in full command string
    token = strtok(command, delimiter);
    args[argIdx] = token;
    argIdx++;

    while (token != NULL) {
        token = strtok(NULL, delimiter);
        args[argIdx] = token;
        argIdx++;
    }

    //set index after last argument to NULL
    args[argIdx] = NULL;


}


/*
* Check if input/output redirection needs to be handled; populate struct members if so
*/
void checkRedirection(char *args[], char **inputFile, char **outputFile) {

    //new args buffer to store args without the < or > chars
    char *newArgs[512];

    int i = 0;
    int j = 0;
    while (args[i] != NULL) {

        //check for output redirection
        if (strcmp(args[i], ">") == 0) {
            
            //store output filename, then skip copying filename w/ i++
            *outputFile = args[i+1];
            i++;

        } else if (strcmp(args[i], "<") == 0) {
            
            //store input filename, then skip copying filename w/ i++
            *inputFile = args[i+1];
            i++;

        } else {
            //copy non < or > argument to new buffer, only increment j when doing so
            newArgs[j] = args[i];
            j++;
        }
        i++;
    }
    newArgs[j] = NULL;

    //copy newArgs back to args
    for (int k = 0; k <= j; k++) {
        args[k] = newArgs[k];
    }
}


/*
* Expands instances of "$$" in the command string into the current process id
*/
bool varExpansion(char *command) {

    //get the process id of smallsh
    pid_t pid = getpid();

    //convert pid to string
    char pid_str[20];
    sprintf(pid_str, "%d", pid);

    int pid_len = strlen(pid_str);
   
    char buffer[2048];       //used to store new string with pid spliced in
    int new_i = 0;          //used to track location within new string

    for (int i = 0; command[i] != '\0'; i++) {

        //if $$ found in command
        if (command[i] == '$' && command[i+1] == '$') {

            //copy process id digits down char by char
            for (int j = 0; j < pid_len; j++) {
                buffer[new_i++] = pid_str[j];
            }
            i++;

        } else {

            //copy char from command to buffer
            buffer[new_i] = command[i];

            new_i++;
        }

            
    }

    buffer[new_i] = '\0';

    if (strcmp(buffer, command) != 0) {
        strcpy(command, buffer);
        return true;
    }


    return false;
}


/*
* Function to implement exit command
*/
void exitSmallsh() {

    //exit the smallsh process
    exit(0);
}


/*
* Function to implement cd command
*/
void changeDirectory(char *args[]) {

    char currPath[100];
    
    //if command is just "cd" and no arguments
    if (args[0] != NULL && args[1] == NULL) {
        
        //change to home directory on server
        chdir("/nfs/stak/users/montgale");
        getcwd(currPath, 100);


    } else if (args[1][0] == '/') {         //if absolute path (slash as first char of second arg)
        

        char newpath[100];
        sprintf(newpath, "%s", args[1]);
        chdir(newpath);
        getcwd(currPath, 100);

    

    } else {                            //if relative path
        //printf("relative\n");

        char newpath[100];
        getcwd(currPath, 100);
        sprintf(newpath, "%s/%s", currPath, args[1]);
        chdir(newpath);
        getcwd(currPath, 100);
    }


}


/*
* Function to implement status command
*/
void getStatus(int *lastExitStatus) {

    if (*lastExitStatus == 0)
        printf("exit value 0\n");
    else if (*lastExitStatus == 1)
        printf("exit value 1\n");

}

/*
* Execute other commands; some code adapted from 4.2 Replit
* Takes flags for var expansion and background, agrs list, flag for last exit status,
* char strings for input/output redirection destinations, and array of unfinished background
* process IDs
*/
void execCommand(bool hasExpansion, bool background, char *args[], 
int *lastExitStatus, char *inputFile, char *outputFile, pid_t bgIDs[], int *bgIDsIdx) {

    int childStatus;

    //check if foreground only is true, then turn background off
    if (foregroundOnly) {
        background = false;
    }

    //fork new process
    int childPid = fork();


    if (childPid == -1) {
        perror("fork() failed!");
		exit(1);

    } else if (childPid == 0) {

        //child process

        checkBackgroundStatus(bgIDs, bgIDsIdx);

        //ignore SIGINT for background processes, default for foreground
        struct sigaction SIGINT_action = {0};
        SIGINT_action.sa_handler = background ? SIG_IGN : SIG_DFL;
        sigaction(SIGINT, &SIGINT_action, NULL);

        //if background and no input redirection specified
        if (background && inputFile == NULL) {
            int fd = open("/dev/null", O_RDONLY);
            if (fd == -1) {
                printf("cannot open /dev/null for input\n");
                exit(1);
            }
            int result = dup2(fd, 0);
            close(fd);
        }

        //if background and no output redirection specified
        if (background && outputFile == NULL) {
            int fd = open("/dev/null", O_WRONLY);
            if (fd == -1) {
                printf("cannot open /dev/null for output\n");
                exit(1);
            }
            int result = dup2(fd, 1);
            close(fd);
        }

        //if input redirection is needed
        if (inputFile != NULL) {
            int fd = open(inputFile, O_RDONLY);
            if (fd == -1) {
                printf("cannot open %s for input\n", inputFile);
                exit(1);
            }
            //redirect input to fd
            int result = dup2(fd, 0);
            close(fd);
        }


        //if output redirection is needed
        if (outputFile != NULL) {
            int fd = open(outputFile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd == -1) {
                printf("cannot open %s for output\n", outputFile);
                exit(1);
            }
            //redirect output to fd
            int result = dup2(fd, 1);
            close(fd);
        }
        
        execvp(args[0], args);
        perror(args[0]);
        exit(1);
        
    } else {

        //parent process


        //print background process id; also add it to the non-completed array
        if (background) {

            bgIDs[*bgIDsIdx] = childPid;
            (*bgIDsIdx)++;

            printf("background pid is %d\n", childPid);
            fflush(stdout);     //to make it print immediately

        } else {

            childPid = waitpid(childPid, &childStatus, 0);

            if (WIFEXITED(childStatus) && WEXITSTATUS(childStatus) != 0) {
                *lastExitStatus = 1;  //if exit code is nonzero, set to 1
            } else if (WIFSIGNALED(childStatus)) {
                *lastExitStatus = 1;  //if term by signal, set to 1
            } else {
                *lastExitStatus = 0;  //if successful exit
            }
        }
    }
}


/*
* Signal handler for SIGSTP
*/
void handle_SIGTSTP(int signo) {

    //if currently in foregroundOnly and ^Z received, switch out of foregroundOnly
    if (foregroundOnly) {
        write(STDOUT_FILENO, "\nExiting foreground-only mode\n: ", 31);
        foregroundOnly = false;

    //if not currently in foregroundOnly mode and ^Z received, turn on foregroundOnly mode
    } else {
        write(STDOUT_FILENO, "\nEntering foreground-only mode (& is now ignored)\n: ", 52);
        foregroundOnly = true;
    }
}


int main() {

    //make this the parent process for the group
    setpgid(0, 0);

    //to track the last exit status of processes, will be updated
    int lastExitStatus = 0;

    //handle SIGINT in parent process
    struct sigaction parent_SIGINT_action = {0};
    parent_SIGINT_action.sa_handler = SIG_IGN;
    sigaction(SIGINT, &parent_SIGINT_action, NULL);

    //handle SIGSTP in parent process
    struct sigaction SIGTSTP_action = {0};
    SIGTSTP_action.sa_handler = handle_SIGTSTP;
    sigfillset(&SIGTSTP_action.sa_mask);
    SIGTSTP_action.sa_flags = SA_RESTART;
    sigaction(SIGTSTP, &SIGTSTP_action, NULL);

    //array to keep track of unfinished background processes + number of them
    pid_t bgIDs[20];
    int bgIDsIdx = 0;

    //prompt loop until exit command is called
    while (1) {
        prompt(&lastExitStatus, bgIDs, &bgIDsIdx);
        usleep(10000);
    }
    
    return 0;
}