/*
 * Round Dashboard with Navigation, Music Player, and Call Management
 * Resolution: 1080x1080 round screen
 * Integrates: nav_logger.c, album_art.c, bt_player.c, hfp_calls.c
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <dbus/dbus.h>
#include <json-c/json.h>

#include "lvgl/lvgl.h"
#include "lv_conf.h"
#include "hal/hal.h"
#include <SDL.h>
#include "speedo_interactive.h"

/* ========== CONFIGURATION ========== */
#define SCREEN_W                1080
#define SCREEN_H                1080
#define MUSIC_COVER_PATH_FS    "/home/pi5/dashboard/cover.jpg"         /* Real filesystem path */
#define MUSIC_COVER_PATH       "A:/home/pi5/dashboard/cover.jpg"        /* LVGL path format */
#define CENTER_RADIUS          250
#define BOTTOM_PLAYER_HEIGHT   250

/* ========== DATA STRUCTURES ========== */

typedef struct {
    char instruction[256];      /* Turn instruction */
    double distance;            /* Distance in meters */
    double next_distance;       /* Distance to next maneuver */
    char street_name[256];      /* Current street */
    double speed;               /* Current speed km/h */
    double lat;                 /* Current latitude */
    double lon;                 /* Current longitude */
} nav_info_t;

typedef struct {
    char title[256];
    char artist[256];
    char album[256];
    char status[32];            /* "playing", "paused", "stopped" */
    unsigned int position_ms;
    unsigned int duration_ms;
    char cover_path[512];
} music_info_t;

typedef struct {
    char state[32];             /* "active", "incoming", "dialing", etc */
    char line_id[64];           /* Phone number */
    char name[64];              /* Contact name */
    char call_path[256];        /* DBus object path */
} call_info_t;

typedef struct {
    nav_info_t nav;
    music_info_t music;
    call_info_t current_call;
    int has_incoming_call;
    pthread_mutex_t lock;
} app_state_t;

static app_state_t g_state = {0};

/* Album art helper (album_art.c) */
extern int download_itunes_cover(const char *artist, const char *album, const char *out_path);

/* ========== UI OBJECTS ========== */

static lv_obj_t *g_scr_main = NULL;
static lv_obj_t *g_nav_label = NULL;
static lv_obj_t *g_nav_dist_label = NULL;
static lv_obj_t *g_nav_street_label = NULL;
static lv_obj_t *g_music_img = NULL;
static lv_obj_t *g_music_label = NULL;
static lv_obj_t *g_music_artist_label = NULL;
static lv_obj_t *g_btn_prev = NULL;
static lv_obj_t *g_btn_play = NULL;
static lv_obj_t *g_btn_next = NULL;
static lv_obj_t *g_call_panel = NULL;
static lv_obj_t *g_call_number_label = NULL;
static lv_obj_t *g_btn_answer = NULL;
static lv_obj_t *g_btn_hangup = NULL;

/* ========== NAVIGATION PARSING ========== */

static int parse_nav_json(const char *json_line, nav_info_t *nav)
{
    json_object *obj = json_tokener_parse(json_line);
    if (!obj) return 0;

    json_object *tmp;
    
    if (json_object_object_get_ex(obj, "next_action", &tmp)) {
        strncpy(nav->instruction, json_object_get_string(tmp), sizeof(nav->instruction) - 1);
    }
    
    if (json_object_object_get_ex(obj, "distance_to_next_m", &tmp)) {
        nav->distance = json_object_get_double(tmp);
    }
    
    if (json_object_object_get_ex(obj, "distance_to_next_m", &tmp)) {
        nav->next_distance = json_object_get_double(tmp);
    }
    
    if (json_object_object_get_ex(obj, "street_name", &tmp)) {
        strncpy(nav->street_name, json_object_get_string(tmp), sizeof(nav->street_name) - 1);
    } else {
        nav->street_name[0] = '\0';
    }
    
    if (json_object_object_get_ex(obj, "speed_kmh", &tmp)) {
        nav->speed = json_object_get_double(tmp);
    } else {
        nav->speed = 0;
    }
    
    if (json_object_object_get_ex(obj, "lat", &tmp)) {
        nav->lat = json_object_get_double(tmp);
    }
    
    if (json_object_object_get_ex(obj, "lon", &tmp)) {
        nav->lon = json_object_get_double(tmp);
    }
    
    json_object_put(obj);
    return 1;
}

/* ========== NAVIGATION THREAD ========== */

