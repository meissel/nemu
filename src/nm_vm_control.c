#include <nm_core.h>
#include <nm_utils.h>
#include <nm_string.h>
#include <nm_window.h>
#include <nm_network.h>
#include <nm_database.h>
#include <nm_cfg_file.h>
#include <nm_vm_control.h>
#include <nm_usb_devices.h>

#include <time.h>

enum {
    NM_VIEWER_SPICE,
    NM_VIEWER_VNC
};

#if defined(NM_WITH_VNC_CLIENT) || defined(NM_WITH_SPICE)
static void nm_vmctl_gen_viewer(const nm_str_t *name, uint32_t port,
                                nm_str_t *cmd, int type);
#endif
static int nm_vmctl_clear_tap_vect(const nm_vect_t *vms);

void nm_vmctl_get_data(const nm_str_t *name, nm_vmctl_data_t *vm)
{
    nm_str_t query = NM_INIT_STR;

    nm_str_format(&query, NM_VM_GET_LIST_SQL, name->data);
    nm_db_select(query.data, &vm->main);

    nm_str_format(&query, NM_VM_GET_IFACES_SQL, name->data);
    nm_db_select(query.data, &vm->ifs);

    nm_str_format(&query, NM_VM_GET_DRIVES_SQL, name->data);
    nm_db_select(query.data, &vm->drives);

    nm_str_format(&query, NM_USB_GET_SQL, name->data);
    nm_db_select(query.data, &vm->usb);

    nm_str_free(&query);
}

void nm_vmctl_start(const nm_str_t *name, int flags)
{
    nm_str_t buf = NM_INIT_STR;
    nm_vect_t argv = NM_INIT_VECT;
    nm_vmctl_data_t vm = NM_VMCTL_INIT_DATA;
    nm_vect_t tfds = NM_INIT_VECT;

    nm_vmctl_get_data(name, &vm);

    /* check if VM is already installed */
    if (nm_str_cmp_st(nm_vect_str(&vm.main, NM_SQL_INST), NM_ENABLE) == NM_OK) {
        int ch = nm_notify(_(NM_MSG_INST_CONF));
        if (ch == 'y') {
            flags &= ~NM_VMCTL_TEMP;
            nm_str_t query = NM_INIT_STR;

            nm_str_trunc(nm_vect_str(&vm.main, NM_SQL_INST), 0);
            nm_str_add_text(nm_vect_str(&vm.main, NM_SQL_INST), NM_DISABLE);

            nm_str_alloc_text(&query,
                              "UPDATE vms SET install='0' WHERE name='");
            nm_str_add_str(&query, name);
            nm_str_add_char(&query, '\'');
            nm_db_edit(query.data);

            nm_str_free(&query);
        }
    }

    nm_vmctl_gen_cmd(&argv, &vm, name, flags, &tfds);
    if (argv.n_memb > 0) {
        if (nm_spawn_process(&argv, NULL) != NM_OK) {
            nm_str_t qmp_path = NM_INIT_STR;
            struct stat qmp_info;

            nm_str_format(&qmp_path, "%s/%s/%s",
                          nm_cfg_get()->vm_dir.data, name->data,
                          NM_VM_QMP_FILE);

            /* must delete qmp sock file if exists */
            if (stat(qmp_path.data, &qmp_info) != -1)
                unlink(qmp_path.data);

            nm_str_free(&qmp_path);

            nm_warn(_(NM_MSG_START_ERR));
        } else {
            nm_cmd_str(&buf, &argv);
            nm_debug("cmd=%s\n", buf.data);
            nm_vmctl_log_last(&buf);

            /* close all tap file descriptors */
            for (size_t n = 0; n < tfds.n_memb; n++)
                close(*((int *)tfds.data[n]));
        }
    }

    nm_str_free(&buf);
    nm_vect_free(&argv, NULL);
    nm_vect_free(&tfds, NULL);
    nm_vmctl_free_data(&vm);
}

void nm_vmctl_delete(const nm_str_t *name)
{
    nm_str_t vmdir = NM_INIT_STR;
    nm_str_t query = NM_INIT_STR;
    nm_vect_t drives = NM_INIT_VECT;
    nm_vect_t snaps = NM_INIT_VECT;
    int delete_ok = NM_TRUE;

    nm_str_format(&vmdir, "%s/%s/", nm_cfg_get()->vm_dir.data, name->data);

    nm_str_format(&query, NM_SELECT_DRIVE_NAMES_SQL, name->data);
    nm_db_select(query.data, &drives);

    for (size_t n = 0; n < drives.n_memb; n++) {
        nm_str_t img_path = NM_INIT_STR;
        nm_str_format(&img_path, "%s%s", vmdir.data, nm_vect_str(&drives,
                                                                 n)->data);

        if (unlink(img_path.data) == -1)
            delete_ok = NM_FALSE;

        nm_str_free(&img_path);
    }

    { /* delete pid and QMP socket if exists */
        nm_str_t path = NM_INIT_STR;

        nm_str_format(&path, "%s%s", vmdir.data, NM_VM_PID_FILE);

        if (unlink(path.data) == -1 && errno != ENOENT)
            delete_ok = NM_FALSE;

        nm_str_trunc(&path, vmdir.len);
        nm_str_add_text(&path, NM_VM_QMP_FILE);
        if (unlink(path.data) == -1 && errno != ENOENT)
            delete_ok = NM_FALSE;

        nm_str_free(&path);
    }

    if (delete_ok)
        if (rmdir(vmdir.data) == -1)
            delete_ok = NM_FALSE;

    nm_vmctl_clear_tap(name);

    nm_str_format(&query, NM_DEL_DRIVES_SQL, name->data);
    nm_db_edit(query.data);

    nm_str_format(&query, NM_DEL_VMSNAP_SQL, name->data);
    nm_db_edit(query.data);

    nm_str_format(&query, NM_DEL_IFS_SQL, name->data);
    nm_db_edit(query.data);

    nm_str_format(&query, NM_DEL_USB_SQL, name->data);
    nm_db_edit(query.data);

    nm_str_format(&query, NM_DEL_VM_SQL, name->data);
    nm_db_edit(query.data);

    if (!delete_ok)
        nm_warn(_(NM_MSG_INC_DEL));

    nm_str_free(&vmdir);
    nm_str_free(&query);
    nm_vect_free(&drives, nm_str_vect_free_cb);
    nm_vect_free(&snaps, nm_str_vect_free_cb);
}

