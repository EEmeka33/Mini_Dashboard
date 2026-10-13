/*
 * Round Dashboard with Navigation, Music Player, and Call Management
 * Resolution: 1080x1080 round screen
 *
 * v4 - changements principaux :
 *   - Arret propre : Ctrl+C, SIGTERM (systemctl stop), Echap ou fermeture de
 *     la fenetre terminent vraiment le processus (avant : SDL avalait le
 *     signal et la boucle while(1) tournait pour toujours).
 *   - Redecouverte automatique du lecteur BlueZ (MediaPlayer1) et du modem
 *     oFono : plus besoin de relancer le programme apres le premier
 *     appairage ou apres une deconnexion.
 *   - Aucun appel D-Bus / reseau bloquant n'est fait sous le verrou partage
 *     ni dans le thread UI -> l'UI ne gele plus.
 *   - Appairage via un agent BlueZ integre (bt_agent.c) : plus de popup du
 *     bureau Pi OS qui fait sortir du plein ecran.
 *   - La jaquette n'est rechargee que lorsqu'elle change (avant : decodage
 *     JPEG toutes les 100 ms).
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/stat.h>
#include <dbus/dbus.h>
#include <json-c/json.h>
#include <curl/curl.h>

#include "lvgl/lvgl.h"
#include "lvgl/src/misc/cache/lv_cache.h"   /* lv_image_cache_drop() */
#include "lv_conf.h"
#include "hal/hal.h"
#include <SDL.h>
#include "speedo_interactive.h"
#include "bt_agent.h"

/* ========== CONFIGURATION ========== */
#define SCREEN_W                1080
#define SCREEN_H                1080
#define MUSIC_COVER_PATH_FS    "/home/pi5/dashboard/cover.jpg"         /* Real filesystem path */
#define MUSIC_COVER_PATH       "A:/home/pi5/dashboard/cover.jpg"        /* LVGL path format */
#define CENTER_RADIUS          250
#define BOTTOM_PLAYER_HEIGHT   250
#define RFCOMM_DEV             "/dev/rfcomm1"
#define PHONE_MAC_FILE         "/tmp/connected_phone_mac.txt"
#define DBUS_TIMEOUT_MS        3000     /* jamais d'attente infinie */
#define REDISCOVER_PERIOD_S    3        /* recherche player / modem */

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
} music_info_t;

typedef struct {
    char state[32];             /* "active", "incoming", "dialing", etc */
    char line_id[64];           /* Phone number */
    char name[64];              /* Contact name */
    char call_path[256];        /* DBus object path */
} call_info_t;

/* Commandes envoyees par l'UI aux threads (executees hors thread UI) */
typedef enum { MEDIA_NONE = 0, MEDIA_PLAY, MEDIA_PAUSE, MEDIA_NEXT, MEDIA_PREV } media_cmd_t;
typedef enum { CALL_NONE = 0, CALL_ANSWER, CALL_HANGUP } call_cmd_t;

typedef struct {
    nav_info_t nav;
    int nav_connected;
    music_info_t music;
    unsigned int cover_version;  /* incremente a chaque nouvelle jaquette */
    call_info_t current_call;
    int has_incoming_call;
    media_cmd_t media_cmd;
    call_cmd_t call_cmd;
    char call_cmd_path[256];
    pthread_mutex_t lock;
} app_state_t;

static app_state_t g_state = { .lock = PTHREAD_MUTEX_INITIALIZER };

/* ========== ARRET PROPRE ========== */

static volatile sig_atomic_t g_quit = 0;

static inline int app_running(void)
{
    return !__atomic_load_n(&g_quit, __ATOMIC_ACQUIRE);
}

static void request_quit(void)
{
    __atomic_store_n(&g_quit, 1, __ATOMIC_RELEASE);
}

static void on_quit_signal(int sig)
{
    (void)sig;
    g_quit = 1;      /* async-signal-safe */
}

static void on_wake_signal(int sig)
{
    (void)sig;       /* sert juste a interrompre open()/poll() bloquants (EINTR) */
}

static void install_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);

    sa.sa_handler = on_quit_signal;     /* pas de SA_RESTART : on veut EINTR */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    sa.sa_handler = on_wake_signal;
    sigaction(SIGUSR1, &sa, NULL);

    signal(SIGPIPE, SIG_IGN);           /* socket coupee pendant un telechargement */
}

/* SDL : fermeture fenetre / SDL_QUIT / Echap -> on quitte.
 * On FILTRE ces evenements pour que LVGL ne detruise pas l'affichage sous
 * nos pieds pendant qu'un timer UI l'utilise encore. */
static int SDLCALL sdl_event_filter(void *ud, SDL_Event *ev)
{
    (void)ud;
    if (ev->type == SDL_QUIT ||
        (ev->type == SDL_WINDOWEVENT && ev->window.event == SDL_WINDOWEVENT_CLOSE)) {
        request_quit();
        return 0;   /* evenement supprime */
    }
    if (ev->type == SDL_KEYDOWN &&
        (ev->key.keysym.sym == SDLK_ESCAPE ||
         (ev->key.keysym.sym == SDLK_q && (ev->key.keysym.mod & KMOD_CTRL)))) {
        request_quit();
        return 0;
    }
    return 1;
}

