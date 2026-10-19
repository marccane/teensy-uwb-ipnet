// USB strings, so udev rules can match the board without caring about the
// Teensy serial number. The serial number keeps the Teensy default (unique
// per board, derived from its factory MAC).

#include <usb_names.h>

#define MANUFACTURER_NAME {'u', 'w', 'b', 'n', 'e', 't'}
#define MANUFACTURER_NAME_LEN 6

#define PRODUCT_NAME {'U', 'W', 'B', '-', 'N', 'e', 't'}
#define PRODUCT_NAME_LEN 7

struct usb_string_descriptor_struct usb_string_manufacturer_name = {
    2 + MANUFACTURER_NAME_LEN * 2,
    3,
    MANUFACTURER_NAME,
};

struct usb_string_descriptor_struct usb_string_product_name = {
    2 + PRODUCT_NAME_LEN * 2,
    3,
    PRODUCT_NAME,
};
