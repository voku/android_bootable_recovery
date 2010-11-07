/*
 * Copyright (C) 2007 The Android Open Source Project
 * Copyright (c) 2010, Code Aurora Forum. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

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
#include <sys/stat.h>
#include <sys/wait.h>

#include "bootloader.h"
#include "common.h"
#include "cutils/properties.h"
#include "install.h"
#include "minui/minui.h"
#include "minzip/DirUtil.h"
#include "roots.h"
#include "recovery_ui.h"

#include "extendedcommands.h"
#include "commands.h"

static const struct option OPTIONS[] = {
  { "send_intent", required_argument, NULL, 's' },
  { "update_package", required_argument, NULL, 'u' },
  { "wipe_data", no_argument, NULL, 'w' },
  { "wipe_cache", no_argument, NULL, 'c' },
  { NULL, 0, NULL, 0 },
};

static int allow_display_toggle = 1;
static int do_reboot = 1;
static int reboot_method = 1;
static int multi = 0;
char os[50];

static const char *COMMAND_FILE = "CACHE:recovery/command";
static const char *INTENT_FILE = "CACHE:recovery/intent";
static const char *LOG_FILE = "CACHE:recovery/log";
static const char *SDCARD_PACKAGE_FILE = "SDCARD:update.zip";
static const char *TEMPORARY_LOG_FILE = "/tmp/recovery.log";

/*
 * The recovery tool communicates with the main system through /cache files.
 *   /cache/recovery/command - INPUT - command line for tool, one arg per line
 *   /cache/recovery/log - OUTPUT - combined log file from recovery run(s)
 *   /cache/recovery/intent - OUTPUT - intent that was passed in
 *
 * The arguments which may be supplied in the recovery.command file:
 *   --send_intent=anystring - write the text out to recovery.intent
 *   --update_package=root:path - verify install an OTA package file
 *   --wipe_data - erase user data (and cache), then reboot
 *   --wipe_cache - wipe cache (but not user data), then reboot
 *   --set_encrypted_filesystem=on|off - enables / diasables encrypted fs
 *
 * After completing, we remove /cache/recovery/command and reboot.
 * Arguments may also be supplied in the bootloader control block (BCB).
 * These important scenarios must be safely restartable at any point:
 *
 * FACTORY RESET
 * 1. user selects "factory reset"
 * 2. main system writes "--wipe_data" to /cache/recovery/command
 * 3. main system reboots into recovery
 * 4. get_args() writes BCB with "boot-recovery" and "--wipe_data"
 *    -- after this, rebooting will restart the erase --
 * 5. erase_root() reformats /data
 * 6. erase_root() reformats /cache
 * 7. finish_recovery() erases BCB
 *    -- after this, rebooting will restart the main system --
 * 8. main() calls reboot() to boot main system
 *
 * OTA INSTALL
 * 1. main system downloads OTA package to /cache/some-filename.zip
 * 2. main system writes "--update_package=CACHE:some-filename.zip"
 * 3. main system reboots into recovery
 * 4. get_args() writes BCB with "boot-recovery" and "--update_package=..."
 *    -- after this, rebooting will attempt to reinstall the update --
 * 5. install_package() attempts to install the update
 *    NOTE: the package install must itself be restartable from any point
 * 6. finish_recovery() erases BCB
 *    -- after this, rebooting will (try to) restart the main system --
 * 7. ** if install failed **
 *    7a. prompt_and_wait() shows an error icon and waits for the user
 *    7b; the user reboots (pulling the battery, etc) into the main system
 * 8. main() calls maybe_install_firmware_update()
 *    ** if the update contained radio/hboot firmware **:
 *    8a. m_i_f_u() writes BCB with "boot-recovery" and "--wipe_cache"
 *        -- after this, rebooting will reformat cache & restart main system --
 *    8b. m_i_f_u() writes firmware image into raw cache partition
 *    8c. m_i_f_u() writes BCB with "update-radio/hboot" and "--wipe_cache"
 *        -- after this, rebooting will attempt to reinstall firmware --
 *    8d. bootloader tries to flash firmware
 *    8e. bootloader writes BCB with "boot-recovery" (keeping "--wipe_cache")
 *        -- after this, rebooting will reformat cache & restart main system --
 *    8f. erase_root() reformats /cache
 *    8g. finish_recovery() erases BCB
 *        -- after this, rebooting will (try to) restart the main system --
 * 9. main() calls reboot() to boot main system
 *
 * ENCRYPTED FILE SYSTEMS ENABLE/DISABLE
 * 1. user selects "enable encrypted file systems"
 * 2. main system writes "--set_encrypted_filesystem=on|off" to
 *    /cache/recovery/command
 * 3. main system reboots into recovery
 * 4. get_args() writes BCB with "boot-recovery" and
 *    "--set_encrypted_filesystems=on|off"
 *    -- after this, rebooting will restart the transition --
 * 5. read_encrypted_fs_info() retrieves encrypted file systems settings from /data
 *    Settings include: property to specify the Encrypted FS istatus and
 *    FS encryption key if enabled (not yet implemented)
 * 6. erase_root() reformats /data
 * 7. erase_root() reformats /cache
 * 8. restore_encrypted_fs_info() writes required encrypted file systems settings to /data
 *    Settings include: property to specify the Encrypted FS status and
 *    FS encryption key if enabled (not yet implemented)
 * 9. finish_recovery() erases BCB
 *    -- after this, rebooting will restart the main system --
 * 10. main() calls reboot() to boot main system
 */