/* Attente interruptible : se reveille si on quitte ou si *wake devient vrai */
static void sleep_ms_interruptible(int ms, const int *wake)
{
    while (ms > 0 && app_running()) {
        if (wake && __atomic_load_n(wake, __ATOMIC_ACQUIRE)) return;
        int step = ms > 50 ? 50 : ms;
        usleep(step * 1000);
        ms -= step;
    }
}

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
static lv_obj_t *g_btn_pair = NULL;
static lv_obj_t *g_pair_status_label = NULL;

/* N'appelle lv_label_set_text que si le texte change (evite les redessins inutiles) */
static void label_set_if_changed(lv_obj_t *label, const char *text)
{
    if (!label) return;
    const char *cur = lv_label_get_text(label);
    if (!cur || strcmp(cur, text) != 0) lv_label_set_text(label, text);
}

/* ========== D-BUS HELPERS ========== */

/* Chaque thread a SA connexion privee : pas de partage d'etat entre threads,
 * et un echec sur une connexion n'affecte pas les autres. */
static DBusConnection *dbus_open_private(const char *who)
{
    while (app_running()) {
        DBusError err;
        dbus_error_init(&err);
        DBusConnection *c = dbus_bus_get_private(DBUS_BUS_SYSTEM, &err);
        if (c) {
            dbus_connection_set_exit_on_disconnect(c, FALSE);
            return c;
        }
        fprintf(stderr, "[%s] system bus unavailable: %s\n", who, err.message);
        dbus_error_free(&err);
        sleep_ms_interruptible(2000, NULL);
    }
    return NULL;
}

static void dbus_close_private(DBusConnection *c)
{
    if (!c) return;
    dbus_connection_close(c);
    dbus_connection_unref(c);
}

static DBusMessage *dbus_call(DBusConnection *c, DBusMessage *msg, const char *what, int quiet)
{
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(c, msg, DBUS_TIMEOUT_MS, &err);
    dbus_message_unref(msg);
    if (!reply) {
        if (!quiet) fprintf(stderr, "%s failed: %s\n", what, err.message);
        dbus_error_free(&err);
    }
    return reply;
}

static void copy_str(char *dst, size_t len, const char *src)
{
    snprintf(dst, len, "%s", src ? src : "");
}

/* ========== NAVIGATION PARSING ========== */

static int parse_nav_json(const char *json_line, nav_info_t *nav)
{
    json_object *obj = json_tokener_parse(json_line);
    if (!obj) return 0;

    json_object *tmp;

    if (json_object_object_get_ex(obj, "next_action", &tmp))
        copy_str(nav->instruction, sizeof(nav->instruction), json_object_get_string(tmp));

    if (json_object_object_get_ex(obj, "distance_to_next_m", &tmp)) {
        nav->distance = json_object_get_double(tmp);
        nav->next_distance = nav->distance;
    }

    if (json_object_object_get_ex(obj, "street_name", &tmp))
        copy_str(nav->street_name, sizeof(nav->street_name), json_object_get_string(tmp));
    else
        nav->street_name[0] = '\0';

    nav->speed = json_object_object_get_ex(obj, "speed_kmh", &tmp) ? json_object_get_double(tmp) : 0;

    if (json_object_object_get_ex(obj, "lat", &tmp)) nav->lat = json_object_get_double(tmp);
    if (json_object_object_get_ex(obj, "lon", &tmp)) nav->lon = json_object_get_double(tmp);

    json_object_put(obj);
    return 1;
}

/* ========== NAVIGATION THREAD ========== */

static void nav_handle_line(char *line)
{
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' ')) line[--len] = '\0';
    if (len == 0) return;

    nav_info_t nav;
    pthread_mutex_lock(&g_state.lock);
    nav = g_state.nav;
    pthread_mutex_unlock(&g_state.lock);

    if (!parse_nav_json(line, &nav)) {
        fprintf(stderr, "Failed to parse nav JSON: %s\n", line);
        return;
    }

    pthread_mutex_lock(&g_state.lock);
    g_state.nav = nav;
    g_state.nav_connected = 1;
    pthread_mutex_unlock(&g_state.lock);
}

static void *nav_thread_fn(void *arg)
{
    (void)arg;
    char buf[4096];
    size_t used = 0;
    int fd = -1;
    int warned = 0;

    while (app_running()) {
        if (fd < 0) {
            /* open() bloque jusqu'a la connexion RFCOMM ; SIGUSR1 l'interrompt a l'arret */
            fd = open(RFCOMM_DEV, O_RDONLY | O_NOCTTY);
            if (fd < 0) {
                if (!warned) fprintf(stderr, "Cannot open %s: %s (waiting...)\n", RFCOMM_DEV, strerror(errno));
                warned = 1;
                sleep_ms_interruptible(2000, NULL);
                continue;
            }
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
            used = 0;
            warned = 0;
            fprintf(stderr, "Connected to %s\n", RFCOMM_DEV);
        }

        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 500);            /* timeout -> on reteste app_running() */
        if (pr == 0 || (pr < 0 && errno == EINTR)) continue;

        ssize_t n = -1;
        if (pr > 0 && (pfd.revents & POLLIN))
            n = read(fd, buf + used, sizeof(buf) - 1 - used);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;

        if (n <= 0) {   /* EOF, POLLHUP/POLLERR ou erreur : reconnexion */
            fprintf(stderr, "%s disconnected, reconnecting...\n", RFCOMM_DEV);
            close(fd);
            fd = -1;
            pthread_mutex_lock(&g_state.lock);
            g_state.nav_connected = 0;
            pthread_mutex_unlock(&g_state.lock);
            sleep_ms_interruptible(2000, NULL);
            continue;
        }

        used += (size_t)n;
        buf[used] = '\0';

        /* Decoupage en lignes JSON */
        char *start = buf, *nl;
        while ((nl = strchr(start, '\n')) != NULL) {
            *nl = '\0';
            nav_handle_line(start);
            start = nl + 1;
        }
        used = strlen(start);
        memmove(buf, start, used + 1);
        if (used >= sizeof(buf) - 1) used = 0;   /* ligne trop longue : on jette */
    }

    if (fd >= 0) close(fd);
    return NULL;
}

