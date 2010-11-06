#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <sys/wait.h>
#include <sys/limits.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/vfs.h> //statfs

#include <signal.h>
#include <sys/wait.h>

#include "bootloader.h"
#include "common.h"
#include "cutils/properties.h"
#include "firmware.h"
#include "install.h"
#include "minui/minui.h"
#include "minzip/DirUtil.h"
#include "roots.h"
#include "recovery_ui.h"

#include "commands.h"
#include "amend/amend.h"

#include "mtdutils/mtdutils.h"
#include "mtdutils/dump_image.h"
#include "../../external/yaffs2/yaffs2/utils/mkyaffs2image.h"
#include "../../external/yaffs2/yaffs2/utils/unyaffs.h"

#include "extendedcommands.h"
#include "nandroid.h"

static const char *SDCARD_PATH = "SDCARD:";
#define SDCARD_PATH_LENGTH 7

int signature_check_enabled = 1;
int script_assert_enabled = 1;
static const char *SDCARD_PACKAGE_FILE = "SDCARD:update.zip";

void
toggle_signature_check()
{
    signature_check_enabled = !signature_check_enabled;
    ui_print("Signature Check: %s\n", signature_check_enabled ? "Enabled" : "Disabled");
}

void toggle_script_asserts()
{
    script_assert_enabled = !script_assert_enabled;
    ui_print("Script Asserts: %s\n", script_assert_enabled ? "Enabled" : "Disabled");
}

int install_zip(const char* packagefilepath)
{
    ui_print("\n-- Installing: %s\n", packagefilepath);
#ifndef BOARD_HAS_NO_MISC_PARTITION
    set_sdcard_update_bootloader_message();
#endif
    int status = install_package(packagefilepath);
    ui_reset_progress();
    if (status != INSTALL_SUCCESS) {
        ui_set_background(BACKGROUND_ICON_ERROR);
        ui_print("Installation aborted.\n");
        return 1;
    } 
#ifndef BOARD_HAS_NO_MISC_PARTITION
    if (firmware_update_pending()) {
        ui_print("\nReboot via menu to complete\ninstallation.\n");
    }
#endif
    ui_set_background(BACKGROUND_ICON_NONE);
    ui_print("\nInstall from sdcard complete.\n");
    return 0;
}

char* INSTALL_MENU_ITEMS[] = {  "Choose zip from sdcard",
                                "Toggle signature verification",
                                "Toggle script asserts",
                                NULL };
#define ITEM_CHOOSE_ZIP       0
#define ITEM_SIG_CHECK        1
#define ITEM_ASSERTS          2

void show_install_update_menu()
{
    static char* headers[] = {  "Apply update from .zip file on SD card",
                                "",
                                NULL 
    };
    for (;;)
    {
        int chosen_item = get_menu_selection(headers, INSTALL_MENU_ITEMS, 0);
        switch (chosen_item)
        {
            case ITEM_ASSERTS:
                toggle_script_asserts();
                break;
            case ITEM_SIG_CHECK:
                toggle_signature_check();
                break;
            case ITEM_CHOOSE_ZIP:
                show_choose_zip_menu();
                break;
            default:
                return;
        }
        
    }
}

void free_string_array(char** array)
{
    if (array == NULL)
        return;
    char* cursor = array[0];
    int i = 0;
    while (cursor != NULL)
    {
        free(cursor);
        cursor = array[++i];
    }
    free(array);
}

char** gather_files(const char* directory, const char* fileExtensionOrDirectory, int* numFiles)
{
    char path[PATH_MAX] = "";
    DIR *dir;
    struct dirent *de;
    int total = 0;
    int i;
    char** files = NULL;
    int pass;
    *numFiles = 0;
    int dirLen = strlen(directory);

    dir = opendir(directory);
    if (dir == NULL) {
        ui_print("Couldn't open directory.\n");
        return NULL;
    }
  
    int extension_length = 0;
    if (fileExtensionOrDirectory != NULL)
        extension_length = strlen(fileExtensionOrDirectory);
  
    int isCounting = 1;
    i = 0;
    for (pass = 0; pass < 2; pass++) {
        while ((de=readdir(dir)) != NULL) {
            // skip hidden files
            if (de->d_name[0] == '.')
                continue;
            
            // NULL means that we are gathering directories, so skip this
            if (fileExtensionOrDirectory != NULL)
            {
                // make sure that we can have the desired extension (prevent seg fault)
                if (strlen(de->d_name) < extension_length)
                    continue;
                // compare the extension
                if (strcmp(de->d_name + strlen(de->d_name) - extension_length, fileExtensionOrDirectory) != 0)
                    continue;
            }
            else
            {
                struct stat info;
                char fullFileName[PATH_MAX];
                strcpy(fullFileName, directory);
                strcat(fullFileName, de->d_name);
                stat(fullFileName, &info);
                // make sure it is a directory
                if (!(S_ISDIR(info.st_mode)))
                    continue;
            }
            
            if (pass == 0)
            {
                total++;
                continue;
            }
            
            files[i] = (char*) malloc(dirLen + strlen(de->d_name) + 2);
            strcpy(files[i], directory);
            strcat(files[i], de->d_name);
            if (fileExtensionOrDirectory == NULL)
                strcat(files[i], "/");
            i++;
        }
        if (pass == 1)
            break;
        if (total == 0)
            break;
        rewinddir(dir);
        *numFiles = total;
        files = (char**) malloc((total+1)*sizeof(char*));
        files[total]=NULL;
    }

    if(closedir(dir) < 0) {
        LOGE("Failed to close directory.");
    }

    if (total==0) {
        return NULL;
    }

	// sort the result
	if (files != NULL) {
		for (i = 0; i < total; i++) {
			int curMax = -1;
			int j;
			for (j = 0; j < total - i; j++) {
				if (curMax == -1 || strcmp(files[curMax], files[j]) < 0)
					curMax = j;
			}
			char* temp = files[curMax];
			files[curMax] = files[total - i - 1];
			files[total - i - 1] = temp;
		}
	}

    return files;
}

