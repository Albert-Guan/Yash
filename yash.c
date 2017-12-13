
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <signal.h>

#define LINE_LIMIT 2000
#define ARG_LIMIT 128

enum job_status {
   RUNNING,
   STOPPED,
   DONE
};
/* A process.  */
typedef struct process
{
   pid_t pid;                  /* process ID */
   char **argv;                /* for exec */
   int file_in, file_out, file_err;  /* I/O redirection */
   enum job_status status;
   struct process *next;       /* next process in pipeline */
} process_t;

/* A job is a process group.
*/
typedef struct procgrp
{
   pid_t pgid;                 /* process group ID */
   char *cmdline;              /* command line input*/
   char notified;              /* true if user told about stopped job */
   int num;                    /* the number associated with the job */
   int background;
   process_t *head;              /* list of processes in this job */
   struct procgrp *next;       /* next job */
} job_t;

pid_t shell_pid;
int shell_terminal;
job_t * job_head = NULL;   // the first job in job linked list
job_t * job_fg = NULL;     // the current foreground job
int job_num = 0;           // equal to the job num of the last job in the list
int job_recent = 0;        // the job number of the job to be brought up by fg or bg

void processCmdline(char *cmdline);
int parseInput(job_t *job);
void launch_job (job_t *job);
void launch_process(process_t *proc, job_t *job);
int isOperator(char *token);
void signal_init(void);
void sig_handler(int signo);
void put_job_in_background (job_t *job, int cont);
void put_job_in_foreground (job_t *job, int cont);
int mark_process_status (pid_t pid, int status);
void update_status(void);
int job_is_done (job_t *j);
int job_is_stopped (job_t *j);
void job_add(job_t * job);
void job_free(job_t *job);
void process_add(job_t * job, process_t * proc);
void print_notify(job_t *job, char * status);
void job_notify(int skip_notify);
void continue_job (job_t *j, int foreground);
void do_fg(void);
void do_bg(void);
void do_jobs(void);
job_t * job_find (pid_t pid);
process_t * process_find (job_t *j, pid_t pid);


int main(int argc, char const *argv[]) {
   signal_init();

   /* Put the shell in its own process group.  */
   shell_pid = getpid();
   // printf("%d\n", shell_pid);
   if (setpgid (shell_pid, shell_pid) < 0)
   {
      perror ("setpgid");
      exit (1);
   }

   /* get control of the terminal.  */
   shell_terminal = STDIN_FILENO; // shell terminal is the standard input
   tcsetpgrp (shell_terminal, shell_pid);

   while (1) {
      printf("%s", "# "); // prompt
      // accepts user input
      char buffer[LINE_LIMIT+1];
      char *ptr = fgets(buffer, LINE_LIMIT, stdin);  // get one line
      if (ptr == NULL) exit(1); // error
      int len = strlen(buffer);
      if (buffer[len-1] == '\n') buffer[len-1] = '\0'; // this should always be true
      processCmdline(buffer);
   }

}

void processCmdline(char *cmdline) {
   // do update status here (but not notifying)
   // so fg and bg would ignore the Done jobs
   update_status();
   // see if the command is the shell built-in command
   if (!strcmp(cmdline, "fg")) {
      do_fg();
      return;
   }
   else if (!strcmp(cmdline, "bg")) {
      do_bg();
      return;
   }
   else if (!strcmp(cmdline, "jobs")) {
      do_jobs();
      job_notify(1);
      return;
   }

   // create a job
   job_t *job = (job_t *) malloc(sizeof(job_t));
   memset(job, 0, sizeof(job_t));

   job->cmdline = strdup(cmdline);
   job->notified = 0;
   int retval = parseInput(job);
   if (retval) {  // job successfully created
      // check executable validity here
      launch_job(job);
   }
   else job_free(job); // job not successfully created, therefore free job

   job_notify(0);
}


/**
   Return: 0 on error, or empty line
 */