/* ========== BLUEZ OBJECT WALK (shared by MAC detection & player search) ========== */

/* Ecrit l'adresse du premier telephone connecte pour nav-bind-daemon.sh */
static void save_connected_phone_mac(DBusConnection *c)
{
    DBusMessage *msg = dbus_message_new_method_call("org.bluez", "/",
        "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
    if (!msg) return;
    DBusMessage *reply = dbus_call(c, msg, "GetManagedObjects", 1);
    if (!reply) return;

    DBusMessageIter args, objs;
    if (!dbus_message_iter_init(reply, &args) ||
        dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_ARRAY) {
        dbus_message_unref(reply);
        return;
    }

    static char last_saved[32] = "";
    dbus_message_iter_recurse(&args, &objs);
    while (dbus_message_iter_get_arg_type(&objs) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter obj, ifaces;
        dbus_message_iter_recurse(&objs, &obj);
        dbus_message_iter_next(&obj);                 /* saute le chemin */
        dbus_message_iter_recurse(&obj, &ifaces);

        while (dbus_message_iter_get_arg_type(&ifaces) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter ie, props;
            const char *iname = NULL;
            dbus_message_iter_recurse(&ifaces, &ie);
            dbus_message_iter_get_basic(&ie, &iname);
            dbus_message_iter_next(&ie);

            if (strcmp(iname, "org.bluez.Device1") == 0) {
                const char *address = NULL;
                dbus_bool_t connected = FALSE;
                dbus_message_iter_recurse(&ie, &props);
                while (dbus_message_iter_get_arg_type(&props) == DBUS_TYPE_DICT_ENTRY) {
                    DBusMessageIter pe, v;
                    const char *key = NULL;
                    dbus_message_iter_recurse(&props, &pe);
                    dbus_message_iter_get_basic(&pe, &key);
                    dbus_message_iter_next(&pe);
                    dbus_message_iter_recurse(&pe, &v);
                    int t = dbus_message_iter_get_arg_type(&v);
                    if (!strcmp(key, "Address") && t == DBUS_TYPE_STRING)
                        dbus_message_iter_get_basic(&v, &address);
                    else if (!strcmp(key, "Connected") && t == DBUS_TYPE_BOOLEAN)
                        dbus_message_iter_get_basic(&v, &connected);
                    dbus_message_iter_next(&props);
                }
                if (connected && address) {
                    if (strcmp(last_saved, address) != 0) {
                        /* ecriture atomique : le daemon ne lit jamais un fichier a moitie ecrit */
                        FILE *f = fopen(PHONE_MAC_FILE ".tmp", "w");
                        if (f) {
                            fprintf(f, "%s\n", address);
                            fclose(f);
                            rename(PHONE_MAC_FILE ".tmp", PHONE_MAC_FILE);
                            copy_str(last_saved, sizeof(last_saved), address);
                            fprintf(stderr, "[Bluetooth] Saved connected phone MAC: %s\n", address);
                        }
                    }
                    dbus_message_unref(reply);
                    return;
                }
            }
            dbus_message_iter_next(&ifaces);
        }
        dbus_message_iter_next(&objs);
    }
    dbus_message_unref(reply);
}

static int find_player_path(DBusConnection *c, char *out, size_t len)
{
    DBusMessage *msg = dbus_message_new_method_call("org.bluez", "/",
        "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
    if (!msg) return 0;
    DBusMessage *reply = dbus_call(c, msg, "GetManagedObjects", 1);
    if (!reply) return 0;

    int found = 0;
    DBusMessageIter args, objs;
    if (dbus_message_iter_init(reply, &args) &&
        dbus_message_iter_get_arg_type(&args) == DBUS_TYPE_ARRAY) {
        dbus_message_iter_recurse(&args, &objs);
        while (!found && dbus_message_iter_get_arg_type(&objs) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter obj, ifaces;
            const char *path = NULL;
            dbus_message_iter_recurse(&objs, &obj);
            dbus_message_iter_get_basic(&obj, &path);
            dbus_message_iter_next(&obj);
            dbus_message_iter_recurse(&obj, &ifaces);
            while (dbus_message_iter_get_arg_type(&ifaces) == DBUS_TYPE_DICT_ENTRY) {
                DBusMessageIter ie;
                const char *iname = NULL;
                dbus_message_iter_recurse(&ifaces, &ie);
                dbus_message_iter_get_basic(&ie, &iname);
                if (strcmp(iname, "org.bluez.MediaPlayer1") == 0) {
                    copy_str(out, len, path);
                    found = 1;
                    break;
                }
                dbus_message_iter_next(&ifaces);
            }
            dbus_message_iter_next(&objs);
        }
    }
    dbus_message_unref(reply);
    return found;
}

/* ========== MUSIC PLAYER (DBus) ========== */

/* Retourne 1 si OK, 0 si le player n'existe plus / erreur */
static int get_music_properties(DBusConnection *c, const char *path, music_info_t *m)
{
    DBusMessage *msg = dbus_message_new_method_call("org.bluez", path,
        "org.freedesktop.DBus.Properties", "GetAll");
    if (!msg) return 0;
    const char *iface = "org.bluez.MediaPlayer1";
    dbus_message_append_args(msg, DBUS_TYPE_STRING, &iface, DBUS_TYPE_INVALID);

    DBusMessage *reply = dbus_call(c, msg, "MediaPlayer1.GetAll", 1);
    if (!reply) return 0;

    DBusMessageIter args, dict;
    if (!dbus_message_iter_init(reply, &args) ||
        dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_ARRAY) {
        dbus_message_unref(reply);
        return 0;
    }

    dbus_message_iter_recurse(&args, &dict);
    while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter e, v;
        const char *key = NULL;
        dbus_message_iter_recurse(&dict, &e);
        dbus_message_iter_get_basic(&e, &key);
        dbus_message_iter_next(&e);
        dbus_message_iter_recurse(&e, &v);
        int vt = dbus_message_iter_get_arg_type(&v);

        if (!strcmp(key, "Status") && vt == DBUS_TYPE_STRING) {
            const char *s = NULL;
            dbus_message_iter_get_basic(&v, &s);
            copy_str(m->status, sizeof(m->status), s);
        } else if (!strcmp(key, "Position") && vt == DBUS_TYPE_UINT32) {
            dbus_message_iter_get_basic(&v, &m->position_ms);
        } else if (!strcmp(key, "Track") && vt == DBUS_TYPE_ARRAY) {
            DBusMessageIter td;
            dbus_message_iter_recurse(&v, &td);
            while (dbus_message_iter_get_arg_type(&td) == DBUS_TYPE_DICT_ENTRY) {
                DBusMessageIter te, tv;
                const char *tk = NULL;
                dbus_message_iter_recurse(&td, &te);
                dbus_message_iter_get_basic(&te, &tk);
                dbus_message_iter_next(&te);
                dbus_message_iter_recurse(&te, &tv);
                int tt = dbus_message_iter_get_arg_type(&tv);
                const char *s = NULL;

                if (tt == DBUS_TYPE_STRING) dbus_message_iter_get_basic(&tv, &s);
                if (!strcmp(tk, "Title") && s)        copy_str(m->title, sizeof(m->title), s);
                else if (!strcmp(tk, "Artist") && s)  copy_str(m->artist, sizeof(m->artist), s);
                else if (!strcmp(tk, "Album") && s)   copy_str(m->album, sizeof(m->album), s);
                else if (!strcmp(tk, "Duration") && tt == DBUS_TYPE_UINT32)
                    dbus_message_iter_get_basic(&tv, &m->duration_ms);
                dbus_message_iter_next(&td);
            }
        }
        dbus_message_iter_next(&dict);
    }
    dbus_message_unref(reply);
    return 1;
}

static int send_player_method(DBusConnection *c, const char *path, const char *method)
{
    DBusMessage *msg = dbus_message_new_method_call("org.bluez", path, "org.bluez.MediaPlayer1", method);
    if (!msg) return 0;
    DBusMessage *reply = dbus_call(c, msg, method, 0);
    if (!reply) return 0;
    dbus_message_unref(reply);
    return 1;
}

static void set_music_placeholder(const char *title, const char *artist)
{
    pthread_mutex_lock(&g_state.lock);
    memset(&g_state.music, 0, sizeof(g_state.music));
    copy_str(g_state.music.title, sizeof(g_state.music.title), title);
    copy_str(g_state.music.artist, sizeof(g_state.music.artist), artist);
    copy_str(g_state.music.status, sizeof(g_state.music.status), "stopped");
    pthread_mutex_unlock(&g_state.lock);
}

/* ----- Thread jaquette : le telechargement (reseau, lent) ne bloque ni
 *       les boutons musique ni l'UI ----- */
static pthread_mutex_t g_cover_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cover_cond = PTHREAD_COND_INITIALIZER;
static char g_cover_artist[256], g_cover_query[256];
static int  g_cover_pending = 0;

static void request_cover(const char *artist, const char *query)
{
    pthread_mutex_lock(&g_cover_lock);
    copy_str(g_cover_artist, sizeof(g_cover_artist), artist);
    copy_str(g_cover_query, sizeof(g_cover_query), query);
    g_cover_pending = 1;
    pthread_cond_signal(&g_cover_cond);
    pthread_mutex_unlock(&g_cover_lock);
}

static void *cover_thread_fn(void *arg)
{
    (void)arg;
    while (app_running()) {
        char artist[256], query[256];
        pthread_mutex_lock(&g_cover_lock);
        while (!g_cover_pending && app_running()) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 300 * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&g_cover_cond, &g_cover_lock, &ts);
        }
        int todo = g_cover_pending;
        g_cover_pending = 0;
        copy_str(artist, sizeof(artist), g_cover_artist);
        copy_str(query, sizeof(query), g_cover_query);
        pthread_mutex_unlock(&g_cover_lock);

        if (!todo || !app_running()) continue;

        fprintf(stderr, "Downloading cover for: %s - %s\n", artist, query);
        if (download_itunes_cover(artist, query, MUSIC_COVER_PATH_FS)) {
            pthread_mutex_lock(&g_state.lock);
            g_state.cover_version++;
            pthread_mutex_unlock(&g_state.lock);
        } else {
            fprintf(stderr, "Album art fetch failed for %s - %s\n", artist, query);
        }
    }
    return NULL;
}