// pass in NULL for fileExtensionOrDirectory and you will get a directory chooser
char* choose_file_menu(const char* directory, const char* fileExtensionOrDirectory, const char* headers[])
{
    char path[PATH_MAX] = "";
    DIR *dir;
    struct dirent *de;
    int numFiles = 0;
    int numDirs = 0;
    int i;
    char* return_value = NULL;
    int dir_len = strlen(directory);

    char** files = gather_files(directory, fileExtensionOrDirectory, &numFiles);
    char** dirs = NULL;
    if (fileExtensionOrDirectory != NULL)
        dirs = gather_files(directory, NULL, &numDirs);
    int total = numDirs + numFiles;
    if (total == 0)
    {
        ui_print("No files found.\n");
    }
    else
    {
        char** list = (char**) malloc((total + 1) * sizeof(char*));
        list[total] = NULL;


        for (i = 0 ; i < numDirs; i++)
        {
            list[i] = strdup(dirs[i] + dir_len);
        }

        for (i = 0 ; i < numFiles; i++)
        {
            list[numDirs + i] = strdup(files[i] + dir_len);
        }

        for (;;)
        {
            int chosen_item = get_menu_selection(headers, list, 0);
            if (chosen_item == GO_BACK)
                break;
            static char ret[PATH_MAX];
            if (chosen_item < numDirs)
            {
                char* subret = choose_file_menu(dirs[chosen_item], fileExtensionOrDirectory, headers);
                if (subret != NULL)
                {
                    strcpy(ret, subret);
                    return_value = ret;
                    break;
                }
                continue;
            } 
            strcpy(ret, files[chosen_item - numDirs]);
            return_value = ret;
            break;
        }
        free_string_array(list);
    }

    free_string_array(files);
    free_string_array(dirs);
    return return_value;
}

void show_choose_zip_menu()
{
    if (ensure_root_path_mounted("SDCARD:") != 0) {
        LOGE ("Can't mount /sdcard\n");
        return;
    }

    static char* headers[] = {  "Choose a zip to apply",
                                "",
                                NULL 
    };
    
    char* file = choose_file_menu("/sdcard/", ".zip", headers);
    if (file == NULL)
        return;
    char sdcard_package_file[1024];
    strcpy(sdcard_package_file, "SDCARD:");
    strcat(sdcard_package_file,  file + strlen("/sdcard/"));
    static char* confirm_install  = "Confirm install?";
    static char confirm[PATH_MAX];
    sprintf(confirm, "Yes - Install %s", basename(file));
    if (confirm_selection(confirm_install, confirm))
        install_zip(sdcard_package_file);
}

// This was pulled from bionic: The default system command always looks
// for shell in /system/bin/sh. This is bad.
#define _PATH_BSHELL "/sbin/ash"

extern char **environ;
int
__system(const char *command)
{
  pid_t pid;
    sig_t intsave, quitsave;
    sigset_t mask, omask;
    int pstat;
    char *argp[] = {"sh", "-c", NULL, NULL};

    if (!command)        /* just checking... */
        return(1);

    argp[2] = (char *)command;

    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &mask, &omask);
    switch (pid = vfork()) {
    case -1:            /* error */
        sigprocmask(SIG_SETMASK, &omask, NULL);
        return(-1);
    case 0:                /* child */
        sigprocmask(SIG_SETMASK, &omask, NULL);
        execve(_PATH_BSHELL, argp, environ);
    _exit(127);
  }

    intsave = (sig_t)  bsd_signal(SIGINT, SIG_IGN);
    quitsave = (sig_t) bsd_signal(SIGQUIT, SIG_IGN);
    pid = waitpid(pid, (int *)&pstat, 0);
    sigprocmask(SIG_SETMASK, &omask, NULL);
    (void)bsd_signal(SIGINT, intsave);
    (void)bsd_signal(SIGQUIT, quitsave);
    return (pid == -1 ? -1 : pstat);
}

void show_nandroid_restore_menu()
{
    if (ensure_root_path_mounted("SDCARD:") != 0) {
        LOGE ("Can't mount /sdcard\n");
        return;
    }
    
    static char* headers[] = {  "Choose an image to restore",
                                "",
                                NULL 
    };

    char* file = choose_file_menu("/sdcard/clockworkmod/backup/", NULL, headers);
    if (file == NULL)
        return;

    if (confirm_selection("Confirm restore?", "Yes - Restore"))
        nandroid_restore(file, 1, 1, 1, 1, 1);
}