static void *nav_thread_fn(void *arg)
{
    char line[2048];
    int fd = -1;
    FILE *f = NULL;

    while (1) {
        /* Open rfcomm device if not open */
        if (fd < 0) {
            fd = open("/dev/rfcomm1", O_RDONLY | O_NOCTTY);
            if (fd < 0) {
                fprintf(stderr, "Cannot open /dev/rfcomm1 (waiting...)\n");
                sleep(2);
                continue;
            }
            f = fdopen(fd, "r");
            if (!f) {
                perror("fdopen");
                close(fd);
                fd = -1;
                sleep(2);
                continue;
            }
            fprintf(stderr, "Connected to /dev/rfcomm1\n");
        }

        /* Read lines from rfcomm device */
        if (fgets(line, sizeof(line), f)) {
            size_t len = strlen(line);
            while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
                line[--len] = '\0';
            }
            if (len == 0) continue;

            pthread_mutex_lock(&g_state.lock);
            if (!parse_nav_json(line, &g_state.nav)) {
                fprintf(stderr, "Failed to parse nav JSON: %s\n", line);
            } else {
                fprintf(stderr, "Nav updated: %.0f m, %s\n", g_state.nav.distance, g_state.nav.instruction);
            }
            pthread_mutex_unlock(&g_state.lock);
        } else {
            /* EOF or read error - reconnect */
            if (f) {
                fclose(f);  /* Also closes fd */
                f = NULL;
                fd = -1;
            }
            fprintf(stderr, "rfcomm1 disconnected, reconnecting...\n");
            sleep(2);
        }
    }

    if (f) fclose(f);
    if (fd >= 0) close(fd);
    return NULL;
}

/* ========== MUSIC PLAYER (DBus) ========== */

static DBusConnection *dbus_conn = NULL;
static char player_path[256] = {0};

static int dbus_connect_system(void)
{
    DBusError err;
    dbus_error_init(&err);
    dbus_conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "dbus_bus_get: %s\n", err.message);
        dbus_error_free(&err);
        return 0;
    }
    return dbus_conn != NULL;
}

