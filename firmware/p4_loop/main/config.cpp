#include "config.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <strings.h>

#include "esp_log.h"

static const char *TAG = "config";

static std::string trim(const char *s, size_t n)
{
    while (n && isspace((unsigned char)*s)) { s++; n--; }
    while (n && isspace((unsigned char)s[n - 1])) n--;
    return std::string(s, n);
}

bool config_load(const char *path, PlayerConfig &out)
{
    out = PlayerConfig{};
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[320];  // FAT names reach 255 characters, plus "file = " 
    int ln = 0;
    while (fgets(line, sizeof(line), f)) {
        ln++;
        if (!strchr(line, '\n') && !feof(f)) {  // overlong line: skip all of it
            ESP_LOGW(TAG, "%s:%d: line too long, ignored", path, ln);
            int ch;
            while ((ch = fgetc(f)) != EOF && ch != '\n') {}
            continue;
        }
        char *p = line;
        if (ln == 1 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) {
            p += 3;  // UTF-8 byte order mark written by some editors
        }
        // '#' starts a comment at the line start or after a space, so file
        // names containing '#' still work.
        for (char *h = p; *h; h++) {
            if (*h == '#' && (h == p || isspace((unsigned char)h[-1]))) { *h = 0; break; }
        }
        char *eq = strchr(p, '=');
        std::string key = trim(p, eq ? (size_t)(eq - p) : strlen(p));
        for (auto &ch : key) ch = (char)tolower((unsigned char)ch);
        if (key.empty()) continue;
        if (!eq) {
            ESP_LOGW(TAG, "%s:%d: no '=' in \"%s\"", path, ln, key.c_str());
            continue;
        }
        std::string val = trim(eq + 1, strlen(eq + 1));
        if (key == "file") {
            out.file = val;
        } else if (key == "update") {
            const char *v = val.c_str();
            if (!strcasecmp(v, "on") || !strcasecmp(v, "yes") || !strcasecmp(v, "true") || !strcmp(v, "1")) {
                out.update = true;
            } else {
                // off/no/false/0, and anything unclear: never flash on a typo.
                if (strcasecmp(v, "off") && strcasecmp(v, "no") && strcasecmp(v, "false") && strcmp(v, "0")) {
                    ESP_LOGW(TAG, "%s:%d: update = \"%s\" not understood, updates off", path, ln, v);
                }
                out.update = false;
            }
        } else {
            ESP_LOGW(TAG, "%s:%d: unknown key \"%s\" ignored", path, ln, key.c_str());
        }
    }
    fclose(f);
    ESP_LOGI(TAG, "file=%s update=%s", out.file.empty() ? "(all)" : out.file.c_str(), out.update ? "on" : "off");
    return true;
}