void show_mount_usb_storage_menu()
{
    char command[PATH_MAX];
    sprintf(command, "echo %s > /sys/devices/platform/usb_mass_storage/lun0/file", SDCARD_DEVICE_PRIMARY);
    __system(command);
    static char* headers[] = {  "USB Mass Storage device",
                                "Leaving this menu unmount",
                                "your SD card from your PC.",
                                "",
                                NULL 
    };
    
    static char* list[] = { "Unmount", NULL };
    
    for (;;)
    {
        int chosen_item = get_menu_selection(headers, list, 0);
        if (chosen_item == GO_BACK || chosen_item == 0)
            break;
    }
    
    __system("echo '' > /sys/devices/platform/usb_mass_storage/lun0/file");
    __system("echo 0 > /sys/devices/platform/usb_mass_storage/lun0/enable");
}

int confirm_selection(const char* title, const char* confirm)
{
    struct stat info;
    if (0 == stat("/sdcard/clockworkmod/.no_confirm", &info))
        return 1;

    char* confirm_headers[]  = {  title, "  THIS CAN NOT BE UNDONE.", "", NULL };
    char* items[] = { "No",
                      "No",
                      "No",
                      "No",
                      "No",
                      "No",
                      "No",
                      confirm, //" Yes -- wipe partition",   // [7
                      "No",
                      "No",
                      "No",
                      NULL };

    int chosen_item = get_menu_selection(confirm_headers, items, 0);
    return chosen_item == 7;
}

int format_non_mtd_device(const char* root)
{
    // if this is SDEXT:, don't worry about it.
    if (0 == strcmp(root, "SDEXT:"))
    {
        struct stat st;
        if (0 != stat(SDEXT_DEVICE, &st))
        {
            ui_print("No app2sd partition found. Skipping format of /sd-ext.\n");
            return 0;
        }
    }

    char path[PATH_MAX];
    translate_root_path(root, path, PATH_MAX);
    if (0 != ensure_root_path_mounted(root))
    {
        ui_print("Error mounting %s!\n", path);
        ui_print("Skipping format...\n");
        return 0;
    }

    static char tmp[PATH_MAX];
    sprintf(tmp, "rm -rf %s/*", path);
    __system(tmp);
    sprintf(tmp, "rm -rf %s/.*", path);
    __system(tmp);
    
    ensure_root_path_unmounted(root);
    return 0;
}

#define MOUNTABLE_COUNT 5
#define MTD_COUNT 4
#define MMC_COUNT 2

void show_partition_menu()
{
    static char* headers[] = {  "Mounts and Storage Menu",
                                "",
                                NULL 
    };

    typedef char* string;
    string mounts[MOUNTABLE_COUNT][3] = { 
        { "mount /system", "unmount /system", "SYSTEM:" },
        { "mount /data", "unmount /data", "DATA:" },
        { "mount /cache", "unmount /cache", "CACHE:" },
        { "mount /sdcard", "unmount /sdcard", "SDCARD:" },
        { "mount /sd-ext", "unmount /sd-ext", "SDEXT:" }
        };
        
    string mtds[MTD_COUNT][2] = {
        { "format boot", "BOOT:" },
        { "format system", "SYSTEM:" },
        { "format data", "DATA:" },
        { "format cache", "CACHE:" },
    };
    
    string mmcs[MMC_COUNT][3] = {
      { "format sdcard", "SDCARD:" },
      { "format sd-ext", "SDEXT:" }  
    };
    
    static char* confirm_format  = "Confirm format?";
    static char* confirm = "Yes - Format";
        
    for (;;)
    {
        int ismounted[MOUNTABLE_COUNT];
        int i;
        static string options[MOUNTABLE_COUNT + MTD_COUNT + MMC_COUNT + 1 + 1]; // mountables, format mtds, format mmcs, usb storage, null
        for (i = 0; i < MOUNTABLE_COUNT; i++)
        {
            ismounted[i] = is_root_path_mounted(mounts[i][2]);
            options[i] = ismounted[i] ? mounts[i][1] : mounts[i][0];
        }
        
        for (i = 0; i < MTD_COUNT; i++)
        {
            options[MOUNTABLE_COUNT + i] = mtds[i][0];
        }
            
        for (i = 0; i < MMC_COUNT; i++)
        {
            options[MOUNTABLE_COUNT + MTD_COUNT + i] = mmcs[i][0];
        }
    
        options[MOUNTABLE_COUNT + MTD_COUNT + MMC_COUNT] = "mount USB storage";
        options[MOUNTABLE_COUNT + MTD_COUNT + MMC_COUNT + 1] = NULL;
        
        int chosen_item = get_menu_selection(headers, options, 0);
        if (chosen_item == GO_BACK)
            break;
        if (chosen_item == MOUNTABLE_COUNT + MTD_COUNT + MMC_COUNT)
        {
            show_mount_usb_storage_menu();
        }
        else if (chosen_item < MOUNTABLE_COUNT)
        {
            if (ismounted[chosen_item])
            {
                if (0 != ensure_root_path_unmounted(mounts[chosen_item][2]))
                    ui_print("Error unmounting %s!\n", mounts[chosen_item][2]);
            }
            else
            {
                if (0 != ensure_root_path_mounted(mounts[chosen_item][2]))
                    ui_print("Error mounting %s!\n", mounts[chosen_item][2]);
            }
        }
        else if (chosen_item < MOUNTABLE_COUNT + MTD_COUNT)
        {
            chosen_item = chosen_item - MOUNTABLE_COUNT;
            if (!confirm_selection(confirm_format, confirm))
                continue;
            ui_print("Formatting %s...\n", mtds[chosen_item][1]);
            if (0 != format_root_device(mtds[chosen_item][1]))
                ui_print("Error formatting %s!\n", mtds[chosen_item][1]);
            else
                ui_print("Done.\n");
        }
        else if (chosen_item < MOUNTABLE_COUNT + MTD_COUNT + MMC_COUNT)
        {
            chosen_item = chosen_item - MOUNTABLE_COUNT - MTD_COUNT;
            if (!confirm_selection(confirm_format, confirm))
                continue;
            ui_print("Formatting %s...\n", mmcs[chosen_item][1]);
            if (0 != format_non_mtd_device(mmcs[chosen_item][1]))
                ui_print("Error formatting %s!\n", mmcs[chosen_item][1]);
            else
                ui_print("Done.\n");
        }
    }
}

