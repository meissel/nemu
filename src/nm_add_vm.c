#include <nm_core.h>
#include <nm_form.h>
#include <nm_utils.h>
#include <nm_string.h>
#include <nm_window.h>
#include <nm_add_drive.h>
#include <nm_add_vm.h>
#include <nm_hw_info.h>
#include <nm_network.h>
#include <nm_database.h>
#include <nm_cfg_file.h>
#include <nm_ovf_import.h>

static const char NM_VM_FORM_NAME[] = "Name";
static const char NM_VM_FORM_ARCH[] = "Architecture";
static const char NM_VM_FORM_CPU_BEGIN[] = "CPU cores [1-";
static const char NM_VM_FORM_CPU_END[] = "]";
static const char NM_VM_FORM_MEM_BEGIN[] = "Memory [4-";
static const char NM_VM_FORM_MEM_END[] = "]Mb";
static const char NM_VM_FORM_DRV_BEGIN[] = "Disk [1-";
static const char NM_VM_FORM_DRV_END[] = "]Gb";
static const char NM_VM_FORM_DRV_IF[] = "Disk interface";
static const char NM_VM_FORM_IMP_PATH[] = "Path to disk image";
static const char NM_VM_FORM_INS_PATH[] = "Path to ISO/IMG";
static const char NM_VM_FORM_NET_IFS[] = "Network interfaces";
static const char NM_VM_FORM_NET_DRV[] = "Net driver";

static void nm_add_vm_field_setup(int import);
static void nm_add_vm_field_names(nm_vect_t *msg, int import);
static int nm_add_vm_get_data(nm_vm_t *vm, int import);
static void nm_add_vm_to_fs(nm_vm_t *vm, int import);
static void nm_add_vm_main(int import);

enum {
    NM_FLD_VMNAME = 0,
    NM_FLD_VMARCH,
    NM_FLD_CPUNUM,
    NM_FLD_RAMTOT,
    NM_FLD_DISKSZ,
    NM_FLD_DISKIN,
    NM_FLD_SOURCE,
    NM_FLD_IFSCNT,
    NM_FLD_IFSDRV,
    NM_FLD_COUNT
};

static nm_form_t *form = NULL;
static nm_field_t *fields[NM_FLD_COUNT + 1];

void nm_import_vm(void)
{
    nm_add_vm_main(NM_IMPORT_VM);
}

void nm_add_vm(void)
{
    nm_add_vm_main(NM_INSTALL_VM);
}

static void nm_add_vm_main(int import)
{
    nm_vm_t vm = NM_INIT_VM;
    nm_vect_t msg_fields = NM_INIT_VECT;
    nm_spinner_data_t sp_data = NM_INIT_SPINNER;
    nm_form_data_t form_data = NM_INIT_FORM_DATA;
    uint64_t last_mac;
    uint32_t last_vnc;
    size_t msg_len;
    pthread_t spin_th = pthread_self();
    int done = 0;

    nm_add_vm_field_names(&msg_fields, import);
    msg_len = nm_max_msg_len((const char **)msg_fields.data);

    if (nm_form_calc_size(msg_len, NM_FLD_COUNT, &form_data) != NM_OK)
        return;

    werase(action_window);
    werase(help_window);
    if (import == NM_INSTALL_VM) {
        nm_init_action(_(NM_MSG_INSTALL));
        nm_init_help_install();
    } else {
        nm_init_action(_(NM_MSG_IMPORT));
        nm_init_help_import();
    }

    for (size_t n = 0; n < NM_FLD_COUNT; ++n)
        fields[n] = new_field(1, form_data.form_len, n * 2, 0, 0, 0);

    fields[NM_FLD_COUNT] = NULL;

    nm_add_vm_field_setup(import);
    for (size_t n = 0, y = 1, x = 2; n < NM_FLD_COUNT; n++) {
        mvwaddstr(form_data.form_window, y, x, msg_fields.data[n]);
        y += 2;
    }

    form = nm_post_form(form_data.form_window, fields, msg_len + 4, NM_TRUE);
    if (nm_draw_form(action_window, form) != NM_OK)
        goto out;

    nm_form_get_last(&last_mac, &last_vnc);
    nm_str_format(&vm.vncp, "%u", last_vnc);

    if (nm_add_vm_get_data(&vm, import) != NM_OK)
        goto out;

    if (import) {
        sp_data.stop = &done;
        sp_data.ctx = &vm;

        if (pthread_create(&spin_th, NULL, nm_file_progress,
                           (void *)&sp_data) != 0)
            nm_bug(_("%s: cannot create thread"), __func__);
    }

    nm_add_vm_to_fs(&vm, import);
    nm_add_vm_to_db(&vm, last_mac, import, NULL);

    if (import) {
        done = 1;
        if (pthread_join(spin_th, NULL) != 0)
            nm_bug(_("%s: cannot join thread"), __func__);
    }

out:
    NM_FORM_EXIT();
    nm_vect_free(&msg_fields, NULL);
    nm_vm_free(&vm);
    nm_form_free(form, fields);
    delwin(form_data.form_window);
}

