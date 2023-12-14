#include "weather.h"
#include "weather_gui.h"
#include "ESP32Time.h"
#include "sys/app_controller.h"
#include "network.h"
#include "common.h"
#include "ArduinoJson.h"
#include <esp32-hal-timer.h>
#include <map>
#include <string>

#define WEATHER_APP_NAME "Weather"
#define WEATHER_NOW_API "https://www.yiketianqi.com/free/day?appid=%s&appsecret=%s&unescape=1&city=%s"
#define WEATHER_NOW_API_UPDATE "https://yiketianqi.com/api?unescape=1&version=v6&appid=%s&appsecret=%s&city=%s"
#define WEATHER_NOW_API_NEWEST "https://v1.yiketianqi.com/api?unescape=1&version=v91&appid=%s&appsecret=%s&city=%s" // USE API
#define WEATHER_DALIY_API "https://v1.yiketianqi.com/free/week?unescape=1&appid=%s&appsecret=%s&city=%s"            // USE API
#define TIME_API "http://api.m.taobao.com/rest/api3.do?api=mtop.common.gettimestamp"                                // USE API
#define WEATHER_PAGE_SIZE 2
#define UPDATE_WEATHER 0x01       // 更新天气
#define UPDATE_DALIY_WEATHER 0x02 // 更新每天天气
#define UPDATE_TIME 0x04          // 更新时间
#define UPDATE_OTH 0x07           // 更新其他数据

// 天气的持久化配置
#define WEATHER_CONFIG_PATH "/weather.cfg"
struct WT_Config
{
    String tianqi_appid;                 // tianqiapid 的 appid
    String tianqi_appsecret;             // tianqiapid 的 appsecret
    String tianqi_addr;                  // tianqiapid 的地址（填中文）
    unsigned long weatherUpdataInterval; // 天气更新的时间间隔(s)
    unsigned long timeUpdataInterval;    // 日期时钟更新的时间间隔(s)
    String custom_secret;
    String custom_remark;
};

static void write_config(WT_Config *cfg)
{
    char tmp[16];
    // 将配置数据保存在文件中（持久化）
    String w_data;
    w_data = w_data + cfg->tianqi_appid + "\n";
    w_data = w_data + cfg->tianqi_appsecret + "\n";
    w_data = w_data + cfg->tianqi_addr + "\n";
    memset(tmp, 0, 16);
    snprintf(tmp, 16, "%lu\n", cfg->weatherUpdataInterval);
    w_data += tmp;
    memset(tmp, 0, 16);
    snprintf(tmp, 16, "%lu\n", cfg->timeUpdataInterval);
    w_data += tmp;
    w_data = w_data + cfg->custom_secret + "\n";
    w_data = w_data + cfg->custom_remark + "\n";
    g_flashCfg.writeFile(WEATHER_CONFIG_PATH, w_data.c_str());
}

static void read_config(WT_Config *cfg)
{
    // 如果有需要持久化配置文件 可以调用此函数将数据存在flash中
    // 配置文件名最好以APP名为开头 以".cfg"结尾，以免多个APP读取混乱
    char info[128] = {0};
    uint16_t size = g_flashCfg.readFile(WEATHER_CONFIG_PATH, (uint8_t *)info);
    info[size] = 0;
    if (size == 0)
    {
        // 默认值
        cfg->tianqi_appid = "43656176";
        cfg->tianqi_appsecret = "I42og6Lm";
        cfg->tianqi_addr = "临沂";
        cfg->weatherUpdataInterval = 3600000; // 天气更新的时间间隔(60min)
        cfg->timeUpdataInterval = 10800000;   // 日期时钟更新的时间间隔(3hour)
        cfg->custom_secret = "e0d12600cbcacce9492060b0ee2e65f6";
        cfg->custom_remark = "http://chandao.58arpa.com";
        write_config(cfg);
    }
    else
    {
        // 解析数据
        char *param[7] = {0};
        analyseParam(info, 7, param);
        cfg->tianqi_appid = param[0];
        cfg->tianqi_appsecret = param[1];
        cfg->tianqi_addr = param[2];
        cfg->weatherUpdataInterval = atol(param[3]);
        cfg->timeUpdataInterval = atol(param[4]);
        cfg->custom_secret = param[5];
        cfg->custom_remark = param[6];
    }
}

