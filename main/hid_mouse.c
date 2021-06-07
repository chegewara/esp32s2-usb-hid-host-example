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
#include "hcd.h"
#include "ctrl_pipe.h"
#include "usb_host_port.h"
#include <string.h>

#define USB_DESC_EP_GET_ADDRESS(desc_ptr) ((desc_ptr).bEndpointAddress & 0x7F)
#define SET_VALUE       0x21
#define SET_IDLE        0x0A

#define USB_CTRL_SET_IDLE(ctrl_req_ptr) ({  \
    (ctrl_req_ptr)->bRequestType = SET_VALUE;   \
    (ctrl_req_ptr)->bRequest = SET_IDLE;  \
    (ctrl_req_ptr)->wValue = (0x0);   \
    (ctrl_req_ptr)->wIndex = (0);    \
    (ctrl_req_ptr)->wLength = (0);   \
})

#define USB_CTRL_GET_HID_REPORT_DESC(ctrl_req_ptr, desc_index, desc_len) ({  \
    (ctrl_req_ptr)->bRequestType = USB_B_REQUEST_TYPE_DIR_IN | USB_B_REQUEST_TYPE_TYPE_STANDARD | USB_B_REQUEST_TYPE_RECIP_INTERFACE;   \
    (ctrl_req_ptr)->bRequest = USB_B_REQUEST_GET_DESCRIPTOR;   \
    (ctrl_req_ptr)->wValue = (0x22<<8) | ((desc_index) & 0xFF); \
    (ctrl_req_ptr)->wIndex = 0;    \
    (ctrl_req_ptr)->wLength = (desc_len);  \
})

static hcd_pipe_handle_t hid_handle;
static QueueHandle_t hid_pipe_evt_queue;
// static ctrl_pipe_cb_t hid_pipe_cb;
static usb_desc_ep_t endpoint;
void xfer_in_data();

static void hid_pipe_cb(pipe_event_msg_t msg, usb_irp_t *irp, void *context)
{
    ESP_LOGD("", "\t-> Pipe [%d] event: %d\n", (uint8_t)context, msg.pipe_event);

    switch (msg.pipe_event)
    {
        case HCD_PIPE_EVENT_NONE:
            break;

        case HCD_PIPE_EVENT_IRP_DONE:
            ESP_LOGD("Pipe cdc: ", "XFER status: %d, num bytes: %d, actual bytes: %d", irp->status, irp->num_bytes, irp->actual_num_bytes);
            // ESP_LOG_BUFFER_HEX("", irp->data_buffer, irp->num_bytes);
            ESP_LOGI("HID REPORT ID", "%d", irp->data_buffer[0]);
            ESP_LOGI("Mouse buttons", "%d", irp->data_buffer[1]);
            ESP_LOGI("X/Y axes", "%d/%d/%d", irp->data_buffer[2], irp->data_buffer[3], irp->data_buffer[4]);
            ESP_LOGI("Mouse wheel", "%d\n", irp->data_buffer[5]);
            break;

        case HCD_PIPE_EVENT_ERROR_XFER:
            ESP_LOGW("", "XFER error: %d", irp->status);
            hcd_pipe_command(msg.pipe_hdl, HCD_PIPE_CMD_RESET);
            break;
        
        case HCD_PIPE_EVENT_ERROR_STALL:
            ESP_LOGW("", "Device stalled: %s pipe, state: %d", "INTR", hcd_pipe_get_state(msg.pipe_hdl));
            hcd_pipe_command(msg.pipe_hdl, HCD_PIPE_CMD_RESET);
            break;
        
        default:
            ESP_LOGW("", "not handled pipe event: %d", msg.pipe_event);
            break;
    }
    xfer_in_data();
}

void hid_pipe_event_task(void* p)
{
    printf("start pipe event task\n");
    pipe_event_msg_t msg;
    hid_pipe_evt_queue = xQueueCreate(10, sizeof(pipe_event_msg_t));

    while(1){
        if(xQueueReceive(hid_pipe_evt_queue, &msg, portMAX_DELAY) == pdTRUE)
        {
            usb_irp_t* irp = hcd_irp_dequeue(msg.pipe_hdl);
            hcd_pipe_handle_t pipe_hdl;
            if(irp == NULL) continue;
            void *context = irp->context;

            hid_pipe_cb(msg, irp, context);
        }
    }
}

