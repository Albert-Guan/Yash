#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/wait.h>

void redirectOutput(char **tokens, char *output) {

   int rc = fork();
   if (rc < 0) { // fork failed; exit
      fprintf(stderr, "fork failed\n");
      exit(1);
   } else if (rc == 0) { // child: redirect standard output to a file
      close(1);
      open(output, O_CREAT|O_WRONLY|O_TRUNC, S_IRWXU);

      execvp(tokens[0], tokens);
   } else { // parent
      int wc = wait(NULL);
   }

}