static int find_player_path(void)
{
    if (!dbus_conn) return 0;

    DBusMessage *msg, *reply;
    DBusError err;
    DBusMessageIter args, dict_outer, dict_ifaces, entry_if, entry_prop;

    dbus_error_init(&err);
    msg = dbus_message_new_method_call(
        "org.bluez", "/", 
        "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
    if (!msg) return 0;

    reply = dbus_connection_send_with_reply_and_block(dbus_conn, msg, -1, &err);
    dbus_message_unref(msg);

    if (!reply) {
        fprintf(stderr, "GetManagedObjects failed: %s\n", err.message);
        dbus_error_free(&err);
        return 0;
    }

    if (!dbus_message_iter_init(reply, &args) ||
        dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_ARRAY) {
        dbus_message_unref(reply);
        return 0;
    }

    dbus_message_iter_recurse(&args, &dict_outer);

    while (dbus_message_iter_get_arg_type(&dict_outer) == DBUS_TYPE_DICT_ENTRY) {
        dbus_message_iter_recurse(&dict_outer, &entry_if);

        const char *obj_path = NULL;
        if (dbus_message_iter_get_arg_type(&entry_if) == DBUS_TYPE_OBJECT_PATH) {
            dbus_message_iter_get_basic(&entry_if, &obj_path);
        }
        dbus_message_iter_next(&entry_if);

        if (obj_path && dbus_message_iter_get_arg_type(&entry_if) == DBUS_TYPE_ARRAY) {
            dbus_message_iter_recurse(&entry_if, &dict_ifaces);

            int has_player = 0;
            while (dbus_message_iter_get_arg_type(&dict_ifaces) == DBUS_TYPE_DICT_ENTRY) {
                dbus_message_iter_recurse(&dict_ifaces, &entry_prop);
                const char *iname = NULL;
                if (dbus_message_iter_get_arg_type(&entry_prop) == DBUS_TYPE_STRING) {
                    dbus_message_iter_get_basic(&entry_prop, &iname);
                }
                if (iname && strcmp(iname, "org.bluez.MediaPlayer1") == 0) {
                    has_player = 1;
                    break;
                }
                dbus_message_iter_next(&dict_ifaces);
            }

            if (has_player) {
                strncpy(player_path, obj_path, sizeof(player_path) - 1);
                dbus_message_unref(reply);
                return 1;
            }
        }
        dbus_message_iter_next(&dict_outer);
    }

    dbus_message_unref(reply);
    return 0;
}

static int get_music_properties(void)
{
    if (!dbus_conn) return 0;

    DBusMessage *msg, *reply;
    DBusError err;
    DBusMessageIter args, dict_iter, entry_iter, var_iter, track_dict, te, tvar;

    if (!player_path[0]) return 0;

    dbus_error_init(&err);
    msg = dbus_message_new_method_call(
        "org.bluez", player_path,
        "org.freedesktop.DBus.Properties", "GetAll");
    if (!msg) return 0;

    const char *iface = "org.bluez.MediaPlayer1";
    if (!dbus_message_append_args(msg, DBUS_TYPE_STRING, &iface, DBUS_TYPE_INVALID)) {
        dbus_message_unref(msg);
        return 0;
    }

    reply = dbus_connection_send_with_reply_and_block(dbus_conn, msg, -1, &err);
    dbus_message_unref(msg);

    if (!reply) {
        fprintf(stderr, "GetAll failed: %s\n", err.message);
        dbus_error_free(&err);
        return 0;
    }

    if (!dbus_message_iter_init(reply, &args) ||
        dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_ARRAY) {
        dbus_message_unref(reply);
        return 0;
    }

    dbus_message_iter_recurse(&args, &dict_iter);

    while (dbus_message_iter_get_arg_type(&dict_iter) == DBUS_TYPE_DICT_ENTRY) {
        dbus_message_iter_recurse(&dict_iter, &entry_iter);
        const char *key = NULL;
        if (dbus_message_iter_get_arg_type(&entry_iter) == DBUS_TYPE_STRING) {
            dbus_message_iter_get_basic(&entry_iter, &key);
        }
        dbus_message_iter_next(&entry_iter);

        if (key && dbus_message_iter_get_arg_type(&entry_iter) == DBUS_TYPE_VARIANT) {
            dbus_message_iter_recurse(&entry_iter, &var_iter);
            int vtype = dbus_message_iter_get_arg_type(&var_iter);

            if (strcmp(key, "Status") == 0 && vtype == DBUS_TYPE_STRING) {
                const char *st = NULL;
                dbus_message_iter_get_basic(&var_iter, &st);
                if (st) strncpy(g_state.music.status, st, sizeof(g_state.music.status) - 1);
            }
            else if (strcmp(key, "Position") == 0 && vtype == DBUS_TYPE_UINT32) {
                dbus_message_iter_get_basic(&var_iter, &g_state.music.position_ms);
            }
            else if (strcmp(key, "Track") == 0 && vtype == DBUS_TYPE_ARRAY) {
                dbus_message_iter_recurse(&var_iter, &track_dict);

                while (dbus_message_iter_get_arg_type(&track_dict) == DBUS_TYPE_DICT_ENTRY) {
                    dbus_message_iter_recurse(&track_dict, &te);
                    const char *tkey = NULL;
                    if (dbus_message_iter_get_arg_type(&te) == DBUS_TYPE_STRING) {
                        dbus_message_iter_get_basic(&te, &tkey);
                    }
                    dbus_message_iter_next(&te);

                    if (tkey && dbus_message_iter_get_arg_type(&te) == DBUS_TYPE_VARIANT) {
                        dbus_message_iter_recurse(&te, &tvar);
                        int tvtype = dbus_message_iter_get_arg_type(&tvar);

                        if (strcmp(tkey, "Title") == 0 && tvtype == DBUS_TYPE_STRING) {
                            const char *t = NULL;
                            dbus_message_iter_get_basic(&tvar, &t);
                            if (t) strncpy(g_state.music.title, t, sizeof(g_state.music.title) - 1);
                        }
                        else if (strcmp(tkey, "Artist") == 0 && tvtype == DBUS_TYPE_STRING) {
                            const char *a = NULL;
                            dbus_message_iter_get_basic(&tvar, &a);
                            if (a) strncpy(g_state.music.artist, a, sizeof(g_state.music.artist) - 1);
                        }
                        else if (strcmp(tkey, "Album") == 0 && tvtype == DBUS_TYPE_STRING) {
                            const char *al = NULL;
                            dbus_message_iter_get_basic(&tvar, &al);
                            if (al) strncpy(g_state.music.album, al, sizeof(g_state.music.album) - 1);
                        }
                        else if (strcmp(tkey, "Duration") == 0 && tvtype == DBUS_TYPE_UINT32) {
                            dbus_message_iter_get_basic(&tvar, &g_state.music.duration_ms);
                        }
                    }
                    dbus_message_iter_next(&track_dict);
                }
            }
        }
        dbus_message_iter_next(&dict_iter);
    }

    dbus_message_unref(reply);
    return 1;
}

static int send_player_method(const char *method)
{
    if (!dbus_conn) return 0;

    DBusMessage *msg, *reply;
    DBusError err;

    if (!player_path[0]) return 0;

    dbus_error_init(&err);
    msg = dbus_message_new_method_call(
        "org.bluez", player_path,
        "org.bluez.MediaPlayer1", method);
    if (!msg) return 0;

    reply = dbus_connection_send_with_reply_and_block(dbus_conn, msg, 5000, &err);
    dbus_message_unref(msg);

    if (!reply) {
        fprintf(stderr, "%s failed: %s\n", method, err.message);
        dbus_error_free(&err);
        return 0;
    }

    dbus_message_unref(reply);
    return 1;
}

static void *music_thread_fn(void *arg)
{
    if (!dbus_connect_system()) {
        fprintf(stderr, "Failed to connect to DBus system bus for music player\n");
        /* Fallback: set demo music info */
        pthread_mutex_lock(&g_state.lock);
        strncpy(g_state.music.title, "No Bluetooth Player", sizeof(g_state.music.title) - 1);
        strncpy(g_state.music.artist, "Connect a device", sizeof(g_state.music.artist) - 1);
        strncpy(g_state.music.status, "stopped", sizeof(g_state.music.status) - 1);
        pthread_mutex_unlock(&g_state.lock);
        return NULL;
    }

    if (!find_player_path()) {
        fprintf(stderr, "No BlueZ MediaPlayer found - music controls will be disabled\n");
        /* Fallback message */
        pthread_mutex_lock(&g_state.lock);
        strncpy(g_state.music.title, "No MediaPlayer", sizeof(g_state.music.title) - 1);
        strncpy(g_state.music.artist, "Searching...", sizeof(g_state.music.artist) - 1);
        pthread_mutex_unlock(&g_state.lock);
    }

    char last_title[256] = "";
    char last_artist[256] = "";
    char last_album[256] = "";

    while (1) {
        pthread_mutex_lock(&g_state.lock);
        if (dbus_conn && player_path[0]) {
            if (get_music_properties()) {
                /* Always log current track info */
                fprintf(stderr, "Music: %s - %s\n", g_state.music.artist, g_state.music.title);
                
                /* Try to download cover if track changed */
                if (g_state.music.artist[0] && g_state.music.title[0]) {
                    if (strcmp(last_title, g_state.music.title) != 0 ||
                        strcmp(last_artist, g_state.music.artist) != 0) {
                        
                        strncpy(last_title, g_state.music.title, sizeof(last_title) - 1);
                        strncpy(last_artist, g_state.music.artist, sizeof(last_artist) - 1);
                        
                        fprintf(stderr, "Attempting to download cover for: %s - %s\n", 
                                g_state.music.artist, g_state.music.title);
                        
                        if (!download_itunes_cover(g_state.music.artist, g_state.music.title, MUSIC_COVER_PATH_FS)) {
                            fprintf(stderr, "Album art fetch failed for %s - %s\n", 
                                    g_state.music.artist, g_state.music.title);
                        } else {
                            fprintf(stderr, "Album art updated: %s - %s\n", 
                                    g_state.music.artist, g_state.music.title);
                        }
                    }
                }
            }
        }
        pthread_mutex_unlock(&g_state.lock);

        sleep(2);  /* Update every 2 seconds */
    }

    return NULL;
}

/* ========== CALLS (oFono/HFP) ========== */

static char modem_path[256] = {0};

static int find_modem_with_voicecall(void)
{
    if (!dbus_conn) return 0;

    DBusMessage *msg, *reply;
    DBusError err;
    DBusMessageIter args, arr, entry, iface_arr, iface_entry, iface_var, iface_inner;

    dbus_error_init(&err);
    msg = dbus_message_new_method_call(
        "org.ofono", "/",
        "org.ofono.Manager", "GetModems");
    if (!msg) return 0;

    reply = dbus_connection_send_with_reply_and_block(dbus_conn, msg, -1, &err);
    dbus_message_unref(msg);
    if (!reply) {
        fprintf(stderr, "GetModems failed: %s\n", err.message);
        dbus_error_free(&err);
        return 0;
    }

    if (!dbus_message_iter_init(reply, &args) ||
        dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_ARRAY) {
        dbus_message_unref(reply);
        return 0;
    }

    dbus_message_iter_recurse(&args, &arr);

    while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRUCT) {
        dbus_message_iter_recurse(&arr, &entry);

        const char *path = NULL;
        if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_OBJECT_PATH)
            dbus_message_iter_get_basic(&entry, &path);

        dbus_message_iter_next(&entry);

        if (path && dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_ARRAY) {
            dbus_message_iter_recurse(&entry, &iface_arr);
            int has_vcm = 0;

            while (dbus_message_iter_get_arg_type(&iface_arr) == DBUS_TYPE_DICT_ENTRY) {
                dbus_message_iter_recurse(&iface_arr, &iface_entry);
                const char *key = NULL;
                if (dbus_message_iter_get_arg_type(&iface_entry) == DBUS_TYPE_STRING)
                    dbus_message_iter_get_basic(&iface_entry, &key);
                dbus_message_iter_next(&iface_entry);

                if (key && strcmp(key, "Interfaces") == 0 &&
                    dbus_message_iter_get_arg_type(&iface_entry) == DBUS_TYPE_VARIANT) {

                    dbus_message_iter_recurse(&iface_entry, &iface_var);
                    if (dbus_message_iter_get_arg_type(&iface_var) == DBUS_TYPE_ARRAY) {
                        dbus_message_iter_recurse(&iface_var, &iface_inner);
                        while (dbus_message_iter_get_arg_type(&iface_inner) == DBUS_TYPE_STRING) {
                            const char *iname = NULL;
                            dbus_message_iter_get_basic(&iface_inner, &iname);
                            if (iname && strcmp(iname, "org.ofono.VoiceCallManager") == 0) {
                                has_vcm = 1;
                                break;
                            }
                            dbus_message_iter_next(&iface_inner);
                        }
                    }
                }
                dbus_message_iter_next(&iface_arr);
            }

            if (has_vcm) {
                strncpy(modem_path, path, sizeof(modem_path) - 1);
                dbus_message_unref(reply);
                return 1;
            }
        }
        dbus_message_iter_next(&arr);
    }

    dbus_message_unref(reply);
    return 0;
}

