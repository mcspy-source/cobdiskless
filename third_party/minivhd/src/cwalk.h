#ifndef CWALK_H
#define CWALK_H
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

enum cwk_path_style {
    CWK_STYLE_WINDOWS,
    CWK_STYLE_UNIX
};

static inline enum cwk_path_style cwk_path_guess_style(const char* path) {
    if (path && (strchr(path, '\\') || (path[0] && path[1] == ':'))) return CWK_STYLE_WINDOWS;
    return CWK_STYLE_UNIX;
}

static inline void cwk_path_set_style(enum cwk_path_style style) {
    (void)style;
}

static inline bool cwk_path_is_absolute(const char* path) {
    if (!path || !path[0]) return false;
    if (path[0] == '/' || path[0] == '\\') return true;
    if (((path[0] >= 'a' && path[0] <= 'z') || (path[0] >= 'A' && path[0] <= 'Z')) && path[1] == ':') return true;
    return false;
}

static inline void cwk_path_get_dirname(const char* path, size_t* length) {
    if (!path || !length) return;
    const char* last_slash = NULL;
    for (const char* p = path; *p; ++p) {
        if (*p == '/' || *p == '\\') last_slash = p;
    }
    if (last_slash) {
        *length = (size_t)(last_slash - path + 1);
    } else {
        *length = 0;
    }
}

static inline size_t cwk_path_join(const char* base, const char* path, char* buffer, size_t buffer_size) {
    if (!buffer || buffer_size == 0) return 0;
    buffer[0] = '\0';
    if (!base && !path) return 0;
    if (!base) {
        strncpy(buffer, path, buffer_size - 1);
        buffer[buffer_size - 1] = '\0';
        return strlen(buffer);
    }
    if (!path) {
        strncpy(buffer, base, buffer_size - 1);
        buffer[buffer_size - 1] = '\0';
        return strlen(buffer);
    }
    size_t base_len = strlen(base);
    bool need_slash = (base_len > 0 && base[base_len - 1] != '/' && base[base_len - 1] != '\\');
    size_t written = 0;
    strncpy(buffer, base, buffer_size - 1);
    buffer[buffer_size - 1] = '\0';
    written = strlen(buffer);
    if (need_slash && written + 1 < buffer_size) {
        buffer[written++] = (strchr(base, '\\') ? '\\' : '/');
        buffer[written] = '\0';
    }
    if (written < buffer_size - 1) {
        strncat(buffer, path, buffer_size - 1 - written);
    }
    return strlen(buffer);
}
#endif
