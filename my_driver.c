#include <linux/module.h>
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/input.h>

#define XBOX_VENDOR_ID       0x045e
#define XBOX_SERIES_S_PID    0x0b12

struct xbox {
    struct usb_device *udev;
    struct usb_interface *interface;
    struct urb *irq_in_urb;
    unsigned char *irq_in_buffer;
    struct input_dev *input;
};

struct xbox *xbox_dev;

static const u8 xbox_init_packets[] = { 0x05, 0x20, 0x00, 0x01, 0x00 };           // welcome/init

static const size_t xbox_init_length = 5;

/* USB IDs */
static const struct usb_device_id xbox_table[] = {
    { USB_DEVICE(XBOX_VENDOR_ID, XBOX_SERIES_S_PID) },
    {}
};
MODULE_DEVICE_TABLE(usb, xbox_table);

s16 left_stick_x, left_stick_y, right_stick_x, right_stick_y;
u16 left_trigger, right_trigger;

/* Convert raw report into input events */
static void xbox_parse_input(struct xbox *xbox_dev, u8 *data, int len)
{
    struct input_dev *input = xbox_dev->input;
    if (len < 14){
        if (len == 6){
            input_report_key(input, BTN_MODE, data[4] & 0x01);
            input_sync(input);
        }
        return;
    }
    // Letter buttons
    input_report_key(input, BTN_A,      data[4] & 0x10);
    input_report_key(input, BTN_B,      data[4] & 0x20);
    input_report_key(input, BTN_Y,      data[4] & 0x40);
    input_report_key(input, BTN_X,      data[4] & 0x80);

    // select and start
    input_report_key(input, BTN_SELECT, data[4] & 0x08);
    input_report_key(input, BTN_START,  data[4] & 0x04);

    //DPAD
    input_report_key(input, BTN_DPAD_UP, data[5] & 0x01);
    input_report_key(input, BTN_DPAD_DOWN, data[5] & 0x02);
    input_report_key(input, BTN_DPAD_LEFT, data[5] & 0x04);
    input_report_key(input, BTN_DPAD_RIGHT, data[5] & 0x08);

    //bumpers
    input_report_key(input, BTN_TL, data[5] & 0x10);
    input_report_key(input, BTN_TR, data[5] & 0x20);

    //stick presses
    input_report_key(input, BTN_THUMBL, data[5] & 0x40);
    input_report_key(input, BTN_THUMBR, data[5] & 0x80);

    // sticks - signed 16-bit integers LE
    // left stick x is (s16)(data[10] | (data[11] << 8))
    // left stick y is (s16)(data[12] | (data[13] << 8))
    // right stick x is (s16)(data[14] | (data[15] << 8))
    // right stick y is (s16)(data[16] | (data[17] << 8))

    left_stick_x = (s16)(data[10] | (data[11] << 8));
    left_stick_y = (s16)(data[12] | (data[13] << 8));
    right_stick_x = (s16)(data[14] | (data[15] << 8));
    right_stick_y = (s16)(data[16] | (data[17] << 8));

    input_report_abs(input, ABS_X,  left_stick_x);
    input_report_abs(input, ABS_Y,  -left_stick_y);
    input_report_abs(input, ABS_RX, right_stick_x);
    input_report_abs(input, ABS_RY, -right_stick_y);

    // triggers - unsigned 8-bit integers
    // left trigger is (u8)(data[6])
    // right trigger is (u8)(data[8])

    left_trigger = (u16)(data[6] | (data[7] << 8));
    right_trigger = (u16)(data[8] | (data[9] << 8));

    input_report_abs(input, ABS_Z, left_trigger);
    input_report_abs(input, ABS_RZ, right_trigger);

    input_report_key(input, BTN_TL2, left_trigger);
    input_report_key(input, BTN_TR2, right_trigger);
    input_sync(input);
}

/* URB callback */
static void xbox_irq_in(struct urb *urb)
{
    struct xbox *xbox_dev = urb->context;
    int retval;

    xbox_parse_input(xbox_dev, urb->transfer_buffer, urb->actual_length);

    retval = usb_submit_urb(urb, GFP_ATOMIC);
    if (retval)
        pr_err("my_driver: resubmit URB failed: %d\n", retval);
}

/* Initialize controller */
static int xbox_initialize_controller(struct xbox *xbox_dev)
{
    int retval;

    retval = usb_interrupt_msg(
        xbox_dev->udev, //uchwyt do struktury urzadzenia
        usb_sndintpipe(xbox_dev->udev, 0x02), //utworzenie potoku do endpointa nr 2 w padzie (lsusb -v -d 045e:012b )
        (void *)xbox_init_packets,//wartosc pakietu poczatkowego
        xbox_init_length,//dlugosci pakietu
        NULL, //rzeczywista dlugosc przeslanych danych (int *) - NULL jezeli nas nie interesuje
        100 //czas ile czeka na udane wyslanie pakietu (ms)
    );
    if (retval < 0) {
        pr_err("my_driver: failed to send init packet, err=%d\n", retval);
        return retval;
    }

    pr_info("my_driver: controller initialized\n");
    return 0;
}