static const int MAX_ARG_LENGTH = 4096;
static const int MAX_ARGS = 100;

// open a file given in root:path format, mounting partitions as necessary
static FILE*
fopen_root_path(const char *root_path, const char *mode) {
    if (ensure_root_path_mounted(root_path) != 0) {
        LOGE("Can't mount %s\n", root_path);
        return NULL;
    }

    char path[PATH_MAX] = "";
    if (translate_root_path(root_path, path, sizeof(path)) == NULL) {
        LOGE("Bad path %s\n", root_path);
        return NULL;
    }

    // When writing, try to create the containing directory, if necessary.
    // Use generous permissions, the system (init.rc) will reset them.
    if (strchr("wa", mode[0])) dirCreateHierarchy(path, 0777, NULL, 1);

    FILE *fp = fopen(path, mode);
    if (fp == NULL && root_path != COMMAND_FILE) LOGE("Can't open %s\n", path);
    return fp;
}

// close a file, log an error if the error indicator is set
static void
check_and_fclose(FILE *fp, const char *name) {
    fflush(fp);
    if (ferror(fp)) LOGE("Error in %s\n(%s)\n", name, strerror(errno));
    fclose(fp);
}

// command line args come from, in decreasing precedence:
//   - the actual command line
//   - the bootloader control block (one per line, after "recovery")
//   - the contents of COMMAND_FILE (one per line)
static void
get_args(int *argc, char ***argv) {
    struct bootloader_message boot;
    memset(&boot, 0, sizeof(boot));
#ifndef BOARD_HAS_NO_MISC_PARTITION
    get_bootloader_message(&boot);  // this may fail, leaving a zeroed structure
#endif

    if (boot.command[0] != 0 && boot.command[0] != 255) {
        LOGI("Boot command: %.*s\n", sizeof(boot.command), boot.command);
    }

    if (boot.status[0] != 0 && boot.status[0] != 255) {
        LOGI("Boot status: %.*s\n", sizeof(boot.status), boot.status);
    }

    struct stat file_info;

    // --- if arguments weren't supplied, look in the bootloader control block
    if (*argc <= 1 && 0 != stat("/tmp/.ignorebootmessage", &file_info)) {
        boot.recovery[sizeof(boot.recovery) - 1] = '\0';  // Ensure termination
        const char *arg = strtok(boot.recovery, "\n");
        if (arg != NULL && !strcmp(arg, "recovery")) {
            *argv = (char **) malloc(sizeof(char *) * MAX_ARGS);
            (*argv)[0] = strdup(arg);
            for (*argc = 1; *argc < MAX_ARGS; ++*argc) {
                if ((arg = strtok(NULL, "\n")) == NULL) break;
                (*argv)[*argc] = strdup(arg);
            }
            LOGI("Got arguments from boot message\n");
        } else if (boot.recovery[0] != 0 && boot.recovery[0] != 255) {
            LOGE("Bad boot message\n\"%.20s\"\n", boot.recovery);
        }
    }

    // --- if that doesn't work, try the command file
    if (*argc <= 1) {
        FILE *fp = fopen_root_path(COMMAND_FILE, "r");
        if (fp != NULL) {
            char *argv0 = (*argv)[0];
            *argv = (char **) malloc(sizeof(char *) * MAX_ARGS);
            (*argv)[0] = argv0;  // use the same program name

            char buf[MAX_ARG_LENGTH];
            for (*argc = 1; *argc < MAX_ARGS; ++*argc) {
                if (!fgets(buf, sizeof(buf), fp)) break;
                (*argv)[*argc] = strdup(strtok(buf, "\r\n"));  // Strip newline.
            }

            check_and_fclose(fp, COMMAND_FILE);
            LOGI("Got arguments from %s\n", COMMAND_FILE);
        }
    }

    // --> write the arguments we have back into the bootloader control block
    // always boot into recovery after this (until finish_recovery() is called)
    strlcpy(boot.command, "boot-recovery", sizeof(boot.command));
    strlcpy(boot.recovery, "recovery\n", sizeof(boot.recovery));
    int i;
    for (i = 1; i < *argc; ++i) {
        strlcat(boot.recovery, (*argv)[i], sizeof(boot.recovery));
        strlcat(boot.recovery, "\n", sizeof(boot.recovery));
    }
#ifndef BOARD_HAS_NO_MISC_PARTITION
    set_bootloader_message(&boot);
#endif
}