static void nm_add_vm_field_setup(int import)
{
    set_field_type(fields[NM_FLD_VMNAME], TYPE_REGEXP,
                   "^[a-zA-Z0-9_-]{1,30} *$");
    set_field_type(fields[NM_FLD_VMARCH], TYPE_ENUM,
                   nm_cfg_get_arch(), false, false);
    set_field_type(fields[NM_FLD_CPUNUM], TYPE_INTEGER, 0, 1, nm_hw_ncpus());
    set_field_type(fields[NM_FLD_RAMTOT], TYPE_INTEGER, 0, 4,
                   nm_hw_total_ram());
    set_field_type(fields[NM_FLD_DISKSZ], TYPE_INTEGER, 0, 1,
                   nm_hw_disk_free());
    set_field_type(fields[NM_FLD_DISKIN], TYPE_ENUM, nm_form_drive_drv, false,
                   false);
    set_field_type(fields[NM_FLD_SOURCE], TYPE_REGEXP, "^/.*");
    set_field_type(fields[NM_FLD_IFSCNT], TYPE_INTEGER, 1, 0, 64);
    set_field_type(fields[NM_FLD_IFSDRV], TYPE_ENUM, nm_form_net_drv, false,
                   false);

    if (import)
        field_opts_off(fields[NM_FLD_DISKSZ], O_ACTIVE);

    set_field_buffer(fields[NM_FLD_VMARCH], 0,
                     *nm_cfg_get()->qemu_targets.data);
    set_field_buffer(fields[NM_FLD_CPUNUM], 0, "1");
    set_field_buffer(fields[NM_FLD_DISKIN], 0, NM_DEFAULT_DRVINT);
    set_field_buffer(fields[NM_FLD_IFSCNT], 0, "1");
    if (import)
        set_field_buffer(fields[NM_FLD_DISKSZ], 0, "unused");
    set_field_buffer(fields[NM_FLD_IFSDRV], 0, NM_DEFAULT_NETDRV);
    field_opts_off(fields[NM_FLD_VMNAME], O_STATIC);
    field_opts_off(fields[NM_FLD_SOURCE], O_STATIC);
}

static void nm_add_vm_field_names(nm_vect_t *msg, int import)
{
    nm_str_t buf = NM_INIT_STR;

    nm_vect_insert(msg, _(NM_VM_FORM_NAME), strlen(_(NM_VM_FORM_NAME)) + 1,
                   NULL);
    nm_vect_insert(msg, _(NM_VM_FORM_ARCH), strlen(_(NM_VM_FORM_ARCH)) + 1,
                   NULL);

    nm_str_format(&buf, "%s%u%s",
                  _(NM_VM_FORM_CPU_BEGIN), nm_hw_ncpus(),
                  _(NM_VM_FORM_CPU_END));
    nm_vect_insert(msg, buf.data, buf.len + 1, NULL);

    nm_str_format(&buf, "%s%u%s",
                  _(NM_VM_FORM_MEM_BEGIN), nm_hw_total_ram(),
                  _(NM_VM_FORM_MEM_END));
    nm_vect_insert(msg, buf.data, buf.len + 1, NULL);

    nm_str_format(&buf, "%s%u%s",
                  _(NM_VM_FORM_DRV_BEGIN), nm_hw_disk_free(),
                  _(NM_VM_FORM_DRV_END));
    nm_vect_insert(msg, buf.data, buf.len + 1, NULL);

    nm_vect_insert(msg, _(NM_VM_FORM_DRV_IF), strlen(_(
                                                         NM_VM_FORM_DRV_IF)) + 1,
                   NULL);
    if (import)
        nm_vect_insert(msg, _(NM_VM_FORM_IMP_PATH),
                       strlen(_(NM_VM_FORM_IMP_PATH)) + 1, NULL);
    else
        nm_vect_insert(msg, _(NM_VM_FORM_INS_PATH),
                       strlen(_(NM_VM_FORM_INS_PATH)) + 1, NULL);

    nm_vect_insert(msg, _(NM_VM_FORM_NET_IFS), strlen(_(
                                                          NM_VM_FORM_NET_IFS)) + 1,
                   NULL);
    nm_vect_insert(msg, _(NM_VM_FORM_NET_DRV), strlen(_(
                                                          NM_VM_FORM_NET_DRV)) + 1,
                   NULL);
    nm_vect_end_zero(msg);

    nm_str_free(&buf);
}

