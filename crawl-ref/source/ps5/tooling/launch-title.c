/* A payload build_ps5.py launch sends to elfldr: starts one title,
   TITLE_ID, for the foreground user, as the home screen would. */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint32_t size;
    int32_t user_id;
    uint32_t app_opt;
    uint64_t crash_report;
    uint32_t check_flag;
} lnc_app_param;

int sceUserServiceInitialize(void *params);
int sceUserServiceGetForegroundUser(int32_t *user_id);
int sceLncUtilLaunchApp(const char *title_id, const char **argv,
                        lnc_app_param *param);

int main(void)
{
    lnc_app_param param;
    memset(&param, 0, sizeof param);
    param.size = sizeof param;
    sceUserServiceInitialize(NULL);
    if (sceUserServiceGetForegroundUser(&param.user_id) != 0) {
        puts("No foreground user");
        return 1;
    }
    int result = sceLncUtilLaunchApp(TITLE_ID, NULL, &param);
    if (result < 0) {
        printf("Failed to launch %s: 0x%x\n", TITLE_ID, result);
        return 1;
    }
    printf("Launched %s\n", TITLE_ID);
    puts("LAUNCH_OK");
    return 0;
}