#ifndef BOARD_HAS_NO_MISC_PARTITION
void
set_sdcard_update_bootloader_message() {
    struct bootloader_message boot;
    memset(&boot, 0, sizeof(boot));
    strlcpy(boot.command, "boot-recovery", sizeof(boot.command));
    strlcpy(boot.recovery, "recovery\n", sizeof(boot.recovery));
    set_bootloader_message(&boot);
}
#endif

// clear the recovery command and prepare to boot a (hopefully working) system,
// copy our log file to cache as well (for the system to read), and
// record any intent we were asked to communicate back to the system.
// this function is idempotent: call it as many times as you like.
static void
finish_recovery(const char *send_intent) {
    // By this point, we're ready to return to the main system...
    if (send_intent != NULL) {
        FILE *fp = fopen_root_path(INTENT_FILE, "w");
        if (fp == NULL) {
            LOGE("Can't open %s\n", INTENT_FILE);
        } else {
            fputs(send_intent, fp);
            check_and_fclose(fp, INTENT_FILE);
        }
    }

    // Copy logs to cache so the system can find out what happened.
    FILE *log = fopen_root_path(LOG_FILE, "a");
    if (log == NULL) {
        LOGE("Can't open %s\n", LOG_FILE);
    } else {
        FILE *tmplog = fopen(TEMPORARY_LOG_FILE, "r");
        if (tmplog == NULL) {
            LOGE("Can't open %s\n", TEMPORARY_LOG_FILE);
        } else {
            static long tmplog_offset = 0;
            fseek(tmplog, tmplog_offset, SEEK_SET);  // Since last write
            char buf[4096];
            while (fgets(buf, sizeof(buf), tmplog)) fputs(buf, log);
            tmplog_offset = ftell(tmplog);
            check_and_fclose(tmplog, TEMPORARY_LOG_FILE);
        }
        check_and_fclose(log, LOG_FILE);
    }

#ifndef BOARD_HAS_NO_MISC_PARTITION
    // Reset to mormal system boot so recovery won't cycle indefinitely.
    struct bootloader_message boot;
    memset(&boot, 0, sizeof(boot));
    set_bootloader_message(&boot);
#endif

    // Remove the command file, so recovery won't repeat indefinitely.
    char path[PATH_MAX] = "";
    if (ensure_root_path_mounted(COMMAND_FILE) != 0 ||
        translate_root_path(COMMAND_FILE, path, sizeof(path)) == NULL ||
        (unlink(path) && errno != ENOENT)) {
        LOGW("Can't unlink %s\n", COMMAND_FILE);
    }

    sync();  // For good measure.
}

static int
erase_root(const char *root) {
    ui_set_background(BACKGROUND_ICON_INSTALLING);
    ui_show_indeterminate_progress();
    ui_print("Formatting %s...\n", root);
    return format_root_device(root);
}

static char**
prepend_title(char** headers) {
	char* current;
	if ( strlen(os) ) {
		current=calloc(60,sizeof(char));
		strcpy(current,"     OS: ");
		strcat(current,os);
	} else current = "     OS: Internal";

	char* title[] = { EXPAND(RECOVERY_VERSION),
                      "",
                      current,
                      "",
                      NULL };
                      
    // count the number of lines in our title, plus the
    // caller-provided headers.
    int count = 0;
    char** p;
    for (p = title; *p; ++p, ++count);
    for (p = headers; *p; ++p, ++count);

    char** new_headers = malloc((count+1) * sizeof(char*));
    char** h = new_headers;
    for (p = title; *p; ++p, ++h) *h = *p;
    for (p = headers; *p; ++p, ++h) *h = *p;
    *h = NULL;

    return new_headers;
}