struct usb_device *udev;
int pipe, maxp, retval;
/* Probe */
static int xbox_probe(struct usb_interface *interface,
                      const struct usb_device_id *id)
{
    int ifnum = interface->cur_altsetting->desc.bInterfaceNumber;
    // do not link with other interfaces just the one with number 0 because:
    // 0 is key control
    // 1 is headphones
    // 2 is rumble controls
    if (ifnum != 0)
        return -ENODEV;

    udev = interface_to_usbdev(interface);

    pr_info("my_driver: device connected %04x:%04x\n", id->idVendor, id->idProduct);

    xbox_dev = kzalloc(sizeof(*xbox_dev), GFP_KERNEL);
    if (!xbox_dev)
        return -ENOMEM;

    xbox_dev->udev = usb_get_dev(udev);
    xbox_dev->interface = interface;
    //associate driver-specific data with an interface
    usb_set_intfdata(interface, xbox_dev);

    /* Input device */
    xbox_dev->input = input_allocate_device();
    if (!xbox_dev->input) {
        retval = -ENOMEM;
        goto fail;
    }

    xbox_dev->input->name = "Xbox Series S|X Controller";
    xbox_dev->input->id.bustype = BUS_USB;
    xbox_dev->input->id.vendor  = id->idVendor;
    xbox_dev->input->id.product = id->idProduct;

    __set_bit(EV_KEY, xbox_dev->input->evbit);
    __set_bit(EV_ABS, xbox_dev->input->evbit);

    /* Buttons */
    __set_bit(BTN_A, xbox_dev->input->keybit);
    __set_bit(BTN_B, xbox_dev->input->keybit);
    __set_bit(BTN_Y, xbox_dev->input->keybit);
    __set_bit(BTN_X, xbox_dev->input->keybit);

    __set_bit(BTN_TL, xbox_dev->input->keybit); //bumpers
    __set_bit(BTN_TR, xbox_dev->input->keybit);
    __set_bit(BTN_SELECT, xbox_dev->input->keybit);
    __set_bit(BTN_START, xbox_dev->input->keybit);

    __set_bit(BTN_THUMBL, xbox_dev->input->keybit); //stick presses
    __set_bit(BTN_THUMBR, xbox_dev->input->keybit);

    __set_bit(BTN_DPAD_UP, xbox_dev->input->keybit);
    __set_bit(BTN_DPAD_DOWN, xbox_dev->input->keybit);
    __set_bit(BTN_DPAD_LEFT, xbox_dev->input->keybit);
    __set_bit(BTN_DPAD_RIGHT, xbox_dev->input->keybit);

    __set_bit(BTN_MODE, xbox_dev->input->keybit); //xbox home button

    input_set_abs_params(xbox_dev->input, ABS_X,  -32768, 32767, 0, 0);
    input_set_abs_params(xbox_dev->input, ABS_Y,  -32768, 32767, 0, 0);
    input_set_abs_params(xbox_dev->input, ABS_RX, -32768, 32767, 0, 0);
    input_set_abs_params(xbox_dev->input, ABS_RY, -32768, 32767, 0, 0);

    // both analog and digital support for triggers
    input_set_abs_params(xbox_dev->input, ABS_Z,   0, 1023, 0, 0);
    input_set_abs_params(xbox_dev->input, ABS_RZ,  0, 1023, 0, 0);
    __set_bit(BTN_TL2, xbox_dev->input->keybit);
    __set_bit(BTN_TR2, xbox_dev->input->keybit);


    retval = input_register_device(xbox_dev->input);
    if (retval)
        goto fail_input;

    /* Fully initialize controller */
    retval = xbox_initialize_controller(xbox_dev);
    if (retval)
        goto fail_input;

    // interrupt handler - entire setup
    pipe = usb_rcvintpipe(udev, 0x82); //tworzenie potoku IN
    maxp = usb_maxpacket(udev, pipe, usb_pipein(pipe)); //maks wielkosc pakietu (info od urz USB)
    xbox_dev->irq_in_buffer = kmalloc(maxp, GFP_KERNEL); //alokacja pamieci kernela dla buforu
    if (!xbox_dev->irq_in_buffer) {
        retval = -ENOMEM;
        goto fail_input;
    }

    xbox_dev->irq_in_urb = usb_alloc_urb(0, GFP_KERNEL);
    if (!xbox_dev->irq_in_urb) {
        retval = -ENOMEM;
        goto fail_buffer;
    }

    //urb setup for periodic polling
    usb_fill_int_urb(xbox_dev->irq_in_urb, udev, pipe,
                     xbox_dev->irq_in_buffer, maxp,
                     xbox_irq_in, xbox_dev, 16);

    retval = usb_submit_urb(xbox_dev->irq_in_urb, GFP_KERNEL);
    if (retval) {
        pr_err("my_driver: failed to submit URB, err=%d\n", retval);
        goto fail_urb;
    }

    pr_info("my_driver: controller ready\n");
    return 0;

fail_urb:
    usb_free_urb(xbox_dev->irq_in_urb);
fail_buffer:
    kfree(xbox_dev->irq_in_buffer);
fail_input:
    input_unregister_device(xbox_dev->input);
fail:
    usb_put_dev(xbox_dev->udev);
    kfree(xbox_dev);
    return retval;
}

/* Disconnect */
static void xbox_disconnect(struct usb_interface *interface)
{
    struct xbox *xbox_dev = usb_get_intfdata(interface);

    usb_kill_urb(xbox_dev->irq_in_urb);
    usb_free_urb(xbox_dev->irq_in_urb);
    kfree(xbox_dev->irq_in_buffer);

    input_unregister_device(xbox_dev->input);
    usb_put_dev(xbox_dev->udev);
    kfree(xbox_dev);

    pr_info("my_driver: device disconnected\n");
}

/* USB driver struct */
static struct usb_driver xbox_driver = {
    .name = "my_driver",
    .id_table = xbox_table,
    .probe = xbox_probe,
    .disconnect = xbox_disconnect,
};

module_usb_driver(xbox_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jakub Dabrowski");
MODULE_DESCRIPTION("Driver for Xbox Series SX controller");
