#include <stdio.h>
#include "app_wifi.h"
#include "app_uvc.h"
#include "app_streaming.h"

void app_main(void)
{
    app_wifi_init();
    app_uvc_init();
    app_streaming_init();
}
