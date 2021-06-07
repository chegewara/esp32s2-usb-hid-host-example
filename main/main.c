/* USB host example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_intr_alloc.h"
#include "esp_err.h"
#include "esp_attr.h"
#include "esp_rom_gpio.h"
#include "soc/gpio_pins.h"
#include "soc/gpio_sig_map.h"
#include "hal/usbh_ll.h"
#include "hcd.h"
#include "esp_log.h"
#include "ctrl_pipe.h"
#include "usb_host_port.h"

// #define USE_ALTERNATIVE_CALLBACKS
extern void hid_pipe_event_task(void* p);
extern void xfer_in_data();
extern void xfer_set_idle(hcd_port_handle_t port_hdl, hcd_pipe_handle_t handle);
extern void get_hid_report_descriptor(hcd_port_handle_t port_hdl, hcd_pipe_handle_t handle);

uint8_t device_state = 0;
uint8_t conf_num;
static hcd_port_handle_t _port_hdl;

hcd_pipe_handle_t ctrl_pipe_hdl;

uint8_t bMaxPacketSize0 = 64;
uint8_t conf_num = 0;
void parse_cfg_descriptor(uint8_t *data_buffer, usb_transfer_status_t status, uint8_t len, uint8_t *conf_num);

static void utf16_to_utf8(char *in, char *out, uint8_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        out[i / 2] = in[i];
        i++;
    }
}

/*------------------------------- USBH EP0 CTRL PIPE CALLBACKS -------------------------------*/
void usbh_get_device_desc_cb(uint8_t *data_buffer, size_t num_bytes, void *context)
{
    ESP_LOG_BUFFER_HEX_LEVEL("DEVICE descriptor", data_buffer, num_bytes, ESP_LOG_INFO);
    parse_cfg_descriptor(data_buffer, 0, num_bytes, &conf_num);

    usb_desc_devc_t* desc = (usb_desc_devc_t*)data_buffer;
    xfer_get_string(port_hdl, ctrl_pipe_hdl, desc->iManufacturer);
    xfer_get_string(port_hdl, ctrl_pipe_hdl, desc->iSerialNumber);
    xfer_get_string(port_hdl, ctrl_pipe_hdl, desc->iProduct);

    xfer_set_address(port_hdl, ctrl_pipe_hdl, DEVICE_ADDR);
}

void usbh_set_address_cb(uint16_t addr, void *context)
{
    ESP_LOGI("ADDRESS", "%d", addr);
    if (ESP_OK != hcd_pipe_update_dev_addr(ctrl_pipe_hdl, DEVICE_ADDR))
        ESP_LOGE("", "failed to update device address");
    xfer_set_configuration(port_hdl, ctrl_pipe_hdl, 1);
}

void usbh_get_config_desc_cb(uint8_t *data_buffer, size_t num_bytes, void *context)
{
    ESP_LOG_BUFFER_HEX_LEVEL("CONFIG descriptor", data_buffer, num_bytes, ESP_LOG_INFO);
    parse_cfg_descriptor(data_buffer, 0, num_bytes, &conf_num);

    // xfer_get_current_config(port_hdl, ctrl_pipe_hdl);
    xfer_set_idle(port_hdl, ctrl_pipe_hdl);
    vTaskDelay(5);
    get_hid_report_descriptor(port_hdl, ctrl_pipe_hdl);
    vTaskDelay(5);
    xfer_in_data();
}

void usbh_set_config_desc_cb(uint16_t data, void *context)
{
    ESP_LOGI("SET CONFIG", "%d", data);
    xfer_get_desc(port_hdl, ctrl_pipe_hdl);
}

void usbh_get_string_cb(uint8_t *data, size_t num_bytes, void *context)
{
    char out[64] = {};
    utf16_to_utf8((char *)data, out, num_bytes);
    ESP_LOGI("STRING CB", "[%d] %s", num_bytes, out);
    parse_cfg_descriptor(data, 0, num_bytes, &conf_num);
}

void usbh_ctrl_pipe_stalled_cb(usb_ctrl_req_t *ctrl)
{
    ESP_LOG_BUFFER_HEX_LEVEL("STALLED", ctrl, 8, ESP_LOG_WARN);
}

void usbh_ctrl_pipe_error_cb(usb_ctrl_req_t *ctrl)
{
    ESP_LOG_BUFFER_HEX_LEVEL("ERROR", ctrl, 8, ESP_LOG_WARN);
}

void usbh_get_configuration_cb(uint8_t addr, void *context)
{
    ESP_LOGI("GET CONFIG", "%d", addr);
}

/*------------------------------- USBH EP0 CTRL PIPE CALLBACKS -------------------------------*/

/*------------------------------- USBH PORT CALLBACKS -------------------------------*/

void usbh_port_connection_cb(port_event_msg_t msg)
{
    hcd_port_state_t state;
    ESP_LOGI("", "HCD_PORT_EVENT_CONNECTION");
    if (HCD_PORT_STATE_DISABLED == hcd_port_get_state(msg.port_hdl))
        ESP_LOGI("", "HCD_PORT_STATE_DISABLED");
    if (ESP_OK == hcd_port_command(msg.port_hdl, HCD_PORT_CMD_RESET))
        ESP_LOGI("", "USB device reset");
    else
        return;
    if (HCD_PORT_STATE_ENABLED == hcd_port_get_state(msg.port_hdl))
    {
        ESP_LOGI("", "HCD_PORT_STATE_ENABLED");
        // we are already physically connected and ready, now we can perform software connection steps
        allocate_ctrl_pipe(msg.port_hdl, &ctrl_pipe_hdl);
        // get device descriptor on EP0, this is first mandatory step
        xfer_get_device_desc(msg.port_hdl, ctrl_pipe_hdl);
        port_hdl = msg.port_hdl;
    }
}

void usbh_port_disconnection_cb(port_event_msg_t msg) {}
void usbh_port_error_cb(port_event_msg_t msg) {}
void usbh_port_overcurrent_cb(port_event_msg_t msg) {}

void usbh_port_sudden_disconn_cb(port_event_msg_t msg)
{
    hcd_port_state_t state;
    if (ctrl_pipe_hdl != NULL && HCD_PIPE_STATE_INVALID == hcd_pipe_get_state(ctrl_pipe_hdl))
    {
        ESP_LOGW("", "pipe state: %d", hcd_pipe_get_state(ctrl_pipe_hdl));
        free_pipe_and_irp_list(ctrl_pipe_hdl);
        ctrl_pipe_hdl = NULL;

        esp_err_t err;
        if (HCD_PORT_STATE_RECOVERY == (state = hcd_port_get_state(msg.port_hdl)))
        {
            if (ESP_OK != (err = hcd_port_recover(msg.port_hdl)))
                ESP_LOGE("recovery", "should be not powered state %d => (%d)", state, err);
        }
        else
        {
            ESP_LOGE("", "hcd_port_state_t: %d", state);
        }
        if (ESP_OK == hcd_port_command(msg.port_hdl, HCD_PORT_CMD_POWER_ON))
            ESP_LOGI("", "Port powered ON");
    }
}

/*------------------------------- USBH PORT CALLBACKS -------------------------------*/


void app_main(void)
{
    printf("Hello world USB host!\n");
    xTaskCreate(hid_pipe_event_task, "pipe_task", 4*1024, NULL, 10, NULL);

    if (setup_usb_host())
    {
        xTaskCreate(ctrl_pipe_event_task, "pipe_task", 4 * 1024, NULL, 10, NULL);
    }

    while (1)
    {
        vTaskDelay(1000);
    }
}