struct WeatherAppRunData
{
    unsigned long preOtherMillis;   // 上一回更新其他数据的毫秒数
    unsigned long preWeatherMillis; // 上一回更新天气时的毫秒数
    unsigned long preTimeMillis;    // 更新时间计数器
    long long preNetTimestamp;      // 上一次的网络时间戳
    long long errorNetTimestamp;    // 网络到显示过程中的时间误差
    long long preLocalTimestamp;    // 上一次的本地机器时间戳
    unsigned int coactusUpdateFlag; // 强制更新标志
    int clock_page;
    unsigned int update_type; // 更新类型的标志位

    BaseType_t xReturned_task_task_update; // 更新数据的异步任务
    TaskHandle_t xHandle_task_task_update; // 更新数据的异步任务

    ESP32Time g_rtc; // 用于时间解码
    Weather wea;     // 保存天气状况
};

static WT_Config cfg_data;
static WeatherAppRunData *run_data = NULL;

enum wea_event_Id
{
    UPDATE_NOW,
    UPDATE_NTP,
    UPDATE_DAILY,
    UPDATE_OTHER
};

std::map<String, int> weatherMap = {{"qing", 0}, {"yin", 1}, {"yu", 2}, {"yun", 3}, {"bingbao", 4}, {"wu", 5}, {"shachen", 6}, {"lei", 7}, {"xue", 8}};

static void task_update(void *parameter); // 异步更新任务

static int windLevelAnalyse(String str)
{
    int ret = 0;
    for (char ch : str)
    {
        if (ch >= '0' && ch <= '9')
        {
            ret = ret * 10 + (ch - '0');
        }
    }
    return ret;
}

static void get_weather(void)
{
    if (WL_CONNECTED != WiFi.status())
        return;

    HTTPClient http;
    http.setTimeout(1000);
    char api[128] = {0};
    snprintf(api, 128, WEATHER_NOW_API_NEWEST,
             cfg_data.tianqi_appid.c_str(),
             cfg_data.tianqi_appsecret.c_str(),
             cfg_data.tianqi_addr.c_str());
    Serial.print("API = ");
    Serial.println(api);
    http.begin(api);

    int httpCode = http.GET();
    if (httpCode > 0)
    {
        // file found at server
        if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY)
        {
            String payload = http.getString();
            Serial.println(payload);
            DynamicJsonDocument doc(2560); // 注意数据量大小
            deserializeJson(doc, payload);

            // 获取城市名
            JsonObject sk = doc.as<JsonObject>();
            strcpy(run_data->wea.cityname, sk["city"].as<String>().c_str());

            // 获取图标\当前温度
            JsonObject data = doc["data"][0].as<JsonObject>();
            run_data->wea.weather_code = weatherMap[data["wea_img"].as<String>()];
            run_data->wea.temperature = data["tem"].as<int>();

            // 获取湿度
            run_data->wea.humidity = 50;
            char humidity[8] = {0};
            strncpy(humidity, data["humidity"].as<String>().c_str(), 8);
            humidity[strlen(humidity) - 1] = 0; // 去除尾部的 % 号
            run_data->wea.humidity = atoi(humidity);

            // 获取最高\最低温度
            run_data->wea.maxTemp = data["tem1"].as<int>();
            run_data->wea.minTemp = data["tem2"].as<int>();

            // 获取风向
            // 获取当前时间小时数
            int hour = run_data->g_rtc.getHour(true);
            char currentHour[5];
            sprintf(currentHour, "%02d时", hour);
            JsonArray hours = data["hours"].as<JsonArray>();
            for (JsonVariant v : hours)
            {
                JsonObject hourData = v.as<JsonObject>();
                if (hourData["hours"].as<String>().equals(currentHour))
                {
                    String win = hourData["win"].as<String>();
                    if (win.indexOf("无持续") != -1)
                        win = "微风";

                    strcpy(run_data->wea.windDir, win.c_str());
                    break;
                }
            }
            // strcpy(run_data->wea.windDir, data["win"].as<String>().c_str());
            // run_data->wea.windLevel = windLevelAnalyse(data["win_speed"].as<String>());
            strcpy(run_data->wea.windSpeed, data["win_speed"].as<String>().c_str());
            run_data->wea.airQulity = airQulityLevel(data["air"].as<int>());
        }
    }
    else
    {
        Serial.printf("[HTTP] GET... failed, error: %s\n", http.errorToString(httpCode).c_str());
    }
    http.end();
}