static int nm_add_vm_get_data(nm_vm_t *vm, int import)
{
    int rc = NM_OK;
    nm_str_t ifs_buf = NM_INIT_STR;
    nm_vect_t err = NM_INIT_VECT;

    nm_get_field_buf(fields[NM_FLD_VMNAME], &vm->name);
    nm_get_field_buf(fields[NM_FLD_VMARCH], &vm->arch);
    nm_get_field_buf(fields[NM_FLD_CPUNUM], &vm->cpus);
    nm_get_field_buf(fields[NM_FLD_RAMTOT], &vm->memo);
    nm_get_field_buf(fields[NM_FLD_SOURCE], &vm->srcp);
    if (!import)
        nm_get_field_buf(fields[NM_FLD_DISKSZ], &vm->drive.size);
    nm_get_field_buf(fields[NM_FLD_DISKIN], &vm->drive.driver);
    nm_get_field_buf(fields[NM_FLD_IFSCNT], &ifs_buf);
    nm_get_field_buf(fields[NM_FLD_IFSDRV], &vm->ifs.driver);

    nm_form_check_data(_("Name"), vm->name, err);
    nm_form_check_data(_("Architecture"), vm->arch, err);
    nm_form_check_data(_("CPU cores"), vm->cpus, err);
    nm_form_check_data(_("Memory"), vm->memo, err);
    if (!import)
        nm_form_check_data(_("Disk"), vm->drive.size, err);
    nm_form_check_data(_("Disk interface"), vm->drive.driver, err);
    nm_form_check_data(_("Path to ISO/IMG"), vm->srcp, err);
    nm_form_check_data(_("Network interfaces"), vm->ifs.driver, err);
    nm_form_check_data(_("Net driver"), vm->ifs.driver, err);

    if ((rc = nm_print_empty_fields(&err)) == NM_ERR)
        goto out;

    /* Check for free space for importing drive image */
    if (import) {
        off_t size_gb = 0;
        struct stat img_info;
        memset(&img_info, 0, sizeof(img_info));

        if (stat(vm->srcp.data, &img_info) == 0) {
            size_gb = img_info.st_size / 1024 / 1024 / 1024;
            nm_str_format(&vm->drive.size, "%ld", size_gb);
        } else {
            nm_bug(_("%s: cannot get stat of file: %s"), __func__,
                   vm->srcp.data);
        }

        if (size_gb >= nm_hw_disk_free()) {
            curs_set(0);
            nm_warn(_(NM_MSG_NO_SPACE));
            goto out;
        }
    }

    vm->ifs.count = nm_str_stoui(&ifs_buf, 10);
#if defined (NM_OS_LINUX)
    vm->usb_enable = 1; /* enable USB by default */
#endif

    rc = nm_form_name_used(&vm->name);

out:
    nm_str_free(&ifs_buf);
    nm_vect_free(&err, NULL);

    return rc;
}