static void *music_thread_fn(void *arg)
{
    (void)arg;
    DBusConnection *c = dbus_open_private("music");
    if (!c) return NULL;

    char player[256] = "";
    char last_title[256] = "", last_artist[256] = "";
    time_t last_mac_check = 0, last_find = 0, last_poll = 0;
    int warned = 0;

    set_music_placeholder("No MediaPlayer", "Pair or connect your phone");

    while (app_running()) {
        time_t now = time(NULL);

        if (!dbus_connection_get_is_connected(c)) {      /* bus systeme redemarre */
            dbus_close_private(c);
            c = dbus_open_private("music");
            if (!c) break;
            player[0] = '\0';
        }

        /* 1. Commande en attente depuis l'UI (traitee immediatement) */
        pthread_mutex_lock(&g_state.lock);
        media_cmd_t cmd = g_state.media_cmd;
        g_state.media_cmd = MEDIA_NONE;
        pthread_mutex_unlock(&g_state.lock);
        if (cmd != MEDIA_NONE && player[0]) {
            static const char *names[] = { "", "Play", "Pause", "Next", "Previous" };
            send_player_method(c, player, names[cmd]);
            last_poll = 0;                               /* rafraichir tout de suite */
        }

        /* 2. MAC du telephone pour nav-bind-daemon */
        if (now - last_mac_check >= 5) {
            save_connected_phone_mac(c);
            last_mac_check = now;
        }

        /* 3. (Re)decouverte du player : c'est ce qui manquait apres le 1er appairage */
        if (!player[0]) {
            if (now - last_find >= REDISCOVER_PERIOD_S) {
                last_find = now;
                if (find_player_path(c, player, sizeof(player))) {
                    fprintf(stderr, "[Music] MediaPlayer found: %s\n", player);
                    warned = 0;
                    last_poll = 0;
                } else if (!warned) {
                    fprintf(stderr, "[Music] No MediaPlayer yet, will keep looking\n");
                    warned = 1;
                }
            }
        }

        /* 4. Lecture des proprietes (1 s) */
        if (player[0] && now - last_poll >= 1) {
            last_poll = now;
            music_info_t m;
            memset(&m, 0, sizeof(m));
            if (get_music_properties(c, player, &m)) {
                pthread_mutex_lock(&g_state.lock);
                g_state.music = m;
                pthread_mutex_unlock(&g_state.lock);

                if (m.artist[0] && m.title[0] &&
                    (strcmp(last_title, m.title) != 0 || strcmp(last_artist, m.artist) != 0)) {
                    copy_str(last_title, sizeof(last_title), m.title);
                    copy_str(last_artist, sizeof(last_artist), m.artist);
                    fprintf(stderr, "Music: %s - %s\n", m.artist, m.title);
                    request_cover(m.artist, m.album[0] ? m.album : m.title);
                }
            } else {
                /* Player disparu (telephone deconnecte, player0 -> player1...) */
                fprintf(stderr, "[Music] Player %s lost, searching again\n", player);
                player[0] = '\0';
                last_find = 0;
                set_music_placeholder("No MediaPlayer", "Searching...");
            }
        }

        sleep_ms_interruptible(200, NULL);
    }

    dbus_close_private(c);
    return NULL;
}

