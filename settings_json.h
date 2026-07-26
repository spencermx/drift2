#ifndef DRIFT_SETTINGS_JSON_H
#define DRIFT_SETTINGS_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <limits.h>
#include <string.h>

// The settings file is already capped at 64 KiB. A separate nesting cap keeps
// adversarial but size-bounded JSON from consuming the C stack while locating
// the two keys Drift owns.
#define DRIFT_SETTINGS_JSON_MAX_DEPTH 64

typedef enum DriftSettingsJsonAction {
    DRIFT_SETTINGS_JSON_NONE = 0,
    DRIFT_SETTINGS_JSON_REPLACE_ARRAY,
    DRIFT_SETTINGS_JSON_INSERT_IN_PERMISSIONS,
    DRIFT_SETTINGS_JSON_INSERT_PERMISSIONS
} DriftSettingsJsonAction;

typedef struct DriftSettingsJsonTarget {
    DriftSettingsJsonAction action;
    int array_start; // inclusive '[' offset for REPLACE_ARRAY
    int array_end;   // inclusive ']' offset for REPLACE_ARRAY
    int insert_at;   // immediately after the proven object-opening '{'
    bool needs_comma;
} DriftSettingsJsonTarget;

typedef struct DriftSettingsJsonParser {
    const char* begin;
    const char* cursor;
    const char* end;
    DriftSettingsJsonTarget* target;
    bool permissions_found;
    bool directories_found;
    int root_insert_at;
    bool root_needs_comma;
    int permissions_insert_at;
    bool permissions_needs_comma;
} DriftSettingsJsonParser;

static void DriftSettingsJsonSkipWhitespace(DriftSettingsJsonParser* parser) {
    while (parser->cursor < parser->end) {
        char c = *parser->cursor;
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
        parser->cursor++;
    }
}

static int DriftSettingsJsonHexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Parses and validates one JSON string. When expected is non-NULL, matches is
// set from the decoded string value, so a key such as "permiss\u0069ons" is
// semantically identical to "permissions".
static bool DriftSettingsJsonParseString(DriftSettingsJsonParser* parser,
                                         const char* expected, bool* matches) {
    if (parser->cursor >= parser->end || *parser->cursor != '"') return false;
    parser->cursor++;

    bool same = expected != NULL;
    size_t expected_at = 0;
    while (parser->cursor < parser->end) {
        unsigned int decoded;
        unsigned char raw = (unsigned char)*parser->cursor++;
        if (raw == '"') {
            if (matches != NULL) {
                *matches = same && expected[expected_at] == '\0';
            }
            return true;
        }
        if (raw < 0x20) return false;

        if (raw != '\\') {
            decoded = raw;
        } else {
            if (parser->cursor >= parser->end) return false;
            char escaped = *parser->cursor++;
            switch (escaped) {
                case '"': decoded = '"'; break;
                case '\\': decoded = '\\'; break;
                case '/': decoded = '/'; break;
                case 'b': decoded = '\b'; break;
                case 'f': decoded = '\f'; break;
                case 'n': decoded = '\n'; break;
                case 'r': decoded = '\r'; break;
                case 't': decoded = '\t'; break;
                case 'u': {
                    if ((size_t)(parser->end - parser->cursor) < 4) return false;
                    decoded = 0;
                    for (int i = 0; i < 4; i++) {
                        int digit = DriftSettingsJsonHexValue(parser->cursor[i]);
                        if (digit < 0) return false;
                        decoded = decoded * 16u + (unsigned int)digit;
                    }
                    parser->cursor += 4;
                    break;
                }
                default:
                    return false;
            }
        }

        if (same) {
            unsigned char wanted = (unsigned char)expected[expected_at];
            if (decoded > 0x7f || wanted == '\0' || decoded != wanted) {
                same = false;
            } else {
                expected_at++;
            }
        }
    }
    return false;
}

static bool DriftSettingsJsonParseValue(DriftSettingsJsonParser* parser,
                                        unsigned int depth);

static bool DriftSettingsJsonParseObject(DriftSettingsJsonParser* parser,
                                         unsigned int depth) {
    if (depth > DRIFT_SETTINGS_JSON_MAX_DEPTH ||
        parser->cursor >= parser->end || *parser->cursor != '{') return false;
    parser->cursor++;
    DriftSettingsJsonSkipWhitespace(parser);
    if (parser->cursor < parser->end && *parser->cursor == '}') {
        parser->cursor++;
        return true;
    }

    while (parser->cursor < parser->end) {
        if (!DriftSettingsJsonParseString(parser, NULL, NULL)) return false;
        DriftSettingsJsonSkipWhitespace(parser);
        if (parser->cursor >= parser->end || *parser->cursor != ':') return false;
        parser->cursor++;
        DriftSettingsJsonSkipWhitespace(parser);
        if (!DriftSettingsJsonParseValue(parser, depth + 1)) return false;
        DriftSettingsJsonSkipWhitespace(parser);
        if (parser->cursor >= parser->end) return false;
        if (*parser->cursor == '}') {
            parser->cursor++;
            return true;
        }
        if (*parser->cursor != ',') return false;
        parser->cursor++;
        DriftSettingsJsonSkipWhitespace(parser);
    }
    return false;
}