static int get_calls(call_info_t *calls, int max_calls)
{
    if (!dbus_conn) return 0;

    DBusMessage *msg, *reply;
    DBusError err;
    DBusMessageIter args, arr, entry, props, prop_entry, var_iter;
    int count = 0;

    if (!modem_path[0]) return 0;

    dbus_error_init(&err);
    msg = dbus_message_new_method_call(
        "org.ofono", modem_path,
        "org.ofono.VoiceCallManager", "GetCalls");
    if (!msg) return -1;

    reply = dbus_connection_send_with_reply_and_block(dbus_conn, msg, -1, &err);
    dbus_message_unref(msg);
    if (!reply) {
        fprintf(stderr, "GetCalls failed: %s\n", err.message);
        dbus_error_free(&err);
        return -1;
    }

    if (!dbus_message_iter_init(reply, &args) ||
        dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_ARRAY) {
        dbus_message_unref(reply);
        return -1;
    }

    dbus_message_iter_recurse(&args, &arr);

    while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRUCT && count < max_calls) {
        dbus_message_iter_recurse(&arr, &entry);

        const char *call_path = NULL;
        if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_OBJECT_PATH)
            dbus_message_iter_get_basic(&entry, &call_path);
        dbus_message_iter_next(&entry);

        if (call_path && dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_ARRAY) {
            strncpy(calls[count].call_path, call_path, sizeof(calls[count].call_path) - 1);
            dbus_message_iter_recurse(&entry, &props);

            while (dbus_message_iter_get_arg_type(&props) == DBUS_TYPE_DICT_ENTRY) {
                DBusMessageIter pe;
                const char *key = NULL;

                dbus_message_iter_recurse(&props, &pe);
                if (dbus_message_iter_get_arg_type(&pe) == DBUS_TYPE_STRING)
                    dbus_message_iter_get_basic(&pe, &key);
                dbus_message_iter_next(&pe);

                if (key && dbus_message_iter_get_arg_type(&pe) == DBUS_TYPE_VARIANT) {
                    dbus_message_iter_recurse(&pe, &var_iter);
                    int vtype = dbus_message_iter_get_arg_type(&var_iter);

                    if (strcmp(key, "State") == 0 && vtype == DBUS_TYPE_STRING) {
                        const char *s = NULL;
                        dbus_message_iter_get_basic(&var_iter, &s);
                        if (s) strncpy(calls[count].state, s, sizeof(calls[count].state) - 1);
                    } else if (strcmp(key, "LineIdentification") == 0 && vtype == DBUS_TYPE_STRING) {
                        const char *s = NULL;
                        dbus_message_iter_get_basic(&var_iter, &s);
                        if (s) strncpy(calls[count].line_id, s, sizeof(calls[count].line_id) - 1);
                    } else if (strcmp(key, "Name") == 0 && vtype == DBUS_TYPE_STRING) {
                        const char *s = NULL;
                        dbus_message_iter_get_basic(&var_iter, &s);
                        if (s) strncpy(calls[count].name, s, sizeof(calls[count].name) - 1);
                    }
                }
                dbus_message_iter_next(&props);
            }

            count++;
        }
        dbus_message_iter_next(&arr);
    }

    dbus_message_unref(reply);
    return count;
}

