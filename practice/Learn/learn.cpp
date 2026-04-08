#include <iostream>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
#include <cstdlib>
#include <cstdio>

/**
 * DelayedHandshake encapsulates the logic of process synchronization
 * using fork and signals in a C++ context.
 */
class DelayedHandshake
{
public:
    // Signal handlers must be static to be compatible with C-style function pointers.
    static void signalHandler(int signum)
	{
        if (signum == SIGUSR1)
            std::cout << "\n[Parent] Received SIGUSR1 from child." << std::endl;
    }

    void run()
	{
        setupSignalHandler();
        pid_t parentPid = getpid();
        pid_t pid = fork();

        if (pid == -1)
		{
            std::cerr << "Fork failed!" << std::endl;
            return;
        }
        if (pid == 0)
            executeChildLogic(parentPid);
        else
            executeParentLogic(pid);
    }

private:
    void setupSignalHandler()
	{
        struct sigaction sa;
        sa.sa_handler = &DelayedHandshake::signalHandler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART; // Prevents waitpid from failing with EINTR

        if (sigaction(SIGUSR1, &sa, NULL) == -1)
		{
            perror("sigaction");
            exit(1);
        }
    }

    void executeChildLogic(pid_t parentPid)
	{
        std::cout << "[Child] PID: " << getpid() << " | Parent PID: " << parentPid;
        sleep(2);
        kill(parentPid, SIGUSR1);
        exit(42);
    }

    void executeParentLogic(pid_t childPid)
	{
        std::cout << "[Parent] Waiting for child to signal and terminate..." << std::endl;
        int status;
        if (waitpid(childPid, &status, 0) > 0)
		{
            if (WIFEXITED(status))
                std::cout << "[Parent] Child reaped. Exit status: " << WEXITSTATUS(status) << std::endl;
        }
    }
};

int main()
{
    DelayedHandshake app;
    app.run();
    return 0;
}