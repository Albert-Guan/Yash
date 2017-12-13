/*
   Xinyuan (Allen) Pan
   EE 461S Project 1
*/

#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <signal.h>

#define LINE_LIMIT 2000

int commandValidation(char *buffer);
void parseInput(char *args[], int *len, char *command[], int *commandLen, int *index);
int parseOperator(char *args[], int len, int index);
int findNextOperator(char *args[], int len, int index);
void executeCommand(char *args[], int background);
void signal_init(void);
void sig_handler(int signo);
void restoreIORedirection(void);

struct job {
   char **args;
   int len;
   int no;
};

struct jobs_struct {
   struct job joblist[100];
   int number;
} jobs;


int stdin_copy = 0,
stdout_copy = 1,
stderr_copy = 2,
file_in = -1,
file_out = -1,
file_err = -1;

int shell_pid;

int main(int argc, char const *argv[]) {
    signal_init();
    shell_pid = getpid();
    jobs.number = 0;

    while (1) {
        printf("%s", "# "); // prompt
        // accepts user input
        char buffer[LINE_LIMIT];
        char *ptr = fgets(buffer, LINE_LIMIT, stdin);
        if (ptr == NULL) break;
        char *buffer_cp = strdup(buffer);
        // split the input into tokens
        int len = 1;
        if (strtok(buffer_cp," \t\n") == NULL) len = 0;
        while (strtok(NULL," \t\n") != NULL) len++;
        char *args[len];
        args[0] = strtok(buffer," \t\n");
        for (int i=1; i<len; i++)
            args[i] = strtok(NULL, " \t\n");

        char *command[100]; // will be used to store the parsed command
        int commandLen = 0;
        command[commandLen] = NULL;
        int background = 0;
        // check if the command exist

        // start to parse the command
        int index = 0; // marks the beginning of the to be parsed input (or part of the input)


        if (!strcmp(args[len-1], "&")){
           background = 1;
           struct job temp = {args, len, jobs.number};
           jobs.joblist[jobs.number] = temp;
           jobs.number++;
           len--;
           parseInput(args, &len, command, &commandLen, &index);
        }
        else {
           parseInput(args, &len, command, &commandLen, &index);
        }
        // done parsing, execute the command
        if (index != -1) { // no error and not a pipe
            // check if command exists
            if (commandValidation(command[0])) {
                executeCommand(command, background);
            }
        }
        restoreIORedirection();
    }

}

/**
 Checks if the command is in the directories described by PATH
 and command must not be NULL
 */
int commandValidation(char *command) {
    if (command == NULL) return 0;
    int len = strlen(command);
    for (int i=0; i<len; i++) {
      command[i] = tolower(command[i]);
   }
    char *PATH = strdup(getenv("PATH"));
    char *path = strtok(PATH, ":");
    do {
        DIR *dp;
        struct dirent *d;
        if ((dp = opendir(path)) == NULL) {
            perror(path);
            continue;
        }
        while ((d = readdir(dp)) != NULL) {
            if (!strcmp(d->d_name, command)) {
                closedir(dp);
                return 1;
            }
        }
        closedir(dp);
        // check if the command is fg, bg, jobs; MAY NEED TO DELETE THIS
        if (!strcmp(command, "fg") | !strcmp(command, "bg") | !strcmp(command, "jobs"))
            return 1;
    } while ((path = strtok(NULL,":")) != NULL);

    // It may include the path
    if (command[0] == '.' || command[0] == '~' || command[0] == '/')
      return 1;

    restoreIORedirection();
    fprintf(stdout, "%s: %s\n", command, "command not found");  // unrecognized command, skip to the next command
    return 0;
}

