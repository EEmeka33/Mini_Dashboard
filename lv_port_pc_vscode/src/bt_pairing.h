#ifndef BLUETOOTH_PAIRING_H
#define BLUETOOTH_PAIRING_H

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <signal.h>

/**
 * Make Bluetooth device discoverable (non-blocking)
 * Spawns background process to run bt_make_discoverable.sh
 * 
 * Returns:
 *   0 on successful spawn
 *  -1 on fork error
 */
static inline int bluetooth_pair_device_start(void)
{
    pid_t pid = fork();
    
    if (pid < 0) {
        fprintf(stderr, "Failed to fork for Bluetooth discoverable\n");
        return -1;
    }
    
    if (pid == 0) {
        /* Child process */
        
        /* Redirect ALL output to avoid interfering with UI */
        int dev_null = open("/dev/null", O_WRONLY);
        dup2(dev_null, STDOUT_FILENO);
        dup2(dev_null, STDERR_FILENO);
        dup2(dev_null, STDIN_FILENO);
        close(dev_null);
        
        /* Detach from parent process group */
        setsid();
        
        /* Execute discoverable script */
        execl("/home/pi5/dashboard/bt_make_discoverable.sh", 
              "bt_make_discoverable.sh", 
              (char *)NULL);
        
        /* If execl fails */
        exit(1);
    }
    
    /* Parent process continues immediately */
    fprintf(stderr, "[Bluetooth] Discoverable mode activated\n");
    return 0;
}

#endif /* BLUETOOTH_PAIRING_H */