int
get_menu_selection(char** headers, char** items, int menu_only) {
    // throw away keys pressed previously, so user doesn't
    // accidentally trigger menu items.
    ui_clear_key_queue();

    int item_count = ui_start_menu(headers, items);
    int selected = 0;
    int chosen_item = -1;

    // Some users with dead enter keys need a way to turn on power to select.
    // Jiggering across the wrapping menu is one "secret" way to enable it.
    // We can't rely on /cache or /sdcard since they may not be available.
    int wrap_count = 0;

    while (chosen_item < 0 && chosen_item != GO_BACK) {
		//ui_menu_select(); //Why was this not here?!
        int key = ui_wait_key();
        int visible = ui_text_visible();

        int action = device_handle_key(key, visible);
        int old_selected = selected;

        if (action < 0) {
            switch (action) {
                case HIGHLIGHT_UP:
                    --selected;
                    selected = ui_menu_select(selected);
                    break;
                case HIGHLIGHT_DOWN:
                    ++selected;
                    selected = ui_menu_select(selected);
                    break;
                case SELECT_ITEM:
                    chosen_item = selected;
                    if (ui_get_showing_back_button()) {
                        if (chosen_item == item_count) {
                            chosen_item = GO_BACK;
                        }
                    }
                    break;
                case NO_ACTION:
                    break;
                case GO_BACK:
                    chosen_item = GO_BACK;
                    break;
            }
        } else if (!menu_only) {
            chosen_item = action;
        }

        if (abs(selected - old_selected) > 1) {
            wrap_count++;
            if (wrap_count == 3) {
                wrap_count = 0;
                if (ui_get_showing_back_button()) {
                    ui_print("Back menu button disabled.\n");
                    ui_set_showing_back_button(0);
                }
                else {
                    ui_print("Back menu button enabled.\n");
                    ui_set_showing_back_button(1);
                }
            }
        }
    }

    ui_end_menu();
    ui_clear_key_queue();
    return chosen_item;
}

static void
wipe_data(int confirm) {
    if (confirm) {
        static char** title_headers = NULL;

        if (title_headers == NULL) {
            char* headers[] = { "Confirm wipe of all user data?",
                                "  THIS CAN NOT BE UNDONE.",
                                "",
                                NULL };
            title_headers = prepend_title(headers);
        }

        char* items[] = { " No",
                          " No",
                          " No",
                          " No",
                          " No",
                          " No",
                          " No",
                          " Yes -- delete all user data",   // [7]
                          " No",
                          " No",
                          " No",
                          NULL };

        int chosen_item = get_menu_selection(title_headers, items, 1);
        if (chosen_item != 7) {
            return;
        }
    }

    ui_print("\n-- Wiping data...\n");
    device_wipe_data();
    erase_root("DATA:");
#ifdef HAS_DATADATA
    erase_root("DATADATA:");
#endif
    erase_root("CACHE:");
    erase_root("SDEXT:");
    erase_root("SDCARD:/.android_secure");
    ui_print("Data wipe complete.\n");
}

static void end_recovery() {
	static char* headers[] = { 	"Choose a method"
                                "",
                                "Use Up/Down and OK to select",
                                "",
                                NULL };

#define HALT_REBOOT		 	0
#define HALT_HALT			1
#define HALT_RECOVERY 		2

    static char* items[] = { 	"Reboot to system",
                                "Shut down",
                                "Reboot to recovery",
                                NULL };



    int chosen_item = -1;
    
    for (;;) {
        chosen_item=get_menu_selection(headers,items,0);

        if ( chosen_item == GO_BACK ) {
			do_reboot=0;
			reboot_method=1;
			return;
		}
        
        switch (chosen_item) {
			case HALT_REBOOT:
				do_reboot=1;
				reboot_method=1;
				return;
			case HALT_RECOVERY:
				do_reboot=1;
				reboot_method=2;
				return;
			case HALT_HALT:
				do_reboot=1;
				reboot_method=0;
				return;
			}
		}
}

static void init_os (char** items,int boot) {    
    if ( !strlen(os) ) {
				RootInfo* info=get_root_info_for_path("SYSTEM:");
				info->device="/dev/stl6";
				
				info=get_root_info_for_path("DATA:");
				info->device="/dev/stl5";
				return;
			}

	recheck();  //We should recheck the Filesystems.

	ensure_root_path_unmounted("SYSTEM:");
    ensure_root_path_unmounted("DATA:");
    ensure_root_path_mounted("SDCARD:");
    
    RootInfo* info=get_root_info_for_path("SYSTEM:");
    char* filename;
    filename=calloc(50,sizeof(char));
    sprintf(filename,"/sdcard/%s/system.img",os);
    info->device=filename;
    
    info=get_root_info_for_path("DATA:");
    filename=calloc(50,sizeof(char)); //Wasn't here before...big mistake, causing the two device to use the same image.
    sprintf(filename,"/sdcard/%s/data.img",os);
    info->device=filename;
    
    items[boot]=calloc(70,sizeof(char));
    strcpy(items[boot],"Boot ");
    strcat(items[boot],os);
    create_mknods(2);
}