#define EXTENDEDCOMMAND_SCRIPT "/cache/recovery/extendedcommand"

int extendedcommand_file_exists()
{
    struct stat file_info;
    return 0 == stat(EXTENDEDCOMMAND_SCRIPT, &file_info);
}

int run_script_from_buffer(char* script_data, int script_len, char* filename)
{
    /* Parse the script.  Note that the script and parse tree are never freed.
     */
    const AmCommandList *commands = parseAmendScript(script_data, script_len);
    if (commands == NULL) {
        printf("Syntax error in update script\n");
        return 1;
    } else {
        printf("Parsed %.*s\n", script_len, filename);
    }

    /* Execute the script.
     */
    int ret = execCommandList((ExecContext *)1, commands);
    if (ret != 0) {
        int num = ret;
        char *line = NULL, *next = script_data;
        while (next != NULL && ret-- > 0) {
            line = next;
            next = memchr(line, '\n', script_data + script_len - line);
            if (next != NULL) *next++ = '\0';
        }
        printf("Failure at line %d:\n%s\n", num, next ? line : "(not found)");
        return 1;
    }    
    
    return 0;
}

int run_script(char* filename)
{
    struct stat file_info;
    if (0 != stat(filename, &file_info)) {
        printf("Error executing stat on file: %s\n", filename);
        return 1;
    }
    
    int script_len = file_info.st_size;
    char* script_data = (char*)malloc(script_len + 1);
    FILE *file = fopen(filename, "rb");
    fread(script_data, script_len, 1, file);
    // supposedly not necessary, but let's be safe.
    script_data[script_len] = '\0';
    fclose(file);
    LOGI("Running script:\n");
    LOGI("\n%s\n", script_data);

    int ret = run_script_from_buffer(script_data, script_len, filename);
    free(script_data);
    return ret;
}

int run_and_remove_extendedcommand()
{
    char tmp[PATH_MAX];
    sprintf(tmp, "cp %s /tmp/%s", EXTENDEDCOMMAND_SCRIPT, basename(EXTENDEDCOMMAND_SCRIPT));
    __system(tmp);
    remove(EXTENDEDCOMMAND_SCRIPT);
    int i = 0;
    for (i = 20; i > 0; i--) {
        ui_print("Waiting for SD Card to mount (%ds)\n", i);
        if (ensure_root_path_mounted("SDCARD:") == 0) {
            ui_print("SD Card mounted...\n");
            break;
        }
        sleep(1);
    }
    remove("/sdcard/clockworkmod/.recoverycheckpoint");
    if (i == 0) {
        ui_print("Timed out waiting for SD card... continuing anyways.");
    }
    
    sprintf(tmp, "/tmp/%s", basename(EXTENDEDCOMMAND_SCRIPT));
    return run_script(tmp);
}

int amend_main(int argc, char** argv)
{
    if (argc != 2) 
    {
        printf("Usage: amend <script>\n");
        return 0;
    }

    RecoveryCommandContext ctx = { NULL };
    if (register_update_commands(&ctx)) {
        LOGE("Can't install update commands\n");
    }
    return run_script(argv[1]);
}

void show_nandroid_advanced_restore_menu()
{
    if (ensure_root_path_mounted("SDCARD:") != 0) {
        LOGE ("Can't mount /sdcard\n");
        return;
    }

    static char* advancedheaders[] = {  "Choose an image to restore",
                                "",
                                "Choose an image to restore",
                                "first. The next menu will",
                                "you more options.",
                                "",
                                NULL
    };

    char* file = choose_file_menu("/sdcard/clockworkdmod/backup/", NULL, advancedheaders);
    if (file == NULL)
        return;

    static char* headers[] = {  "Nandroid Advanced Restore",
                                "",
                                NULL
    };

    static char* list[] = { "Restore boot",
                            "Restore system",
                            "Restore data",
                            "Restore cache",
                            "Restore sd-ext",
                            NULL
    };


    static char* confirm_restore  = "Confirm restore?";

    int chosen_item = get_menu_selection(headers, list, 0);
    switch (chosen_item)
    {
        case 0:
            if (confirm_selection(confirm_restore, "Yes - Restore boot"))
                nandroid_restore(file, 1, 0, 0, 0, 0);
            break;
        case 1:
            if (confirm_selection(confirm_restore, "Yes - Restore system"))
                nandroid_restore(file, 0, 1, 0, 0, 0);
            break;
        case 2:
            if (confirm_selection(confirm_restore, "Yes - Restore data"))
                nandroid_restore(file, 0, 0, 1, 0, 0);
            break;
        case 3:
            if (confirm_selection(confirm_restore, "Yes - Restore cache"))
                nandroid_restore(file, 0, 0, 0, 1, 0);
            break;
        case 4:
            if (confirm_selection(confirm_restore, "Yes - Restore sd-ext"))
                nandroid_restore(file, 0, 0, 0, 0, 1);
            break;
    }
}

