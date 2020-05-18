/* SPDX-License-Identifier: LGPL-2.1+ */

#include <errno.h>
#include <stdio_ext.h>

#include "alloc-util.h"
#include "dropin.h"
#include "escape.h"
#include "fd-util.h"
#include "fileio.h"
#include "fstab-util.h"
#include "generator.h"
#include "hashmap.h"
#include "id128-util.h"
#include "log.h"
#include "mkdir.h"
#include "parse-util.h"
#include "path-util.h"
#include "proc-cmdline.h"
#include "specifier.h"
#include "string-util.h"
#include "strv.h"
#include "unit-name.h"
#include "util.h"

typedef struct crypto_device {
        char *uuid;
        char *keyfile;
        char *keydev;
        char *hdrdev;
        char *datadev;
        char *name;
        char *options;
        bool create;
} crypto_device;

static const char *arg_dest = "/tmp";
static bool arg_enabled = true;
static bool arg_read_crypttab = true;
static bool arg_whitelist = false;
static Hashmap *arg_disks = NULL;
static char *arg_default_options = NULL;
static char *arg_default_keyfile = NULL;

static int generate_dev_mount(const char *name, const char *dev, const char *type_prefix, bool readonly, char **unit, char **mount) {
        _cleanup_free_ char *u = NULL, *what = NULL, *where = NULL, *name_escaped = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        int r;

        assert(name);
        assert(dev);
        assert(unit);
        assert(mount);

        r = mkdir_parents("/run/systemd/cryptsetup", 0755);
        if (r < 0)
                return r;

        r = mkdir("/run/systemd/cryptsetup", 0700);
        if (r < 0 && errno != EEXIST)
                return -errno;

        name_escaped = cescape(name);
        if (!name_escaped)
                return -ENOMEM;

        where = strjoin("/run/systemd/cryptsetup/", type_prefix, "-", name_escaped);
        if (!where)
                return -ENOMEM;

        r = mkdir(where, 0700);
        if (r < 0 && errno != EEXIST)
                return -errno;

        r = unit_name_from_path(where, ".mount", &u);
        if (r < 0)
                return r;

        r = generator_open_unit_file(arg_dest, NULL, u, &f);
        if (r < 0)
                return r;

        what = fstab_node_to_udev_node(dev);
        if (!what)
                return -ENOMEM;

        fprintf(f,
                "[Unit]\n"
                "DefaultDependencies=no\n\n"
                "[Mount]\n"
                "What=%s\n"
                "Where=%s\n"
                "Options=%s\n", what, where, readonly ? "ro" : "rw");

        r = fflush_and_check(f);
        if (r < 0)
                return r;

        *unit = TAKE_PTR(u);
        *mount = TAKE_PTR(where);

        return 0;
}

static int print_dependencies(FILE *f, const char* device_path) {
        int r;

        if (STR_IN_SET(device_path, "-", "none"))
                /* None, nothing to do */
                return 0;

        if (PATH_IN_SET(device_path, "/dev/urandom", "/dev/random", "/dev/hw_random")) {
                /* RNG device, add random dep */
                fputs("After=systemd-random-seed.service\n", f);
                return 0;
        }

        _cleanup_free_ char *udev_node = fstab_node_to_udev_node(device_path);
        if (!udev_node)
                return log_oom();

        if (path_equal(udev_node, "/dev/null"))
                return 0;

        if (path_startswith(udev_node, "/dev/")) {
                /* We are dealing with a block device, add dependency for correspoding unit */
                _cleanup_free_ char *unit = NULL;

                r = unit_name_from_path(udev_node, ".device", &unit);
                if (r < 0)
                        return log_error_errno(r, "Failed to generate unit name: %m");

                fprintf(f, "After=%1$s\nRequires=%1$s\n", unit);
        } else {
                /* Regular file, add mount dependency */
                _cleanup_free_ char *escaped_path = specifier_escape(device_path);
                if (!escaped_path)
                        return log_oom();

                fprintf(f, "RequiresMountsFor=%s\n", escaped_path);
        }

        return 0;
}

