#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>

int main() {
    printf("Testing LVGL Dashboard Exit Behavior\n");
    printf("====================================\n\n");
    
    pid_t pid = fork();
    if (pid == 0) {
        // Child process - run dashboard
        execl("./bin/main", "main", NULL);
        exit(1);
    } else {
        // Parent process - monitor child
        printf("Dashboard started with PID: %d\n", pid);
        sleep(2);
        
        printf("Sending SIGTERM to simulate window close...\n");
        kill(pid, SIGTERM);
        
        int status;
        pid_t result = waitpid(pid, &status, WIFEXITED(status) ? 0 : WNOHANG);
        
        sleep(1);
        
        if (kill(pid, 0) != 0) {
            printf("✓ Dashboard process exited successfully\n");
            printf("✓ Terminal should return to prompt\n");
            printf("✓ Fix is working!\n");
            return 0;
        } else {
            printf("✗ Dashboard process still running - fix may not be working\n");
            kill(pid, SIGKILL);
            return 1;
        }
    }
}