void start_os() {
			if ( !strlen(os) ) {
				ui_print("You can't start internal os from here!\n");
				return;
			}
            ui_print("\nINIT New OS...");
            char* file_name;
            char* dir_name;
            FILE* f;
            int err;
          
                file_name = malloc(60 * sizeof(char));
                strcpy(file_name,"/sdcard/");
                strcat(file_name,os);
                dir_name = malloc( (strlen(file_name)+1)*sizeof(char) );
                strcpy(dir_name,file_name);
                strcat(file_name,"/init.sh");
            
			if (  ( f=fopen(file_name,"r") )  )	{
				fclose(f); 
				chdir(dir_name);
				char *args[] = {"/xbin/ash", file_name, NULL};          
				pid_t pid = fork();
				if (pid == 0) {
					execv("/xbin/ash", args);
					fprintf(stderr, "E:Can't run %s\n(%s)\n",file_name, strerror(errno));
					_exit(-1);
					}
				int status;

				while (waitpid(pid, &status, WNOHANG) == 0) {
					sleep(1);
					ui_print(".");
				}
				ui_print("done\nBooting New OS..\nPlease wait...");
				ui_end_menu();
				finish_recovery(NULL);
				args[0]=NULL;
				execv("/init_new", args);
				do_reboot=0;
			}
			else {
				ui_print("\n%s not exists!\n",file_name);
				err=1;
			}
			free(dir_name);
			free(file_name);
}

void show_wipe_menu() { //by LeshaK
	static char* headers[] = { 	"Choose what you want to wipe?"
                                "",
                                "Use Up/Down and OK to select",
                                "",
                                NULL };

#define WTYPE_DATA_CACHE 	0
#define WTYPE_CACHE	 		1
#define WTYPE_DELVIK_CACHE	2

    static char* items[] = { 	"Wipe data/cache (factory reset)",
                                "Wipe cache",
                                "Wipe dalvik-cache",
                                NULL };


    int selected = 0;
    int chosen_item = -1;

    finish_recovery(NULL);
    ui_reset_progress();
    for (;;) {
        chosen_item = get_menu_selection(headers,items,0);

		if (chosen_item == GO_BACK) break;

        if (chosen_item >= 0) {
            // turn off the menu, letting ui_print() to scroll output
            // on the screen.
            ui_end_menu();

            ui_print("\n-- This will ERASE your data!");
            ui_print("\n-- Press HOME to confirm, or");
            ui_print("\n-- any other key to abort..");
            int confirm_wipe = ui_wait_key();
            if (confirm_wipe == KEY_DREAM_HOME) {
              	ui_print("\nWiping data...\n");
                switch (chosen_item) {
                case WTYPE_DATA_CACHE:
                    erase_root("DATA:");
                case WTYPE_CACHE:
                    erase_root("CACHE:");
                    ui_print("Data wipe complete.\n");
                    break;
                case WTYPE_DELVIK_CACHE: {
                        if (ensure_root_path_mounted("DATA:") != 0) {
                            ui_print("Can't mount DATA\n");
                        } else {
                            ui_print("Formatting DATA:dalvik-cache..");
                            pid_t pid = fork();
                            if (pid == 0) {
                                char *args[] = {"/xbin/rm", "-r", "/data/dalvik-cache", NULL};
                                execv("/xbin/rm", args);
                                fprintf(stderr, "E:Can't wipe dalvik-cache\n(%s)\n", strerror(errno));
    	                        _exit(-1);
                            }

    	                    int status;

                            while (waitpid(pid, &status, WNOHANG) == 0) {
                                ui_print(".");
                                sleep(1);
    	                    }
                            ui_print("\n");

            	            if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
                                ui_print("Error wiping dalvik-cache.\n\n");
                            } else {
                                ui_print("Data wipe complete.\n");
                            }
                        }
                    }
                    break;
                }
            } else {
                ui_print("\nData wipe aborted.\n");
            }

            if (!ui_text_visible()) break;
            break;
        }

	
    }
}

static char password() {
	char* headers[] = { 	"   Password prompt by Xmister",
                                "   -- Samsung Spica i5700 --",
                                "",
                                "Use Up/Down and OK to select",
                                "",
                                "Type your password:",
                                NULL,
                                NULL };
    headers[6]=calloc(21,sizeof(char));
    char* list[] = { "OK", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "RESET", NULL };

	ensure_root_path_mounted("SYSTEM:");
	
    FILE* f=fopen("/system/.recovery_password","r");
    if ( f == NULL ) return 0;
	char* pass=calloc(50,sizeof(char));
	fgets(pass,49,f);
	fclose(f);
	int i=0;
	for(;;) {
		int chosen_item=get_menu_selection(headers,list,0);
		if ( chosen_item == 0 ) {
			if ( headers[6] != NULL && !strcmp( pass , headers[6] ) ) {
				return 0;
			}
			else {
				ui_print("Wrong password!\n");
				if ( headers[6] != NULL ) {
					headers[6][0]='\0';
					i=0;
				}
				continue;
			}
		}
		if ( chosen_item == 11 ) {
			if ( headers[6] != NULL ) {
				headers[6][0]='\0';
				i=0;
			}
		}
		else {
			if ( i>19 ) {
				ui_print("Maximum length reached!\n");
				continue;
			}
			sprintf( &(headers[6][i++]),"%c",(char)( ((int)'0')+chosen_item-1 ) );
		}
	}
		
}