static int
        choose_tar_file(char* sfpath) //by LeshaK
{
    static char* headers[] = { "Choose backup TAR file",
                               "",
                               "Use Up/Down keys to highlight;",
                               "click OK to select.",
                               "",
                               NULL };

    char path[PATH_MAX] = "";
    DIR *dir;
    struct dirent *de;
    char **files;
    int total = 0;
    int retval = 1;
    int i;

    if (ensure_root_path_mounted(SDCARD_PATH) != 0) {
        LOGE("Can't mount %s\n", SDCARD_PATH);
        return 1;
    }

    if (translate_root_path(SDCARD_PATH, path, sizeof(path)) == NULL) {
        LOGE("Bad path %s", path);
        return 2;
    }

    strcat(path, "samdroid/");

    dir = opendir(path);
    if (dir == NULL) {
        LOGE("Couldn't open directory %s", path);
        return 3;
    }

    /* count how many files we're looking at */
    while ((de = readdir(dir)) != NULL) {
        char *extension = strrchr(de->d_name, '.');
        if (extension == NULL || de->d_name[0] == '.') {
            continue;
        } else if (!strcasecmp(extension, ".tar")) {
            total++;
        }
    }

    /* allocate the array for the file list menu */
    files = (char **) malloc((total + 1) * sizeof(*files));
    files[total] = NULL;

    /* set it up for the second pass */
    rewinddir(dir);

    /* put the names in the array for the menu */
    i = 0;
    while ((de = readdir(dir)) != NULL) {
        char *extension = strrchr(de->d_name, '.');
        if (extension == NULL || de->d_name[0] == '.') {
            continue;
        } else if (!strcasecmp(extension, ".tar")) {
            files[i] = (char *) malloc(SDCARD_PATH_LENGTH + strlen(de->d_name) + 1);
            //strcpy(files[i], SDCARD_PATH);
            //strcat(files[i], de->d_name);
            strcpy(files[i], de->d_name);
            i++;
        }
    }

    /* close directory handle */
    if (closedir(dir) < 0) {
        LOGE("Failure closing directory %s", path);
        goto out;
    }

    int selected = 0;
    int chosen_item = -1;

    ui_reset_progress();
    for (;;) {

        chosen_item = get_menu_selection(headers,files,0);

        if (chosen_item >= 0) {
            // turn off the menu, letting ui_print() to scroll output
            // on the screen.
            ui_end_menu();
            strcpy(sfpath, files[chosen_item]);
            retval = 0;
            break;
        }
    }

    out:

    for (i = 0; i < total; i++) {
        free(files[i]);
    }
    free(files);
    return retval;
}