int parseInput(job_t *job) {
   // split the input into tokens
   char *cmdline_cp = strdup(job->cmdline);
   char *args[ARG_LIMIT+1];
   int len = 1;
   if ((args[0] = strtok(cmdline_cp," \t")) == NULL) len = 0;
   while ((args[len] = strtok(NULL," \t")) != NULL) len++;
   if (len == 0) return 0; // empty line
   args[len] = NULL; // terminate with a NULL

   // create a process;
   process_t *proc = (process_t *) malloc(sizeof(process_t));
   memset(proc, 0, sizeof(process_t));
   proc->file_in = STDIN_FILENO;
   proc->file_out = STDOUT_FILENO;
   proc->file_err = STDERR_FILENO;

   char **command = (char **) malloc(ARG_LIMIT+1); // will be used to store the parsed command
   int commandLen = 0;
   command[commandLen] = NULL;
   int index = 0; // marks the beginning of the to be parsed input (or part of the input)
   while (index < len) {
      if (isOperator(args[index])) {  // is an operator
         if (!strcmp(args[index], "|")) {
            if (++index == len) { // | is the last argument
               perror("Error: no argument after pipe");
               free(proc);
               free(command);
               return 0;
            }
            // store this command to the proc, and add to the job, and create a new command
            proc->argv = command;
            command = (char **) malloc(ARG_LIMIT+1);
            commandLen = 0;
            command[commandLen] = NULL;
            process_add(job, proc);
            proc = (process_t *) malloc(sizeof(process_t));
            memset(proc, 0, sizeof(process_t));
            proc->file_in = STDIN_FILENO;
            proc->file_out = STDOUT_FILENO;
            proc->file_err = STDERR_FILENO;
         }

         else if (!strcmp(args[index], "&")) {
            if (++index != len) {
               perror("Error: & must be at the end of the line");
               free(proc);
               free(command);
               return 0;
            }
            // job->cmdline[strlen(job->cmdline)-1] = '\0';  // not required for yash
            job->background = 1;
         }

         else if (!strcmp(args[index], ">")) {
            if (++index == len) {
               perror("Error: no filename after operator");
               free(proc);
               free(command);
               return 0;
            }
            else {
               char *output = strdup(args[index++]);  // skip the file name
               // stdout_copy = dup(stdout_copy);
               // close(1);
               proc->file_out = open(output, O_CREAT|O_WRONLY|O_TRUNC, S_IRWXU);
            }
         }

         else if (!strcmp(args[index], "<")) {
            if (++index == len) {
               perror("Error: no filename after operator");
               free(proc);
               free(command);
               return 0;
            }
            else {
                char *input = strdup(args[index++]);  // skip the file name
                // stdin_copy = dup(stdin_copy);
                // close(0);
                proc->file_in = open(input, O_RDONLY);
                if (proc->file_in < 0) { // error
                    printf("file %s does not exist.\n", input);
                    free(proc);
                    free(command);
                    return 0;
                }
            }
         }

         else if (!strcmp(args[index], "2>")) {
            if (++index == len) {
               perror("Error: no filename after operator");
               free(proc);
               free(command);
               return 0;
            }
            else {
               char *errout = strdup(args[index++]);  // skip the file name
               // stderr_copy = dup(stderr_copy);
               // close(2);
               proc->file_err = open(errout, O_CREAT|O_WRONLY|O_TRUNC, S_IRWXU);
            }
         }
      }

      else { // a command or arg, add to COMMAND list
         command[commandLen++] = strdup(args[index]);
         command[commandLen] = NULL;
         index++; // advance to the next un-parsed input token
      }
   }
   // add the process to the job
   proc->argv = command;
   process_add(job, proc);
   return 1;
}

