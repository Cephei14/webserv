#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

void print_msg()
{
	printf("Parent received SIGUSR1 from child.\n");
}

int main()
{
	pid_t pid, cpid, ppid;
	struct sigaction sa;
	int status;

	sa.sa_handler = print_msg;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags=SA_RESTART;
	sigaction(SIGUSR1, &sa, NULL);
	pid = getpid();
	cpid = fork();
	if (cpid == -1)
	{
		perror("fork failed\n");
		return -1;
	}
	if(!cpid)
	{
		cpid = getpid();
		ppid = getppid();
		printf("child PID = %d\nParent PID using getpid() = %d\nParent PID using getppid() = %d\n", cpid, pid, ppid);
		sleep (2);
		kill(pid, SIGUSR1);
		exit(42);
	}
	printf("Parent waiting the child to end\n");
	while (waitpid(-1, &status, 0) > 0)
	{
		if (WIFEXITED(status))
			printf("Child exited with status %d\n", WEXITSTATUS(status));
	}
	return 0;
}