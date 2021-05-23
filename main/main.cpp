#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#include "Messagehandler.h"

#include "vector"

#include "MadgwickAHRS.h"
#include "PID.h"

#include "Mpu9250.h"
#include "LSM9DS1.h"
//#include "SPIbus.h"
#include "Madgwick.h"

#include "driver/ledc.h"

#include "command_list.h"

using namespace Eigen;

static const char *TAG = "example";
static EventGroupHandle_t wifi_event_group;
const int IPV4_GOTIP_BIT = BIT0;
const int IPV6_GOTIP_BIT = BIT1;
#define PORT 3232
#define CONFIG_EXAMPLE_IPV4
std::vector<int> listened_sockets;

Message_handler msg_handler;
int udp_sock = 0;

typedef struct
{
	Eigen::Vector3f gyro;
	Eigen::Vector3f accel;
	Eigen::Vector3f mag;
}imu_data;
xQueueHandle imu_queu;

SemaphoreHandle_t orienation_mutex;
Vector3f orientation_euler_orient(0, 0, 0);

SemaphoreHandle_t desired_orienation_mutex;
Vector3f desired_orientation(0, 0, 0);

SemaphoreHandle_t throttle_mutex;
float throttle = 0;

char str[300];

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

        char *ip6 = ip6addr_ntoa(const_cast <const ip6_addr_t *> (reinterpret_cast <ip6_addr_t * > (&event->event_info.got_ip6.ip6_info.ip)));
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
	memcpy(wifi_config.sta.ssid, CONFIG_ESP_WIFI_SSID, sizeof(CONFIG_ESP_WIFI_SSID));
	memcpy(wifi_config.sta.password, CONFIG_ESP_WIFI_PASSWORD, sizeof(CONFIG_ESP_WIFI_PASSWORD));
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
					int len = read(listened_sockets[i], rx_buffer, sizeof(rx_buffer));
					if(len < 0)
					{
						listened_sockets.erase(listened_sockets.begin() + i);
						ESP_LOGI(TAG, "connection error %i %i" ,i ,len);
						perror("cnt");
						printf("cnt connection %i \r\n", listened_sockets.size());
					}
					if(len == 0)
					{
						listened_sockets.erase(listened_sockets.begin()+i);
						ESP_LOGI(TAG, "connection closed");
						printf("cnt connection %i \r\n", listened_sockets.size());
					}
					if(len > 0)
					{
						printf("%s", rx_buffer);
						msg_handler.get_new_message(rx_buffer, len);
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

static void udp_task(void *pvParameters)
{
    char rx_buffer[128];
    char addr_str[128];
    int addr_family;
    int ip_protocol;
    struct sockaddr from;

#ifdef CONFIG_EXAMPLE_IPV4
        struct sockaddr_in destAddr;
        destAddr.sin_addr.s_addr = htonl(INADDR_ANY);
        destAddr.sin_family = AF_INET;
        destAddr.sin_port = htons(PORT);
        addr_family = AF_INET;
        ip_protocol = IPPROTO_UDP;
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

        udp_sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
        if (udp_sock < 0) {
            ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        }
        ESP_LOGI(TAG, "Socket created");

        int err = bind(udp_sock, (struct sockaddr *)&destAddr, sizeof(destAddr));
        if (err != 0) {
            ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        }
        ESP_LOGI(TAG, "Socket binded");

        while (1) {
		ESP_LOGI(TAG, "Socket listening");

		struct sockaddr_in6 sourceAddr; // Large enough for both IPv4 or IPv6
		socklen_t addrLen = sizeof(sourceAddr);

		fd_set write_fds;
		fd_set read_fds;
		fd_set error_fds;

		FD_ZERO(&read_fds);
		FD_ZERO(&write_fds);
		FD_ZERO(&error_fds);

		int max_fd = udp_sock+1;

		while (1)
		{
			FD_SET(udp_sock,&read_fds);
			FD_SET(udp_sock,&error_fds);

			timeval delay;
			delay.tv_sec = 1;
			delay.tv_usec = 0;

			select(max_fd,&read_fds, &write_fds, &error_fds,NULL);
//			ESP_LOGI(TAG,"slect");

			if(FD_ISSET(udp_sock,&read_fds))
			{
//				int rec_len = recvfrom(udp_sock, rx_buffer, 128, 0, &from, &addrLen);
				int rec_len = read(udp_sock, rx_buffer, 128);
				uint16_t comman_id = *reinterpret_cast <uint16_t*> (rx_buffer);
				void* data_ptr = rx_buffer + 2;
				switch (comman_id)
				{
				case set_throttle_comm:
				{
					struct set_throttle* ptr = reinterpret_cast <struct set_throttle*> (data_ptr);
					xSemaphoreTake(throttle_mutex, portMAX_DELAY);
					throttle = ptr->value;
					xSemaphoreGive(throttle_mutex);
				}
				break;
				case set_orientation_comm:
				{

				}
				break;
				case start_comm:
				{

				}
				break;
				case stop_comm:
				{

				}
				break;
				}

				printf("%s %i \r\n",rx_buffer, rec_len);
				memset(rx_buffer,0,128);
			}

			FD_ZERO(&read_fds);
			FD_ZERO(&write_fds);
			FD_ZERO(&error_fds);
		}
	}
    vTaskDelete(NULL);
}

void Gy91_thread(void *pvParameters) {

	imu_data imu;
	TickType_t time;
	LSM9DS1 Sensor(SPI3_HOST, 23, 19, 18, 5, 22);
	if (Sensor.init() != 0)
	{
		fprintf(stderr, "Unsuccessful initialization lsm9ds1 \n ");
		vTaskSuspend(NULL);
	}
	fprintf(stderr, "Successful initialization lsm9ds1 \n ");

    while (1) {
    	time = xTaskGetTickCount();
    	Sensor.read_data();
    	imu.accel = Sensor.get_linear_acellration();
    	imu.gyro =  Sensor.get_angular_velo() * M_PI / 180.0f;
    	imu.mag = Sensor.get_magnetic_field();

    	xQueueSend(imu_queu,&imu,0);

        vTaskDelayUntil(&time, 10);
    }
}

Vector3f ToEulerAngles(Quaternionf& q) {
	Vector3f angles;

    // roll (x-axis rotation)
	float sinr_cosp = 2 * (q.w() * q.x() + q.y() * q.z());
	float cosr_cosp = 1 - 2 * (q.x() * q.x() + q.y() * q.y());
    angles[0] = std::atan2(sinr_cosp, cosr_cosp);

    // pitch (y-axis rotation)
    float sinp = 2 * (q.w() * q.y() - q.z() * q.x());
    if (std::abs(sinp) >= 1)
        angles[1] = std::copysign(M_PI / 2, sinp); // use 90 degrees if out of range
    else
        angles[1] = std::asin(sinp);

    // yaw (z-axis rotation)
    float siny_cosp = 2 * (q.w() * q.z() + q.x() * q.y());
    float cosy_cosp = 1 - 2 * (q.y() * q.y() + q.z() * q.z());
    angles[2] = std::atan2(siny_cosp, cosy_cosp);

    return angles;
}

void sending_task(void *pvParameters) {

	imu_data imu;
	BaseType_t pd = pdFALSE;
	uint32_t cnt__ = 10;
    Madgwick_filter madgwick;
	Quaternionf orientation;

	struct sockaddr_in to;
	to.sin_family = AF_INET;
	to.sin_port = htons(PORT);
	to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
	while(true){

		pd = xQueueReceive(imu_queu, &imu, portMAX_DELAY);
		if(pd == pdTRUE)
		{
//--------------------------------------------------------------
//			orientation_avs += imu.gyro * 0.01;
//			printf("%11.4f %11.4f %11.4f "
//					"%11.4f %11.4f %11.4f \n",
//					imu.gyro.x(), imu.gyro.y(), imu.gyro.z(),
//					orientation_avs.x(), orientation_avs.y(), orientation_avs.z());
//			continue;
//--------------------------------------------------------------
//			MadgwickAHRSupdateIMU(
//					imu.gyro.x(), imu.gyro.y(), imu.gyro.z(),
//					imu.accel.x(), imu.accel.y(), imu.accel.z());
//			orentation = Quaternionf(
//					const_cast<float&>(q0),
//					const_cast<float&>(q1),
//					const_cast<float&>(q2),
//					const_cast<float&>(q3)
//					);
//--------------------------------------------------------------
//			MadgwickAHRSupdate(
//					imu.gyro.x(), imu.gyro.y(), imu.gyro.z(),
//					imu.accel.x(), imu.accel.y(), imu.accel.z(),
//					imu.mag.x(), imu.mag.y(), imu.mag.z());
//			orientation = Quaternionf(
//					const_cast<float&>(q0),
//					const_cast<float&>(q1),
//					const_cast<float&>(q2),
//					const_cast<float&>(q3)
//					);
//---------------------------------------------------------------------
			Vector3d ref_1_accel = Vector3d(0, 0, 1);
			Vector3d meas_1_accel (imu.accel.x(), imu.accel.y(), imu.accel.z());
			meas_1_accel.normalize();
			Vector3d gyro (imu.gyro.x(), imu.gyro.y(), imu.gyro.z());

			madgwick.update(ref_1_accel, meas_1_accel, gyro, 0.01);
			Quaterniond madgw_orient = madgwick.get_orientaion();
			orientation = Quaternionf(madgw_orient.w(), madgw_orient.x(), madgw_orient.y(), madgw_orient.z());

//---------------------------------------------------------------------
			Vector3f euler = ToEulerAngles(orientation) * 180.0f / M_PI;

			xSemaphoreTake(orienation_mutex, portMAX_DELAY);
			orientation_euler_orient = euler;
			xSemaphoreGive(orienation_mutex);

			Vector3f omega_deg = imu.gyro * 180 / M_PI;
			Vector3d omega_bias_deg = madgwick.getOmega_bias()  * 180 / M_PI;

			if(0)
			{
				int strl = sprintf(str,
						"%11.4f %11.4f %11.4f %11.4f "
						"%11.4f %11.4f %11.4f %11.4f "
						"%11.4f %11.4f %11.4f %11.4f "
						"%11.4f %11.4f %11.4f %11.4f "
						"%11.4f %11.4f %11.4f "
						"%i "
						"%u "
						"\r\n",
						imu.accel.x(), imu.accel.y(), imu.accel.z(), imu.accel.norm(),
						omega_deg.x(), omega_deg.y(), omega_deg.z(), omega_deg.norm(),
						omega_bias_deg.x(), omega_bias_deg.y(), omega_bias_deg.z(), omega_bias_deg.norm(),
						q0, q1, q2, q3,
						euler.x(),euler.y(),euler.z(),
	 					static_cast <int>(uxQueueSpacesAvailable(imu_queu)),
						xTaskGetTickCount()
						);
				if(udp_sock != 0 )
				{
					for(int i = 0;i < listened_sockets.size();i++)
					{
						write(listened_sockets[i] ,str ,strl);
					}

					sendto(udp_sock, str, strl, 0, reinterpret_cast <sockaddr *> (&to), sizeof(struct sockaddr_in));
				}
				cnt__++;
			}

		}

	}

}

uint32_t normalise_duty(float pid_res)
{
	if(pid_res < 0.0)
		pid_res = 0.0;
	else if(pid_res > 1.0)
		pid_res = 1.0;
	return static_cast<uint32_t>(pid_res * 8000);
}

Vector4f calc_channel(Vector3f pid_res, float throttle)
{
	Vector4f out;
	float pt = pid_res[0];
	float ro = pid_res[1];
	float ya = pid_res[2];
	out[0] = throttle + pt / 2 - ro / 2 + ya / 2;
	out[1] = throttle - pt / 2 - ro / 2 - ya / 2;
	out[2] = throttle - pt / 2 + ro / 2 + ya / 2;
	out[3] = throttle + pt / 2 + ro / 2 - ya / 2;
	return out;
}

void quadro_control(void *pvParameters) {
	ledc_timer_config_t ledc_timer ;
	ledc_timer.duty_resolution = LEDC_TIMER_13_BIT, // resolution of PWM duty
	ledc_timer.freq_hz = 500,                      // frequency of PWM signal
	ledc_timer.speed_mode = LEDC_LOW_SPEED_MODE,           // timer mode
	ledc_timer.timer_num = LEDC_TIMER_1,            // timer index
	ledc_timer.clk_cfg = LEDC_AUTO_CLK,              // Auto select the source clock

	// Set configuration of timer0 for high speed channels
	ledc_timer_config(&ledc_timer);

//    ledc_timer.speed_mode = LEDC_LOW_SPEED_MODE;
    ledc_timer.timer_num = LEDC_TIMER_1;
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel[4];

	ledc_channel[0].channel    = LEDC_CHANNEL_0;
	ledc_channel[0].duty       = 0;
	ledc_channel[0].gpio_num   = 13;
	ledc_channel[0].speed_mode = LEDC_LOW_SPEED_MODE;
	ledc_channel[0].hpoint     = 0;
	ledc_channel[0].timer_sel  = LEDC_TIMER_1;

	ledc_channel[1].channel    = LEDC_CHANNEL_1;
	ledc_channel[1].duty       = 0;
	ledc_channel[1].gpio_num   = 12;
	ledc_channel[1].speed_mode = LEDC_LOW_SPEED_MODE;
	ledc_channel[1].hpoint     = 0;
	ledc_channel[1].timer_sel  = LEDC_TIMER_1;

	ledc_channel[2].channel    = LEDC_CHANNEL_2;
	ledc_channel[2].duty       = 0;
	ledc_channel[2].gpio_num   = 27;
	ledc_channel[2].speed_mode = LEDC_LOW_SPEED_MODE;
	ledc_channel[2].hpoint     = 0;
	ledc_channel[2].timer_sel  = LEDC_TIMER_1;

	ledc_channel[3].channel    = LEDC_CHANNEL_3;
	ledc_channel[3].duty       = 0;
	ledc_channel[3].gpio_num   = 14;
	ledc_channel[3].speed_mode = LEDC_LOW_SPEED_MODE;
	ledc_channel[3].hpoint     = 0;
	ledc_channel[3].timer_sel  = LEDC_TIMER_1;

	ledc_channel_config(&ledc_channel[0]);
	ledc_channel_config(&ledc_channel[1]);
	ledc_channel_config(&ledc_channel[2]);
	ledc_channel_config(&ledc_channel[3]);

	Vector3f orientation_;
	Vector3f target_orientation_;

	PID <float, float> PID_PITCH;
	PID <float, float> PID_ROLL;
	PID <float, float> PID_YAW;
	PID_PITCH.setKp(0.05);
	PID_ROLL.setKp(0.05);
	PID_YAW.setKp(0);

	struct sockaddr_in to;
	to.sin_family = AF_INET;
	to.sin_port = htons(PORT);
	to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
	uint32_t cnt__ = 10;

	while(true) {
		xSemaphoreTake(orienation_mutex, portMAX_DELAY);
		orientation_ = orientation_euler_orient;
		xSemaphoreGive(orienation_mutex);

		xSemaphoreTake(orienation_mutex, portMAX_DELAY);
		target_orientation_ = desired_orientation;
		xSemaphoreGive(orienation_mutex);

		PID_PITCH.update(target_orientation_[0], orientation_ [0], 0.01);
		PID_ROLL.update(target_orientation_[1], orientation_ [1], 0.01);
		PID_YAW.update(target_orientation_[2], orientation_ [2], 0.01);
		Vector3f pid_out (PID_PITCH.getOutput(), PID_ROLL.getOutput(), PID_YAW.getOutput());
		Vector4f res = calc_channel(pid_out, 0.1);

		uint32_t tick_cnt = xTaskGetTickCount();
		float time = (static_cast <float> (tick_cnt)) * 1e-3f;
		float omega = 10;

		ledc_channel[0].duty = 0; //normalise_duty(res[0]); //static_cast<uint32_t>((cosf(time * omega                     ) + 1) * 4000.0f);
		ledc_channel[1].duty = 0; //normalise_duty(res[1]); //static_cast<uint32_t>((cosf(time * omega +        M_PI / 4.0f) + 1) * 4000.0f);
		ledc_channel[2].duty = 0; //normalise_duty(res[2]); //static_cast<uint32_t>((cosf(time * omega +        M_PI / 2.0f) + 1) * 4000.0f);
		ledc_channel[3].duty = 0; //normalise_duty(res[3]); //static_cast<uint32_t>((cosf(time * omega + 3.0f * M_PI / 4.0f) + 1) * 4000.0f);

		if(1)
		{
			int strl = sprintf(str,
					"%u %u %u %u "
					"%7.4f %7.4f %7.4f "
					"%7.4f %7.4f %7.4f"
					"\r\n",
					normalise_duty(res[0]), normalise_duty(res[1]), normalise_duty(res[2]), normalise_duty(res[3]),
					orientation_[0], orientation_[1], orientation_[2],
					target_orientation_[0], target_orientation_[1], target_orientation_[2]
					);
			if(udp_sock != 0 )
			{
				for(int i = 0;i < listened_sockets.size();i++)
				{
					write(listened_sockets[i] ,str ,strl);
				}

				sendto(udp_sock, str, strl, 0, reinterpret_cast <sockaddr *> (&to), sizeof(struct sockaddr_in));
			}
			cnt__++;
		}

		ledc_channel_config(&ledc_channel[0]);
		ledc_channel_config(&ledc_channel[1]);
		ledc_channel_config(&ledc_channel[2]);
		ledc_channel_config(&ledc_channel[3]);

		vTaskDelay(10);
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
		ESP_LOGE("ESP", "can't create queue");
		while(1);
	}

	orienation_mutex = xSemaphoreCreateMutex();
	if(orienation_mutex == 0)
	{
		ESP_LOGE("ESP", "can't create mutex");
		while(1);
	}
	
	desired_orienation_mutex = xSemaphoreCreateMutex();
	if(desired_orienation_mutex == 0)
	{
		ESP_LOGE("ESP", "can't create mutex");
		while(1);
	}

	throttle_mutex = xSemaphoreCreateMutex();
	if(throttle_mutex == 0)
	{
		ESP_LOGE("ESP", "can't create mutex");
		while(1);
	}

    xTaskCreate(udp_task, "udp_server", 4096, NULL, 5, NULL);

    xTaskCreate(tcp_server_task, "tcp_server", 4096, NULL, 5, NULL);

    xTaskCreate(Gy91_thread, "sensor_thread", 4096, NULL, 5, NULL);
    xTaskCreate(sending_task, "task  send", 4096, NULL, 5, NULL);
    xTaskCreate(quadro_control, "define pwm", 4096, NULL, 5, NULL);

}

}