static int create_disk(
                const char *name,
                const char *device,
                const char *keydev,
                const char *hdrdev,
                const char *password,
                const char *options) {

        _cleanup_free_ char *n = NULL, *d = NULL, *u = NULL, *e = NULL,
                *filtered = NULL, *u_escaped = NULL, *password_escaped = NULL, *filtered_escaped = NULL, *name_escaped = NULL, *keydev_mount = NULL, *header_path = NULL,
                *filtered_header = NULL, *hdrdev_mount = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        const char *dmname;
        bool noauto, nofail, tmp, swap, netdev;
        int r, detached_header;

        assert(name);
        assert(device);

        noauto = fstab_test_yes_no_option(options, "noauto\0" "auto\0");
        nofail = fstab_test_yes_no_option(options, "nofail\0" "fail\0");
        tmp = fstab_test_option(options, "tmp\0");
        swap = fstab_test_option(options, "swap\0");
        netdev = fstab_test_option(options, "_netdev\0");

        detached_header = fstab_filter_options(options, "header\0", NULL, &header_path, hdrdev ? &filtered_header : NULL);
        if (detached_header < 0)
                return log_error_errno(detached_header, "Failed to parse header= option value: %m");

        if (tmp && swap)
                return log_error("Device '%s' cannot be both 'tmp' and 'swap'. Ignoring.", name);

        name_escaped = specifier_escape(name);
        if (!name_escaped)
                return log_oom();

        e = unit_name_escape(name);
        if (!e)
                return log_oom();

        u = fstab_node_to_udev_node(device);
        if (!u)
                return log_oom();

        r = unit_name_build("systemd-cryptsetup", e, ".service", &n);
        if (r < 0)
                return log_error_errno(r, "Failed to generate unit name: %m");

        u_escaped = specifier_escape(u);
        if (!u_escaped)
                return log_oom();

        r = unit_name_from_path(u, ".device", &d);
        if (r < 0)
                return log_error_errno(r, "Failed to generate unit name: %m");

        if (password) {
                password_escaped = specifier_escape(password);
                if (!password_escaped)
                        return log_oom();
        }

        if (keydev && !password) {
                log_error("Key device is specified, but path to the password file is missing.");
                return -EINVAL;
        }

        if (hdrdev && !detached_header)
                return log_error("Header device is specified, but path to the header file is missing.");

        r = generator_open_unit_file(arg_dest, NULL, n, &f);
        if (r < 0)
                return r;

        fprintf(f,
                "[Unit]\n"
                "Description=Cryptography Setup for %%I\n"
                "Documentation=man:crypttab(5) man:systemd-cryptsetup-generator(8) man:systemd-cryptsetup@.service(8)\n"
                "SourcePath=/etc/crypttab\n"
                "DefaultDependencies=no\n"
                "Conflicts=umount.target\n"
                "IgnoreOnIsolate=true\n"
                "After=%s\n",
                netdev ? "remote-fs-pre.target" : "cryptsetup-pre.target");

        if (keydev) {
                _cleanup_free_ char *unit = NULL, *p = NULL;

                r = generate_dev_mount(name, keydev, "keydev", true, &unit, &keydev_mount);
                if (r < 0)
                        return log_error_errno(r, "Failed to generate keydev mount unit: %m");

                p = prefix_root(keydev_mount, password_escaped);
                if (!p)
                        return log_oom();

                free_and_replace(password_escaped, p);

                fprintf(f, "After=%s\n"
                           "Requires=%s\n", unit, unit);
        }

        if (hdrdev) {
                _cleanup_free_ char *unit = NULL, *p = NULL;

                /* header device timeout does not make sense, rw access is required for LUKS2 recovery  */
                r = generate_dev_mount(name, hdrdev, "hdrdev", false, &unit, &hdrdev_mount);
                if (r < 0)
                        return log_error_errno(r, "Failed to generate hdrdev mount unit: %m");

                p = path_join(NULL, hdrdev_mount, header_path);
                if (!p)
                        return log_oom();

                free_and_replace(header_path, p);

                p = strjoin(filtered_header, *filtered_header ? ",header=" : "header=", header_path);
                if (!p)
                        return log_oom();

                free_and_replace(filtered_header, p);
                options = filtered_header;

                fprintf(f, "After=%s\n"
                           "Requires=%s\n", unit, unit);
        }

        if (!nofail)
                fprintf(f,
                        "Before=%s\n",
                        netdev ? "remote-cryptsetup.target" : "cryptsetup.target");

        if (password) {
                r = print_dependencies(f, password);
                if (r < 0)
                        return r;
        }

        /* Check if a header option was specified */
        if (detached_header > 0 && !hdrdev) {
                r = print_dependencies(f, header_path);
                if (r < 0)
                        return r;
        }

        if (path_startswith(u, "/dev/")) {
                fprintf(f,
                        "BindsTo=%s\n"
                        "After=%s\n"
                        "Before=umount.target\n",
                        d, d);

                if (swap)
                        fputs("Before=dev-mapper-%i.swap\n",
                              f);
        } else
                fprintf(f,
                        "RequiresMountsFor=%s\n",
                        u_escaped);

        r = generator_write_timeouts(arg_dest, device, name, options, &filtered);
        if (r < 0)
                return r;

        if (filtered) {
                filtered_escaped = specifier_escape(filtered);
                if (!filtered_escaped)
                        return log_oom();
        }

        fprintf(f,
                "\n[Service]\n"
                "Type=oneshot\n"
                "RemainAfterExit=yes\n"
                "TimeoutSec=0\n" /* the binary handles timeouts anyway */
                "KeyringMode=shared\n" /* make sure we can share cached keys among instances */
                "ExecStart=" SYSTEMD_CRYPTSETUP_PATH " attach '%s' '%s' '%s' '%s'\n"
                "ExecStop=" SYSTEMD_CRYPTSETUP_PATH " detach '%s'\n",
                name_escaped, u_escaped, strempty(password_escaped), strempty(filtered_escaped),
                name_escaped);

        if (tmp)
                fprintf(f,
                        "ExecStartPost=/sbin/mke2fs '/dev/mapper/%s'\n",
                        name_escaped);

        if (swap)
                fprintf(f,
                        "ExecStartPost=/sbin/mkswap '/dev/mapper/%s'\n",
                        name_escaped);

        if (keydev)
                fprintf(f,
                        "ExecStartPost=" UMOUNT_PATH " %s\n\n",
                        keydev_mount);

        r = fflush_and_check(f);
        if (r < 0)
                return log_error_errno(r, "Failed to write unit file %s: %m", n);

        if (!noauto) {
                r = generator_add_symlink(arg_dest, d, "wants", n);
                if (r < 0)
                        return r;

                r = generator_add_symlink(arg_dest,
                                          netdev ? "remote-cryptsetup.target" : "cryptsetup.target",
                                          nofail ? "wants" : "requires", n);
                if (r < 0)
                        return r;
        }

        dmname = strjoina("dev-mapper-", e, ".device");
        r = generator_add_symlink(arg_dest, dmname, "requires", n);
        if (r < 0)
                return r;

        if (!noauto && !nofail) {
                r = write_drop_in(arg_dest, dmname, 90, "device-timeout",
                                  "# Automatically generated by systemd-cryptsetup-generator \n\n"
                                  "[Unit]\nJobTimeoutSec=0");
                if (r < 0)
                        return log_error_errno(r, "Failed to write device drop-in: %m");
        }

        return 0;
}