static int call_voicecall_method(const char *call_path, const char *method)
{
    if (!dbus_conn) return 0;

    DBusMessage *msg, *reply;
    DBusError err;

    dbus_error_init(&err);
    msg = dbus_message_new_method_call(
        "org.ofono", call_path,
        "org.ofono.VoiceCall", method);
    if (!msg) return 0;

    reply = dbus_connection_send_with_reply_and_block(dbus_conn, msg, 5000, &err);
    dbus_message_unref(msg);
    if (!reply) {
        fprintf(stderr, "%s failed: %s\n", method, err.message);
        dbus_error_free(&err);
        return 0;
    }

    dbus_message_unref(reply);
    return 1;
}

static void *calls_thread_fn(void *arg)
{
    if (!dbus_connect_system()) {
        fprintf(stderr, "Failed to connect to DBus system bus for calls\n");
        return NULL;
    }

    if (!find_modem_with_voicecall()) {
        fprintf(stderr, "No modem with VoiceCallManager found - call handling will be disabled\n");
    }

    call_info_t calls[8];
    while (1) {
        memset(calls, 0, sizeof(calls));
        int n = get_calls(calls, 8);

        pthread_mutex_lock(&g_state.lock);
        if (n > 0) {
            g_state.has_incoming_call = 0;
            for (int i = 0; i < n; i++) {
                if (strcmp(calls[i].state, "incoming") == 0 || 
                    strcmp(calls[i].state, "waiting") == 0) {
                    memcpy(&g_state.current_call, &calls[i], sizeof(call_info_t));
                    g_state.has_incoming_call = 1;
                    break;
                }
            }
        } else {
            g_state.has_incoming_call = 0;
        }
        pthread_mutex_unlock(&g_state.lock);

        sleep(2);  /* Check every 2 seconds */
    }

    return NULL;
}

