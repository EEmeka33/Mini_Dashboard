/*
 * bt_agent.c - Agent d'appairage BlueZ (org.bluez.Agent1) integre au dashboard
 *
 * Voir bt_agent.h pour le principe. Resume :
 *   - enregistrement comme agent par defaut (RequestDefaultAgent) ->
 *     l'agent du bureau Pi OS ne recoit plus les demandes, donc plus de popup ;
 *   - bouton Pair -> Pi visible + appairage accepte automatiquement pendant
 *     BT_PAIR_WINDOW_S secondes ;
 *   - le telephone appaire est marque "Trusted" (reconnexion auto sans
 *     demande d'autorisation) puis connecte ;
 *   - hors fenetre : seuls les appareils deja appaires sont autorises.
 */
#define _GNU_SOURCE
#include "bt_agent.h"

#include <dbus/dbus.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---------- Configuration ---------- */
#define AGENT_PATH        "/org/minidashboard/agent"
/* "NoInputNoOutput" = Just Works : le telephone affiche juste "Associer ?".
 * Mettre "DisplayYesNo" pour une comparaison de code (toujours confirmee
 * automatiquement cote Pi, le code s'affiche seulement sur le telephone). */
#define AGENT_CAPABILITY  "NoInputNoOutput"
/* Nom visible depuis le telephone (NULL = ne pas changer le nom du Pi) */
#define BT_ALIAS          NULL
#define DBUS_TIMEOUT_MS   3000

#define BLUEZ             "org.bluez"
#define AGENT_IFACE       "org.bluez.Agent1"
#define ADAPTER_IFACE     "org.bluez.Adapter1"
#define DEVICE_IFACE      "org.bluez.Device1"
#define PROPS_IFACE       "org.freedesktop.DBus.Properties"

/* ---------- Etat partage (protege par s_lock) ---------- */
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t       s_thread;
static int             s_thread_started = 0;
static volatile int    s_quit = 0;
static int             s_open_request = 0;     /* demande du bouton Pair  */
static time_t          s_window_end = 0;       /* 0 = fenetre fermee      */
static char            s_status[128] = "";
static char            s_pending_dev[256] = "";/* device en cours d'appairage */

/* Etat interne au thread agent */
static char s_adapter_path[128] = "/org/bluez/hci0";
static int  s_registered = 0;

static void set_status(const char *fmt, const char *arg)
{
    pthread_mutex_lock(&s_lock);
    snprintf(s_status, sizeof(s_status), fmt, arg ? arg : "");
    pthread_mutex_unlock(&s_lock);
}

static int window_is_open(void)
{
    pthread_mutex_lock(&s_lock);
    int open = s_window_end != 0 && time(NULL) < s_window_end;
    pthread_mutex_unlock(&s_lock);
    return open;
}

/* =================================================================== */
/*  Helpers D-Bus                                                      */
/* =================================================================== */

/* Appel bloquant ; retourne la reponse (a liberer) ou NULL. */
static DBusMessage *call(DBusConnection *c, DBusMessage *msg, char *errname, size_t errlen)
{
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(c, msg, DBUS_TIMEOUT_MS, &err);
    dbus_message_unref(msg);
    if (!reply) {
        if (errname) snprintf(errname, errlen, "%s", err.name ? err.name : "unknown");
        dbus_error_free(&err);
    }
    return reply;
}

static int set_property(DBusConnection *c, const char *path, const char *iface,
                        const char *name, int type, const void *value)
{
    DBusMessage *msg = dbus_message_new_method_call(BLUEZ, path, PROPS_IFACE, "Set");
    if (!msg) return 0;

    const char *sig = (type == DBUS_TYPE_BOOLEAN) ? DBUS_TYPE_BOOLEAN_AS_STRING :
                      (type == DBUS_TYPE_UINT32)  ? DBUS_TYPE_UINT32_AS_STRING  :
                                                    DBUS_TYPE_STRING_AS_STRING;
    DBusMessageIter it, var;
    dbus_message_iter_init_append(msg, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &name);
    dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, sig, &var);
    dbus_message_iter_append_basic(&var, type, value);
    dbus_message_iter_close_container(&it, &var);

    char errname[128];
    DBusMessage *reply = call(c, msg, errname, sizeof(errname));
    if (!reply) {
        fprintf(stderr, "[BT agent] Set %s.%s failed: %s\n", path, name, errname);
        return 0;
    }
    dbus_message_unref(reply);
    return 1;
}