static bool DriftSettingsJsonParseArray(DriftSettingsJsonParser* parser,
                                        unsigned int depth) {
    if (depth > DRIFT_SETTINGS_JSON_MAX_DEPTH ||
        parser->cursor >= parser->end || *parser->cursor != '[') return false;
    parser->cursor++;
    DriftSettingsJsonSkipWhitespace(parser);
    if (parser->cursor < parser->end && *parser->cursor == ']') {
        parser->cursor++;
        return true;
    }

    while (parser->cursor < parser->end) {
        if (!DriftSettingsJsonParseValue(parser, depth + 1)) return false;
        DriftSettingsJsonSkipWhitespace(parser);
        if (parser->cursor >= parser->end) return false;
        if (*parser->cursor == ']') {
            parser->cursor++;
            return true;
        }
        if (*parser->cursor != ',') return false;
        parser->cursor++;
        DriftSettingsJsonSkipWhitespace(parser);
    }
    return false;
}

static bool DriftSettingsJsonParseNumber(DriftSettingsJsonParser* parser) {
    const char* p = parser->cursor;
    if (p < parser->end && *p == '-') p++;
    if (p >= parser->end) return false;

    if (*p == '0') {
        p++;
        if (p < parser->end && *p >= '0' && *p <= '9') return false;
    } else {
        if (*p < '1' || *p > '9') return false;
        do { p++; } while (p < parser->end && *p >= '0' && *p <= '9');
    }

    if (p < parser->end && *p == '.') {
        p++;
        if (p >= parser->end || *p < '0' || *p > '9') return false;
        do { p++; } while (p < parser->end && *p >= '0' && *p <= '9');
    }
    if (p < parser->end && (*p == 'e' || *p == 'E')) {
        p++;
        if (p < parser->end && (*p == '+' || *p == '-')) p++;
        if (p >= parser->end || *p < '0' || *p > '9') return false;
        do { p++; } while (p < parser->end && *p >= '0' && *p <= '9');
    }
    parser->cursor = p;
    return true;
}

static bool DriftSettingsJsonParseLiteral(DriftSettingsJsonParser* parser,
                                          const char* literal, size_t length) {
    if ((size_t)(parser->end - parser->cursor) < length ||
        memcmp(parser->cursor, literal, length) != 0) return false;
    parser->cursor += length;
    return true;
}

static bool DriftSettingsJsonParseValue(DriftSettingsJsonParser* parser,
                                        unsigned int depth) {
    if (depth > DRIFT_SETTINGS_JSON_MAX_DEPTH || parser->cursor >= parser->end) {
        return false;
    }
    switch (*parser->cursor) {
        case '{': return DriftSettingsJsonParseObject(parser, depth);
        case '[': return DriftSettingsJsonParseArray(parser, depth);
        case '"': return DriftSettingsJsonParseString(parser, NULL, NULL);
        case 't': return DriftSettingsJsonParseLiteral(parser, "true", 4);
        case 'f': return DriftSettingsJsonParseLiteral(parser, "false", 5);
        case 'n': return DriftSettingsJsonParseLiteral(parser, "null", 4);
        default: return DriftSettingsJsonParseNumber(parser);
    }
}

static bool DriftSettingsJsonParseDirectoriesArray(
        DriftSettingsJsonParser* parser) {
    if (parser->cursor >= parser->end || *parser->cursor != '[') return false;
    parser->target->array_start = (int)(parser->cursor - parser->begin);
    parser->cursor++;
    DriftSettingsJsonSkipWhitespace(parser);
    if (parser->cursor < parser->end && *parser->cursor == ']') {
        parser->target->array_end = (int)(parser->cursor - parser->begin);
        parser->cursor++;
        return true;
    }

    while (parser->cursor < parser->end) {
        if (!DriftSettingsJsonParseString(parser, NULL, NULL)) return false;
        DriftSettingsJsonSkipWhitespace(parser);
        if (parser->cursor >= parser->end) return false;
        if (*parser->cursor == ']') {
            parser->target->array_end = (int)(parser->cursor - parser->begin);
            parser->cursor++;
            return true;
        }
        if (*parser->cursor != ',') return false;
        parser->cursor++;
        DriftSettingsJsonSkipWhitespace(parser);
    }
    return false;
}