void nm_vmctl_kill(const nm_str_t *name)
{
    pid_t pid;
    int fd;
    char buf[10];
    nm_str_t pid_file = NM_INIT_STR;

    nm_str_format(&pid_file, "%s/%s/%s",
                  nm_cfg_get()->vm_dir.data, name->data, NM_VM_PID_FILE);

    if ((fd = open(pid_file.data, O_RDONLY)) == -1)
        return;

    if (read(fd, buf, sizeof(buf)) <= 0) {
        close(fd);
        return;
    }

    pid = atoi(buf);
    kill(pid, SIGTERM);

    close(fd);

    nm_str_free(&pid_file);
}

#if defined(NM_WITH_VNC_CLIENT) || defined(NM_WITH_SPICE)
void nm_vmctl_connect(const nm_str_t *name)
{
    nm_str_t cmd = NM_INIT_STR;
    nm_str_t query = NM_INIT_STR;
    nm_vect_t vm = NM_INIT_VECT;
    uint32_t port;
    int unused __attribute__((unused));

    nm_str_format(&query, NM_VMCTL_GET_VNC_PORT_SQL, name->data);
    nm_db_select(query.data, &vm);
    port = nm_str_stoui(nm_vect_str(&vm, 0), 10) + 5900;
# if defined(NM_WITH_SPICE)
    if (nm_str_cmp_st(nm_vect_str(&vm, 1), NM_ENABLE) == NM_OK) {
        nm_vmctl_gen_viewer(name, port, &cmd, NM_VIEWER_SPICE);
    } else {
# endif
    nm_vmctl_gen_viewer(name, port, &cmd, NM_VIEWER_VNC);
# if defined(NM_WITH_SPICE)
}
# endif
    unused = system(cmd.data);

    nm_vect_free(&vm, nm_str_vect_free_cb);
    nm_str_free(&query);
    nm_str_free(&cmd);
}
#endif