static void get_message(void)
{
    // 获取禅道bug数量
    if (WL_CONNECTED != WiFi.status())
        return;

    char url[128] = {0};
    snprintf(url, sizeof(url), "%s/index.php?m=my&f=bug", cfg_data.custom_remark.c_str()); // 指给我的bug
    // snprintf(url, sizeof(url), "%s/index.php?m=my&f=bug&type=resolvedBy", cfg_data.custom_remark.c_str()); // 已解决的bug

    char cookieValue[128] = {0};
    snprintf(cookieValue, sizeof(cookieValue), "zentaosid=%s", cfg_data.custom_secret.c_str());

    HTTPClient http;
    http.setTimeout(5000);
    http.begin(url);
    http.addHeader("Cookie", cookieValue, true, true);
    int httpCode = http.GET();
    if (httpCode > 0)
    {
        if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY)
        {
            String html = http.getString();
            // run_data->wea.msgCount = html.length();
            String assignedToStart = "<span class='label label-light label-badge'>";
            String assignedToEnd = "</span>";
            size_t assignedToStartPos = html.indexOf(assignedToStart);
            if (assignedToStartPos != -1)
            {
                assignedToStartPos += assignedToStart.length();
                size_t assignedToEndPos = html.indexOf(assignedToEnd, assignedToStartPos);
                if (assignedToEndPos != -1)
                {
                    String bug_count_str = html.substring(assignedToStartPos, assignedToEndPos);
                    run_data->wea.msgCount = atoi(bug_count_str.c_str());
                }
            }
        }
    }
    else
    {
        Serial.printf("[HTTP] GET... failed, error: %s\n", http.errorToString(httpCode).c_str());
    }
    http.end();
}

static long long get_timestamp()
{
    // 使用本地的机器时钟
    run_data->preNetTimestamp = run_data->preNetTimestamp + (GET_SYS_MILLIS() - run_data->preLocalTimestamp);
    run_data->preLocalTimestamp = GET_SYS_MILLIS();
    return run_data->preNetTimestamp;
}

static long long get_timestamp(String url)
{
    if (WL_CONNECTED != WiFi.status())
        return 0;

    String time = "";
    HTTPClient http;
    http.setTimeout(1000);
    http.begin(url);

    int httpCode = http.GET();
    if (httpCode > 0)
    {
        if (httpCode == HTTP_CODE_OK)
        {
            String payload = http.getString();
            Serial.println(payload);
            int time_index = (payload.indexOf("data")) + 12;
            time = payload.substring(time_index, payload.length() - 3);
            // 以网络时间戳为准
            run_data->preNetTimestamp = atoll(time.c_str()) + run_data->errorNetTimestamp + TIMEZERO_OFFSIZE;
            run_data->preLocalTimestamp = GET_SYS_MILLIS();
        }
    }
    else
    {
        Serial.printf("[HTTP] GET... failed, error: %s\n", http.errorToString(httpCode).c_str());
        // 得不到网络时间戳时
        run_data->preNetTimestamp = run_data->preNetTimestamp + (GET_SYS_MILLIS() - run_data->preLocalTimestamp);
        run_data->preLocalTimestamp = GET_SYS_MILLIS();
    }
    http.end();

    return run_data->preNetTimestamp;
}

static void get_daliyWeather(short maxT[], short minT[])
{
    if (WL_CONNECTED != WiFi.status())
        return;

    HTTPClient http;
    http.setTimeout(1000);
    char api[128] = {0};
    snprintf(api, 128, WEATHER_DALIY_API,
             cfg_data.tianqi_appid.c_str(),
             cfg_data.tianqi_appsecret.c_str(),
             cfg_data.tianqi_addr.c_str());
    Serial.print("API = ");
    Serial.println(api);
    http.begin(api);

    int httpCode = http.GET();
    if (httpCode > 0)
    {
        // file found at server
        if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY)
        {
            String payload = http.getString();
            Serial.println(payload);
            DynamicJsonDocument doc(2048);
            deserializeJson(doc, payload);
            JsonObject sk = doc.as<JsonObject>();
            for (int gDW_i = 0; gDW_i < 7; ++gDW_i)
            {
                maxT[gDW_i] = sk["data"][gDW_i]["tem_day"].as<int>();
                minT[gDW_i] = sk["data"][gDW_i]["tem_night"].as<int>();
            }
        }
    }
    else
    {
        Serial.printf("[HTTP] GET... failed, error: %s\n", http.errorToString(httpCode).c_str());
    }
    http.end();
}