/* ========== CALLS (oFono/HFP) ========== */

static int find_modem_with_voicecall(DBusConnection *c, char *out, size_t len)
{
    DBusMessage *msg = dbus_message_new_method_call("org.ofono", "/", "org.ofono.Manager", "GetModems");
    if (!msg) return 0;
    DBusMessage *reply = dbus_call(c, msg, "GetModems", 1);
    if (!reply) return 0;

    int found = 0;
    DBusMessageIter args, arr;
    if (dbus_message_iter_init(reply, &args) &&
        dbus_message_iter_get_arg_type(&args) == DBUS_TYPE_ARRAY) {
        dbus_message_iter_recurse(&args, &arr);
        while (!found && dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRUCT) {
            DBusMessageIter st, props;
            const char *path = NULL;
            int has_vcm = 0, powered = -1;

            dbus_message_iter_recurse(&arr, &st);
            dbus_message_iter_get_basic(&st, &path);
            dbus_message_iter_next(&st);
            dbus_message_iter_recurse(&st, &props);

            while (dbus_message_iter_get_arg_type(&props) == DBUS_TYPE_DICT_ENTRY) {
                DBusMessageIter pe, v;
                const char *key = NULL;
                dbus_message_iter_recurse(&props, &pe);
                dbus_message_iter_get_basic(&pe, &key);
                dbus_message_iter_next(&pe);
                dbus_message_iter_recurse(&pe, &v);

                if (!strcmp(key, "Powered") && dbus_message_iter_get_arg_type(&v) == DBUS_TYPE_BOOLEAN) {
                    dbus_bool_t b = FALSE;
                    dbus_message_iter_get_basic(&v, &b);
                    powered = b;
                } else if (!strcmp(key, "Interfaces") && dbus_message_iter_get_arg_type(&v) == DBUS_TYPE_ARRAY) {
                    DBusMessageIter in;
                    dbus_message_iter_recurse(&v, &in);
                    while (dbus_message_iter_get_arg_type(&in) == DBUS_TYPE_STRING) {
                        const char *iname = NULL;
                        dbus_message_iter_get_basic(&in, &iname);
                        if (!strcmp(iname, "org.ofono.VoiceCallManager")) has_vcm = 1;
                        dbus_message_iter_next(&in);
                    }
                }
                dbus_message_iter_next(&props);
            }

            if (has_vcm) {
                copy_str(out, len, path);
                found = 1;
            } else if (powered == 0 && path && strncmp(path, "/hfp/", 5) == 0) {
                /* Modem HFP present mais eteint : on l'allume, VoiceCallManager
                 * apparaitra au prochain tour */
                DBusMessage *set = dbus_message_new_method_call("org.ofono", path, "org.ofono.Modem", "SetProperty");
                if (set) {
                    const char *prop = "Powered";
                    dbus_bool_t on = TRUE;
                    DBusMessageIter it, var;
                    dbus_message_iter_init_append(set, &it);
                    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &prop);
                    dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, DBUS_TYPE_BOOLEAN_AS_STRING, &var);
                    dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &on);
                    dbus_message_iter_close_container(&it, &var);
                    DBusMessage *r = dbus_call(c, set, "Modem.SetProperty(Powered)", 0);
                    if (r) {
                        fprintf(stderr, "[Calls] Powered on modem %s\n", path);
                        dbus_message_unref(r);
                    }
                }
            }
            dbus_message_iter_next(&arr);
        }
    }
    dbus_message_unref(reply);
    return found;
}