void nm_vmctl_gen_cmd(nm_vect_t *argv, const nm_vmctl_data_t *vm,
                      const nm_str_t *name, int flags, nm_vect_t *tfds)
{
    nm_str_t vmdir = NM_INIT_STR;
    const nm_cfg_t *cfg = nm_cfg_get();
    size_t drives_count = vm->drives.n_memb / NM_DRV_IDX_COUNT;
    size_t ifs_count = vm->ifs.n_memb / NM_IFS_IDX_COUNT;
    int scsi_added = NM_FALSE;
    nm_str_t buf = NM_INIT_STR;

    nm_str_format(&vmdir, "%s/%s/", cfg->vm_dir.data, name->data);

    nm_str_format(&buf, "%s%s",
                  NM_STRING(NM_USR_PREFIX) "/bin/qemu-system-",
                  nm_vect_str(&vm->main, NM_SQL_ARCH)->data);
    nm_vect_insert(argv, buf.data, buf.len + 1, NULL);

    nm_vect_insert_cstr(argv, "-daemonize");

    /* setup install source */
    if (nm_str_cmp_st(nm_vect_str(&vm->main, NM_SQL_INST),
                      NM_ENABLE) == NM_OK) {
        const char *iso = nm_vect_str_ctx(&vm->main, NM_SQL_ISO);
        size_t srcp_len = strlen(iso);

        if ((srcp_len == 0) && (!(flags & NM_VMCTL_INFO))) {
            nm_warn(_(NM_MSG_ISO_MISS));
            nm_vect_free(argv, NULL);
            goto out;
        }
        if ((srcp_len > 4) &&
            (nm_str_cmp_tt(iso + (srcp_len - 4), ".iso") == NM_OK)) {
            nm_vect_insert_cstr(argv, "-boot");
            nm_vect_insert_cstr(argv, "d");
            nm_vect_insert_cstr(argv, "-cdrom");
            nm_vect_insert_cstr(argv, iso);
        } else {
            nm_vect_insert_cstr(argv, "-drive");

            nm_str_format(&buf, "file=%s,media=disk,if=ide", iso);
            nm_vect_insert(argv, buf.data, buf.len + 1, NULL);
        }
    } else { /* just mount cdrom */
        const char *iso = nm_vect_str_ctx(&vm->main, NM_SQL_ISO);
        size_t srcp_len = strlen(iso);
        struct stat info;
        int rc = -1;

        if (srcp_len > 0) {
            memset(&info, 0x0, sizeof(info));
            rc = stat(iso, &info);

            if ((rc == -1) && (!(flags & NM_VMCTL_INFO))) {
                nm_warn(_(NM_MSG_ISO_NF));
                nm_vect_free(argv, NULL);
                goto out;
            }
            if (rc != -1) {
                nm_vect_insert_cstr(argv, "-cdrom");
                nm_vect_insert_cstr(argv, iso);
            }
        }
    }

    for (size_t n = 0; n < drives_count; n++) {
        int scsi_drv = NM_FALSE;
        size_t idx_shift = NM_DRV_IDX_COUNT * n;
        const nm_str_t *drive_img = nm_vect_str(&vm->drives,
                                                NM_SQL_DRV_NAME + idx_shift);
        const nm_str_t *blk_drv = nm_vect_str(&vm->drives,
                                              NM_SQL_DRV_TYPE + idx_shift);

        if (nm_str_cmp_st(blk_drv, "scsi") == NM_OK) {
            scsi_drv = NM_TRUE;
            if (!scsi_added) {
                nm_vect_insert_cstr(argv, "-device");
                nm_vect_insert_cstr(argv, "virtio-scsi-pci,id=scsi");
                scsi_added = NM_TRUE;
            }
        }
        nm_vect_insert_cstr(argv, "-drive");

        nm_str_format(&buf, "id=hd%zu,media=disk,if=%s,file=%s%s",
                      n, (scsi_drv) ? "none" : blk_drv->data, vmdir.data,
                      drive_img->data);
        nm_vect_insert(argv, buf.data, buf.len + 1, NULL);

        if (scsi_drv) {
            nm_vect_insert_cstr(argv, "-device");
            nm_str_format(&buf, "scsi-hd,drive=hd%zu", n);
            nm_vect_insert(argv, buf.data, buf.len + 1, NULL);
        }
    }

#ifdef NM_SAVEVM_SNAPSHOTS
    /* load vm snapshot if exists */
    {
        nm_str_t query = NM_INIT_STR;
        nm_vect_t snap_res = NM_INIT_VECT;

        nm_str_format(&query, NM_GET_VMSNAP_LOAD_SQL, name->data);
        nm_db_select(query.data, &snap_res);

        if (snap_res.n_memb > 0) {
            nm_vect_insert_cstr(argv, "-loadvm");
            nm_vect_insert_cstr(argv, nm_vect_str_ctx(&snap_res, 0));

            /* reset load flag */
            if (!(flags & NM_VMCTL_INFO)) {
                nm_str_format(&query, NM_RESET_LOAD_SQL, name->data);
                nm_db_edit(query.data);
            }
        }

        nm_str_free(&query);
        nm_vect_free(&snap_res, nm_str_vect_free_cb);
    }
#endif /* NM_SAVEVM_SNAPSHOTS */

    nm_vect_insert_cstr(argv, "-m");
    nm_vect_insert(argv,
                   nm_vect_str(&vm->main, NM_SQL_MEM)->data,
                   nm_vect_str(&vm->main, NM_SQL_MEM)->len + 1, NULL);

    if (nm_str_stoui(nm_vect_str(&vm->main, NM_SQL_SMP), 10) > 1) {
        nm_vect_insert_cstr(argv, "-smp");
        nm_vect_insert(argv,
                       nm_vect_str(&vm->main, NM_SQL_SMP)->data,
                       nm_vect_str(&vm->main, NM_SQL_SMP)->len + 1, NULL);
    }

    /* 9p sharing.
     *
     * guest mount example:
     * mount -t 9p -o trans=virtio,version=9p2000.L hostshare /mnt/host
     */
    if (nm_str_cmp_st(nm_vect_str(&vm->main, NM_SQL_9FLG),
                      NM_ENABLE) == NM_OK) {
        nm_vect_insert_cstr(argv, "-fsdev");
        nm_str_format(&buf, "local,security_model=none,id=fsdev0,path=%s",
                      nm_vect_str(&vm->main, NM_SQL_9PTH)->data);
        nm_vect_insert(argv, buf.data, buf.len + 1, NULL);

        nm_vect_insert_cstr(argv, "-device");
        nm_str_format(&buf, "virtio-9p-pci,fsdev=fsdev0,mount_tag=%s",
                      nm_vect_str(&vm->main, NM_SQL_9ID)->data);
        nm_vect_insert(argv, buf.data, buf.len + 1, NULL);
    }

    if (nm_str_cmp_st(nm_vect_str(&vm->main, NM_SQL_KVM), NM_ENABLE) == NM_OK) {
        nm_vect_insert_cstr(argv, "-enable-kvm");
        if (nm_str_cmp_st(nm_vect_str(&vm->main, NM_SQL_HCPU),
                          NM_ENABLE) == NM_OK) {
            nm_vect_insert_cstr(argv, "-cpu");
            nm_vect_insert_cstr(argv, "host");
        }
    }

    /* Save info about usb subsystem status at boot time.
     * Needed for USB hotplug feature. */
    if (!(flags & NM_VMCTL_INFO)) {
        nm_str_t query = NM_INIT_STR;
        nm_str_format(&query,
                      NM_USB_UPDATE_STATE_SQL,
                      nm_vect_str_ctx(&vm->main, NM_SQL_USBF),
                      name->data);
        nm_db_edit(query.data);
        nm_str_free(&query);
    }

    if (nm_str_cmp_st(nm_vect_str(&vm->main, NM_SQL_USBF),
                      NM_ENABLE) == NM_OK) {
        size_t usb_count = vm->usb.n_memb / NM_USB_IDX_COUNT;
        nm_vect_t usb_list = NM_INIT_VECT;
        nm_vect_t serial_cache = NM_INIT_VECT;
        nm_str_t serial = NM_INIT_STR;

        nm_vect_insert_cstr(argv, "-usb");
        nm_vect_insert_cstr(argv, "-device");

        if (nm_str_cmp_st(nm_vect_str(&vm->main,
                                      NM_SQL_USBT), NM_DEFAULT_USBVER) == NM_OK)
            nm_vect_insert_cstr(argv, "qemu-xhci");
        else
            nm_vect_insert_cstr(argv, "usb-ehci");

        if (usb_count > 0)
            nm_usb_get_devs(&usb_list);

        for (size_t n = 0; n < usb_count; n++) {
            int found_in_cache = 0;
            int found_in_devs = 0;
            size_t idx_shift = NM_USB_IDX_COUNT * n;
            nm_usb_dev_t *usb = NULL;

            const char *vid = nm_vect_str_ctx(&vm->usb,
                                              NM_SQL_USB_VID + idx_shift);
            const char *pid = nm_vect_str_ctx(&vm->usb,
                                              NM_SQL_USB_PID + idx_shift);
            const char *ser = nm_vect_str_ctx(&vm->usb,
                                              NM_SQL_USB_SERIAL + idx_shift);

            /* look for cached data first, libusb_open() is very expensive */
            for (size_t m = 0; m < serial_cache.n_memb; m++) {
                usb = *nm_usb_data_dev(serial_cache.data[m]);
                nm_str_t *ser_str = nm_usb_data_serial(serial_cache.data[m]);

                if ((nm_str_cmp_st(nm_usb_vendor_id(usb), vid) == NM_OK) &&
                    (nm_str_cmp_st(nm_usb_product_id(usb), pid) == NM_OK) &&
                    (nm_str_cmp_st(ser_str, ser) == NM_OK)) {
                    found_in_cache = 1;
                    break;
                }
            }

            if (found_in_cache) {
                assert(usb != NULL);
                nm_vect_insert_cstr(argv, "-device");
                nm_str_format(&buf,
                              "usb-host,hostbus=%d,hostaddr=%d,id=usb-%s-%s-%s",
                              *nm_usb_bus_num(usb), *nm_usb_dev_addr(usb),
                              nm_usb_vendor_id(usb)->data, nm_usb_product_id(
                                  usb)->data, ser);
                nm_vect_insert(argv, buf.data, buf.len + 1, NULL);

                continue;
            }

            for (size_t m = 0; m < usb_list.n_memb; m++) {
                usb = (nm_usb_dev_t *)usb_list.data[m];
                if ((nm_str_cmp_st(nm_usb_vendor_id(usb), vid) == NM_OK) &&
                    (nm_str_cmp_st(nm_usb_product_id(usb), pid) == NM_OK)) {
                    if (serial.len)
                        nm_str_trunc(&serial, 0);

                    nm_usb_get_serial(usb, &serial);
                    if (nm_str_cmp_st(&serial, ser) == NM_OK) {
                        found_in_devs = 1;
                        break;
                    } else {
                        /* save result in cache */
                        nm_usb_data_t usb_data = NM_INIT_USB_DATA;
                        nm_str_copy(&usb_data.serial, &serial);
                        usb_data.dev = usb;
                        nm_vect_insert(&serial_cache, &usb_data,
                                       sizeof(usb_data),
                                       nm_usb_data_vect_ins_cb);
                        nm_str_free(&usb_data.serial);
                    }
                }
            }

            if (found_in_devs) {
                assert(usb != NULL);
                nm_vect_insert_cstr(argv, "-device");
                nm_str_format(&buf,
                              "usb-host,hostbus=%d,hostaddr=%d,id=usb-%s-%s-%s",
                              *nm_usb_bus_num(usb), *nm_usb_dev_addr(usb),
                              nm_usb_vendor_id(usb)->data, nm_usb_product_id(
                                  usb)->data, ser);
                nm_vect_insert(argv, buf.data, buf.len + 1, NULL);

                continue;
            }
        }

        nm_vect_free(&serial_cache, nm_usb_data_vect_free_cb);
        nm_vect_free(&usb_list, nm_usb_vect_free_cb);
        nm_str_free(&serial);
    }

    if (nm_vect_str_len(&vm->main, NM_SQL_BIOS)) {
        nm_vect_insert_cstr(argv, "-bios");
        nm_vect_insert(argv,
                       nm_vect_str(&vm->main, NM_SQL_BIOS)->data,
                       nm_vect_str(&vm->main, NM_SQL_BIOS)->len + 1, NULL);
    }

    if (nm_vect_str_len(&vm->main, NM_SQL_MACH)) {
        nm_vect_insert_cstr(argv, "-M");
        nm_vect_insert(argv,
                       nm_vect_str(&vm->main, NM_SQL_MACH)->data,
                       nm_vect_str(&vm->main, NM_SQL_MACH)->len + 1, NULL);
    }

    if (nm_vect_str_len(&vm->main, NM_SQL_KERN)) {
        nm_vect_insert_cstr(argv, "-kernel");
        nm_vect_insert(argv,
                       nm_vect_str(&vm->main, NM_SQL_KERN)->data,
                       nm_vect_str(&vm->main, NM_SQL_KERN)->len + 1, NULL);

        if (nm_vect_str_len(&vm->main, NM_SQL_KAPP)) {
            nm_vect_insert_cstr(argv, "-append");
            nm_vect_insert(argv,
                           nm_vect_str(&vm->main, NM_SQL_KAPP)->data,
                           nm_vect_str(&vm->main, NM_SQL_KAPP)->len + 1, NULL);
        }
    }

    if (nm_vect_str_len(&vm->main, NM_SQL_INIT)) {
        nm_vect_insert_cstr(argv, "-initrd");
        nm_vect_insert(argv,
                       nm_vect_str(&vm->main, NM_SQL_INIT)->data,
                       nm_vect_str(&vm->main, NM_SQL_INIT)->len + 1, NULL);
    }

    if (nm_str_cmp_st(nm_vect_str(&vm->main, NM_SQL_OVER),
                      NM_ENABLE) == NM_OK) {
        nm_vect_insert_cstr(argv, "-usbdevice");
        nm_vect_insert_cstr(argv, "tablet");
    }

    /* setup serial socket */
    if (nm_vect_str_len(&vm->main, NM_SQL_SOCK)) {
        if (!(flags & NM_VMCTL_INFO)) {
            struct stat info;

            if (stat(nm_vect_str_ctx(&vm->main, NM_SQL_SOCK), &info) != -1) {
                nm_warn(_(NM_MSG_SOCK_USED));
                nm_vect_free(argv, NULL);
                goto out;
            }
        }

        nm_vect_insert_cstr(argv, "-chardev");
        nm_str_format(&buf, "socket,path=%s,server,nowait,id=socket_%s",
                      nm_vect_str(&vm->main, NM_SQL_SOCK)->data, name->data);
        nm_vect_insert(argv, buf.data, buf.len + 1, NULL);

        nm_vect_insert_cstr(argv, "-device");
        nm_str_format(&buf, "isa-serial,chardev=socket_%s", name->data);
        nm_vect_insert(argv, buf.data, buf.len + 1, NULL);
    }

    /* setup debug port for GDB */
    if (nm_vect_str_len(&vm->main, NM_SQL_DEBP)) {
        nm_vect_insert_cstr(argv, "-gdb");
        nm_str_format(&buf, "tcp::%s",
                      nm_vect_str(&vm->main, NM_SQL_DEBP)->data);
        nm_vect_insert(argv, buf.data, buf.len + 1, NULL);
    }
    if (nm_str_cmp_st(nm_vect_str(&vm->main, NM_SQL_DEBF), NM_ENABLE) == NM_OK)
        nm_vect_insert_cstr(argv, "-S");

    /* setup serial TTY */
    if (nm_vect_str_len(&vm->main, NM_SQL_TTY)) {
        if (!(flags & NM_VMCTL_INFO)) {
            int fd;

            if ((fd =
                     open(nm_vect_str_ctx(&vm->main, NM_SQL_TTY),
                          O_RDONLY)) == -1) {
                nm_warn(_(NM_MSG_TTY_MISS));
                nm_vect_free(argv, NULL);
                goto out;
            }

            if (!isatty(fd)) {
                close(fd);
                nm_warn(_(NM_MSG_TTY_INVAL));
                nm_vect_free(argv, NULL);
                goto out;
            }
        }

        nm_vect_insert_cstr(argv, "-chardev");
        nm_str_format(&buf, "tty,path=%s,id=tty_%s",
                      nm_vect_str(&vm->main, NM_SQL_TTY)->data,
                      name->data);
        nm_vect_insert(argv, buf.data, buf.len + 1, NULL);

        nm_vect_insert_cstr(argv, "-device");
        nm_str_format(&buf, "isa-serial,chardev=tty_%s",
                      name->data);
        nm_vect_insert(argv, buf.data, buf.len + 1, NULL);
    }

    /* setup network interfaces */
    for (size_t n = 0; n < ifs_count; n++) {
        size_t idx_shift = NM_IFS_IDX_COUNT * n;

        nm_vect_insert_cstr(argv, "-device");
        nm_str_format(&buf, "%s,mac=%s,netdev=netdev%zu",
                      nm_vect_str(&vm->ifs, NM_SQL_IF_DRV + idx_shift)->data,
                      nm_vect_str(&vm->ifs, NM_SQL_IF_MAC + idx_shift)->data,
                      n);
        nm_vect_insert(argv, buf.data, buf.len + 1, NULL);

        if (nm_str_cmp_st(nm_vect_str(&vm->ifs, NM_SQL_IF_MVT + idx_shift),
                          NM_DISABLE) == NM_OK) {
            nm_vect_insert_cstr(argv, "-netdev");
            nm_str_format(&buf,
                          "tap,ifname=%s,script=no,downscript=no,id=netdev%zu",
                          nm_vect_str(&vm->ifs,
                                      NM_SQL_IF_NAME + idx_shift)->data, n);

#if defined (NM_OS_LINUX)
            /* Delete macvtap iface if exists, we using simple tap iface now.
             * We do not need to create tap interface: QEMU will create it itself. */
            if (!(flags & NM_VMCTL_INFO)) {
                uint32_t tap_idx = 0;
                tap_idx = nm_net_iface_idx(nm_vect_str(&vm->ifs,
                                                       NM_SQL_IF_NAME +
                                                       idx_shift));

                if (tap_idx != 0) {
                    /* is this iface macvtap? */
                    struct stat tap_info;
                    nm_str_t tap_path = NM_INIT_STR;

                    nm_str_format(&tap_path, "/dev/tap%u", tap_idx);
                    if (stat(tap_path.data, &tap_info) == 0) {
                        /* iface is macvtap, delete it */
                        nm_net_del_iface(nm_vect_str(&vm->ifs,
                                                     NM_SQL_IF_NAME +
                                                     idx_shift));
                    }
                    nm_str_free(&tap_path);
                }
            }
#endif /* NM_OS_LINUX */
        } else {
#if defined (NM_OS_LINUX)
            int tap_fd = 0, wait_perm = 0;

            if (!(flags & NM_VMCTL_INFO)) {
                nm_str_t tap_path = NM_INIT_STR;
                uint32_t tap_idx = 0;

                /* Delete simple tap iface if exists, we using macvtap iface now */
                if ((tap_idx = nm_net_iface_idx(nm_vect_str(&vm->ifs,
                                                            NM_SQL_IF_NAME +
                                                            idx_shift))) != 0) {
                    /* is this iface simple tap? */
                    struct stat tap_info;
                    nm_str_format(&tap_path, "/dev/tap%u", tap_idx);
                    if (stat(tap_path.data, &tap_info) != 0) {
                        /* iface is simple tap, delete it */
                        nm_net_del_tap(nm_vect_str(&vm->ifs,
                                                   NM_SQL_IF_NAME + idx_shift));
                    }

                    tap_idx = 0;
                }

                if (nm_net_iface_exists(nm_vect_str(&vm->ifs,
                                                    NM_SQL_IF_NAME +
                                                    idx_shift)) != NM_OK) {
                    wait_perm = 1;
                    int macvtap_type = nm_str_stoui(nm_vect_str(&vm->ifs,
                                                                NM_SQL_IF_MVT +
                                                                idx_shift), 10);

                    /* check for lower iface (parent) exists */
                    if (nm_vect_str_len(&vm->ifs,
                                        NM_SQL_IF_PET + idx_shift) == 0) {
                        nm_warn(_(NM_MSG_MTAP_NSET));
                        nm_vect_free(argv, NULL);
                        goto out;
                    }

                    nm_net_add_macvtap(nm_vect_str(&vm->ifs,
                                                   NM_SQL_IF_NAME + idx_shift),
                                       nm_vect_str(&vm->ifs,
                                                   NM_SQL_IF_PET + idx_shift),
                                       nm_vect_str(&vm->ifs,
                                                   NM_SQL_IF_MAC + idx_shift),
                                       macvtap_type);

                    if (nm_vect_str_len(&vm->ifs,
                                        NM_SQL_IF_ALT + idx_shift) != 0)
                        nm_net_set_altname(nm_vect_str(&vm->ifs,
                                                       NM_SQL_IF_NAME +
                                                       idx_shift),
                                           nm_vect_str(&vm->ifs,
                                                       NM_SQL_IF_ALT +
                                                       idx_shift));
                }

                tap_idx = nm_net_iface_idx(nm_vect_str(&vm->ifs,
                                                       NM_SQL_IF_NAME +
                                                       idx_shift));
                if (tap_idx == 0)
                    nm_bug("%s: MacVTap interface not found", __func__);

                nm_str_format(&tap_path, "/dev/tap%u", tap_idx);

                /* wait for udev fixes permitions on /dev/tapN */
                if ((getuid() != 0) && wait_perm) {
                    struct timespec ts;
                    int tap_rw_ok = 0;

                    memset(&ts, 0, sizeof(ts));
                    ts.tv_nsec = 5e+7; /* 0.05sec */

                    for (int m = 0; m < 40; m++) {
                        if (access(tap_path.data, R_OK | W_OK) == 0) {
                            tap_rw_ok = 1;
                            break;
                        }
                        nanosleep(&ts, NULL);
                    }
                    if (!tap_rw_ok) {
                        nm_warn(_(NM_MSG_TAP_EACC));
                        nm_vect_free(argv, NULL);
                        goto out;
                    }
                }

                tap_fd = open(tap_path.data, O_RDWR);
                if (tap_fd == -1)
                    nm_bug("%s: open failed: %s", __func__, strerror(errno));
                if (tfds == NULL)
                    nm_bug("%s: tfds is NULL", __func__);
                nm_vect_insert(tfds, &tap_fd, sizeof(int), NULL);
                nm_str_free(&tap_path);
            }

            nm_vect_insert_cstr(argv, "-netdev");
            nm_str_format(&buf, "tap,id=netdev%zu,fd=%d",
                          n, (flags & NM_VMCTL_INFO) ? -1 : tap_fd);
#endif /* NM_OS_LINUX */
        }
        if (nm_str_cmp_st(nm_vect_str(&vm->ifs, NM_SQL_IF_VHO + idx_shift),
                          NM_ENABLE) == NM_OK)
            nm_str_add_text(&buf, ",vhost=on");
        nm_vect_insert(argv, buf.data, buf.len + 1, NULL);

#if defined (NM_OS_LINUX)
        /* Simple tap interface additional setup:
         * If we need to setup IPv4 address or altname we must create
         * the tap interface yourself. */
        if ((!(flags & NM_VMCTL_INFO)) &&
            (nm_net_iface_exists(nm_vect_str(&vm->ifs,
                                             NM_SQL_IF_NAME + idx_shift)) !=
             NM_OK) &&
            (nm_str_cmp_st(nm_vect_str(&vm->ifs, NM_SQL_IF_MVT + idx_shift),
                           NM_DISABLE) == NM_OK)) {
            nm_net_add_tap(nm_vect_str(&vm->ifs, NM_SQL_IF_NAME + idx_shift));

            if (nm_vect_str_len(&vm->ifs, NM_SQL_IF_IP4 + idx_shift) != 0)
                nm_net_set_ipaddr(nm_vect_str(&vm->ifs,
                                              NM_SQL_IF_NAME + idx_shift),
                                  nm_vect_str(&vm->ifs,
                                              NM_SQL_IF_IP4 + idx_shift));
            if (nm_vect_str_len(&vm->ifs, NM_SQL_IF_ALT + idx_shift) != 0)
                nm_net_set_altname(nm_vect_str(&vm->ifs,
                                               NM_SQL_IF_NAME + idx_shift),
                                   nm_vect_str(&vm->ifs,
                                               NM_SQL_IF_ALT + idx_shift));
        }
#elif defined (NM_OS_FREEBSD)
        if (nm_net_iface_exists(nm_vect_str(&vm->ifs,
                                            NM_SQL_IF_NAME + idx_shift)) ==
            NM_OK)
            nm_net_del_tap(nm_vect_str(&vm->ifs, NM_SQL_IF_NAME + idx_shift));
        (void)tfds;
#endif /* NM_OS_LINUX */
    }

    if (flags & NM_VMCTL_TEMP)
        nm_vect_insert_cstr(argv, "-snapshot");

    nm_vect_insert_cstr(argv, "-pidfile");
    nm_str_format(&buf, "%s%s",
                  vmdir.data, NM_VM_PID_FILE);
    nm_vect_insert(argv, buf.data, buf.len + 1, NULL);

    nm_vect_insert_cstr(argv, "-qmp");
    nm_str_format(&buf, "unix:%s%s,server,nowait",
                  vmdir.data, NM_VM_QMP_FILE);
    nm_vect_insert(argv, buf.data, buf.len + 1, NULL);

#if defined (NM_WITH_SPICE)
    if (nm_str_cmp_st(nm_vect_str(&vm->main, NM_SQL_SPICE),
                      NM_ENABLE) == NM_OK) {
        nm_vect_insert_cstr(argv, "-vga");
        nm_vect_insert_cstr(argv, "qxl");
        nm_vect_insert_cstr(argv, "-spice");
        nm_str_format(&buf, "port=%u,disable-ticketing",
                      nm_str_stoui(nm_vect_str(&vm->main,
                                               NM_SQL_VNC), 10) + 5900);
        if (!cfg->listen_any)
            nm_str_append_format(&buf, ",addr=127.0.0.1");
        nm_vect_insert(argv, buf.data, buf.len + 1, NULL);
    } else {
#endif
    nm_vect_insert_cstr(argv, "-vnc");
    if (cfg->listen_any)
        nm_str_format(&buf, ":");
    else
        nm_str_format(&buf, "127.0.0.1:");
    nm_str_add_str(&buf, nm_vect_str(&vm->main, NM_SQL_VNC));
    nm_vect_insert(argv, buf.data, buf.len + 1, NULL);
#if defined (NM_WITH_SPICE)
}
#endif
    nm_vect_end_zero(argv);

    nm_cmd_str(&buf, argv);
    nm_debug("cmd=%s\n", buf.data);

out:
    nm_str_free(&vmdir);
    nm_str_free(&buf);
}