static void
prompt_and_wait() {
    char** headers = prepend_title(MENU_HEADERS);
    char** items = MENU_ITEMS;
#ifdef DEBUG
	LOGE("Prompt\n");
#endif

	init_os(items,ITEM_CHOOSE_OS);  //Set the pointers to the actual device/image

	recheck();  //We should recheck the Filesystems.

	password();
	
	if ( multi ) items[ITEM_BACK] = "Choose another OS";
	else items[ITEM_BACK] = "Recheck Filesystems";

	/*FS INFO*/
    ui_print("%s Filesystems:\n",os);
		RootInfo * fst = (char *)get_root_info_for_path("SYSTEM:");
		ui_print(" SYSTEM:\t%s\n",fst->filesystem);
		fst = (char *)get_root_info_for_path("DATA:");
		ui_print(" DATA:\t%s\n",fst->filesystem);
		fst = (char *)get_root_info_for_path("CACHE:");
		ui_print(" CACHE:\t%s\n",fst->filesystem);
	ui_print("\n\n");
	
    for (;;) {
        finish_recovery(NULL);
        ui_reset_progress();

        allow_display_toggle = 1;
        int chosen_item = get_menu_selection(headers, MENU_ITEMS, 0);
#ifdef DEBUG
		ui_print("Selected: %d\n",chosen_item);
#endif
        allow_display_toggle = 0;

        // device-specific code may take some action here.  It may
        // return one of the core actions handled in the switch
        // statement below.
        chosen_item = device_perform_action(chosen_item);

        switch (chosen_item) {
            case ITEM_REBOOT:
                do_reboot=0;
				end_recovery();
				if (do_reboot) return;
                else break;

            case ITEM_WIPE:
                show_wipe_menu();
                break;

            case ITEM_APPLY_SDCARD:
                if (confirm_selection("Confirm install?", "Yes - Install /sdcard/update.zip"))
                {
                    ui_print("\n-- Install from sdcard...\n");
#ifndef BOARD_HAS_NO_MISC_PARTITION
                    set_sdcard_update_bootloader_message();
#endif
                    int status = install_package(SDCARD_PACKAGE_FILE);
                    if (status != INSTALL_SUCCESS) {
                        ui_set_background(BACKGROUND_ICON_ERROR);
                        ui_print("Installation aborted.\n");
                    } else if (!ui_text_visible()) {
                        return;  // reboot if logs aren't visible
                    } else {
#ifndef BOARD_HAS_NO_MISC_PARTITION
                        if (firmware_update_pending()) {
                            ui_print("\nReboot via menu to complete\n"
                                     "installation.\n");
                        } else {
                            ui_print("\nInstall from sdcard complete.\n");
                        }
#else
                        ui_print("\nInstall from sdcard complete.\n");
#endif
                    }
                }
                break;
            case ITEM_INSTALL_ZIP:
                show_install_update_menu();
                break;
            case ITEM_BACKUP:
                show_backup_menu();
                break;
            case ITEM_PARTITION:
                show_partition_menu();
                break;
            case ITEM_ADVANCED:
                show_advanced_menu();
                break;
            case ITEM_CHOOSE_OS:
                start_os();
                break;
            case ITEM_BACK:
				do_reboot=0;
				return;
        }
    }
}