/* Retourne le nombre d'appels, ou -1 si le modem n'est plus joignable */
static int get_calls(DBusConnection *c, const char *modem, call_info_t *calls, int max_calls)
{
    DBusMessage *msg = dbus_message_new_method_call("org.ofono", modem, "org.ofono.VoiceCallManager", "GetCalls");
    if (!msg) return -1;
    DBusMessage *reply = dbus_call(c, msg, "GetCalls", 1);
    if (!reply) return -1;

    int count = 0;
    DBusMessageIter args, arr;
    if (dbus_message_iter_init(reply, &args) &&
        dbus_message_iter_get_arg_type(&args) == DBUS_TYPE_ARRAY) {
        dbus_message_iter_recurse(&args, &arr);
        while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRUCT && count < max_calls) {
            DBusMessageIter st, props;
            const char *path = NULL;
            dbus_message_iter_recurse(&arr, &st);
            dbus_message_iter_get_basic(&st, &path);
            dbus_message_iter_next(&st);
            copy_str(calls[count].call_path, sizeof(calls[count].call_path), path);
            dbus_message_iter_recurse(&st, &props);

            while (dbus_message_iter_get_arg_type(&props) == DBUS_TYPE_DICT_ENTRY) {
                DBusMessageIter pe, v;
                const char *key = NULL, *s = NULL;
                dbus_message_iter_recurse(&props, &pe);
                dbus_message_iter_get_basic(&pe, &key);
                dbus_message_iter_next(&pe);
                dbus_message_iter_recurse(&pe, &v);
                if (dbus_message_iter_get_arg_type(&v) == DBUS_TYPE_STRING) {
                    dbus_message_iter_get_basic(&v, &s);
                    if (!strcmp(key, "State"))                   copy_str(calls[count].state, sizeof(calls[count].state), s);
                    else if (!strcmp(key, "LineIdentification")) copy_str(calls[count].line_id, sizeof(calls[count].line_id), s);
                    else if (!strcmp(key, "Name"))               copy_str(calls[count].name, sizeof(calls[count].name), s);
                }
                dbus_message_iter_next(&props);
            }
            count++;
            dbus_message_iter_next(&arr);
        }
    }
    dbus_message_unref(reply);
    return count;
}

static int call_voicecall_method(DBusConnection *c, const char *call_path, const char *method)
{
    DBusMessage *msg = dbus_message_new_method_call("org.ofono", call_path, "org.ofono.VoiceCall", method);
    if (!msg) return 0;
    DBusMessage *reply = dbus_call(c, msg, method, 0);
    if (!reply) return 0;
    dbus_message_unref(reply);
    return 1;
}

static void *calls_thread_fn(void *arg)
{
    (void)arg;
    DBusConnection *c = dbus_open_private("calls");
    if (!c) return NULL;

    char modem[256] = "";
    time_t last_find = 0, last_poll = 0;
    int warned = 0;
    call_info_t calls[8];

    while (app_running()) {
        time_t now = time(NULL);

        if (!dbus_connection_get_is_connected(c)) {
            dbus_close_private(c);
            c = dbus_open_private("calls");
            if (!c) break;
            modem[0] = '\0';
        }

        /* Commande Decrocher/Raccrocher depuis l'UI */
        pthread_mutex_lock(&g_state.lock);
        call_cmd_t cmd = g_state.call_cmd;
        char path[256];
        copy_str(path, sizeof(path), g_state.call_cmd_path);
        g_state.call_cmd = CALL_NONE;
        pthread_mutex_unlock(&g_state.lock);
        if (cmd != CALL_NONE && path[0]) {
            call_voicecall_method(c, path, cmd == CALL_ANSWER ? "Answer" : "Hangup");
            last_poll = 0;
        }

        /* (Re)decouverte du modem HFP */
        if (!modem[0] && now - last_find >= REDISCOVER_PERIOD_S) {
            last_find = now;
            if (find_modem_with_voicecall(c, modem, sizeof(modem))) {
                fprintf(stderr, "[Calls] HFP modem found: %s\n", modem);
                warned = 0;
            } else if (!warned) {
                fprintf(stderr, "[Calls] No HFP modem yet, will keep looking\n");
                warned = 1;
            }
        }

        if (modem[0] && now - last_poll >= 1) {
            last_poll = now;
            memset(calls, 0, sizeof(calls));
            int n = get_calls(c, modem, calls, 8);
            if (n < 0) {
                fprintf(stderr, "[Calls] Modem %s lost, searching again\n", modem);
                modem[0] = '\0';
                last_find = 0;
                n = 0;
            }

            pthread_mutex_lock(&g_state.lock);
            g_state.has_incoming_call = 0;
            for (int i = 0; i < n; i++) {
                if (!strcmp(calls[i].state, "incoming") || !strcmp(calls[i].state, "waiting")) {
                    g_state.current_call = calls[i];
                    g_state.has_incoming_call = 1;
                    break;
                }
            }
            pthread_mutex_unlock(&g_state.lock);
        }

        sleep_ms_interruptible(200, NULL);
    }

    dbus_close_private(c);
    return NULL;
}