static void
        tar_backup() //by Leshak
{
    static char* headers[] = { 	"Choose what you want to backup?"
                                "",
                                "Use Up/Down and OK to select",
                                "",
                                NULL };

#define BRTYPE_B_SYS		0
#define BRTYPE_B_DATA	 	1
#define BRTYPE_HL1		 	2
#define BRTYPE_RESTORE	 	3
#define BRTYPE_REST_FORMAT 	4

    char st[255];
    static char* backup_parts[] = { "/system", "/data"};
    static char* backup_file[] = { "Sys", "Data"};

    static char* items[] = { 	"TAR backup system",
                                "TAR backup data",
                                "    -------",
                                "TAR restore",
                                "TAR restore (+ format)",
                                NULL };


    for (;;) {

        int chosen_item = get_menu_selection(headers, items,0);

        if (chosen_item == GO_BACK) break;

        if (chosen_item >= BRTYPE_RESTORE) {
            char sfpath[255];
            if (choose_tar_file(st) == 0) {
                ui_print("\n-- Press HOME to confirm, or");
    	        ui_print("\n-- any other key to abort..");
                if (ui_wait_key() == KEY_DREAM_HOME) {
                    switch (chosen_item) {
                    case BRTYPE_REST_FORMAT:
                        ui_print("\nFormating ");
                        if (strstr(st, "_Sys.")) {
                            if (!ensure_root_path_unmounted("SYSTEM:")) {
                                ui_print("/system");
                                if (!format_root_device("SYSTEM:")) ui_print("ok");
                            }
                        }
                        if (strstr(st, "_Data.")) {
                            if (!ensure_root_path_unmounted("DATA:")) {
                                ui_print("/data");
                                if (!format_root_device("DATA:")) ui_print("ok");
                            }
                        }
						case BRTYPE_RESTORE:
                        strcpy(sfpath, "/sdcard/samdroid/");
                        strcat(sfpath, st);

                        ui_print("\nMount ");
                        if (strstr(st, "_Sys.")) {
                            ui_print("/system");
                            if (ensure_root_path_mounted("SYSTEM:")) { ui_print("\nError mount /system\n"); return; }
                        }
                        if (strstr(st, "_Data.")) {
                            ui_print("/data");
                            if (ensure_root_path_mounted("DATA:")) { ui_print("\nError mount /data\n"); return; }
                        }

                        ui_print("\nRestoring..");

                        pid_t pid = fork();
                        if (pid == 0) {
                            chdir("/");
                            char *args[] = {"/xbin/tar", "-x","-f", sfpath, NULL};
                            execv("/xbin/tar", args);
                            fprintf(stderr, "E:Can't backup\n(%s)\n", strerror(errno));
                            _exit(-1);
                        }

                        int status;

                        while (waitpid(pid, &status, WNOHANG) == 0) {
                            ui_print(".");
                            sleep(1);
                        }
                        ui_print("\n");

                        if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
                            LOGE("Can't extract tar file %s\n", st);
                        } else {
                            ui_print("\nRestore complete.\n");
                        }
                        break;
                    }
                    continue;
                }
            }
            ui_print("\nData restore aborted.\n");
            continue;
        }

        if (chosen_item >= 0 && chosen_item < BRTYPE_HL1) {

            ui_print("\n-- Press HOME to confirm, or");
            ui_print("\n-- any other key to abort..");
            int confirm_wipe = ui_wait_key();
            if (confirm_wipe == KEY_DREAM_HOME) {
                switch (chosen_item) {
                case BRTYPE_B_SYS:
                    if (ensure_root_path_mounted("SYSTEM:")) { ui_print("\nError mount /system\n"); return; }
                    break;
                case BRTYPE_B_DATA:
                    if (ensure_root_path_mounted("DATA:")) { ui_print("\nError mount /data\n"); return; }
                    break;
                }
                switch (chosen_item) {
                case BRTYPE_B_SYS:
                case BRTYPE_B_DATA:
                    if (ensure_root_path_mounted("SDCARD:")) { ui_print("\nError mount sdcard\n"); return; }
                    ui_print("\nBackuping: ");
                    ui_print(backup_parts[chosen_item-1]);
                    ui_print("\n");

                    // create backup folder
                    mkdir("/sdcard/samdroid", 0777);

                    // create file name
                    time_t rawtime;
                    struct tm * ti;
                    time ( &rawtime );
                    ti = localtime ( &rawtime );
                    strftime(st,255,"/sdcard/samdroid/Backup_%Y%m%d-%H%M%S_",ti);
                    strcat(st, backup_file[chosen_item-1]);
                    strcat(st, ".tar");

                    pid_t pid = fork();
                    if (pid == 0) {
                        char *args[] = {"/xbin/busybox", "tar", "-c", "--exclude=*RFS_LOG.LO*", "-f", st, backup_parts[chosen_item-1], NULL};
                        execv("/xbin/busybox", args);
                        fprintf(stderr, "E:Can't backup\n(%s)\n", strerror(errno));
                        _exit(-1);
                    }

                    int status;

                    while (waitpid(pid, &status, WNOHANG) == 0) {
                        ui_print(".");
                        sleep(1);
                    }
                    ui_print("\n");

                    if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
                        LOGE("Can't create tar file %s\n", st);
                    } else {
                        ui_print("Backup complete.\n");
                    }
                    break;
                }

            } else {
                ui_print("\nData backup aborted.\n");
            }
            if (!ui_text_visible()) break;
        }
    }
}

static void samdroid_backup()
{
	if (ensure_root_path_mounted("SDCARD:") != 0) {
		ui_print("Can't mount sdcard\n");
	} else {
		ui_print("\nPerforming backup");
		pid_t pid = fork();
		if (pid == 0) {
			char *args[] = {"/xbin/bash", "-c", "/xbin/samdroid backup", "1>&2", NULL};
			execv("/xbin/bash", args);
			fprintf(stderr, "E:Can't run samdroid\n(%s)\n", strerror(errno));
			_exit(-1);
		}

		int status;

		while (waitpid(pid, &status, WNOHANG) == 0) {
			ui_print(".");
			sleep(1);
		}
		ui_print("\n");

		if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
			ui_print("Error running samdroid backup. Backup not performed.\n\n");
		} else {
			ui_print("Backup complete!\nUse Odin for restore\n\n");
		}
	}
}


void show_backup_menu()
{
	static char* headers[] = {  "Backups and Restore",
                                "",
                                NULL 
    };

    static char* list[] = { "TAR Backup", 
                            "Samdroid Backup (Odin)",
                            NULL
    };

    int chosen_item = get_menu_selection(headers, list, 0);
    switch (chosen_item)
    {
        case 0:
            tar_backup();
            break;
        case 1:
            samdroid_backup();
            break;
    }
}

void show_nandroid_menu()
{
    static char* headers[] = {  "Nandroid",
                                "",
                                NULL 
    };

    static char* list[] = { "Backup", 
                            "Restore",
                            "Advanced Restore",
                            NULL
    };

    int chosen_item = get_menu_selection(headers, list, 0);
    switch (chosen_item)
    {
        case 0:
            {
                char backup_path[PATH_MAX];
                time_t t = time(NULL);
                struct tm *tmp = localtime(&t);
                if (tmp == NULL)
                {
                    struct timeval tp;
                    gettimeofday(&tp, NULL);
                    sprintf(backup_path, "/sdcard/clockworkmod/backup/%d", tp.tv_sec);
                }
                else
                {
                    strftime(backup_path, sizeof(backup_path), "/sdcard/clockworkmod/backup/%F.%H.%M.%S", tmp);
                }
                nandroid_backup(backup_path);
            }
            break;
        case 1:
            show_nandroid_restore_menu();
            break;
        case 2:
            show_nandroid_advanced_restore_menu();
            break;
    }
}

