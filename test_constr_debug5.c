#define SP_IMPLEMENTATION
#include "fxsh.h"
#include <stdio.h>

int main() {
    /* Test basic hash table with sp_str_t keys */
    sp_ht(sp_str_t, int) ht = SP_NULLPTR;
    
    /* Insert with string key */
    c8 *key1 = sp_alloc(5);
    memcpy(key1, "None", 4);
    key1[4] = '\0';
    sp_str_t key1_str = {.data = key1, .len = 4};
    
    int val1 = 42;
    
    /* Ensure and set string functions */
    sp_ht_ensure(ht);
    sp_ht_set_fns(ht, sp_ht_on_hash_str_key, sp_ht_on_compare_str_key);
    
    sp_ht_insert(ht, key1_str, val1);
    
    printf("After insert, size: %zu\n", (size_t)sp_ht_size(ht));
    
    /* Lookup - also need to set functions */
    c8 *key2 = sp_alloc(5);
    memcpy(key2, "None", 4);
    key2[4] = '\0';
    sp_str_t key2_str = {.data = key2, .len = 4};
    
    sp_ht_set_fns(ht, sp_ht_on_hash_str_key, sp_ht_on_compare_str_key);
    int *found = sp_ht_getp(ht, key2_str);
    if (found) {
        printf("Found: %d\n", *found);
    } else {
        printf("NOT found!\n");
    }
    
    sp_free(key1);
    sp_free(key2);
    return 0;
}
