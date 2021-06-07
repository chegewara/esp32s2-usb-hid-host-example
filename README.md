## Simple usb host HID example 
Since usb host support is still beta stage and include file is in private_include i copied component to this example. 
Code has been refactored to use events in client code. 

## Example logs:

### Before connecting device
```
Hello world USB host!
start pipe event task
I (332) : USB host setup properly
I (332) : Port is power ON now
I (342) : port event: 1
start pipe event task
```

### After connecting device
```
I (592) : HCD_PORT_EVENT_CONNECTION
I (592) : HCD_PORT_STATE_DISABLED
I (652) : USB device reset
I (652) : HCD_PORT_STATE_ENABLED
I (652) : Creating default pipe
...
I (689) STRING CB: [4] 
strings: 
I (693) STRING CB: [4] 
strings: 
I (697) STRING CB: [46] . USB OPTICAL MOUSE
strings:  USB OPTICAL MOUSE
I (705) ADDRESS: 1
I (708) SET CONFIG: 1
```

### Example reading device descriptors and strings
- device descriptor on EP0
```
Device descriptor:
Length: 18
Descriptor type: 1
USB version: 1.10
Device class: 0x00 (>ifc)
Device subclass: 0x00
Device protocol: 0x00
EP0 max packet size: 8
VID: 0x0000
PID: 0x6800
Revision number: 1.00
Manufacturer id: 0
Product id: 1
Serial id: 0
Configurations num: 1
```

- configuration descriptor
```
Config:
Number of Interfaces: 1
Attributes: 0xa0
Max power: 100 mA

Interface:
bInterfaceNumber: 0
bAlternateSetting: 0
bNumEndpoints: 1
bInterfaceClass: 0x03 (HID)
bInterfaceSubClass: 0x01
bInterfaceProtocol: 0x02
I (750) : HID descriptor
I (753) Report map size: 0x42

Endpoint:
bEndpointAddress: 0x81
bmAttributes: 0x03
bDescriptorType: 5
wMaxPacketSize: 6
bInterval: 10 ms
```


## Mouse reports
Mouse reports may vary, depending on connected device. My mouse is sending 6 bytes report. 1st byte is always report ID, then depending on HID report map we can have 1 byte for buttons, 2 x 12 bits (in my case) for X and Y axes delta and 1 byte for wheel delta:
```
I (3256) HID REPORT ID: 1
I (3256) Mouse buttons: 0
I (3257) X/Y axes: 45/112/254 (3 bytes)
I (3257) Mouse wheel: 0

I (3264) HID REPORT ID: 1
I (3264) Mouse buttons: 0
I (3265) X/Y axes: 63/160/253
I (3269) Mouse wheel: 0

I (3280) HID REPORT ID: 1
I (3280) Mouse buttons: 0
I (3281) X/Y axes: 81/112/252
I (3283) Mouse wheel: 0

I (3288) HID REPORT ID: 1
I (3289) Mouse buttons: 0
I (3292) X/Y axes: 3/240/255
I (3296) Mouse wheel: 0
```

Have a nice play.