static int set_bool(DBusConnection *c, const char *path, const char *iface,
                    const char *name, int on)
{
    dbus_bool_t v = on ? TRUE : FALSE;
    return set_property(c, path, iface, name, DBUS_TYPE_BOOLEAN, &v);
}

/* Lit une propriete booleenne d'un device. Retourne -1 si erreur. */
static int get_device_bool(DBusConnection *c, const char *dev, const char *name)
{
    DBusMessage *msg = dbus_message_new_method_call(BLUEZ, dev, PROPS_IFACE, "Get");
    if (!msg) return -1;
    const char *iface = DEVICE_IFACE;
    dbus_message_append_args(msg, DBUS_TYPE_STRING, &iface,
                             DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID);
    DBusMessage *reply = call(c, msg, NULL, 0);
    if (!reply) return -1;

    int result = -1;
    DBusMessageIter it, var;
    if (dbus_message_iter_init(reply, &it) &&
        dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_VARIANT) {
        dbus_message_iter_recurse(&it, &var);
        if (dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_BOOLEAN) {
            dbus_bool_t b = FALSE;
            dbus_message_iter_get_basic(&var, &b);
            result = b ? 1 : 0;
        }
    }
    dbus_message_unref(reply);
    return result;
}

/* Appel "fire and forget" (ne bloque pas le thread, ex: Device1.Connect). */
static void call_async(DBusConnection *c, const char *path, const char *iface, const char *method)
{
    DBusMessage *msg = dbus_message_new_method_call(BLUEZ, path, iface, method);
    if (!msg) return;
    dbus_message_set_no_reply(msg, TRUE);
    dbus_connection_send(c, msg, NULL);
    dbus_message_unref(msg);
}

/* ---------- Parcours des objets BlueZ ---------- */

typedef struct {
    const char *path;
    const char *alias;
    int paired, trusted, connected;
} device_info_t;

typedef void (*device_cb)(DBusConnection *c, const device_info_t *dev, void *ud);

static void read_device_props(DBusMessageIter *props, device_info_t *d)
{
    while (dbus_message_iter_get_arg_type(props) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter e, v;
        const char *key = NULL;
        dbus_message_iter_recurse(props, &e);
        dbus_message_iter_get_basic(&e, &key);
        dbus_message_iter_next(&e);
        dbus_message_iter_recurse(&e, &v);
        int t = dbus_message_iter_get_arg_type(&v);

        if (t == DBUS_TYPE_BOOLEAN) {
            dbus_bool_t b = FALSE;
            dbus_message_iter_get_basic(&v, &b);
            if (!strcmp(key, "Paired"))         d->paired = b;
            else if (!strcmp(key, "Trusted"))   d->trusted = b;
            else if (!strcmp(key, "Connected")) d->connected = b;
        } else if (t == DBUS_TYPE_STRING && !strcmp(key, "Alias")) {
            dbus_message_iter_get_basic(&v, &d->alias);
        }
        dbus_message_iter_next(props);
    }
}