/* ========== UI UPDATE FUNCTIONS (thread UI uniquement, copies locales) ========== */

static void update_nav_display(const nav_info_t *nav, int connected)
{
    char dist_buf[64];

    if (nav->instruction[0])
        label_set_if_changed(g_nav_label, nav->instruction);
    else
        label_set_if_changed(g_nav_label, connected ? "Waiting for route..." : "No Navigation");

    if (nav->distance >= 1000)
        snprintf(dist_buf, sizeof(dist_buf), "%.1f km", nav->distance / 1000.0);
    else if (nav->instruction[0])
        snprintf(dist_buf, sizeof(dist_buf), "%.0f m", nav->distance);
    else
        snprintf(dist_buf, sizeof(dist_buf), "-- m");
    label_set_if_changed(g_nav_dist_label, dist_buf);

    label_set_if_changed(g_nav_street_label, nav->street_name);
}

static void update_music_display(const music_info_t *m, unsigned int cover_version)
{
    static unsigned int shown_cover = (unsigned int)-1;

    if (m->title[0])  label_set_if_changed(g_music_label, m->title);
    if (m->artist[0]) label_set_if_changed(g_music_artist_label, m->artist);

    /* Recharge la jaquette uniquement si elle a change (avant : toutes les 100 ms) */
    if (cover_version != shown_cover) {
        struct stat st;
        if (stat(MUSIC_COVER_PATH_FS, &st) == 0) {
            lv_image_cache_drop(MUSIC_COVER_PATH);  /* sinon LVGL garde l'ancienne image en cache */
            lv_image_set_src(g_music_img, MUSIC_COVER_PATH);
        }
        shown_cover = cover_version;
    }

    lv_obj_t *label_play = lv_obj_get_child(g_btn_play, 0);
    label_set_if_changed(label_play, strcmp(m->status, "playing") == 0 ? "Pause" : "Play");
}