void wipe_battery_stats()
{
    ensure_root_path_mounted("DATA:");
    remove("/data/system/batterystats.bin");
    ensure_root_path_unmounted("DATA:");
}

void show_fs_select(RootInfo* info)
{
	char* list[]={ "rfs",
				   "ext2",
				   "ext4",
				   NULL
	};
	
	char nm[20];
	char fs[30];

	sprintf(nm,"     %s",info->name);
	sprintf(fs,"     Now: %s",info->filesystem);
	char* headers[]={ "Choose a new filesystem for",
					 nm,
					 fs,
					 "",
					 NULL
	};
	if ( !strcmp(info->name,"CACHE:") ) {
		int ret;
		struct statfs s;
		if (0 != (ret = statfs("/sdcard", &s)))
			return print_and_error("Unable to stat /sdcard\n");
		uint64_t bavail = s.f_bavail;
		uint64_t bsize = s.f_bsize;
		uint64_t sdcard_free = bavail * bsize;
		uint64_t sdcard_free_mb = sdcard_free / (uint64_t)(1024 * 1024);
		ui_print("SD Card space free: %lluMB\n", sdcard_free_mb);
		if (sdcard_free_mb < 220) return print_and_error("You don't have enough free space on your SD Card!\n");
	}

	for(;;) {
		int err=0;
		int chosen_item = get_menu_selection(headers, list, 0);
		if (chosen_item == GO_BACK)
            break;
        ui_end_menu();
		ui_print("\n-- This method can be dangerous!");
		ui_print("\n-- It is going to be very long!");
		ui_print("\n-- Press HOME to confirm, or");
		ui_print("\n-- any other key to abort..");
		int confirm_wipe = ui_wait_key();
		if (confirm_wipe == KEY_DREAM_HOME) {
			ui_print("\nPlease wait..");
			pid_t pid = fork();
			if (pid == 0) {
				struct stat st;
				if(stat("/sdcard/samdroid",&st)) {
					mkdir("/sdcard/samdroid",0777);
				}
				if (ensure_root_path_mounted(info->name)) {
					LOGE("Backup failed:\nCan't mount filesystem!\n");
					err=1;
				}
				else {
					char old[10];
					char new[10];
					strcpy(old,info->filesystem);
					strcpy(new,list[chosen_item]);
					char backup[PATH_MAX];
					sprintf(backup,"/sdcard/samdroid/Backup_%s_%sTO%s.tar",&(info->mount_point[1]),old,new);
					FILE* f=fopen(backup,"r");
					if ( f != NULL ) {
						fclose(f);
						unlink(backup);
					}
					char cmd[PATH_MAX];
					if ( strcmp(info->name,"CACHE:") ) {
						ui_print("\nBackuping");
						sprintf(cmd,"tar -c --exclude=*RFS_LOG.LO* -f %s %s",backup,info->mount_point);
						if (__system(cmd)) {
							LOGE("Backup failed:\n%s\n",strerror(errno));
							err=1;
						}
					}
					if ( !err ) {
						if (ensure_root_path_unmounted(info->name)) {
							LOGE("Can't unmount filesystem!\n");
							err=1;
						}
						else {
							ui_print("\nFormatting");
							strcpy(info->filesystem,new);
							if ( format_root_device(info->name) < 0 ) {
								LOGE("Can't format device!\n");
								err=1;
							}
							else {
								if (ensure_root_path_mounted(info->name)){
									LOGE("Can't remount filesystem!\n");
									err=1;
								}
								else {
									chdir("/");
									if ( strcmp(info->name,"CACHE:") ) {
										ui_print("\nRestoring");
										sprintf(cmd,"tar -x -f %s",backup);
										if (__system(cmd)) {
											LOGE("Restore failed:\n%s\n",strerror(errno));
											err=1;
										}
									}
								}
							}
						}
					}
				}
				_exit(-1);
			}

			int status;

			while (waitpid(pid, &status, WNOHANG) == 0) {
				ui_print(".");
				sleep(1);
			}
			if (!err) ui_print("\nConversion was successful!\n");
			else ui_print("\nConversion failed!\n");

			break;
		}
	}
}
			
			

void show_fs_menu()
{
	static char* list[4];
	static char* headers[]={ "Choose a device",
					 "",
					 NULL
	};
	
	if (ensure_root_path_mounted("SDCARD:") != 0)
        return print_and_error("Can't mount /sdcard\n");
	
	list[0]=NULL;
	list[1]=NULL;
	list[2]=NULL;
	list[3]=NULL;

	for(;;) {
		//We want to recheck the actual FileSystem after show_fs_select returned.
		RootInfo *info;
		int i;
		for (i = 1 ;i < 4 ;i++){
		  switch (i) {
			case 1:
				info=get_root_info_for_path("CACHE:");
				break;
			case 2:
				info=get_root_info_for_path("DATA:");
				break;
			case 3:
				info=get_root_info_for_path("SYSTEM:");
				break;
		  }
		  if (list[i-1] != NULL) free(list[i-1]);
		  list[i-1]=calloc(20,sizeof(char));
		  sprintf(list[i-1],"%s (%s)",info->name,info->filesystem);
		}
	
		int chosen_item = get_menu_selection(headers, list, 0);
		if (chosen_item == GO_BACK)
            break;
        switch (chosen_item) {
			case 0:
				info=get_root_info_for_path("CACHE:");
				break;
			case 1:
				info=get_root_info_for_path("DATA:");
				break;
			case 2:
				info=get_root_info_for_path("SYSTEM:");
				break;
		}
		if ( ensure_root_path_unmounted(info->name) ) {
			LOGE("Can't unmount selected device!\n");
			continue;
		} else {
			ensure_root_path_unmounted(info->name); //If it may system, it's better to unmount for create_mtab
			create_mtab();
			show_fs_select(info);
			//For rechecking
			ensure_root_path_unmounted(info->name);
			recheck();
		}
	}
}
        

