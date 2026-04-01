// album_art.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

/* -------- Helpers mémoire pour libcurl -------- */

struct Memory {
    char *data;
    size_t size;
};

static size_t write_to_memory(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    struct Memory *mem = (struct Memory *)userp;

    char *ptr = realloc(mem->data, mem->size + realsize + 1);
    if (!ptr) {
        fprintf(stderr, "Not enough memory\n");
        return 0;
    }

    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = '\0';

    return realsize;
}

static size_t write_to_file(void *contents, size_t size, size_t nmemb, void *userp)
{
    FILE *f = (FILE *)userp;
    return fwrite(contents, size, nmemb, f);
}

/* URL‑encode une chaîne pour l’inclure dans une query */
static char *url_encode(CURL *curl, const char *s)
{
    char *enc = curl_easy_escape(curl, s, 0);
    if (!enc) return NULL;
    char *out = strdup(enc);
    curl_free(enc);
    return out;
}

/* -------- Fonction principale : téléchargement de la jaquette -------- */

/**
 * download_itunes_cover:
 *  - artist, album: chaînes UTF‑8
 *  - out_path: chemin du fichier image de sortie (ex: "cover.jpg")
 * Retourne 1 si OK, 0 si erreur.
 *
 * Utilise l’API iTunes Search:
 *   https://itunes.apple.com/search?term=<artist>+<album>&entity=album&limit=1 [web:148][web:151]
 */
int download_itunes_cover(const char *artist, const char *album, const char *out_path)
{
    CURL *curl = NULL;
    CURLcode res;
    struct Memory mem = {0};
    int ok = 0;

    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "curl_easy_init failed\n");
        goto cleanup;
    }

    /* Construire la query "artist album" encodée */
    char query[512];
    snprintf(query, sizeof(query), "%s %s", artist ? artist : "", album ? album : "");

    char *enc_query = url_encode(curl, query);
    if (!enc_query) {
        fprintf(stderr, "URL encode failed\n");
        goto cleanup;
    }

    char url[1024];
    snprintf(url, sizeof(url),
             "https://itunes.apple.com/search?term=%s&entity=album&limit=1",
             enc_query);
    free(enc_query);

    printf("Fetching JSON from: %s\n", url);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "album-art-fetcher/1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_memory);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&mem);

    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform (JSON) failed: %s\n",
                curl_easy_strerror(res));
        goto cleanup;
    }

    if (!mem.data || mem.size == 0) {
        fprintf(stderr, "Empty JSON response\n");
        goto cleanup;
    }

    /* Chercher "artworkUrl100":"...". [web:148][web:151] */
    const char *key = "\"artworkUrl100\":\"";
    char *p = strstr(mem.data, key);
    if (!p) {
        fprintf(stderr, "artworkUrl100 not found in JSON\n");
        goto cleanup;
    }
    p += strlen(key);
    char *q = strchr(p, '"');
    if (!q) {
        fprintf(stderr, "Malformed JSON around artworkUrl100\n");
        goto cleanup;
    }

    size_t len = (size_t)(q - p);
    char *art_url = malloc(len + 1);
    if (!art_url) {
        fprintf(stderr, "malloc failed\n");
        goto cleanup;
    }
    memcpy(art_url, p, len);
    art_url[len] = '\0';

    /* Optionnel: passer en 600x600 au lieu de 100x100, comme suggéré pour iTunes. [web:147] */
    /*
    char *size_pos = strstr(art_url, "100x100");
    if (size_pos) {
        memcpy(size_pos, "600x600", strlen("600x600"));
    }
        */

    printf("Artwork URL: %s\n", art_url);

    /* Deuxième requête : télécharger l’image */
    FILE *f = fopen(out_path, "wb");
    if (!f) {
        perror("fopen");
        free(art_url);
        goto cleanup;
    }

    /* Réutiliser le même handle curl pour l’image */
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, art_url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "album-art-fetcher/1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);

    res = curl_easy_perform(curl);
    fclose(f);
    free(art_url);

    if (res != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform (image) failed: %s\n",
                curl_easy_strerror(res));
        goto cleanup;
    }

    printf("Saved artwork to %s\n", out_path);
    ok = 1;

cleanup:
    if (curl) curl_easy_cleanup(curl);
    curl_global_cleanup();
    free(mem.data);
    return ok;
}