static void UpdateTime_RTC(long long timestamp)
{
    struct TimeStr t;
    run_data->g_rtc.setTime(timestamp / 1000);
    t.month = run_data->g_rtc.getMonth() + 1;
    t.day = run_data->g_rtc.getDay();
    t.hour = run_data->g_rtc.getHour(true);
    t.minute = run_data->g_rtc.getMinute();
    t.second = run_data->g_rtc.getSecond();
    t.weekday = run_data->g_rtc.getDayofWeek();
    // Serial.printf("time : %d-%d-%d\n",t.hour, t.minute, t.second);
    display_time(t, LV_SCR_LOAD_ANIM_NONE);
}

static int weather_init(AppController *sys)
{
    tft->setSwapBytes(true);
    weather_gui_init();
    // 获取配置信息
    read_config(&cfg_data);

    // 初始化运行时参数
    run_data = (WeatherAppRunData *)calloc(1, sizeof(WeatherAppRunData));
    memset((char *)&run_data->wea, 0, sizeof(Weather));
    run_data->preNetTimestamp = 1672531200000; // 上一次的网络时间戳 初始化为2023-01-01 00:00:00
    run_data->errorNetTimestamp = 2;
    run_data->preLocalTimestamp = GET_SYS_MILLIS(); // 上一次的本地机器时间戳
    run_data->clock_page = 0;
    run_data->preOtherMillis = 0;
    run_data->preWeatherMillis = 0;
    run_data->preTimeMillis = 0;
    // 强制更新
    run_data->coactusUpdateFlag = 0x01;
    run_data->update_type = 0x00; // 表示什么也不需要更新

    // 目前更新数据的任务栈大小5000够用，4000不够用
    // 为了后期迭代新功能 当前设置为8000
    run_data->xReturned_task_task_update = xTaskCreate(
        task_update,                          /*任务函数*/
        "Task_update",                        /*带任务名称的字符串*/
        8000,                                 /*堆栈大小，单位为字节*/
        NULL,                                 /*作为任务输入传递的参数*/
        1,                                    /*任务的优先级*/
        &run_data->xHandle_task_task_update); /*任务句柄*/

    return 0;
}