void nm_vmctl_free_data(nm_vmctl_data_t *vm)
{
    nm_vect_free(&vm->main, nm_str_vect_free_cb);
    nm_vect_free(&vm->ifs, nm_str_vect_free_cb);
    nm_vect_free(&vm->drives, nm_str_vect_free_cb);
    nm_vect_free(&vm->usb, nm_str_vect_free_cb);
}

void nm_vmctl_log_last(const nm_str_t *msg)
{
    FILE *fp;
    const nm_cfg_t *cfg = nm_cfg_get();

    if ((msg->len == 0) || (!cfg->log_enabled))
        return;

    if ((fp = fopen(cfg->log_path.data, "w+")) == NULL)
        nm_bug(_("%s: cannot open file %s:%s"),
               __func__, cfg->log_path.data, strerror(errno));

    fprintf(fp, "%s\n", msg->data);
    fclose(fp);
}

void nm_vmctl_clear_tap(const nm_str_t *name)
{
    nm_vect_t vm = NM_INIT_VECT;

    nm_vect_insert(&vm, name, sizeof(nm_str_t), nm_str_vect_ins_cb);
    (void)nm_vmctl_clear_tap_vect(&vm);

    nm_vect_free(&vm, nm_str_vect_free_cb);
}

void nm_vmctl_clear_all_tap(void)
{
    nm_vect_t vms = NM_INIT_VECT;
    nm_str_t query = NM_INIT_STR;
    int clear_done;

    nm_db_select("SELECT name FROM vms", &vms);
    clear_done = nm_vmctl_clear_tap_vect(&vms);

    nm_notify(clear_done ? _(NM_MSG_IFCLR_DONE) : _(NM_MSG_IFCLR_NONE));

    nm_str_free(&query);
    nm_vect_free(&vms, nm_str_vect_free_cb);
}

