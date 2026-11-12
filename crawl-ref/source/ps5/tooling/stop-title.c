/* A payload build_ps5.py deploy --stop sends to elfldr: closes one title,
   TITLE_ID, if it is running, and nothing else (ps5-Towelket's
   stop-native.c). */

#include <stdio.h>

int sceLncUtilGetAppId(const char *title_id);
int sceSystemServiceKillApp(int app_id, int how, int reason, int core_dump);

int main(void)
{
    int app = sceLncUtilGetAppId(TITLE_ID);
    if (app > 0) {
        int result = sceSystemServiceKillApp(app, -1, 0, 0);
        if (result) {
            printf("Failed to stop %s: 0x%x\n", TITLE_ID, result);
            return 1;
        }
        printf("Stopped %s\n", TITLE_ID);
    }
    puts("STOP_OK");
    return 0;
}
