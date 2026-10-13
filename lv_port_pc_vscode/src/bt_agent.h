/*
 * bt_agent.h - Agent d'appairage BlueZ integre au dashboard
 *
 * Remplace bt_pairing.h + bt_make_discoverable.sh.
 *
 * Le dashboard s'enregistre comme agent BlueZ PAR DEFAUT (org.bluez.Agent1).
 * BlueZ lui envoie donc les demandes de confirmation au lieu de les envoyer
 * a l'agent du bureau Pi OS : plus aucune popup ne sort de l'application
 * plein ecran.
 *
 * Securite : les NOUVEAUX appairages ne sont acceptes que pendant la fenetre
 * ouverte par le bouton "Pair" (BT_PAIR_WINDOW_S secondes). En dehors de
 * cette fenetre, seuls les telephones deja appaires peuvent se reconnecter.
 *
 * Toutes les operations D-Bus sont faites dans un thread dedie ; les
 * fonctions ci-dessous sont non bloquantes et appelables depuis l'UI.
 */
#ifndef BT_AGENT_H
#define BT_AGENT_H

#include <stddef.h>

/* Duree de la fenetre d'appairage (secondes) */
#define BT_PAIR_WINDOW_S 180

/* Demarre le thread agent. Retourne 0 si le thread est lance. */
int  bt_agent_start(void);

/* Desenregistre l'agent, coupe le mode visible et arrete le thread. */
void bt_agent_stop(void);

/* Ouvre la fenetre d'appairage (Pi visible + appairage auto-accepte). */
void bt_agent_open_pairing_window(void);

/* Copie le texte d'etat a afficher sous le bouton Pair. */
void bt_agent_get_status(char *buf, size_t len);

#endif /* BT_AGENT_H */