void launch_job (job_t *job) {
  process_t *proc;
  pid_t pid;
  int pfd[2];
  // Assume no strange pipe arrangement e.g. cat > a.txt b.txt | grep a
  // in which case pipe isn't even used. We assume left side of pipe must
  // direct to right side, and right side must read from left side
  for (proc = job->head; proc; proc = proc->next) {
      /* Set up pipes, if necessary.
         if proc->next is NULL then no pipe*/
      if (proc->next) {
          if (pipe(pfd) < 0) {
              perror("pipe");
              exit(EXIT_FAILURE);
           }
          proc->file_out = pfd[1];
          proc->next->file_in = pfd[0];
        }

      /* Fork the child processes.  */
      pid = fork();
      if (pid < 0) {
          /* The fork failed.  */
          perror("fork");
          exit(EXIT_FAILURE);
       }
       else if (pid == 0) { /* This is the child process.  */
          launch_process(proc, job);
       }
       else { /* This is the parent process.  */
          /* Put the process into the process group and give the process group the terminal, if appropriate.
          This has to be done both by the shell and in the individual child processes because of potential race conditions. */
          proc->pid = pid;
          if (!job->pgid) job->pgid = pid;  // child is the group leader
            setpgid (pid, job->pgid);
        }

      /* Clean up after executing the process. */
      if (proc->file_in != STDIN_FILENO)
        close (proc->file_in);
      if (proc->file_out != STDOUT_FILENO)
        close (proc->file_out);
       if(proc->file_err != STDERR_FILENO)
       close (proc->file_err);
   }

   // parent after launching all processes in the group (not waiting for any to complete yet)
   // children won't reach here since they run exec
   if (job->background)
   put_job_in_background (job, 0); // puts in job list
   else
   put_job_in_foreground (job, 0); // waits for the job to complete
}


void launch_process(process_t *proc, job_t *job) {
   pid_t pid;
   /* Put the process into the process group and give the process group the terminal, if appropriate.
   This has to be done both by the shell and in the individual child processes because of potential race conditions. */
   pid = getpid();
   proc->pid = pid;
   if (!job->pgid) job->pgid = pid;
      setpgid (pid, job->pgid);
   if (!job->background)  // if foreground, give the JOB the controlling terminal
      tcsetpgrp (shell_terminal, job->pgid);

   signal (SIGINT, SIG_DFL);
   signal (SIGQUIT, SIG_DFL);
   signal (SIGTSTP, SIG_DFL);
   signal (SIGTTIN, SIG_DFL);
   signal (SIGTTOU, SIG_DFL);
   signal (SIGCHLD, SIG_DFL);

   // printf("%d\n", proc->pid);
   // printf("%d %d\n", proc->file_in, proc->file_out);

   if (proc->file_in != STDIN_FILENO) {
      dup2 (proc->file_in, STDIN_FILENO);
      close (proc->file_in);  // close the temporarily opened fd
   }
   if (proc->file_out != STDOUT_FILENO) {
      dup2 (proc->file_out, STDOUT_FILENO);
      close (proc->file_out);
   }
   if (proc->file_err != STDERR_FILENO){
      dup2 (proc->file_err, STDERR_FILENO);
      close (proc->file_err);
   }

   proc->status = RUNNING;
   // fprintf(stderr, "%s %s %s %s\n", proc->argv[0], proc->argv[1], proc->argv[2], proc->argv[3]);
   /* Exec the new process.  Make sure we exit.  */
   execvp (proc->argv[0], proc->argv);

   perror ("execvp");
   exit (EXIT_FAILURE);
}


/* Check for processes that have status information available,
blocking until all processes in the given job have reported.  */
void wait_for_job (job_t *j) {
   int status;
   pid_t pid;

   do {
      pid = waitpid (-1, &status, WUNTRACED);  // waits for any child process.
   }
   while (mark_process_status(pid, status) && !job_is_stopped(j)); // job is still running
   // not hanlding error from mark_process_status
}