void parseInput(char *args[], int *len, char *command[], int *commandLen, int *index) {

    while (*index < *len && *index != -1) {
        int op = findNextOperator(args, *len, *index); // index of the next operator
        if (op == -1) { // no operator
            if (*index == 0 && !strcmp(args[*index], "fg")) {

            }
            else if (*index == 0 && !strcmp(args[*index], "bg")) {

            }
            else if (*index == 0 && !strcmp(args[*index], "jobs")) {

            }
            else {  // does not contain operator and is not background commands, that means it's part of the command
                for (int i=*index; i<*len; i++) {
                    command[*commandLen] = strdup(args[i]);
                    (*commandLen)++;
                }
                command[*commandLen] = NULL;
                *index = *len;  // update the index to the end of the input; exits the loop
            }
        }
        else {  // contains operator(s)
            if (!strcmp(args[op], "|")) { // need to consider a pipe differently
                // first get everything before the pipe
                for (int i=*index;i<op;i++) {
                    command[*commandLen] = strdup(args[i]);
                    (*commandLen)++;
                }
                command[*commandLen] = NULL;
                *index = op+1;
                char *newCommand[100];
                int newCommandLen = 0;
                newCommand[newCommandLen] = NULL;
                parseInput(args, len, newCommand, &newCommandLen, index); // it is going to parse the second part of the pipe and fill out newCommand
                // check command

                if (commandValidation(command[0]) && commandValidation(newCommand[0])) {  // proceed if both commands are valid
                    int pfd[2];
                    if (pipe(pfd) == -1) {
                        perror("pipe");
                        exit(EXIT_FAILURE);
                    }
                    int pid1 = fork();
                    if (pid1 == -1) {
                        perror("fork");
                        exit(EXIT_FAILURE);
                    }
                    if (pid1 == 0) {    /* first Child runs the first command */
                        close(pfd[0]);          /* Close unused read end */
                        close(1);
                        dup(pfd[1]);
                        close(pfd[1]);
                        execvp(command[0],command);
                    }
                    int pid2 = fork();
                    if (pid2 == -1) {
                        perror("fork");
                        exit(EXIT_FAILURE);
                    }
                    if (pid2 == 0) {    /* second Child runs the second command */
                        close(pfd[1]);          /* Close unused write end */
                        close(0);
                        dup(pfd[0]);
                        close(pfd[0]);
                        execvp(newCommand[0],newCommand);

                    }
                    close(pfd[0]);
                    close(pfd[1]);
                    /* Wait for the children to finish, then exit. */
                    waitpid(pid1,NULL,0);
                    waitpid(pid2,NULL,0);
                }
                *index = -1;  // not an illgeal command, but this skips the execute stage, since pipe is handled differently
            }
            else {
                // if index == op, there is no un-parsed argument before this operator, means the command should have already been parsed and stored
                if (*index != op) {   // the arguments right before this operator should be added to the command
                    for (int i=*index;i<op;i++) {
                        command[*commandLen] = strdup(args[i]);
                        (*commandLen)++;
                    }
                    command[*commandLen] = NULL;
                }
                *index = parseOperator(args, *len, op);
            }
        }
    }
}


int findNextOperator(char *args[], int len, int index) {
    for (int i=index; i<len; i++) {
        if (!strcmp(args[i],">") | !strcmp(args[i],"<") | !strcmp(args[i],"2>") |!strcmp(args[i],"|") | (!strcmp(args[i],"&"))) {
            return i;
        }
    }
    return -1;
}

void executeCommand(char *command[],int background) {
    int pid = fork();
    if (pid < 0) { // fork failed; exit
        fprintf(stderr, "fork failed\n");
        exit(1);
    } else if (pid == 0) { // child: redirect standard output to a file
        execvp(command[0], command);
    } else { // parent
        if (!background)
         wait(NULL);
    }
}


int parseOperator(char *args[], int len, int index) {
    while (index < len) {
        if (!strcmp(args[index],">")) {
            if (++index == len) {
                // error: no filename after operator
                return -1;
            }
            else {
                char *output = strdup(args[index]);
                stdout_copy = dup(stdout_copy);
                close(1);
                file_out = open(output, O_CREAT|O_WRONLY|O_TRUNC, S_IRWXU);
                return ++index; // advance to the next un-parsed input token
            }
        }

        else if (!strcmp(args[index],"<")) {
            if (++index == len) {
                // error: no filename after operator
                return -1;
            }
            else {
                char *input = strdup(args[index]);
                stdin_copy = dup(stdin_copy);
                close(0);
                file_in = open(input, O_RDONLY);
                if (file_in < 0) { // error
                    printf("file %s does not exist.\n", input);
                    return -1;
                }
                return ++index; // advance to the next un-parsed input token
            }
        }

        else if (!strcmp(args[index],"2>")) {
            if (++index == len) {
                // error: no filename after operator
                return -1;
            }
            else {
                char *errout = strdup(args[index]);
                stderr_copy = dup(stderr_copy);
                close(2);
                file_err = open(errout, O_CREAT|O_WRONLY|O_TRUNC, S_IRWXU);
                return ++index; // advance to the next un-parsed input token
            }
        }
        index++;
    }
    // reaches the end
    return index;
}

void signal_init(void) {
    if (signal(SIGINT, sig_handler) == SIG_ERR)
        printf("signal(SIGINT) error");
    if (signal(SIGTSTP, sig_handler) == SIG_ERR)
        printf("signal(SIGTSTP) error");
    if (signal(SIGCHLD, sig_handler) == SIG_ERR)
        printf("signal(SIGCHLD) error");
}

void sig_handler(int signo) {
   int pid = getpid();
   switch(signo){
      case SIGINT:
      // printf("Sending signals to group:%d\n",pid);
      if (pid != shell_pid) {
         kill(-pid,SIGINT);
      }
      break;

      case SIGTSTP:
      if (pid != shell_pid) {
         kill(-pid,SIGTSTP);
      }
      break;

      case SIGCHLD:
      break;
   }
}

void restoreIORedirection(void) {
   // reopen the standard i/o
   close(file_in);
   stdin_copy = dup2(stdin_copy, 0);
   file_in = -1;
   close(file_out);
   stdout_copy = dup2(stdout_copy, 1);
   file_out = -1;
   close(file_err);
   stderr_copy = dup2(stderr_copy, 2);
   file_err = -1;
}