void show_advanced_menu()
{
    static char* headers[] = {  "Advanced and Debugging Menu",
                                "",
                                NULL
    };

    static char* list[] = { "Wipe Battery Stats",
                            "Report Error",
                            "Key Test",
                            "Filesystem conversion",
#ifndef BOARD_HAS_SMALL_RECOVERY
                            "Partition SD Card",
                            "Fix Permissions",
#endif
                            NULL
    };

    for (;;)
    {
        int chosen_item = get_menu_selection(headers, list, 0);
        if (chosen_item == GO_BACK)
            break;
        switch (chosen_item)
        {
            case 0:
            {
                if (confirm_selection( "Confirm wipe?", "Yes - Wipe Battery Stats"))
                    wipe_battery_stats();
                break;
            }
            case 1:
                handle_failure(1);
                break;
            case 2:
            {
                ui_print("Outputting key codes.\n");
                ui_print("Go back to end debugging.\n");
                int key;
                int action;
                do
                {
                    key = ui_wait_key();
                    action = device_handle_key(key, 1);
                    ui_print("Key: %d\n", key);
                }
                while (action != GO_BACK);
                break;
            }
            case 3:
				show_fs_menu();
				break;
            case 4:
            {
                static char* ext_sizes[] = { "128M",
                                             "256M",
                                             "512M",
                                             "1024M",
                                             NULL };

                static char* swap_sizes[] = { "0M",
                                              "32M",
                                              "64M",
                                              "128M",
                                              "256M",
                                              NULL };

                static char* ext_headers[] = { "Ext Size", "", NULL };
                static char* swap_headers[] = { "Swap Size", "", NULL };

                int ext_size = get_menu_selection(ext_headers, ext_sizes, 0);
                if (ext_size == GO_BACK)
                    continue;
                 
                int swap_size = get_menu_selection(swap_headers, swap_sizes, 0);
                if (swap_size == GO_BACK)
                    continue;

                char sddevice[256];
                const RootInfo *ri = get_root_info_for_path("SDCARD:");
                strcpy(sddevice, ri->device);
                // we only want the mmcblk, not the partition
                sddevice[strlen("/dev/block/mmcblkX")] = NULL;
                char cmd[PATH_MAX];
                setenv("SDPATH", sddevice, 1);
                sprintf(cmd, "sdparted -es %s -ss %s -efs ext3 -s", ext_sizes[ext_size], swap_sizes[swap_size]);
                ui_print("Partitioning SD Card... please wait...\n");
                if (0 == __system(cmd))
                    ui_print("Done!\n");
                else
                    ui_print("An error occured while partitioning your SD Card. Please see /tmp/recovery.log for more details.\n");
                break;
            }
            case 5:
            {
                ensure_root_path_mounted("SYSTEM:");
                ensure_root_path_mounted("DATA:");
                ui_print("Fixing permissions...\n");
                __system("fix_permissions");
                ui_print("Done!\n");
                break;
            }
        }
    }
}

void write_fstab_root(char *root_path, FILE *file)
{
    RootInfo *info = get_root_info_for_path(root_path);
    if (info == NULL) {
        LOGW("Unable to get root info for %s during fstab generation!", root_path);
        return;
    }
    MtdPartition *mtd = get_root_mtd_partition(root_path);
    if (mtd != NULL)
    {
        fprintf(file, "/dev/block/mtdblock%d ", mtd->device_index);
    }
    else
    {
        fprintf(file, "%s ", info->device);
    }
    
    fprintf(file, "%s ", info->mount_point);
    fprintf(file, "%s %s\n", info->filesystem, info->filesystem_options == NULL ? "rw" : info->filesystem_options); 
}

void create_fstab()
{
    __system("touch /etc/mtab");
    FILE *file = fopen("/etc/fstab", "w");
    if (file == NULL) {
        LOGW("Unable to create /etc/fstab!");
        return;
    }
    write_fstab_root("CACHE:", file);
    write_fstab_root("DATA:", file);
#ifdef HAS_DATADATA
    write_fstab_root("DATADATA:", file);
#endif
    write_fstab_root("SYSTEM:", file);
    write_fstab_root("SDCARD:", file);
    write_fstab_root("SDEXT:", file);
    fclose(file);
}

void handle_failure(int ret)
{
    if (ret == 0)
        return;
    if (0 != ensure_root_path_mounted("SDCARD:"))
        return;
    mkdir("/sdcard/clockworkmod", S_IRWXU);
    __system("cp /tmp/recovery.log /sdcard/clockworkmod/recovery.log");
    ui_print("/tmp/recovery.log was copied to /sdcard/clockworkmod/recovery.log. Please quote it on the forum.\n");
}