static int nm_vmctl_clear_tap_vect(const nm_vect_t *vms)
{
    nm_str_t lock_path = NM_INIT_STR;
    nm_str_t query = NM_INIT_STR;
    int clear_done = 0;

    for (size_t n = 0; n < vms->n_memb; n++) {
        struct stat file_info;
        size_t ifs_count;
        nm_vect_t ifaces = NM_INIT_VECT;

        nm_str_format(&lock_path, "%s/%s/%s",
                      nm_cfg_get()->vm_dir.data, nm_vect_str(vms,
                                                             n)->data,
                      NM_VM_QMP_FILE);

        if (stat(lock_path.data, &file_info) == 0)
            continue;

        nm_str_format(&query, NM_VM_GET_IFACES_SQL, nm_vect_str_ctx(vms, n));
        nm_db_select(query.data, &ifaces);
        ifs_count = ifaces.n_memb / NM_IFS_IDX_COUNT;

        for (size_t ifn = 0; ifn < ifs_count; ifn++) {
            size_t idx_shift = NM_IFS_IDX_COUNT * ifn;
            if (nm_net_iface_exists(nm_vect_str(&ifaces,
                                                NM_SQL_IF_NAME + idx_shift)) ==
                NM_OK) {
#if defined (NM_OS_LINUX)
                nm_net_del_iface(nm_vect_str(&ifaces,
                                             NM_SQL_IF_NAME + idx_shift));
#else
                nm_net_del_tap(nm_vect_str(&ifaces,
                                           NM_SQL_IF_NAME + idx_shift));
#endif
                clear_done = 1;
            }
        }

        nm_str_trunc(&lock_path, 0);
        nm_str_trunc(&query, 0);
        nm_vect_free(&ifaces, nm_str_vect_free_cb);
    }

    nm_str_free(&query);
    nm_str_free(&lock_path);

    return clear_done;
}

