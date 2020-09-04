#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#include "vector"

#include "MadgwickAHRS.h"

static const char *TAG = "example";
static EventGroupHandle_t wifi_event_group;
const int IPV4_GOTIP_BIT = BIT0;
const int IPV6_GOTIP_BIT = BIT1;
#define PORT 3232
#define CONFIG_EXAMPLE_IPV4

#include "Mpu9250.h"
#include "SPIbus.h"

typedef struct
{
	Eigen::Vector3f gyro;
	Eigen::Vector3f accel;
	Eigen::Vector3f mag;
}imu_data;
xQueueHandle imu_queu;

std::vector<int> listened_sockets;
char str[200];



static esp_err_t event_handler(void *ctx, system_event_t *event)
{
    switch (static_cast<uint16_t>(event->event_id)) {
    case SYSTEM_EVENT_STA_START:
        esp_wifi_connect();
        ESP_LOGI(TAG, "SYSTEM_EVENT_STA_START");
        break;
    case SYSTEM_EVENT_STA_CONNECTED:
        /* enable ipv6 */
        tcpip_adapter_create_ip6_linklocal(TCPIP_ADAPTER_IF_STA);
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        xEventGroupSetBits(wifi_event_group, IPV4_GOTIP_BIT);
        ESP_LOGI(TAG, "SYSTEM_EVENT_STA_GOT_IP");
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        /* This is a workaround as ESP32 WiFi libs don't currently auto-reassociate. */
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, IPV4_GOTIP_BIT);
        xEventGroupClearBits(wifi_event_group, IPV6_GOTIP_BIT);
        break;
    case SYSTEM_EVENT_AP_STA_GOT_IP6:
    {
        xEventGroupSetBits(wifi_event_group, IPV6_GOTIP_BIT);
        ESP_LOGI(TAG, "SYSTEM_EVENT_STA_GOT_IP6");

        char *ip6 = ip6addr_ntoa(&event->event_info.got_ip6.ip6_info.ip);
        ESP_LOGI(TAG, "IPv6: %s", ip6);
    }break;
    default:
        break;
    }
    return ESP_OK;
}

static void initialise_wifi(void)
{
    tcpip_adapter_init();
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    wifi_config_t wifi_config;
    memset(&wifi_config,0,sizeof(wifi_config_t));
	memcpy(wifi_config.sta.ssid,CONFIG_ESP_WIFI_SSID,sizeof(wifi_config.sta.ssid));
	memcpy(wifi_config.sta.password,CONFIG_ESP_WIFI_PASSWORD,sizeof(wifi_config.sta.password));
	printf("%s \r\n %s \r\n",wifi_config.sta.ssid,wifi_config.sta.password);

    ESP_LOGI(TAG, "Setting WiFi configuration SSID %s...", wifi_config.sta.ssid);
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK( esp_wifi_start() );
}

static void wait_for_ip()
{
    uint32_t bits = IPV4_GOTIP_BIT | IPV6_GOTIP_BIT ;

    ESP_LOGI(TAG, "Waiting for AP connection...");
    xEventGroupWaitBits(wifi_event_group, bits, false, true, portMAX_DELAY);
    ESP_LOGI(TAG, "Connected to AP");
}