static bool DriftSettingsJsonParsePermissionsObject(
        DriftSettingsJsonParser* parser, unsigned int depth) {
    if (depth > DRIFT_SETTINGS_JSON_MAX_DEPTH ||
        parser->cursor >= parser->end || *parser->cursor != '{') return false;
    parser->permissions_insert_at = (int)(parser->cursor - parser->begin) + 1;
    parser->cursor++;
    DriftSettingsJsonSkipWhitespace(parser);
    if (parser->cursor < parser->end && *parser->cursor == '}') {
        parser->permissions_needs_comma = false;
        parser->cursor++;
        return true;
    }

    bool has_member = false;
    bool seen_directories = false;
    while (parser->cursor < parser->end) {
        bool is_directories = false;
        if (!DriftSettingsJsonParseString(parser, "additionalDirectories",
                                          &is_directories)) return false;
        has_member = true;
        DriftSettingsJsonSkipWhitespace(parser);
        if (parser->cursor >= parser->end || *parser->cursor != ':') return false;
        parser->cursor++;
        DriftSettingsJsonSkipWhitespace(parser);
        if (is_directories) {
            if (seen_directories) return false;
            seen_directories = true;
            if (!DriftSettingsJsonParseDirectoriesArray(parser)) return false;
            parser->directories_found = true;
        } else if (!DriftSettingsJsonParseValue(parser, depth + 1)) {
            return false;
        }
        DriftSettingsJsonSkipWhitespace(parser);
        if (parser->cursor >= parser->end) return false;
        if (*parser->cursor == '}') {
            parser->permissions_needs_comma = has_member;
            parser->cursor++;
            return true;
        }
        if (*parser->cursor != ',') return false;
        parser->cursor++;
        DriftSettingsJsonSkipWhitespace(parser);
    }
    return false;
}

static bool DriftSettingsJsonParseRootObject(DriftSettingsJsonParser* parser) {
    if (parser->cursor >= parser->end || *parser->cursor != '{') return false;
    parser->root_insert_at = (int)(parser->cursor - parser->begin) + 1;
    parser->cursor++;
    DriftSettingsJsonSkipWhitespace(parser);
    if (parser->cursor < parser->end && *parser->cursor == '}') {
        parser->root_needs_comma = false;
        parser->cursor++;
        return true;
    }

    bool has_member = false;
    bool seen_permissions = false;
    while (parser->cursor < parser->end) {
        bool is_permissions = false;
        if (!DriftSettingsJsonParseString(parser, "permissions",
                                          &is_permissions)) return false;
        has_member = true;
        DriftSettingsJsonSkipWhitespace(parser);
        if (parser->cursor >= parser->end || *parser->cursor != ':') return false;
        parser->cursor++;
        DriftSettingsJsonSkipWhitespace(parser);
        if (is_permissions) {
            if (seen_permissions) return false;
            seen_permissions = true;
            parser->permissions_found = true;
            if (!DriftSettingsJsonParsePermissionsObject(parser, 2)) return false;
        } else if (!DriftSettingsJsonParseValue(parser, 2)) {
            return false;
        }
        DriftSettingsJsonSkipWhitespace(parser);
        if (parser->cursor >= parser->end) return false;
        if (*parser->cursor == '}') {
            parser->root_needs_comma = has_member;
            parser->cursor++;
            return true;
        }
        if (*parser->cursor != ',') return false;
        parser->cursor++;
        DriftSettingsJsonSkipWhitespace(parser);
    }
    return false;
}

// Validates the complete bounded document and returns the only safe edit Drift
// may perform. False means malformed, over-deep, duplicate, or wrong-typed
// target structure; callers must leave the file untouched.
static bool DriftLocateSettingsJsonTarget(const char* json, size_t length,
                                          DriftSettingsJsonTarget* target) {
    if (json == NULL || target == NULL || length > INT_MAX) return false;
    memset(target, 0, sizeof(*target));

    DriftSettingsJsonParser parser;
    memset(&parser, 0, sizeof(parser));
    parser.begin = json;
    parser.cursor = json;
    parser.end = json + length;
    parser.target = target;

    // RFC 8259 permits parsers to ignore a UTF-8 BOM rather than treating it
    // as part of the JSON value. Preserve it in place while locating offsets.
    if ((size_t)(parser.end - parser.cursor) >= 3 &&
        (unsigned char)parser.cursor[0] == 0xef &&
        (unsigned char)parser.cursor[1] == 0xbb &&
        (unsigned char)parser.cursor[2] == 0xbf) {
        parser.cursor += 3;
    }
    DriftSettingsJsonSkipWhitespace(&parser);
    if (!DriftSettingsJsonParseRootObject(&parser)) return false;
    DriftSettingsJsonSkipWhitespace(&parser);
    if (parser.cursor != parser.end) return false;

    if (parser.directories_found) {
        target->action = DRIFT_SETTINGS_JSON_REPLACE_ARRAY;
    } else if (parser.permissions_found) {
        target->action = DRIFT_SETTINGS_JSON_INSERT_IN_PERMISSIONS;
        target->insert_at = parser.permissions_insert_at;
        target->needs_comma = parser.permissions_needs_comma;
    } else {
        target->action = DRIFT_SETTINGS_JSON_INSERT_PERMISSIONS;
        target->insert_at = parser.root_insert_at;
        target->needs_comma = parser.root_needs_comma;
    }
    return true;
}

#endif