static void crypt_device_free(crypto_device *d) {
        free(d->uuid);
        free(d->keyfile);
        free(d->keydev);
        free(d->name);
        free(d->options);
        free(d);
}

static crypto_device *get_crypto_device(const char *uuid) {
        int r;
        crypto_device *d;

        assert(uuid);

        d = hashmap_get(arg_disks, uuid);
        if (!d) {
                d = new0(struct crypto_device, 1);
                if (!d)
                        return NULL;

                d->create = false;
                d->keyfile = d->options = d->name = NULL;

                d->uuid = strdup(uuid);
                if (!d->uuid)
                        return mfree(d);

                r = hashmap_put(arg_disks, d->uuid, d);
                if (r < 0) {
                        free(d->uuid);
                        return mfree(d);
                }
        }

        return d;
}

static int parse_proc_cmdline_item(const char *key, const char *value, void *data) {
        _cleanup_free_ char *uuid = NULL, *uuid_value = NULL;
        crypto_device *d;
        int r;

        if (streq(key, "luks")) {

                r = value ? parse_boolean(value) : 1;
                if (r < 0)
                        log_warning("Failed to parse luks= kernel command line switch %s. Ignoring.", value);
                else
                        arg_enabled = r;

        } else if (streq(key, "luks.crypttab")) {

                r = value ? parse_boolean(value) : 1;
                if (r < 0)
                        log_warning("Failed to parse luks.crypttab= kernel command line switch %s. Ignoring.", value);
                else
                        arg_read_crypttab = r;

        } else if (streq(key, "luks.uuid")) {

                if (proc_cmdline_value_missing(key, value))
                        return 0;

                d = get_crypto_device(startswith(value, "luks-") ? value+5 : value);
                if (!d)
                        return log_oom();

                d->create = arg_whitelist = true;

        } else if (streq(key, "luks.options")) {

                if (proc_cmdline_value_missing(key, value))
                        return 0;

                r = sscanf(value, "%m[0-9a-fA-F-]=%ms", &uuid, &uuid_value);
                if (r == 2) {
                        d = get_crypto_device(uuid);
                        if (!d)
                                return log_oom();

                        free_and_replace(d->options, uuid_value);
                } else if (free_and_strdup(&arg_default_options, value) < 0)
                        return log_oom();

        } else if (streq(key, "luks.key")) {
                size_t n;
                _cleanup_free_ char *keyfile = NULL, *keydev = NULL;
                char *c;
                const char *keyspec;

                if (proc_cmdline_value_missing(key, value))
                        return 0;

                n = strspn(value, LETTERS DIGITS "-");
                if (value[n] != '=') {
                        if (free_and_strdup(&arg_default_keyfile, value) < 0)
                                 return log_oom();
                        return 0;
                }

                uuid = strndup(value, n);
                if (!uuid)
                        return log_oom();

                if (!id128_is_valid(uuid)) {
                        log_warning("Failed to parse luks.key= kernel command line switch. UUID is invalid, ignoring.");
                        return 0;
                }

                d = get_crypto_device(uuid);
                if (!d)
                        return log_oom();

                keyspec = value + n + 1;
                c = strrchr(keyspec, ':');
                if (c) {
                         *c = '\0';
                        keyfile = strdup(keyspec);
                        keydev = strdup(c + 1);

                        if (!keyfile || !keydev)
                                return log_oom();
                } else {
                        /* No keydev specified */
                        keyfile = strdup(keyspec);
                        if (!keyfile)
                                return log_oom();
                }

                free_and_replace(d->keyfile, keyfile);
                free_and_replace(d->keydev, keydev);
        } else if (streq(key, "luks.hdr")) {
                 size_t n;
                 _cleanup_free_ char *hdrdev = NULL;

                 if (proc_cmdline_value_missing(key, value))
                         return 0;

                 n = strspn(value, ALPHANUMERICAL "-");
                 if (value[n] != '=') {
                         log_warning("Failed to parse luks.hdr= kernel command line switch. UUID is invalid, ignoring.");
                         return 0;
                 }

                 uuid = strndup(value, n);
                 if (!uuid)
                         return log_oom();

                 if (!id128_is_valid(uuid)) {
                         log_warning("Failed to parse luks.hdr= kernel command line switch. UUID is invalid, ignoring.");
                         return 0;
                 }

                 d = get_crypto_device(uuid);
                 if (!d)
                         return log_oom();

                 hdrdev = fstab_node_to_udev_node(value + n + 1);
                 if (!hdrdev)
                         return log_oom();

                 free_and_replace(d->hdrdev, hdrdev);
         } else if (streq(key, "luks.data")) {
                 size_t n;
                 _cleanup_free_ char *datadev = NULL;

                 if (proc_cmdline_value_missing(key, value))
                         return 0;

                 n = strspn(value, ALPHANUMERICAL "-");
                 if (value[n] != '=') {
                         log_warning("Failed to parse luks.data= kernel command line switch. UUID is invalid, ignoring.");
                         return 0;
                 }

                 uuid = strndup(value, n);
                 if (!uuid)
                         return log_oom();

                 if (!id128_is_valid(uuid)) {
                         log_warning("Failed to parse luks.data= kernel command line switch. UUID is invalid, ignoring.");
                         return 0;
                 }

                 d = get_crypto_device(uuid);
                 if (!d)
                         return log_oom();

                 datadev = fstab_node_to_udev_node(value + n + 1);
                 if (!datadev)
                         return log_oom();

                 free_and_replace(d->datadev, datadev);
        } else if (streq(key, "luks.name")) {

                if (proc_cmdline_value_missing(key, value))
                        return 0;

                r = sscanf(value, "%m[0-9a-fA-F-]=%ms", &uuid, &uuid_value);
                if (r == 2) {
                        d = get_crypto_device(uuid);
                        if (!d)
                                return log_oom();

                        d->create = arg_whitelist = true;

                        free_and_replace(d->name, uuid_value);
                } else
                        log_warning("Failed to parse luks name switch %s. Ignoring.", value);
        }

        return 0;
}