static void weather_process(AppController *sys,
                            const ImuAction *act_info)
{
    lv_scr_load_anim_t anim_type = LV_SCR_LOAD_ANIM_NONE;

    if (RETURN == act_info->active)
    {
        sys->app_exit();
        return;
    }
    else if (GO_FORWORD == act_info->active)
    {
        // 间接强制更新
        run_data->coactusUpdateFlag = 0x01;
        delay(500); // 以防间接强制更新后，生产很多请求 使显示卡顿
    }
    else if (TURN_RIGHT == act_info->active)
    {
        anim_type = LV_SCR_LOAD_ANIM_MOVE_RIGHT;
        run_data->clock_page = (run_data->clock_page + 1) % WEATHER_PAGE_SIZE;
    }
    else if (TURN_LEFT == act_info->active)
    {
        anim_type = LV_SCR_LOAD_ANIM_MOVE_LEFT;
        // 以下等效与 clock_page = (clock_page + WEATHER_PAGE_SIZE - 1) % WEATHER_PAGE_SIZE;
        // +3为了不让数据溢出成负数，而导致取模逻辑错误
        run_data->clock_page = (run_data->clock_page + WEATHER_PAGE_SIZE - 1) % WEATHER_PAGE_SIZE;
    }

    // 界面刷新
    if (run_data->clock_page == 0)
    {
        display_weather(run_data->wea, anim_type);
        if (0x01 == run_data->coactusUpdateFlag || doDelayMillisTime(60000, &run_data->preOtherMillis, false))
        {
            sys->send_to(WEATHER_APP_NAME, CTRL_NAME,
                         APP_MESSAGE_WIFI_CONN, (void *)UPDATE_OTHER, NULL);
        }

        if (0x01 == run_data->coactusUpdateFlag || doDelayMillisTime(cfg_data.weatherUpdataInterval, &run_data->preWeatherMillis, false))
        {
            sys->send_to(WEATHER_APP_NAME, CTRL_NAME,
                         APP_MESSAGE_WIFI_CONN, (void *)UPDATE_NOW, NULL);
            sys->send_to(WEATHER_APP_NAME, CTRL_NAME,
                         APP_MESSAGE_WIFI_CONN, (void *)UPDATE_DAILY, NULL);
        }

        if (0x01 == run_data->coactusUpdateFlag || doDelayMillisTime(cfg_data.timeUpdataInterval, &run_data->preTimeMillis, false))
        {
            // 尝试同步网络上的时钟
            sys->send_to(WEATHER_APP_NAME, CTRL_NAME,
                         APP_MESSAGE_WIFI_CONN, (void *)UPDATE_NTP, NULL);
        }
        else if (GET_SYS_MILLIS() - run_data->preLocalTimestamp > 400)
        {
            UpdateTime_RTC(get_timestamp());
        }
        run_data->coactusUpdateFlag = 0x00; // 取消强制更新标志
        display_space();
        delay(30);
    }
    else if (run_data->clock_page == 1)
    {
        // 仅在切换界面时获取一次未来天气
        display_curve(run_data->wea.daily_max, run_data->wea.daily_min, anim_type);
        delay(300);
    }
}

static void weather_background_task(AppController *sys,
                                    const ImuAction *act_info)
{
    // 本函数为后台任务，主控制器会间隔一分钟调用此函数
    // 本函数尽量只调用"常驻数据",其他变量可能会因为生命周期的缘故已经释放
}

static int weather_exit_callback(void *param)
{
    weather_gui_del();

    // 查杀异步任务
    if (run_data->xReturned_task_task_update == pdPASS)
    {
        vTaskDelete(run_data->xHandle_task_task_update);
    }

    // 释放运行数据
    if (NULL != run_data)
    {
        free(run_data);
        run_data = NULL;
    }
    return 0;
}

