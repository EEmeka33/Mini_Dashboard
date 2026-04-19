#ifndef BLUETOOTH_PAIRING_H
#define BLUETOOTH_PAIRING_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <signal.h>

/**
 * Start Bluetooth device discoverable mode (non-blocking)
 * This function makes the Pi discoverable so phones can find and pair with it
 * via the Pi OS Bluetooth settings GUI. The audio setup script runs automatically
 * after successful pairing.
 * 
 * Output will be logged to /tmp/bt_discoverable.log
 * 
 * Returns:
 *   0 on successful spawn
 *  -1 on fork error
 */
static inline int bluetooth_pair_device_start(void)
{
    pid_t pid = fork();
    
    if (pid < 0) {
        fprintf(stderr, "Failed to fork for Bluetooth pairing\n");
        return -1;
    }
    
    if (pid == 0) {
        /* Child process */
        
        /* Redirect ALL output to avoid interfering with UI */
        int dev_null = open("/dev/null", O_WRONLY);
        dup2(dev_null, STDOUT_FILENO);  /* Redirect stdout */
        dup2(dev_null, STDERR_FILENO);  /* Redirect stderr */
        dup2(dev_null, STDIN_FILENO);   /* Redirect stdin */
        close(dev_null);
        
        /* Detach from parent process group to prevent terminal signals */
        setsid();
        
        /* Execute discoverable script (no pairing - let OS GUI handle it) */
        execl("/home/pi5/dashboard/bt_make_discoverable.sh", 
              "bt_make_discoverable.sh", 
              (char *)NULL);
        
        /* If execl fails */
        exit(1);
    }
    
    /* Parent process continues immediately */
    fprintf(stderr, "[BT Pairing] Background task started (PID: %d)\n", pid);
    return 0;
}

/**
 * Check the status of Bluetooth pairing
 * Reads the result file to determine pairing status
 * 
 * Returns:
 *   1 if pairing succeeded
 *   0 if pairing is still in progress or failed
 *  -1 if result file not found
 */
static inline int bluetooth_pair_device_check_result(char *device_name, size_t max_len)
{
    FILE *result_file = fopen("/tmp/bt_pair_result.txt", "r");
    
    if (!result_file) {
        return -1;  /* File doesn't exist yet - still in progress */
    }
    
    char line[256];
    int ret = 0;
    
    if (fgets(line, sizeof(line), result_file)) {
        if (strncmp(line, "PAIRED:", 7) == 0) {
            ret = 1;  /* Success */
            if (device_name && max_len > 0) {
                /* Extract device name */
                char *name_start = strchr(line, '-');
                if (name_start) {
                    name_start++;  /* Skip the dash */
                    while (*name_start == ' ') name_start++;  /* Skip spaces */
                    
                    strncpy(device_name, name_start, max_len - 1);
                    device_name[max_len - 1] = '\0';
                    
                    /* Remove trailing newline */
                    char *newline = strchr(device_name, '\n');
                    if (newline) *newline = '\0';
                }
            }
        } else if (strncmp(line, "FAILED:", 7) == 0) {
            ret = 0;  /* Failed */
        }
    }
    
    fclose(result_file);
    return ret;
}

/**
 * Display Bluetooth pairing progress/status
 * Should be called periodically in the UI to show user feedback
 */
static inline void bluetooth_show_pairing_status(void)
{
    char device_name[128] = {0};
    
    int status = bluetooth_pair_device_check_result(device_name, sizeof(device_name));
    
    if (status == 1) {
        fprintf(stderr, "[BT Pairing SUCCESS] Device paired: %s\n", device_name);
    } else if (status == 0) {
        fprintf(stderr, "[BT Pairing FAILED]\n");
    }
    /* else: still in progress (status == -1) */
}

#endif /* BLUETOOTH_PAIRING_H */