static int add_crypttab_devices(void) {
        struct stat st;
        unsigned crypttab_line = 0;
        _cleanup_fclose_ FILE *f = NULL;

        if (!arg_read_crypttab)
                return 0;

        f = fopen("/etc/crypttab", "re");
        if (!f) {
                if (errno != ENOENT)
                        log_error_errno(errno, "Failed to open /etc/crypttab: %m");
                return 0;
        }

        (void) __fsetlocking(f, FSETLOCKING_BYCALLER);

        if (fstat(fileno(f), &st) < 0) {
                log_error_errno(errno, "Failed to stat /etc/crypttab: %m");
                return 0;
        }

        for (;;) {
                int r, k;
                char line[LINE_MAX], *l, *uuid;
                crypto_device *d = NULL;
                _cleanup_free_ char *name = NULL, *device = NULL, *keyfile = NULL, *options = NULL;

                if (!fgets(line, sizeof(line), f))
                        break;

                crypttab_line++;

                l = strstrip(line);
                if (IN_SET(*l, 0, '#'))
                        continue;

                k = sscanf(l, "%ms %ms %ms %ms", &name, &device, &keyfile, &options);
                if (k < 2 || k > 4) {
                        log_error("Failed to parse /etc/crypttab:%u, ignoring.", crypttab_line);
                        continue;
                }

                uuid = startswith(device, "UUID=");
                if (!uuid)
                        uuid = path_startswith(device, "/dev/disk/by-uuid/");
                if (!uuid)
                        uuid = startswith(name, "luks-");
                if (uuid)
                        d = hashmap_get(arg_disks, uuid);

                if (arg_whitelist && !d) {
                        log_info("Not creating device '%s' because it was not specified on the kernel command line.", name);
                        continue;
                }

                r = create_disk(name, device, NULL, NULL, keyfile, (d && d->options) ? d->options : options);
                if (r < 0)
                        return r;

                if (d)
                        d->create = false;
        }

        return 0;
}