static char
        pre_menu()
{
    static char* headers[] = { 	"     Boot loader by Xmister",
                                "   -- Samsung Spica i5700 --",
                                "",
                                "Use Up/Down and OK to select",
                                "",
                                "Choose a recovery:",
                                "",
                                NULL };

    // these constants correspond to elements of the items[] list.
#define ITEM_RECOVERY      0

	static char* list[20];
    int err;
    int init = 1;
    int chosen_item=-1;
	
	list[0]=NULL;
	
	//In case if something goes wrong
	ui_print("."); //9
	finish_recovery(NULL);
	
	
    for (;;) {
		if (init) {
			ui_print("."); //10
			if (ensure_root_path_mounted("SDCARD:")) {
				LOGE("BL: Cant' mount SDCARD\n");
				return 1;
			}
			//Give a chance to filesystems to unmount properly
			ensure_root_path_unmounted("SYSTEM:");
			ensure_root_path_unmounted("DATA:");
			ensure_root_path_unmounted("CACHE:");
			
			
			int i;
			for (i=0; list[i] != NULL; i++) {
				free(list[i]);
			}
			list[0]=NULL;
			
			ui_print("."); //11
			
			FILE* f = fopen("/sdcard/.bootlst", "r");
			if (f == NULL) {
				ui_print("\n");
				return 1;
			}
				list[ITEM_RECOVERY]="Start Internal";
				static char* prefix="Start ";
				i=ITEM_RECOVERY+1;
				while(!feof(f))
				{
					char* temp=calloc(50,sizeof(char));
					list[i]=malloc(50 * sizeof(char));
					fgets(temp,50,f);
					int j=0;
					for(j=0;j<50;j++) {
						if(temp[j] == '\n' || temp[j] == '\r') {
							temp[j]='\0';
							break;
						}
					}
					int x;
					for (x=0;x<i;x++)
						if (!strcmp(&(list[x][6]),temp)) break;
					if ( i == x ) {
						strcpy(list[i],prefix);
						strncpy(&(list[i][6]),temp,42);
						i++;
					}
				}
				list[i-1]=NULL;
				fclose(f);
			if ( i > ITEM_RECOVERY+1) multi=1;
			else {
				multi=0;
				return 1;
			}
			ui_print("."); //12
			//rest a bit, to let FS's to be detected and unmounted properly
			sleep(3);
			ui_print("."); //13
			int err=0;
			if (ensure_root_path_unmounted("SYSTEM:")) {
				LOGE("BL: Cant' unmount SYSTEM\n");
				err=1;
			}
			if (ensure_root_path_unmounted("DATA:")){
				LOGE("BL: Cant' unmount SYSTEM\n");
				err=1;
			}
			if (ensure_root_path_unmounted("CACHE:")){
				LOGE("BL: Cant' unmount SYSTEM\n");
				err=1;
			}
			ui_print("."); //14
			if (err) return err;
			ui_clear_key_queue();
			init=0;
			err = 0;
			
			//Write back the filtered list while the user chooses	
			pid_t pid = fork();
			if ( pid == 0 )	{
				f = fopen("/sdcard/.bootlst","w");
				if ( f != NULL ) {
					for (i=1; list[i] != NULL; i++ ) {
						fputs(&(list[i][6]),f);
						fputc('\n',f);
					}
					check_and_fclose(f,"/sdcard/.bootlst");
				}
				_exit(-1);
			}
			else {
				chosen_item = get_menu_selection(headers,list,0);
				ui_print(".\n"); //15
			}
		}

        if (chosen_item >= ITEM_RECOVERY) {
            // turn off the menu, letting ui_print() to scroll output
            // on the screen.
            if (chosen_item > ITEM_RECOVERY) strcpy(os,&(list[chosen_item][6]));
			else os[0]='\0';
			ui_end_menu();
			prompt_and_wait();
			if (do_reboot) return 0;
			else init=1;
		}
		else if ( chosen_item == GO_BACK ) {
			do_reboot=1;
			reboot_method=1;
			return 0;
		}
	}
return 1;
}

static void
print_property(const char *key, const char *name, void *cookie) {
    fprintf(stderr, "%s=%s\n", key, name);
}