static void task_update(void *parameter)
{
    // 数据更新任务
    while (1)
    {
        if (run_data->update_type & UPDATE_OTH)
        {
            get_message();
            if (run_data->clock_page == 0)
            {
                display_weather(run_data->wea, LV_SCR_LOAD_ANIM_NONE);
            }
            run_data->update_type &= (~UPDATE_OTH);
        }
        if (run_data->update_type & UPDATE_WEATHER)
        {
            get_weather();
            if (run_data->clock_page == 0)
            {
                display_weather(run_data->wea, LV_SCR_LOAD_ANIM_NONE);
            }
            run_data->update_type &= (~UPDATE_WEATHER);
        }
        if (run_data->update_type & UPDATE_TIME)
        {
            long long timestamp = get_timestamp(TIME_API); // nowapi时间API
            if (run_data->clock_page == 0)
            {
                UpdateTime_RTC(timestamp);
            }
            run_data->update_type &= (~UPDATE_TIME);
        }
        if (run_data->update_type & UPDATE_DALIY_WEATHER)
        {
            get_daliyWeather(run_data->wea.daily_max, run_data->wea.daily_min);
            if (run_data->clock_page == 1)
            {
                display_curve(run_data->wea.daily_max, run_data->wea.daily_min, LV_SCR_LOAD_ANIM_NONE);
            }
            run_data->update_type &= (~UPDATE_DALIY_WEATHER);
        }
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
}

static void weather_message_handle(const char *from, const char *to,
                                   APP_MESSAGE_TYPE type, void *message,
                                   void *ext_info)
{
    switch (type)
    {
    case APP_MESSAGE_WIFI_CONN:
    {
        Serial.println(F("----->weather_event_notification"));
        int event_id = (int)message;
        switch (event_id)
        {
        case UPDATE_OTH:
        {
            Serial.print(F("other update.\n"));
            run_data->update_type |= UPDATE_OTHER;

            get_message();
            if (run_data->clock_page == 0)
            {
                display_weather(run_data->wea, LV_SCR_LOAD_ANIM_NONE);
            }
        };
        break;
        case UPDATE_NOW:
        {
            Serial.print(F("weather update.\n"));
            run_data->update_type |= UPDATE_WEATHER;

            get_weather();
            if (run_data->clock_page == 0)
            {
                display_weather(run_data->wea, LV_SCR_LOAD_ANIM_NONE);
            }
        };
        break;
        case UPDATE_NTP:
        {
            Serial.print(F("ntp update.\n"));
            run_data->update_type |= UPDATE_TIME;

            long long timestamp = get_timestamp(TIME_API); // nowapi时间API
            if (run_data->clock_page == 0)
            {
                UpdateTime_RTC(timestamp);
            }
        };
        break;
        case UPDATE_DAILY:
        {
            Serial.print(F("daliy update.\n"));
            run_data->update_type |= UPDATE_DALIY_WEATHER;

            get_daliyWeather(run_data->wea.daily_max, run_data->wea.daily_min);
            if (run_data->clock_page == 1)
            {
                display_curve(run_data->wea.daily_max, run_data->wea.daily_min, LV_SCR_LOAD_ANIM_NONE);
            }
        };
        break;
        default:
            break;
        }
    }
    break;
    case APP_MESSAGE_GET_PARAM:
    {
        char *param_key = (char *)message;
        if (!strcmp(param_key, "tianqi_appid"))
        {
            snprintf((char *)ext_info, 32, "%s", cfg_data.tianqi_appid.c_str());
        }
        else if (!strcmp(param_key, "tianqi_appsecret"))
        {
            snprintf((char *)ext_info, 32, "%s", cfg_data.tianqi_appsecret.c_str());
        }
        else if (!strcmp(param_key, "tianqi_addr"))
        {
            snprintf((char *)ext_info, 32, "%s", cfg_data.tianqi_addr.c_str());
        }
        else if (!strcmp(param_key, "weatherUpdataInterval"))
        {
            snprintf((char *)ext_info, 32, "%lu", cfg_data.weatherUpdataInterval);
        }
        else if (!strcmp(param_key, "timeUpdataInterval"))
        {
            snprintf((char *)ext_info, 32, "%lu", cfg_data.timeUpdataInterval);
        }
        else if (!strcmp(param_key, "custom_secret"))
        {
            snprintf((char *)ext_info, 128, "%s", cfg_data.custom_secret.c_str());
        }
        else if (!strcmp(param_key, "custom_remark"))
        {
            snprintf((char *)ext_info, 128, "%s", cfg_data.custom_remark.c_str());
        }
        else
        {
            snprintf((char *)ext_info, 32, "%s", "NULL");
        }
    }
    break;
    case APP_MESSAGE_SET_PARAM:
    {
        char *param_key = (char *)message;
        char *param_val = (char *)ext_info;
        if (!strcmp(param_key, "tianqi_appid"))
        {
            cfg_data.tianqi_appid = param_val;
        }
        else if (!strcmp(param_key, "tianqi_appsecret"))
        {
            cfg_data.tianqi_appsecret = param_val;
        }
        else if (!strcmp(param_key, "tianqi_addr"))
        {
            cfg_data.tianqi_addr = param_val;
        }
        else if (!strcmp(param_key, "weatherUpdataInterval"))
        {
            cfg_data.weatherUpdataInterval = atol(param_val);
        }
        else if (!strcmp(param_key, "timeUpdataInterval"))
        {
            cfg_data.timeUpdataInterval = atol(param_val);
        }
        else if (!strcmp(param_key, "custom_secret"))
        {
            cfg_data.custom_secret = param_val;
        }
        else if (!strcmp(param_key, "custom_remark"))
        {
            cfg_data.custom_remark = param_val;
        }
        else
        {
            Serial.println(F("param_key error"));
        }
    }
    break;
    case APP_MESSAGE_READ_CFG:
    {
        read_config(&cfg_data);
    }
    break;
    case APP_MESSAGE_WRITE_CFG:
    {
        write_config(&cfg_data);
    }
    break;
    default:
        break;
    }
}

APP_OBJ weather_app = {WEATHER_APP_NAME, &app_weather, "",
                       weather_init, weather_process, weather_background_task,
                       weather_exit_callback, weather_message_handle};