/* Check for processes that have new status available without blocking.  */
void update_status (void) {
  int status;
  pid_t pid;

  do
    pid = waitpid (-1, &status, WUNTRACED|WNOHANG);
  while (mark_process_status (pid, status));

}
//
/* Store the status of the process pid that was returned by waitpid.
Return 1 if marked the process successfully, 0 if no unwaited child or error.  */
int mark_process_status (pid_t pid, int status) {
   job_t *j;
   process_t *p;
   if (pid > 0) {
      /* Update the record for the process.  */
      for (j = job_head; j; j = j->next)
         for (p = j->head; p; p = p->next)
            if (p->pid == pid) {  // the process we were waiting for
               if (WIFSTOPPED (status)) { // maybe due to SIGTTIN or SIGTTOU
                  p->status = STOPPED;
                  job_recent = j->num;

                  // handle SIGTSTP
                  if (WSTOPSIG(status) == SIGTSTP)
                      j->notified = 1;     // do not want to notify this as required
                      job_recent = j->num;
               }
               else {
                  p->status = DONE;
               }
               return 1;
            }
      // if reach here, meaning not in the job list, so the foreground process
      if (WIFSTOPPED (status)) {
         // handle SIGTSTP
         if (WSTOPSIG(status) == SIGTSTP) {
             job_add(job_fg);  // not in the list, so add to the list
             process_t *p = process_find(job_fg, pid);
             p->status = STOPPED;
             job_fg->notified = 1;     // do not want to notify this as required
             job_recent = job_fg->num;  // this is the most recent stopped job now
         }
      }
      return 0;
   }
   return 0;
   // No error handling, so the other case is (pid == 0 || errno == ECHILD)
   // which means no unwaited child process left  */
}

/* Put a job in the background.  If the cont argument is true, send
the process group a SIGCONT signal to wake it up.  */
void put_job_in_background (job_t *job, int cont) {
   /* Send the job a continue signal, if necessary.  */
   if (cont) {
      if (kill (-job->pgid, SIGCONT) < 0)
      perror ("kill (SIGCONT)");
   }
   else {
      // add to job linked list
      // if cont = 1, meaning job was stopped, so already in the list
      job_add(job);
      printf("[%d]   %d\n", job->num, job->pgid);
   }
   // this is specified by YASH
   job_recent = job->num;
}

/* Put job j in the foreground.  If cont is nonzero,
send the process group a SIGCONT signal to wake
it up before we block.  */
void put_job_in_foreground(job_t *job, int cont) {
   /* Put the job into the foreground.  */
   tcsetpgrp(shell_terminal, job->pgid);
   /* Send the job a continue signal, if necessary.  */
   if (cont) {
      if (kill(-job->pgid, SIGCONT) < 0)
         perror("kill (SIGCONT)");
   }
   job_fg = job;  // current foreground job
   /* Wait for it to report.  */
   wait_for_job(job);
   /* Put the shell back in the foreground.  */
   tcsetpgrp(shell_terminal, shell_pid);
}

/*
parameter:
skip_notify - if 1, not printing
*/
void job_notify(int skip_notify) {
   job_t *j, *jpre = NULL, *jnext;
   update_status(); // first, update all the status
   for (j = job_head; j; j = jnext) {
      jnext = j->next; // need it cuz of potential freeing of j
      // job is done
      if (job_is_done(j)) {
         // no need to check notified, since once done, removed from list
         // if this job is just terminated (somehow) from the foreground, then don't print
         if (!skip_notify && job_fg != j) print_notify(j, "Done");
         // remove this job from list
         if (jpre) {
            jpre->next = jnext;
         }
         else job_head = jnext;  // jpre doesnt exist meaning j was the head

         // if this is the last job, then update job num
         if (j->num == job_num) {
            if (jpre) job_num = jpre->num;
            else job_num = 0;
         }
         // if this is the most recent job, then make the new last job the most recent
         if (job_recent == j->num) job_recent = job_num;

         job_free(j);
      }
      // job is stopped
      else if (job_is_stopped(j) && !j->notified) {
         if (!skip_notify) print_notify(j, "Stopped");
         j->notified = 1;
         jpre = j;
      }

      else jpre = j; // job is runninng
   }
}

void print_notify(job_t * job,  char * status) {
   if (job_recent == job->num)
      printf("[%d]+   %s  %s\n", job->num, status, job->cmdline);
   else
      printf("[%d]-   %s  %s\n", job->num, status, job->cmdline);
}

void do_jobs() {
   job_t *j;
   for (j = job_head; j; j = j->next) {
      if (job_is_done(j))
         print_notify(j, "Done");
      else if (job_is_stopped(j))
         print_notify(j, "Stopped");
      else {
         print_notify(j, "Running");
      }
   }
}