int
main(int argc, char **argv) {
	if (strstr(argv[0], "recovery") == NULL)
	{
	    if (strstr(argv[0], "flash_image") != NULL)
	        return flash_image_main(argc, argv);
	    if (strstr(argv[0], "dump_image") != NULL)
	        return dump_image_main(argc, argv);
	    if (strstr(argv[0], "erase_image") != NULL)
	        return erase_image_main(argc, argv);
	    if (strstr(argv[0], "mkyaffs2image") != NULL)
	        return mkyaffs2image_main(argc, argv);
	    if (strstr(argv[0], "unyaffs") != NULL)
	        return unyaffs_main(argc, argv);
        if (strstr(argv[0], "amend"))
            return amend_main(argc, argv);
        if (strstr(argv[0], "nandroid"))
            return nandroid_main(argc, argv);
        if (strstr(argv[0], "reboot"))
            return reboot_main(argc, argv);
        if (strstr(argv[0], "setprop"))
            return setprop_main(argc, argv);
		return busybox_driver(argc, argv);
	}
    //create_fstab();
    
    int is_user_initiated_recovery = 0;
    time_t start = time(NULL);

#ifdef DEBUG
	LOGW("LOG\n");
#endif
    // If these fail, there's not really anywhere to complain...
    freopen(TEMPORARY_LOG_FILE, "a", stdout); setbuf(stdout, NULL);
#ifdef DEBUG
	LOGW(" 1\n");
#endif
    freopen(TEMPORARY_LOG_FILE, "a", stderr); setbuf(stderr, NULL);
#ifdef DEBUG
	LOGE(" 2\n");
#endif
    fprintf(stderr, "Starting recovery on %s", ctime(&start));
#ifdef DEBUG
	LOGW("UI\n");
#endif
    ui_init();
    ui_print(EXPAND(RECOVERY_VERSION)"\n");
    ui_print("Loading. Please wait...\n");
    ui_set_show_text(1);
#ifdef DEBUG
	LOGW("BOOT\n");
#endif
    get_args(&argc, &argv);
    ui_print("."); //4

    int previous_runs = 0;
    const char *send_intent = NULL;
    const char *update_package = NULL;
    int wipe_data = 0, wipe_cache = 0;

    int arg;
    while ((arg = getopt_long(argc, argv, "", OPTIONS, NULL)) != -1) {
        switch (arg) {
        case 'p': previous_runs = atoi(optarg); break;
        case 's': send_intent = optarg; break;
        case 'u': update_package = optarg; break;
        case 'w': wipe_data = wipe_cache = 1; break;
        case 'c': wipe_cache = 1; break;
        case '?':
            LOGE("Invalid command argument\n");
            continue;
        }
    }

	ui_print("."); //5
    device_recovery_start();

	ui_print("."); //6
    fprintf(stderr, "Command:");
    for (arg = 0; arg < argc; arg++) {
        fprintf(stderr, " \"%s\"", argv[arg]);
    }
    fprintf(stderr, "\n\n");

	ui_print("."); //7
    property_list(print_property, NULL);
    fprintf(stderr, "\n");

    int status = INSTALL_SUCCESS;

    ui_print("."); //8
    RecoveryCommandContext ctx = { NULL };
    if (register_update_commands(&ctx)) {
        LOGE("Can't install update commands\n");
    }

    if (update_package != NULL) {
        if (wipe_data && erase_root("DATA:")) status = INSTALL_ERROR;
        status = install_package(update_package);
        if (status != INSTALL_SUCCESS) ui_print("Installation aborted.\n");
    } else if (wipe_data) {
        if (device_wipe_data()) status = INSTALL_ERROR;
        if (erase_root("DATA:")) status = INSTALL_ERROR;
        if (wipe_cache && erase_root("CACHE:")) status = INSTALL_ERROR;
        if (status != INSTALL_SUCCESS) ui_print("Data wipe failed.\n");
    } else if (wipe_cache) {
        if (wipe_cache && erase_root("CACHE:")) status = INSTALL_ERROR;
        if (status != INSTALL_SUCCESS) ui_print("Cache wipe failed.\n");
    } else {
        LOGI("Checking for extendedcommand...\n");
        status = INSTALL_ERROR;  // No command specified
        // we are starting up in user initiated recovery here
        // let's set up some default options
        signature_check_enabled = 0;
        script_assert_enabled = 0;
        is_user_initiated_recovery = 1;
        ui_set_show_text(1);
        
        if (extendedcommand_file_exists()) {
            LOGI("Running extendedcommand...\n");
            int ret;
            if (0 == (ret = run_and_remove_extendedcommand())) {
                status = INSTALL_SUCCESS;
                ui_set_show_text(0);
            }
            else {
                handle_failure(ret);
            }
        } else {
            LOGI("Skipping execution of extendedcommand, file not found...\n");
        }
    }

	int ret=1; //We want recovery by default
    if (status != INSTALL_SUCCESS && !is_user_initiated_recovery) ui_set_background(BACKGROUND_ICON_ERROR);
    if (status != INSTALL_SUCCESS || ui_text_visible()) ret=pre_menu();

    /*On error, or if choosed, show recovery*/
    if ( ret ) prompt_and_wait();

#ifndef BOARD_HAS_NO_MISC_PARTITION
    // If there is a radio image pending, reboot now to install it.
    you_should_set_BOARD_HAS_NO_MISC_PARTITION_for_spica_in_BoardConfig.mk
    maybe_install_firmware_update(send_intent);
#endif

    // Otherwise, get ready to boot the main system...
    if (do_reboot) {
		sync();
		if ( reboot_method < 2 )finish_recovery(send_intent);
		else {
			ui_print("Rebooting to recovery...\n");
			__reboot(LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_RESTART2, "recovery");
			return EXIT_SUCCESS;
		}
			
		if (!reboot_method) {
			ui_print("Shutting down...\n");
			sync();
			reboot(RB_POWER_OFF);
		}
		else 
		{
			ui_print("Rebooting...\n");
			sync();
			reboot(RB_AUTOBOOT);
		}
	}
    return EXIT_SUCCESS;
}

int get_allow_toggle_display() {
    return allow_display_toggle;
}