#if defined(NM_WITH_VNC_CLIENT) || defined(NM_WITH_SPICE)
static void nm_vmctl_gen_viewer(const nm_str_t *name, uint32_t port,
                                nm_str_t *cmd, int type)
{
    const nm_cfg_t *cfg = nm_cfg_get();
    const nm_str_t *bin =
        (type == NM_VIEWER_SPICE) ? &cfg->spice_bin : &cfg->vnc_bin;
    const nm_str_t *args =
        (type == NM_VIEWER_SPICE) ? &cfg->spice_args : &cfg->vnc_args;
    const nm_view_args_t *pos =
        (type == NM_VIEWER_SPICE) ? &cfg->spice_view : &cfg->vnc_view;
    char *argsp = args->data;
    size_t total = 0;
    nm_str_t warn_msg = NM_INIT_STR;
    nm_str_t buf = NM_INIT_STR;

    nm_str_format(&warn_msg, _("%s viewer: port is not set"),
                  (type == NM_VIEWER_SPICE) ? "spice" : "vnc");

    if (pos->port == -1) {
        nm_warn(warn_msg.data);
        goto out;
    }

    nm_str_append_format(cmd, "%s ", bin->data);

    if (pos->title != -1 && pos->title < pos->port) {
        nm_str_add_str_part(cmd, args, pos->title);
        nm_str_add_text(cmd, name->data);
        argsp += pos->title + 2;
        nm_str_add_text_part(cmd, argsp, pos->port - pos->title - 2);
        nm_str_append_format(cmd, "%u", port);
        total = pos->port;
    } else if (pos->title != -1 && pos->title > pos->port) {
        nm_str_add_str_part(cmd, args, pos->port);
        nm_str_append_format(cmd, "%u", port);
        argsp += pos->port + 2;
        nm_str_add_text_part(cmd, argsp, pos->title - pos->port - 2);
        nm_str_add_text(cmd, name->data);
        total = pos->title;
    } else {
        nm_str_add_str_part(cmd, args, pos->port);
        nm_str_append_format(cmd, "%u", port);
        total = pos->port;
    }

    if (total < (args->len - 2)) {
        argsp += args->len - total;
        nm_str_add_text_part(cmd, argsp, args->len - total - 2);
    }

    nm_str_append_format(cmd, " > /dev/null 2>&1 &");
    nm_debug("viewer cmd:\"%s\"\n", cmd->data);
out:
    nm_str_free(&buf);
    nm_str_free(&warn_msg);
}
#endif

/* vim:set ts=4 sw=4: */