/* Appelle cb pour chaque device ; met a jour s_adapter_path au passage. */
static int foreach_device(DBusConnection *c, device_cb cb, void *ud)
{
    DBusMessage *msg = dbus_message_new_method_call(BLUEZ, "/",
        "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
    if (!msg) return 0;
    DBusMessage *reply = call(c, msg, NULL, 0);
    if (!reply) return 0;

    DBusMessageIter root, objs;
    if (!dbus_message_iter_init(reply, &root) ||
        dbus_message_iter_get_arg_type(&root) != DBUS_TYPE_ARRAY) {
        dbus_message_unref(reply);
        return 0;
    }
    int adapter_found = 0;
    dbus_message_iter_recurse(&root, &objs);
    while (dbus_message_iter_get_arg_type(&objs) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter obj, ifaces;
        const char *path = NULL;
        dbus_message_iter_recurse(&objs, &obj);
        dbus_message_iter_get_basic(&obj, &path);
        dbus_message_iter_next(&obj);
        dbus_message_iter_recurse(&obj, &ifaces);

        while (dbus_message_iter_get_arg_type(&ifaces) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter ie, props;
            const char *iname = NULL;
            dbus_message_iter_recurse(&ifaces, &ie);
            dbus_message_iter_get_basic(&ie, &iname);
            dbus_message_iter_next(&ie);
            dbus_message_iter_recurse(&ie, &props);

            if (!strcmp(iname, ADAPTER_IFACE) && !adapter_found) {
                snprintf(s_adapter_path, sizeof(s_adapter_path), "%s", path);
                adapter_found = 1;
            } else if (!strcmp(iname, DEVICE_IFACE) && cb) {
                device_info_t d = { .path = path, .alias = "" };
                read_device_props(&props, &d);
                cb(c, &d, ud);
            }
            dbus_message_iter_next(&ifaces);
        }
        dbus_message_iter_next(&objs);
    }
    dbus_message_unref(reply);
    return 1;
}

/* =================================================================== */
/*  Objet agent (appele par bluetoothd)                                */
/* =================================================================== */

static void remember_pending(const char *dev)
{
    pthread_mutex_lock(&s_lock);
    snprintf(s_pending_dev, sizeof(s_pending_dev), "%s", dev ? dev : "");
    pthread_mutex_unlock(&s_lock);
}

static DBusHandlerResult agent_message(DBusConnection *c, DBusMessage *m, void *ud)
{
    (void)ud;
    if (!dbus_message_has_interface(m, AGENT_IFACE))
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    const char *member = dbus_message_get_member(m);
    const char *dev = NULL;
    int window = window_is_open();
    DBusMessage *reply = NULL;

    /* Le premier argument est le device pour toutes les requetes sauf Release/Cancel */
    DBusMessageIter it;
    if (dbus_message_iter_init(m, &it) &&
        dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_OBJECT_PATH)
        dbus_message_iter_get_basic(&it, &dev);

    if (!strcmp(member, "Release")) {
        fprintf(stderr, "[BT agent] Released by BlueZ\n");
        s_registered = 0;
        reply = dbus_message_new_method_return(m);
    }
    else if (!strcmp(member, "Cancel")) {
        fprintf(stderr, "[BT agent] Pairing cancelled by remote\n");
        reply = dbus_message_new_method_return(m);
    }
    else if (!strcmp(member, "DisplayPinCode") || !strcmp(member, "DisplayPasskey")) {
        reply = dbus_message_new_method_return(m);
    }
    else if (!strcmp(member, "RequestPinCode")) {         /* vieux appareils */
        if (window) {
            const char *pin = "0000";
            remember_pending(dev);
            reply = dbus_message_new_method_return(m);
            dbus_message_append_args(reply, DBUS_TYPE_STRING, &pin, DBUS_TYPE_INVALID);
        }
    }
    else if (!strcmp(member, "RequestPasskey")) {
        if (window) {
            dbus_uint32_t pk = 0;
            remember_pending(dev);
            reply = dbus_message_new_method_return(m);
            dbus_message_append_args(reply, DBUS_TYPE_UINT32, &pk, DBUS_TYPE_INVALID);
        }
    }
    else if (!strcmp(member, "RequestConfirmation") || !strcmp(member, "RequestAuthorization")) {
        if (window) {
            remember_pending(dev);
            reply = dbus_message_new_method_return(m);
        }
    }
    else if (!strcmp(member, "AuthorizeService")) {
        /* Profils (A2DP, HFP, SPP...) : OK si fenetre ouverte ou deja appaire */
        if (window || (dev && get_device_bool(c, dev, "Paired") == 1))
            reply = dbus_message_new_method_return(m);
    }
    else {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    if (reply) {
        fprintf(stderr, "[BT agent] %s(%s) -> accepted\n", member, dev ? dev : "");
    } else {
        fprintf(stderr, "[BT agent] %s(%s) -> rejected (press Pair first)\n",
                member, dev ? dev : "");
        reply = dbus_message_new_error(m, "org.bluez.Error.Rejected",
                                       "Pairing window closed");
    }
    dbus_connection_send(c, reply, NULL);
    dbus_message_unref(reply);
    return DBUS_HANDLER_RESULT_HANDLED;
}

static const DBusObjectPathVTable agent_vtable = { .message_function = agent_message };

static int agent_manager_call(DBusConnection *c, const char *method, int with_cap, char *errname, size_t len)
{
    DBusMessage *msg = dbus_message_new_method_call(BLUEZ, "/org/bluez",
                                                    "org.bluez.AgentManager1", method);
    if (!msg) return 0;
    const char *path = AGENT_PATH, *cap = AGENT_CAPABILITY;
    if (with_cap)
        dbus_message_append_args(msg, DBUS_TYPE_OBJECT_PATH, &path,
                                 DBUS_TYPE_STRING, &cap, DBUS_TYPE_INVALID);
    else
        dbus_message_append_args(msg, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID);

    DBusMessage *reply = call(c, msg, errname, len);
    if (!reply) return 0;
    dbus_message_unref(reply);
    return 1;
}

/* (Re)enregistre l'agent et le (re)declare agent par defaut. */
static void agent_assert_default(DBusConnection *c)
{
    char err[128] = "";
    if (!s_registered) {
        if (!agent_manager_call(c, "RegisterAgent", 1, err, sizeof(err)) &&
            !strstr(err, "AlreadyExists")) {
            fprintf(stderr, "[BT agent] RegisterAgent failed: %s\n", err);
            return;
        }
        s_registered = 1;
        fprintf(stderr, "[BT agent] Registered (%s)\n", AGENT_CAPABILITY);
    }
    if (!agent_manager_call(c, "RequestDefaultAgent", 0, err, sizeof(err))) {
        fprintf(stderr, "[BT agent] RequestDefaultAgent failed: %s\n", err);
        s_registered = 0;   /* bluetoothd redemarre ? on re-enregistre au prochain tour */
    }
}

/* =================================================================== */
/*  Fenetre d'appairage                                                */
/* =================================================================== */

static void window_open(DBusConnection *c)
{
    dbus_uint32_t timeout = BT_PAIR_WINDOW_S;

    foreach_device(c, NULL, NULL);    /* rafraichit s_adapter_path */
    agent_assert_default(c);          /* au cas ou le bureau aurait repris la main */

    int ok = set_bool(c, s_adapter_path, ADAPTER_IFACE, "Powered", 1);
    if (BT_ALIAS) {
        const char *alias = BT_ALIAS;
        set_property(c, s_adapter_path, ADAPTER_IFACE, "Alias", DBUS_TYPE_STRING, &alias);
    }
    set_property(c, s_adapter_path, ADAPTER_IFACE, "DiscoverableTimeout", DBUS_TYPE_UINT32, &timeout);
    set_property(c, s_adapter_path, ADAPTER_IFACE, "PairableTimeout", DBUS_TYPE_UINT32, &timeout);
    ok &= set_bool(c, s_adapter_path, ADAPTER_IFACE, "Pairable", 1);
    ok &= set_bool(c, s_adapter_path, ADAPTER_IFACE, "Discoverable", 1);

    pthread_mutex_lock(&s_lock);
    s_pending_dev[0] = '\0';
    s_window_end = ok ? time(NULL) + BT_PAIR_WINDOW_S : 0;
    snprintf(s_status, sizeof(s_status), ok ? "" : "Bluetooth error");
    pthread_mutex_unlock(&s_lock);

    fprintf(stderr, "[BT agent] Pairing window %s (%d s)\n", ok ? "OPEN" : "FAILED", BT_PAIR_WINDOW_S);
}

static void window_close(DBusConnection *c)
{
    set_bool(c, s_adapter_path, ADAPTER_IFACE, "Discoverable", 0);
    pthread_mutex_lock(&s_lock);
    s_window_end = 0;
    pthread_mutex_unlock(&s_lock);
    fprintf(stderr, "[BT agent] Pairing window closed\n");
}

/* Pendant la fenetre : trust + connexion du telephone fraichement appaire */
static void check_new_device(DBusConnection *c, const device_info_t *d, void *ud)
{
    int *done = ud;
    char pending[256];
    pthread_mutex_lock(&s_lock);
    snprintf(pending, sizeof(pending), "%s", s_pending_dev);
    pthread_mutex_unlock(&s_lock);

    if (!d->paired || d->trusted) return;

    fprintf(stderr, "[BT agent] New paired device %s (%s) -> Trusted\n", d->alias, d->path);
    set_bool(c, d->path, DEVICE_IFACE, "Trusted", 1);
    if (!d->connected)
        call_async(c, d->path, DEVICE_IFACE, "Connect");

    if (pending[0] == '\0' || strcmp(pending, d->path) == 0) {
        set_status("Paired: %s", d->alias);
        *done = 1;
    }
}

/* =================================================================== */
/*  Thread                                                             */
/* =================================================================== */

static void *agent_thread_fn(void *arg)
{
    (void)arg;
    while (!s_quit) {
        DBusError err;
        dbus_error_init(&err);
        DBusConnection *c = dbus_bus_get_private(DBUS_BUS_SYSTEM, &err);
        if (!c) {
            fprintf(stderr, "[BT agent] system bus: %s\n", err.message);
            dbus_error_free(&err);
            sleep(2);
            continue;
        }
        dbus_connection_set_exit_on_disconnect(c, FALSE);
        dbus_connection_register_object_path(c, AGENT_PATH, &agent_vtable, NULL);
        s_registered = 0;

        time_t last_assert = 0, last_scan = 0;
        while (!s_quit && dbus_connection_get_is_connected(c)) {
            dbus_connection_read_write_dispatch(c, 200);
            time_t now = time(NULL);

            /* Reprend la place d'agent par defaut toutes les 15 s */
            if (now - last_assert >= 15) {
                agent_assert_default(c);
                last_assert = now;
            }

            /* Demande du bouton Pair */
            pthread_mutex_lock(&s_lock);
            int req = s_open_request;
            s_open_request = 0;
            time_t end = s_window_end;
            pthread_mutex_unlock(&s_lock);
            if (req) window_open(c);

            if (end != 0) {
                if (now >= end) {
                    window_close(c);
                    set_status("", NULL);
                } else if (now != last_scan) {
                    int done = 0;
                    foreach_device(c, check_new_device, &done);
                    if (done) window_close(c);
                    last_scan = now;
                }
            }
        }

        /* Nettoyage propre */
        if (window_is_open()) window_close(c);
        if (s_registered) agent_manager_call(c, "UnregisterAgent", 0, NULL, 0);
        s_registered = 0;
        dbus_connection_unregister_object_path(c, AGENT_PATH);
        dbus_connection_close(c);
        dbus_connection_unref(c);
    }
    return NULL;
}

/* =================================================================== */
/*  API publique                                                       */
/* =================================================================== */

int bt_agent_start(void)
{
    if (s_thread_started) return 0;
    s_quit = 0;
    if (pthread_create(&s_thread, NULL, agent_thread_fn, NULL) != 0) {
        fprintf(stderr, "[BT agent] cannot create thread\n");
        return -1;
    }
    s_thread_started = 1;
    return 0;
}

void bt_agent_stop(void)
{
    if (!s_thread_started) return;
    s_quit = 1;
    /* Le thread se reveille toutes les 200 ms ; on borne l'attente a ~3 s */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 3;
    if (pthread_timedjoin_np(s_thread, NULL, &ts) != 0)
        fprintf(stderr, "[BT agent] thread did not stop in time\n");
    s_thread_started = 0;
}

void bt_agent_open_pairing_window(void)
{
    pthread_mutex_lock(&s_lock);
    s_open_request = 1;
    snprintf(s_status, sizeof(s_status), "Opening...");
    pthread_mutex_unlock(&s_lock);
}

void bt_agent_get_status(char *buf, size_t len)
{
    pthread_mutex_lock(&s_lock);
    time_t now = time(NULL);
    if (s_window_end != 0 && now < s_window_end) {
        int left = (int)(s_window_end - now);
        snprintf(buf, len, "Visible %d:%02d - pair from your phone", left / 60, left % 60);
    } else {
        snprintf(buf, len, "%s", s_status);
    }
    pthread_mutex_unlock(&s_lock);
}