void do_fg() {
   job_t *j;

   // empty, or has only DONE jobs that aren't yet notified
   if (job_num == 0 || job_recent == 0) {
      printf("%s\n", "-yash: fg: current: no such job");
      return;
   }

   for (j = job_head; j; j = j->next) {
      if (j->num == job_recent) {
         // simply don't do anything if it's DONE. this is a non-ideal workaround for one case:
         // job A is the most recent stopped job, we send a bg so it starts to execute in bg and is DONE (to be notified with the next command)
         // but now we send another bg or a fg: the most recent is yet to be updated, so would still be job A, but since job A is done, cannot execute again
         // we simply ignore the command; bash would have already know what job to bring up
         if (!job_is_done(j)) {
            printf("%s\n", j->cmdline);
            continue_job (j, 1);
         }
      }
   }
}

void do_bg() {
   job_t *j;

   // empty, or has only DONE jobs that aren't yet notified
   if (job_num == 0 || job_recent == 0) {
      printf("%s\n", "-yash: bg: current: no such job");
      return;
   }

   for (j = job_head; j; j = j->next) {
      if (j->num == job_recent) {
         if (!job_is_done(j)) {// same issue with fg
            printf("%s\n", j->cmdline);
            continue_job (j, 0);
         }
      }
   }
}

/* Mark a stopped job as being running again.  */
void mark_job_as_running (job_t *j) {
   process_t *p;
   // ASSERT job is STOPPED
   for (p = j->head; p; p = p->next)
      if (p->status == STOPPED)
         p->status = RUNNING;
   j->notified = 0;  // IMPORTANT
}

/* Continue the job.  */
void continue_job (job_t *j, int foreground)
{
  mark_job_as_running (j);
  if (foreground)
    put_job_in_foreground (j, 1);  // not yet removed, possible to be brought back to background later on
  else
    put_job_in_background (j, 1);
}


/* Return 1 if all processes in the job have stopped or completed.  */
int job_is_stopped (job_t *j) {
  process_t *p;
  for (p = j->head; p; p = p->next)
    if (p->status == RUNNING)
      return 0;
  return 1;
}


/* Return 1 if all processes in the job have completed.  */
int job_is_done (job_t *j)
{
  process_t *p;
  for (p = j->head; p; p = p->next)
    if (!(p->status == DONE))
      return 0;
  return 1;
}


void signal_init(void) {

   signal (SIGINT, SIG_IGN);
   signal (SIGQUIT, SIG_IGN);
   signal (SIGTSTP, SIG_IGN);
   signal (SIGTTIN, SIG_IGN);
   signal (SIGTTOU, SIG_IGN);
   signal (SIGCHLD, SIG_IGN);

   if (signal(SIGCHLD, sig_handler)  == SIG_ERR)
      printf("signal(SIGCHLD) error");
}


void sig_handler(int signo) {
   switch(signo){
      case SIGCHLD:
      break;
   }
}

int isOperator(char *token) {
   if (!strcmp(token,">") | !strcmp(token,"<") | !strcmp(token,"2>") |!strcmp(token,"|") | (!strcmp(token,"&")))
      return 1;
   return 0;
}

void process_add(job_t * job, process_t * proc) {
   process_t ** pp;
   pp = &job->head;
   while (*pp != NULL) pp = &((*pp)->next);
   *pp = proc;
}

void job_add(job_t * job) {
   job_t ** jj = &job_head;
   while (*jj != NULL) jj = &((*jj)->next);
   job->num = ++job_num;
   *jj = job;
}

void job_free(job_t *job) {
   process_t * p = job->head;
   if (p != NULL) {
      process_t * next = p->next;
      free(p->argv);
      free(p);
      p = next;
   }
   free(job);
}

/* Find the active job with the indicated pid.  */
job_t * job_find (pid_t pid) {
   job_t *j;
   process_t *p;
   for (j = job_head; j; j = j->next)
      for (p = j->head; p; p = p->next) {
         if (p->pid == pid)
            return j;
      }
   return NULL;
}

process_t * process_find (job_t *j, pid_t pid) {
   process_t *p;
   for (p = j->head; p; p = p->next) {
      if (p->pid == pid)
         return p;
   }
   return NULL;
}
