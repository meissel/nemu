#ifndef NM_FORM_H_
#define NM_FORM_H_

#include <nm_string.h>
#include <nm_vector.h>
#include <nm_ncurses.h>
#include <nm_usb_devices.h>

#include <form.h>
#include <pthread.h>

typedef FORM nm_form_t;
typedef FIELD nm_field_t;

typedef struct {
    nm_str_t    driver;
    nm_str_t    size;
} nm_vm_drive_t;

#define NM_INIT_VM_DRIVE (nm_vm_drive_t) { NM_INIT_STR, NM_INIT_STR }

typedef struct {
    size_t          form_len;
    size_t          w_start_x;
    size_t          w_cols;
    size_t          w_rows;
    nm_window_t *   form_window;
} nm_form_data_t;

#define NM_INIT_FORM_DATA (nm_form_data_t) { 0, 0, 0, 0, NULL }

typedef struct {
    nm_str_t    driver;
    uint32_t    count;
} nm_vm_ifs_t;

#define NM_INIT_VM_IFS (nm_vm_ifs_t) { NM_INIT_STR, 0 }

typedef struct {
    nm_str_t        name;
    nm_usb_dev_t *  device;
} nm_vm_usb_t;

#define NM_INIT_VM_USB (nm_vm_usb_t) { NM_INIT_STR, NULL, 0 }

typedef struct {
    uint32_t    enable : 1;
    uint32_t    hostcpu_enable : 1;
} nm_vm_kvm_t;

#define NM_INIT_VM_KVM (nm_vm_kvm_t) { 0, 0 }

typedef struct {
    nm_str_t    inst_path;
    nm_str_t    mach;
    nm_str_t    bios;
    nm_str_t    kernel;
    nm_str_t    cmdline;
    nm_str_t    initrd;
    nm_str_t    tty;
    nm_str_t    socket;
    nm_str_t    debug_port;
    uint32_t    installed : 1;
    uint32_t    debug_freeze : 1;
} nm_vm_boot_t;

#define NM_INIT_VM_BOOT (nm_vm_boot_t) { \
        NM_INIT_STR, NM_INIT_STR, NM_INIT_STR, \
        NM_INIT_STR, NM_INIT_STR, NM_INIT_STR, \
        NM_INIT_STR, NM_INIT_STR, NM_INIT_STR, \
        0, 0 }

typedef struct {
    nm_str_t        name;
    nm_str_t        arch;
    nm_str_t        cpus;
    nm_str_t        memo;
    nm_str_t        srcp;
    nm_str_t        vncp;
    nm_vm_drive_t   drive;
    nm_vm_ifs_t     ifs;
    nm_vm_kvm_t     kvm;
    uint32_t        mouse_sync : 1;
    uint32_t        usb_enable : 1;
    uint32_t        usb_xhci : 1;
    uint32_t        spice : 1;
} nm_vm_t;

#define NM_INIT_VM (nm_vm_t) { \
        NM_INIT_STR, NM_INIT_STR, NM_INIT_STR, \
        NM_INIT_STR, NM_INIT_STR, NM_INIT_STR, \
        NM_INIT_VM_DRIVE, NM_INIT_VM_IFS, \
        NM_INIT_VM_KVM, 0, 0, 0, 0 }

typedef struct {
    const int * stop;
    const void *ctx;
} nm_spinner_data_t;

#define NM_INIT_SPINNER (nm_spinner_data_t) { NULL, NULL }

nm_form_t *nm_post_form(nm_window_t *w, nm_field_t **field, int begin_x,
                        int color);
int nm_draw_form(nm_window_t *w, nm_form_t *form);
void nm_form_free(nm_form_t *form, nm_field_t **fields);
void nm_get_field_buf(nm_field_t *f, nm_str_t *res);
int nm_form_name_used(const nm_str_t *name);
void nm_form_get_last(uint64_t *mac, uint32_t *vnc);
void nm_form_update_last_mac(uint64_t mac);
void nm_form_update_last_vnc(uint32_t vnc);
int nm_print_empty_fields(const nm_vect_t *v);
void nm_vm_free(nm_vm_t *vm);
void nm_vm_free_boot(nm_vm_boot_t *vm);
void *nm_progress_bar(void *data);
void *nm_file_progress(void *data);
int nm_form_calc_size(size_t max_msg, size_t f_num, nm_form_data_t *form);

extern const char *nm_form_yes_no[];
extern const char *nm_form_net_drv[];
extern const char *nm_form_drive_drv[];
extern const char *nm_form_macvtap[];
extern const char *nm_form_usbtype[];
extern const char *nm_form_svg_layer[];

#define NM_FORM_RATIO  0.80

#define NM_FORM_RESET()                                       \
    do {                                                      \
        curs_set(0);                                          \
        wtimeout(action_window, -1);                          \
    } while (0)

#define NM_FORM_EXIT()                                        \
    do {                                                      \
        wtimeout(action_window, -1);                          \
        delwin(form_data.form_window);                        \
        werase(help_window);                                  \
        nm_init_help_main();                                  \
    } while (0)

#define nm_form_check_data(name, val, v)                      \
    {                                                         \
        if (val.len == 0)                                     \
        nm_vect_insert(&v, name, strlen(name) + 1, NULL); \
    }

#define nm_form_check_datap(name, val, v)                     \
    {                                                         \
        if (val->len == 0)                                    \
        nm_vect_insert(&v, name, strlen(name) + 1, NULL); \
    }

#endif /* NM_FORM_H_ */
/* vim:set ts=4 sw=4: */
