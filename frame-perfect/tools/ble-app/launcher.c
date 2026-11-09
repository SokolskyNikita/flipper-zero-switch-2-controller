/* Starts the command in FBT_CMD (cmd.sh next to the .app, so the signed bundle never changes) as a child and
 * waits for it. Being an app launched by LaunchServices, it is the "responsible"
 * process for the child, so macOS checks *this* app's Bluetooth permission. */
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <mach-o/dyld.h>
#include <libgen.h>
extern char **environ;
int main(void) {
    char exe[4096]; uint32_t n = sizeof exe;
    if (_NSGetExecutablePath(exe, &n) != 0) return 1;
    char script[4200];
    snprintf(script, sizeof script, "%s/../../../cmd.sh", dirname(exe));
    char *argv[] = {"/bin/sh", script, NULL};
    pid_t pid;
    if (posix_spawn(&pid, "/bin/sh", NULL, NULL, argv, environ) != 0) return 1;
    int st = 0; waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}