void nm_add_vm_to_db(nm_vm_t *vm, uint64_t mac,
                     int import, const nm_vect_t *drives)
{
    nm_str_t query = NM_INIT_STR;
    int altname = 0;

    /* insert main VM data */
    nm_str_format(&query,
                  "INSERT INTO vms(name, mem, smp, kvm, hcpu, vnc, arch, iso, install, "
                  "mouse_override, usb, usb_type, fs9p_enable, spice, debug_port, debug_freeze) "
                  "VALUES('%s', '%s', '%s', '%s', '%s', '%s', '%s', '%s', '%s', '%s', '%s', '%s', '%s', '%s', '%s', '%s')",
                  vm->name.data, vm->memo.data, vm->cpus.data,
#if (NM_OS_LINUX)
                  NM_ENABLE, NM_ENABLE,     /* enable KVM and host CPU by default */
#else
                  NM_DISABLE, NM_DISABLE,   /* disable KVM on non Linux platform */
#endif
                  vm->vncp.data, vm->arch.data,
                  import ? "" : vm->srcp.data,
                  import ? NM_DISABLE : NM_ENABLE,                          /* if imported, then no need to install */
                  NM_DISABLE,                                               /* mouse override */
                  vm->usb_enable ? NM_ENABLE : NM_DISABLE,                  /* USB enabled */
                  NM_DEFAULT_USBVER,                                        /* set USB 3.0 by default */
                  NM_DISABLE,                                               /* disable 9pfs by default */
                  (nm_cfg_get()->spice_default) ? NM_ENABLE : NM_DISABLE,   /* SPICE enabled */
                  "",                                                       /* disable GDB debug by default */
                  NM_DISABLE);

    nm_db_edit(query.data);

    /* insert drive info */
    if (drives == NULL) {
        nm_str_format(&query,
                      "INSERT INTO drives(vm_name, drive_name, drive_drv, capacity, boot) "
                      "VALUES('%s', '%s_a.img', '%s', '%s', '%s')",
                      vm->name.data, vm->name.data, vm->drive.driver.data,
                      vm->drive.size.data,
                      NM_ENABLE /* boot flag */
                      );
        nm_db_edit(query.data);
    } else { /* imported from OVF */
        for (size_t n = 0; n < drives->n_memb; n++) {
            nm_str_format(&query,
                          "INSERT INTO drives(vm_name, drive_name, drive_drv, capacity, boot) "
                          "VALUES('%s', '%s', '%s', '%s', '%s')",
                          vm->name.data,
                          nm_drive_file(
                              drives->data[n])->data, NM_DEFAULT_DRVINT,
                          nm_drive_size(drives->data[n])->data,
                          n == 0 ? NM_ENABLE : NM_DISABLE /* boot flag */
                          );
            nm_db_edit(query.data);
        }
    }

    /* insert network interface info */
    for (size_t n = 0; n < vm->ifs.count; n++) {
        nm_str_t if_name = NM_INIT_STR;
        nm_str_t if_name_copy = NM_INIT_STR;
        nm_str_t maddr = NM_INIT_STR;
        mac++;

        nm_net_mac_n2a(mac, &maddr);
        nm_str_format(&if_name, "%s_eth%zu", vm->name.data, n);
        nm_str_copy(&if_name_copy, &if_name);
        altname = nm_net_fix_tap_name(&if_name, &maddr);

        nm_str_format(&query,
                      "INSERT INTO ifaces(vm_name, if_name, mac_addr, if_drv, vhost, macvtap, altname) "
                      "VALUES('%s', '%s', '%s', '%s', '%s', '%s', '%s')",
                      vm->name.data, if_name.data, maddr.data,
                      vm->ifs.driver.data,
#if defined (NM_OS_LINUX)
                      nm_str_cmp_st(&vm->ifs.driver,
                                    NM_DEFAULT_NETDRV) == NM_OK ?
                      "1" : "0", /* Enable vhost by default for virtio-net-pci device on Linux */
#else
                      "0",
#endif
                      "0", /* disable macvtap by default */
                      (altname) ? if_name_copy.data : ""
                      );
        nm_db_edit(query.data);

        nm_str_free(&if_name);
        nm_str_free(&if_name_copy);
        nm_str_free(&maddr);
    }

    nm_form_update_last_mac(mac);
    nm_form_update_last_vnc(nm_str_stoui(&vm->vncp, 10));

    nm_str_free(&query);
}

static void nm_add_vm_to_fs(nm_vm_t *vm, int import)
{
    nm_str_t vm_dir = NM_INIT_STR;
    nm_str_t buf = NM_INIT_STR;

    nm_str_format(&vm_dir, "%s/%s", nm_cfg_get()->vm_dir.data, vm->name.data);

    if (mkdir(vm_dir.data, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH) != 0)
        nm_bug(_("%s: cannot create VM directory %s: %s"),
               __func__, vm_dir.data, strerror(errno));

    if (!import) {
        if (nm_add_drive_to_fs(&vm->name, &vm->drive.size, NULL) != NM_OK)
            nm_bug(_("%s: cannot create image file"), __func__);
    } else {
        nm_str_format(&buf, "%s/%s_a.img", vm_dir.data, vm->name.data);
        nm_copy_file(&vm->srcp, &buf);
    }

    nm_str_free(&vm_dir);
    nm_str_free(&buf);
}

/* vim:set ts=4 sw=4: */