/* ========== UI UPDATE FUNCTIONS ========== */

static void update_nav_display(void)
{
    pthread_mutex_lock(&g_state.lock);
    
    if (g_state.nav.instruction[0]) {
        lv_label_set_text(g_nav_label, g_state.nav.instruction);
    } else {
        lv_label_set_text(g_nav_label, "Waiting for route...");
    }

    static char dist_buf[64];
    if (g_state.nav.distance > 1000) {
        snprintf(dist_buf, sizeof(dist_buf), "%.1f km", g_state.nav.distance / 1000.0);
    } else {
        snprintf(dist_buf, sizeof(dist_buf), "%.0f m", g_state.nav.distance);
    }
    lv_label_set_text(g_nav_dist_label, dist_buf);

    if (g_state.nav.speed > 0) {
        static char speed_buf[64];
        snprintf(speed_buf, sizeof(speed_buf), "%.0f km/h", g_state.nav.speed);
        lv_label_set_text(g_nav_street_label, speed_buf);
    } else {
        lv_label_set_text(g_nav_street_label, "--");
    }

    if (g_state.nav.street_name[0]) {
        lv_label_set_text(g_nav_street_label, g_state.nav.street_name);
    } else {
        lv_label_set_text(g_nav_street_label, "Unknown Street");
    }

    pthread_mutex_unlock(&g_state.lock);
}

static void update_music_display(void)
{
    pthread_mutex_lock(&g_state.lock);

    if (g_state.music.title[0]) {
        lv_label_set_text(g_music_label, g_state.music.title);
    }

    if (g_state.music.artist[0]) {
        lv_label_set_text(g_music_artist_label, g_state.music.artist);
    }

    /* Update cover image if exists */
    struct stat st;
    if (stat(MUSIC_COVER_PATH_FS, &st) == 0) {
        lv_image_set_src(g_music_img, MUSIC_COVER_PATH);
    }

    /* Update play/pause button label */
    if (strcmp(g_state.music.status, "playing") == 0) {
        lv_obj_t *label_play = lv_obj_get_child(g_btn_play, 0);
        if (label_play) lv_label_set_text(label_play, "Pause");
    } else {
        lv_obj_t *label_play = lv_obj_get_child(g_btn_play, 0);
        if (label_play) lv_label_set_text(label_play, "Play");
    }

    pthread_mutex_unlock(&g_state.lock);
}

static void update_call_display(void)
{
    pthread_mutex_lock(&g_state.lock);

    if (g_state.has_incoming_call) {
        lv_obj_clear_flag(g_call_panel, LV_OBJ_FLAG_HIDDEN);
        
        static char call_text[256];
        snprintf(call_text, sizeof(call_text), "📞 %s\n%s",
                 g_state.current_call.name[0] ? g_state.current_call.name : "Unknown",
                 g_state.current_call.line_id);
        lv_label_set_text(g_call_number_label, call_text);
    } else {
        lv_obj_add_flag(g_call_panel, LV_OBJ_FLAG_HIDDEN);
    }

    pthread_mutex_unlock(&g_state.lock);
}

static void ui_update_timer(lv_timer_t *timer)
{
    update_nav_display();
    update_music_display();
    update_call_display();
    speedo_set_speed((int32_t)g_state.nav.speed);
}

/* ========== UI BUTTON CALLBACKS ========== */

static void on_btn_prev_clicked(lv_event_t *e)
{
    pthread_mutex_lock(&g_state.lock);
    send_player_method("Previous");
    pthread_mutex_unlock(&g_state.lock);
}

static void on_btn_play_clicked(lv_event_t *e)
{
    pthread_mutex_lock(&g_state.lock);
    if (strcmp(g_state.music.status, "playing") == 0) {
        send_player_method("Pause");
    } else {
        send_player_method("Play");
    }
    pthread_mutex_unlock(&g_state.lock);
}

static void on_btn_next_clicked(lv_event_t *e)
{
    pthread_mutex_lock(&g_state.lock);
    send_player_method("Next");
    pthread_mutex_unlock(&g_state.lock);
}

static void on_btn_answer_clicked(lv_event_t *e)
{
    pthread_mutex_lock(&g_state.lock);
    if (g_state.has_incoming_call) {
        call_voicecall_method(g_state.current_call.call_path, "Answer");
        g_state.has_incoming_call = 0;
    }
    pthread_mutex_unlock(&g_state.lock);
}

