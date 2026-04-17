// object.c — Content-addressable object store
//
// Every piece of data (file contents, directory listings, commits) is stored
// as an "object" named by its SHA-256 hash. Objects are stored under
// .pes/objects/XX/YYYYYY... where XX is the first two hex characters of the
// hash (directory sharding).
//
// PROVIDED functions: compute_hash, object_path, object_exists, hash_to_hex, hex_to_hash
// TODO functions:     object_write, object_read

#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>
#include <errno.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    }
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    unsigned int hash_len;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, id_out->hash, &hash_len);
    EVP_MD_CTX_free(ctx);
}

// Get the filesystem path where an object should be stored.
// Format: .pes/objects/XX/YYYYYYYY...
// The first 2 hex chars form the shard directory; the rest is the filename.
void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {

    const char *type_str;
    switch (type) {
        case OBJ_BLOB:   type_str = "blob";   break;
        case OBJ_TREE:   type_str = "tree";   break;
        case OBJ_COMMIT: type_str = "commit"; break;
        default: return -1;
    }

    char header[64];
    int header_len = snprintf(header, sizeof(header), "%s %zu", type_str, len);
    if (header_len < 0) return -1;
    header_len += 1;

    size_t full_len = (size_t)header_len + len;
    uint8_t *full_obj = malloc(full_len);
    if (!full_obj) return -1;

    memcpy(full_obj, header, (size_t)header_len);
    if (len > 0) memcpy(full_obj + header_len, data, len);

    compute_hash(full_obj, full_len, id_out);

    if (object_exists(id_out)) {
        free(full_obj);
        return 0;
    }

    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id_out, hex);

    char shard_dir[512];
    snprintf(shard_dir, sizeof(shard_dir), "%s/%.2s", OBJECTS_DIR, hex);
    mkdir(shard_dir, 0755);

    char final_path[512];
    object_path(id_out, final_path, sizeof(final_path));
  
    char temp_path[640];
    snprintf(temp_path, sizeof(temp_path), "%s/.tmp-XXXXXX", shard_dir);

    int fd = mkstemp(temp_path);
    if (fd < 0) {
        free(full_obj);
        return -1;
    }

    ssize_t w = write(fd, full_obj, full_len);
    if (w < 0 || (size_t)w != full_len) {
        close(fd);
        unlink(temp_path);
        free(full_obj);
        return -1;
    }

    fsync(fd);
    close(fd);
    if (rename(temp_path, final_path) != 0) {
        unlink(temp_path);
        free(full_obj);
        return -1;
    }

    int dir_fd = open(shard_dir, O_RDONLY);
    if (dir_fd >= 0) {
        fsync(dir_fd);
        close(dir_fd);
    }

    free(full_obj);
    return 0;
}

int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {

    char path[512];
    object_path(id, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long file_len = ftell(f);
    if (file_len < 0) { fclose(f); return -1; }
    rewind(f);

    uint8_t *buf = malloc((size_t)file_len);
    if (!buf) { fclose(f); return -1; }

    if (fread(buf, 1, (size_t)file_len, f) != (size_t)file_len) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);

    uint8_t *null_byte = memchr(buf, '\0', (size_t)file_len);
    if (!null_byte) { free(buf); return -1; }

    size_t header_len = (size_t)(null_byte - buf);
    char header[64];
    if (header_len >= sizeof(header)) { free(buf); return -1; }
    memcpy(header, buf, header_len);
    header[header_len] = '\0';

    char type_str[16];
    size_t claimed_size = 0;
    if (sscanf(header, "%15s %zu", type_str, &claimed_size) != 2) {
        free(buf);
        return -1;
    }

    if (strcmp(type_str, "blob") == 0) *type_out = OBJ_BLOB;
    else if (strcmp(type_str, "tree") == 0) *type_out = OBJ_TREE;
    else if (strcmp(type_str, "commit") == 0) *type_out = OBJ_COMMIT;
    else { free(buf); return -1; }

    size_t data_len = (size_t)file_len - header_len - 1;
    if (data_len != claimed_size) { free(buf); return -1; }

    ObjectID verify_id;
    compute_hash(buf, (size_t)file_len, &verify_id);
    if (memcmp(&verify_id, id, sizeof(ObjectID)) != 0) {
        free(buf);
        return -1;
    }

    uint8_t *data = malloc(data_len > 0 ? data_len : 1);
    if (!data) { free(buf); return -1; }
    if (data_len > 0) memcpy(data, null_byte + 1, data_len);

    *data_out = data;
    *len_out = data_len;

    free(buf);
    return 0;
}