static void tcp_server_task(void *pvParameters)
{
    char rx_buffer[128];
    char addr_str[128];
    int addr_family;
    int ip_protocol;


#ifdef CONFIG_EXAMPLE_IPV4
        struct sockaddr_in destAddr;
        destAddr.sin_addr.s_addr = htonl(INADDR_ANY);
        destAddr.sin_family = AF_INET;
        destAddr.sin_port = htons(PORT);
        addr_family = AF_INET;
        ip_protocol = IPPROTO_IP;
        inet_ntoa_r(destAddr.sin_addr, addr_str, sizeof(addr_str) - 1);
#else // IPV6
        struct sockaddr_in6 destAddr;
        bzero(&destAddr.sin6_addr.un, sizeof(destAddr.sin6_addr.un));
        destAddr.sin6_family = AF_INET6;
        destAddr.sin6_port = htons(PORT);
        addr_family = AF_INET6;
        ip_protocol = IPPROTO_IPV6;
        inet6_ntoa_r(destAddr.sin6_addr, addr_str, sizeof(addr_str) - 1);
#endif

        int listen_sock = socket(addr_family, SOCK_STREAM, ip_protocol);
        if (listen_sock < 0) {
            ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        }
        ESP_LOGI(TAG, "Socket created");

        int err = bind(listen_sock, (struct sockaddr *)&destAddr, sizeof(destAddr));
        if (err != 0) {
            ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        }
        ESP_LOGI(TAG, "Socket binded");

        err = listen(listen_sock, 1);
        if (err != 0) {
            ESP_LOGE(TAG, "Error occured during listen: errno %d", errno);
        }

        while (1) {
		ESP_LOGI(TAG, "Socket listening");

		struct sockaddr_in6 sourceAddr; // Large enough for both IPv4 or IPv6
		uint addrLen = sizeof(sourceAddr);

		fd_set write_fds;
		fd_set read_fds;
		fd_set error_fds;

		FD_ZERO(&read_fds);
		FD_ZERO(&write_fds);
		FD_ZERO(&error_fds);

		int max_fd = listen_sock+1;

		while (1)
		{
			FD_SET(listen_sock,&read_fds);
			FD_SET(listen_sock,&error_fds);

			for(int socket:listened_sockets) {
				FD_SET(socket,&read_fds);
			}

			timeval delay;
			delay.tv_sec = 1;
			delay.tv_usec = 0;

			select(max_fd,&read_fds,&write_fds,&error_fds,NULL);
			ESP_LOGI(TAG,"slect");

			if(FD_ISSET(listen_sock,&read_fds))
			{

				int new_socket = accept(listen_sock, (struct sockaddr *)&sourceAddr, &addrLen);
				if (new_socket < 0) {
					ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
					continue;
				}
				//----------------------
				if(listened_sockets.size() >= 1)
				{
					close(new_socket);
					printf("reject connection \r\n");
					continue;
				}
				//----------------------
				ESP_LOGI(TAG, "Socket accepted");
				listened_sockets.push_back(new_socket);
				max_fd=(new_socket+1)>max_fd?(new_socket+1):max_fd;
				printf("cnt connection %i %i %i\r\n",listened_sockets.size(),max_fd,new_socket);
			}

			for(int i=0;i<listened_sockets.size();i++)
			{
				if(FD_ISSET(listened_sockets[i],&read_fds))
				{
					ssize_t len = read(listened_sockets[i], rx_buffer, sizeof(rx_buffer));
					if(len < 0)
					{
						listened_sockets.erase(listened_sockets.begin() + i);
						ESP_LOGI(TAG, "connection error %i %i" ,i ,len);
						perror("cnt");
						printf("cnt connection %i \r\n", listened_sockets.size());
					}
					if(len==0)
					{
						listened_sockets.erase(listened_sockets.begin()+i);
						ESP_LOGI(TAG, "connection closed");
						printf("cnt connection %i \r\n", listened_sockets.size());
					}
					if(len>0)
					{
						printf("%s", rx_buffer);
						memset(rx_buffer, 0, sizeof(rx_buffer));
					}
				}
			}
//			for(int i = 0;i < listened_sockets.size();i++)
//			{
//				const char * hell = "hello world \r\n";
//				write(listened_sockets[i] ,hell ,strlen(hell));
//			}

			FD_ZERO(&read_fds);
			FD_ZERO(&write_fds);
			FD_ZERO(&error_fds);
		}
	}
    vTaskDelete(NULL);
}



void Gy91_thread(void *pvParameters) {
	Mpu9250 avs;
	avs.init();
	imu_data imu;
	TickType_t time;
    while (1) {
    	time = xTaskGetTickCount();

    	avs.read_data();
    	imu.accel = avs.getAccel();
    	imu.gyro = avs.getAnguarVelo();
    	imu.mag = avs.getMag();
    	xQueueSend(imu_queu,&imu,0);

//    	MadgwickAHRSupdate(
//    			gyro.x(), gyro.y(), gyro.z(),
//				accel.x(), accel.y(), accel.z(),
//				0, 0, 0);
//    	printf(
//    			"%6.3f %6.3f %6.3f "
//    			"%6.3f %6.3f %6.3f"
//    			"%6.3f %6.3f %6.3f %6.3f \r\n",
//				accel.x(), accel.y(), accel.z(),
//				gyro.x(), gyro.y(), gyro.z(),
//				q0, q1, q2, q3);

//        vTaskDelay(1);
        vTaskDelayUntil(&time, 2);
    }
}

void sending_task(void *pvParameters) {

	imu_data imu;
	BaseType_t pd = pdFALSE;
	uint32_t cnt__ = 10;
	while(true){

		pd = xQueueReceive(imu_queu, &imu, portMAX_DELAY);
		if(pd == pdTRUE)
		{
			MadgwickAHRSupdate(
					imu.gyro.x(), imu.gyro.y(), imu.gyro.z(),
					imu.accel.x(), imu.accel.y(), imu.accel.z(),
					0, 0, 0);
			int strl = sprintf(str,
					"%6.3f %6.3f %6.3f "
					"%6.3f %6.3f %6.3f"
					"%6.3f %6.3f %6.3f %6.3f"
					" %i "
					"%u"
					"\r\n",
					imu.accel.x(), imu.accel.y(), imu.accel.z(),
					imu.gyro.x(), imu.gyro.y(), imu.gyro.z(),
					q0, q1, q2, q3,
					static_cast <int>(uxQueueSpacesAvailable(imu_queu)),
					xTaskGetTickCount()
					);
			for(int i = 0;i < listened_sockets.size();i++)
			{
				write(listened_sockets[i] ,str ,strl);
			}
			cnt__++;
		}

	}

}


extern "C"
{

void app_main(void)
{
    ESP_ERROR_CHECK( nvs_flash_init() );
    initialise_wifi();
    wait_for_ip();


	imu_queu = xQueueCreate(128, sizeof(imu_data));
	if(imu_queu == 0)
	{
		ESP_LOGE("ESP", "can't crate queue");while(1);
	}


    xTaskCreate(tcp_server_task, "tcp_server", 4096, NULL, 5, NULL);
    xTaskCreate(Gy91_thread, "sensor_thread", 4096, NULL, 5, NULL);
    xTaskCreate(sending_task, "task  send", 4096, NULL, 5, NULL);

}

}