static void update_call_display(int incoming, const call_info_t *call)
{
    if (incoming) {
        char call_text[256];
        lv_obj_remove_flag(g_call_panel, LV_OBJ_FLAG_HIDDEN);
        snprintf(call_text, sizeof(call_text), LV_SYMBOL_CALL " %s\n%s",
                 call->name[0] ? call->name : "Unknown", call->line_id);
        label_set_if_changed(g_call_number_label, call_text);
    } else {
        lv_obj_add_flag(g_call_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

static void ui_update_timer(lv_timer_t *timer)
{
    (void)timer;
    nav_info_t nav;
    music_info_t music;
    call_info_t call;
    int nav_connected, incoming;
    unsigned int cover_version;

    /* Copie rapide sous verrou, puis travail LVGL sans verrou */
    pthread_mutex_lock(&g_state.lock);
    nav = g_state.nav;
    nav_connected = g_state.nav_connected;
    music = g_state.music;
    cover_version = g_state.cover_version;
    call = g_state.current_call;
    incoming = g_state.has_incoming_call;
    pthread_mutex_unlock(&g_state.lock);

    update_nav_display(&nav, nav_connected);
    update_music_display(&music, cover_version);
    update_call_display(incoming, &call);
    speedo_set_speed((int32_t)nav.speed);

    char status[128];
    bt_agent_get_status(status, sizeof(status));
    label_set_if_changed(g_pair_status_label, status);
}

/* ========== UI BUTTON CALLBACKS (non bloquants) ========== */

static void post_media_cmd(media_cmd_t cmd)
{
    pthread_mutex_lock(&g_state.lock);
    g_state.media_cmd = cmd;
    pthread_mutex_unlock(&g_state.lock);
}

static void on_btn_prev_clicked(lv_event_t *e) { (void)e; post_media_cmd(MEDIA_PREV); }
static void on_btn_next_clicked(lv_event_t *e) { (void)e; post_media_cmd(MEDIA_NEXT); }

static void on_btn_play_clicked(lv_event_t *e)
{
    (void)e;
    pthread_mutex_lock(&g_state.lock);
    g_state.media_cmd = strcmp(g_state.music.status, "playing") == 0 ? MEDIA_PAUSE : MEDIA_PLAY;
    pthread_mutex_unlock(&g_state.lock);
}

static void post_call_cmd(call_cmd_t cmd)
{
    pthread_mutex_lock(&g_state.lock);
    if (g_state.has_incoming_call) {
        g_state.call_cmd = cmd;
        copy_str(g_state.call_cmd_path, sizeof(g_state.call_cmd_path), g_state.current_call.call_path);
        g_state.has_incoming_call = 0;     /* masque le panneau tout de suite */
    }
    pthread_mutex_unlock(&g_state.lock);
}

static void on_btn_answer_clicked(lv_event_t *e) { (void)e; post_call_cmd(CALL_ANSWER); }
static void on_btn_hangup_clicked(lv_event_t *e) { (void)e; post_call_cmd(CALL_HANGUP); }

static void on_btn_pair_device_clicked(lv_event_t *e)
{
    (void)e;
    fprintf(stderr, "[UI] Opening Bluetooth pairing window\n");
    bt_agent_open_pairing_window();
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
    lv_label_set_text(g_nav_street_label, "");

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

    /* ===== BLUETOOTH PAIRING (outside music container) ===== */
    g_btn_pair = lv_button_create(g_scr_main);
    lv_obj_set_size(g_btn_pair, 60, 60);
    lv_obj_align(g_btn_pair, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_color(g_btn_pair, lv_color_hex(0x00a8e8), 0);
    lv_obj_add_event_cb(g_btn_pair, on_btn_pair_device_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label_pair = lv_label_create(g_btn_pair);
    lv_label_set_text(label_pair, "Pair");
    lv_obj_center(label_pair);

    /* Pairing status label */
    g_pair_status_label = lv_label_create(g_scr_main);
    lv_obj_align(g_pair_status_label, LV_ALIGN_BOTTOM_MID, 0, -100);
    lv_obj_set_style_text_color(g_pair_status_label, lv_color_hex(0x00a8e8), 0);
    lv_obj_set_style_text_font(g_pair_status_label, &lv_font_montserrat_16, 0);
    lv_label_set_text(g_pair_status_label, "");

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
    lv_label_set_text(label_answer, LV_SYMBOL_OK " Answer");
    lv_obj_center(label_answer);

    g_btn_hangup = lv_button_create(g_call_panel);
    lv_obj_set_size(g_btn_hangup, 100, 60);
    lv_obj_align(g_btn_hangup, LV_ALIGN_BOTTOM_RIGHT, -30, -20);
    lv_obj_add_event_cb(g_btn_hangup, on_btn_hangup_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_bg_color(g_btn_hangup, lv_color_hex(0xe74c3c), 0);
    lv_obj_t *label_hangup = lv_label_create(g_btn_hangup);
    lv_label_set_text(label_hangup, LV_SYMBOL_CLOSE " Reject");
    lv_obj_center(label_hangup);

    /* Charge la jaquette existante si presente */
    struct stat st;
    if (stat(MUSIC_COVER_PATH_FS, &st) == 0) lv_image_set_src(g_music_img, MUSIC_COVER_PATH);
}

/* ========== MAIN ========== */

typedef struct {
    const char *name;
    void *(*fn)(void *);
    pthread_t tid;
    int started;
} worker_t;

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    /* SDL ne doit PAS intercepter Ctrl+C/SIGTERM : c'est ce qui empechait
     * le programme de se terminer. On gere les signaux nous-memes. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    install_signal_handlers();
    curl_global_init(CURL_GLOBAL_DEFAULT);   /* une seule fois, avant les threads */

    lv_init();
    lv_fs_posix_init();

    lv_display_t *disp = sdl_hal_init(SCREEN_W, SCREEN_H);
    if (!disp) {
        fprintf(stderr, "Failed to initialize display\n");
        return 1;
    }
    SDL_SetEventFilter(sdl_event_filter, NULL);

    printf("=== Round Dashboard (1080x1080) ===\n");
    printf("Navigation | Music Player | Calls   (Esc or Ctrl+C to quit)\n");

    create_ui();

    /* Agent Bluetooth (appairage sans popup du bureau) */
    bt_agent_start();

    worker_t workers[] = {
        { "nav",   nav_thread_fn,   0, 0 },
        { "music", music_thread_fn, 0, 0 },
        { "cover", cover_thread_fn, 0, 0 },
        { "calls", calls_thread_fn, 0, 0 },
    };
    const int n_workers = (int)(sizeof(workers) / sizeof(workers[0]));
    for (int i = 0; i < n_workers; i++) {
        if (pthread_create(&workers[i].tid, NULL, workers[i].fn, NULL) == 0)
            workers[i].started = 1;
        else
            fprintf(stderr, "Failed to create %s thread\n", workers[i].name);
    }

    lv_timer_create(ui_update_timer, 100, NULL);

    /* ===== Boucle principale ===== */
    while (app_running()) {
        uint32_t wait_ms = lv_timer_handler();
        if (wait_ms == LV_NO_TIMER_READY || wait_ms > 30) wait_ms = 30;
        usleep(wait_ms * 1000);
    }

    /* ===== Arret propre ===== */
    fprintf(stderr, "Shutting down...\n");
    alarm(6);   /* filet de securite : si quelque chose bloque, SIGALRM tue le processus */

    bt_agent_stop();                        /* desenregistre l'agent, coupe le mode visible */

    pthread_mutex_lock(&g_cover_lock);      /* reveille le thread jaquette */
    pthread_cond_broadcast(&g_cover_cond);
    pthread_mutex_unlock(&g_cover_lock);

    for (int i = 0; i < n_workers; i++) {
        if (!workers[i].started) continue;
        pthread_kill(workers[i].tid, SIGUSR1);   /* interrompt un open()/poll() bloquant */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 3;
        if (pthread_timedjoin_np(workers[i].tid, NULL, &ts) != 0)
            fprintf(stderr, "Thread %s did not stop in time\n", workers[i].name);
    }

    lv_sdl_quit();          /* rend l'ecran au bureau (sortie du plein ecran) */
    curl_global_cleanup();
    fprintf(stderr, "Bye.\n");
    return 0;
}