static void on_btn_hangup_clicked(lv_event_t *e)
{
    pthread_mutex_lock(&g_state.lock);
    if (g_state.has_incoming_call) {
        call_voicecall_method(g_state.current_call.call_path, "Hangup");
        g_state.has_incoming_call = 0;
    }
    pthread_mutex_unlock(&g_state.lock);
}

/* ========== UI CREATION ========== */

static void create_ui(void)
{
    g_scr_main = lv_screen_active();
    lv_obj_set_style_bg_color(g_scr_main, lv_color_hex(0x000000), 0);

    /* Create the speedometer */
    speedo_interactive_create();

    /* ===== NAVIGATION CENTER ===== */
    lv_obj_t *nav_container = lv_obj_create(g_scr_main);
    lv_obj_set_size(nav_container, CENTER_RADIUS * 2, CENTER_RADIUS * 2);
    lv_obj_align(nav_container, LV_ALIGN_CENTER, 0, 100);
    lv_obj_set_style_bg_color(nav_container, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_border_width(nav_container, 3, 0);
    lv_obj_set_style_border_color(nav_container, lv_color_hex(0x16213e), 0);
    lv_obj_set_style_radius(nav_container, CENTER_RADIUS, 0);

    g_nav_dist_label = lv_label_create(nav_container);
    lv_obj_align(g_nav_dist_label, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_text_color(g_nav_dist_label, lv_color_hex(0x00d4ff), 0);
    lv_obj_set_style_text_font(g_nav_dist_label, &lv_font_montserrat_48, 0);
    lv_label_set_text(g_nav_dist_label, "-- m");

    g_nav_label = lv_label_create(nav_container);
    lv_label_set_long_mode(g_nav_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_nav_label, CENTER_RADIUS * 2 - 60);
    lv_obj_align(g_nav_label, LV_ALIGN_CENTER, 0, 40);
    lv_obj_set_style_text_color(g_nav_label, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(g_nav_label, &lv_font_montserrat_22, 0);
    lv_label_set_text(g_nav_label, "No Navigation");

    g_nav_street_label = lv_label_create(nav_container);
    lv_obj_align(g_nav_street_label, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_width(g_nav_street_label, CENTER_RADIUS * 2 - 60);
    lv_label_set_long_mode(g_nav_street_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_color(g_nav_street_label, lv_color_hex(0xcccccc), 0);
    lv_obj_set_style_text_font(g_nav_street_label, &lv_font_montserrat_16, 0);
    lv_label_set_text(g_nav_street_label, "--");

    /* ===== MUSIC PLAYER (BOTTOM) ===== */
    lv_obj_t *music_container = lv_obj_create(g_scr_main);
    lv_obj_set_size(music_container, SCREEN_W - 250, BOTTOM_PLAYER_HEIGHT - 30);
    lv_obj_align(music_container, LV_ALIGN_BOTTOM_MID, 0, -150);
    lv_obj_set_style_bg_color(music_container, lv_color_hex(0x0f0f1e), 0);
    lv_obj_set_style_border_width(music_container, 1, 0);
    lv_obj_set_style_border_color(music_container, lv_color_hex(0x16213e), 0);
    lv_obj_set_style_pad_all(music_container, 10, 0);

    /* Album cover (left side) */
    g_music_img = lv_image_create(music_container);
    lv_obj_set_size(g_music_img, 100, 100);
    lv_obj_align(g_music_img, LV_ALIGN_LEFT_MID, 10, 0);
    lv_image_set_src(g_music_img, MUSIC_COVER_PATH);
    lv_obj_set_style_img_opa(g_music_img, LV_OPA_COVER, 0);

    /* Song info (center) */
    lv_obj_t *info_container = lv_obj_create(music_container);
    lv_obj_set_size(info_container, 560, 90);
    lv_obj_align(info_container, LV_ALIGN_LEFT_MID, 110, 0);
    lv_obj_set_style_bg_opa(info_container, LV_OPA_0, 0);
    lv_obj_set_style_border_width(info_container, 0, 0);

    g_music_label = lv_label_create(info_container);
    lv_label_set_long_mode(g_music_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_music_label, 580);
    lv_obj_align(g_music_label, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_text_color(g_music_label, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(g_music_label, &lv_font_montserrat_18, 0);
    lv_label_set_text(g_music_label, "No track");

    g_music_artist_label = lv_label_create(info_container);
    lv_obj_align(g_music_artist_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_text_color(g_music_artist_label, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(g_music_artist_label, &lv_font_montserrat_14, 0);
    lv_label_set_text(g_music_artist_label, "Unknown Artist");

    /* Music controls (right side) */
    g_btn_prev = lv_button_create(music_container);
    lv_obj_set_size(g_btn_prev, 50, 50);
    lv_obj_align(g_btn_prev, LV_ALIGN_RIGHT_MID, -180, 0);
    lv_obj_add_event_cb(g_btn_prev, on_btn_prev_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label_prev = lv_label_create(g_btn_prev);
    lv_label_set_text(label_prev, "Prev");
    lv_obj_center(label_prev);

    g_btn_play = lv_button_create(music_container);
    lv_obj_set_size(g_btn_play, 50, 50);
    lv_obj_align(g_btn_play, LV_ALIGN_RIGHT_MID, -90, 0);
    lv_obj_add_event_cb(g_btn_play, on_btn_play_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label_play = lv_label_create(g_btn_play);
    lv_label_set_text(label_play, "Play");
    lv_obj_center(label_play);

    g_btn_next = lv_button_create(music_container);
    lv_obj_set_size(g_btn_next, 50, 50);
    lv_obj_align(g_btn_next, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(g_btn_next, on_btn_next_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label_next = lv_label_create(g_btn_next);
    lv_label_set_text(label_next, "Next");
    lv_obj_center(label_next);

    /* ===== CALL OVERLAY (hidden by default) ===== */
    g_call_panel = lv_obj_create(g_scr_main);
    lv_obj_set_size(g_call_panel, 600, 250);
    lv_obj_align(g_call_panel, LV_ALIGN_BOTTOM_MID, 0, -150);
    lv_obj_set_style_bg_color(g_call_panel, lv_color_hex(0x393a4f), 0);
    lv_obj_set_style_border_width(g_call_panel, 3, 0);
    lv_obj_set_style_border_color(g_call_panel, lv_color_hex(0xff6b6b), 0);
    lv_obj_set_style_pad_all(g_call_panel, 20, 0);
    lv_obj_add_flag(g_call_panel, LV_OBJ_FLAG_HIDDEN);

    g_call_number_label = lv_label_create(g_call_panel);
    lv_label_set_long_mode(g_call_number_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_call_number_label, 560);
    lv_obj_align(g_call_number_label, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_text_color(g_call_number_label, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(g_call_number_label, &lv_font_montserrat_24, 0);
    lv_label_set_text(g_call_number_label, "Incoming Call");

    g_btn_answer = lv_button_create(g_call_panel);
    lv_obj_set_size(g_btn_answer, 100, 60);
    lv_obj_align(g_btn_answer, LV_ALIGN_BOTTOM_LEFT, 30, -20);
    lv_obj_add_event_cb(g_btn_answer, on_btn_answer_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_bg_color(g_btn_answer, lv_color_hex(0x2ecc71), 0);
    lv_obj_t *label_answer = lv_label_create(g_btn_answer);
    lv_label_set_text(label_answer, "✓ Answer");
    lv_obj_center(label_answer);

    g_btn_hangup = lv_button_create(g_call_panel);
    lv_obj_set_size(g_btn_hangup, 100, 60);
    lv_obj_align(g_btn_hangup, LV_ALIGN_BOTTOM_RIGHT, -30, -20);
    lv_obj_add_event_cb(g_btn_hangup, on_btn_hangup_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_bg_color(g_btn_hangup, lv_color_hex(0xe74c3c), 0);
    lv_obj_t *label_hangup = lv_label_create(g_btn_hangup);
    lv_label_set_text(label_hangup, "✕ Reject");
    lv_obj_center(label_hangup);
}

/* ========== MAIN ========== */

int main(int argc, char **argv)
{
    /* Initialize LVGL */
    lv_init();

    /* Initialize random for nav demo */
    srand(time(NULL));

    /* Initialize filesystem for image loading */
    lv_fs_posix_init();

    /* Create display */
    lv_display_t *disp = sdl_hal_init(SCREEN_W, SCREEN_H);
    if (!disp) {
        fprintf(stderr, "Failed to initialize display\n");
        return 1;
    }

    printf("=== Round Dashboard (1080x1080) ===\n");
    printf("Navigation | Music Player | Calls\n");

    /* Initialize mutex */
    pthread_mutex_init(&g_state.lock, NULL);

    /* Create UI */
    create_ui();

    /* Start background threads */
    pthread_t nav_thread, music_thread, calls_thread;

    if (pthread_create(&nav_thread, NULL, nav_thread_fn, NULL) != 0) {
        fprintf(stderr, "Failed to create nav thread\n");
    }

    if (pthread_create(&music_thread, NULL, music_thread_fn, NULL) != 0) {
        fprintf(stderr, "Failed to create music thread\n");
    }

    if (pthread_create(&calls_thread, NULL, calls_thread_fn, NULL) != 0) {
        fprintf(stderr, "Failed to create calls thread\n");
    }

    /* Create timer for UI updates */
    lv_timer_create(ui_update_timer, 100, NULL);

    /* Main loop */
    while (1) {
        uint32_t time_till_next = lv_timer_handler();
        usleep(time_till_next * 1000);  /* Convert ms to us */
    }

    return 0;
}