static int add_proc_cmdline_devices(void) {
        int r;
        Iterator i;
        crypto_device *d;

        HASHMAP_FOREACH(d, arg_disks, i) {
                const char *options;
                _cleanup_free_ char *device = NULL;

                if (!d->create)
                        continue;

                if (!d->name) {
                        d->name = strappend("luks-", d->uuid);
                        if (!d->name)
                                return log_oom();
                }

                device = strappend("UUID=", d->uuid);
                if (!device)
                        return log_oom();

                if (d->options)
                        options = d->options;
                else if (arg_default_options)
                        options = arg_default_options;
                else
                        options = "timeout=0";

                r = create_disk(d->name, d->datadev ?: device, d->keydev, d->hdrdev, d->keyfile ?: arg_default_keyfile, options);
                if (r < 0)
                        return r;
        }

        return 0;
}

int main(int argc, char *argv[]) {
        int r;

        if (argc > 1 && argc != 4) {
                log_error("This program takes three or no arguments.");
                return EXIT_FAILURE;
        }

        if (argc > 1)
                arg_dest = argv[1];

        log_set_prohibit_ipc(true);
        log_set_target(LOG_TARGET_AUTO);
        log_parse_environment();
        log_open();

        umask(0022);

        arg_disks = hashmap_new(&string_hash_ops);
        if (!arg_disks) {
                r = log_oom();
                goto finish;
        }

        r = proc_cmdline_parse(parse_proc_cmdline_item, NULL, PROC_CMDLINE_STRIP_RD_PREFIX);
        if (r < 0) {
                log_warning_errno(r, "Failed to parse kernel command line: %m");
                goto finish;
        }

        if (!arg_enabled) {
                r = 0;
                goto finish;
        }

        r = add_crypttab_devices();
        if (r < 0)
                goto finish;

        r = add_proc_cmdline_devices();
        if (r < 0)
                goto finish;

        r = 0;

finish:
        hashmap_free_with_destructor(arg_disks, crypt_device_free);
        free(arg_default_options);
        free(arg_default_keyfile);

        return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