static bool hid_pipe_callback(hcd_pipe_handle_t pipe_hdl, hcd_pipe_event_t pipe_event, void *user_arg, bool in_isr)
{
    QueueHandle_t pipe_evt_queue = (QueueHandle_t)user_arg;
    pipe_event_msg_t msg = {
        .pipe_hdl = pipe_hdl,
        .pipe_event = pipe_event,
    };
    if (in_isr) {
        BaseType_t xTaskWoken = pdFALSE;
        xQueueSendFromISR(pipe_evt_queue, &msg, &xTaskWoken);
        return (xTaskWoken == pdTRUE);
    } else {
        xQueueSend(pipe_evt_queue, &msg, portMAX_DELAY);
        return false;
    }
}

static void hid_allocate_pipe(usb_desc_ep_t* ep)
{
    //We don't support hubs yet. Just get the speed of the port to determine the speed of the device
    usb_speed_t port_speed;
    if (ESP_OK == hcd_port_get_speed(port_hdl, &port_speed)){}

    //Create default pipe
    ESP_LOGI("", "Creating default pipe\n");
    hcd_pipe_config_t config = {
        .callback = hid_pipe_callback,
        .callback_arg = (void *)hid_pipe_evt_queue,
        .context = NULL,
        .ep_desc = ep,
        .dev_addr = DEVICE_ADDR,
        .dev_speed = port_speed,
    };
    if (ESP_OK != hcd_pipe_alloc(port_hdl, &config, &hid_handle))
        ESP_LOGE("", "cant alloc pipe");
    if (NULL == hid_handle)
    {
        ESP_LOGE("", "NULL == pipe_hdl");
    }
}

void hid_create_pipe(usb_desc_ep_t* ep)
{
    hid_allocate_pipe(ep);
    memcpy(&endpoint, ep, sizeof(usb_desc_ep_t));
}

static usb_irp_t *allocate_hid_irp(hcd_port_handle_t port_hdl, size_t size)
{
    //Create IRPs and their data buffers
    ESP_LOGD("", "Creating new IRP, free memory: %d", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    usb_irp_t *_irp = heap_caps_calloc(1, sizeof(usb_irp_t), MALLOC_CAP_DEFAULT);
    if (NULL == _irp)
        ESP_LOGE("", "err to alloc IRP");
    //Allocate data buffer
    uint8_t *_data_buffer = heap_caps_calloc(1, sizeof(usb_ctrl_req_t) + size, MALLOC_CAP_DMA);
    if (NULL == _data_buffer)
        ESP_LOGE("", "err to alloc data buffer");
    //Initialize IRP and IRP list
    _irp->data_buffer = _data_buffer;
    _irp->num_iso_packets = 0;
    _irp->num_bytes = size;

    return _irp;
}

void xfer_in_data()
{
    usb_irp_t *irp = allocate_hid_irp(port_hdl, 6);
    ESP_LOGD("", "EP: 0x%02x", USB_DESC_EP_GET_ADDRESS(endpoint));

    esp_err_t err;
    if(ESP_OK != (err = hcd_irp_enqueue(hid_handle, irp))) {
        ESP_LOGW("", "INTR irp enqueue err: 0x%x", err);
    }
}

void xfer_set_idle(hcd_port_handle_t port_hdl, hcd_pipe_handle_t handle)
{
    usb_irp_t *irp = allocate_hid_irp(port_hdl, 0);
    USB_CTRL_SET_IDLE((usb_ctrl_req_t *)irp->data_buffer);

    esp_err_t err;
    if(ESP_OK != (err = hcd_irp_enqueue(handle, irp))) {
        ESP_LOGW("", "SET_IDLE enqueue err: 0x%x", err);
    }
}

void get_hid_report_descriptor(hcd_port_handle_t port_hdl, hcd_pipe_handle_t handle)
{
    extern uint16_t report_map_size;
    usb_irp_t *irp = allocate_hid_irp(port_hdl, report_map_size);
    USB_CTRL_GET_HID_REPORT_DESC((usb_ctrl_req_t *)irp->data_buffer, 0, report_map_size);

    esp_err_t err;
    if(ESP_OK != (err = hcd_irp_enqueue(handle, irp))) {
        ESP_LOGW("", "SET_IDLE enqueue err: 0x%x", err);
    }
}
